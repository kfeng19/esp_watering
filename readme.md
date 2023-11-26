ESP Watering
===============
A smart watering system built with ESP32 and ESP-IDF framework. 

Dependencies
----------
Apart from the IDF components listed in idf_component.yml, this project also depends on two custom libraries:
- [LED module](https://github.com/kfeng19/esp-led)
- [Moisture sensor module](https://github.com/kfeng19/moisture)

Usage
---
Please choose the correct target chip and run menuconfig before building the project. Some of the necessary configurations include:
- Flash size
- Partition table
- Wifi info

Once flash is done, measurement can be controlled by visiting esp-home.local (enabled by the mdns function).