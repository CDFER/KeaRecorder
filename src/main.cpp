#include <Arduino.h>

#include "USB.h"
#define HWSerial Serial
//USBCDC USBSerial;

#include <SD.h>
#include <tft_eSPI.h>
TFT_eSPI screen = TFT_eSPI();

#include <WiFi.h>
const char* ssid = "";
const char* password = "";

#include "pcf8563.h"  // pcf8563 (Backup RTC Clock)
#include "sntp.h"
#include "time.h"
PCF8563_Class rtc;
const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// Time zone (see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)
const char* time_format = "%d/%m/%y,%H:%M:%S";

#include <DallasTemperature.h>
#include <OneWire.h>

#define DEEPSLEEP_INTERUPT_BITMASK pow(2, WAKE_BUTTON) + pow(2, UP_BUTTON) + pow(2, DOWN_BUTTON)

RTC_DATA_ATTR uint16_t bootCount = 0;
RTC_DATA_ATTR uint16_t batteryMilliVolts = 0;
RTC_DATA_ATTR bool recording = false;
RTC_DATA_ATTR uint16_t recordingIntervalSeconds = 5;
RTC_DATA_ATTR time_t currentRecordingEpoch;
RTC_DATA_ATTR uint16_t errorCounter = 0;

uint8_t tasksFinished = 0;

#define ONEWIRE_TEMP_RESOLUTION 12
#define ONEWIRE_PORT_COUNT 3

struct temperatureSensor {
	DeviceAddress address;
	float temperature;
	bool error;
};

struct temperatureSensorBus {
	uint8_t numberOfSensors;
	uint8_t oneWirePin;
	OneWire oneWireBus;
	DallasTemperature dallasTemperatureBus;
	temperatureSensor sensorList[5];
};

RTC_DATA_ATTR temperatureSensorBus oneWirePort[ONEWIRE_PORT_COUNT];

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

void clockManagerTask(void* parameter) {
	Wire.begin(WIRE_SDA, WIRE_SCL, 100000);

	rtc.begin(Wire);

	if (rtc.syncToSystem() == true) {
		setenv("TZ", time_zone, 1);
		tzset();
	}

	tasksFinished++;
	vTaskDelete(NULL);
}

void screenManagerTask(void* parameter) {
	pinMode(SPI_EN, OUTPUT);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	digitalWrite(SPI_EN, HIGH);
	digitalWrite(TFT_CS, HIGH);
	digitalWrite(SD_CARD_CS, LOW);

	pinMode(BACKLIGHT, OUTPUT);
	analogWrite(BACKLIGHT, 256);

	screen.begin();
	screen.setRotation(3);
	screen.fillScreen(TFT_BLACK);

	while (true) {
		screen.setTextColor(TFT_WHITE);
		screen.setTextFont(4);
		screen.println("XXX");
		vTaskSuspend(NULL);	 // Wait for new info
	}
}

void writeLineToSDcard() {
	pinMode(SPI_EN, OUTPUT);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	struct tm timeInfo;
	char timeStamp[32];
	File file;
	char buf[64];

	localtime_r(&currentRecordingEpoch, &timeInfo);

	strftime(timeStamp, 32, time_format, &timeInfo);
	ESP_LOGI("Time", "%s", timeStamp);

	sprintf(buf, "%s,%i,%.2f,%.2f,%.2f", timeStamp, batteryMilliVolts, oneWirePort[1].sensorList[0].temperature, oneWirePort[1].sensorList[1].temperature, oneWirePort[1].sensorList[2].temperature);

	digitalWrite(SPI_EN, HIGH);
	digitalWrite(TFT_CS, LOW);
	digitalWrite(SD_CARD_CS, HIGH);

	if (!SD.begin(SD_CARD_CS)) {
		ESP_LOGW("SD Card", "Mount Failed!");
	}

	file = SD.open("/data.csv", FILE_APPEND, true);
	file.println(buf);
	file.close();

	digitalWrite(SPI_EN, LOW);

	ESP_LOGD("data Written to file", "");
}

void readOneWireTemperaturesToSD(void* parameter) {
	pinMode(OUTPUT_EN, OUTPUT);
	digitalWrite(OUTPUT_EN, HIGH);

	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			oneWirePort[i].oneWireBus.begin(oneWirePort[i].oneWirePin);
			oneWirePort[i].dallasTemperatureBus.setOneWire(&oneWirePort[i].oneWireBus);	 // Sets up pointer to oneWire Instance
			oneWirePort[i].dallasTemperatureBus.setWaitForConversion(false);
			oneWirePort[i].dallasTemperatureBus.requestTemperatures();
			ESP_LOGD("requested Temperatures on", "bus: %i", i);
		}
	}

	vTaskDelay(750 / portTICK_PERIOD_MS);

	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				oneWirePort[i].sensorList[j].temperature = oneWirePort[i].dallasTemperatureBus.getTempC(oneWirePort[i].sensorList[j].address);
				ESP_LOGD("got", " %.2f for bus %i sensor %i", oneWirePort[i].sensorList[j].temperature, i, j);
			}
		}
	}
	digitalWrite(OUTPUT_EN, LOW);

	writeLineToSDcard();

	tasksFinished++;
	vTaskDelete(NULL);
}

