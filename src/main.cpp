#include <Arduino.h>

// #include <SPI.h>
#include "USB.h"
USBCDC USBSerial;
// #include <FS.h>
#include <SD.h>

#include "pcf8563.h"  // pcf8563 (Backup Clock)

#define WIRE_SDA_PIN 8
#define WIRE_SCL_PIN 9

// #include <OneWire.h>
// #include <DallasTemperature.h>

// // Data wire is plugged into port 2 on the Arduino
// #define ONE_WIRE_BUS 27
// #define TEMPERATURE_PRECISION 11

// // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
// OneWire oneWire(ONE_WIRE_BUS);

// // Pass our oneWire reference to Dallas Temperature.
// DallasTemperature sensors(&oneWire);

// arrays to hold device addresses
// DeviceAddress insideThermometer, outsideThermometer;

// Assign address manually. The addresses below will need to be changed
// to valid device addresses on your bus. Device address can be retrieved
// by using either oneWire.search(deviceAddress) or individually via
// sensors.getAddress(deviceAddress, index)
// DeviceAddress insideThermometer = { 0x28, 0x1D, 0x39, 0x31, 0x2, 0x0, 0x0, 0xF0 };
// DeviceAddress outsideThermometer   = { 0x28, 0x3F, 0x1C, 0x31, 0x2, 0x0, 0x0, 0x2 };

#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();

void setup() {
	USBSerial.begin(115200);
	// while (!USBSerial)
	// 	;
	USBSerial.setDebugOutput(true);

	USBSerial.println("DallasTemp0.1.0");

	// digitalWrite(TFT_CS, HIGH); // TFT screen chip select
	// digitalWrite(5, HIGH); // SD card chips select, must use GPIO 5 (ESP32 SS)

	pinMode(38, OUTPUT);
	digitalWrite(38, LOW);	 // 3V3_SPI_EN

	delay(500);

	if (!SD.begin()) {
		USBSerial.println("Card Mount Failed");
		return;
	}
	uint8_t cardType = SD.cardType();

	if (cardType == CARD_NONE) {
		USBSerial.println("No SD card attached");
		return;
	}

	USBSerial.print("SD Card Type: ");
	if (cardType == CARD_MMC) {
		USBSerial.println("MMC");
	} else if (cardType == CARD_SD) {
		USBSerial.println("SDSC");
	} else if (cardType == CARD_SDHC) {
		USBSerial.println("SDHC");
	} else {
		USBSerial.println("UNKNOWN");
	}

	uint64_t cardSize = SD.cardSize() / (1024 * 1024);
	Serial.printf("SD Card Size: %lluMB\n", cardSize);

	PCF8563_Class rtc;
	const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// Time zone (see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)

	Wire.begin(WIRE_SDA_PIN, WIRE_SCL_PIN, 100000);

	rtc.begin(Wire);

	if (rtc.syncToSystem() == true) {
		setenv("TZ", time_zone, 1);
		tzset();
	}

	// pinMode(13,OUTPUT);
	// analogWrite(13,1);

	tft.begin();
	tft.setRotation(2);
	tft.fillScreen(TFT_GREEN);

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
}

void loop() {
	// // call sensors.requestTemperatures() to issue a global temperature
	// // request to all devices on the bus
	// sensors.requestTemperatures(); // Send the command to get temperatures

	// char buf[32];  // temp char array
	// sprintf(buf, "%2.1f\n%2.1f\n%2.1f", sensors.getTempCByIndex(0), sensors.getTempCByIndex(1), sensors.getTempCByIndex(2));

	// //Serial.println(millis());

	// // Set "cursor" at top left corner of display (0,0) and select font (must include in platformio.ini)
	// // (cursor will move to next line automatically during printing with 'tft.println'
	// //  or stay on the line is there is room for the text with tft.print)
	// tft.setCursor(0, 5, 6);

	// Set the font colour to be green with black background, set to font 2
	tft.setTextColor(TFT_BLACK, TFT_BLACK);
	tft.setTextFont(4);
	// tft.println(buf);
	tft.println("USB Upload :)");

	// vTaskDelay(100);

	delay(1000);

	char string[16];
	uint32_t f;

	f = getCpuFrequencyMhz();
	sprintf(string, "CPU Freq: %i", f);
	Serial.println(string);
	// Serial.println("YYY");
}