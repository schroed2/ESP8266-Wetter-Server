/* vim:set ts=4 sw=4 fileformat=unix fileencodings=utf-8 fileencoding=utf-8 noexpandtab:  */

/**\file
 * \brief  Weather sensor (power save variant)
 * \author Ralf Schröder
 *
 * (C) Feel free to change (MIT)
 *
 */


/*** Personal parameter adaptations ***/

#define WITH_LED 1    /** activate the onboard LED for testing, use the given LED */
//#undef WITH_LED    /** no onboard LED for testing */
#undef WITH_SERIAL   /** activate the serial printings for testing */
#undef LIGHT_SLEEP   /**< no deep sleep if defined, attend the link XPD_DCDC->RST for deep sleep */
#define VERSION "0.9" /**< Version */ 
#define BUILD 3       /**< Build number */ 
#define LOCATION "Testsensor"
//#define DHT_PIN D2
//#define DHT_VCC D0
#define DHT_PIN 0
#define DHT_VCC 2
ADC_MODE(ADC_VCC);    /**< vcc read */

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
static const int interval_sec = 60;  // Note: >= 2
static const char* sensor_id = "Ralfs " LOCATION; //< ID for data base separation, could be the sensor location name */

static const char* weather_server = "raspi.fritz.box";
static const int weather_port = 8000;


#include <ESP8266WiFi.h>
#include <DHT.h>

#ifdef WITH_SERIAL
extern "C" {
#include "user_interface.h"
}
#endif /* WITH_SERIAL */

#ifdef WITH_LED
#define ledON()  digitalWrite(WITH_LED, 0)
#define ledOFF() digitalWrite(WITH_LED, 1)
#else
#define ledON()
#define ledOFF()
#endif /* WITH_LED */

#ifdef WITH_SERIAL
#define TRACE_LINE(line) Serial.print(line)
#define TRACE(...)       Serial.printf(__VA_ARGS__)
#else
#define TRACE_LINE(line)
#define TRACE(...)
#endif

static unsigned long reconnect_timeout;  /**< Reconnect timeout within loop() for lost WiFi connections (millis() value) */

static WiFiClient client;                /**< client for data transmission */
/** If set, a data message was sent, until the set value the response should be received (millis() value). After that time hangup. */
static unsigned long client_timeout;

static DHT dht(DHT_PIN, DHT22);               /**< Sensor object */
static unsigned long data_next;          /**< time to read the sensor (millis value) */



/** Deep sleep methon. I assume here, that the function continues after the sleep.
 *  That's not true for reset based variants, but other light sleep varaint can be test so, too.
 */
static void deep_sleep()
{
	unsigned long now = millis();
	unsigned long wait = data_next - now;
#ifdef LIGHT_SLEEP
	if (wait > 60000) { wait = 60000; }
	TRACE("sleeping %lu msec at: %lu\n", wait, now);
	delay(wait);
#else
	if (wait > interval_sec * 1000) { wait = interval_sec * 1000; }
	pinMode(DHT_VCC, INPUT);
#ifdef WITH_SERIAL
	TRACE("%s %lu msec at: %lu\n", __func__, wait, now);
	Serial.flush();
#endif
	delay(100);
	ESP.deepSleep(wait*1000);
	/* point never reached */
	delay(1000);
#endif
}


/** Transmit a message to a server (force connect/closing) */
static void transmit_msg(float temperature, float humidity)
{
	if (client.connect(weather_server, weather_port))
	{
		char buf[600];
		unsigned jlen = snprintf(buf, sizeof(buf),
				"{\"sender_id\":\"%s\",\"password\":\"" DB_SECRET "\",\"temperature\":%.1f,"
				"\"humidity\":%.1f,\"vcc\":%.2f,\"millis\":%lu,\"rssi\":%d }\r\n",
				sensor_id, temperature, humidity, ESP.getVcc() / 1000.0, millis(), WiFi.RSSI());
		char* header = buf + jlen + 1;
		unsigned hlen = snprintf(header, sizeof(buf)-jlen-1,
				"POST /esp8266_trigger HTTP/1.1\r\n"
				"Host: %s:%u\r\n"
				"Connection: close\r\n"
				"Content-Type: application/json; charset=utf-8\r\n"
				"Content-Length: %u\r\n\r\n", weather_server, weather_port, jlen);
		TRACE_LINE(header);
		TRACE_LINE(buf);
		client.write(header, hlen); client.write(buf, jlen);
		client_timeout = millis() + 5000;   // expect answer finished after 5 sec
	} else {
		TRACE("%s: no http server connection\n", __func__);
	}
}


