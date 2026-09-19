/*
 * OtamaticClient - Local update portal example
 *
 * Checks the Otamatic server for a new firmware version and applies it
 * automatically. When the device cannot join the configured Wi-Fi network within
 * 5 seconds it falls back to a soft-AP running the local update portal: connect
 * to the "Otamatic AP" network and open http://192.168.4.1/ to write a firmware
 * .bin to the OTA slot or a filesystem image (LittleFS/SPIFFS) to the filesystem
 * partition; the device restarts as soon as the write completes. A filesystem
 * image requires a spiffs/littlefs partition in the partition table.
 *
 * The library works with any Arduino `Client` implementation: switch WiFiClient
 * to WiFiClientSecure for HTTPS, or to EthernetClient / TinyGsmClient for
 * Ethernet / GSM transports.
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

    // Attach the transport: can also be called again at runtime (e.g. on a
    // network interface change) without losing any configuration.
    ota.setClient(&httpClient);

    // Register the event handler BEFORE begin(): events emitted by begin()
    // (e.g. ConfigInvalid) are delivered to it.
    ota.onEvent(onOtaEvent);

    if (!ota.begin(OTAMATIC_FIRMWARE_VERSION, kOtamaticServiceKey)) {
        Serial.println("[OtamaticClient] Initialization Failed");
    }

    ota.setCheckInterval(1000);

    Serial.printf(F("[OtamaticClient] Firmware version: %d\n"), ota.firmwareVersion());
    Serial.printf(F("[OtamaticClient] Device ID: %llu\n"), ota.getDeviceId());

    WiFi.mode(WIFI_STA);
    WiFi.begin(kWifiSsid, kWifiPassword);
    uint32_t start = millis();

    while (WiFi.status() != WL_CONNECTED) {
        
        delay(500);
        Serial.print(".");

        if (millis() - start > 5000) {
            
            Serial.println("WiFi timeout -- Starting AP Portal");

            WiFi.disconnect(true);
            WiFi.mode(WIFI_AP);
            if (!WiFi.softAP("Otamatic AP")) {
                Serial.println("softAP creation failed -- Restarting");
                delay(1000);
                ESP.restart();
            }
            delay(200);  // give the AP netif time to come up (192.168.4.1)
            Serial.print("AP IP address: ");
            Serial.println(WiFi.softAPIP());

            ota.setAutoRestart(true);
            if (!ota.startPortal(60000)) {
                Serial.println("Portal failed to start -- Restarting");
                delay(1000);
                ESP.restart();
            }
            start = millis();

            while (millis() - start < 60000) {
                ota.loop();
            }

            Serial.println("AP Portal timeout -- Restarting");
            ESP.restart();

        }

    }

    Serial.println();
    Serial.print("Wi-Fi connected, IP address: ");
    Serial.println(WiFi.localIP());
}

void loop() {
    ota.loop();
}