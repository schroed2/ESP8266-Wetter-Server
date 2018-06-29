/* vim:set ts=4 sw=4 fileformat=unix fileencodings=utf-8 fileencoding=utf-8 noexpandtab:  */

/**\file
 * \brief  Weather senor (standalone server variant)
 * \author Ralf Schröder
 * 
 * (C) Feel free to change (MIT)
 *
 */ 

//#define WITH_LED 2    /**< activate the onboard LED for testing, use the given LED */
#define WITH_SERIAL   /**< activate the serial printings for testing */
#define LIGHT_SLEEP   /**< no deep sleep if defined, attend the link XPD_DCDC->RST for deep sleep */
#define CLIENT        /**< additional data transmittion as for wetter_sensor */
#define WEBSERVER     /**< active web server scripts */
#undef WITH_OTA      /**< Integrate over the air update (not on S0) */
#define VERSION "0.9" /**< Version */ 
#define BUILD 18      /**< Build number */ 
#undef USE_DTH        /**< DTH sensor used */
#define USE_DS18B20   /**< DS18B20 sensor used */
#define LOCATION "Testsensor2"
#define SENSOR_PIN 2
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

#include <ESP8266WiFi.h>

static const char* ssid = WLAN_SSID;
static const char* password = WLAN_PASSWORD;
static const int interval_sec = 60;  // >= 2
static const int data_max = 24 * 3600 / interval_sec;  // 1 day
static const int sensors_max = 2;  // max supported sensors
//static const char* sensor_id = "Ralfs Wetterstation"; //< ID for data base separation, could be the sensor location name */
static const char* sensor_id = "Ralfs " LOCATION; //< ID for data base separation, could be the sensor location name */

#ifdef CLIENT
static IPAddress weather_server(192,168,178,31);
static const int weather_port = 80;
#endif /* CLIENT */


#ifdef USE_DTH
#include <DHT.h>
#endif /* USE_DTH */
#ifdef USE_DS18B20
#include <OneWire.h>
#include <DallasTemperature.h>
#endif /* USE_DS18B20 */

#ifdef WEBSERVER
#include <TimeLib.h> 
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#endif /* WEBSERVER */

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


#ifdef WEBSERVER
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
#endif /* WEBSERVER */

static unsigned long reconnect_timeout;  /**< Reconnect timeout within loop() for lost WiFi connections (millis() value) */

#ifdef CLIENT
static WiFiClient client;
static unsigned long client_timeout;
static unsigned long client_error;
#endif /* CLIENT */

// Sensor object
#ifdef USE_DTH
static DHT dht(SENSOR_PIN, DHT22);
#endif /* USE_DTH */
#ifdef USE_DS18B20
static OneWire oneWire(SENSOR_PIN);
static DallasTemperature DS18B20(&oneWire);
static uint8_t numberOfDevices;              /** connected temperature devices */
static DeviceAddress devAddr[sensors_max];
static boolean requested;
#endif /* USE_DS18B20 */
static unsigned long data_next;          /**< time to read the sensor (millis value) */


// sensor data
static int data_index;
static int data_overrun = -1;

static struct
{
	int temp;
#ifdef USE_DTH
	int hum;
#endif /* USE_DTH */
} data[sensors_max][data_max];


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
	static unsigned long next = data_next;
	if (next != data_next && wait > 1000)
	{
		TRACE("sleeping %lu msec at: %lu\n", wait, now);
		next = data_next;
	}
	if (wait > 500) { wait = 500; }
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
				"Host: %u.%u.%u.%u:%u\r\n"
				"Connection: close\r\n"
				"Content-Type: application/json; charset=utf-8\r\n"
				"Content-Length: %u\r\n\r\n", weather_server[0], weather_server[1], weather_server[2], weather_server[3], weather_port, jlen);
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
			case 7:
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
	bool ret = true;
	ledON();

