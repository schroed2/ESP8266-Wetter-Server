/* vim:set ts=4 sw=4 fileformat=unix fileencodings=utf-8 fileencoding=utf-8 noexpandtab:  */

/**\file
 * \brief  Weather senor (standalone server variant)
 * \author Ralf Schröder
 * 
 * (C) Feel free to change (MIT)
 *
 */ 

#define WITH_LED 2    /**< activate the onboard LED for testing, use the given LED */
#define WITH_SERIAL   /**< activate the serial printings for testing */
#define LIGHT_SLEEP   /**< no deep sleep if defined, attend the link XPD_DCDC->RST for deep sleep */
#define CLIENT        /**< additional data transmittion as for wetter_sensor */
#define WITH_OTA      /**< Integrate over the air update */
#define VERSION "0.9" /**< Version */ 
#define BUILD 16      /**< Build number */ 
#define LOCATION "Wetterstation"
#define DHT_PIN D2    /**< DHT data pin */
#undef DHT_VCC_PIN    /**< DHT VCC pin if not connected with VCC directly */
ADC_MODE(ADC_VCC)     /**< vcc read */

#include "auth.h"

#ifndef WLAN_SSID
#define WLAN_SSID "SSID"
#endif
#ifndef WLAN_PASSWORD
#define WLAN_PASSWORD "geheim"
#endif
#ifndef DB_SECRET
#define DB_SECRET "geheim"
#endif


static const char* ssid = WLAN_SSID;
static const char* password = WLAN_PASSWORD;
static const int interval_sec = 60;  // >= 2
static const int data_max = 24 * 3600 / interval_sec;  // 1 day
static const char* sensor_id = "Ralfs " LOCATION; //< ID for data base separation, could be the sensor location name */

#ifdef CLIENT
static const char* weather_server = "raspi.fritz.box";
static const int weather_port = 80;
#endif /* CLIENT */


#include <ESP8266WiFi.h>
#include <DHT.h>

#include <TimeLib.h> 
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>

#ifdef WITH_OTA
#include <ArduinoOTA.h>
#endif /* WITH_OTA */

#ifdef WITH_SERIAL
extern "C" {
#include "user_interface.h"
}
#endif /* WITH_SERIAL */


#ifdef WITH_LED
#define ledON()  digitalWrite(WITH_LED, 0)
#define ledOFF() digitalWrite(WITH_LED, 1)
static unsigned ledFlash;
#else
#define ledON()
#define ledOFF()
#endif /* WITH_LED */

#ifdef WITH_SERIAL
boolean doTrace = true;
#define TRACE_LINE(line) (doTrace && Serial.print(line))
#define TRACE(...)       (doTrace && Serial.printf(__VA_ARGS__))
#else
#define TRACE_LINE(line)
#define TRACE(...)
#endif /* WITH_SERIAL */


// NTP Servers:
static IPAddress timeServer(192, 168, 178, 1); // fritz.box
static WiFiUDP Udp;
static unsigned int localPort = 8888;  // local port to listen for UDP packets


class myServer: public ESP8266WebServer
{
	public:
		myServer(int port) : ESP8266WebServer(port) {}
		virtual ~myServer() {}
		void prepareHeader(int code, const char* content_type, unsigned content_len) {
			String header;
			_prepareHeader(header, code, content_type, content_len);
			TRACE_LINE(header);
			_currentClientWrite(header.c_str(), header.length());
		}
};



static myServer server(80);              /**< web server instance */
static unsigned long reconnect_timeout;  /**< Reconnect timeout within loop() for lost WiFi connections (millis() value) */


#ifdef CLIENT
static WiFiClient client;
static unsigned long client_timeout;
static unsigned long client_error;
#endif /* CLIENT */

// Sensor object
static DHT dht(DHT_PIN, DHT22);
static unsigned long data_next;          /**< time to read the sensor (millis value) */


// sensor data
static struct
{
	int temp;
	int hum;
} data[data_max];
static int data_index;
static int data_overrun = -1;

/** Scratch buffer to avoid expensive object management in many situations */
static char buf[600];


