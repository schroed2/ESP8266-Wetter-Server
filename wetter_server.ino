/* Editor hints for vim
 * vim:set ts=4 sw=4 fileencoding=utf-8 fileformat=unix noexpandtab: */
/**\file
 * \brief  Weather senor (standalone server variant)
 * \author Ralf Schröder
 * 
 * (C) Feel free to change (MIT)
 *
 */ 

#undef CLIENT
#define WITH_LED

#include <ESP8266WiFi.h>
#include <DHT.h>

#include <TimeLib.h> 
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>



#ifdef WITH_LED
#define ledON()  digitalWrite(2, 0)
#define ledOFF() digitalWrite(2, 1)
#else
#define ledON()
#define ledOFF()
#endif

static const char* ssid = "ssid";
static const char* password = "password";
static const int interval_sec = 60;  // >= 2

static const int data_max = 24 * 3600 / interval_sec;  // 1 day

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
			Serial.print(header);
			_currentClientWrite(header.c_str(), header.length());
		}
};

// Create an instance of the server
// specify the port to listen on as an argument
static myServer server(80);


#ifdef CLIENT
static WiFiClient client;
static unsigned client_timeout;
static unsigned reconnect_timeout;
#endif /* CLIENT */

// Sensor object
static DHT dht(D2, DHT22);


// sensor data
static struct
{
	int temp;
	int hum;
} data[data_max];
static int data_index;
static int data_overrun = -1;
static unsigned long data_next;



static void deep_sleep()
{
	unsigned wait = data_next - millis();
	if (wait > 60000) { wait = 60000; }
	Serial.println(String("sleeping ") + wait + "msec at: " + millis());
	delay(wait);
	ESP.deepSleep(wait*1000);
	delay(100);
	Serial.println(String("wakeup: ") + millis());
#endif
}

#ifdef CLIENT
static void transmit_msg(float temperature, float humidity)
{
	if (client.connect("raspi.fritz.box", 8000))
	{
		String json = String("{\"sender_id\":\"" + WiFi.macAddress() + "\",\"password\":\"geheim\",\"temperature\":") + temperature + ",\"humidity\":" + humidity + "}\r\n";
		String header = String("POST /esp8266_trigger HTTP/1.1\r\nHost: raspi.fritz.box:8000\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: ") + json.length() + "\r\n\r\n";
		Serial.print(header);
		Serial.print(json);
		client.print(header + json);
		client_timeout = millis() + 5000;
	} else {
		Serial.println("no http server connection");
	}
}
#endif /* CLIENT */


static void sensorRead()
{
	ledON();

	if (++data_index >= data_max) {
		data_index = 0;
		data_overrun++;
	}
	data[data_index].temp = data[data_index].hum = -1;

	float t, h;
	int count;
	for (count = 0; count < 5; count++) {
		t = dht.readTemperature();
		h = dht.readHumidity();
		if (!isnan(t) && !isnan(h))
		{
			data[data_index].temp = (int)(t * 10 + 0.5);
			data[data_index].hum = (int)(h * 10 + 0.5);
#ifdef CLIENT
			transmit_msg(t, h);
#endif /* CLIENT */
			break;
		}
		Serial.println("reading sensor failed, try again");
		delay(2000);
	}

	char buf[60];
	snprintf(buf, sizeof(buf), "Index:%d/%d Temperatur: %d.%d°C Luftfeuchtikeit: %d.%d%%", data_overrun, data_index, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10);
	ledOFF();
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

	char temp[600];
	int sec = millis() / 1000;
	int min = sec / 60;
	int hr = min / 60;
	time_t t = now();
	t += timeZone_de(t);

	snprintf ( temp, sizeof(temp),
			"<!DOCTYPE html>"
			"<html>"
			"<head>"
			"<meta charset=\"utf-8\" http-equiv='refresh' content='%u'/>"
			//"<meta charset=\"utf-8\" />"
			"<title>Ralfs Zimmerstation</title>"
			"<style>"
			"body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }"
			"svg { width: 98%%; height: auto; }"
			"</style>"
			"</head>"
			"<body>"
			"<h1>Ralfs Zimmerstation</h1>"
			"<p>Uhrzeit: %02d:%02d:%02d %02u.%02u.%04u Online seit: %02u:%02u:%02u</p>"
			"<p>%u.%u&deg;C bei %u.%u%% Luftfeuchtigkeit</p>\n"
			"<embed src=\"graph.svg\" type=\"image/svg+xml\" />"
			"</body>"
			"</html>",
			interval_sec, hour(t), minute(t), second(t), day(t), month(t), year(t), hr, min % 60, sec % 60, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10
			//hour(t), minute(t), second(t), day(t), month(t), year(t), hr, min % 60, sec % 60, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10
				);
	server.send ( 200, "text/html", temp );
	Serial.println(__func__);
	ledOFF();
}

