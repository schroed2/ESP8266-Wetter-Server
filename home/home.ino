/* vim:set ts=4 sw=4 fileformat=unix fileencodings=utf-8 fileencoding=utf-8 noexpandtab:  */

/**\file
 * \brief  IR tests
 * \author Ralf Schröder
 *
 * (C) Feel free to change (MIT)
 *
 */

/*** Personal parameter adaptations ***/

#define WITH_UDP_LOG_PORT 2108		/** activates UDP logging */
#define WITH_LED D4					/** activate the onboard LED for testing, use the given LED */
#define WITH_SERIAL					/** activate the serial printings for testing */
#define WITH_OTA					/**< Integrate over the air update (not on S0) */
#undef USE_DHT						/**< DTH sensor used */
#define USE_DS18B20					/**< DS18B20 sensor used */
#define WITH_ADC					/**< VCC messure */
#define VERSION "0.1"				/**< Version */ 
#define BUILD 31					/**< Build number */ 
#define LOCATION "Testsensor2"
#define WITH_IR_REC_PIN D6			/**< IR receiver pin */
#define WITH_IR_SEND_PIN D5			/**< IR sender pin */
#define WITH_RF_REC_PIN D2			/**< ISR number of the RF receiver */
#undef WITH_RF_TFA // D2				/**< Enable TFA wether station receiver insted of switches */
#define WITH_RF_SEND_PIN D1			/**< RF sender connected on pin */


#if defined(USE_DS18B20) || defined(USE_DHT)
#define TEMPERATURE_SENSOR_PIN D7
#endif

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


#ifdef WITH_LED
#define ledON()  digitalWrite(WITH_LED, 0)
#define ledOFF() digitalWrite(WITH_LED, 1)
#else
#define ledON()
#define ledOFF()
#endif

#ifdef WITH_SERIAL
#define TRACELN_SERIAL(line) Serial.print(line)
#define TRACE_SERIAL(...)    Serial.printf(__VA_ARGS__)
#else
#define TRACELN_SERIAL(line) ((void)0)
#define TRACE_SERIAL(args...) ((void)0)
#endif


#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <TimeLib.h> 

#ifdef WITH_ADC
ADC_MODE(ADC_VCC)     /**< vcc read */
#endif /* WITH_ADC */

#ifdef WITH_OTA
#include <ArduinoOTA.h>
#endif /* WITH_OTA */

#ifdef WITH_UDP_LOG_PORT

#if 0
class RaspiUDP : public WiFiUDP
{
		uint8_t _active;
	public:
		RaspiUDP() : WiFiUDP(), _active(0) {}
		int active() { return _active ? 1 : 0; }
		uint8_t begin(uint16_t port) { return _active ? _active : (_active = WiFiUDP::begin(port)); }
};
#endif
static WiFiUDP raspiUDP;                /**< UDP message logger */

#define TRACELN_UDP(line) (raspiUDP.beginPacket("raspi.fritz.box", WITH_UDP_LOG_PORT) && (raspiUDP.print(line), raspiUDP.endPacket()))
#define TRACE_UDP(...) (raspiUDP.beginPacket("raspi.fritz.box", WITH_UDP_LOG_PORT) && (raspiUDP.printf(__VA_ARGS__), raspiUDP.endPacket()))
#else
#define TRACELN_UDP(line) 0
#define TRACE_UDP(args...) 0
#endif /* WITH_UDP_LOG_PORT */

static unsigned doTrace = 1;
#define TRACE_LINE(line) (doTrace && (TRACELN_SERIAL(line),TRACELN_UDP(line)))
#define TRACE(...)       (doTrace && (TRACE_SERIAL(__VA_ARGS__),TRACE_UDP(__VA_ARGS__)))

class myServer: public ESP8266WebServer
{
	public:
		char buf[600];
		myServer(int port) : ESP8266WebServer(port) {}
		void prepareHeader(int code, const char* content_type, unsigned content_len) {
			String header;
			_prepareHeader(header, code, content_type, content_len);
			TRACE_LINE(header);
			client().write(header.c_str(), header.length());
		}
};

extern "C" {
#include "user_interface.h"
}


static const char* wifi_ssid = WLAN_SSID;
static const char* wifi_password = WLAN_PASSWORD;
static myServer web_server(80);						/**< web server instance */
static unsigned long wifi_reconnect_timeout = 0;	/** If set, a data message was sent, until the set value the response should be received
														(millis() value). After that time hangup. */
static const char* sensor_id = "Ralfs " LOCATION;	/**< ID for data base separation, could be the sensor location name */


#ifdef TEMPERATURE_SENSOR_PIN
// Sensor object
static const int sensors_max = 1;        /* max supported sensors */
#ifdef USE_DHT
#include <DHT.h>
static DHT dht(TEMPERATURE_SENSOR_PIN, DHT22);
#endif /* USE_DHT */
#ifdef USE_DS18B20
#include <OneWire.h>
#include <DallasTemperature.h>
static OneWire oneWire(TEMPERATURE_SENSOR_PIN);
static DallasTemperature DS18B20(&oneWire);
static uint8_t numberOfDevices;              /** connected temperature devices */
static DeviceAddress devAddr[sensors_max];
static boolean requested;
#endif /* USE_DS18B20 */
static unsigned long data_next;					/**< absolute time to read the sensor (millis value) */
static const unsigned long interval_sec = 60;	/**< delay for reading the sensor (millis value) */
static float temperature[sensors_max];			/**< read temerature field */

//static IPAddress weather_server(192,168,178,31);
static const char* weather_server = "raspi.fritz.box";
static const int weather_port = 80;

static WiFiClient temperature_client;
static unsigned long client_timeout = 0;
static unsigned client_error = 0;
#endif /* TEMPERATURE_SENSOR_PIN */