/** Deep sleep methon. I assume here, that the function continues after the sleep.
 *  That's not true for reset based variants, but other light sleep varaint can be test so, too.
 */
static void deep_sleep()
{
	unsigned long now = millis();
	unsigned long wait = data_next - now;
#ifdef LIGHT_SLEEP
	if (wait > 2000) { wait = 2000; }
	TRACE("sleeping %lu msec at: %lu\n", wait, now);
	delay(wait);
#else
#ifdef WITH_SERIAL
	if (wait > interval_sec * 1000) { wait = interval_sec * 1000; }
	TRACE("%s %lu msec at: %lu\n", __func__, wait, now);
	Serial.flush();
#endif /* WITH_SERIAL */
	delay(100);
	ESP.deepSleep(wait*1000);
	/* point never reached */
	delay(1000);
#endif /* LIGHT_SLEEP */
}


#ifdef CLIENT
/** Transmit a message to a server */
static void transmit_msg(float temperature, float humidity)
{
	if (client.connect(weather_server, weather_port))
	{
		unsigned jlen = snprintf(buf, sizeof(buf), 
				"{\"sender_id\":\"%s\",\"password\":\"" DB_SECRET "\",\"temperature\":%.1f,\"humidity\":%.1f,\"vcc\":%.2f}\r\n",
				sensor_id, temperature, humidity, ESP.getVcc() / 1000.0);
		char* header = buf + jlen + 1;
		unsigned hlen = snprintf(header, sizeof(buf)-jlen-1,
				"POST /sensor.php HTTP/1.1\r\n"
				"Host: %s:%u\r\n"
				"Connection: close\r\n"
				"Content-Type: application/json; charset=utf-8\r\n"
				"Content-Length: %u\r\n\r\n", weather_server, weather_port, jlen);
		TRACE_LINE(header);
		TRACE_LINE(buf);
		client.write(header, hlen); client.write(buf, jlen);
		client_timeout = millis() + 3000;  // expect answer fin after 3 sec
		client_error = 0;
	} else {
		TRACE("%s: no http server connection\n", __func__);
		wifi_trace(WiFi.status());
		switch (client_error++)
		{
			case 3:
				TRACE("%s client connection errors:%u - force WiFi shutdown\n", __func__, client_error);
				WiFi.mode(WIFI_OFF);
				break;
			case 10:
				TRACE("%s client connection errors:%u - force ESP reset\n", __func__, client_error);
				ESP.reset();
				break;
			default:
				TRACE("%s client connection errors:%u now:%lu\n", __func__, client_error, millis());
		}
	}
}

#endif /* CLIENT */


static bool sensorRead()
{
	bool ret;
	ledON();

#ifdef CLIENT
	if (!client_error)
#endif /* CLIENT */
	{
		if (++data_index >= data_max) {
			data_index = 0;
			data_overrun++;
		}
	} // otherwise reuse the slot, we are calles faster in that case...
	data[data_index].temp = 0;
	data[data_index].hum = -1;

	float t = dht.readTemperature();
	float h = dht.readHumidity();
	if (!isnan(t) && !isnan(h))
	{
		data[data_index].temp = (int)(t * 10 + 0.5);
		data[data_index].hum = (int)(h * 10 + 0.5);
#ifdef CLIENT
		transmit_msg(t, h);
#endif /* CLIENT */
		TRACE("Index:%d/%d Temperatur: %d.%d°C Luftfeuchtikeit: %d.%d%% RSSI:%d\n",
				data_overrun, data_index, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10,
				WiFi.RSSI());
		ret = true;
	} else {
		TRACE("%s: reading sensor failed, try again\n", __func__);
#ifdef DHT_VCC_PIN
		digitalWrite(DHT_VCC_PIN, LOW);
		delay(20);
		digitalWrite(DHT_VCC_PIN, HIGH);
#endif /* DHT_VCC_PIN */
		ret = false;
	}

	ledOFF();
	return ret;
}


