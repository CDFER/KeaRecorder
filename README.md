# KeaRecorder

[![Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL&labelColor=5e2751&color=bc8c3d)](https://firstdonoharm.dev/version/3/0/cl-extr-ffd-media-mil-my-sv-tal.html)

KeaRecorder is a powerful ESP32S2-based device created to accurately record ground water temperature to an SD card. It combines advanced features and carefully selected hardware components to provide a reliable and efficient solution for temperature monitoring. This document offers an in-depth overview of the project, highlights the main hardware components, provides installation instructions, explains how to use the device effectively, offers configuration options, outlines guidelines for contributing to the project, and provides licensing information.

## Table of Contents

- [KeaRecorder](#kearecorder)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Main Hardware Components](#main-hardware-components)
  - [Installation](#installation)
  - [Usage](#usage)
  - [Configuration](#configuration)
  - [Contributing](#contributing)
  - [Other](#other)
  - [License](#license)

## Overview

KeaRecorder is an innovative ESP32S2-based device designed specifically for recording and monitoring ground water temperature. It offers a range of powerful features:

- Backup clock: The built-in backup clock ensures accurate timekeeping, even during power outages, guaranteeing reliable temperature data.
- Timezone support: Customize the time display and recordings based on your location, allowing seamless integration into your local time zone.
- Wi-Fi time synchronization: KeaRecorder synchronizes its time through a Wi-Fi network server, providing accurate time information for precise temperature monitoring.
- Low power consumption: With its deepsleep current of only 30uA, KeaRecorder is energy-efficient and can operate for extended periods without draining the battery.
- USB drive emulation: KeaRecorder emulates a USB drive through its USB-C port, simplifying access to temperature data stored on the SD card.

## Main Hardware Components

KeaRecorder utilizes high-quality hardware components to ensure optimal performance and reliability:

- SLM6500 Lithium ion BMS: The Battery Management System ensures safe and efficient operation, managing power effectively.
- ESP32S2: The ESP32S2 microcontroller provides the necessary processing power and connectivity options, serving as the brain of KeaRecorder.
- MicroSD card (SPI): The SD card securely stores the recorded temperature data, offering ample storage capacity for monitoring needs.
- ST7789V3 IPS Display (SPI): The vibrant display module shows real-time temperature readings from the sensors in vivid detail.
- PCF8563 RTC (I2C): The Real-Time Clock (RTC) module enables precise timekeeping, ensuring accurate temperature records.
- 3 OneWire Busses with up to 5 DS18B20 Temperature Sensors on each: These temperature sensors allow monitoring of multiple areas or depths with ease and accuracy.

## Installation

To install KeaRecorder, follow these steps:

1. Clone the repository to your local machine to access the latest version.
2. Install the required libraries automatically if using PlatformIO. Otherwise, manually install the necessary libraries.
3. Compile the code and upload it to the ESP32 device.

## Usage

To use KeaRecorder, follow these steps:

1. Power on the KeaRecorder unit by pressing the

 button to activate the temperature recording functionality.
2. The display shows real-time temperature readings obtained from the sensors, providing instant insights into ground water temperature.
3. Press and hold the button to toggle the recording mode, enabling or disabling the logging of temperature readings to the SD card at regular intervals, according to your needs.
4. Once recording is enabled, KeaRecorder diligently captures and logs temperature data, ensuring comprehensive records of ground water temperature fluctuations.
5. Access the recorded temperature data by safely removing the SD card from KeaRecorder and inserting it into a computer or compatible device.

## Configuration

Customize KeaRecorder using the following configuration options:

- WiFi Credentials: Update the `main.cpp` file with your WiFi network name and password for seamless integration into your existing network.
- Recording Interval: Adjust the `recordingIntervalMins` variable to set the desired interval for recording temperature readings, tailoring the device's logging frequency to your specific monitoring requirements.
- Time Zone: Modify the `time_zone` variable to establish the desired time zone, ensuring accurate time display and recording based on your location.

## Contributing

Join the KeaRecorder community and contribute to the project's ongoing development:

1. Fork the repository to create a personal copy of the project.
2. Create a new branch dedicated to your feature or bug fix, ensuring focused and organized contributions.
3. Implement the necessary changes and commit your code, including clear and concise documentation.
4. Push the changes to your forked repository, making them available for further review and integration.
5. Submit a pull request to the main repository, allowing your contributions to be considered for inclusion in the project.

## Other

Special thanks to the exceptional team at @Espressif Systems for their remarkable work in developing this outstanding chip and seamlessly porting it to Arduino.

Made with passion and dedication by Chris Dirks (@cd_fer) in the breathtaking landscape of Aotearoa New Zealand.

## License

KeaRecorder is proudly licensed under the Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL. For detailed licensing terms, please review the license file provided.