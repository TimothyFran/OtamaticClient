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
 *
 * This example also shows the failed-version guard that breaks the
 * update/rollback loop: a version that keeps failing (download errors,
 * integrity/signature rejections, rollback or onVerify() rejection) is
 * counted and, after `setMaxFailedAttempts()` failures, ignored until the
 * server offers a newer version (see the `UpdateIgnored` event below).
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

void onOtaEvent(OtamaticClientEventData eventData) {
    Serial.print(F("[OtamaticClient] Received event: "));
    Serial.print(OtamaticClient::getEventName(eventData.event));

    if (eventData.event == OtamaticClientEvent::UpdateProgress) {
        // data carries the progress percentage (0-100)
        Serial.printf(F(" (%u%%)"), eventData.data);
    } else if (eventData.event == OtamaticClientEvent::OperationFailed) {
        // data carries an OtamaticClientError code
        Serial.printf(F(" (%s)"), OtamaticClient::getErrorName((OtamaticClientError)eventData.data));
    } else if (eventData.event == OtamaticClientEvent::UpdateIgnored) {
        // The failing remote version is ignored: the device stays on the
        // running firmware until the server offers a newer version.
        Serial.printf(F(" (version %lu ignored after %u failed attempt(s))"),
                      (unsigned long)ota.ignoredVersion(), (unsigned)ota.ignoredVersionFailures());
    }

    Serial.println();
}

void logVerifyOutcome() {
    switch (ota.verifyState()) {
        case OtamaticClient::VerifyState::Confirmed:
            Serial.println(F("[OtamaticClient] Pending update confirmed, failure counter cleared"));
            break;
        case OtamaticClient::VerifyState::RolledBack:
            Serial.printf(F("[OtamaticClient] Rollback detected (failures for v%lu: %u/%u)\n"),
                          (unsigned long)ota.ignoredVersion(),
                          (unsigned)ota.ignoredVersionFailures(),
                          (unsigned)ota.maxFailedAttempts());
            break;
        case OtamaticClient::VerifyState::Rejected:
            Serial.printf(F("[OtamaticClient] Rejected by onVerify() (failures for v%lu: %u/%u)\n"),
                          (unsigned long)ota.ignoredVersion(),
                          (unsigned)ota.ignoredVersionFailures(),
                          (unsigned)ota.maxFailedAttempts());
            break;
        case OtamaticClient::VerifyState::Pending:
            Serial.printf(F("[OtamaticClient] Reboot to version %lu pending\n"),
                          (unsigned long)ota.pendingVersion());
            break;
        case OtamaticClient::VerifyState::None:
        default:
            break;
    }

    if (ota.ignoredVersion() != 0) {
        Serial.printf(F("[OtamaticClient] Tracked version: %lu (%u failure(s), ignored after %u)\n"),
                      (unsigned long)ota.ignoredVersion(),
                      (unsigned)ota.ignoredVersionFailures(),
                      (unsigned)ota.maxFailedAttempts());
    }
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
    ota.onEvent(onOtaEvent);

    // Failed-version guard (enabled by default): ignore a remote version
    // after N failed attempts so a bad release cannot loop
    // update -> rollback -> update forever. The counter resets on the
    // first confirmed update, when a different version fails, or manually
    // with resetIgnoredVersion().
    ota.setFailedVersionGuard(true);
    ota.setMaxFailedAttempts(3);

    if (!ota.begin(OTAMATIC_FIRMWARE_VERSION, kOtamaticServiceKey)) {
        Serial.println("[OtamaticClient] Initialization Failed");
    }

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());

    logVerifyOutcome();
}

void loop() {
    ota.loop();
}

