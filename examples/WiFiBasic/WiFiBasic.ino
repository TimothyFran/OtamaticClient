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

WiFiClient httpClient;

OtamaticClient ota(httpClient);

void onOtaEvent(OtamaticClientEvent event) {
    Serial.printf(F("[OtamaticClient] Received event: %s\n"), OtamaticClient::getEventName(event));
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

    ota.setCheckInterval(1000);
    ota.onEvent(onOtaEvent);

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());
}

void loop() {
    ota.loop();
}