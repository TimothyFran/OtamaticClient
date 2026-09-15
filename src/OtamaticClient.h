#pragma once

#include <Arduino.h>
#include <Update.h>
#include <Client.h>
#include "OtamaticClientEvent.h"
#include "OtamaticVersionCheckData.h"

// Version of the OtamaticClient library itself.
#define OTAMATIC_CLIENT_VERSION "0.1.0"

class OtamaticClient {
public:
    /**
     * Create a client bound to an externally managed Client instance
     * (e.g. WiFiClient, WiFiClientSecure, EthernetClient...).
     * The referenced object must outlive the OtamaticClient instance.
     */
    OtamaticClient(Client& client);
    ~OtamaticClient();

    /**
     * Initialize the library.
     * @param currentFwVersion The current firmware version.
     * @return true if the library was initialized successfully, false otherwise.
     */
    bool begin(uint32_t currentFwVersion, const char* serviceKey);
    
    /**
     * Check if there is a new version available and apply it if needed.
     * Call this method in your loop() function.
     */
    void loop();

    /**
     * Set the interval in milliseconds between two checks.
     * @param interval The interval in milliseconds
     */
    void setCheckInterval(uint32_t interval) { _checkInterval = interval; }

    /**
     * Force a check and update to be performed immediately instead of waiting for the next check interval.
     * @param restartCounter true to restart the interval counter.
     */
    void requestCheckNow(bool restartCounter = true);

    /**
     * Set the callback to be called when an event occurs.
     * @param callback The callback to be called.
     */
    void onEvent(void (*callback)(OtamaticClientEventData));

    /**
     * Get the current firmware version, as provided by the
     * OTAMATIC_FIRMWARE_VERSION define.
     * @return The current firmware version.
     */
    uint32_t firmwareVersion() const { return _firmwareVersion; }

    /**
     * Set the device ID.
     * @param deviceId The device Unique ID.
     */
    void setDeviceId(uint64_t deviceId) { _deviceId = deviceId; }

    /**
     * Get the device ID.
     * @return The device Unique ID.
     */
    uint64_t getDeviceId();

    /**
     * Get the user friendly name of an event.
     * @param event The event.
     * @return The user friendly name of the event.
     */
    static const __FlashStringHelper* getEventName(OtamaticClientEvent event);

    /**
     * Get the user friendly name of an error code.
     * @param error The error code.
     * @return The user friendly name of the error code.
     */
    static const __FlashStringHelper* getErrorName(OtamaticClientError error);

    /**
     * Get the data of the last version check. It is populated when an
     * update is found and cleared at the beginning of every check.
     * @return A const reference to the version check data.
     */
    const OtamaticVersionCheckData& getCheckData() const { return _checkData; }

    /**
     * Check if there is a valid version check result stored, meaning an
     * update was found and not yet applied.
     * @return true if a valid version check data is available.
     */
    bool hasUpdateAvailable() const { return _checkData.isValid(); }

    /**
     * Set the flag to automatically apply the update as soon as it is available.
     * @param enable true to automatically apply the update
     */
    void setAutoUpdate(bool enable) { _autoUpdate = enable; }

    /**
     * Apply the update if available.
     * @return true if the update was applied, false otherwise.
     */
    bool applyUpdate();

    /**
     * Set the flag to automatically restart the device after an update.
     * @param enable true to automatically restart the device
     */
    void setAutoRestart(bool enable) { _autoRestart = enable; }

    /**
     * Set the ECDSA public key used to verify the firmware signature
     * (ECDSA over NIST P-256 with SHA-256 on the downloaded firmware).
     * @param key The public key in PEM or Base64-encoded DER (SPKI) format.
     */
    void setPublicKey(const char* key);

private:

    const char* _serverHost = "192.168.1.17";
    uint16_t _serverPort = 8000;

    uint32_t _checkInterval = 5 * 60 * 1000;
    uint32_t _lastCheck = 0;
    uint32_t _firmwareVersion = 0;
    char _serviceKey[49] = {0};
    char _publicKey[512] = {0};
    bool _autoUpdate = true;
    bool _autoRestart = true;

    uint64_t _deviceId = 0;

    Client* _client = nullptr;
    void (*_onEvent)(OtamaticClientEventData) = nullptr;

    OtamaticVersionCheckData _checkData;

    /** Maximum time in milliseconds to wait for incoming data before considering the download stalled. */
    static constexpr uint32_t kUpdateStallTimeout = 10000;

    /**
     * Verify the SHA-256 integrity hash of the downloaded firmware against
     * the value provided by the server.
     * @param integrityHash The computed SHA-256 digest (32 bytes).
     * @return true if the integrity check passed.
     */
    bool verifyIntegrity(const uint8_t* integrityHash);

    /**
     * Verify the ECDSA signature of the downloaded firmware over its
     * SHA-256 digest, using the configured public key.
     * @param integrityHash The computed SHA-256 digest (32 bytes).
     * @return true if the signature check passed.
     */
    bool verifySignature(const uint8_t* integrityHash);

    void transmitEvent(OtamaticClientEvent event, uint8_t data = 0) { if (_onEvent) _onEvent(OtamaticClientEventData(event, data)); }

};