#ifdef WITH_IR_SEND_PIN
#include <IRremoteESP8266.h>
#include <IRsend.h>

static IRsend irsend(WITH_IR_SEND_PIN);  // IR sender object
#endif /* WITH_IR_SEND_PIN */


#ifdef WITH_IR_REC_PIN
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>


static unsigned irPanasonicCmd = 0;
static unsigned ir_repeat = 0;

// The following are only needed for extended decoding of A/C Messages
#include <ir_Panasonic.h>

// As this program is a special purpose capture/decoder, let us use a larger
// than normal buffer so we can handle Air Conditioner remote codes.
static const uint16_t kCaptureBufferSize = 1024;

// kTimeout is the Nr. of milli-Seconds of no-more-data before we consider a
// message ended.
// This parameter is an interesting trade-off. The longer the timeout, the more
// complex a message it can capture. e.g. Some device protocols will send
// multiple message packets in quick succession, like Air Conditioner remotes.
// Air Coniditioner protocols often have a considerable gap (20-40+ms) between
// packets.
// The downside of a large timeout value is a lot of less complex protocols
// send multiple messages when the remote's button is held down. The gap between
// them is often also around 20+ms. This can result in the raw data be 2-3+
// times larger than needed as it has captured 2-3+ messages in a single
// capture. Setting a low timeout value can resolve this.
// So, choosing the best kTimeout value for your use particular case is
// quite nuanced. Good luck and happy hunting.
// NOTE: Don't exceed kMaxTimeoutMs. Typically 130ms.
#if DECODE_AC
// Some A/C units have gaps in their protocols of ~40ms. e.g. Kelvinator
// A value this large may swallow repeats of some protocols
static const uint8_t kTimeout = 50;
#else   // DECODE_AC
// Suits most messages, while not swallowing many repeats.
static const uint8_t kTimeout = 15;
#endif  // DECODE_AC
// Alternatives:
// const uint8_t kTimeout = 90;
// Suits messages with big gaps like XMP-1 & some aircon units, but can
// accidentally swallow repeated messages in the rawData[] output.
//
// const uint8_t kTimeout = kMaxTimeoutMs;
// This will set it to our currently allowed maximum.
// Values this high are problematic because it is roughly the typical boundary
// where most messages repeat.
// e.g. It will stop decoding a message and start sending it to serial at
//      precisely the time when the next message is likely to be transmitted,
//      and may miss it.

// Set the smallest sized "UNKNOWN" message packets we actually care about.
// This value helps reduce the false-positive detection rate of IR background
// noise as real messages. The chances of background IR noise getting detected
// as a message increases with the length of the kTimeout value. (See above)
// The downside of setting this message too large is you can miss some valid
// short messages for protocols that this library doesn't yet decode.
//
// Set higher if you get lots of random short UNKNOWN messages when nothing
// should be sending a message.
// Set lower if you are sure your setup is working, but it doesn't see messages
// from your device. (e.g. Other IR remotes work.)
// NOTE: Set this value very high to effectively turn off UNKNOWN detection.
static const uint16_t kMinUnknownSize = 12;
// ==================== end of TUNEABLE PARAMETERS ====================

// Use turn on the save buffer feature for more complete capture coverage.
static IRrecv irrecv(WITH_IR_REC_PIN, kCaptureBufferSize, kTimeout, true);


#if DECODE_AC
// Display the human readable state of an A/C message if we can.
static void dumpACInfo(class decode_results &results)
{
  String description = "";
#if DECODE_PANASONIC_AC
  if (results.decode_type == PANASONIC_AC &&
      results.bits > kPanasonicAcShortBits) {
    IRPanasonicAc ac(0);
    ac.setRaw(results.state);
    description = ac.toString();
  }
#endif  // DECODE_PANASONIC_AC
  if (description != "") TRACE("%s: %s", __func__, description.c_str());
}
#else
#define dumpACInfo(results)
#endif /* DECODE_AC */

#define IR_REC_ON() irrecv.enableIRIn()
#define IR_REC_OFF() irrecv.disableIRIn()
#else
#define IR_REC_ON()
#define IR_REC_OFF()
#endif /* WITH_IR_REC_PIN */

#ifdef WITH_RF_TFA
#include <tfa433.h>
TFA433 tfa = TFA433(); //Input pin where 433 receiver is connected.
#endif /* WITH_RF_TFA */

#if defined(WITH_RF_REC_PIN) || defined(WITH_RF_SEND_PIN)
#include <RCSwitch.h>
static RCSwitch mySwitch = RCSwitch();
#endif /* WITH_RF_REC_PIN */



static int wifi_trace(int status)
{
	switch (status)
	{
		case WL_CONNECTED: TRACE_LINE("WiFi: WL_CONNECTED\n"); break;
		case WL_NO_SHIELD: TRACE_LINE("WiFi: WL_NO_SHIELD\n"); break;
		case WL_IDLE_STATUS: TRACE_LINE("WiFi: WL_IDLE_STATUS\n"); break;
		case WL_NO_SSID_AVAIL: TRACE_LINE("WiFi: WL_NO_SSID_AVAIL\n"); break;
		case WL_SCAN_COMPLETED: TRACE_LINE("WiFi: WL_SCAN_COMPLETED\n"); break;
		case WL_CONNECT_FAILED: TRACE_LINE("WiFi: WL_CONNECT_FAILED\n"); break;
		case WL_CONNECTION_LOST: TRACE_LINE("WiFi: WL_CONNECTION_LOST\n"); break;
		case WL_DISCONNECTED: TRACE_LINE("WiFi: WL_DISCONNECTED\n"); break;
		default: TRACE("%s: illegal state %d", __func__, status); break;
	}
	return status;
}


