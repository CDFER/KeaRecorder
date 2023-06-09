#include <Arduino.h>

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

#define SCREEN_ON_TIME 10

#include <WiFi.h>

#include "credentials.h"
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#define WIFI_WIFI_SSID "YourWIFI"
#define WIFI_PW "PASSWORD"

#endif

#include "pcf8563.h"  // pcf8563 (Backup RTC Clock)
#include "sntp.h"
#include "time.h"
PCF8563_Class rtc;
const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// Time zone (see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)
const char* time_format = "%d/%m/%y,%H:%M:%S";

#include <DallasTemperature.h>
#include <OneWire.h>

#define DEEPSLEEP_INTERUPT_BITMASK pow(2, WAKE_BUTTON) + pow(2, VUSB_SENSE)
#define RTC_DEEPSLEEP_INTERUPT_BITMASK DEEPSLEEP_INTERUPT_BITMASK + pow(2, WIRE_RTC_INT)

#define HOLD_DURATION 3000	// Hold duration in milliseconds to start and end recording

RTC_DATA_ATTR uint32_t batteryMilliVolts = 0;
RTC_DATA_ATTR bool recording = false;
RTC_DATA_ATTR uint8_t recordingIntervalMins = 2;

#define ONEWIRE_TEMP_RESOLUTION 9
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
	uint32_t color;
};

struct sdCard {
	uint8_t cardTotalGib;	/** Card size in GiB. */
	uint8_t cardUsedGib;	/** Card space used in GiB. */
	uint8_t cardType;		/** Card type. */
	bool connected = false; /** Flag indicating if an SD card is connected. */
};

RTC_DATA_ATTR temperatureSensorBus oneWirePort[ONEWIRE_PORT_COUNT];
RTC_DATA_ATTR sdCard microSDCard;

bool systemTimeValid = false;

bool recordingDot = true;

SemaphoreHandle_t buttonSemaphore;

void buttonInterrupt() {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(buttonSemaphore, &xHigherPriorityTaskWoken);
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void setupNextAlarm() {
	// Read the current time from the RTC
	RTC_Date currentTime = rtc.getDateTime();

	// Calculate the nearest alarm time based on the interval
	uint8_t alarmMinute = currentTime.minute / recordingIntervalMins * recordingIntervalMins;
	if (currentTime.minute % recordingIntervalMins != 0) {
		alarmMinute += recordingIntervalMins;
	}

	// Check if the calculated alarm minute is in the past, increase into the future if needed
	if (alarmMinute <= currentTime.minute) {
		alarmMinute += recordingIntervalMins;
		if (alarmMinute >= 60) {
			alarmMinute = 0;
		}
	}

	// Set the alarm
	rtc.setAlarmByMinutes(alarmMinute);
	rtc.enableAlarm();
	rtc.disableTimer();
	rtc.clearTimer();

	ESP_LOGI("Setting Recording Alarm to ", "%i, currently %i", alarmMinute, currentTime.minute);
}

void buttonTask(void* parameter) {
	pinMode(WAKE_BUTTON, INPUT);
	attachInterrupt(digitalPinToInterrupt(WAKE_BUTTON), buttonInterrupt, RISING);

	buttonSemaphore = xSemaphoreCreateBinary();

	TickType_t lastWakeTime = xTaskGetTickCount();

	while (1) {
		if (xSemaphoreTake(buttonSemaphore, portMAX_DELAY) == pdTRUE) {
			if (digitalRead(WAKE_BUTTON) == HIGH) {
				vTaskDelay(HOLD_DURATION / portTICK_PERIOD_MS);

				if (digitalRead(WAKE_BUTTON) == HIGH) {
					if (recording == true) {
						recording = false;

					} else {
						recording = true;
						setupNextAlarm();
					}
				}
			}
		}

		vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(10));	// Check every 10 milliseconds for inturpt semaphore
	}
}