/* Returns timeshift to GMT for MEZ/MESZ periods */
static int timeZone_de(time_t t)
{
	tmElements_t tm;
	breakTime(t, tm);
	if (tm.Month < 3 || tm.Month > 10) return SECS_PER_HOUR; // keine Sommerzeit in Jan, Feb, Nov, Dez
	if (tm.Month > 3 && tm.Month < 10) return 2*SECS_PER_HOUR; // Sommerzeit in Apr, Mai, Jun, Jul, Aug, Sep
	if ((tm.Month == 3 && ((tm.Hour + 24 * tm.Day) >= (2 + 24*(31 - (5 * tm.Year /4 + 4) % 7)))) || (tm.Month == 10 && ((tm.Hour + 24 * tm.Day) < (2 + 24*(31 - (5 * tm.Year /4 + 1) % 7)))))
		return 2*SECS_PER_HOUR;
	else
		return SECS_PER_HOUR;
}

static void onRoot()
{
	ledON();

	int sec = millis() / 1000;
	int min = sec / 60;
	int hr = min / 60;
	time_t t = now();
	t += timeZone_de(t);

	snprintf(buf, sizeof(buf),
			"<!DOCTYPE html>"
			"<html>"
			"<head>"
			"<meta charset=\"utf-8\" http-equiv='refresh' content='%u'/>"
			"<title>%s</title>"
			"<style>"
			"body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }"
			"svg { width: 98%%; height: auto; }"
			"</style>"
			"</head>"
			"<body>"
			"<p>Version %s build %u</p>"
			"<h1>%s</h1>"
			"<p>Uhrzeit: %02d:%02d:%02d %02u.%02u.%04u Online seit: %02u:%02u:%02u</p>"
			"<p>%u.%u&deg;C bei %u.%u%% Luftfeuchtigkeit</p>\n"
			"<embed src=\"graph.svg\" type=\"image/svg+xml\" />"
			"</body>"
			"</html>",
			interval_sec, sensor_id, VERSION, BUILD, sensor_id, hour(t), minute(t), second(t), day(t), month(t), year(t), hr, min % 60, sec % 60, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10
			);
	server.send ( 200, "text/html", buf );
	TRACE("%s\n",__func__);
	ledOFF();
}

static void onTemperature()
{
	ledON();
	snprintf(buf, 400, "%d.%d\n", data[data_index].temp / 10, data[data_index].temp % 10);
	server.send(200, "text/plain", buf);
	TRACE_LINE(buf);
	ledOFF();
}

static void onHumidity()
{
	ledON();
	snprintf(buf, 400, "%d.%d\n", data[data_index].hum / 10, data[data_index].hum % 10);
	server.send( 200, "text/plain", buf);
	TRACE_LINE(buf);
	ledOFF();
}

static void onSensor()
{
	ledON();
	snprintf(buf, 400, "<html>Uptime:%d Temperatur: %d.%d&deg;C Luftfeuchtikeit: %d.%d%%</html>\n",
			data_overrun * interval_sec * data_max + data_index * interval_sec, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10);
	server.send( 200, "text/html", buf);
	TRACE_LINE(buf);
	ledOFF();
}

static void onLedON()
{
	ledON();
	server.send( 200, "text/html", "<html>LED ON</html>");
	TRACE("%s\n", __func__);
}

static void onLedOFF()
{
	ledOFF();
	server.send( 200, "text/html", "<html>LED OFF</html>");
	TRACE("%s\n", __func__);
}

static void handleNotFound()
{
	ledON();
	unsigned len = snprintf(buf, sizeof(buf),
			"URI: '%s' not implemented\n"
			"Method: %s\n"
			"Arguments: %d\n",
			server.uri().c_str(), (( server.method() == HTTP_GET ) ? "GET" : "POST"), server.args());
	for (int i = 0; i < server.args(); i++ ) 
	{
		len += snprintf(buf+len, sizeof(buf) - len, " %s: %s\n", server.argName(i).c_str(), server.arg(i).c_str());
	}
	server.send ( 404, "text/plain", buf);
	TRACE_LINE(buf);
	ledOFF();
}