#ifdef TEMPERATURE_SENSOR_PIN
/** Transmit a temperature to a web_server */
static void transmit_msg(float temperature, float humidity)
{
	if (temperature_client.connect(weather_server, weather_port))
	{
		unsigned jlen = snprintf(web_server.buf, sizeof(web_server.buf), 
				"{\"sender_id\":\"%s\",\"password\":\"" DB_SECRET "\",\"temperature\":%.1f,\"humidity\":%.1f"
#ifdef WITH_ADC
				",\"vcc\":%.2f"
#endif /* WITH_ADC */
				"}\r\n",
				sensor_id, temperature, humidity
#ifdef WITH_ADC
				, ESP.getVcc() / 1000.0
#endif /* WITH_ADC */
				);
		char* header = web_server.buf + jlen + 1;
		unsigned hlen = snprintf(header, sizeof(web_server.buf)-jlen-1,
				"POST /sensor.php HTTP/1.1\r\n"
				//"Host: %u.%u.%u.%u:%u\r\n"
				"Host: %s:%u\r\n"
				"Connection: close\r\n"
				"Content-Type: application/json; charset=utf-8\r\n"
				"Content-Length: %u\r\n\r\n",
				weather_server, weather_port, jlen);
		TRACE_LINE(header);
		TRACE_LINE(web_server.buf);
		temperature_client.write(header, hlen); temperature_client.write(web_server.buf, jlen);
		client_timeout = millis() + 3000;  // expect answer fin after 3 sec
	} else {
		TRACE("%s: no http server connection\n", __func__);
		wifi_trace(WiFi.status());
		client_error++;
	}
	switch (client_error)
	{
		case 0: break;
		case 3:
				TRACE("%s client connection errors:%u - force WiFi shutdown\n", __func__, client_error);
				if (temperature_client.connected()) { temperature_client.stop(); client_timeout = 0; }
				WiFi.mode(WIFI_OFF);
				delay(5000);
				break;
		case 7:
				TRACE("%s client connection errors:%u - force ESP reset\n", __func__, client_error);
				ESP.reset();
				break;
		default:
				TRACE("%s client connection errors:%u now:%lu\n", __func__, client_error, millis());
	}
}
#endif /* TEMPERATURE_SENSOR_PIN */

#ifdef USE_DS18B20
static bool sensorRead()
{
	bool ret = true;
	ledON();
	if (!requested)
	{ 
			TRACE("%s: sensor data request at %lu\n", __func__, millis());
			requested = true;
			DS18B20.requestTemperatures();
	}
	if (!DS18B20.isConversionComplete()) 
	{ 
		if (data_next + 5000 < millis())
		{
			TRACE("%s: sensor data request failed at %lu - skipping\n", __func__, millis());
			requested = false;
		} else {
			ret = false;
		}
	} else {
		requested = false;
		for(unsigned i = 0; i < numberOfDevices; i++)
		{
			temperature[i] = DS18B20.getTempC(devAddr[i]);
			TRACE("Sensor %d Temperatur: %.1f°C RSSI:%d\n", i+1, temperature[i], WiFi.RSSI());
			if (0 == i) { transmit_msg(temperature[0], -1); }
		}
	}
	ledOFF();
	return ret;
}
#endif /* USE_DS18B20 */

#ifdef USE_DHT
static bool sensorRead()
{
	bool ret = true;
	ledON();

	float t = dht.readTemperature();
	float h = dht.readHumidity();
	if (!isnan(t) && !isnan(h))
	{
		temperature[0] = t;
		humidity[0] = h;
#ifdef CLIENT
		transmit_msg(t, h);
#endif /* CLIENT */
		TRACE("Sensor DHT Temperatur: %.1f°C humidity %.1%% RSSI:%d\n", t, h, WiFi.RSSI());
	} else {
		TRACE("%s: reading DHT sensor failed at %lu, try again\n", __func__, millis());
#ifdef SENSOR_VCC_PIN
		digitalWrite(SENSOR_VCC_PIN, LOW);
		delay(20);
		digitalWrite(SENSOR_VCC_PIN, HIGH);
#endif /* SENSOR_VCC_PIN */
		data_next = millis() + 2000;
		ret = false;
	}
	ledOFF();
	return ret;
}
#endif /* USE_DHT */

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

	snprintf(web_server.buf, sizeof(web_server.buf), "\"%02d:%02d %02u.%02u.%04u\"", hour(t), minute(t), day(t), month(t), year(t));
	web_server.sendHeader("ETag", web_server.buf, true);

	snprintf(web_server.buf, sizeof(web_server.buf),
			"<!DOCTYPE html>"
			"<html>"
			"<head>"
			"<meta charset=\"utf-8\"/>"
			"<title>%s</title>"
			"</head>"
			"<body>"
			"<p>Version %s build %u</p>"
			"<h1>%s</h1>"
			"<p>Uhrzeit: %02d:%02d:%02d %02u.%02u.%04u Online seit: %02u:%02u:%02u</p>"
			"<p>Features:"
#ifdef WITH_UDP_LOG_PORT
			" UDPlog"
#endif
#ifdef WITH_LED
			" LED"
#endif
#ifdef WITH_SERIAL
			" Serial"
#endif
#ifdef WITH_OTA
			" OTA"
#endif
#ifdef USE_DHT
			" DHT"
#endif
#ifdef USE_DS18B20
			" DS18B20"
#endif
#ifdef WITH_ADC
			" ADC"
#endif
#ifdef WITH_IR_REC_PIN
			" IR-Receiver"
#endif
#ifdef WITH_IR_SEND_PIN
			" IR-Sender"
#endif
#ifdef WITH_RF_REC_PIN
			" RF-Receiver"
#endif
#ifdef WITH_RF_SEND_PIN
			" RF-Sender"
#endif
			"</p>"
#ifdef TEMPERATURE_SENSOR_PIN
			"<p>%.1f&deg;C"
#ifdef USE_DHT
			"bei %.1f%% Luftfeuchtigkeit"
#endif /* USE_DHT */
			"</p>\n"
#endif /* TEMPERATURE_SENSOR_PIN */
			"</body>"
			"</html>",
		sensor_id, VERSION, BUILD, sensor_id, hour(t), minute(t), second(t), day(t), month(t), year(t), hr, min % 60, sec % 60
#ifdef TEMPERATURE_SENSOR_PIN
			,temperature[0]
#ifdef USE_DHT
			,humidity[0]
#endif /* USE_DHT */
#endif /* TEMPERATURE_SENSOR_PIN */
			);
	web_server.send ( 200, "text/html", web_server.buf );
	TRACE("%s: %s\n", __func__, (doTrace > 1) ? web_server.buf : "");
	ledOFF();
}

