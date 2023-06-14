#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SD.h>
#include <WiFi.h>
#include <tft_eSPI.h>

// #include "sdusb.h"
#include "USB.h"
#include "USBMSC.h"
#include "credentials.h"
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#define WIFI_WIFI_SSID "YourWIFI"
#define WIFI_PW "PASSWORD"

#endif

#include "pcf8563.h"
#include "sntp.h"
#include "time.h"

// Constants
constexpr uint8_t SCREEN_ON_TIME = 10;
constexpr uint16_t HOLD_DURATION = 3000;
constexpr uint8_t ONEWIRE_TEMP_RESOLUTION = 10;

const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";
constexpr uint32_t DEEPSLEEP_INTERUPT_BITMASK = (1UL << WAKE_BUTTON) | (1UL << VUSB_SENSE);
constexpr uint32_t RTC_DEEPSLEEP_INTERUPT_BITMASK = DEEPSLEEP_INTERUPT_BITMASK | (1UL << WIRE_RTC_INT);

// Ultra Global Variables (stored even in deep sleep)
RTC_DATA_ATTR uint32_t batteryMilliVolts = 0;
RTC_DATA_ATTR bool recording = false;
RTC_DATA_ATTR uint8_t recordingIntervalMins = 2;
RTC_DATA_ATTR char logFilePath[64];

// Global Variables
TFT_eSPI screen;
PCF8563_Class rtc;
bool systemTimeValid = false;
bool recordingDot = true;

SemaphoreHandle_t buttonSemaphore;

USBMSC MSC;

// For this to work with Espressif's ESP32 code you need to change line 694 esp32 / hardware / eps32 / 2.0.3 / libraries / SD / src / sd_diskio.cpp :

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
	// Serial.printf("MSC WRITE: lba: %u, offset: %u, bufsize: %u\n", lba, offset, bufsize);
	return SD.writeRAW((uint8_t*)buffer, lba) ? bufsize : -1;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
	// Serial.printf("MSC READ: lba: %u, offset: %u, bufsize: %u\n", lba, offset, bufsize);
	return SD.readRAW((uint8_t*)buffer, lba) ? bufsize : -1;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
	// Serial.printf("MSC START/STOP: power: %u, start: %u, eject: %u\n", power_condition, start, load_eject);
	return true;
}

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
	uint16_t cardTotalMib;
	uint16_t cardUsedMib;
	const char* cardTypeString = "";
	bool connected = false;
};

const uint8_t oneWirePortCount = 3;

RTC_DATA_ATTR temperatureSensorBus oneWirePort[oneWirePortCount];

RTC_DATA_ATTR sdCard microSDCard;

auto configurePin = [](int pin, int mode, int initialState) {
	pinMode(pin, mode);
	digitalWrite(pin, initialState);
};

/**
 * @brief Interrupt handler for the button press.
 *
 * This function is called when the button interrupt is triggered. It gives the button semaphore,
 * allowing the button task to proceed. It also yields to a higher priority task if necessary.
 */