static void onList()
{
	ledON();
	unsigned long now_ms = millis();
	int count;
	String out = "";
	for (count = 0; count < data_max; count++)
	{
		int index = (data_index + count + 1) % data_max;
		out += -(data_max - count) * interval_sec / 60; out += '\t';
		out += data[index].temp; out += '\t';
		out += data[index].hum; out += '\n';
	}
	server.send ( 200, "text/plain", out);
	TRACE("%s: duration: %lu\n", __func__, millis() - now_ms);
	ledOFF();
}

static void onGraph()
{
	ledON();
	unsigned long now_ms = millis();
	int count, t_min = 0, t_max = -10000;
	for (count = 0; count < data_max; count++)
	{
		if (data[count].hum < 0) { continue; }
		if (t_min > data[count].temp) {
			t_min = data[count].temp;
		}
		if (t_max < data[count].temp) {
			t_max = data[count].temp;
		}
	}
	if (t_min < 0) { t_min = t_min - t_min % 50 - ((t_min < 0) ? 50 : 0); }
	t_max = t_max - t_max % 50 + ((t_max > 0) ? 50 : 0);

	int t_norm = t_max - t_min;
	int sent, len;
	int step_v = data_max / 24;  // 1h raster, 5 degree (step_h == 50)
	String str1 = String("<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" viewBox=\"0 0 ") + (data_max + 2 * step_v) + " " + (t_norm + 100) + "\">\n"
		"<defs><pattern id=\"grid\" patternUnits=\"userSpaceOnUse\" width=\"" + step_v + "\" height=\"50\" x=\"0\" y=\"0\""
		" fill=\"NavajoWhite\" stroke-width=\"1\" stroke=\"grey\" stroke-dasharray=\"2,2\"><desc>Raster</desc><path d=\"M0,0 v50 h" + step_v + " v-50 z\" /></pattern></defs>\n"
		"<rect width=\"" + data_max + "\" height=\"" + t_norm + "\" fill=\"url(#grid)\" stroke-width=\"1\" stroke=\"Black\" x=\"" + step_v + "\" y=\"50\" />\n"
		"<polyline points=\"";
	String str2 = String("\" style=\"fill:none;stroke:MediumBlue;stroke-width:2\" />\n<polyline points=\"");
	String str3 = String("\" style=\"fill:none;stroke:DarkGreen;stroke-width:2\" />\n");
	String str4 = String("<text x=\"") + (data_max/2) + "\" y=\"" + (t_norm + 75) + "\" fill=\"MediumBlue\">24h Temperatur " + (t_min/10) + "-" + (t_max/10) + "°C</text>\n"
		"<text x=\"100\" y=\""                          + (t_norm + 75) + "\" fill=\"DarkGreen\">24h Luftfeuchtigkeit 0-100%</text>\n"
		"</svg>\n\r\n";

	len = (str1.length() + str2.length() + 20 * data_max + str3.length() + str4.length());
	len += (sizeof("<text x=\"15\" y=\"0000\" fill=\"MediumBlue\">000°C</text>\n" "<text x=\"0000\" y=\"0000\" fill=\"MediumBlue\">000%</text>\n") - 1) * (t_norm / 50 + 1);
	server.prepareHeader(200, "image/svg+xml", len);
	server.sendContent(str1); sent = str1.length();

	TRACE("%s step1: %lums\n", __func__, millis() - now_ms);
	char buf[200];
	int x;
	for (x = 0; x < data_max; x++)
	{
		int i = (data_index + x + 1) % data_max;
		if (data[i].hum < 0) { continue; }
		sent += snprintf(buf, sizeof(buf), "%4d,%4d ", x + step_v, t_max - data[i].temp + 50); /* size 10 */
		server.sendContent(buf);
	}
	server.sendContent(str2); sent += str2.length();
	for (x = 0; x < data_max; x++)
	{
		int i = (data_index + x + 1) % data_max;
		if (data[i].hum < 0) { continue; }
		sent += snprintf(buf, sizeof(buf), "%4d,%4d ", x + step_v, (1000 - data[i].hum) * t_norm / 1000 + 50); /* size 10 */
		server.sendContent(buf);
	}
	server.sendContent(str3); sent += str3.length();
	for (x = 0; x <= t_norm / 50; x++)
	{
		sent += snprintf(buf, sizeof(buf),
				"<text x=\"%d\" y=\"%d\" fill=\"MediumBlue\">%2d°C</text>\n"
				"<text x=\"5\" y=\"%d\" fill=\"DarkGreen\">%3d%%</text>\n",
				data_max + step_v + 5,
				55 + x * 50, (t_max - x * 50) / 10,
				55 + x * 50,  100 - 5000 *x / t_norm);
		server.sendContent(buf);
	}
	server.sendContent(str4); sent += str4.length();
	while (sent < len)
	{
		str2 = " ";
		while (sent++ < len && str2.length() < 256) {
			str2 += " ";
		}
		server.sendContent(str2);
	}

	TRACE("%s: %lums\n", __func__, millis() - now_ms);
	ledOFF();
}


