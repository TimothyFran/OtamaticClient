/*
 * OtamaticClient - Minimal example: post-boot update verification with rollback
 *
 * Shows how to add a custom post-boot verification to the update lifecycle:
 *
 * - `verifyNewFirmware()` is the user code that checks the freshly installed
 *   firmware (self-tests on peripherals, sensors, stored configuration...).
 *   It runs at boot, inside begin(), only on the first boot after an update
 *   and only when `rollbackVerificationEnabled` is true (the default).
 * - The callback returns true when the new firmware is healthy: the client
 *   confirms it and the rollback is cancelled (UpdateConfirmed event). When it
 *   returns false the client restores the previous firmware and restarts the
 *   device into it (UpdateRolledBack event, emitted just before the restart).
 * - Register the callback with `client.onUpdateVerification()` BEFORE
 *   `begin()`: the verification runs inside begin(). A second overload,
 *   `onUpdateVerification(callback, userData)`, forwards a context pointer
 *   for use with classes.
 * - Without a registered callback the verification always succeeds: the new
 *   firmware is confirmed and no rollback can happen.
 *
 * Requires an OTA partition scheme (two app slots + otadata) and a rollback
 * enabled bootloader: the official Arduino-ESP32 core ships one, so this
 * example works out of the box there.
 */
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

// Otamatic service key of the account that owns this device.
#define OTAMATIC_API_KEY            "your-otamatic-service-key"

#define WIFI_SSID                   "your-ssid"
#define WIFI_PASS                   "your-password"

#define FIRMWARE_VERSION            1
#define FIRMWARE_VERSION_NAME       "1.0.0"

OtamaticClient client;

// --- Custom post-boot verification -----------------------------------------
//
// This function decides the fate of a freshly installed firmware: it runs at
// boot (inside begin()), only on the first boot after an update. Return true
// to confirm the new firmware (rollback cancelled), false to restore the
// previous version (the device restarts into it right away).
//
// Replace the placeholder checks with the self-tests that make sense for your
// product. Keep them quick: the firmware is not confirmed until this function
// returns.
bool verifyNewFirmware() {
    Serial.println("Running post-boot verification...");

    // Example self-tests: every check that fails must return false.
    bool ok = true;

    // e.g. a critical peripheral must respond
    // ok = ok && mySensor.begin();
    // e.g. the configuration must have survived the update
    // ok = ok && loadConfiguration();
    // e.g. the device must be on the network
    ok = ok && (WiFi.status() == WL_CONNECTED);

    Serial.printf("Post-boot verification %s\n", ok ? "passed" : "FAILED");
    return ok;
}

// Event callback: logs the outcome of the post-boot verification (and every
// other step of the update lifecycle).
void onOtaEvent(const OtamaticClientEventData& event) {
    switch (event.event) {
        case OtamaticClientEvent::UpdateConfirmed:
            // The freshly installed firmware passed the verification (or there
            // was nothing to verify): the rollback has been cancelled.
            Serial.println("New firmware confirmed at boot");
            break;

        case OtamaticClientEvent::UpdateRolledBack:
            // The new firmware failed the verification: the rollback to the
            // previous version starts right after this event.
            Serial.printf("New firmware version %lu failed verification, rolling back\n",
                          (unsigned long)event.version);
            break;

        case OtamaticClientEvent::OperationFailed:
            Serial.printf("OTA event: %s (%s)\n",
                          OtamaticClient::getEventName(event.event),
                          OtamaticClient::getErrorName(event.error));
            break;

        default:
            Serial.printf("OTA event: %s\n",
                          OtamaticClient::getEventName(event.event));
            break;
    }
}

void connectWiFi() {
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void setup() {
    Serial.begin(115200);
    delay(200);

    // Connecting to the AP before starting the client
    connectWiFi();

    // System clock synchronization: TLS certificate validation checks the
    // validity dates, otherwise every certificate results "not yet valid"
    // and the HTTPS connection fails.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.print("Waiting for NTP time sync");
    while (time(nullptr) < 1609459200) {  // 2021-01-01: clock considered synced
        delay(500);
        Serial.print(".");
    }
    Serial.println(" done");

    // Client configuration
    OtamaticConfig config;
    config.apiKey = OTAMATIC_API_KEY;
    config.firmwareVersion = FIRMWARE_VERSION;
    config.versionName = FIRMWARE_VERSION_NAME;

    // Post-boot verification with automatic rollback on failure (default:
    // true, set explicitly to highlight it).
    config.rollbackVerificationEnabled = true;

    // Callbacks must be registered before begin(): on the first boot after an
    // update the verification runs inside begin() and its outcome is reported
    // through the UpdateConfirmed / UpdateRolledBack events.
    client.onEvent(onOtaEvent);
    client.onUpdateVerification(verifyNewFirmware);

    // Initializing the client (starts the automatic update check task)
    client.begin(config);
}

void loop() {
    // Nothing to do here: the client runs its own auto-check task.
    delay(1000);
}