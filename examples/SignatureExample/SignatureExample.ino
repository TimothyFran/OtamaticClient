/*
 * OtamaticClient - Basic Wi-Fi example
 *
 * Checks the Otamatic server for a new firmware version and applies it
 * automatically. The library works with any Arduino `Client`
 * implementation: switch WiFiClient to WiFiClientSecure for HTTPS, or to
 * EthernetClient / TinyGsmClient for Ethernet / GSM transports.
 */
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>

// Current firmware version, should be incremental, starting from 1
#define OTAMATIC_FIRMWARE_VERSION 1

const char* kWifiSsid = "your-wifi-ssid";
const char* kWifiPassword = "your-wifi-password";
const char* kOtamaticServiceKey = "your-otamatic-service-key";

const char* PUBLIC_KEY_PEM = R"(
-----BEGIN PUBLIC KEY-----
...
-----END PUBLIC KEY-----
)";

WiFiClient httpClient;

OtamaticClient ota;

void onOtaEvent(OtamaticClientEventData eventData) {
    Serial.print(F("[OtamaticClient] Received event: "));
    Serial.print(OtamaticClient::getEventName(eventData.event));

    if (eventData.event == OtamaticClientEvent::UpdateProgress) {
        // data carries the progress percentage (0-100)
        Serial.printf(F(" (%u%%)"), eventData.data);
    } else if (eventData.event == OtamaticClientEvent::OperationFailed) {
        // data carries an OtamaticClientError code
        Serial.printf(F(" (%s)"), OtamaticClient::getErrorName((OtamaticClientError)eventData.data));
    }

    Serial.println();
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

    // Register the event handler BEFORE begin(): events emitted by begin()
    // (e.g. ConfigInvalid) are delivered to it.
    ota.onEvent(onOtaEvent);
    ota.setPublicKey(PUBLIC_KEY_PEM);

    if (!ota.begin(OTAMATIC_FIRMWARE_VERSION, kOtamaticServiceKey)) {
        Serial.println("[OtamaticClient] Initialization Failed");
    }

    ota.setCheckInterval(15000);

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());

    // Perform the first check immediately
    ota.requestCheckNow();
}

void loop() {
    ota.loop();
}