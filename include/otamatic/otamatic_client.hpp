#pragma once

#include <esp_err.h>
#include <otamatic/otamatic_config.hpp>
#include <otamatic/otamatic_update.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * @brief OTA client for the Otamatic OTA server.
 */
class OtamaticClient {

    public:
        OtamaticClient();
        ~OtamaticClient();

        /// @brief Initializes the client.
        /// @param config Configuration for the client.
        /// @return true if the initialization was successful, false otherwise
        bool begin(const OtamaticConfig& config);

        /// @brief Deinitializes the client.
        void deinit();

        /// @brief Starts the automatic firmware update check process
        /// @return true if the automatic firmware update check process was started successfully, false otherwise
        bool startAutoCheck();

        /// @brief Stops the automatic firmware update check process
        void stopAutoCheck();

        /// @brief Checks if the automatic firmware update check process is running
        /// @return true if the automatic firmware update check process is running, false otherwise
        bool isAutoCheckRunning();

        /// @brief Checks for a new firmware update
        /// @return true if a newer version is available, false if latest version or an error occurred
        bool checkForUpdate();

        /// @brief Downloads and installs the latest firmware update
        /// @return true if the operation was successful, false otherwise
        bool applyUpdate();

    private:
        OtamaticConfig config;
        TaskHandle_t taskHandle = nullptr;
        OtamaticUpdate pendingUpdate;

        static void autoCheckTaskWrapper(void* pvParameters) {
            OtamaticClient* instance = static_cast<OtamaticClient*>(pvParameters);
            instance->autoCheckTask(pvParameters);
        }
        void autoCheckTask(void* pvParameters);

        esp_err_t checkForUpdateInternal();
        esp_err_t applyUpdateInternal();

};