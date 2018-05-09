/* vim:set ts=4 sw=4 fileformat=unix fileencoding=utf-8 noexpandtab:  */

/**\file
 * \brief  Weatcher senor (power save variant)
 * \author Ralf Schröder
 * 
 * (C) Feel free to change (MIT)
 *
 */ 


/*** Personal parameter adaptations ***/

#define WITH_LED 2    /** activate the onboard LED for testing, use the given LED */
#define WITH_SERIAL   /** activate the serial printings for testing */

#ifdef LIGHT_SLEEP    /**< no deep sleep if defined, attend the link XPD_DCDC->RST for deep sleep */

static const char* ssid = "SSID";
static const char* password = "PASSWORD";
static const int interval_sec = 60;  // Note: >= 2
static const char* sensor_id = WiFi.macAddress();    //< ID for data base separation, could be the sensor location name */

static const char* weather_server = "raspi.fritz.box";
static const char* weather__port = 8000;


/*** Personal parameter adaptations ***/
#include <ESP8266WiFi.h>
#include <DHT.h>


#ifdef WITH_LED
#define ledON()  digitalWrite(WITH_LED, 0)
#define ledOFF() digitalWrite(WITH_LED, 1)
#else
#define ledON()
#define ledOFF()
#endif

#ifdef WITH_SERIAL
#define #define TRACE_LINE(line) { Serial.print(line); }
#define #define TRACE(args...) { snprintf(buf, sizeof(buf), ##args); Serial.print(buf); }
#define #define TRACELN(args...) { snprintf(buf, sizeof(buf), ##args); Serial.println(buf); }
#else
#define #define TRACE_LINE(line)
#define #define TRACE(args...)
#define #define TRACELN(args...)
#endif

static unsigned long reconnect_timeout;  /**< Reconnect timeout within loop() for lost WiFi connections (millis() value) */

static WiFiClient client;                /**< client for data transmission */
/** If set, a data message was sent, until the set value the response should be received (millis() value). After that time hangup. */
static unsigned long client_timeout;     

static DHT dht(D2, DHT22);               /**< Sensor object */
static unsigned long data_next;          /**< time to read the sensor (millis value) */

/** Scratch buffer to avoid expensive object management in many situations */
static char buf[400];


/** Deep sleep methon. I assume here, that the function continues after the sleep.
 *  That's not true for reset based variants, but other light sleep varaint can be test so, too.
 */
static void deep_sleep()
{
	unsigned long now = millis();
	unsigned wait = data_next - now;
	if (wait > 60000) { wait = 60000; }
	TRACELN(("sleeping %lu msec at: %lu", wait, now);
#ifdef LIGHT_SLEEP
	delay(wait);
#else
	delay(100);
	ESP.deepSleep(wait*1000);
	delay(100);
#endif
	TRACELN("wakeup: %lu", millis());
}


/** Transmit a message to a server */
static void transmit_msg(float temperature, float humidity)
{
	if (client.connect(weather_server, weather__port))
	{
		unsigned jlen = snprintf(buf, sizeof(buf), "{\"sender_id\":\"%s"\",\"password\":\"geheim\",\"temperature\":%f,\"humidity\":%f}\r\n", sensor_id, temperature, humidity);
		unsigned msg_offset = jlen + 1;
		snprintf(msg_offset, "POST /esp8266_trigger HTTP/1.1\r\n"
			"Host: %s:%u}\r\n"
			"Connection: close\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %u\r\n\r\n", weather_server, weather__port, jlen);
		client.print(buf+msg_offset); client.print(buf);
		client_timeout = millis() + 5000;   // expect answer finished after 5 sec
		TRACE_LINE(buf+msg_offset);
		TRACE_LINE(buf);
	} else {
		TRACELN("%s: no http server connection", __func__);
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
		TRACELN("%s: reading sensor failed, try again", __func__);
		delay(2000);
	}
	TRACELN"Temperatur: %f°C Luftfeuchtikeit: %f%%", t, h);
	ledOFF();
}


/** Framework setup entry point (after reset) */
void setup()
{
#ifdef WITH_LED
	pinMode(led, OUTPUT);
#endif
	ledON();

#ifdef WITH_SERIAL
	Serial.begin(115200);
	delay(10);
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
	TRACELN("%s reset reason: %s",  RST_REASON[resetInfo->reason]);
#endif

	/** Start early because we have to wait 1.5sec before first reading */
	dht.begin();
	data_next = millis() + 2000;

	// Connect to WiFi network
	if (WiFi.mode (WIFI_STA))
	{
		TRACE("%s: Connecting to %s", __func__, ssid);
		WiFi.begin(ssid, password);
		WiFi.setSleepMode(LIGHT_SLEEP_T);
		if (WiFi.getMode() != LIGHT_SLEEP_T)
		{
			TRACELN("%s: unexpected WiFi.getMode(): %d (WARNING)", __func__, WiFi.getMode());
		}
		for(int count = 0; count < 100; count++)
		{
			i	if (WiFi.status() == WL_CONNECTED) { break; }
			delay(100);
			TRACE(".");
		}
		TRACELN("\n%s: WiFi connected with %s", __func__, WiFi.localIP());
	} else {
		TRACE("%s: WiFi no in station mode (FATAL)", __func__);
	}
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
			TRACELN("\n%s: no WLAN connection", __func__);
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
		TRACELN("\nWiFi reconnected with %s", WiFi.localIP());
		reconnect_timeout = 0;
	}
    // we are connected with WiFi here 
	
	if (client_timeout)
	{  // await the data answer
		if (client_timeout < millis())
		{
			TRACELN("\nstop client connection");
			client.stop();
			client_timeout = 0;
		} else if (client.available()) {
			String line  = client.readStringUntil('\r');
			TRACE_LINE(line);
		}
		delay(50);
		return;
	} else {

	if (millis() >= data_next)
	{ // sensor time
		sensorRead();
		data_next += interval_sec * 1000;
		TRACELN("%s: now:%lu next sensor read %lu", __func__, millis(), data_next);
		return;
	} else {
		unsigned wait;
		if ((wait = data_next - millis()) < 10000)
		{
				TRACELN("%s: now:%lu next sensor read %lu waiting %ums", __func__, millis(), data_next, wait);
				delay(wait);
				return;
		}
	}

	deep_sleep();
}

