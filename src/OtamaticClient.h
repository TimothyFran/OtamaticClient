#pragma once

#include <Arduino.h>
#include <Update.h>
#include <Client.h>
#include <atomic>
#include "OtamaticClientEvent.h"
#include "OtamaticUpdateTarget.h"
#include "OtamaticVersionCheckData.h"

class OtamaticWebPortal;

// Version of the OtamaticClient library itself.
#define OTAMATIC_CLIENT_VERSION "0.3.1"

class OtamaticClient {
public:
    /** Create an unbound client. Call setClient() before begin(). */
    OtamaticClient() = default;
    ~OtamaticClient();

    /**
     * Bind an externally managed transport. Settings survive rebinds.
     * @param client Must not be nullptr and must outlive this instance.
     */
    bool setClient(Client* client);

    Client* getClient() const { return _client; }

    /** Initialize the library. @param serviceKey 48-character service key. */
    bool begin(uint32_t currentFwVersion, const char* serviceKey);

    /** Check for updates and service the portal. Call from loop(). */
    void loop();

    /** Set the interval between version checks. */
    void setCheckInterval(uint32_t interval) { _checkInterval = interval; }

    /** Run a version check immediately. @param restartCounter Restart the interval timer. */
    void requestCheckNow(bool restartCounter = true);

    /** Set the event callback. */
    void onEvent(void (*callback)(OtamaticClientEventData));

    uint32_t firmwareVersion() const { return _firmwareVersion; }

    /**
     * Optional human-readable version name shown in the portal
     * (e.g. "v1.2.3-beta"). Empty by default (field hidden).
     * Copied internally (31 chars max, truncated).
     */
    void setVersionName(const char* name) {
        if (name == nullptr) _versionName[0] = '\0';
        else strlcpy(_versionName, name, sizeof(_versionName));
    }

    const char* versionName() const { return _versionName; }

    /**
     * Optional portal page title (H1 + <title>). Empty by default,
     * keeping "Firmware update" / "Otamatic update".
     * Copied internally (31 chars max, truncated).
     */
    void setPortalTitle(const char* title) {
        if (title == nullptr) _portalTitle[0] = '\0';
        else strlcpy(_portalTitle, title, sizeof(_portalTitle));
    }

    const char* portalTitle() const { return _portalTitle; }

    /** Set a custom device ID (default: derived from the Wi-Fi MAC). */
    void setDeviceId(uint64_t deviceId) { _deviceId = deviceId; }

    uint64_t getDeviceId();

    /** User-friendly event name. */
    static const __FlashStringHelper* getEventName(OtamaticClientEvent event);

    /** User-friendly error name. */
    static const __FlashStringHelper* getErrorName(OtamaticClientError error);

    /** Data of the last version check (valid when an update was found). */
    const OtamaticVersionCheckData& getCheckData() const { return _checkData; }

    /** @return true if an update was found and not yet applied. */
    bool hasUpdateAvailable() const { return _checkData.isValid(); }

    /** Apply a found update automatically (default: enabled). */
    void setAutoUpdate(bool enable) { _autoUpdate = enable; }

    /** Apply the pending update. @return true if applied. */
    bool applyUpdate();

    /** Restart automatically after an update (default: enabled). */
    void setAutoRestart(bool enable) { _autoRestart = enable; }

    /**
     * Override the update server. @param host Hostname or IP (copied internally).
     * @return false if host is null or empty.
     */
    bool setServer(const char* host, uint16_t port = 80) {
        if (host == nullptr || host[0] == '\0') return false;
        strlcpy(_serverHostBuf, host, sizeof(_serverHostBuf));
        _serverHost = _serverHostBuf;
        _serverPort = port;
        return true;
    }

    const char* getServerHost() const { return _serverHost; }

    uint16_t getServerPort() const { return _serverPort; }

    /** Reject updates without a server signature when enabled. */
    void setRequireSignature(bool enable) { _requireSignature = enable; }

    /** Set the ECDSA public key (PEM or Base64 DER) for signature verification. */
    void setPublicKey(const char* key);

    /**
     * Start the local update portal (browser upload of an image).
     * Requires an active SoC network interface (Wi-Fi or wired).
     * @param timeoutMs Inactivity timeout in ms (0 = no timeout).
     * @param username HTTP Basic auth username (nullptr = auth disabled).
     * @param password HTTP Basic auth password (nullptr = auth disabled).
     */
    bool startPortal(uint32_t timeoutMs = 0, const char* username = nullptr, const char* password = nullptr);

    /** Stop the portal, aborting any upload in progress. */
    void stopPortal();

    /** @return true if the portal is active. */
    bool isPortalActive() const;

private:

    static constexpr size_t kServiceKeyMaxLen = 49;
    static constexpr const char* kBearerPrefix = "Bearer ";
    static constexpr size_t kBearerTokenMaxLen = kServiceKeyMaxLen + 7;

    static constexpr size_t kServerHostMaxLen = 64;
    const char* _serverHost = "otamatic.eu";
    char _serverHostBuf[kServerHostMaxLen] = {0};
    uint16_t _serverPort = 80;

    static constexpr size_t kPortalTextMaxLen = 32;
    char _versionName[kPortalTextMaxLen] = {0};
    char _portalTitle[kPortalTextMaxLen] = {0};

    uint32_t _checkInterval = 5 * 60 * 1000;
    uint32_t _lastCheck = 0;
    uint32_t _firmwareVersion = 0;
    char _serviceKey[kServiceKeyMaxLen] = {0};
    char _publicKey[512] = {0};
    bool _autoUpdate = true;
    bool _autoRestart = true;

    bool _requireSignature = false;

    uint64_t _deviceId = 0;

    Client* _client = nullptr;
    void (*_onEvent)(OtamaticClientEventData) = nullptr;

    OtamaticVersionCheckData _checkData;

    /** OTA pipeline owner (remote download or portal upload): at most one
     *  writer at a time. Claimed atomically; not held during a version check. */
    std::atomic<bool> _busy{false};

    /** Transport owner (version check or remote download): guards the shared
     *  Client against concurrent use. The portal upload does not use it. */
    std::atomic<bool> _transportBusy{false};

    static constexpr uint32_t kUpdateStallTimeout = 10000;

    void requestCheckNowInternal(bool restartCounter);

    bool applyUpdateInternal();

    uint32_t getOtaPartitionSize() const;

    /** Size of the filesystem partition (LittleFS/SPIFFS, FAT as fallback), 0 if absent. */
    uint32_t getFilesystemPartitionSize() const;

    void buildBearerToken(char* buffer, size_t bufferSize) const;

    bool verifyIntegrity(const uint8_t* integrityHash);

    bool verifySignature(const uint8_t* integrityHash);

    void transmitEvent(OtamaticClientEvent event, uint8_t data = 0) { if (_onEvent) _onEvent(OtamaticClientEventData(event, data)); }

    bool beginUpdate(uint32_t size, uint32_t progressTotal, OtamaticUpdateTarget target);

    bool writeUpdateChunk(const uint8_t* data, size_t len);

    bool finalizeUpdate(bool checkIntegrity, bool checkSignature);

    void abortUpdate(OtamaticClientError reason);

    struct UpdateState;
    UpdateState* _updateState = nullptr;

    OtamaticWebPortal* _portal = nullptr;

    friend class OtamaticWebPortal;


};