static void onTemperature()
{
	ledON();
	char buf[400];
	snprintf(buf, 400, "%d.%d", data[data_index].temp / 10, data[data_index].temp % 10);
	server.send(200, "text/plain", buf);
	Serial.println(__func__);
	ledOFF();
}

static void onHumidity()
{
	ledON();
	char buf[400];
	snprintf(buf, 400, "%d.%d", data[data_index].hum / 10, data[data_index].hum % 10);
	server.send( 200, "text/plain", buf);
	Serial.println(__func__);
	ledOFF();
}

static void onSensor()
{
	ledON();
	char buf[400];
	snprintf(buf, 400, "<html>Uptime:%d Temperatur: %d.%d&deg;C Luftfeuchtikeit: %d.%d%%</html>",
			data_overrun * interval_sec * data_max + data_index * interval_sec, data[data_index].temp / 10, data[data_index].temp % 10, data[data_index].hum / 10, data[data_index].hum % 10);
	server.send( 200, "text/html", buf);
	Serial.println(__func__);
	ledOFF();
}

static void onLedON()
{
	ledON();
	server.send( 200, "text/html", "<html>LED ON</html>");
	Serial.println(__func__);
}

static void onLedOFF()
{
	ledOFF();
	server.send( 200, "text/html", "<html>LED OFF</html>");
	Serial.println(__func__);
}

static void handleNotFound()
{
	ledON();
	String message = "File Not Found\n\n";
	message += "URI: ";
	message += server.uri();
	message += "\nMethod: ";
	message += ( server.method() == HTTP_GET ) ? "GET" : "POST";
	message += "\nArguments: ";
	message += server.args();
	message += "\n";

	for ( uint8_t i = 0; i < server.args(); i++ ) {
		message += " " + server.argName ( i ) + ": " + server.arg ( i ) + "\n";
	}

	server.send ( 404, "text/plain", message );
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
	server.send ( 200, "test/plain", out);
	Serial.println(String(__func__) + " duration: " + (millis() - now_ms));
	ledOFF();
}

static void onGraph()
{
	ledON();
	unsigned long now_ms = millis();
	int count, t_min = 0, t_max = -10000;
	for (count = 0; count < data_max; count++)
	{
		if (t_min > data[count].temp) {
			t_min = data[count].temp;
		}
		if (t_max < data[count].temp) {
			t_max = data[count].temp;
		}
	}
	t_min = t_min - t_min % 50 - ((t_min < 0) ? 50 : 0);
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
	String str4 = String("<text x=\"") + (data_max/2) + "\" y=\"" + (t_norm + 75) + "\" fill=\"MediumBlue\">24h Temperatur " + (t_min/10) + "-" + (t_max/10) + "Â°C</text>\n"
		"<text x=\"100\" y=\""                          + (t_norm + 75) + "\" fill=\"DarkGreen\">24h Luftfeuchtigkeit 0-100%</text>\n"
		"</svg>\n\r\n";

	len = (str1.length() + str2.length() + 20 * data_max + str3.length() + str4.length());
	len += (sizeof("<text x=\"15\" y=\"0000\" fill=\"MediumBlue\">000Â°C</text>\n" "<text x=\"0000\" y=\"0000\" fill=\"MediumBlue\">000%</text>\n") - 1) * (t_norm / 50 + 1);
	server.prepareHeader(200, "image/svg+xml", len);
	server.sendContent(str1); sent = str1.length();

	Serial.println(String(__func__) + " step1: " + (millis() - now_ms));
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
				"<text x=\"%d\" y=\"%d\" fill=\"MediumBlue\">%2dÂ°C</text>\n"
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

	Serial.println(String(__func__) + " " + (millis() - now_ms));
	ledOFF();
}