/*-------- NTP code ----------*/

static const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
static byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// send an NTP request to the time server at the given address
static void sendNTPpacket()
{
	// set all bytes in the buffer to 0
	memset(packetBuffer, 0, NTP_PACKET_SIZE);
	// Initialize values needed to form NTP request
	// (see URL above for details on the packets)
	packetBuffer[0] = 0b11100011;   // LI, Version, Mode
	packetBuffer[1] = 0;     // Stratum, or type of clock
	packetBuffer[2] = 6;     // Polling Interval
	packetBuffer[3] = 0xEC;  // Peer Clock Precision
	// 8 bytes of zero for Root Delay & Root Dispersion
	packetBuffer[12]  = 49;
	packetBuffer[13]  = 0x4E;
	packetBuffer[14]  = 49;
	packetBuffer[15]  = 52;
	// all NTP fields have been given values, now
	// you can send a packet requesting a timestamp:                 
	Udp.beginPacket(timeServer, 123); //NTP requests are to port 123
	Udp.write(packetBuffer, NTP_PACKET_SIZE);
	Udp.endPacket();
}

static time_t getNtpTime()
{
	while (Udp.parsePacket() > 0) ; // discard any previously received packets
	TRACE_LINE("Transmit NTP Request\n");
	sendNTPpacket();
	uint32_t beginWait = millis();
	while (millis() - beginWait < 1500) {
		int size = Udp.parsePacket();
		if (size >= NTP_PACKET_SIZE) {
			TRACE_LINE("Receive NTP Response\n");
			Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
			unsigned long secsSince1900;
			// convert four bytes starting at location 40 to a long integer
			secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
			secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
			secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
			secsSince1900 |= (unsigned long)packetBuffer[43];
			return secsSince1900 - 2208988800UL;
		}
	}
	TRACE_LINE("No NTP Response :-(\n");
	return 0; // return 0 if unable to get the time
}

#ifdef WITH_SERIAL
static int wifi_trace(int status)
{
	switch (status)
	{
		case WL_CONNECTED: TRACE_LINE("WiFi: WL_CONNECTED\n"); break;
		case WL_NO_SHIELD: TRACE_LINE("WiFi: WL_NO_SHIELD\n"); break;
		case WL_IDLE_STATUS: TRACE_LINE("WiFi: WL_IDLE_STATUS\n"); break;
		case WL_NO_SSID_AVAIL: TRACE_LINE("WiFi: WL_SSID_AVAIL\n"); break;
		case WL_SCAN_COMPLETED: TRACE_LINE("WiFi: WL_SCAN_COMPLETED\n"); break;
		case WL_CONNECT_FAILED: TRACE_LINE("WiFi: WL_CONNECT_FAILED\n"); break;
		case WL_CONNECTION_LOST: TRACE_LINE("WiFi: WL_CONNECTION_LOST\n"); break;
		case WL_DISCONNECTED: TRACE_LINE("WiFi: WL_DISCONNECTEDNO_SHIELD\n"); break;
		default: TRACE("%s: illegal state %d", __func__, status); break;
	}
	return status;
}
#else
#define wifi_trace(status) status
#endif

