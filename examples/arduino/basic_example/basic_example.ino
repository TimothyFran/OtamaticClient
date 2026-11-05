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

// Event callback: receives every lifecycle event with its data (error code,
// progress percentage, firmware version/size).
void onOtaEvent(const OtamaticClientEventData& event) {
    switch (event.event) {
        case OtamaticClientEvent::UpdateProgress:
            // Log only every 5%.
            if (event.progress % 5 == 0) {
                Serial.printf("OTA progress: %u%%\n", event.progress);
            }
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
    // config.checkInterval = 300000;   // default: 5 minutes

    // Event callback registration
    client.onEvent(onOtaEvent);

    // Initializing the client (starts the automatic update check task)
    client.begin(config);
}

void loop() {
    // Nothing to do here: the client runs its own auto-check task.
    delay(1000);
}