#ifdef USE_DTH
	if (++data_index >= data_max) {
		data_index = 0;
		data_overrun++;
	}
	data[0][data_index].temp = -1000;

	float t = dht.readTemperature();
	float h = dht.readHumidity();
	if (!isnan(t) && !isnan(h))
	{
		data[0][data_index].temp = (int)(t * 10 + 0.5);
		data[0][data_index].hum = (int)(h * 10 + 0.5);
#ifdef CLIENT
		transmit_msg(t, h);
#endif /* CLIENT */
		TRACE("Index:%d/%d Temperatur: %d.%d°C Luftfeuchtikeit: %d.%d%% RSSI:%d\n",
				data_overrun, data_index, data[0][data_index].temp / 10, data[0][data_index].temp % 10, data[0][data_index].hum / 10, data[0][data_index].hum % 10,
				WiFi.RSSI());
	} else {
		TRACE("%s: reading sensor failed, try again\n", __func__);
#ifdef DHT_VCC_PIN
		digitalWrite(DHT_VCC_PIN, LOW);
		delay(20);
		digitalWrite(DHT_VCC_PIN, HIGH);
#endif /* DHT_VCC_PIN */
		ret = false;
	}
#endif /* USE_DTH */
#ifdef USE_DS18B20
	int i;
	float t[numberOfDevices];
	if (!DS18B20.isConversionComplete()) 
	{ 
		ret = false;
	} else {
		if (++data_index >= data_max) {
			data_index = 0;
			data_overrun++;
		}
		requested = false;
		for(i = 0; i < numberOfDevices; i++)
		{
			t[i] = DS18B20.getTempC(devAddr[i]);
			data[i][data_index].temp = (int)(t[i] * 10 + 0.5);
			TRACE("Sensor %d Index:%d/%d Temperatur: %d.%d°C RSSI:%d\n",
					i+1, data_overrun, data_index, data[i][data_index].temp / 10, data[i][data_index].temp % 10, WiFi.RSSI());
		}
#ifdef CLIENT
		transmit_msg(t[0], -1);
#endif /* CLIENT */
	}
#endif /* USE_DS18B20 */

	ledOFF();
	return ret;
}


#ifdef WEBSERVER
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
	String arg = server.arg("n");
	unsigned sensor = 0;
	if (arg.c_str()[0]) { sensor = arg.c_str()[0] - '1'; }
	if (sensor >= sensors_max)
	{
		snprintf(buf, sizeof(buf), "<html>illegal sensor %u allowed %u</html>", sensor+1, sensors_max);
		server.send( 200, "text/html", buf);
	} else {
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
				"<p>%d.%u&deg;C"
#ifdef USE_DTH
				"bei %u.%u%% Luftfeuchtigkeit"
#endif /* USE_DTH */
				"</p>\n"
				"<embed src=\"graph.svg?n=%u\" type=\"image/svg+xml\" />"
				"</body>"
				"</html>",
			interval_sec, sensor_id, VERSION, BUILD, sensor_id, hour(t), minute(t), second(t), day(t), month(t), year(t), hr, min % 60, sec % 60,
			data[sensor][data_index].temp / 10, data[sensor][data_index].temp % 10,
#ifdef USE_DTH
			data[sensor][data_index].hum / 10, data[sensor][data_index].hum % 10,
#endif /* USE_DTH */
			sensor+1
				);
		server.send ( 200, "text/html", buf );
		TRACE("%s\n",__func__);
	}
	ledOFF();
}

static void onTemperature()
{
	ledON();
	unsigned len = 0;
	for(int i = 0; i < sensors_max; i++)
	{
		len = snprintf(buf+len, sizeof(buf)-len, "%d.%d\n", data[i][data_index].temp / 10, data[i][data_index].temp % 10);
	}
	server.send(200, "text/plain", buf);
	TRACE_LINE(buf);
	ledOFF();
}

#ifdef USE_DTH
static void onHumidity()
{
	ledON();
	snprintf(buf, sizeof(buf), "%d.%d\n", data[0][data_index].hum / 10, data[0][data_index].hum % 10);
	server.send( 200, "text/plain", buf);
	TRACE_LINE(buf);
	ledOFF();
}
#endif /* USE_DTH */

static void onSensor()
{
	ledON();
	String arg = server.arg("n");
	unsigned sensor = 0;
	if (arg.c_str()[0]) { sensor = arg.c_str()[0] - '1'; }
	if (sensor >= sensors_max)
	{
		snprintf(buf, sizeof(buf), "<html>illegal sensor %u allowed %u</html>", sensor+1, sensors_max);
		server.send( 200, "text/html", buf);
	} else {
		snprintf(buf, sizeof(buf), "<html>Uptime:%d Temperatur: %d.%d&deg;C"
#ifdef USE_DTH
				" Luftfeuchtikeit: %d.%d%%"
#endif /* USE_DTH */
				"</html>\n",
				data_overrun * interval_sec * data_max + data_index * interval_sec,
				data[sensor][data_index].temp / 10, data[sensor][data_index].temp % 10
#ifdef USE_DTH
				,data[sensor][data_index].hum / 10, data[sensor][data_index].hum % 10
#endif /* USE_DTH */
				);
		server.send( 200, "text/html", buf);
	}
	TRACE_LINE(buf);
	ledOFF();
}