/** Sensor reading and data transmission */
static bool sensorRead()
{
	bool ret;
	ledON();
	float t = dht.readTemperature();
	float h = dht.readHumidity();
	if (!isnan(t) && !isnan(h))
	{
		transmit_msg(t, h);
		TRACE("Temperatur: %.1f°C Luftfeuchtikeit: %.1f%% RSSI:%d\n", t, h, WiFi.RSSI());
		ret = true;
	} else {
		digitalWrite(DHT_VCC, LOW);
		TRACE("%s: reading sensor failed, try again\n", __func__);
		delay(100);
		digitalWrite(DHT_VCC, HIGH);
		ret = false;
	}
	ledOFF();
	return ret;
}

static int wifi_trace(int status)
{
#ifdef WITH_SERIAL
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
#endif
	return status;
}


/** Framework setup entry point (after reset) */
void setup()
{
#ifdef WITH_LED
	pinMode(WITH_LED, OUTPUT);
#endif /* WITH_LED */
	ledON();

#ifdef WITH_SERIAL
	Serial.begin(115200);
	delay(20);
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

	TRACE("%s reset reason: %s flash: %s size: %u real %u speed %u freq %uMHz scetch %u free %u Vcc: %d\n",
			__func__, ESP.getResetInfo().c_str(), FLASH_MAP[fmap],
		 ESP.getFlashChipSize(), ESP.getFlashChipRealSize(), ESP.getFlashChipSpeed(), ESP.getCpuFreqMHz(),
		 ESP.getSketchSize(), ESP.getFreeSketchSpace(), ESP.getVcc());
#endif /* WITH_SERIAL */

	/** Start early because we have to wait 1.5sec before first reading */
	pinMode(DHT_VCC, OUTPUT);
	digitalWrite(DHT_VCC, LOW);

	// Connect to WiFi network
#ifdef WITH_SERIAL
	WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event) 
			{ TRACE("WiFI connected on %s channel%u, IP: %s\n", WiFi.SSID().c_str(), WiFi.channel(), WiFi.localIP().toString().c_str()); }
		);

	WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event)
			{ TRACE("Station disconnected from %s, reason %d\n", event.ssid.c_str(), event.reason ); }
		);
#endif /* WITH_SERIAL */


	if (WL_NO_SSID_AVAIL == WiFi.begin())
	{
		TRACE("Connecting to %s ", ssid);
		WiFi.begin(ssid, password);
	} else {
		TRACE("Connecting to persistent %s ", WiFi.SSID().c_str());
	}

	if (!WiFi.getAutoConnect()) { WiFi.setAutoConnect(true); }
	WiFi.setAutoReconnect(true);

	for(int count = 0; count < 100; count++)
	{
			if (WiFi.status() == WL_CONNECTED) { break; }
			delay(100);
			TRACE(".");
	}
	wifi_trace(WiFi.status());

	ledOFF();
	digitalWrite(DHT_VCC, HIGH);
	dht.begin();
	data_next = millis() + 1200;
}


/** Framework mainloop */
void loop()
{
	/** Check WiFi connection first */
	if (WiFi.status() != WL_CONNECTED)
	{
		wifi_trace(WiFi.status());
		deep_sleep(); // give up, wait one data interval
		return;
	}

	// we are connected with WiFi here
	if (client_timeout && client.connected())
	{  // await the data answer
		if (client_timeout < millis())
		{
			TRACE_LINE("\nWiFi: stop client connection");
			client.stop();
			client_timeout = 0;
		} else {
			while (client.available()) { char c = client.read(); TRACE("%c", c); }
			if (client.connected())
			{
				yield();
			} else {
				TRACE_LINE("\nWiFi: client connection finished\n");
				client_timeout = 0;
				client.stop();
			}
		}
		return;
	} else {
		if (millis() >= data_next)
		{ // sensor time
			if (sensorRead())
			{
				data_next += interval_sec * 1000;
#ifndef LIGHT_SLEEP
				data_next -= millis();
#endif
				TRACE("%s: now:%lu next sensor read %lu\n", __func__, millis(), data_next);
			}
#ifdef LIGHT_SLEEP
			else { data_next = millis() + 4000; }
#else
			else if (millis() < 20000) {
				data_next = millis() + 4000;
			} else {
				data_next = interval_sec * 1000;
#ifndef LIGHT_SLEEP
				data_next -= millis();
#endif
				TRACE("%s: sensor timeout now:%lu next sensor read %lu\n", __func__, millis(), data_next);
			}
#endif /* LIGHT_SLEEP */
			return;
		} else {
			unsigned wait;
			if ((wait = data_next - millis()) < 10000)
			{
				TRACE("%s: now:%lu next sensor read %lu waiting %ums\n", __func__, millis(), data_next, wait);
				delay(wait);
				return;
			}
		}
	}
	deep_sleep();
}