#ifdef TEMPERATURE_SENSOR_PIN
static void onTemperature()
{
	ledON();
	unsigned len = 0;
	for(int i = 0; i < sensors_max; i++)
	{
		len = snprintf(web_server.buf+len, sizeof(web_server.buf)-len, "%.1f\n", temperature[i]);
	}
	web_server.send(200, "text/plain", web_server.buf);
	TRACE_LINE(web_server.buf);
	ledOFF();
}

#ifdef USE_DHT
static void onHumidity()
{
	ledON();
	snprintf(web_server.buf, sizeof(web_server.buf), "%d.%d\n", data[0][data_index].hum / 10, data[0][data_index].hum % 10);
	web_server.send( 200, "text/plain", web_server.buf);
	TRACE_LINE(web_server.buf);
	ledOFF();
}
#endif /* USE_DHT */

static void onSensor()
{
	ledON();
	String arg = web_server.arg("n");
	unsigned sensor = 0;
	if (arg.c_str()[0]) { sensor = arg.c_str()[0] - '1'; }
	if (sensor >= sensors_max)
	{
		snprintf(web_server.buf, sizeof(web_server.buf), "<html>illegal sensor %u allowed %u</html>", sensor+1, sensors_max);
		web_server.send( 200, "text/html", web_server.buf);
	} else {
		snprintf(web_server.buf, sizeof(web_server.buf), "<html>Uptime:%lu Temperatur: %.1f&deg;C"
#ifdef USE_DHT
				" Luftfeuchtikeit: %.1f%%"
#endif /* USE_DHT */
				"</html>\n",
				millis() / 1000,
				temperature[sensor]
#ifdef USE_DHT
				,humidity[sensor]
#endif /* USE_DHT */
				);
		web_server.send( 200, "text/html", web_server.buf);
	}
	TRACE_LINE(web_server.buf);
	ledOFF();
}
#endif /* TEMPERATURE_SENSOR_PIN */

#ifdef WITH_IR_SEND_PIN
static void onNEC()
{
	ledON();
	String arg = web_server.arg("code");
	unsigned code = 0;
	if (arg.c_str()[0] && (1 ==sscanf(arg.c_str(), "%x", &code)) && code)
	{
		TRACE("%s code:%08X\n", __func__, code);
		irsend.sendNEC(code, 32);
		irsend.sendNEC(code, 32);
		irsend.sendNEC(code, 32);
		web_server.send( 200, "text/html", "ok");
	} else {
		TRACE("%s code: '%s' illegal argument\n", __func__, arg.c_str());
		web_server.send( 200, "text/html", "illegal argument");
	}
	ledOFF();
}

static void onPAN()
{
	ledON();
	String args = web_server.arg("code");
	unsigned code = 0, addr = 0;
	if (args.c_str()[0] && (2 ==sscanf(args.c_str(), "%x,%x", &addr, &code)) && addr && code)
	{
		TRACE("%s addr:%04X code:%08X\n", __func__, addr, code);
		irsend.sendPanasonic(addr, code, kPanasonicBits, 3);
		web_server.send( 200, "text/html", "ok");
	} else {
		TRACE("%s code: '%s' illegal argument\n", __func__, args.c_str());
		web_server.send( 200, "text/html", "illegal argument");
	}
	ledOFF();
}
#endif /* WITH_IR_SEND_PIN */

#ifdef WITH_RF_SEND_PIN
static void onRF()
{
	ledON();
	String arg = web_server.arg("code");
	unsigned code = 0;
	if (arg.c_str()[0] && (1 ==sscanf(arg.c_str(), "%u", &code)) && code)
	{
		TRACE("%s code:%u\n", __func__, code);
		mySwitch.send(code, 24);
		web_server.send( 200, "text/html", "ok");
	} else {
		TRACE("%s code: '%s' illegal argument\n", __func__, arg.c_str());
		web_server.send( 200, "text/html", "illegal argument");
	}
	ledOFF();
}
#endif /* WITH_RF_SEND_PIN */

#ifdef WITH_LED
static void onLedON()
{
	ledON();
	web_server.send( 200, "text/html", "<html>LED ON</html>");
	TRACE("%s\n", __func__);
}

static void onLedOFF()
{
	ledOFF();
	web_server.send( 200, "text/html", "<html>LED OFF</html>");
	TRACE("%s\n", __func__);
}
#endif /* WITH_LED */

static void handleNotFound()
{
	ledON();
	unsigned len = snprintf(web_server.buf, sizeof(web_server.buf),
			"URI: '%s' not implemented\n"
			"Method: %s\n"
			"Arguments: %d\n",
			web_server.uri().c_str(), (( web_server.method() == HTTP_GET ) ? "GET" : "POST"), web_server.args());
	for (int i = 0; i < web_server.args(); i++ ) 
	{
		len += snprintf(web_server.buf+len, sizeof(web_server.buf) - len, " %s: %s\n", web_server.argName(i).c_str(), web_server.arg(i).c_str());
	}
	web_server.send ( 404, "text/plain", web_server.buf);
	TRACE_LINE(web_server.buf);
	ledOFF();
}

