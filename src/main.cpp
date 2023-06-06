#include <Arduino.h>

#include "USB.h"
#define HWSerial Serial
// USBCDC USBSerial;

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
RTC_DATA_ATTR uint16_t recordingIntervalSeconds = 900;
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
	uint32_t color;
};

/**
 * @brief Structure to hold SD card information.
 */
struct sdCard {
	uint8_t cardTotalGib;	/** Card size in GiB. */
	uint8_t cardUsedGib;	/** Card space used in GiB. */
	uint8_t cardType;		/** Card type. */
	bool connected = false; /** Flag indicating if an SD card is connected. */
};

struct logFile {
	uint16_t sizeMib;
	uint16_t datapoints;
	char* filename;
};

RTC_DATA_ATTR temperatureSensorBus oneWirePort[ONEWIRE_PORT_COUNT];
RTC_DATA_ATTR sdCard microSDCard;

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
		Serial.println("No SD card attached");
	} else {
		Serial.print("SD Card Type: ");
		if (cardInfo.cardType == CARD_MMC) {
			cardTypeString = "MMC";
		} else if (cardInfo.cardType == CARD_SD) {
			cardTypeString = "SDSC";
		} else if (cardInfo.cardType == CARD_SDHC) {
			cardTypeString = "SDHC";
		} else {
			cardTypeString = "UNKNOWN";
		}
		Serial.println(cardTypeString);
	}
	return cardTypeString;
}

/**
 * @brief Populates the provided sdCard struct with information about the connected SD card.
 *
 * @param cardInfo Reference to the sdCard struct to populate.
 */
void populateSDCardInfo(struct sdCard& cardInfo) {
	pinMode(SPI_EN, OUTPUT);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	digitalWrite(SPI_EN, HIGH);
	digitalWrite(TFT_CS, LOW);
	digitalWrite(SD_CARD_CS, HIGH);

	if (!SD.begin(SD_CARD_CS)) {
		ESP_LOGW("SD Card", "Mount Failed!");
		cardInfo.connected = false;
	}

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

void screenManagerTask(void* parameter) {
	bool recordingDot = true;

	pinMode(SPI_EN, OUTPUT);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	digitalWrite(SPI_EN, HIGH);
	digitalWrite(TFT_CS, HIGH);
	digitalWrite(SD_CARD_CS, LOW);

	pinMode(BACKLIGHT, OUTPUT);
	analogWrite(BACKLIGHT, 256);

	screen.begin();
	screen.setRotation(2);
	// screen.setRotation(3);
	screen.fillScreen(TFT_BLACK);

	while (true) {
		screen.setTextColor(TFT_WHITE, TFT_BLACK, true);  // fills background to wipe previous text

		// Draw REC symbol
		if (recordingDot == true) {
			screen.fillSmoothCircle(8 + 10, 8 + 10, 10, TFT_RED);
			screen.drawString("REC", 34, 8, 4);
		} else {
			screen.fillSmoothCircle(8 + 10, 8 + 10, 10, TFT_BLACK);
		}
		recordingDot = !recordingDot;

		// Draw Battery Percentage
		screen.drawString(calculateBatteryPercentage(batteryMilliVolts), 112, 8, 4);

		// Draw Temerature Values
		uint8_t yPosition = 36;

		for (uint8_t i = 0; i < ONEWIRE_PORT_COUNT; i++) {
			if (oneWirePort[i].numberOfSensors > 0) {
				screen.drawWideLine(6, yPosition, 6, yPosition + (oneWirePort[i].numberOfSensors - 1) * 26 + 20, 5, oneWirePort[i].color);	// draw color bar to indicate bus

				for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
					screen.drawString(deviceAddressto4Char(oneWirePort[i].sensorList[j].address), 12, yPosition, 4);

					screen.drawFloat(oneWirePort[i].sensorList[j].temperature, 1, 12 + (4 * 20), yPosition, 4);
					screen.drawString("`c", 12 + (4 * 20) + 50, yPosition, 4);

					// screen.setCursor(12 + (4 * 20), yPosition, 4);
					// screen.printf("%2.1f`C", oneWirePort[i].sensorList[j].temperature);
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
		localtime_r(&currentRecordingEpoch, &timeInfo);
		strftime(dateTime, 32, "%d/%b/%Y %H:%M", &timeInfo);

		screen.drawString(dateTime, 8, 300, 2);

		vTaskDelay(500 / portTICK_PERIOD_MS);

		// vTaskSuspend(NULL);	 // Wait for new info
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

	// writeLineToSDcard();

	tasksFinished++;
	vTaskDelete(NULL);
}

void scanOneWireAddresses() {
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

		case ESP_SLEEP_WAKEUP_EXT0:
		case ESP_SLEEP_WAKEUP_EXT1:
		default:
			ESP_LOGI("Woken by", "Button");
			populateSDCardInfo(microSDCard);

			vTaskDelay(1000 / portTICK_PERIOD_MS);

			xTaskCreate(screenManagerTask, "screenManagerTask", 5000, NULL, 1, NULL);
			xTaskCreate(readBatteryVoltageTask, "readBatteryVoltageTask", 5000, NULL, 1, NULL);
			scanOneWireAddresses();
			// xTaskCreate(readOneWireTemperaturesToSD, "readOneWireTemperaturesToSD", 10000, NULL, 1, NULL);
			vTaskDelay(1200000 / portTICK_PERIOD_MS);

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
	}
}

void loop() {
	vTaskSuspend(NULL);	 // Loop task Not Needed
}