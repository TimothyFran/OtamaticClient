#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Event types emitted by the OtamaticClient during its lifecycle.
 *
 * Events are delivered synchronously to the callback registered with
 * OtamaticClient::onEvent(), in the context of the task that generated
 * the event (the auto-check task or the caller of checkForUpdate()/applyUpdate()).
 */
enum class OtamaticClientEvent : uint8_t {
    /// A new update check has started.
    CheckingForUpdate,
    /// A newer firmware version is available (version/size fields are filled).
    UpdateAvailable,
    /// The server reports no newer firmware than the one running.
    UpdateNotNeeded,
    /// The update was skipped by the failed-version guard.
    UpdateIgnored,
    /// The firmware download started and the image is being written to flash.
    UpdateStarted,
    /// Download progress update (progress field holds the percentage 0-100).
    UpdateProgress,
    /// The firmware was downloaded, verified and installed successfully.
    UpdateCompleted,
    /// The freshly installed firmware was confirmed at boot (rollback cancelled).
    UpdateConfirmed,
    /// The installed firmware failed post-boot verification and was rolled back.
    UpdateRolledBack,
    /// An operation failed (error field holds the specific error code).
    OperationFailed,
};

/**
 * @brief Error codes carried by OtamaticClientEvent::OperationFailed events.
 */
enum class OtamaticClientError : uint8_t {
    /// No error.
    None,
    /// Invalid or incomplete client configuration.
    ConfigInvalid,
    /// The HTTP connection could not be established.
    ConnectionFailed,
    /// The server answered with a non-200 HTTP status.
    HttpFailed,
    /// The check response is missing required fields or is malformed.
    InvalidCheckData,
    /// The advertised firmware image is invalid (bad size, does not fit the partition).
    InvalidFirmware,
    /// The OTA update could not be started (partition/error while beginning).
    BeginFailed,
    /// The firmware download failed (connection dropped, stalled or size mismatch).
    DownloadFailed,
    /// The firmware could not be written to flash.
    WriteFailed,
    /// The update could not be finalized (esp_ota_end / boot partition switch failed).
    EndFailed,
    /// The SHA-256 integrity verification failed.
    IntegrityFailed,
    /// The firmware signature verification failed (or is missing when required).
    SignatureFailed,
    /// The firmware image is not a valid app image.
    InvalidImage,
    /// The post-boot firmware verification failed.
    VerificationFailed,
};

/**
 * @brief Data passed to the event callback.
 *
 * Fields are meaningful depending on the event type:
 * - error:    set for OtamaticClientEvent::OperationFailed.
 * - progress: set for OtamaticClientEvent::UpdateProgress (0-100).
 * - version:  firmware version related to the event.
 * - size:     firmware image size (UpdateAvailable / UpdateStarted).
 */
struct OtamaticClientEventData {
    OtamaticClientEvent event = OtamaticClientEvent::OperationFailed;
    OtamaticClientError error = OtamaticClientError::None;
    uint8_t progress = 0;
    uint32_t version = 0;
    uint32_t size = 0;
};

/// @brief Simple event callback (no user context).
using OtamaticClientEventCallback = void (*)(const OtamaticClientEventData&);

/// @brief Event callback with a user context pointer (e.g. to bind a class instance).
using OtamaticClientEventCallbackEx = void (*)(const OtamaticClientEventData&, void* userData);