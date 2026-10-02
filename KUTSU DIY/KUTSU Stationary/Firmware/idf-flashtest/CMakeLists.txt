# Minimal ESP-IDF project — flash-test for the KUTSU Sense ESP32-C5-DevKitC-1.
# Purpose: confirm Espressif IDE can build + flash this board over USB before
# committing to a full firmware port. Blinks the 3 status LEDs (GPIO5/26/0).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kutsu_flashtest)