void enterDeepSleep() {
	if (batteryMilliVolts > 3300 & recording == true) {
		esp_sleep_enable_ext1_wakeup(RTC_DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
		ESP_LOGV("Enter DeepSleep", "Waiting For RTC or user input");
	} else {
		esp_sleep_enable_ext1_wakeup(DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
		ESP_LOGI("Enter DeepSleep", "Waiting for user input");
	}
	esp_deep_sleep_start();
}

void updateClock() {
	Wire.begin(WIRE_SDA, WIRE_SCL, 100000);
	rtc.begin(Wire);

	if (rtc.syncToSystem() == true) {
		setenv("TZ", time_zone, 1);
		tzset();
		systemTimeValid = true;
	} else {
		ESP_LOGE("Time", "NOT VALID");
	}

	if (recording == true) {
		pinMode(WIRE_RTC_INT, INPUT);

		if (digitalRead(WIRE_RTC_INT) == HIGH) {
			setupNextAlarm();
		}

	} else if (systemTimeValid == false) {
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

		WiFi.begin(WIFI_SSID, WIFI_PW);

		do {
			delay(10);
		} while (!(sntp_getreachability(0) + sntp_getreachability(1) + sntp_getreachability(2) > 0));

		rtc.syncToRtc();
		WiFi.disconnect();
	}
}

// Function to extract the first hex character from byte 1, 3, 5, and 7 of a DeviceAddress
char* deviceAddressto4Char(const DeviceAddress& address) {
	static char result[5];	// Static array to hold the extracted hex characters
	result[4] = '\0';		// Null-terminate the result array

	// Extract the hex characters from bytes 1, 3, 5, and 7
	result[0] = "0123456789ABCDEF"[(address[1] >> 4) & 0x0F];
	result[1] = "0123456789ABCDEF"[(address[3] >> 4) & 0x0F];
	result[2] = "0123456789ABCDEF"[(address[5] >> 4) & 0x0F];
	result[3] = "0123456789ABCDEF"[(address[7] >> 4) & 0x0F];

	return result;
}

/**
 * @brief Calculate battery percentage based on voltage using a lookup table.
 *
 * Lookup table for battery voltage in millivolts and corresponding percentage (based on PANASONIC_NCR_18650_B).
 * battery voltage 3300 = 3.3V, state of charge 22 = 22%.
 */
const uint16_t batteryDischargeCurve[2][10] = {
	{3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200},
	{0, 13, 22, 39, 53, 62, 74, 84, 94, 100}};

/**
 * @brief Calculate battery percentage based on voltage.
 *
 * @param batteryMilliVolts The battery voltage in millivolts.
 * @return The battery percentage as a null-terminated char array.
 */
const char* calculateBatteryPercentage(uint16_t batteryMilliVolts) {
	static char batteryPercentage[5];  // Static array to hold the battery percentage
	batteryPercentage[4] = '\0';	   // Null-terminate the array

	uint8_t tableSize = sizeof(batteryDischargeCurve[0]) / sizeof(batteryDischargeCurve[0][0]);
	uint8_t percentage = 0;
	for (uint8_t i = 0; i < tableSize - 1; i++) {  // loop through table to find the two lookup values we are between
		if (batteryMilliVolts <= batteryDischargeCurve[0][i + 1]) {
			// x axis is millivolts, y axis is charge percentage interpolation to find the battery percentage
			uint16_t x0 = batteryDischargeCurve[0][i];
			uint16_t x1 = batteryDischargeCurve[0][i + 1];
			uint8_t y0 = batteryDischargeCurve[1][i];
			uint8_t y1 = batteryDischargeCurve[1][i + 1];
			percentage = static_cast<uint8_t>(y0 + ((y1 - y0) * (batteryMilliVolts - x0)) / (x1 - x0));
			break;
		}
	}

	// Convert the percentage to a char array
	snprintf(batteryPercentage, sizeof(batteryPercentage), "%d%%", percentage);

	return batteryPercentage;
}

const char* sdCardType(struct sdCard& cardInfo) {
	const char* cardTypeString = "";

	if (cardInfo.cardType == CARD_NONE) {
		// Serial.println("No SD card attached");
	} else {
		// Serial.print("SD Card Type: ");
		if (cardInfo.cardType == CARD_MMC) {
			cardTypeString = "MMC";
		} else if (cardInfo.cardType == CARD_SD) {
			cardTypeString = "SDSC";
		} else if (cardInfo.cardType == CARD_SDHC) {
			cardTypeString = "SDHC";
		} else {
			cardTypeString = "UNKNOWN";
		}
		// Serial.println(cardTypeString);
	}
	return cardTypeString;
}

/**
 * @brief Populates the provided sdCard struct with information about the connected SD card.
 *
 * @param cardInfo Reference to the sdCard struct to populate.
 */
void populateSDCardInfo(struct sdCard& cardInfo) {
	uint8_t cardType = SD.cardType();
	cardInfo.connected = true;
	cardInfo.cardType = cardType;

	if (cardType == CARD_NONE) {
		Serial.println("No SD card attached");
		return;
	}

	uint64_t cardSize = SD.totalBytes() / (1024 * 1024 * 1024);
	cardInfo.cardTotalGib = static_cast<uint8_t>(cardSize);

	uint64_t usedSize = SD.usedBytes() / (1024 * 1024 * 1024);
	cardInfo.cardUsedGib = static_cast<uint8_t>(usedSize);

	SD.end();
}

void writeLineToSDcard() {
	pinMode(SPI_EN, OUTPUT);
	digitalWrite(SPI_EN, HIGH);

	struct tm timeInfo;
	time_t currentEpoch;
	time(&currentEpoch);
	char currentTimeStamp[32];
	localtime_r(&currentEpoch, &timeInfo);
	strftime(currentTimeStamp, 32, time_format, &timeInfo);

	char buf[128];
	sprintf(buf, "%s,%i", currentTimeStamp, batteryMilliVolts);

	SD.begin(SD_CARD_CS);
	File file;
	file = SD.open("/timeTest.csv", FILE_APPEND, true);

	// Check if the file is empty
	if (file.size() == 0) {
		char headerBuf[128] = "currentTimeStamp, batteryMilliVolts";
		// Iterate over each temperature sensor bus
		for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
			if (oneWirePort[i].numberOfSensors > 0) {
				// Iterate over each sensor on the current bus
				for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
					// Convert the address to a string
					strcat(headerBuf, deviceAddressto4Char(oneWirePort[i].sensorList[j].address));
				}
			}
		}

		file.println(headerBuf);
	}

	// Iterate over each temperature sensor bus
	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			// Iterate over each sensor on the current bus
			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				// Read the temperature for the current sensor
				float temperature = oneWirePort[i].dallasTemperatureBus.getTempC(oneWirePort[i].sensorList[j].address);

				// Update the temperature value in the sensor list
				oneWirePort[i].sensorList[j].temperature = temperature;

				// Append the temperature value to the string
				char tempStr[10];
				sprintf(tempStr, ",%.2f", temperature);
				strcat(buf, tempStr);
			}
		}
	}

	file.println(buf);
	file.close();

	ESP_LOGD("", "%s", buf);
}

