/* vim:set ts=4 sw=4 fileformat=unix fileencodings=utf-8 fileencoding=utf-8 noexpandtab:  */

/**\file
 * \brief  Weather sensor (power save variant)
 * \author Ralf Schröder
 *
 * (C) Feel free to change (MIT)
 *
 */


/*** Personal parameter adaptations ***/

#undef WITH_LED     /** activate the onboard LED for testing, use the given LED */
#define WITH_SERIAL   /** activate the serial printings for testing */
#undef LIGHT_SLEEP    /**< no deep sleep if defined, attend the link XPD_DCDC->RST for deep sleep */
#define VERSION "0.9" /**< Version */ 
#define BUILD 1       /**< Build number */ 

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
static const char* sensor_id = "Ralfs Testsensor"; //< ID for data base separation, could be the sensor location name */

static const char* weather_server = "raspi.fritz.box";
static const int weather_port = 8000;


#include <ESP8266WiFi.h>
#include <DHT.h>

extern "C" {
#include "user_interface.h"
}

#ifdef WITH_LED
#define ledON()  digitalWrite(WITH_LED, 0)
#define ledOFF() digitalWrite(WITH_LED, 1)
#else
#define ledON()
#define ledOFF()
#endif

#ifdef WITH_SERIAL
#define TRACE_LINE(line) Serial.print(line)
#define TRACE(...)       Serial.printf(__VA_ARGS__)
#else
#define TRACE_LINE(line)
#define TRACE(args...)
#endif

static unsigned long reconnect_timeout;  /**< Reconnect timeout within loop() for lost WiFi connections (millis() value) */

static WiFiClient client;                /**< client for data transmission */
/** If set, a data message was sent, until the set value the response should be received (millis() value). After that time hangup. */
static unsigned long client_timeout;

static DHT dht(2, DHT22);               /**< Sensor object */
static unsigned long data_next;          /**< time to read the sensor (millis value) */



/** Deep sleep methon. I assume here, that the function continues after the sleep.
 *  That's not true for reset based variants, but other light sleep varaint can be test so, too.
 */
static void deep_sleep()
{
	unsigned long now = millis();
	unsigned long wait = data_next - now;
	if (wait > 60000) { wait = 60000; }
	TRACE("sleeping %lu msec at: %lu\n", wait, now);
#ifdef LIGHT_SLEEP
	delay(wait);
#else
#ifdef WITH_SERIAL
	Serial.flush();
	Serial.end();
	TRACE("wakeup: %lu\n", millis());
#endif
	delay(100);
	ESP.deepSleep(wait*1000);
	delay(200);
	return;
#endif
}


/** Transmit a message to a server */
static void transmit_msg(float temperature, float humidity)
{
	if (client.connect(weather_server, weather_port))
	{
		char buf[400];
		unsigned jlen = snprintf(buf, sizeof(buf), "{\"sender_id\":\"%s\",\"password\":\"" DB_SECRET "\",\"temperature\":%.1f,\"humidity\":%.1f}\r\n", sensor_id, temperature, humidity);
		char* header = buf + jlen + 1;
		snprintf(header, sizeof(buf)-jlen-1,
				"POST /esp8266_trigger HTTP/1.1\r\n"
				"Host: %s:%u\r\n"
				"Connection: close\r\n"
				"Content-Type: application/json; charset=utf-8\r\n"
				"Content-Length: %u\r\n\r\n", weather_server, weather_port, jlen);
		TRACE_LINE(header);
		TRACE_LINE(buf);
		client.print(header); client.print(buf);
		client_timeout = millis() + 5000;   // expect answer finished after 5 sec
	} else {
		TRACE("%s: no http server connection\n", __func__);
	}
}


/** Sensor reading and data transmission */
static void sensorRead()
{
	float t, h;
	ledON();
	for (int count = 0; count < 5; count++)
	{
		t = dht.readTemperature();
		h = dht.readHumidity();
		if (!isnan(t) && !isnan(h))
		{
			transmit_msg(t, h);
			break;
		}
		TRACE("%s: reading sensor failed, try again\n", __func__);
		delay(2500);
	}
	TRACE("Temperatur: %.1f°C Luftfeuchtikeit: %.1f%%\n", t, h);
	ledOFF();
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
	delay(10);
	TRACE("%s serial connection online\n", __func__);
	const rst_info * resetInfo = system_get_rst_info();
	const char * const RST_REASONS[] = {
		"REASON_DEFAULT_RST",
		"REASON_WDT_RST",
		"REASON_EXCEPTION_RST",
		"REASON_SOFT_WDT_RST",
		"REASON_SOFT_RESTART",
		"REASON_DEEP_SLEEP_AWAKE",
		"REASON_EXT_SYS_RST"
	};
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

	TRACE("%s reset reason: %s flash: %s\n", __func__, RST_REASONS[resetInfo->reason], FLASH_MAP[fmap]);
#endif

	// Connect to WiFi network
	if (WiFi.mode (WIFI_STA))
	{
		TRACE("%s: Connecting to %s", __func__, ssid);
		WiFi.begin(ssid, password);
#if 0  // freezes after the first loop, investigate later
		if (!WiFi.setSleepMode(WIFI_LIGHT_SLEEP))
		{
			TRACE("%s: unable to set WIFI_LIGHT_SLEEP (WARNING)\n", __func__);
		}
#endif
		for(int count = 0; count < 100; count++)
		{
			if (WiFi.status() == WL_CONNECTED) { break; }
			delay(100);
			TRACE(".");
		}
		TRACE("\n%s: WiFi connected with %s\n", __func__, WiFi.localIP().toString().c_str());
	} else {
		TRACE("%s: WiFi no in station mode (FATAL)", __func__);
	}

	/** Start early because we have to wait 1.5sec before first reading */
	dht.begin();
	data_next = millis() + 2500;

	ledOFF();
}


/** Framework mainloop */
void loop()
{
	/** Check WiFi connection first */
	if (WiFi.status() != WL_CONNECTED)
	{
		if (!reconnect_timeout)
		{
			TRACE("%s: await reconnecting to %s", __func__, ssid);
			reconnect_timeout = millis() + 5000;
			TRACE(".");
			delay(100);
		} else if (reconnect_timeout < millis()) {
			TRACE("\n%s: no WLAN connection\n", __func__);
			reconnect_timeout = 0;
			data_next += interval_sec;
			deep_sleep(); // give up, wait one data interval
		} else {
			TRACE(".");
			delay(100);
		}
		return;
	}
	if (reconnect_timeout)
	{
		TRACE("\n%s: WiFi reconnected with %s\n", __func__, WiFi.localIP().toString().c_str());
		reconnect_timeout = 0;
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
			while (client.available()) { TRACE("%c", client.read()); }
			if (client.connected())
			{
				delay(10);
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
			sensorRead();
			data_next += interval_sec * 1000;
			TRACE("%s: now:%lu next sensor read %lu\n", __func__, millis(), data_next);
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

