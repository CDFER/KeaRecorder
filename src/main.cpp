#include <Arduino.h>

// #include <SPI.h>
#include "USB.h"
#if ARDUINO_USB_CDC_ON_BOOT
#define HWSerial Serial0
#define USBSerial Serial
#else
#define HWSerial Serial
USBCDC USBSerial;
#endif

#include <SD.h>
#include <tft_eSPI.h>
TFT_eSPI screen = TFT_eSPI();

#include <WiFi.h>

#include "time.h"
const char* ssid = "";
const char* password = "";

#include "pcf8563.h"  // pcf8563 (Backup Clock)
#include "sntp.h"
PCF8563_Class rtc;
const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// Time zone (see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)
const char* time_format = "%d/%m/%y,%H:%M:%S";

#include <DallasTemperature.h>
#include <OneWire.h>
#define ONE_WIRE_BUS JST_IO_1_1
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature dallasSensors(&oneWire);

#define DEEPSLEEP_INTERUPT_BITMASK pow(2, WAKE_BUTTON) + pow(2, UP_BUTTON) + pow(2, DOWN_BUTTON)

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int batteryMilliVolts = 0;
RTC_DATA_ATTR bool recording = false;
RTC_DATA_ATTR int recordingIntervalSeconds = 10;
RTC_DATA_ATTR int errorCounter = 0;
RTC_DATA_ATTR int numberOfTempSensors;

struct TemperatureSensor {
	DeviceAddress address;
	float temperature;
};

RTC_DATA_ATTR TemperatureSensor temperatureSensorList[10];

void syncClock() {
	// Define the NTP servers to be used for time synchronization
	const char* ntpServer1 = "pool.ntp.org";
	const char* ntpServer2 = "time.nist.gov";
	const char* ntpServer3 = "time.google.com";

	// Set the SNTP operating mode to polling, and configure the NTP servers
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, ntpServer1);
	sntp_setservername(1, ntpServer2);
	sntp_setservername(2, ntpServer3);
	sntp_init();

	WiFi.begin(ssid, password);

	do {
		delay(10);
	} while (!(sntp_getreachability(0) + sntp_getreachability(1) + sntp_getreachability(2) > 0));

	rtc.syncToRtc();
	WiFi.disconnect();
	screen.println("sync to RTC");
}

void setup() {
	++bootCount;

	float tempC;
	char timeStamp[32];
	char buf[32];
	File file;
	int numberOfDevices;			  // Number of temperature devices found
	DeviceAddress tempDeviceAddress;  // We'll use this variable to store a found device address

	esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

	switch (wakeup_reason) {
		case ESP_SLEEP_WAKEUP_EXT0:
		case ESP_SLEEP_WAKEUP_EXT1:
			pinMode(SPI_EN, OUTPUT);
			digitalWrite(SPI_EN, HIGH);	 // 3V3_SPI_EN

			screen.begin();
			screen.setRotation(3);
			screen.fillScreen(TFT_BLACK);
			// screen.setTextColor(TFT_WHITE);
			screen.setTextFont(4);
			screen.println("Reboot: " + String(bootCount));
			// print_wakeup_reason();

			pinMode(BACKLIGHT, OUTPUT);
			analogWrite(BACKLIGHT, 256);

			pinMode(OUTPUT_EN, OUTPUT);
			digitalWrite(OUTPUT_EN, HIGH);

			dallasSensors.begin();

			// Grab a count of devices on the wire
			numberOfTempSensors = dallasSensors.getDeviceCount();

			for (int i = 0; i < numberOfTempSensors; i++) {
				if (dallasSensors.getAddress(tempDeviceAddress, i)) {
					dallasSensors.setResolution(tempDeviceAddress, 10);
					memcpy(temperatureSensorList[i].address, tempDeviceAddress, sizeof(DeviceAddress));
					temperatureSensorList[i].temperature = 0.0;	 // Initialize temperature if needed
				}
			}

			analogSetPinAttenuation(VBAT_SENSE, ADC_0db);  // 0db (0 mV ~ 750 mV)
			delay(300);
			screen.println("Battery: " + String(analogReadMilliVolts(VBAT_SENSE) * VBAT_SENSE_SCALE) + "mV");
			delay(5000);
			break;
		case ESP_SLEEP_WAKEUP_TIMER:
			Wire.begin(WIRE_SDA, WIRE_SCL, 100000);

			rtc.begin(Wire);

			if (rtc.syncToSystem() == true) {
				setenv("TZ", time_zone, 1);
				tzset();
			}

			time_t currentEpoch;
			struct tm timeInfo;
			time(&currentEpoch);
			localtime_r(&currentEpoch, &timeInfo);

			strftime(timeStamp, 32, time_format, &timeInfo);
			ESP_LOGI("RTC Time", "%s", timeStamp);

			pinMode(SPI_EN, OUTPUT);
			digitalWrite(SPI_EN, HIGH);	 // 3V3_SPI_EN

			if (!SD.begin(SD_CARD_CS)) {
				ESP_LOGW("SD Card", "Mount Failed!");
			}

			pinMode(OUTPUT_EN, OUTPUT);
			digitalWrite(OUTPUT_EN, HIGH);

			dallasSensors.requestTemperatures();  // Send the command to get temperatures

			// Loop through each sensor and retrieve temperature
			for (int i = 0; i < numberOfTempSensors; i++) {
				// Get temperature for the current sensor
				tempC = dallasSensors.getTempC(temperatureSensorList[i].address);

				// Check if reading was successful
				if (tempC != DEVICE_DISCONNECTED_C) {
					ESP_LOGI("Dallas", "Temperature %d is: %.1f", i, tempC);
					temperatureSensorList[i].temperature = tempC;  // Update the temperature in the struct
				} else {
					ESP_LOGW("Dallas", "Could not read temperature data for device %d", i);
				} 
			}

			file = SD.open("/data.csv", FILE_APPEND, true);
			sprintf(buf, "%s, %.2f", timeStamp, temperatureSensorList[0].temperature);
			file.println(buf);
			file.close();

			break;
		default:
			break;
	}
}

void loop() {
	esp_sleep_enable_ext1_wakeup(DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
	esp_sleep_enable_timer_wakeup(recordingIntervalSeconds * 1000000ULL);
	ESP_LOGI("DeepSleep", "Going to sleep now");
	esp_deep_sleep_start();
}