# 1 "C:\\Users\\ilcon\\AppData\\Local\\Temp\\tmpibpfa3ul"
#include <Arduino.h>
# 1 "D:/Sviluppo/Personale/OtamaticClient/examples/PortalExample/PortalExample.ino"
# 16 "D:/Sviluppo/Personale/OtamaticClient/examples/PortalExample/PortalExample.ino"
#include <Arduino.h>
#include <OtamaticClient.h>
#include <WiFi.h>


#define OTAMATIC_FIRMWARE_VERSION 1

const char* kWifiSsid = "your-wifi-ssid";
const char* kWifiPassword = "your-wifi-password";
const char* kOtamaticServiceKey = "your-otamatic-service-key";

WiFiClient httpClient;

OtamaticClient ota;
void onOtaEvent(OtamaticClientEventData eventData);
void setup();
void loop();
#line 31 "D:/Sviluppo/Personale/OtamaticClient/examples/PortalExample/PortalExample.ino"
void onOtaEvent(OtamaticClientEventData eventData) {
    Serial.print(F("[OtamaticClient] Received event: "));
    Serial.print(OtamaticClient::getEventName(eventData.event));

    if (eventData.event == OtamaticClientEvent::UpdateProgress) {

        Serial.printf(F(" (%u%%)"), eventData.data);
    } else if (eventData.event == OtamaticClientEvent::OperationFailed) {

        Serial.printf(F(" (%s)"), OtamaticClient::getErrorName((OtamaticClientError)eventData.data));
    }

    Serial.println();
}

void setup() {
    Serial.begin(115200);



    ota.setClient(&httpClient);



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
            delay(200);
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