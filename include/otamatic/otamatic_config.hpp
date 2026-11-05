#pragma once

#include <otamatic/otamatic_client.hpp>
#include <string>

/**
 * @brief Configuration for the Otamatic OTA client.
 *
 * Each field can be set before initializing the client;
 * fields with a default value are optional.
 */
struct OtamaticConfig {
    /// Account API key. Required: if nullptr the client cannot authenticate.
    const char* apiKey = nullptr;

    /// Unique device ID, used by the server to identify this device.
    uint64_t deviceId = 0;

    /// Base URL of the API. Change only for self-hosted instances.
    const char* apiBaseUrl = "https://otamatic.eu/api";

    /// Interval (in milliseconds) between update checks. Default: 300000 ms (5 min).
    uint32_t checkInterval = 300000;

    /// Human-readable version name of the currently running firmware (e.g. "1.0.0").
    const char* versionName = nullptr;

    /// Monotonically increasing integer firmware version, used for version comparisons.
    uint32_t firmwareVersion = 0;

    /// If true, downloads and applies updates automatically without confirmation.
    bool autoUpdateEnabled = true;

    /// If true, restarts the device after an update completes.
    bool autoRestartEnabled = true;

    /// If true, verifies the new firmware boots correctly (with automatic rollback on failure).
    bool rollbackVerificationEnabled = true;

    /// If true, keeps skipping a version that already caused failures (prevents update loops).
    bool failedVersionGuardEnabled = true;

    /// Maximum number of failed update attempts before the version is blocked by the guard.
    uint8_t maxUpdateFailedAttempts = 3;

    /// If true, only accepts updates signed/certified by the server.
    bool requireSignedUpdates = false;

    /// Public key (PEM "-----BEGIN PUBLIC KEY-----" or base64/raw DER, ECDSA P-256)
    /// used to verify the firmware signature. Required when requireSignedUpdates is true.
    const char* publicKey = nullptr;

    /// If true, enables automatic periodic update checks
    bool enableAutoCheckMode = true;

    /// Size of the task stack for the automatic update check task. Default: 10240.
    uint32_t autoCheckTaskStackSize = 10240;


    /// Serializes the configuration to JSON.
    // TODO: Aggiornare con tutti i valori
    size_t toJson(char* buffer, size_t bufferSize) const {
        size_t n = snprintf(buffer, bufferSize, "{\"apiKey\":\"%s\",\"deviceId\":\"%llu\",\"apiBaseUrl\":\"%s\",\"checkInterval\":%u,\"versionName\":\"%s\",\"firmwareVersion\":%u,\"autoUpdateEnabled\":%s,\"autoRestartEnabled\":%s,\"rollbackVerificationEnabled\":%s,\"failedVersionGuardEnabled\":%s,\"maxUpdateFailedAttempts\":%u,\"requireSignedUpdates\":%s}",
            apiKey,
            static_cast<unsigned long long>(deviceId),
            apiBaseUrl,
            static_cast<unsigned int>(checkInterval),
            versionName,
            static_cast<unsigned int>(firmwareVersion),
            autoUpdateEnabled ? "true" : "false",
            autoRestartEnabled ? "true" : "false",
            rollbackVerificationEnabled ? "true" : "false",
            failedVersionGuardEnabled ? "true" : "false",
            static_cast<unsigned int>(maxUpdateFailedAttempts),
            requireSignedUpdates ? "true" : "false");
        return n;
    }
    
    /// Serializes the configuration to JSON.
    std::string toJson() const {
        size_t needed = toJson(nullptr, 0);
        std::string result(needed, '\0');
        toJson(&result[0], needed);
        return result;
    }
};