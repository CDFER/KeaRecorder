# KeaRecorder

[![Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL](https://img.shields.io/static/v1?label=Hippocratic%20License&message=HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL&labelColor=5e2751&color=bc8c3d)](https://firstdonoharm.dev/version/3/0/cl-extr-ffd-media-mil-my-sv-tal.html)

Welcome to KeaRecorder! This device allows you to accurately record ground water temperature using the ESP32S2 microcontroller.
## Table of Contents

- [KeaRecorder](#kearecorder)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Main Hardware Components](#main-hardware-components)
  - [Usage](#usage)
  - [Installation](#installation)
  - [Configuration](#configuration)
  - [Contributing](#contributing)
  - [Other](#other)
  - [License](#license)

## Overview

KeaRecorder is an designed specifically for recording and monitoring ground water temperature. Here are some key features of KeaRecorder:

- Backup clock: The built-in backup clock ensures accurate timekeeping, even when far away from civilization, guaranteeing reliable temperature data.
- Timezone support: Customize the time display and recordings based on your location, allowing seamless integration into your local time zone.
- Wi-Fi time synchronization: KeaRecorder synchronizes its time through a Wi-Fi network time server, providing accurate time information for precise temperature monitoring.
- Low power consumption: With its energy-efficient design, KeaRecorder can operate for extended periods without draining the battery.
- Easy access to data: KeaRecorder emulates a USB drive through its USB-C port, simplifying access to temperature data stored on the SD card.

## Main Hardware Components

KeaRecorder utilizes high-quality hardware components to ensure optimal performance and reliability. Here are the main hardware components used:

- SLM6500 Lithium ion BMS: This Battery Management System ensures safe and efficient operation, managing power effectively.
- ESP32S2: The ESP32S2 microcontroller provides the necessary processing power and connectivity options, serving as the brain of KeaRecorder.
- MicroSD card (SPI): The SD card securely stores the recorded temperature data, offering ample storage capacity for monitoring needs.
- ST7789V3 IPS Display (SPI): The vibrant display module shows real-time temperature readings from the sensors in vivid detail.
- PCF8563 RTC (I2C): The Real-Time Clock (RTC) module enables precise timekeeping, ensuring accurate temperature records.
- 3 OneWire Busses with up to 5 DS18B20 Temperature Sensors on each: These temperature sensors allow monitoring of multiple areas or depths with ease and accuracy.

## Usage

Using KeaRecorder is easy and intuitive. Follow these steps to get started:

1. Power on the KeaRecorder unit by pressing the button to activate the display.
2. The display will show real-time temperature readings obtained from the sensors, providing instant insights into ground water temperature.
3. Press and hold the button to toggle the recording mode. This enables or disables the logging of temperature readings to the SD card at regular intervals, according to your needs.
4. Once recording is enabled, KeaRecorder will diligently capture and log temperature data, ensuring comprehensive records of ground water temperature fluctuations.
5. To access the recorded temperature data, either remove the SD card from KeaRecorder and insert it into a computer or connect KeaRecorder to a computer using a USB cable.

![](/images/IMG_20230608_104536.jpg)
![](/images/IMG_20230617_150623.jpg)
![](/images/IMG_20230622_151534.jpg)

## Installation

Installing KeaRecorder is a straightforward process. Just follow these steps:

1. Clone the repository to your local machine to access the latest version of the code.
2. If you're using PlatformIO, the required libraries will be installed automatically. If not, make sure to manually install the necessary libraries.
3. Compile the code and upload it to the ESP32 device.

## Configuration

You can customize KeaRecorder using the following configuration options:

- WiFi Credentials: Update the `main.cpp` file with your WiFi network name and password to seamlessly integrate KeaRecorder into your existing network.
- Recording Interval: Adjust the `recordingIntervalMins` variable to set the desired interval for recording temperature readings. This allows you to tailor the device's logging frequency to your specific monitoring requirements.
- Time Zone: Modify the `time_zone` variable to establish the desired time zone, ensuring accurate time display and recording based on your location.

## Contributing

We welcome contributions from the community! Here's how you can contribute to the project's ongoing development:

1. Fork the repository to create a personal copy of the project.
2. Create a new branch dedicated to your feature or bug fix, ensuring focused and organized contributions.
3. Implement the necessary changes and commit your code, including clear and concise documentation.
4. Push the changes to your forked repository, making them available for further review and integration.
5. Submit a pull request to the main repository, allowing your contributions to be considered for inclusion in the project.

## Other

Special thanks to the exceptional team at @Espressif Systems for their remarkable work in developing this outstanding chip and seamlessly porting it to Arduino.

Made with passion and dedication by Chris Dirks (@cd_fer) in Aotearoa New Zealand.

## License

KeaRecorder is proudly licensed under the Hippocratic License HL3-CL-EXTR-FFD-MEDIA-MIL-MY-SV-TAL. For detailed licensing terms, please review the license file provided.

Feel free to reach out if you have any questions or need further assistance. Happy monitoring with KeaRecorder!