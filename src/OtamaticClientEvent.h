#pragma once

#include <Arduino.h>

/**
 * Events emitted by OtamaticClient during the check/update cycle.
 */
enum class OtamaticClientEvent {
    CheckingForUpdate,  // Start of a version check cycle
    UpdateAvailable,    // A new firmware version was found
    UpdateNotNeeded,    // No update available
    UpdateIgnored,      // Remote version ignored after too many failed attempts
    UpdateStarted,      // Writing of the new image has started (firmware or filesystem)
    UpdateProgress,     // Image write progress
    UpdateCompleted,    // Update completed (reboot imminent with setAutoRestart(true))
    UpdateConfirmed,    // A pending remote firmware update passed verification after reboot
    UpdateRolledBack,   // A pending remote firmware update failed verification (rollback requested)
    OperationFailed     // Operation failed (check, update or configuration)
};

/**
 * Error codes transmitted as the payload of OtamaticClientEvent::OperationFailed.
 */
enum class OtamaticClientError : uint8_t {
    None = 0,           // No error (used as default payload)
    InvalidCheckData,   // No valid version check result when applying the update
    ConnectionFailed,   // Could not establish the connection to the server
    HttpFailed,         // The server answered with an unexpected HTTP status
    InvalidFirmware,    // The image size is invalid (zero, or larger than the target partition)
    BeginFailed,        // Could not reserve the update partition (no space, missing partition...)
    DownloadFailed,     // Download stalled or connection dropped
    WriteFailed,        // Error while writing the image into the flash partition
    EndFailed,          // Final image verification failed
    IntegrityFailed,    // The integrity hash of the downloaded firmware does not match
    SignatureFailed,    // The signature of the downloaded firmware does not match
    ConfigInvalid,      // Invalid configuration (e.g. service key length)
    InvalidImage,       // The uploaded image does not match the selected target
    VerificationFailed  // The post-reboot verification callback rejected the new firmware
};

/**
 * Event transmitted by OtamaticClient: the event type plus an uint8_t payload.
 *
 * The meaning of the payload depends on the event:
 * - UpdateProgress: the progress percentage (0-100).
 * - OperationFailed: the OtamaticClientError code describing the failure.
 * - Any other event: 0 (no payload).
 */
struct OtamaticClientEventData {
    /** The event type. */
    OtamaticClientEvent event = OtamaticClientEvent::CheckingForUpdate;

    /** The event payload (see the struct documentation). */
    uint8_t data = 0;

    OtamaticClientEventData() = default;

    /**
     * Create an event with an optional payload.
     * @param event The event type.
     * @param data The event payload.
     */
    OtamaticClientEventData(OtamaticClientEvent event, uint8_t data = 0) : event(event), data(data) {}
};

