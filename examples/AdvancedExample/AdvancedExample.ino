/*
 * OtamaticClient - Advanced Wi-Fi example
 *
 * Combines the basic Wi-Fi setup with firmware signature verification
 * (ECDSA over NIST P-256 with SHA-256) and fully manual control over the
 * update lifecycle:
 *
 * - `autoUpdate` is disabled: the `UpdateAvailable` event only records the
 *   request in a flag, and the download is started from `loop()` by calling
 *   `applyUpdate()`. Never call blocking library APIs from inside the event
 *   callback: it runs while the library is still performing the operation.
 * - `autoRestart` is disabled: the reboot must be performed manually when
 *   the `UpdateCompleted` event is received.
 *
 * The library works with any Arduino `Client` implementation: switch
 * WiFiClient to WiFiClientSecure for HTTPS, or to EthernetClient /
 * TinyGsmClient for Ethernet / GSM transports.
 */
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

// Current firmware version, should be incremental, starting from 1
#define OTAMATIC_FIRMWARE_VERSION 1

const char* kWifiSsid = "your-wifi-ssid";
const char* kWifiPassword = "your-wifi-password";
const char* kOtamaticServiceKey = "your-otamatic-service-key";

// ECDSA public key (PEM) used to verify the firmware signature.
// The server must provide the hex-encoded DER signature of the firmware.
const char* PUBLIC_KEY_PEM = R"(
-----BEGIN PUBLIC KEY-----
...
-----END PUBLIC KEY-----
)";

WiFiClient httpClient;

OtamaticClient ota;

// Set by the UpdateAvailable event callback, consumed in loop(). Never call
// blocking library APIs (e.g. applyUpdate()) from inside the event callback:
// it fires while the library is still performing the check operation.
volatile bool applyUpdateRequested = false;

void onOtaEvent(OtamaticClientEventData eventData) {
    Serial.print(F("[OtamaticClient] Received event: "));
    Serial.print(OtamaticClient::getEventName(eventData.event));

    if (eventData.event == OtamaticClientEvent::UpdateProgress) {
        // data carries the progress percentage (0-100)
        Serial.printf(F(" (%u%%)"), eventData.data);
    } else if (eventData.event == OtamaticClientEvent::UpdateFailed) {
        // data carries an OtamaticClientError code
        Serial.printf(F(" (%s)"), OtamaticClient::getErrorName((OtamaticClientError)eventData.data));
    }

    Serial.println();

    switch (eventData.event) {
        case OtamaticClientEvent::UpdateAvailable:
            // NEVER call applyUpdate() here. Just record the
            // request and act on it from outside the callback.
            applyUpdateRequested = true;
            break;

        case OtamaticClientEvent::UpdateCompleted:
            // autoRestart is disabled: restart the device manually

            Serial.println(F("[OtamaticClient] Update completed, restarting..."));
            
            // Insert any custom pre-restart logic here (e.g. ask the user,
            // wait for idle state, notify the server...).

            delay(1000);
            ESP.restart();
            break;

        case OtamaticClientEvent::UpdateFailed:
            Serial.println(F("[OtamaticClient] Update failed, the next check will retry automatically"));
            break;

        default:
            break;
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

    // Attach the transport: can also be called again at runtime (e.g. on a
    // network interface change) without losing any configuration.
    ota.setClient(&httpClient);

    if (!ota.begin(OTAMATIC_FIRMWARE_VERSION, kOtamaticServiceKey)) {
        Serial.println("[OtamaticClient] Initialization Failed");
    }

    ota.setCheckInterval(15000);
    ota.onEvent(onOtaEvent);
    ota.setPublicKey(PUBLIC_KEY_PEM);

    // Disable the automatic behaviors: the update is applied manually from
    // loop(), the restart from the UpdateCompleted event.
    ota.setAutoUpdate(false);
    ota.setAutoRestart(false);

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());

    // Perform the first check immediately
    ota.requestCheckNow();
}

void loop() {
    ota.loop();

    if (applyUpdateRequested && ota.hasUpdateAvailable()) {
        applyUpdateRequested = false;

        const OtamaticVersionCheckData& checkData = ota.getCheckData();
        Serial.printf(F("[OtamaticClient] New firmware version %lu available (%lu bytes)\n"),
                      (unsigned long)checkData.version, (unsigned long)checkData.size);
        Serial.printf(F("[OtamaticClient] Integrity hash: %s\n"), checkData.getIntegrity());

        // Insert any custom pre-update logic here (e.g. ask the user,
        // wait for idle state, notify the server...).

        Serial.println(F("[OtamaticClient] Applying update manually..."));
        if (!ota.applyUpdate()) {
            Serial.println(F("[OtamaticClient] Update could not be applied (see UpdateFailed event)"));
        }
    }
}

