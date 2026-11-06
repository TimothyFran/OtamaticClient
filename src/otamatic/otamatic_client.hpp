#pragma once

#include <esp_err.h>
#include <otamatic/otamatic_config.hpp>
#include <otamatic/otamatic_events.hpp>
#include <otamatic/otamatic_update.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/// @brief Post-boot update verification callback (no user context).
/// @return true to confirm the freshly installed firmware, false to roll back
///         to the previous version.
using OtamaticUpdateVerifierCallback = bool (*)();

/// @brief Post-boot update verification callback with a user context pointer.
/// @param userData Opaque pointer forwarded to the callback (e.g. a class instance).
/// @return true to confirm the freshly installed firmware, false to roll back
///         to the previous version.
using OtamaticUpdateVerifierCallbackEx = bool (*)(void* userData);

/**
 * @brief OTA client for the Otamatic OTA server.
 */
class OtamaticClient {

    public:
        OtamaticClient();
        ~OtamaticClient();

        /// @brief The client owns a task and a synchronization lock: copying is not allowed.
        OtamaticClient(const OtamaticClient&) = delete;
        OtamaticClient& operator=(const OtamaticClient&) = delete;

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
        /// @details Fails when another operation (e.g. the automatic check) is already in
        /// progress: an OperationFailed event with the Busy error code is emitted.
        /// @return true if a newer version is available, false if latest version or an error occurred
        bool checkForUpdate();

        /// @brief Downloads and installs the latest firmware update
        /// @details Fails when another operation (e.g. the automatic check) is already in
        /// progress: an OperationFailed event with the Busy error code is emitted.
        /// @return true if the operation was successful, false otherwise
        bool applyUpdate();

        /// @brief Registers a simple event callback (no user context).
        /// @param callback Function invoked for every client event, or nullptr to unregister.
        void onEvent(OtamaticClientEventCallback callback);

        /// @brief Registers an event callback with a user context pointer.
        /// @param callback Function invoked for every client event, or nullptr to unregister.
        /// @param userData Opaque pointer forwarded to the callback (e.g. a class instance).
        void onEvent(OtamaticClientEventCallbackEx callback, void* userData);

        /// @brief Registers the custom post-boot update verification callback.
        /// @details The callback runs at boot (inside begin()) on the first boot after an
        /// update, when the freshly installed firmware is awaiting confirmation and
        /// rollbackVerificationEnabled is true. Return true to confirm the new firmware
        /// (rollback cancelled), false to restore the previous version (the device
        /// restarts into it immediately). When no callback is registered the
        /// verification always succeeds. Register it before begin().
        /// @param callback Function invoked to verify the new firmware, or nullptr to unregister.
        void onUpdateVerification(OtamaticUpdateVerifierCallback callback);

        /// @brief Registers the verification callback with a user context pointer.
        /// @param callback Function invoked to verify the new firmware, or nullptr to unregister.
        /// @param userData Opaque pointer forwarded to the callback (e.g. a class instance).
        void onUpdateVerification(OtamaticUpdateVerifierCallbackEx callback, void* userData);

        /// @brief Returns a human-readable name for an event type.
        static const char* getEventName(OtamaticClientEvent event);

        /// @brief Returns a human-readable name for an error code.
        static const char* getErrorName(OtamaticClientError error);

    private:
        OtamaticConfig config;
        TaskHandle_t taskHandle = nullptr;
        SemaphoreHandle_t operationLock = nullptr;
        OtamaticUpdate pendingUpdate;

        OtamaticClientEventCallback eventCallback = nullptr;
        OtamaticClientEventCallbackEx eventCallbackEx = nullptr;
        void* eventUserData = nullptr;

        OtamaticUpdateVerifierCallback updateVerifier = nullptr;
        OtamaticUpdateVerifierCallbackEx updateVerifierEx = nullptr;
        void* updateVerifierUserData = nullptr;

        /// @brief Builds the event data and delivers it to the registered callback (if any).
        void transmitEvent(OtamaticClientEvent event,
                           OtamaticClientError error = OtamaticClientError::None,
                           uint8_t progress = 0,
                           uint32_t version = 0,
                           uint32_t size = 0);

        static void autoCheckTaskWrapper(void* pvParameters) {
            OtamaticClient* instance = static_cast<OtamaticClient*>(pvParameters);
            instance->autoCheckTask(pvParameters);
        }
        void autoCheckTask(void* pvParameters);

        /// @brief Tries to acquire the operation lock without blocking.
        /// @return true when the caller may run an operation (the lock is held).
        bool tryBeginOperation();

        /// @brief Releases the operation lock acquired with tryBeginOperation().
        void endOperation();

        esp_err_t checkForUpdateInternal();
        esp_err_t applyUpdateInternal();

        /// @brief Confirms or rolls back the freshly installed firmware at boot.
        /// @details Called by begin() on the first boot after an update (running firmware
        /// awaiting confirmation). On verification failure the previous firmware is
        /// restored and the device restarts: this method does not return in that case.
        void runBootVerification();

};