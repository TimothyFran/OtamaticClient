/*
 * OtamaticClient - Advanced Wi-Fi example
 *
 * Combines the basic Wi-Fi setup with firmware signature verification
 * (ECDSA over NIST P-256 with SHA-256) and fully manual control over the
 * update lifecycle:
 *
 * - `autoUpdate` is disabled: the firmware download must be started
 *   explicitly by calling `applyUpdate()` when the `UpdateAvailable` event
 *   is received.
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

OtamaticClient ota(httpClient);

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
        case OtamaticClientEvent::UpdateAvailable: {
            // autoUpdate is disabled: decide here whether to apply the
            // update. The check data carries version, size, signature and
            // integrity hash provided by the server.
            const OtamaticVersionCheckData& checkData = ota.getCheckData();
            Serial.printf(F("[OtamaticClient] New firmware version %lu available (%lu bytes)\n"),
                          (unsigned long)checkData.version, (unsigned long)checkData.size);
            Serial.printf(F("[OtamaticClient] Integrity hash: %s\n"), checkData.getIntegrity());

            // Apply the update manually. Insert any custom pre-update logic
            // here (e.g. ask the user, wait for idle state, notify the
            // server...).
            Serial.println(F("[OtamaticClient] Applying update manually..."));
            if (!ota.applyUpdate()) {
                Serial.println(F("[OtamaticClient] Update could not be applied (see UpdateFailed event)"));
            }
            break;
        }

        case OtamaticClientEvent::UpdateCompleted:
            // autoRestart is disabled: restart the device manually.
            // Any pre-reboot logic (e.g. flush logs, notify a status
            // endpoint) can go here.
            Serial.println(F("[OtamaticClient] Update completed, restarting..."));
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

    if (!ota.begin(OTAMATIC_FIRMWARE_VERSION, kOtamaticServiceKey)) {
        Serial.println("[OtamaticClient] Initialization Failed");
    }

    ota.setCheckInterval(15000);
    ota.onEvent(onOtaEvent);
    ota.setPublicKey(PUBLIC_KEY_PEM);

    // Disable the automatic behaviors: update and restart are handled
    // manually in the event callback.
    ota.setAutoUpdate(false);
    ota.setAutoRestart(false);

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());

    // Perform the first check immediately
    ota.requestCheckNow();
}

void loop() {
    ota.loop();
}

