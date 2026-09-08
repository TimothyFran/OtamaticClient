#pragma once

#include <Arduino.h>

/**
 * Events emitted by OtamaticClient during the check/update cycle.
 */
enum class OtamaticClientEvent {
    CheckingForUpdate,  // Start of a version check cycle
    UpdateAvailable,    // A new firmware version was found
    UpdateNotNeeded,    // No update available
    UpdateStarted,      // Writing of the new firmware has started
    UpdateProgress,     // Firmware write progress
    UpdateCompleted,    // Update completed (reboot imminent)
    UpdateFailed        // Update failed
};

/**
 * Error codes transmitted as the payload of OtamaticClientEvent::UpdateFailed.
 */
enum class OtamaticClientError : uint8_t {
    None = 0,           // No error (used as default payload)
    InvalidCheckData,   // No valid version check result when applying the update
    ConnectionFailed,   // Could not establish the connection to the server
    HttpFailed,         // The server answered with an unexpected HTTP status
    InvalidFirmware,    // The firmware binary size is invalid
    BeginFailed,        // Could not reserve the OTA partition (no space, partition issue...)
    DownloadFailed,     // Download stalled or connection dropped
    WriteFailed,        // Error while writing the firmware into the OTA partition
    EndFailed           // Final image verification failed
};

/**
 * Event transmitted by OtamaticClient: the event type plus an uint8_t payload.
 *
 * The meaning of the payload depends on the event:
 * - UpdateProgress: the progress percentage (0-100).
 * - UpdateFailed:   the OtamaticClientError code describing the failure.
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