void buttonInterrupt() {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(buttonSemaphore, &xHigherPriorityTaskWoken);
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
 * @brief Sets up the next alarm based on the current time and recording interval.
 *
 * This function calculates the next alarm minute based on the current time and the specified
 * recording interval. If the current minute is not aligned with the recording interval, it
 * adjusts the alarm minute to align with the next interval. If the alarm minute exceeds 60,
 * it wraps around to the next hour. The alarm is then set using the calculated minute.
 * Additionally, the alarm, timer, and CLK are enabled, and a log message is printed.
 */
void setupNextAlarm() {
	RTC_Date currentTime = rtc.getDateTime();

	uint8_t alarmMinute = currentTime.minute;

	if (currentTime.minute % recordingIntervalMins != 0) {
		// The current minute is not aligned with the recording interval
		// Calculate the adjustment needed to align with the next interval
		alarmMinute += recordingIntervalMins - (currentTime.minute % recordingIntervalMins);
	} else {
		// The current minute is already aligned with the recording interval
		// Increment alarmMinute by the recording interval
		alarmMinute += recordingIntervalMins;
	}

	if (alarmMinute >= 60) {
		// Set alarmMinute to 0 to wrap around to the next hour
		alarmMinute = 0;
	}

	rtc.setAlarmByMinutes(alarmMinute);
	rtc.enableAlarm();
	rtc.disableTimer();
	rtc.disableCLK();

	ESP_LOGI("Setting Recording Alarm", "Next alarm: %i, Current minute: %i", alarmMinute, currentTime.minute);
}

const char* generateFilename() {
	// Get current timestamp
	struct tm timeInfo;
	time_t currentEpoch;
	time(&currentEpoch);
	localtime_r(&currentEpoch, &timeInfo);

	// Format the timestamp
	static char currentTimeStamp[32];
	strftime(currentTimeStamp, sizeof(currentTimeStamp), "%e-%b-%Y_%H-%M", &timeInfo);	// 14-Jun-2023_15-37

	// Get the last two digits of the MAC address
	uint8_t mac[6];
	WiFi.macAddress(mac);
	char macSuffix[3];
	sprintf(macSuffix, "%02X", mac[5]);

	// Static buffer size for the filename
	static char filename[64];

	// Build the filename with leading forward slash
	snprintf(filename, sizeof(filename), "/%s_%s.csv", currentTimeStamp, macSuffix);  // 14-Jun-2023_15-37_DA.csv

	return filename;
}

/**
 * @brief Task that monitors the wake button and toggles recording mode.
 *
 * This task initializes the wake button pin, attaches an interrupt to detect rising edges,
 * and monitors the button state to toggle the recording mode. When the button is pressed
 * and held for a certain duration, the recording mode is toggled. If recording is enabled,
 * the next alarm is set. The task periodically checks the button state using a semaphore
 * and waits for interrupts to occur.
 *
 * @param parameter Pointer to the task parameters (not used in this implementation).
 */
void buttonTask(void* parameter) {
	pinMode(WAKE_BUTTON, INPUT);
	attachInterrupt(digitalPinToInterrupt(WAKE_BUTTON), buttonInterrupt, RISING);

	buttonSemaphore = xSemaphoreCreateBinary();

	TickType_t lastWakeTime = xTaskGetTickCount();

	while (true) {
		if (xSemaphoreTake(buttonSemaphore, portMAX_DELAY) == pdTRUE) {
			if (digitalRead(WAKE_BUTTON) == HIGH) {
				vTaskDelay(pdMS_TO_TICKS(HOLD_DURATION));  // Delay for the hold duration

				if (digitalRead(WAKE_BUTTON) == HIGH) {
					if (recording) {
						recording = false;
					} else {
						recording = true;
						strcpy(logFilePath, generateFilename());
						ESP_LOGI("Started New File", "%s", logFilePath);

						setupNextAlarm();
					}
				}
			}
		}

		vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(10));	// Check every 10 milliseconds for interrupt semaphore
	}
}

/**
 * @brief Enters deep sleep mode based on battery voltage and recording status.
 *
 * If the battery voltage is above 3300 millivolts and recording is enabled, the function
 * enables deep sleep mode with RTC wakeup. Otherwise, it enables deep sleep mode with
 * wakeup triggered by user input. After setting up the wakeup mode, the function starts
 * the deep sleep process.
 */