void scanOneWireAddresses() {
	DeviceAddress tempAddress;	// We'll use this variable to store a found device address

	oneWirePort[0].oneWirePin = (JST_IO_1_1);
	oneWirePort[1].oneWirePin = (JST_IO_2_1);
	oneWirePort[2].oneWirePin = (JST_IO_3_1);

	pinMode(OUTPUT_EN, OUTPUT);
	digitalWrite(OUTPUT_EN, HIGH);

	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		oneWirePort[i].oneWireBus.begin(oneWirePort[i].oneWirePin);
		oneWirePort[i].dallasTemperatureBus.setOneWire(&oneWirePort[i].oneWireBus);				// Sets up pointer to oneWire Instance
		oneWirePort[i].dallasTemperatureBus.begin();											// Sets up and Scans the Bus
		oneWirePort[i].numberOfSensors = oneWirePort[i].dallasTemperatureBus.getDeviceCount();	// Grab a count of devices on the wire
		ESP_LOGD("Found Sensors on", "bus: %i, %i sensors", i, oneWirePort[i].numberOfSensors);

		if (oneWirePort[i].numberOfSensors > 0) {
			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				if (oneWirePort[i].dallasTemperatureBus.getAddress(tempAddress, j)) {
					memcpy(oneWirePort[i].sensorList[j].address, tempAddress, sizeof(DeviceAddress));
					oneWirePort[i].dallasTemperatureBus.setResolution(oneWirePort[i].sensorList[j].address, ONEWIRE_TEMP_RESOLUTION);
				}
			}
		}
	}
	digitalWrite(OUTPUT_EN, LOW);
}

void readBatteryVoltageTask(void* parameter) {
	analogSetPinAttenuation(VBAT_SENSE, ADC_0db);  // 0db (0 mV ~ 750 mV)
	vTaskDelay(300 / portTICK_PERIOD_MS);
	batteryMilliVolts = analogReadMilliVolts(VBAT_SENSE) * VBAT_SENSE_SCALE;
	ESP_LOGD("Battery Voltage", "%imV", batteryMilliVolts);
	tasksFinished++;
	vTaskDelete(NULL);
}

void enterDeepSleep() {
	esp_sleep_enable_ext1_wakeup(DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
	ESP_LOGI("DeepSleep", "Going to sleep now");
	esp_deep_sleep_start();
}

void setup() {
	++bootCount;
	ESP_LOGI("BootCount", "%i", bootCount);

	esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

	switch (wakeup_reason) {
		case ESP_SLEEP_WAKEUP_EXT0:
		case ESP_SLEEP_WAKEUP_EXT1:
			ESP_LOGI("Woken by", "Button");
			xTaskCreate(screenManagerTask, "screenManagerTask", 5000, NULL, 1, NULL);
			xTaskCreate(readBatteryVoltageTask, "readBatteryVoltageTask", 5000, NULL, 1, NULL);
			scanOneWireAddresses();
			vTaskDelay(5000 / portTICK_PERIOD_MS);

			if (batteryMilliVolts > 3300) {
				time_t currentEpoch;
				time(&currentEpoch);
				uint16_t secondsToNextRecording = currentRecordingEpoch - currentEpoch;
				esp_sleep_enable_timer_wakeup(secondsToNextRecording * 1000000ULL);
				ESP_LOGI("Going to sleep for", "%is", secondsToNextRecording);
			} else {
				ESP_LOGW("Battery", "Low Voltage, entering long sleep");
			}

			enterDeepSleep();

			break;
		case ESP_SLEEP_WAKEUP_TIMER:
			ESP_LOGI("Woken by", "Timer");
			tasksFinished = 0;
			xTaskCreate(readOneWireTemperaturesToSD, "readOneWireTemperaturesToSD", 10000, NULL, 1, NULL);
			vTaskDelay(50 / portTICK_PERIOD_MS);
			xTaskCreate(readBatteryVoltageTask, "readBatteryVoltageTask", 5000, NULL, 3, NULL);
			xTaskCreate(clockManagerTask, "clockManagerTask", 5000, NULL, 3, NULL);

			vTaskDelay(900 / portTICK_PERIOD_MS);

			while (tasksFinished < 3) {
				vTaskDelay(30 / portTICK_PERIOD_MS);
			}

			if (batteryMilliVolts > 3300) {
				currentRecordingEpoch += recordingIntervalSeconds;

				time_t currentEpoch;
				time(&currentEpoch);
				uint16_t secondsToNextRecording = currentRecordingEpoch - currentEpoch;
				esp_sleep_enable_timer_wakeup(secondsToNextRecording * 1000000ULL);
				ESP_LOGI("Going to sleep for", "%is", secondsToNextRecording);
			} else {
				ESP_LOGW("Battery", "Low Voltage, entering long sleep");
			}
			enterDeepSleep();

			break;

		default:
			ESP_LOGI("Woken by", "Other");
			xTaskCreate(clockManagerTask, "clockManagerTask", 5000, NULL, 1, NULL);
			scanOneWireAddresses();
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			time_t currentEpoch;
			time(&currentEpoch);
			currentRecordingEpoch = currentEpoch + recordingIntervalSeconds;
			esp_sleep_enable_timer_wakeup(recordingIntervalSeconds * 1000000ULL);
			ESP_LOGI("Going to sleep for", "%is", recordingIntervalSeconds);
			enterDeepSleep();

			break;
	}
}

void loop() {
	vTaskSuspend(NULL);	 // Loop task Not Needed
}