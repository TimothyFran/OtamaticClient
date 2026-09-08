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

