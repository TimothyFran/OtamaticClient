/*
 * OtamaticClient - Advanced example: manual update & restart control
 *
 * Builds on the basic example with full manual control over the update
 * lifecycle and a custom device identity:
 *
 * - `autoUpdateEnabled` is disabled: the `UpdateAvailable` event only records
 *   the request in a flag, and the download is started from the main loop by
 *   calling `applyUpdate()`. Never call blocking library APIs from inside the
 *   event callback: it runs while the library is still performing the
 *   operation (in the auto-check task context).
 * - `autoRestartEnabled` is disabled: the `UpdateCompleted` event only
 *   records the request in a flag; the reboot is performed from the main
 *   loop after any custom pre-restart logic.
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
 */
#include "otamatic/otamatic_client.hpp"
#include <cstring>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
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

// Custom device identifier registered on the Otamatic platform. When left to
// 0 the client derives it from the station MAC address.
#define OTAMATIC_DEVICE_ID          0x0123456789ABCDEFULL

#define WIFI_CONNECTED_BIT          BIT0
#define WIFI_FAIL_BIT               BIT1

static const char* TAG = "advanced_example";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// --- Shared state between the event callback and the main loop -----------
//
// The event callback runs in the context of the task that generated the
// event (the auto-check task, or the main loop itself during applyUpdate())
// while the client is still performing the operation: never call blocking
// library APIs from inside it. Just record what happened in these flags and
// act on them from the main loop.
static volatile bool s_applyUpdateRequested = false;
static volatile bool s_restartRequested = false;

// Firmware metadata advertised by the server with the UpdateAvailable event
// (the pending update data kept by the client is private).
static volatile uint32_t s_pendingVersion = 0;
static volatile uint32_t s_pendingSize = 0;

// Custom post-boot verification: runs at boot on the first boot after an
// update (only when rollbackVerificationEnabled is true). Return true to
// confirm the new firmware, false to roll back to the previous one.
static bool verifyNewFirmware()
{
    ESP_LOGI(TAG, "Running post-boot verification...");
    // Insert your self-tests here (peripherals, sensors, configuration...).
    return true;
}

static void onOtaEvent(const OtamaticClientEventData& event)
{
    switch (event.event) {
        case OtamaticClientEvent::UpdateProgress:
            // progress carries the completion percentage (0-100): log every 10%.
            if (event.progress % 10 == 0) {
                ESP_LOGI(TAG, "OTA progress: %u%%", event.progress);
            }
            break;
        case OtamaticClientEvent::UpdateAvailable:
            // version/size carry the advertised firmware metadata.
            s_pendingVersion = event.version;
            s_pendingSize = event.size;
            // autoUpdateEnabled is disabled: just record the request and
            // start the download from the main loop with applyUpdate().
            s_applyUpdateRequested = true;
            break;
        case OtamaticClientEvent::UpdateCompleted:
            // autoRestartEnabled is disabled: just record the request, the
            // reboot is performed from the main loop after custom
            // pre-restart logic.
            s_restartRequested = true;
            break;
        case OtamaticClientEvent::UpdateConfirmed:
            // Emitted at boot when the freshly installed firmware passed the
            // post-boot verification and the rollback has been cancelled.
            ESP_LOGI(TAG, "New firmware confirmed at boot");
            break;
        case OtamaticClientEvent::UpdateRolledBack:
            // Emitted at boot when the new firmware failed verification: the
            // rollback to the previous version starts right after this event.
            ESP_LOGW(TAG, "New firmware failed verification, rolling back");
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

    // Custom device identifier: overrides the MAC-derived default.
    config.deviceId = OTAMATIC_DEVICE_ID;

    // Check for updates every 60 seconds (default: 5 minutes).
    config.checkInterval = 60000;

    // Manual update lifecycle: the UpdateAvailable event only records the
    // request in a flag, the download is started from the main loop.
    config.autoUpdateEnabled = false;

    // Manual reboot: the UpdateCompleted event only records the request,
    // the restart is performed from the main loop after custom pre-restart
    // logic.
    config.autoRestartEnabled = false;

    // Event callback registration
    client.onEvent(onOtaEvent);

    // Custom post-boot verification callback (register before begin(): the
    // verification runs inside begin() on the first boot after an update).
    client.onUpdateVerification(verifyNewFirmware);

    // Initializing the client (starts the automatic update check task: the
    // first check runs immediately, then one every checkInterval)
    if (!client.begin(config)) {
        ESP_LOGE(TAG, "OTA initialization failed");
    }

    ESP_LOGI(TAG, "Firmware version: %s (%u)", FIRMWARE_VERSION_NAME, FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Device ID: %llu", (unsigned long long)OTAMATIC_DEVICE_ID);

    while (true) {
        if (s_applyUpdateRequested) {
            s_applyUpdateRequested = false;

            ESP_LOGI(TAG, "New firmware version %lu available (%lu bytes)",
                     (unsigned long)s_pendingVersion, (unsigned long)s_pendingSize);

            // Insert any custom pre-update logic here (e.g. ask the user,
            // wait for an idle state, notify the server...).

            ESP_LOGI(TAG, "Applying update manually...");
            if (!client.applyUpdate()) {
                // The specific error has already been reported by an
                // OperationFailed event (e.g. Busy when the auto-check is
                // mid-cycle: just retry later).
                ESP_LOGW(TAG, "Update could not be applied");
            }
        }

        if (s_restartRequested) {
            s_restartRequested = false;

            ESP_LOGI(TAG, "Update completed, restarting...");

            // Insert any custom pre-restart logic here (e.g. persist state,
            // flush queues, notify the server...).

            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}