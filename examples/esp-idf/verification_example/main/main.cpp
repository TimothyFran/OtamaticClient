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
 * Requires an OTA partition scheme (two app slots + otadata, see
 * partitions.csv) and a rollback enabled bootloader: both are configured by
 * the sdkconfig.defaults of this example.
 */
#include "otamatic/otamatic_client.hpp"
#include <cstring>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Otamatic service key of the account that owns this device.
// Replace the placeholders below with your own values and never commit real
// credentials: this file is tracked by git.
#define OTAMATIC_API_KEY            "your-otamatic-service-key"

#define EXAMPLE_ESP_WIFI_SSID       "your-ssid"
#define EXAMPLE_ESP_WIFI_PASS       "your-password"
#define EXAMPLE_ESP_MAXIMUM_RETRY   5

#define FIRMWARE_VERSION            1
#define FIRMWARE_VERSION_NAME       "1.0.0"

#define WIFI_CONNECTED_BIT          BIT0
#define WIFI_FAIL_BIT               BIT1

static const char* TAG = "verification_example";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

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
static bool verifyNewFirmware()
{
    ESP_LOGI(TAG, "Running post-boot verification...");

    // Example self-tests: every check that fails must return false.
    bool ok = true;

    // e.g. a critical peripheral must respond
    // ok = ok && my_sensor_init();
    // e.g. the configuration must have survived the update
    // ok = ok && load_configuration();

    ESP_LOGI(TAG, "Post-boot verification %s", ok ? "passed" : "FAILED");
    return ok;
}

// Event callback: logs the outcome of the post-boot verification (and every
// other step of the update lifecycle).
static void onOtaEvent(const OtamaticClientEventData& event)
{
    switch (event.event) {
        case OtamaticClientEvent::UpdateConfirmed:
            // The freshly installed firmware passed the verification (or there
            // was nothing to verify): the rollback has been cancelled.
            ESP_LOGI(TAG, "New firmware confirmed at boot");
            break;
        case OtamaticClientEvent::UpdateRolledBack:
            // The new firmware failed the verification: the rollback to the
            // previous version starts right after this event.
            ESP_LOGW(TAG, "New firmware version %lu failed verification, rolling back",
                     (unsigned long)event.version);
            break;
        case OtamaticClientEvent::OperationFailed:
            ESP_LOGE(TAG, "OTA event: %s (%s)",
                     OtamaticClient::getEventName(event.event),
                     OtamaticClient::getErrorName(event.error));
            break;
        default:
            ESP_LOGI(TAG, "OTA event: %s",
                     OtamaticClient::getEventName(event.event));
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), EXAMPLE_ESP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password), EXAMPLE_ESP_WIFI_PASS,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished");
}

extern "C" void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Connecting to the AP before starting the client
    wifi_init_sta();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", EXAMPLE_ESP_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "failed to connect to SSID:%s", EXAMPLE_ESP_WIFI_SSID);
    }

    // System clock synchronization: TLS certificate validation checks the
    // validity dates, otherwise every certificate results "not yet valid"
    // and the HTTPS connection fails.
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP time synchronization failed or timed out, TLS may fail");
    } else {
        ESP_LOGI(TAG, "System time synchronized");
    }

    OtamaticClient client;
    OtamaticConfig config;

    // Client Configuration
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

    // Initializing the client
    client.begin(config);

    while(true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}