void setup()
{
#ifdef WITH_LED
	pinMode(WITH_LED, OUTPUT);
	ledFlash = millis() + 5000;
#endif /* WITH_LED */
	ledON();

#ifdef WITH_SERIAL
	Serial.begin(115200);
	delay(20);
	enum flash_size_map fmap = system_get_flash_size_map();
	const char * const FLASH_MAP[] = {
		"4Mbits. Map : 256KBytes + 256KBytes",
		"2Mbits. Map : 256KBytes",
		"8Mbits. Map : 512KBytes + 512KBytes",
		"16Mbits. Map : 512KBytes + 512KBytes",
		"32Mbits. Map : 512KBytes + 512KBytes",
		"16Mbits. Map : 1024KBytes + 1024KBytes",
		"32Mbits. Map : 1024KBytes + 1024KBytes",
		"32Mbits. Map : 2048KBytes + 2048KBytes",
		"64Mbits. Map : 1024KBytes + 1024KBytes",
		"128Mbits. Map : 1024KBytes + 1024KBytes"
		};

	TRACE("%s reset reason: %s flash: %s size: %u real %u speed %u freq %uMHz sketch %u free %u Vcc: %d\n",
			__func__, ESP.getResetInfo().c_str(), FLASH_MAP[fmap],
		 ESP.getFlashChipSize(), ESP.getFlashChipRealSize(), ESP.getFlashChipSpeed(), ESP.getCpuFreqMHz(),
		 ESP.getSketchSize(), ESP.getFreeSketchSpace(), ESP.getVcc());
#endif /* WITH_SERIAL */

	// Connect to WiFi network
	TRACE("Connecting to %s", ssid);
	//WiFi.persistent(false);
	WiFi.mode(WIFI_STA);
	if (WiFi.mode (WIFI_STA))
	{
		WiFi.begin(ssid, password);
		for(int count = 0; count < 100; count++)
		{
			if (WiFi.status() == WL_CONNECTED) { break; }
			delay(100);
			TRACE(".");
		}
		wifi_trace(WiFi.status());
		TRACE("WiFI connected on %s channel%u, IP: %s\n", WiFi.SSID().c_str(), WiFi.channel(), WiFi.localIP().toString().c_str());
	} else {
		TRACE("%s: WiFi no in station mode (FATAL)", __func__);
	}

	for(data_index = 0; data_index < data_max; data_index++) { data[data_index].hum = -1; }
	data_index = data_max - 1;

#ifdef WITH_OTA
	ArduinoOTA.setHostname(LOCATION);
#ifdef WITH_SERIAL
	ArduinoOTA.onStart([]() { TRACE("Start updaing %s\n", ((ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem")); });
	ArduinoOTA.onEnd([]() { TRACE_LINE("End updating\n"); });
#endif /* WITH_SERIAL */
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) 
	{ 
		TRACE("Progress: %u%%\n", (progress / (total / 100))); 
#ifdef WITH_LED
		static boolean led = true;
		if (led) { ledON(); } else { ledOFF(); }
		led = !led;
#endif /* WITH_LED */
	});
#ifdef WITH_SERIAL
	ArduinoOTA.onError([](ota_error_t error) {
			TRACE("Error[%u]: ", error);
			if (error == OTA_AUTH_ERROR) TRACE_LINE("Auth Failed\n");
			else if (error == OTA_BEGIN_ERROR) TRACE_LINE("Begin Failed\n");
			else if (error == OTA_CONNECT_ERROR) TRACE_LINE("Connect Failed\n");
			else if (error == OTA_RECEIVE_ERROR) TRACE_LINE("Receive Failed\n");
			else if (error == OTA_END_ERROR) TRACE_LINE("End Failed\n");
			});
#endif /* WITH_SERIAL */
	ArduinoOTA.begin();
#endif /* WITH_OTA */

	/** Start early because we have to wait 1.5sec before first reading */
#ifdef DHT_VCC_PIN
	pinMode(DHT_VCC_PIN, OUTPUT);
	digitalWrite(DHT_VCC_PIN, HIGH);
#endif /* DHT_VCC_PIN */
	dht.begin();
	data_next = millis() + 2000;

	// Start the server
	server.on("/", onRoot);
	server.on("/graph.svg", onGraph);
	server.on("/list", onList);
	server.on("/led1", onLedON);
	server.on("/led0", onLedOFF);
	server.on("/temperature", onTemperature);
	server.on("/humidity", onHumidity);
#ifdef WITH_SERIAL
	server.on("/trace", []() { doTrace = !doTrace; server.send(200, "text/play", "ok"); });
#endif /* WITH_SERIAL */
	server.on("/sensor", onSensor);
	server.onNotFound(handleNotFound);
	server.begin();
	TRACE_LINE("Server started\n");

	Udp.begin(localPort);
	TRACE("waiting for time sync, port %u\n", Udp.localPort());
	setSyncProvider(getNtpTime);
	ledOFF();
}

void loop()
{
	static int wifi_retry_count = 0;
#ifdef WITH_LED
	if (ledFlash <= millis()) { ledON(); ledFlash += 5000; }
#endif /* WITH_LED */

	/** Check WiFi connection first */
	if (WL_CONNECTED != WiFi.status())
	{
		if (!reconnect_timeout)
		{
			wifi_trace(WiFi.status());
			switch(WiFi.status())
			{
				case WL_CONNECTED:
					break;
				case WL_DISCONNECTED:
					ledON();
					TRACE("%s: WiFi disconnected, connect again\n", __func__);
					WiFi.mode(WIFI_STA);
					WiFi.begin(ssid, password);
					reconnect_timeout = millis() + 5000;
					break;
				case WL_CONNECTION_LOST:
					ledON();
					TRACE("%s: WiFi connection lost, restart", __func__);
					WiFi.reconnect();
					reconnect_timeout = millis() + 5000;
					break;
				case WL_IDLE_STATUS:
					reconnect_timeout = millis() + 5000;
					break;
				default:
					reconnect_timeout = 0;
					WiFi.disconnect(true);
					ledOFF();
					break;
			}
			delay(100);
		} else if (reconnect_timeout < millis()) {
			wifi_trace(WiFi.status());
			if (wifi_retry_count++ == 5)
			{
				TRACE("\n%s: no WLAN connection, retry count %u force reset\n", __func__, wifi_retry_count);
				ESP.reset();
			}
			TRACE("\n%s: no WLAN connection, retry count %u\n", __func__, wifi_retry_count);
			WiFi.mode(WIFI_OFF);
			reconnect_timeout = 0;
			data_next += interval_sec;
			ledOFF();
			deep_sleep(); // give up, wait one data interval
		} else {
			TRACE(".");
			delay(100);
		}
		return;
	}
	if (reconnect_timeout)
	{
		TRACE("\nWiFi reconnected with %s\n", WiFi.localIP().toString().c_str());
		reconnect_timeout = 0;
		wifi_retry_count = 0;
		ledOFF();
	}
	// we are connected with WiFi here

	server.handleClient();
#ifdef WITH_OTA
	ArduinoOTA.handle();
#endif /* WITH_OTA */

#ifdef CLIENT
	if (client_timeout)
	{
		if (client.connected())
		{  // await the data answer
			while (client.available()) { char c = client.read(); TRACE("%c", c); }
			if (client_timeout < millis())
			{
				client_timeout = 0;
				client.stop();
				TRACE("%s client connection closed (timeout at %lu)\n", __func__, millis);
			}
		} else {
				TRACE("%s client connection finished at %lu\n", __func__, millis());
				client_timeout = 0;
				client.stop();
		}
		return;
	}
#endif /* CLIENT */

	if (millis() >= data_next)
	{ // sensor time
		if (sensorRead())
		{
#ifdef CLIENT
			data_next = client_error ? (millis() + 10000) : ((data_overrun * interval_sec * data_max + (data_index + 1) * interval_sec) * 1000);
#else
			data_next = (data_overrun * interval_sec * data_max + (data_index + 1) * interval_sec) * 1000;
#endif
			TRACE("%s: now:%lu next sensor read %lu\n", __func__, millis(), data_next);
			return;
		} else {
			data_next = millis() + 4000;
		}
	} 
	ledOFF();
}