#ifdef WITH_LED
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
#endif /* WITH_LED */

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
		out += -(data_max - count) * interval_sec / 60;
		out += '\t'; out += data[0][index].temp;
#ifdef USE_DTH
	   	out += '\t'; out += data[0][index].hum;
#endif /* USE_DTH */
	   	out += '\n';
	}
	server.send ( 200, "text/plain", out);
	TRACE("%s: duration: %lu\n", __func__, millis() - now_ms);
	ledOFF();
}

static void onGraph()
{
	ledON();
	String arg = server.arg("n");
	unsigned sensor = 0;
	if (arg.c_str()[0]) { sensor = arg.c_str()[0] - '1'; }
	if (sensor >= sensors_max)
	{
		snprintf(buf, sizeof(buf), "<html>illegal sensor %u allowed %u</html>", sensor+1, sensors_max);
		server.send( 200, "text/html", buf);
	} else {
		unsigned long now_ms = millis();
		int count, t_min = 0, t_max = 0;
		for (count = 0; count < data_max; count++)
		{
			if (data[sensor][count].temp <= -1000) { continue; }
			if (t_min > data[sensor][count].temp) {
				t_min = data[sensor][count].temp;
			}
			if (t_max < data[sensor][count].temp) {
				t_max = data[sensor][count].temp;
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
		String str2 = String("\" style=\"fill:none;stroke:MediumBlue;stroke-width:2\" />\n"
#ifdef USE_DTH
				"<polyline points=\"");
		String str3 = String("\" style=\"fill:none;stroke:DarkGreen;stroke-width:2\" />\n"
#endif /* USE_DTH */
				);
		String str4 = String("<text x=\"") + (data_max/2) + "\" y=\"" + (t_norm + 75) + "\" fill=\"MediumBlue\">24h Temperatur " + (t_min/10) + "-" + (t_max/10) + "°C</text>\n"
#ifdef USE_DTH
			"<text x=\"100\" y=\""                          + (t_norm + 75) + "\" fill=\"DarkGreen\">24h Luftfeuchtigkeit 0-100%</text>\n"
#endif /* USE_DTH */
			"</svg>\n\r\n";

		len = (str1.length() + str2.length() + 10 * data_max
#ifdef USE_DTH
				+ str3.length() + 10 * data_max
#endif /* USE_DTH */
				+ str4.length());
		len += (sizeof("<text x=\"15\" y=\"0000\" fill=\"MediumBlue\">000°C</text>\n" "<text x=\"0000\" y=\"0000\" fill=\"MediumBlue\">000%</text>\n") - 1) * (t_norm / 50 + 1);
		server.prepareHeader(200, "image/svg+xml", len);
		server.sendContent(str1); sent = str1.length();

		TRACE("%s step1: %lums\n", __func__, millis() - now_ms);
		char buf[200];
		int x;
		for (x = 0; x < data_max; x++)
		{
			int i = (data_index + x + 1) % data_max;
			if (data[sensor][i].temp <= -1000) { continue; }
			sent += snprintf(buf, sizeof(buf), "%4d,%4d ", x + step_v, t_max - data[sensor][i].temp + 50); /* size 10 */
			server.sendContent(buf);
		}
		server.sendContent(str2); sent += str2.length();
#ifdef USE_DTH
		for (x = 0; x < data_max; x++)
		{
			int i = (data_index + x + 1) % data_max;
			if (data[sensor][i].temp <= -1000) { continue; }
			sent += snprintf(buf, sizeof(buf), "%4d,%4d ", x + step_v, (1000 - data[sensor][i].hum) * t_norm / 1000 + 50); /* size 10 */
			server.sendContent(buf);
		}
		server.sendContent(str3); sent += str3.length();
#endif /* USE_DTH */
		for (x = 0; x <= t_norm / 50; x++)
		{
			sent += snprintf(buf, sizeof(buf),
					"<text x=\"%d\" y=\"%d\" fill=\"MediumBlue\">%2d°C</text>\n"
#ifdef USE_DTH
					"<text x=\"5\" y=\"%d\" fill=\"DarkGreen\">%3d%%</text>\n"
#endif /* USE_DTH */
					,data_max + step_v + 5,
					55 + x * 50, (t_max - x * 50) / 10
#ifdef USE_DTH
					,55 + x * 50,  100 - 5000 *x / t_norm
#endif /* USE_DTH */
					);
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
	}
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
#endif /* WEBSERVER */

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
		case WL_DISCONNECTED: TRACE_LINE("WiFi: WL_DISCONNECTED\n"); break;
		default: TRACE("%s: illegal state %d", __func__, status); break;
	}
	return status;
}
#else
#define wifi_trace(status) status
#endif



#ifdef USE_DS18B20
//Setting the temperature sensor
static void setupDS18B20()
{
	DS18B20.begin();
	uint8_t devs = DS18B20.getDeviceCount();
	numberOfDevices = DS18B20.getDS18Count();
	TRACE("%s: device count: %u/%u/%u parasite power is: %s\n", __func__, numberOfDevices, devs, sensors_max, (DS18B20.isParasitePowerMode() ? "ON" : "OFF"));
	if (numberOfDevices > sensors_max) { numberOfDevices = sensors_max; }

	data_next = millis() + 2000;
	DS18B20.setWaitForConversion(true);
	DS18B20.requestTemperatures();

	// Loop through each device, print out address
	for(int i=0, d=0; d < devs; d++)
	{
		DeviceAddress addr;
		// Search the wire for address
		if (DS18B20.getAddress(addr, d))
		{
			if (DS18B20.validFamily(addr) && (i < sensors_max) && DS18B20.getAddress(devAddr[i], d))
			{
				i++;
				TRACE("%s: found device %d/%d with address: %02X %02X %02X %02X %02X %02X %02X %02X resolution: %d temperature: %f°C\n",
					   	__func__, i, d,
						addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
						DS18B20.getResolution(addr), DS18B20.getTempC(addr));
			} else {
				TRACE("%s: found other device %d with address: %02X %02X %02X %02X %02X %02X %02X %02X\n",
					   	__func__, d, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
			}
		} else {
			TRACE("%s: found ghost device at %d/%d but could not detect address. Check power and cabling\n", __func__, i+1, devs);
		}
	}
	DS18B20.setWaitForConversion(false);
}
#endif /* USE_DS18B20 */

static void setupWiFi()
{
	WiFi.persistent(false);
	if (WiFi.mode(WIFI_STA))
	{
		TRACE("%s: Connecting to %s", __func__, ssid);
#if 0
		IPAddress ip(192,168,178,38);
		IPAddress gw(192,168,178,1);
		IPAddress mask(255,255,255,0);
		WiFi.config(ip, gw, mask);
#endif
		WiFi.begin(ssid, password);

		if (WiFi.getAutoConnect()) { WiFi.setAutoConnect(false); }
		WiFi.setAutoReconnect(true);
	} else {
		TRACE("%s: WiFi no in station mode (FATAL)", __func__);
		ESP.reset();
	}
}


void setup()
{
	WiFi.mode(WIFI_OFF);
	WiFi.forceSleepBegin();
	delay(1);

#ifdef WITH_LED
	pinMode(WITH_LED, OUTPUT);
	ledFlash = millis() + 5000;
#endif /* WITH_LED */
	ledON();

#ifdef WITH_SERIAL
	Serial.begin(115200);
	delay(30);
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

	TRACE("\n-------------------\n%s reset reason: %s flash: %s size: %u real %u speed %u freq %uMHz scetch %u free %u Vcc: %d\n",
			__func__, ESP.getResetInfo().c_str(), FLASH_MAP[fmap],
		 ESP.getFlashChipSize(), ESP.getFlashChipRealSize(), ESP.getFlashChipSpeed(), ESP.getCpuFreqMHz(),
		 ESP.getSketchSize(), ESP.getFreeSketchSpace(), ESP.getVcc());
#endif /* WITH_SERIAL */

#ifdef USE_DS18B20
	/** Start early because we have to wait 1.5sec before first reading */
	setupDS18B20();
	data_next = millis();
#endif /* USE_DS18B20 */

	// Connect to WiFi network
	setupWiFi();
	for(int count = 0; count < 100; count++)
	{
			if (WiFi.status() == WL_CONNECTED) { break; }
			delay(50);
			TRACE(".");
	}
	if (WL_CONNECTED == wifi_trace(WiFi.status()))
	{
		TRACE("%s WiFI connected on %s channel%u, IP: %s\n", __func__, WiFi.SSID().c_str(), WiFi.channel(), WiFi.localIP().toString().c_str());
	}

	for(data_index = 0; data_index < data_max; data_index++)
	{ 
		for(int i = 0; i < sensors_max; i++) { data[i][data_index].temp = -1000; } 
	}
	data_index = data_max - 1;

#ifdef WITH_OTA
	ArduinoOTA.setHostname(LOCATION);
#ifdef WITH_SERIAL
	ArduinoOTA.onStart([]() { TRACE("Start updating %s\n", ((ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem")); });
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

#ifdef USE_DTH
#ifdef DHT_VCC_PIN
	pinMode(DHT_VCC_PIN, OUTPUT);
	digitalWrite(DHT_VCC_PIN, HIGH);
#endif /* DHT_VCC_PIN */
	dht.begin();
	data_next = millis() + 2000;
#endif /* USE_DTH */

#ifdef WEBSERVER
	// Start the server
	server.on("/", onRoot);
	server.on("/graph.svg", onGraph);
	server.on("/list", onList);
#ifdef WITH_LED
	server.on("/led1", onLedON);
	server.on("/led0", onLedOFF);
#endif /* WITH_LED */
	server.on("/temperature", onTemperature);
#ifdef USE_DTH
	server.on("/humidity", onHumidity);
#endif /* USE_DTH */
#ifdef WITH_SERIAL
	server.on("/trace", []() { doTrace = !doTrace; server.send(200, "text/play", "ok"); });
#endif /* WITH_SERIAL */
	server.on("/sensor", onSensor);
	server.onNotFound(handleNotFound);
	server.begin();
	TRACE_LINE("WEB Server started\n");

	Udp.begin(localPort);
	TRACE("waiting for time sync, port %u\n", Udp.localPort());
	setSyncProvider(getNtpTime);
#endif /* WEBSERVER */

	ledOFF();
	TRACE("%s fin at %lu\n", __func__, millis());
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
			TRACE("%s: WiFi disconnected, await reconnection on state ", __func__); wifi_trace(WiFi.status());
			reconnect_timeout = millis() + 5000;
		} else if (reconnect_timeout < millis()) {
			TRACE("%s: WiFi no WLAN connection (retry %u) on state ", __func__, wifi_retry_count); wifi_trace(WiFi.status());
			switch(WiFi.status())
			{
				case WL_CONNECTED:
					break;
				case WL_DISCONNECTED:
					ledON();
					TRACE("%s: WiFi disconnected, connect again\n", __func__);
					WiFi.mode(WIFI_STA);
					WiFi.begin(ssid, password);
					reconnect_timeout = millis() + 10000;
					break;
				case WL_CONNECTION_LOST:
					ledON();
					TRACE("%s: WiFi connection lost, restart", __func__);
					WiFi.reconnect();
					reconnect_timeout = millis() + 10000;
					break;
				case WL_IDLE_STATUS:
					reconnect_timeout = millis() + 10000;
					break;
				default:
					reconnect_timeout = 0;
					WiFi.disconnect(true);
					ledOFF();
					data_next += interval_sec;
					deep_sleep(); // give up, wait one data interval
					break;
			}
			delay(100);
			wifi_trace(WiFi.status());
			if (wifi_retry_count++ == 5)
			{
				TRACE("\n%s: no WLAN connection, retry count %u force reset\n", __func__, wifi_retry_count);
				ESP.reset();
			}
		} else {
			TRACE(".");
			delay(100);
		}
		return;
	}
	if (reconnect_timeout)
	{
		TRACE("%s WiFI connected on %s channel%u, IP: %s\n", __func__, WiFi.SSID().c_str(), WiFi.channel(), WiFi.localIP().toString().c_str());
		reconnect_timeout = 0;
		wifi_retry_count = 0;
		ledOFF();
	}
	// we are connected with WiFi here

#ifdef WEBSERVER
	server.handleClient();
#endif /* WEBSERVER */
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
#ifdef USE_DS18B20
		if (!requested) { requested = true; DS18B20.requestTemperatures(); }
#endif
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
#ifdef USE_DTH
			data_next = millis() + 4000;
#endif
#ifdef USE_DS18B20
			data_next = millis() + 10;

#endif
		}
	}
	ledOFF();
	deep_sleep();
}

