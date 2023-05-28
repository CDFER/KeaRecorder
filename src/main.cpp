#include <Arduino.h>

// #include <SPI.h>
#include "USB.h"
USBCDC USBSerial;

#include <SD.h>

#include <tft_eSPI.h>
TFT_eSPI screen = TFT_eSPI();

#include "pcf8563.h"  // pcf8563 (Backup Clock)

#define DEEPSLEEP_INTERUPT_BITMASK pow(2, WAKE_BUTTON) + pow(2, UP_BUTTON) + pow(2, DOWN_BUTTON)




RTC_DATA_ATTR int bootCount = 0;

void print_wakeup_reason() {
	esp_sleep_wakeup_cause_t wakeup_reason;

	wakeup_reason = esp_sleep_get_wakeup_cause();

	int wakeup_pin = esp_sleep_get_ext1_wakeup_status();

	switch (wakeup_reason) {
		case ESP_SLEEP_WAKEUP_EXT0:
			screen.println("ULP_GPIO");
			break;
		case ESP_SLEEP_WAKEUP_EXT1:
			screen.print("ULP_GPIO_");
			screen.println((log(wakeup_pin)) / log(2), 0);
			break;
		case ESP_SLEEP_WAKEUP_TIMER:
			screen.println("ULP timer");
			break;
		case ESP_SLEEP_WAKEUP_TOUCHPAD:
			screen.println("ULP touchpad");
			break;
		case ESP_SLEEP_WAKEUP_ULP:
			screen.println("ULP program");
			break;
		default:
			screen.println("External Reset");
			break;
	}
}

void IRAM_ATTR ISR() {
	while (digitalRead(12) == 1)
	{
		vTaskDelay(1);
	}
	
	digitalWrite(SPI_EN, LOW);

	esp_sleep_enable_ext1_wakeup(DEEPSLEEP_INTERUPT_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
	esp_sleep_enable_timer_wakeup(5 * 1000000ULL);
	esp_deep_sleep_start();
}

void setup() {
	USBSerial.begin(115200);
	USBSerial.setDebugOutput(true);

	USBSerial.println("Kea Recorder 0.1.0");

	++bootCount;


	pinMode(SPI_EN, OUTPUT);
	digitalWrite(SPI_EN, HIGH);	 // 3V3_SPI_EN

	screen.begin();
	screen.setRotation(1);
	screen.fillScreen(TFT_BLACK);
	screen.setTextColor(TFT_WHITE);
	screen.setTextFont(4);
	screen.println("   Kea Recorder ");
	screen.println("Reboot: " + String(bootCount));
	print_wakeup_reason();

	pinMode(BACKLIGHT, OUTPUT);
	analogWrite(BACKLIGHT, 64);

	analogSetPinAttenuation(VBAT_SENSE, ADC_0db);  // 0db (0 mV ~ 750 mV)
	delay(300);
	screen.println("Battery: " + String(analogReadMilliVolts(VBAT_SENSE) * VBAT_SENSE_SCALE) + "mV");

	// digitalWrite(screen_CS,LOW);
	// delay(300);
	// if (!SD.begin()) {
	// 	screen.println("Card Mount Failed");
	// 	return;
	// }
	// uint8_t cardType = SD.cardType();

	// if (cardType == CARD_NONE) {
	// 	screen.println("No SD card attached");
	// 	return;
	// }

	// screen.print("SD Card Type: ");
	// if (cardType == CARD_MMC) {
	// 	screen.println("MMC");
	// } else if (cardType == CARD_SD) {
	// 	screen.println("SDSC");
	// } else if (cardType == CARD_SDHC) {
	// 	screen.println("SDHC");
	// } else {
	// 	screen.println("UNKNOWN");
	// }

	// uint64_t cardSize = SD.cardSize() / (1024 * 1024);
	// screen.printf("%lluMB\n", cardSize);

	// sensors.begin();

	// // Grab a count of devices on the wire
	// int numberOfDevices = sensors.getDeviceCount();

	// DeviceAddress tempDeviceAddress; // We'll use this variable to store a found device address

	// // locate devices on the bus
	// Serial.print("Locating devices...");

	// Serial.print("Found ");
	// Serial.print(numberOfDevices, DEC);
	// Serial.println(" devices.");

	// // report parasite power requirements
	// Serial.print("Parasite power is: ");
	// if (sensors.isParasitePowerMode()) Serial.println("ON");
	// else Serial.println("OFF");

	// // Loop through each device, print out address
	// for (int i = 0; i < numberOfDevices; i++)
	// {
	// 	// Search the wire for address
	// 	if (sensors.getAddress(tempDeviceAddress, i))
	// 	{
	// 		Serial.print("Found device ");

	// 		Serial.print("Setting resolution to ");
	// 		Serial.println(TEMPERATURE_PRECISION, DEC);

	// 		// set the resolution to TEMPERATURE_PRECISION bit (Each Dallas/Maxim device is capable of several different resolutions)
	// 		sensors.setResolution(tempDeviceAddress, TEMPERATURE_PRECISION);
	// 	} else {
	// 		Serial.print("Found ghost device at ");
	// 		Serial.print(i, DEC);
	// 		Serial.print(" but could not detect address. Check power and cabling");
	// 	}
	// }
	pinMode(12, INPUT);
	attachInterrupt(12, ISR, HIGH);
}

void loop() {

	// // call sensors.requestTemperatures() to issue a global temperature
	// // request to all devices on the bus
	// sensors.requestTemperatures(); // Send the command to get temperatures

	// char buf[32];  // temp char array
	// sprintf(buf, "%2.1f\n%2.1f\n%2.1f", sensors.getTempCByIndex(0), sensors.getTempCByIndex(1), sensors.getTempCByIndex(2));

	// //Serial.println(millis());

	// // Set "cursor" at top left corner of display (0,0) and select font (must include in platformio.ini)
	// // (cursor will move to next line automatically during printing with 'screen.println'
	// //  or stay on the line is there is room for the text with screen.print)
	// screen.setCursor(0, 5, 6);

	// // Set the font colour to be green with black background, set to font 2
	// screen.setTextColor(screen_BLACK, screen_BLACK);
	// screen.setTextFont(4);
	// // screen.println(buf);
	// screen.println("Test...");

	vTaskDelay(100);

	// delay(1000);
}