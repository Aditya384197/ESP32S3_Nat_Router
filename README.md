# ESP32S3_Nat_Router

## Build baseline

This project is pinned to the **ESP-IDF 6.1 stable release line** for ESP32-S3.
The GitHub Actions workflow uses `espressif/idf:release-v6.1` rather than a beta/development
image. The component dependency for LittleFS is pinned to `joltwallet/littlefs` 1.20.4
for reproducible dependency resolution.

The source has been updated for ESP-IDF 6.1 APIs, including FreeRTOS idle-task APIs,
LittleFS VFS registration, Wi-Fi TX-power types, HTTP-server URI matching, and lwIP
MIB2 feature availability.