/*-------- NTP code ----------*/


// send an NTP request to the time server at the given address
static void sendNTPpacket(WiFiUDP &Udp, byte* packetBuffer)
{
#define NTP_PACKET_SIZE 48 // NTP time is in the first 48 bytes of message
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
	if (!Udp.beginPacket("fritz.box", 123) ||
			(NTP_PACKET_SIZE != Udp.write(packetBuffer, NTP_PACKET_SIZE)) ||
			!Udp.endPacket()
	   )
	{
		TRACE("%s failed\n", __func__);
	}
}

static time_t getNtpTime()
{
	byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets
	WiFiUDP Udp;
	Udp.begin(8888);
	TRACE("waiting for time sync, port %u\n", Udp.localPort());
	while (Udp.parsePacket() > 0) ; // discard any previously received packets
	TRACE_LINE("Transmit NTP Request\n");
	sendNTPpacket(Udp, packetBuffer);
	uint32_t beginWait = millis();
	while (millis() - beginWait < 1500)
	{
		int size = Udp.read(packetBuffer, NTP_PACKET_SIZE);
		if (size >= NTP_PACKET_SIZE)
		{
			TRACE("Receive NTP Response size %u\n", size);
			unsigned long secsSince1900;
			// convert four bytes starting at location 40 to a long integer
			secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
			secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
			secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
			secsSince1900 |= (unsigned long)packetBuffer[43];
			return secsSince1900 - 2208988800UL;
		}
		yield();
	}
	TRACE_LINE("No NTP Response :-(\n");
	return 0; // return 0 if unable to get the time
}

