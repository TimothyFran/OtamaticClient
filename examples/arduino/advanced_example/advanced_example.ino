/*
 * OtamaticClient - Advanced example: manual update & restart control
 *
 * Builds on the basic example with full manual control over the update
 * lifecycle and a custom device identity:
 *
 * - `autoUpdateEnabled` is disabled: the `UpdateAvailable` event only records
 *   the request in a flag, and the download is started from `loop()` by
 *   calling `applyUpdate()`. Never call blocking library APIs from inside the
 *   event callback: it runs while the library is still performing the
 *   operation (in the auto-check task context).
 * - `autoRestartEnabled` is disabled: the `UpdateCompleted` event only
 *   records the request in a flag; the reboot is performed from `loop()`
 *   after any custom pre-restart logic.
 * - `checkInterval` is 60 seconds: the auto-check task runs a check every
 *   minute (the first check runs immediately at `begin()`).
 * - `deviceId` is a custom identifier instead of the MAC-derived default.
 * - `verifyNewFirmware()` is the custom post-boot verification, run at boot on
 *   the first boot after an update to decide between confirming the new
 *   firmware and rolling back to the previous one.
 * - The client runs one update operation at a time: `checkForUpdate()` and
 *   `applyUpdate()` fail with the `Busy` error while the auto-check is
 *   mid-cycle (just retry later), and the auto-check skips its cycle while a
 *   manual operation is running.
 *
 * With the automatic behaviors disabled the whole update lifecycle is driven
 * by the application: download when it is safe (e.g. while idle), restart
 * when it is convenient.
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

// Custom device identifier registered on the Otamatic platform. When left to
// 0 the client derives it from the station MAC address.
#define OTAMATIC_DEVICE_ID          0x0123456789ABCDEFULL

OtamaticClient client;

// --- Shared state between the event callback and loop() ------------------
//
// The event callback runs in the context of the task that generated the
// event (the auto-check task, or loop() itself during applyUpdate()) while
// the client is still performing the operation: never call blocking library
// APIs from inside it. Just record what happened in these flags and act on
// them from loop().
volatile bool applyUpdateRequested = false;
volatile bool restartRequested = false;

// Firmware metadata advertised by the server with the UpdateAvailable event
// (the pending update data kept by the client is private).
volatile uint32_t pendingVersion = 0;
volatile uint32_t pendingSize = 0;

// Custom post-boot verification: runs at boot on the first boot after an
// update (only when rollbackVerificationEnabled is true). Return true to
// confirm the new firmware, false to roll back to the previous one.
bool verifyNewFirmware() {
    Serial.println("Running post-boot verification...");
    // Insert your self-tests here (peripherals, sensors, configuration...).
    return true;
}

void onOtaEvent(const OtamaticClientEventData& event) {
    switch (event.event) {
        case OtamaticClientEvent::UpdateProgress:
            // progress carries the completion percentage (0-100): log every 10%.
            if (event.progress % 10 == 0) {
                Serial.printf("OTA progress: %u%%\n", event.progress);
            }
            break;

        case OtamaticClientEvent::UpdateAvailable:
            // version/size carry the advertised firmware metadata.
            pendingVersion = event.version;
            pendingSize = event.size;
            // autoUpdateEnabled is disabled: just record the request and
            // start the download from loop() with client.applyUpdate().
            applyUpdateRequested = true;
            break;

        case OtamaticClientEvent::UpdateCompleted:
            // autoRestartEnabled is disabled: just record the request, the
            // reboot is performed from loop() after custom pre-restart logic.
            restartRequested = true;
            break;

        case OtamaticClientEvent::UpdateConfirmed:
            // Emitted at boot when the freshly installed firmware passed the
            // post-boot verification and the rollback has been cancelled.
            Serial.println("New firmware confirmed at boot");
            break;

        case OtamaticClientEvent::UpdateRolledBack:
            // Emitted at boot when the new firmware failed verification: the
            // rollback to the previous version starts right after this event.
            Serial.println("New firmware failed verification, rolling back");
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

    // Custom device identifier: overrides the MAC-derived default.
    config.deviceId = OTAMATIC_DEVICE_ID;

    // Check for updates every 60 seconds (default: 5 minutes).
    config.checkInterval = 60000;

    // Manual update lifecycle: the UpdateAvailable event only records the
    // request in a flag, the download is started from loop().
    config.autoUpdateEnabled = false;

    // Manual reboot: the UpdateCompleted event only records the request,
    // the restart is performed from loop() after custom pre-restart logic.
    config.autoRestartEnabled = false;

    // Event callback registration
    client.onEvent(onOtaEvent);

    // Custom post-boot verification callback (register before begin(): the
    // verification runs inside begin() on the first boot after an update).
    client.onUpdateVerification(verifyNewFirmware);

    // Initializing the client (starts the automatic update check task: the
    // first check runs immediately, then one every checkInterval)
    if (!client.begin(config)) {
        Serial.println("OTA initialization failed");
    }

    Serial.printf("Firmware version: %s (%u)\n", FIRMWARE_VERSION_NAME, FIRMWARE_VERSION);
    Serial.printf("Device ID: %llu\n", (unsigned long long)OTAMATIC_DEVICE_ID);
}

void loop() {
    if (applyUpdateRequested) {
        applyUpdateRequested = false;

        Serial.printf("New firmware version %lu available (%lu bytes)\n",
                      (unsigned long)pendingVersion, (unsigned long)pendingSize);

        // Insert any custom pre-update logic here (e.g. ask the user,
        // wait for an idle state, notify the server...).

        Serial.println("Applying update manually...");
        if (!client.applyUpdate()) {
            // The specific error has been reported by an OperationFailed event
            // (e.g. Busy when the auto-check is mid-cycle: just retry later).
            Serial.println("Update could not be applied");
        }
    }

    if (restartRequested) {
        restartRequested = false;

        Serial.println("Update completed, restarting...");

        // Insert any custom pre-restart logic here (e.g. persist state,
        // flush queues, notify the server...).

        delay(1000);
        ESP.restart();
    }

    delay(100);
}