/*-------- NTP code ----------*/

static const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
static byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

static time_t getNtpTime()
{
	while (Udp.parsePacket() > 0) ; // discard any previously received packets
	Serial.println("Transmit NTP Request");
	sendNTPpacket(timeServer);
	uint32_t beginWait = millis();
	while (millis() - beginWait < 1500) {
		int size = Udp.parsePacket();
		if (size >= NTP_PACKET_SIZE) {
			Serial.println("Receive NTP Response");
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
	Serial.println("No NTP Response :-(");
	return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
static void sendNTPpacket(IPAddress &address)
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
	Udp.beginPacket(address, 123); //NTP requests are to port 123
	Udp.write(packetBuffer, NTP_PACKET_SIZE);
	Udp.endPacket();
}

void setup()
{
	Serial.begin(115200);
	delay(10);

#ifdef LED
	// prepare GPIO2
	pinMode(led, OUTPUT);
#endif
	ledON();

	// Connect to WiFi network
	Serial.print("Connecting to "); Serial.println(ssid);
	WiFi.mode ( WIFI_STA );
	WiFi.begin(ssid, password);

	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(".");
	}
	Serial.println("");
	Serial.print("WiFi connected with "); Serial.println(WiFi.localIP());

	for(data_index = 0; data_index < data_max; data_index++) { data[data_index].hum = -1; }
	data_index = data_max - 1;

	// Start the server
	server.on( "/", onRoot);
	server.on( "/graph.svg", onGraph);
	server.on( "/list", onList);
	server.on( "/led1", onLedON);
	server.on( "/led0", onLedOFF);
	server.on( "/temperature", onTemperature);
	server.on( "/humidity", onHumidity);
	server.on( "/sensor", onSensor);
	server.onNotFound ( handleNotFound );
	server.begin();
	Serial.println("Server started");

	data_next = millis() + 2000;
	dht.begin();

	Udp.begin(localPort);
	Serial.println(Udp.localPort());
	Serial.println("waiting for time sync");
	setSyncProvider(getNtpTime);
	ledOFF();
}

void loop()
{
	if (WiFi.status() != WL_CONNECTED)
	{
		if (!reconnect_timeout)
		{ 
			reconnect_timeout = millis() + 5000;
			Serial.print(".");
			delay(100);
		} else if (reconnect_timeout < millis()) {
			Serial.println("no WLAN connection");
			reconnect_timeout = 0;
			deep_sleep(); return;
			data_next = millis();
		} else {
			Serial.print(".");
			delay(100);
		}
		return;
	}
	if (reconnect_timeout)
	{
		Serial.print("WiFi connected with "); Serial.println(WiFi.localIP());
		reconnect_timeout = 0;
	}

    // we are connected with WiFi here 
	server.handleClient();
	if (millis() >= data_next)
	{
		sensorRead();
		data_next = (data_overrun * interval_sec * data_max + (data_index + 1) * interval_sec) * 1000;
		char buf[400];
		snprintf(buf, 400, "now:%lu next sensor read %lu", millis(), data_next);
		Serial.println(buf);
	}

#ifdef CLIENT
	if (client_timeout)
	{
		if (client_timeout < millis())
		{
			Serial.println("\nstop client connection");
			client.stop();
			client_timeout = 0;
		} else if (client.available()) {
			String line  = client.readStringUntil('\r');
			Serial.print(line);
		}
	} else {
		deep_sleep();
	}
#endif /* CLIENT */
}

