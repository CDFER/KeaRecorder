#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SD.h>
#include <WiFi.h>
#include <tft_eSPI.h>

#include "USB.h"
#include "USBMSC.h"
#include "credentials.h"
#include "pcf8563.h"
#include "sntp.h"
#include "time.h"

#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#define WIFI_WIFI_SSID "YourWIFI"
#define WIFI_PW "PASSWORD"

#endif

// Constants
constexpr uint8_t SCREEN_ON_TIME = 30;
constexpr uint16_t HOLD_DURATION = 3000;
constexpr uint8_t ONEWIRE_TEMP_RESOLUTION = 10;

const uint8_t batterySmoothingFactor = 5;	   // Example: 10 represents 10% of new value
const float temperatureSmoothingFactor = 0.5;  // Smaller values for slower response, larger values for faster response with more noise

// const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";
const char* time_zone = "CST6CDT,M3.2.0,M11.1.0";
constexpr uint32_t DEEPSLEEP_INTERUPT_BITMASK = (1UL << WAKE_BUTTON) | (1UL << VUSB_SENSE);
constexpr uint32_t RTC_DEEPSLEEP_INTERUPT_BITMASK = DEEPSLEEP_INTERUPT_BITMASK | (1UL << WIRE_RTC_INT);

// Ultra Global Variables (stored even in deep sleep)
RTC_DATA_ATTR uint16_t batteryMilliVolts = 0;
RTC_DATA_ATTR bool recording = false;
RTC_DATA_ATTR uint8_t recordingIntervalMins = 15;
RTC_DATA_ATTR char logFilePath[64];
RTC_DATA_ATTR char serialNumber[3];

// Global Variables
TFT_eSPI screen;
PCF8563_Class rtc;
bool systemTimeValid = false;
bool recordingDot = true;
bool sensorsChanged = false;

SemaphoreHandle_t buttonSemaphore;

USBMSC MSC;
USBCDC USBSerial;

auto configurePin = [](int pin, int mode, int initialState) {
	pinMode(pin, mode);
	digitalWrite(pin, initialState);
};

// For this to work with Espressif's ESP32 code you need to change line 694 esp32 / hardware / eps32 / 2.0.3 / libraries / SD / src / sd_diskio.cpp

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
	return SD.writeRAW((uint8_t*)buffer, lba) ? bufsize : -1;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
	return SD.readRAW((uint8_t*)buffer, lba) ? bufsize : -1;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
	return true;
}

// Struct to hold information about a single temperature sensor
struct temperatureSensor {
	DeviceAddress address;
	float temperature;
	bool error;
};

// Struct to hold information about a temperature sensor bus
struct temperatureSensorBus {
	uint8_t numberOfSensors;
	uint8_t oneWirePin;
	OneWire oneWireBus;
	DallasTemperature dallasTemperatureBus;
	temperatureSensor sensorList[5];
	uint32_t color;
};

// Struct to hold information about the SD card
struct sdCard {
	uint16_t cardTotalMib;
	uint16_t cardUsedMib;
	const char* cardType;
	bool connected;
};

const uint8_t oneWirePortCount = 3;
RTC_DATA_ATTR temperatureSensorBus oneWirePort[oneWirePortCount];
RTC_DATA_ATTR sdCard microSDCard;

/**
 * @brief Extracts the first hex character from byte 1, 3, 5, and 7 of a DeviceAddress.
 *
 * This function takes a DeviceAddress, which is an array of bytes representing a device address,
 * and extracts the first hex character from bytes 1, 3, 5, and 7. The result is stored in a static
 * character array and returned. The result array is null-terminated.
 *
 * @param address The DeviceAddress from which to extract the hex characters.
 * @return A pointer to the static character array holding the extracted hex characters.
 */