void initScreen() {
	screen.begin();
	screen.setRotation(2);
	screen.fillScreen(TFT_BLACK);

	pinMode(BACKLIGHT, OUTPUT);
	analogWrite(BACKLIGHT, 256);
}

void updateScreen() {
	// Draw REC symbol
	if (recording == true) {
		if (recordingDot == true) {
			screen.fillSmoothCircle(8 + 10, 8 + 10, 10, TFT_RED, TFT_BLACK);

			screen.setTextColor(TFT_WHITE, TFT_BLACK, true);
			screen.drawString("REC", 34, 8, 4);
		} else {
			screen.fillCircle(8 + 10, 8 + 10, 12, TFT_BLACK);
		}
		recordingDot = !recordingDot;
	}

	screen.setTextColor(TFT_WHITE, TFT_BLACK, true);  // fills background to wipe previous text

	// Draw Battery Percentage
	screen.drawString(calculateBatteryPercentage(batteryMilliVolts), 112, 8, 4);

	// Draw Temperature Values
	uint8_t yPosition = 36;

	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			screen.drawWideLine(6, yPosition, 6, yPosition + (oneWirePort[i].numberOfSensors - 1) * 26 + 20, 5, oneWirePort[i].color, TFT_BLACK);  // draw color bar to indicate bus

			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				screen.drawString(deviceAddressto4Char(oneWirePort[i].sensorList[j].address), 12, yPosition, 4);

				screen.drawFloat(oneWirePort[i].sensorList[j].temperature, 1, 12 + (4 * 20), yPosition, 4);
				screen.drawString("`c", 12 + (4 * 20) + 50, yPosition, 4);

				yPosition += 26;
			}

			yPosition += 13;
		}
	}

	screen.setTextColor(TFT_DARKGREY, TFT_BLACK, true);	 // fills background to wipe previous text
	screen.setCursor(8, 285, 2);
	if (microSDCard.connected == true) {
		screen.printf("%i/%iGiB %s", microSDCard.cardUsedGib, microSDCard.cardTotalGib, sdCardType(microSDCard));
	} else {
		screen.printf("No SD card");
	}

	// Draw Clock
	struct tm timeInfo;
	char dateTime[32];
	time_t currentEpoch;
	time(&currentEpoch);
	localtime_r(&currentEpoch, &timeInfo);
	strftime(dateTime, 32, "%d/%b/%Y %H:%M", &timeInfo);

	screen.drawString(dateTime, 8, 300, 2);
}

void SPIManagerTask(void* parameter) {
	pinMode(SPI_EN, OUTPUT);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	digitalWrite(SPI_EN, HIGH);
	digitalWrite(TFT_CS, LOW);
	digitalWrite(SD_CARD_CS, LOW);

	if (SD.begin(SD_CARD_CS)) {
		populateSDCardInfo(microSDCard);
	} else {
		ESP_LOGW("SD Card", "Mount Failed!");
		microSDCard.connected = false;
	}

	initScreen();

	while (true) {
		updateScreen();
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}

	vTaskDelete(NULL);
}