#ifdef USE_DS18B20
//Setting the temperature sensor
static void setupDS18B20()
{
#ifdef SENSOR_VCC_PIN
	pinMode(SENSOR_VCC_PIN, OUTPUT);
	digitalWrite(SENSOR_VCC_PIN, HIGH);
#endif /* SENSOR_VCC_PIN */
	DS18B20.begin();
	uint8_t devs = DS18B20.getDeviceCount();
	numberOfDevices = DS18B20.getDS18Count();
	TRACE("%s: device count: %u/%u/%u parasite power is: %s\n", __func__, numberOfDevices, devs, sensors_max, (DS18B20.isParasitePowerMode() ? "ON" : "OFF"));
	if (numberOfDevices > sensors_max) { numberOfDevices = sensors_max; }

	data_next = millis() + 2000;
	DS18B20.setWaitForConversion(true);
	DS18B20.requestTemperatures();
	requested = true;

	// Loop through each device, print out address
	for(int i=0, d=0; d < devs; d++)
	{
		DeviceAddress addr;
		// Search the wire for address
		if (DS18B20.getAddress(addr, d))
		{
			if (DS18B20.validFamily(addr) && (i < sensors_max) && DS18B20.getAddress(devAddr[i], d))
			{
				temperature[i] = DS18B20.getTempC(addr);
				TRACE("%s: found device %d/%d with address: %02X %02X %02X %02X %02X %02X %02X %02X resolution: %d temperature: %f°C\n",
					   	__func__, i+1, d,
						addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
						DS18B20.getResolution(addr), temperature[i]);
				i++;
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
	//WiFi.persistent(false);
	if (WiFi.mode(WIFI_STA))
	{
		TRACE("%s: Connecting to %s", __func__, wifi_ssid);
#if 0
		IPAddress ip(192,168,178,38);
		IPAddress gw(192,168,178,1);
		IPAddress mask(255,255,255,0);
		WiFi.config(ip, gw, mask);
#endif
		WiFi.begin(wifi_ssid, wifi_password);

		if (WiFi.getAutoConnect()) { WiFi.setAutoConnect(false); }
		WiFi.setAutoReconnect(false);
	} else {
		TRACE("%s: WiFi no in station mode (FATAL)\n", __func__);
		ESP.reset();
	}
}


/** Framework setup entry point (after reset) */
void setup()
{
#ifdef WITH_LED
	pinMode(WITH_LED, OUTPUT);
#endif
	ledON();

#ifdef WITH_SERIAL
	Serial.begin(115200);
	Serial.setDebugOutput(true);
	delay(30);
	TRACE("%s serial connection online\n", __func__);
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

	TRACE("\n-------------------\n%s reset reason: %s flash: %s size: %u real %u speed %u freq %uMHz scetch %u free %u"
#ifdef WITH_ADC
			" Vcc: %d"
#endif /* WITH_ADC */
			"\n",
			__func__, ESP.getResetInfo().c_str(), FLASH_MAP[fmap],
			ESP.getFlashChipSize(), ESP.getFlashChipRealSize(), ESP.getFlashChipSpeed(), ESP.getCpuFreqMHz(),
			ESP.getSketchSize(), ESP.getFreeSketchSpace()
#ifdef WITH_ADC
			, ESP.getVcc()
#endif /* WITH_ADC */
		 );
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

#ifdef WITH_UDP_LOG_PORT
	//raspiUDP.begin(WITH_UDP_LOG_PORT);
#endif /* WITH_UDP_LOG_PORT */

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


#ifdef USE_DHT
#ifdef SENSOR_VCC_PIN
	pinMode(SENSOR_VCC_PIN, OUTPUT);
	digitalWrite(SENSOR_VCC_PIN, HIGH);
#endif /* SENSOR_VCC_PIN */
	dht.begin();
	data_next = millis() + 2000;
#endif /* USE_DHT */


#ifdef WITH_IR_REC_PIN
	ir_repeat = 0;
	irPanasonicCmd = 0;
#if DECODE_HASH
	// Ignore messages with less than minimum on or off pulses.
	irrecv.setUnknownThreshold(kMinUnknownSize);
#endif                  // DECODE_HASH
	IR_REC_ON();
#endif

#ifdef WITH_IR_SEND_PIN
	irsend.begin();
#endif /* WITH_IR_SEND_PIN */

#ifdef WITH_RF_TFA
	tfa.start(WITH_RF_TFA);
#endif /* WITH_RF_TFA */

#ifdef WITH_RF_REC_PIN
	TRACE("RF-receiver in ISR %i\n", digitalPinToInterrupt(WITH_RF_REC_PIN));
	mySwitch.enableReceive(digitalPinToInterrupt(WITH_RF_REC_PIN));
#endif /* WITH_RF_REC_PIN */
#ifdef WITH_RF_SEND_PIN
	mySwitch.enableTransmit(WITH_RF_SEND_PIN);
#endif /* WITH_RF_REC_PIN */

	setSyncProvider(getNtpTime);
	time_t t = now();
	TRACE("%s: %s version %s build %d %02d:%02d:%02d %02d.%02d.%04d\n",
			__func__, sensor_id, VERSION, BUILD, hour(t), minute(t), second(t), day(t), month(t), year(t));

	// Start the server
#ifdef WITH_LED
	web_server.on("/led1", onLedON);
	web_server.on("/led0", onLedOFF);
#endif /* WITH_LED */

	web_server.on("/", onRoot);
#ifdef TEMPERATURE_SENSOR_PIN
	web_server.on("/temperature", onTemperature);
#ifdef USE_DHT
	web_server.on("/humidity", onHumidity);
#endif /* USE_DHT */
	web_server.on("/sensor", onSensor);
#endif /* TEMPERATURE_SENSOR_PIN */
#ifdef WITH_IR_SEND_PIN
	web_server.on("/ir/nec", onNEC);
	web_server.on("/ir/pan", onPAN);
#endif /* WITH_IR_SEND_PIN */
#ifdef WITH_RF_SEND_PIN
	web_server.on("/rf", onRF);
#endif /* WITH_RF_SEND_PIN */
	web_server.on("/trace", []() { if (++doTrace == 3) { doTrace = 0; } snprintf(web_server.buf, sizeof(web_server.buf), "trace:%u", doTrace); web_server.send(200, "text/plain", web_server.buf); });
	web_server.onNotFound(handleNotFound);
	web_server.begin();
	TRACE("WEB Server started at %lu\n", millis());

	/** Start early because we have to wait 1.5sec before first reading */
	ledOFF();
}


/** Framework mainloop */
void loop()
{
	/** Check WiFi connection first */
	if (WiFi.status() != WL_CONNECTED)
	{
		if (!wifi_reconnect_timeout)
		{
			WiFi.begin(wifi_ssid, wifi_password);
			TRACE("%s: await reconnecting to %s\n", __func__, wifi_ssid);
			wifi_reconnect_timeout = millis() + 30000;
		} else if (wifi_reconnect_timeout < millis()) {
			TRACE("\n%s: no WLAN connection - REBOOT!!!\n", __func__);
			ESP.reset();
		}
	} else if (wifi_reconnect_timeout)
	{
		TRACE("\n%s: WiFi reconnected with %s\n", __func__, WiFi.localIP().toString().c_str());
		wifi_reconnect_timeout = 0;
		web_server.begin();
	}

#ifdef WITH_OTA
	if (!wifi_reconnect_timeout) { ArduinoOTA.handle(); yield(); }
#endif /* WITH_OTA */

#ifdef WITH_IR_REC_PIN
	decode_results results;  // Somewhere to store the results
	if (irrecv.decode(&results))
	{
		ledON();
		// Display a crude timestamp.
		if (results.overflow) { TRACE("WARNING: kCaptureBufferSize:%d to small\n", kCaptureBufferSize); }
		if (doTrace > 1)
		{
			TRACE_LINE(resultToHumanReadableBasic(&results));
			dumpACInfo(results);  // Display any extra A/C info if we have it.
			yield();  // Feed the WDT as the text output can take a while to print.
			// Output RAW timing info of the result.
			TRACE_LINE(resultToTimingInfo(&results));
			yield();  // Feed the WDT (again)
		}
		TRACE("IR code:%s %s%s addr:%04X cmd:%04X data:%08X\n",
				resultToHexidecimal(&results).c_str(), typeToString(results.decode_type, results.repeat).c_str(), (hasACState(results.decode_type) ? "(AC)" : ""),
				results.address, results.command, (unsigned)results.value);
		if (!ir_repeat && (0x4004 == results.address))
		{	
			switch (results.command)
			{
				case 0x01904FDE: irPanasonicCmd = results.command; ir_repeat = 5; break; // netflix
				case 0x01000E0F: if (0x01008E8F == irPanasonicCmd) { ir_repeat = 1; } irPanasonicCmd = results.command; break; // red key
				case 0x01008E8F: if (0x01000E0F == irPanasonicCmd) { ir_repeat = 1; } irPanasonicCmd = results.command; break; // green key
				default: break;
			}
		}
		ledOFF();
		yield();
	}
#endif

#ifdef WITH_RF_TFA
	if (tfa.isDataAvailable())
	{
		ledON();
		tfaResult result = tfa.getData();
		TRACE("id: %d, channel: %d, humidity: %d%%, temperature: %d.%d C, battery: %s\n",
				result.id, result.channel, result.humidity, result.temperature / 100, result.temperature % 100, (result.battery ? "OK" : "NOK"));
		ledOFF();
		yield();
	}
#endif /* WITH_RF_TFA */

#ifdef WITH_RF_REC_PIN
	if (mySwitch.available()) {
		ledON();
		int value = mySwitch.getReceivedValue();
		if (value == 0) {
			TRACE_LINE("Unknown encoding");
		} else {
			TRACE("Received %d / %u bit Protocol: %u\n", value, mySwitch.getReceivedBitlength(), mySwitch.getReceivedProtocol());
		}
		mySwitch.resetAvailable();
		ledOFF();
		yield();             // Feed the WDT (again)
	}
#endif /* WITH_RF_REC_PIN */

#if defined(WITH_IR_SEND_PIN) && defined(WITH_IR_REC_PIN)
	if (ir_repeat)
	{
		switch(irPanasonicCmd)
		{
			case 0x01000E0F: /* red */
				TRACE("light off\n");
				mySwitch.send(13982723, 24);
				mySwitch.send(13988867, 24);
				ir_repeat--;
				break;
			case 0x01008E8F: /* green */
				TRACE("light on\n");
				mySwitch.send(13982732, 24);
				mySwitch.send(13988876, 24);
				ir_repeat--;
				break;
			case 0x01904FDE:
				ledON();
				TRACE("netflix pressed, send TV and guide\n");
				//IR_REC_OFF();
				irsend.sendPanasonic(0x4004, 0x01400C4D);
				irsend.sendPanasonic(0x4004, 0x0190E170);
				//IR_REC_ON();
				ledOFF();
				yield();             // Feed the WDT (again)
				ir_repeat--;
				break;
			default:
				if (irPanasonicCmd) {
					TRACE("Panasonic: %08X repeat:%u\n", irPanasonicCmd, ir_repeat);
					ir_repeat = 0;
				}
				break;
		}
	}
#endif /* WITH_IR_ */

#ifdef TEMPERATURE_SENSOR_PIN
	if (!wifi_reconnect_timeout && (millis() >= data_next))
	{
		if (sensorRead())
		{
			data_next = millis() + interval_sec * 1000;
			data_next -= data_next % (interval_sec * 1000);
			TRACE("next sensor read at %lu\n", data_next);
			yield();
		}

	}
	if (client_timeout)
	{
		if (temperature_client.connected())
		{  // await the data answer
			while (temperature_client.available()) { char c = temperature_client.read(); TRACE("%c", c); client_error = 0; }
			if (client_timeout < millis())
			{
				client_timeout = 0;
				client_error++;
				temperature_client.stop();
				TRACE("%s client connection closed (timeout at %lu)\n", __func__, millis());
			}
		} else {
			TRACE("%s client connection finished at %lu\n", __func__, millis());
			client_timeout = 0;
			temperature_client.stop();
		}
		yield();
	}
#endif /* TEMPERATURE_SENSOR_PIN */

	if (!wifi_reconnect_timeout)
	{
		web_server.handleClient();
		yield();
	} else {
		web_server.stop();
	}
}


#if 0
#include <Arduino.h>
/*
  Sketch zur Vorab-Analyse unbekannter 433-MHZ-Wettersensoren
  und Fernbedienungen von 433MHz-Funksteckdosen
  Inspiriert durch Beiträge im Arduino-Forum:
  http://arduino.cc/forum/index.php/topic,119739.0.html
  http://arduino.cc/forum/index.php/topic,136836.0.html
  
  Hardware: 
  1. Arduino-Board mit 433 MHz Regenerativempfänger für ASK/OOK,
  angeschlossen an einem interruptfähigen Pin.
  2. Funksensor entweder eines 433 MHz Funkthermometers 
  oder Funk-Wetterstation oder Steckdosen-Funkfernbedienung
  
  Analysiert werden können Sensoren mit folgendem Funkprotokoll:
  - extra langes Startbit (extra langer LOW Pulse am Receiver)
  - langes 1-Datenbit  (langer LOW Pulse am Receiver)
  - kurzes 0-Datenbit  (kurzer LOW Pulse am Receiver)
  - sehr kurze Trenn-Impulse zwischen den Datenbits (sehr kurze HIGH-Impulse am Receiver)
  - 20 bis 50 Datenbits pro Datenpaket
  Diese Art Funkprotokoll trifft auf die meisten billigen 433 MHZ
  Funkthermometer, Funk-Wetterstationen und Funk-Steckdosen zu.
  
  Ausgabe ueber den seriellen Monitor 
  Je erkanntem Datenpaket am Receiver wird ausgegeben:
  - Länge des Startbits (Mikrosekunden LOW) und des nachfolgenden HIGH-Impulses
  - Anzahl der erkannten Datenbits im Datenpaket
  - Länge aller erkannten Datenbits (Mikrosekunden LOW)
  - Länge der zwischen den Datenbits erkannten Pausen (Mikrosekunden HIGH)
  - die als 0/1-Bitstrom decodierten Datenbits des Datenpakets
  
  Nur Vorab-Analyse des Timings im Funkprotokoll!
  In einem weiteren Schritt muss dann die Bedeutung der Bits
  und die Umsetzung in Messwerte erst noch detalliert decodiert werden,
  dieser Sketch erkennt nur das Timing und die Groesse der Datenpakete!
*/

// connect data pin of rx433 module to a pin that can handle hardware interrupts
// with an Arduino UNO this is digital I/O pin 2 or 3 only
#define RX433DATAPIN D3

// hardware interrupt connected to the pin
// with Arduino UNO interrupt-0 belongs to pin-2, interrupt-1 to pin-3
#define RX433INTERRUPT 0

// Set speed of serial in Arduino IDE to the following value
#define SERIALSPEED 115200

// Now make some suggestions about pulse lengths that may be detected
// minimum duration (microseconds) of the start pulse
#define MINSTARTPULSE 4500

// minimum duration (microseconds) of a short bit pulse
#define MINBITPULSE 450

// minimum duration (microseconds) of a HIGH pulse between valid bits
#define MINHITIME 50

// variance between pulses that should have the same duration
#define PULSEVARIANCE 250

// minimum count of data bit pulses following the start pulse
#define MINPULSECOUNT 20

// maximum count of data bit pulses following the start pulse
#define MAXPULSECOUNT 50

// buffer sizes for buffering pulses in the interrupt handler
#define PBSIZE 216

void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Start!");
  pinMode(RX433DATAPIN, INPUT);
  attachInterrupt(RX433INTERRUPT, rx433Handler, CHANGE);
}

volatile unsigned int pulsbuf[PBSIZE]; // ring buffer storing LOW pulse lengths
volatile unsigned int hibuf[PBSIZE]; // ring buffer storing HIGH pulse lengths
unsigned int validpulsbuf[MAXPULSECOUNT]; // linear buffer storing valid LOW pulses
unsigned int validhibuf[MAXPULSECOUNT];  // linear buffer storing valid HIGH pulses

volatile byte pbread,pbwrite;  // read and write index into ring buffer

void rx433Handler()
{
  static long rx433LineUp, rx433LineDown;
  long LowVal, HighVal;
  int rx433State = digitalRead(RX433DATAPIN); // current pin state
  if (rx433State) // pin is now HIGH
  {
    rx433LineUp=micros(); // line went HIGH after being LOW at this time
    LowVal=rx433LineUp - rx433LineDown; // calculate the LOW pulse time
    if (LowVal>MINBITPULSE)
    { // store pulse in ring buffer only if duration is longer than MINBITPULSE
      // To be able to store startpulses of more than Maxint duration, we dont't store the actual time,
     // but we store  MINSTARTPULSE+LowVal/10, be sure to calculate back showing the startpulse length!
      if (LowVal>MINSTARTPULSE) LowVal=MINSTARTPULSE+LowVal/10; // we will store this as unsigned int, so do range checking

      pulsbuf[pbwrite]=LowVal; // store the LOW pulse length
      pbwrite++;  // advance write pointer in ringbuffer
      if (pbwrite>=PBSIZE) pbwrite=0; // ring buffer is at its end
    }  
  }
  else 
  {
    rx433LineDown=micros(); // line went LOW after being HIGH
    HighVal=rx433LineDown - rx433LineUp; // calculate the HIGH pulse time
    if (HighVal>31999) HighVal=31999; // we will store this as unsigned int
    hibuf[pbwrite]=HighVal; // store the HIGH pulse length
  }
}


boolean counting;
byte i,counter;
int startBitDurationL,startBitDurationH,shortBitDuration,longBitDuration;

void showBuffer()
// this function will show the results on the serial monitor
// output will be shown if more bits than MINPULSECOUNT have been collected
{
  long sum;
  int avg;
  sum=0;
  if (counter>=MINPULSECOUNT)
  { // only show buffer contents if it has enough bits in it
    Serial.println();
    Serial.print("Start Bit L: "); Serial.print((startBitDurationL-MINSTARTPULSE)*10L);
    Serial.print("   H: ");Serial.println(startBitDurationH);
    Serial.print("Data Bits: ");Serial.println(counter);
    Serial.print("L: ");
    for (i=0;i<counter;i++)
    {
      Serial.print(validpulsbuf[i]);Serial.print(" ");
      sum+=validpulsbuf[i];
    }
    Serial.println();

    Serial.print("H: ");
    for (i=0;i<counter;i++)
    {
      Serial.print(validhibuf[i]);Serial.print(" ");
    }
    Serial.println();

    avg=sum/counter; // calculate the average pulse length
    // then assume that 0-bits are shorter than avg, 1-bits are longer than avg
    for (i=0;i<counter;i++)
    {
      if (validpulsbuf[i]<avg) Serial.print('0'); else Serial.print('1');
    }
    Serial.println();
  
  }
  counting=false;
  counter=0;
}

void loop() 
{
  long lowtime, hitime;
  if (pbread!=pbwrite) // check for data in ring buffer
  {
    lowtime=pulsbuf[pbread]; // read data from ring buffer
    hitime=hibuf[pbread];
    cli(); // Interrupts off while changing the read pointer for the ringbuffer
    pbread++;
    if (pbread>=PBSIZE) pbread=0;
    sei(); // Interrupts on again
    if (lowtime>MINSTARTPULSE) // we found a valid startbit!
    {
      if (counting) showBuffer(); // new buffer starts while old is still counting, show it first      
      startBitDurationL=lowtime;
      startBitDurationH=hitime;
      counting=true;     // then start collecting bits
      counter=0;         // no data bits yet
    }
    else if (counting && (counter==0)) // we now see the first data bit
    { // this may be a 0-bit or a 1-bit, so make some assumption about max/min lengths of data bits that will follow
      shortBitDuration=lowtime/2;
      if (shortBitDuration<MINBITPULSE+PULSEVARIANCE)
        shortBitDuration=MINBITPULSE;
      else  
        shortBitDuration-=PULSEVARIANCE;
      longBitDuration=lowtime*2+PULSEVARIANCE;
      validpulsbuf[counter]=lowtime;
      validhibuf[counter]=hitime;
      counter++;
    }
    else if (counting&&(lowtime>shortBitDuration)&&(lowtime<longBitDuration))
    {
      validpulsbuf[counter]=lowtime;
      validhibuf[counter]=hitime;
      counter++;
      if ((counter==MAXPULSECOUNT) || (hitime<MINHITIME))
      {
        showBuffer();
      }  
    }
    else // Low Pulse is too short
    {
      if (counting) showBuffer();
      counting=false;
      counter=0;
    }  
  }
}

#endif