char* deviceAddressTo4Char(const DeviceAddress& address) {
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
 * @brief Get the current date and time as a formatted string.
 *
 * @param format The desired format of the date and time string.
 * @return The current date and time as a formatted string.
 */
const char* getCurrentDateTime(const char* format) {
	static char dateTime[32];
	time_t currentEpoch;
	time(&currentEpoch);
	struct tm* timeInfo = localtime(&currentEpoch);
	strftime(dateTime, sizeof(dateTime), format, timeInfo);
	return dateTime;
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

	ESP_LOGI("Setting Recording Alarm", "Next alarm: %u, Current minute: %u", alarmMinute, currentTime.minute);
}

/**
 * @brief Clears the RTC alarm.
 *
 */
void clearAlarm() {
	rtc.disableAlarm();
	rtc.disableTimer();
	rtc.disableCLK();
}

/**
 * @brief Gets the serial number based on the MAC address
 */
void getSerialNumber() {
	uint8_t mac[6];
	WiFi.macAddress(mac);
	sprintf(serialNumber, "%02X", mac[5]);
}

/**
 * @brief Generates a filename based on the current timestamp and MAC address.
 * @return The generated filename as a null-terminated string.
 */
void generateFilename() {
	// Get the serial number
	getSerialNumber();

	// Build the filename with leading forward slash
	snprintf(logFilePath, sizeof(logFilePath), "/%s_%s.csv", getCurrentDateTime("%Y-%b-%e-%H%M"), serialNumber);  // Format: /2023-Jun-23-2041_C8.csv

	File file = SD.open(logFilePath, FILE_WRITE, true);
	if (!file) {
		ESP_LOGW("generateFilename", "Failed to open file");
		return;
	}

	char header[128] = "Date(YYYY-MM-DD),Time(HH:MM),Battery(mV)";

	// Iterate over each temperature sensor bus
	for (uint8_t portIndex = 0; portIndex < oneWirePortCount; portIndex++) {
		if (oneWirePort[portIndex].numberOfSensors > 0) {
			// Iterate over each sensor on the current bus
			for (uint8_t sensorIndex = 0; sensorIndex < oneWirePort[portIndex].numberOfSensors; sensorIndex++) {
				// Convert the address to text
				char tempStr[8];
				snprintf(tempStr, sizeof(tempStr), ",%s", deviceAddressTo4Char(oneWirePort[portIndex].sensorList[sensorIndex].address));
				strcat(header, tempStr);
			}
		}
	}

	file.println(header);
	ESP_LOGD("", "%s", header);

	file.close();
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
	// Initialize wake button pin and attach interrupt
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
						vTaskDelay(10000 / portTICK_PERIOD_MS);
					} else {
						recording = true;
						generateFilename();
						ESP_LOGI("Started New File", "%s", logFilePath);

						setupNextAlarm();
						vTaskDelay(10000 / portTICK_PERIOD_MS);
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
	if ((batteryMilliVolts > 3300) && recording) {
		// Enable deep sleep mode with RTC wakeup
		esp_sleep_enable_ext1_wakeup(RTC_DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
		ESP_LOGV("Enter DeepSleep", "Waiting For RTC or user input");
	} else {
		// Enable deep sleep mode with wakeup triggered by user input
		esp_sleep_enable_ext1_wakeup(DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
		ESP_LOGI("Enter DeepSleep", "Waiting for user input");
	}

	// Start the deep sleep process
	esp_deep_sleep_start();

	// code here will never be run...
}

/**
 * @brief Updates the clock based on the current system time or using NTP servers.
 *
 * This function initializes the RTC module and syncs it to the system time if recording is enabled.
 * If recording is disabled or the system time is not valid, the function syncs the RTC using NTP servers.
 * The function also sets up the next alarm if recording is enabled and the RTC interrupt pin is in the HIGH state.
 */
void updateClock() {
	Wire.begin(WIRE_SDA, WIRE_SCL, 100000);
	rtc.begin(Wire);

	// Sync RTC to the system time if available
	if (rtc.syncToSystem()) {
		setenv("TZ", time_zone, 1);
		tzset();
		systemTimeValid = true;
	} else {
		ESP_LOGE("Time", "NOT VALID");
		systemTimeValid = false;
	}

	pinMode(WIRE_RTC_INT, INPUT);

	if (recording) {
		// Setup the next alarm if the RTC interrupt pin is in the HIGH state
		if (digitalRead(WIRE_RTC_INT) == HIGH) {
			setupNextAlarm();
		}
	} else {
		if (digitalRead(WIRE_RTC_INT) == HIGH) {
			clearAlarm();
		}

		if (!systemTimeValid) {
			// Sync RTC using WIFI and NTP servers if system time is not valid

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

			// Wait until at least one NTP server is reachable
			while (!(sntp_getreachability(0) + sntp_getreachability(1) + sntp_getreachability(2) > 0)) {
				delay(10);
			}

			// Sync RTC to the real-time clock (RTC) of the ESP32
			rtc.syncToRtc();

			WiFi.disconnect();
		}
	}
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
	for (uint8_t index = 0; index < tableSize - 1; index++) {
		// Check if the battery voltage is within the current range
		if (batteryMilliVolts <= batteryDischargeCurve[0][index + 1]) {
			// Get the x and y values for interpolation
			uint16_t x0 = batteryDischargeCurve[0][index];
			uint16_t x1 = batteryDischargeCurve[0][index + 1];
			uint8_t y0 = batteryDischargeCurve[1][index];
			uint8_t y1 = batteryDischargeCurve[1][index + 1];

			// Perform linear interpolation to calculate the battery percentage
			percentage = static_cast<uint8_t>(y0 + ((y1 - y0) * (batteryMilliVolts - x0)) / (x1 - x0));
			break;
		}
	}

	// Convert the percentage to a char array
	snprintf(batteryPercentage, sizeof(batteryPercentage), "%u%%", percentage);

	return batteryPercentage;
}

/**
 * @brief Populates the provided sdCard struct with information about the connected SD card.
 *
 * @param cardInfo Reference to the sdCard struct to populate.
 */
void populateSDCardInfo(sdCard& cardInfo) {
	if (cardInfo.connected) {
		uint8_t cardType = SD.cardType();
		switch (cardType) {
			case CARD_NONE:
				cardInfo.cardType = "No SD card attached";
				break;
			case CARD_MMC:
				cardInfo.cardType = "MMC";
				break;
			case CARD_SD:
				cardInfo.cardType = "SDSC";
				break;
			case CARD_SDHC:
				cardInfo.cardType = "SDHC";
				break;
			default:
				cardInfo.cardType = "UNKNOWN";
				break;
		}

		cardInfo.cardTotalMib = static_cast<uint16_t>(SD.totalBytes() / (1024 * 1024));
		cardInfo.cardUsedMib = static_cast<uint16_t>(SD.usedBytes() / (1024 * 1024));

		// Log the SD card information
		ESP_LOGI("SD Card Info", "Type: %s, Total Size: %u MiB, Used Size: %u MiB",
				 cardInfo.cardType, cardInfo.cardTotalMib, cardInfo.cardUsedMib);

	} else {
		// If SD card mount failed, log a warning and set the connected flag to false
		ESP_LOGW("SD Card", "Mount Failed!");
		cardInfo.connected = false;
	}
}

/**
 * @brief Writes a line of data to the SD card.
 *
 * This function writes a line of data to the SD card, including timestamp, battery information,
 * and temperature readings from various sensors. It performs the following steps:
 *
 * - Initializes the SD card.
 * - Opens the log file in append mode.
 * - Checks if the file is empty and writes the header if needed.
 * - Retrieves the current timestamp.
 * - Formats the data line with timestamp, battery voltage, and temperature readings.
 * - Writes the data line to the log file.
 * - Closes the file.
 *
 * @note Before calling this function, make sure to set up the log file path.
 *
 * @note This function assumes the existence of global variables and data structures
 *       related to temperature sensors and battery information.
 *
 * @note Make sure to initialize the SD card library before calling this function.
 *
 */
void writeLineToSDcard() {
	// Configure pins
	configurePin(SPI_EN, OUTPUT, HIGH);
	configurePin(TFT_CS, OUTPUT, HIGH);
	configurePin(SD_CARD_CS, OUTPUT, HIGH);

	// Initialize SD card
	if (SD.begin(SD_CARD_CS)) {
		ESP_LOGI("SD Card", "Connected");

		// Open file in append mode
		File file = SD.open(logFilePath, FILE_APPEND, true);
		if (!file) {
			ESP_LOGW("writeLineToSDcard", "Failed to open file");
			return;
		}

		// Create a buffer to store the data line
		char dataLine[128];

		// Format the data line with current date and time, and battery voltage
		snprintf(dataLine, sizeof(dataLine), "%s,%u", getCurrentDateTime("%Y-%m-%d,%H:%M"), batteryMilliVolts);

		// Iterate through each temperature sensor bus
		for (uint8_t portIndex = 0; portIndex < oneWirePortCount; portIndex++) {
			temperatureSensorBus& bus = oneWirePort[portIndex];

			// Check if the current bus has sensors
			if (bus.numberOfSensors > 0) {
				// Iterate through each sensor on the current bus
				for (uint8_t sensorIndex = 0; sensorIndex < bus.numberOfSensors; sensorIndex++) {
					char tempStr[8];

					// Check if the current sensor has an error
					if (bus.sensorList[sensorIndex].error) {
						strcpy(tempStr, ",ERR");
					} else {
						// Get the temperature reading from ram
						float temperature = bus.sensorList[sensorIndex].temperature;
						snprintf(tempStr, sizeof(tempStr), ",%.1f", temperature);
					}

					// Append the temperature reading to the data line
					strcat(dataLine, tempStr);
				}
			}
		}

		// Write the data line to the csv file
		file.println(dataLine);
		file.close();

		// Log the data line
		ESP_LOGD("", "%s", dataLine);

	} else {
		ESP_LOGW("No SD Card", "");
	}
}

/**
 * @brief Updates the user interface (UI) display with the latest information.
 *
 * This function updates the UI display to reflect the current state of the system. It includes
 * updating the REC symbol if the system is in recording mode, displaying battery percentage,
 * temperature values for sensors, SD card information, and the current date and time.
 * The function utilizes specific positions and colors to ensure consistent and organized
 * presentation of the information on the screen.
 */
void updateScreen() {
	if (sensorsChanged) {
		screen.fillScreen(TFT_BLACK);
		sensorsChanged = false;
	}

	// Define positions for REC symbol
	const int REC_CIRCLE_X = 18;
	const int REC_CIRCLE_Y = 18;
	const int REC_TEXT_X = 34;
	const int REC_TEXT_Y = 8;

	// Update REC symbol if recording
	if (recording) {
		// Determine the circle color and text color
		uint16_t circleColor = TFT_RED;
		uint16_t backgroundColor = TFT_BLACK;

		// Update the circle and text based on the recordingDot value
		if (recordingDot) {
			// Draw filled circle for REC symbol
			screen.fillSmoothCircle(REC_CIRCLE_X, REC_CIRCLE_Y, 10, circleColor, backgroundColor);
		} else {
			// Clear the circle for REC symbol
			screen.fillCircle(REC_CIRCLE_X, REC_CIRCLE_Y, 12, backgroundColor);
		}

		// Draw the text for REC symbol
		screen.setTextColor(TFT_WHITE, backgroundColor, true);
		screen.drawString("REC", REC_TEXT_X, REC_TEXT_Y, 4);

		// Toggle the recordingDot value
		recordingDot = !recordingDot;
	} else {
		// Clear the circle for REC symbol
		screen.fillCircle(REC_CIRCLE_X, REC_CIRCLE_Y, 12, TFT_BLACK);
	}

	// Define positions for battery percentage and temperature values
	const int BATTERY_PERCENTAGE_X = 100;
	const int BATTERY_PERCENTAGE_Y = 8;
	const int TEMPERATURE_START_Y = 36;
	const int COLOR_BAR_X = 6;
	const int COLOR_BAR_WIDTH = 5;
	const int COLOR_BAR_SPACING = 22;
	const int DEVICE_ADDRESS_X = 12;
	const int TEMPERATURE_X = DEVICE_ADDRESS_X + (4 * 20);
	const int DEGREE_SYMBOL_X = TEMPERATURE_X + 50;

	// Set text color and background color for battery percentage and temperature values
	screen.setTextColor(TFT_WHITE, TFT_BLACK, true);

	// Draw Battery Percentage
	screen.drawString(calculateBatteryPercentage(batteryMilliVolts), BATTERY_PERCENTAGE_X, BATTERY_PERCENTAGE_Y, 4);

	// Draw Temperature Values
	uint8_t yPosition = TEMPERATURE_START_Y;

	for (uint8_t portIndex = 0; portIndex < oneWirePortCount; portIndex++) {
		uint8_t numberOfSensors = oneWirePort[portIndex].numberOfSensors;

		if (numberOfSensors > 0) {
			const uint16_t colorBarEndY = yPosition + (numberOfSensors - 1) * COLOR_BAR_SPACING + 20;
			screen.drawWideLine(COLOR_BAR_X, yPosition, COLOR_BAR_X, colorBarEndY, COLOR_BAR_WIDTH, oneWirePort[portIndex].color, TFT_BLACK);

			for (uint8_t sensorIndex = 0; sensorIndex < numberOfSensors; sensorIndex++) {
				screen.drawString(deviceAddressTo4Char(oneWirePort[portIndex].sensorList[sensorIndex].address), DEVICE_ADDRESS_X, yPosition, 4);
				screen.drawFloat(oneWirePort[portIndex].sensorList[sensorIndex].temperature, 1, TEMPERATURE_X, yPosition, 4);
				screen.drawString("`C", DEGREE_SYMBOL_X, yPosition, 4);

				yPosition += COLOR_BAR_SPACING;
			}

			yPosition += 13;
		}
	}

	// Define positions for SD card information and current date and time
	const int SD_CARD_INFO_X = 8;
	const int SD_CARD_INFO_Y = 285;
	const int DATE_TIME_X = 8;
	const int DATE_TIME_Y = 300;

	// Set text color and background color for SD card information
	screen.setTextColor(TFT_DARKGREY, TFT_BLACK, true);

	// Draw SD card and unit id information
	screen.setCursor(SD_CARD_INFO_X, SD_CARD_INFO_Y, 2);
	if (microSDCard.connected) {
		screen.printf("%u/%u MiB %s SN: %s", microSDCard.cardUsedMib, microSDCard.cardTotalMib, microSDCard.cardType, serialNumber);
	} else {
		screen.printf("No SD Card SN: %s", serialNumber);
	}

	// Draw current date and time
	screen.drawString(getCurrentDateTime("%e %b %Y %H:%M"), DATE_TIME_X, DATE_TIME_Y, 2);
}

/**
 * @brief Task that manages SPI communication and updates the screen periodically.
 *
 * This task handles the SPI communication and updates the screen periodically. It configures
 * the necessary pins, initializes the screen, sets up the SD card if available, and manages
 * the backlight. It also initializes the USB functionality for data transfer. The task runs
 * in an infinite loop, continuously updating the screen at a regular interval.
 *
 * @param parameter Task parameter (not used in this implementation).
 */
void SPIManagerTask(void* parameter) {
	// Configure pins
	configurePin(SPI_EN, OUTPUT, HIGH);
	configurePin(TFT_CS, OUTPUT, HIGH);
	configurePin(SD_CARD_CS, OUTPUT, HIGH);

	// Initialize screen
	screen.init();
	screen.setRotation(2);
	screen.fillScreen(TFT_BLACK);
	updateScreen();

	// Fade in the backlight gradually
	pinMode(BACKLIGHT, OUTPUT);
	for (uint8_t brightness = 0; brightness < 255; brightness++) {
		analogWrite(BACKLIGHT, brightness);
		vTaskDelay(5 / portTICK_PERIOD_MS);
	}

	// Initialize SD card
	if (SD.begin(SD_CARD_CS, screen.getSPIinstance(), SPI_FREQUENCY)) {
		microSDCard.connected = true;
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
		USBSerial.begin();
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
	DeviceAddress tempAddress;	// Variable to store a found device address

	for (uint8_t portIndex = 0; portIndex < oneWirePortCount; portIndex++) {
		temperatureSensorBus& bus = oneWirePort[portIndex];

		// Initialize the OneWire bus
		bus.oneWireBus.begin(bus.oneWirePin);
		bus.dallasTemperatureBus.setOneWire(&bus.oneWireBus);			  // Sets up pointer to OneWire instance
		bus.dallasTemperatureBus.begin();								  // Sets up and scans the bus
		uint8_t deviceCount = bus.dallasTemperatureBus.getDeviceCount();  // Get the count of devices on the bus
		// ESP_LOGD("deviceCount", "%u %u", bus.oneWirePin, deviceCount);

		if (deviceCount != bus.numberOfSensors) {
			bus.numberOfSensors = deviceCount;
			sensorsChanged = true;

			if (bus.numberOfSensors > 0) {
				// Populate device addresses and set resolution for each sensor
				for (uint8_t sensorIndex = 0; sensorIndex < bus.numberOfSensors; sensorIndex++) {
					if (bus.dallasTemperatureBus.getAddress(tempAddress, sensorIndex)) {
						memcpy(bus.sensorList[sensorIndex].address, tempAddress, sizeof(DeviceAddress));
						bus.dallasTemperatureBus.setResolution(bus.sensorList[sensorIndex].address, ONEWIRE_TEMP_RESOLUTION);
					}
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
	for (uint8_t portIndex = 0; portIndex < oneWirePortCount; ++portIndex) {
		temperatureSensorBus& bus = oneWirePort[portIndex];

		if (bus.numberOfSensors > 0) {
			bus.oneWireBus.begin(bus.oneWirePin);
			bus.dallasTemperatureBus.setWaitForConversion(false);
			bus.dallasTemperatureBus.requestTemperatures();
		}
	}

	vTaskDelay(oneWirePort[0].dallasTemperatureBus.millisToWaitForConversion(ONEWIRE_TEMP_RESOLUTION) / portTICK_PERIOD_MS);

	for (uint8_t portIndex = 0; portIndex < oneWirePortCount; ++portIndex) {
		temperatureSensorBus& bus = oneWirePort[portIndex];

		if (bus.numberOfSensors > 0) {
			for (uint8_t sensorIndex = 0; sensorIndex < bus.numberOfSensors; ++sensorIndex) {
				// Read the temperature
				float currentTemperature = bus.dallasTemperatureBus.getTempC(bus.sensorList[sensorIndex].address);

				// Check if the temperature is disconnected
				if (currentTemperature == DEVICE_DISCONNECTED_C) {
					bus.sensorList[sensorIndex].error = true;
				} else {
					// Apply exponential smoothing
					if (bus.sensorList[sensorIndex].error) {
						bus.sensorList[sensorIndex].temperature = currentTemperature;
					} else {
						bus.sensorList[sensorIndex].temperature = (temperatureSmoothingFactor * currentTemperature) + ((1 - temperatureSmoothingFactor) * bus.sensorList[sensorIndex].temperature);
					}

					bus.sensorList[sensorIndex].error = false;
				}
			}
		}
	}
}

void printTemperatures() {
	for (uint8_t portIndex = 0; portIndex < oneWirePortCount; portIndex++) {
		uint8_t numberOfSensors = oneWirePort[portIndex].numberOfSensors;

		if (numberOfSensors > 0) {
			for (uint8_t sensorIndex = 0; sensorIndex < numberOfSensors; sensorIndex++) {
				USBSerial.print(deviceAddressTo4Char(oneWirePort[portIndex].sensorList[sensorIndex].address));
				USBSerial.print(": ");
				USBSerial.print(oneWirePort[portIndex].sensorList[sensorIndex].temperature, 1);
				USBSerial.print("°C, ");
			}
		}
	}
	USBSerial.println("");
}

/**
 * @brief Task that periodically reads temperatures from OneWire sensors.
 *
 * @param parameter Task parameter (not used in this implementation).
 */
void readOneWireTemperaturesTask(void* parameter) {
	// Configure OneWire bus pins and colors
	oneWirePort[0].oneWirePin = JST_IO_1_1;
	oneWirePort[1].oneWirePin = JST_IO_2_1;
	oneWirePort[2].oneWirePin = JST_IO_3_1;

	oneWirePort[0].color = TFT_RED;
	oneWirePort[1].color = TFT_GREEN;
	oneWirePort[2].color = TFT_BLUE;

	while (true) {
		if (!recording) {
			scanOneWireBusses();
			printTemperatures();
		}
		readOneWireTemperatures();
	}

	// This line will never be reached as the task runs in an infinite loop
	vTaskDelete(NULL);
}

void readBatteryVoltage() {
	// Read the current battery millivolts
	uint16_t currentMilliVolts = static_cast<uint16_t>(analogReadMilliVolts(VBAT_SENSE) * VBAT_SENSE_SCALE);

	// Exponential smoothing calculation
	if (batteryMilliVolts == 0) {
		batteryMilliVolts = currentMilliVolts;
	} else {
		batteryMilliVolts = (batterySmoothingFactor * currentMilliVolts + (100 - batterySmoothingFactor) * batteryMilliVolts) / 100;
	}

	ESP_LOGD("Battery", "%umV", batteryMilliVolts);

	return;
}

void setup() {
	// Serial.begin(115200);

	// Get the wakeup status and determine the wakeup pin
	uint64_t wakeupStatus = esp_sleep_get_ext1_wakeup_status();
	uint8_t wakeupPin = static_cast<uint8_t>(log2(wakeupStatus));

	readBatteryVoltage();

	switch (wakeupPin) {
		case WIRE_RTC_INT:
			// Low Power Mode
			ESP_LOGV("Low Power Mode", "");
			updateClock();
			readOneWireTemperatures();
			writeLineToSDcard();
			enterDeepSleep();
			break;

		case VUSB_SENSE:
		case WAKE_BUTTON:
		case UP_BUTTON:
		case DOWN_BUTTON:
		default:
			// UI Mode
			ESP_LOGV("UI Mode", "");

			pinMode(VUSB_SENSE, INPUT);

			if (digitalRead(VUSB_SENSE) == HIGH) {
				// USB Mode
				ESP_LOGV("USB Mode", "");
				setCpuFrequencyMhz(240);  // Set CPU frequency to boost when needed
			}

			// Create tasks
			xTaskCreate(SPIManagerTask, "SPIManagerTask", 100000, NULL, 2, NULL);
			xTaskCreate(buttonTask, "Button Task", 4000, NULL, 1, NULL);
			xTaskCreate(readOneWireTemperaturesTask, "readOneWireTemperaturesTask", 10000, NULL, 1, NULL);

			updateClock();
			getSerialNumber();

			while (millis() < (SCREEN_ON_TIME * 1000) || digitalRead(VUSB_SENSE) == HIGH) {
				vTaskDelay(5000 / portTICK_PERIOD_MS);
				readBatteryVoltage();
			}

			// Fade out the backlight gradually
			for (uint8_t brightness = 255; brightness > 0; brightness--) {
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