void scanOneWireBusses() {
	DeviceAddress tempAddress;	// We'll use this variable to store a found device address

	oneWirePort[0].oneWirePin = (JST_IO_1_1);
	oneWirePort[1].oneWirePin = (JST_IO_2_1);
	oneWirePort[2].oneWirePin = (JST_IO_3_1);

	oneWirePort[0].color = TFT_RED;
	oneWirePort[1].color = TFT_GREEN;
	oneWirePort[2].color = TFT_BLUE;

	pinMode(OUTPUT_EN, OUTPUT);
	digitalWrite(OUTPUT_EN, HIGH);

	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		oneWirePort[i].oneWireBus.begin(oneWirePort[i].oneWirePin);
		oneWirePort[i].dallasTemperatureBus.setOneWire(&oneWirePort[i].oneWireBus);				// Sets up pointer to oneWire Instance
		oneWirePort[i].dallasTemperatureBus.begin();											// Sets up and Scans the Bus
		oneWirePort[i].numberOfSensors = oneWirePort[i].dallasTemperatureBus.getDeviceCount();	// Grab a count of devices on the wire
		// ESP_LOGD("Found Sensors on", "bus: %i, %i sensors", i, oneWirePort[i].numberOfSensors);

		if (oneWirePort[i].numberOfSensors > 0) {
			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				if (oneWirePort[i].dallasTemperatureBus.getAddress(tempAddress, j)) {
					memcpy(oneWirePort[i].sensorList[j].address, tempAddress, sizeof(DeviceAddress));
					oneWirePort[i].dallasTemperatureBus.setResolution(oneWirePort[i].sensorList[j].address, ONEWIRE_TEMP_RESOLUTION);
				}
			}
		}
	}
}

void readOneWireTemperatures() {
	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			oneWirePort[i].oneWireBus.begin(oneWirePort[i].oneWirePin);
			oneWirePort[i].dallasTemperatureBus.setOneWire(&oneWirePort[i].oneWireBus);	 // Sets up pointer to oneWire Instance
			oneWirePort[i].dallasTemperatureBus.setWaitForConversion(false);
			oneWirePort[i].dallasTemperatureBus.requestTemperatures();
			// ESP_LOGD("requested Temperatures on", "bus: %i", i);
		}
	}

	vTaskDelay(oneWirePort[0].dallasTemperatureBus.millisToWaitForConversion(ONEWIRE_TEMP_RESOLUTION) / portTICK_PERIOD_MS);

	for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				oneWirePort[i].sensorList[j].temperature = oneWirePort[i].dallasTemperatureBus.getTempC(oneWirePort[i].sensorList[j].address);
				// ESP_LOGD("got", " %.2f for bus %i sensor %i", oneWirePort[i].sensorList[j].temperature, i, j);
			}
		}
	}
}

void readOneWireTemperaturesTask(void* parameter) {
	while (true) {
		if (recording == false){
			scanOneWireBusses();
		}
		readOneWireTemperatures();
	}
	vTaskDelete(NULL);
}

void setup() {
	uint64_t wakeupStatus = esp_sleep_get_ext1_wakeup_status();
	uint8_t wakeupPin = static_cast<uint8_t>(log(wakeupStatus) / log(2));
	batteryMilliVolts = analogReadMilliVolts(VBAT_SENSE) * VBAT_SENSE_SCALE;

	switch (wakeupPin) {
		case WIRE_RTC_INT:
			ESP_LOGD("batteryMilliVolts", " %i", batteryMilliVolts);
			updateClock();
			readOneWireTemperatures();
			writeLineToSDcard();
			enterDeepSleep();
			break;

		case VUSB_SENSE:
			USBSerial.begin();
			USBSerial.setDebugOutput(true);
			USB.begin();

		case WAKE_BUTTON:
		case UP_BUTTON:
		case DOWN_BUTTON:
		default:

			ESP_LOGV("UI Mode", "");

			xTaskCreate(buttonTask, "Button Task", 2048, NULL, 2, NULL);
			xTaskCreate(SPIManagerTask, "SPIManagerTask", 5000, NULL, 1, NULL);
			xTaskCreate(readOneWireTemperaturesTask, "readOneWireTemperaturesTask", 10000, NULL, 2, NULL);

			updateClock();

			pinMode(VUSB_SENSE, INPUT);

			while (millis() <= SCREEN_ON_TIME * 1000 || digitalRead(VUSB_SENSE) == HIGH) {
				vTaskDelay(500 / portTICK_PERIOD_MS);
			}
			enterDeepSleep();

			break;
	}
}

void loop() {
	vTaskSuspend(NULL);	 // Loop task Not Needed
}