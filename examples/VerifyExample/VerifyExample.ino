/*
 * OtamaticClient - Rollback verification example
 *
 * Confirms a remote update with a custom callback via `onVerify()`. After
 * every remote update the library stores the expected version in NVS and
 * reboots; on the next boot `begin()` runs the callback: return `Valid` to
 * confirm the image and cancel the rollback, `Invalid` to roll back to the
 * previous OTA slot (when available) and reboot.
 *
 * Insert your own evaluation logic inside verifyUpdate() below to decide
 * whether the update works; return Invalid when it does not, so the device
 * automatically falls back to the previous firmware.
 *
 * Rules for the callback:
 * - Register it with `onVerify()` BEFORE `begin()`: verification runs inside
 *   `begin()`, so registering it later is too late for the current boot.
 * - Keep it short and non-blocking: it runs before setup() continues.
 * - Never call OtamaticClient APIs from inside the callback.
 */

#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

// Current firmware version, should be incremental, starting from 1
#define OTAMATIC_FIRMWARE_VERSION 1

const char* kWifiSsid = "your-wifi-ssid";
const char* kWifiPassword = "your-wifi-password";
const char* kOtamaticServiceKey = "your-otamatic-service-key";

WiFiClient httpClient;

OtamaticClient ota;

OtamaticClient::VerifyResult verifyUpdate() {
    
    // Insert here your own logic to decide whether the update works
    // (e.g. check sensors, peripherals, calibration, ...).
    // Return Valid to confirm the image and cancel the rollback, or
    // Invalid to roll back to the previous firmware:
    //   return OtamaticClient::VerifyResult::Valid;
    //   return OtamaticClient::VerifyResult::Invalid;

    // If this check fails, the firmware is rejected and
    // device will roll back to the previous version.

    return OtamaticClient::VerifyResult::Valid;
}

void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_STA);
    WiFi.begin(kWifiSsid, kWifiPassword);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Wi-Fi connected, IP address: ");
    Serial.println(WiFi.localIP());

    ota.setClient(&httpClient);

    // Register BEFORE begin()
    // Rollback verification is enabled by default; use
    // setRollbackVerification(false) to skip it entirely.
    ota.onVerify(verifyUpdate);

    if (!ota.begin(OTAMATIC_FIRMWARE_VERSION, kOtamaticServiceKey)) {
        Serial.println("[OtamaticClient] Initialization Failed");
    }

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());
}

void loop() {
    ota.loop();
}

