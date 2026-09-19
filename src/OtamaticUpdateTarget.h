#pragma once

#include <Arduino.h>

/**
 * Partition an OTA write goes to (remote download or portal upload).
 *
 * The portal page can force the target or leave it to "auto": the device then
 * recognizes the image from its first bytes. ESP32 application images start
 * with the ESP_IMAGE_HEADER_MAGIC byte (0xE9), LittleFS images carry the
 * "littlefs" magic string at offset 8 (littlefs specification), while SPIFFS
 * and FAT images have no marker and must be stated explicitly.
 */
enum class OtamaticUpdateTarget : uint8_t {
    /** Application image: written to the next OTA partition. */
    Firmware = 0,

    /** Filesystem image (LittleFS/SPIFFS): written to the filesystem partition. */
    Filesystem = 1
};