void enterDeepSleep() {
	bool enableRTCWakeup = (batteryMilliVolts > 3300) && recording;

	if (enableRTCWakeup) {
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

	if (rtc.syncToSystem()) {
		setenv("TZ", time_zone, 1);
		tzset();
		systemTimeValid = true;
	} else {
		ESP_LOGE("Time", "NOT VALID");
	}

	if (recording) {
		pinMode(WIRE_RTC_INT, INPUT);

		if (digitalRead(WIRE_RTC_INT) == HIGH) {
			setupNextAlarm();
		}
	} else if (!systemTimeValid) {
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

		while (!(sntp_getreachability(0) + sntp_getreachability(1) + sntp_getreachability(2) > 0)) {
			delay(10);
		}

		rtc.syncToRtc();
		WiFi.disconnect();
	}
}

// Function to extract the first hex character from byte 1, 3, 5, and 7 of a DeviceAddress
char* deviceAddressto4Char(const DeviceAddress& address) {
	static char result[5];	// Static array to hold the extracted hex characters
	result[4] = '\0';		// Null-terminate the result array

	// Define a lookup table for hex characters
	const char hexLookup[] = "0123456789ABCDEF";

	// Extract the hex characters from bytes 1, 3, 5, and 7
	result[0] = hexLookup[(address[1] >> 4) & 0x0F];
	result[1] = hexLookup[(address[3] >> 4) & 0x0F];
	result[2] = hexLookup[(address[5] >> 4) & 0x0F];
	result[3] = hexLookup[(address[7] >> 4) & 0x0F];

	return result;
}

/**
 * @brief Calculate battery percentage based on voltage using a lookup table.
 *
 * Lookup table for battery voltage in millivolts and corresponding percentage (based on PANASONIC_NCR_18650_B).
 * battery voltage 3300 = 3.3V, state of charge 22 = 22%.
 */
const uint16_t batteryDischargeCurve[2][12] = {
	{0, 3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200, 9999},
	{0, 0, 13, 22, 39, 53, 62, 74, 84, 94, 100, 100}};

/**
 * @brief Calculate battery percentage based on voltage.
 *
 * @param batteryMilliVolts The battery voltage in millivolts.
 * @return The battery percentage as a null-terminated char array.
 */
const char* calculateBatteryPercentage(uint16_t batteryMilliVolts) {
	static char batteryPercentage[5];  // Static array to hold the battery percentage
	batteryPercentage[4] = '\0';	   // Null-terminate the array

	// Determine the size of the lookup table
	uint8_t tableSize = sizeof(batteryDischargeCurve[0]) / sizeof(batteryDischargeCurve[0][0]);

	// Initialize the percentage variable
	uint8_t percentage = 0;

	// Iterate through the lookup table to find the two lookup values we are between
	for (uint8_t i = 0; i < tableSize - 1; i++) {
		// Check if the battery voltage is within the current range
		if (batteryMilliVolts <= batteryDischargeCurve[0][i + 1]) {
			// Get the x and y values for interpolation
			uint16_t x0 = batteryDischargeCurve[0][i];
			uint16_t x1 = batteryDischargeCurve[0][i + 1];
			uint8_t y0 = batteryDischargeCurve[1][i];
			uint8_t y1 = batteryDischargeCurve[1][i + 1];

			// Perform linear interpolation to calculate the battery percentage
			percentage = static_cast<uint8_t>(y0 + ((y1 - y0) * (batteryMilliVolts - x0)) / (x1 - x0));
			break;
		}
	}

	// Convert the percentage to a char array
	snprintf(batteryPercentage, sizeof(batteryPercentage), "%d%%", percentage);

	return batteryPercentage;
}

/**
 * @brief Populates the provided sdCard struct with information about the connected SD card.
 *
 * @param cardInfo Reference to the sdCard struct to populate.
 */
void populateSDCardInfo(sdCard& cardInfo) {
	if (cardInfo.connected == true) {
		uint8_t cardType = SD.cardType();
		cardInfo.cardTypeString = "";

		switch (cardType) {
			case CARD_NONE:
				cardInfo.cardTypeString = "No SD card attached";
				break;
			case CARD_MMC:
				cardInfo.cardTypeString = "MMC";
				break;
			case CARD_SD:
				cardInfo.cardTypeString = "SDSC";
				break;
			case CARD_SDHC:
				cardInfo.cardTypeString = "SDHC";
				break;
			default:
				cardInfo.cardTypeString = "UNKNOWN";
				break;
		}

		uint64_t cardSize = SD.totalBytes() / (1024 * 1024);
		cardInfo.cardTotalMib = static_cast<uint8_t>(cardSize);

		uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
		cardInfo.cardUsedMib = static_cast<uint8_t>(usedSize);

		ESP_LOGI("SD Card Info", "Type: %s", cardInfo.cardTypeString);
		ESP_LOGI("SD Card Info", "Total Size: %d MiB", cardInfo.cardTotalMib);
		ESP_LOGI("SD Card Info", "Used Size: %d MiB", cardInfo.cardUsedMib);
	} else {
		ESP_LOGW("SD Card", "Mount Failed!");
		cardInfo.connected = false;
	}
}

/**
 * @brief Writes a line of data to the SD card.
 */
void writeLineToSDcard() {
	// Configure pins
	configurePin(SPI_EN, OUTPUT, HIGH);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	// Initialize SD card
	if (SD.begin(SD_CARD_CS)) {
		ESP_LOGI("SD Card", "Connected");

		// Open file in append mode
		File file = SD.open(logFilePath, FILE_APPEND, true);
		if (!file) {
			ESP_LOGW("writeLineToSDcard", "Failed to open file");
			return;
		}

		// Check if the file is empty
		if (file.size() == 0) {
			char header[128];
			strcat(header, "Date, Time, Battery");

			// Iterate over each temperature sensor bus
			for (uint8_t i = 0; i < oneWirePortCount; i++) {
				if (oneWirePort[i].numberOfSensors > 0) {
					// Iterate over each sensor on the current bus
					for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
						// Convert the address to text
						char tempStr[10];
						snprintf(tempStr, 6, ",%s", deviceAddressto4Char(oneWirePort[i].sensorList[j].address));
						strcat(header, tempStr);
					}
				}
			}

			file.println(header);
		}

		// Get current timestamp
		struct tm timeInfo;
		time_t currentEpoch;
		time(&currentEpoch);
		char currentTimeStamp[32];
		localtime_r(&currentEpoch, &timeInfo);
		strftime(currentTimeStamp, sizeof(currentTimeStamp), "%e-%b-%Y,%H:%M", &timeInfo);

		char buf[128];
		snprintf(buf, sizeof(buf), "%s,%i", currentTimeStamp, batteryMilliVolts);

		for (uint8_t i = 0; i < oneWirePortCount; i++) {
			temperatureSensorBus& bus = oneWirePort[i];

			if (bus.numberOfSensors > 0) {
				for (uint8_t j = 0; j < bus.numberOfSensors; j++) {
					float temperature = bus.sensorList[j].temperature;

					// Append the temperature value to the string
					char tempStr[10];

					if (bus.sensorList[j].error == true) {
						snprintf(tempStr, sizeof(",ERR"), ",ERR");
					} else {
						snprintf(tempStr, sizeof(tempStr), ",%.2f", temperature);
					}
					strcat(buf, tempStr);
				}
			}
		}

		file.println(buf);
		file.close();

		ESP_LOGD("", "%s", buf);
	} else {
		ESP_LOGW("No SD Card", "");
	}
}

