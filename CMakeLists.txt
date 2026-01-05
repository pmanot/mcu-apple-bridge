# USB NCM Server for ESP32-S3
# Ethernet-over-USB with DHCP server for iPhone/macOS connectivity

cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_ncm_server)
