#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SD.h>
#include <tft_eSPI.h>

#include "USB.h"
#if ARDUINO_USB_CDC_ON_BOOT
#define HWSerial Serial0
#define USBSerial Serial
#else
#define HWSerial Serial
USBCDC USBSerial;
#endif

#include <WiFi.h>

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
constexpr uint8_t ONEWIRE_TEMP_RESOLUTION = 9;

const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";
const char* time_format = "%d/%m/%y,%H:%M:%S";
constexpr uint32_t DEEPSLEEP_INTERUPT_BITMASK = (1UL << WAKE_BUTTON) | (1UL << VUSB_SENSE);
constexpr uint32_t RTC_DEEPSLEEP_INTERUPT_BITMASK = DEEPSLEEP_INTERUPT_BITMASK | (1UL << WIRE_RTC_INT);

// Ultra Global Variables (stored even in deep sleep)
RTC_DATA_ATTR uint32_t batteryMilliVolts = 0;
RTC_DATA_ATTR bool recording = false;
RTC_DATA_ATTR uint8_t recordingIntervalMins = 2;

// Global Variables
TFT_eSPI screen;
PCF8563_Class rtc;
bool systemTimeValid = false;
bool recordingDot = true;

SemaphoreHandle_t buttonSemaphore;

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
	uint8_t cardTotalGib;
	uint8_t cardUsedGib;
	const char* cardTypeString = "";
	bool connected = false;
};

// Define the array with compile-time pin and color values
constexpr std::array<uint8_t, 3> pins = {JST_IO_1_1, JST_IO_2_1, JST_IO_3_1};
constexpr std::array<uint32_t, 3> colors = {TFT_RED, TFT_GREEN, TFT_BLUE};

// Define the array with compile-time pin and color values
RTC_DATA_ATTR temperatureSensorBus oneWirePort[pins.size()] = {};

const uint8_t oneWirePortCount = sizeof(oneWirePort) / sizeof(oneWirePort[0]);

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
	if (SD.begin(SD_CARD_CS)) {
		uint8_t cardType = SD.cardType();
		cardInfo.connected = true;
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

		uint64_t cardSize = SD.totalBytes() / (1024 * 1024 * 1024);
		cardInfo.cardTotalGib = static_cast<uint8_t>(cardSize);

		uint64_t usedSize = SD.usedBytes() / (1024 * 1024 * 1024);
		cardInfo.cardUsedGib = static_cast<uint8_t>(usedSize);

		SD.end();

		ESP_LOGI("SD Card Info", "Type: %s", cardInfo.cardTypeString);
		ESP_LOGI("SD Card Info", "Total Size: %d GiB", cardInfo.cardTotalGib);
		ESP_LOGI("SD Card Info", "Used Size: %d GiB", cardInfo.cardUsedGib);
	} else {
		ESP_LOGW("SD Card", "Mount Failed!");
		cardInfo.connected = false;
	}
}

/**
 * @brief Writes a line of data to the SD card.
 */