/**
 * @brief Updates the UI display.
 */
void updateScreen() {
	// Draw REC symbol if recording
	if (recording) {
		if (recordingDot) {
			// Draw filled circle and text for REC symbol
			screen.fillSmoothCircle(8 + 10, 8 + 10, 10, TFT_RED, TFT_BLACK);
			screen.setTextColor(TFT_WHITE, TFT_BLACK, true);
			screen.drawString("REC", 34, 8, 4);
		} else {
			// Clear the circle for REC symbol
			screen.fillCircle(8 + 10, 8 + 10, 12, TFT_BLACK);
		}
		recordingDot = !recordingDot;
	}

	screen.setTextColor(TFT_WHITE, TFT_BLACK, true);  // Set text color and background color

	// Draw Battery Percentage
	screen.drawString(calculateBatteryPercentage(batteryMilliVolts), 100, 8, 4);

	// Draw Temperature Values
	uint8_t yPosition = 36;

	for (uint8_t i = 0; i < oneWirePortCount; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			// Draw color bar to indicate the bus
			screen.drawWideLine(6, yPosition, 6, yPosition + (oneWirePort[i].numberOfSensors - 1) * 26 + 20, 5, oneWirePort[i].color, TFT_BLACK);

			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				// Draw device address and temperature values for each sensor
				screen.drawString(deviceAddressto4Char(oneWirePort[i].sensorList[j].address), 12, yPosition, 4);
				screen.drawFloat(oneWirePort[i].sensorList[j].temperature, 1, 12 + (4 * 20), yPosition, 4);
				screen.drawString("`c", 12 + (4 * 20) + 50, yPosition, 4);

				yPosition += 26;
			}

			yPosition += 13;
		}
	}

	screen.setTextColor(TFT_DARKGREY, TFT_BLACK, true);	 // Set text color and background color

	// Draw SD card information
	screen.setCursor(8, 285, 2);
	if (microSDCard.connected) {
		screen.printf("%i/%iMiB %s", microSDCard.cardUsedMib, microSDCard.cardTotalMib, microSDCard.cardTypeString);
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

/**
 * @brief Task that manages SPI communication and updates the screen periodically.
 *
 * @param parameter Task parameter (not used in this implementation).
 */
void SPIManagerTask(void* parameter) {
	// Configure pins
	configurePin(SPI_EN, OUTPUT, HIGH);
	pinMode(TFT_CS, OUTPUT);
	pinMode(SD_CARD_CS, OUTPUT);

	// Initialize screen
	screen.init();
	screen.setRotation(2);
	screen.fillScreen(TFT_BLACK);
	updateScreen();

	// Fade in the backlight gradually
	pinMode(BACKLIGHT, OUTPUT);
	for (int brightness = 0; brightness < 255; brightness++) {
		analogWrite(BACKLIGHT, brightness);
		vTaskDelay(5 / portTICK_PERIOD_MS);
	}

	// Initialize SD card
	if (SD.begin(SD_CARD_CS, screen.getSPIinstance(), SPI_FREQUENCY)) {
		microSDCard.connected = true;
		ESP_LOGI("SD Card", "Connected");
		populateSDCardInfo(microSDCard);

		// Initialize USB
		MSC.vendorID("Kea");		 // max 8 chars
		MSC.productID("Recorder");	 // max 16 chars
		MSC.productRevision("020");	 // max 4 chars
		MSC.onStartStop(onStartStop);
		MSC.onRead(onRead);
		MSC.onWrite(onWrite);
		MSC.mediaPresent(true);
		MSC.begin(SD.numSectors(), SD.cardSize() / SD.numSectors());
		USB.begin();
	} else {
		ESP_LOGW("No SD Card", "");
		microSDCard.connected = false;
	}

	while (true) {
		// Update the screen periodically
		updateScreen();
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}

	// This line will never be reached as the task runs in an infinite loop
	vTaskDelete(NULL);
}

/**
 * @brief Scans the OneWire buses, initializes DallasTemperature instances, and populates the temperature sensor information.
 *
 * This function scans the configured OneWire buses, sets up the DallasTemperature instances,
 * and populates the temperature sensor information including the device addresses and resolutions.
 *
 * @note This function modifies the `oneWirePort` array.
 */
void scanOneWireBusses() {
	DeviceAddress tempAddress;	// We'll use this variable to store a found device address

	oneWirePort[0].oneWirePin = (JST_IO_1_1);
	oneWirePort[1].oneWirePin = (JST_IO_2_1);
	oneWirePort[2].oneWirePin = (JST_IO_3_1);

	oneWirePort[0].color = TFT_RED;
	oneWirePort[1].color = TFT_GREEN;
	oneWirePort[2].color = TFT_BLUE;

	for (uint8_t i = 0; i < oneWirePortCount; i++) {
		temperatureSensorBus& bus = oneWirePort[i];

		bus.oneWireBus.begin(bus.oneWirePin);
		bus.dallasTemperatureBus.setOneWire(&bus.oneWireBus);			  // Sets up pointer to oneWire Instance
		bus.dallasTemperatureBus.begin();								  // Sets up and Scans the Bus
		bus.numberOfSensors = bus.dallasTemperatureBus.getDeviceCount();  // Grab a count of devices on the wire

		if (bus.numberOfSensors > 0) {
			for (uint8_t j = 0; j < bus.numberOfSensors; j++) {
				if (bus.dallasTemperatureBus.getAddress(tempAddress, j)) {
					memcpy(bus.sensorList[j].address, tempAddress, sizeof(DeviceAddress));
					bus.dallasTemperatureBus.setResolution(bus.sensorList[j].address, ONEWIRE_TEMP_RESOLUTION);
				}
			}
		}
	}
}

/**
 * @brief Reads temperatures from the OneWire temperature sensors.
 *
 * This function reads temperatures from the configured OneWire buses and populates the temperature
 * values in the temperatureSensorBus structure.
 *
 * @note This function assumes that the OneWire buses and DallasTemperature instances are already set up.
 */
void readOneWireTemperatures() {
	for (uint8_t i = 0; i < oneWirePortCount; i++) {
		temperatureSensorBus& bus = oneWirePort[i];

		if (bus.numberOfSensors > 0) {
			bus.oneWireBus.begin(bus.oneWirePin);
			//bus.dallasTemperatureBus.setOneWire(&bus.oneWireBus);  // Sets up pointer to oneWire Instance
			bus.dallasTemperatureBus.setWaitForConversion(false);
			bus.dallasTemperatureBus.requestTemperatures();
			//ESP_LOGD("requested Temperatures on", "bus: %i", i);
		}
	}

	vTaskDelay(oneWirePort[0].dallasTemperatureBus.millisToWaitForConversion(ONEWIRE_TEMP_RESOLUTION) / portTICK_PERIOD_MS);

	for (uint8_t i = 0; i < oneWirePortCount; i++) {
		temperatureSensorBus& bus = oneWirePort[i];

		if (bus.numberOfSensors > 0) {
			for (uint8_t j = 0; j < bus.numberOfSensors; j++) {
				float temperature = bus.dallasTemperatureBus.getTempC(bus.sensorList[j].address);

				bus.sensorList[j].temperature = temperature;

				//ESP_LOGD("got temp ", " %f", temperature);
				if (temperature == DEVICE_DISCONNECTED_C) {
					bus.sensorList[j].error = true;
				} else {
					bus.sensorList[j].error = false;
					bus.sensorList[j].temperature = temperature;
				}
			}
		}
	}
}

/**
 * @brief Task that periodically reads temperatures from OneWire sensors.
 *
 * @param parameter Task parameter (not used in this implementation).
 */
void readOneWireTemperaturesTask(void* parameter) {
	while (true) {
		if (!recording) {
			scanOneWireBusses();
		}
		readOneWireTemperatures();
	}

	// This line will never be reached as the task runs in an infinite loop
	vTaskDelete(NULL);
}

void setup() {
	Serial.begin(115200);

	uint64_t wakeupStatus = esp_sleep_get_ext1_wakeup_status();
	uint8_t wakeupPin = static_cast<uint8_t>(log2(wakeupStatus));
	batteryMilliVolts = analogReadMilliVolts(VBAT_SENSE) * VBAT_SENSE_SCALE;

	switch (wakeupPin) {
		case WIRE_RTC_INT:
			ESP_LOGV("Low Power Mode", "");
			ESP_LOGD("batteryMilliVolts", " %i", batteryMilliVolts);
			updateClock();
			readOneWireTemperatures();
			writeLineToSDcard();
			enterDeepSleep();
			break;

		case VUSB_SENSE:
			ESP_LOGV("USB Mode", "");

		case WAKE_BUTTON:
		case UP_BUTTON:
		case DOWN_BUTTON:
		default:

			ESP_LOGV("UI Mode", "");
			ESP_LOGD("batteryMilliVolts", " %i", batteryMilliVolts);

			xTaskCreate(SPIManagerTask, "SPIManagerTask", 100000, NULL, 3, NULL);

			xTaskCreate(buttonTask, "Button Task", 2048, NULL, 1, NULL);
			xTaskCreate(readOneWireTemperaturesTask, "readOneWireTemperaturesTask", 10000, NULL, 1, NULL);

			updateClock();

			pinMode(VUSB_SENSE, INPUT);

			while (millis() <= SCREEN_ON_TIME * 1000 || digitalRead(VUSB_SENSE) == HIGH) {
				vTaskDelay(500 / portTICK_PERIOD_MS);
			}

			// Fade out the backlight gradually
			for (int brightness = 255; brightness > 0; brightness--) {
				analogWrite(BACKLIGHT, brightness);
				vTaskDelay(5 / portTICK_PERIOD_MS);
			}

			enterDeepSleep();
			break;
	}
}

void loop() {
	// Empty loop, not needed
	vTaskSuspend(NULL);
}