void writeLineToSDcard() {
	// Set SPI_EN pin as output and enable SPI
	pinMode(SPI_EN, OUTPUT);
	digitalWrite(SPI_EN, HIGH);

	// Get current timestamp
	struct tm timeInfo;
	time_t currentEpoch;
	time(&currentEpoch);
	char currentTimeStamp[32];
	localtime_r(&currentEpoch, &timeInfo);
	strftime(currentTimeStamp, sizeof(currentTimeStamp), time_format, &timeInfo);

	char buf[128];
	snprintf(buf, sizeof(buf), "%s,%i", currentTimeStamp, batteryMilliVolts);

	// Initialize SD card
	SD.begin(SD_CARD_CS);

	// Open file in append mode
	File file = SD.open("/timeTest.csv", FILE_APPEND);
	if (!file) {
		ESP_LOGW("writeLineToSDcard", "Failed to open file");
		return;
	}

	// Check if the file is empty
	if (file.size() == 0) {
		String header = "currentTimeStamp, batteryMilliVolts";

		// Iterate over each temperature sensor bus
		for (uint8_t i = 0; i < oneWirePortCount; i++) {
			if (oneWirePort[i].numberOfSensors > 0) {
				// Iterate over each sensor on the current bus
				for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
					// Convert the address to a string
					header += deviceAddressto4Char(oneWirePort[i].sensorList[j].address);
				}
			}
		}

		file.println(header);
	}

	// Iterate over each temperature sensor bus
	for (uint8_t i = 0; i < oneWirePortCount; i++) {
		if (oneWirePort[i].numberOfSensors > 0) {
			// Iterate over each sensor on the current bus
			for (uint8_t j = 0; j < oneWirePort[i].numberOfSensors; j++) {
				// Read the temperature for the current sensor
				float temperature = oneWirePort[i].dallasTemperatureBus.getTempC(oneWirePort[i].sensorList[j].address);

				// Update the temperature value in the sensor list
				oneWirePort[i].sensorList[j].temperature = temperature;

				// Append the temperature value to the string
				char tempStr[10];
				snprintf(tempStr, sizeof(tempStr), ",%.2f", temperature);
				strcat(buf, tempStr);
			}
		}
	}

	file.println(buf);
	file.close();

	ESP_LOGD("writeLineToSDcard", "Data written to SD card: %s", buf);
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
	screen.drawString(calculateBatteryPercentage(batteryMilliVolts), 112, 8, 4);

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
		screen.printf("%i/%iGiB %s", microSDCard.cardUsedGib, microSDCard.cardTotalGib, microSDCard.cardTypeString);
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
	configurePin(SPI_EN, OUTPUT, HIGH);
	configurePin(TFT_CS, OUTPUT, LOW);
	configurePin(SD_CARD_CS, OUTPUT, LOW);

	// Populate SD card information
	populateSDCardInfo(microSDCard);

	// Initialize TFT screen
	screen.begin();
	screen.setRotation(2);
	screen.fillScreen(TFT_BLACK);

	// Set backlight control pin
	pinMode(BACKLIGHT, OUTPUT);

	// Fade in the backlight gradually
	for (int brightness = 0; brightness <= 256; brightness += 8) {
		analogWrite(BACKLIGHT, brightness);
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}

	while (true) {
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
 * It is assumed that the `oneWirePort` array is pre-configured with the correct pin assignments and colors.
 *
 * @note This function modifies the `oneWirePort` array.
 */
void scanOneWireBusses() {
	DeviceAddress tempAddress;	// We'll use this variable to store a found device address

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
			bus.dallasTemperatureBus.setWaitForConversion(false);
			bus.dallasTemperatureBus.requestTemperatures();
			// ESP_LOGD("requested Temperatures on", "bus: %i", i);
		}
	}

	vTaskDelay(oneWirePort[0].dallasTemperatureBus.millisToWaitForConversion(ONEWIRE_TEMP_RESOLUTION) / portTICK_PERIOD_MS);

	for (uint8_t i = 0; i < oneWirePortCount; i++) {
		temperatureSensorBus& bus = oneWirePort[i];

		if (bus.numberOfSensors > 0) {
			for (uint8_t j = 0; j < bus.numberOfSensors; j++) {
				bus.sensorList[j].temperature = bus.dallasTemperatureBus.getTempC(bus.sensorList[j].address);
				// ESP_LOGD("got", " %.2f for bus %i sensor %i", bus.sensorList[j].temperature, i, j);
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
	uint64_t wakeupStatus = esp_sleep_get_ext1_wakeup_status();
	uint8_t wakeupPin = static_cast<uint8_t>(log2(wakeupStatus));
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
			// Fall-through to execute the default code as well

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
	// Empty loop, not needed
	vTaskSuspend(NULL);
}