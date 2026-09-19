#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <stdarg.h>
#include <type_traits>
#include "OtamaticClientEvent.h"
#include "OtamaticUpdateTarget.h"

class OtamaticClient;

/**
 * Local update portal. Serves a page from flash (no filesystem) to write an
 * image to this device from a browser: an application image (firmware, written
 * to the next OTA partition) or a filesystem image such as LittleFS/SPIFFS
 * (written to the filesystem partition). Integrity and signature checks are
 * skipped; update events are emitted as usual.
 * Managed through OtamaticClient::startPortal()/stopPortal().
 */
class OtamaticWebPortal {
public:
    explicit OtamaticWebPortal(OtamaticClient& client);
    ~OtamaticWebPortal();

    OtamaticWebPortal(const OtamaticWebPortal&) = delete;
    OtamaticWebPortal& operator=(const OtamaticWebPortal&) = delete;

    /**
     * Start the portal. Call from loop().
     * Requires a SoC network interface (Wi-Fi or wired), not an external modem.
     * @param timeoutMs Inactivity timeout in ms (0 = disabled).
     * @param username HTTP Basic auth username (nullptr = auth disabled).
     * @param password HTTP Basic auth password (nullptr = auth disabled).
     * Auth is active only when BOTH are non-null and non-empty.
     */
    bool start(uint32_t timeoutMs = 0, const char* username = nullptr,
               const char* password = nullptr);

    /** Stop the portal, aborting any upload in progress. Call from loop(). */
    void stop();

    /** @return true if the portal is running. */
    bool isActive() const { return _active; }

    /** Service timeouts and post-upload restart. Call from loop(). */
    void handleLoop();

private:
    void handleUpload(AsyncWebServerRequest* request, const String& filename,
                      size_t index, uint8_t* data, size_t len, bool final);

    enum class UploadState : uint8_t {
        Idle,
        InProgress,
        AbortRequested,
        Finalizing,
        Done
    };

    /** Owner of a pending abort. */
    enum class AbortOwner : uint8_t { None, Loop, Upload };

    /** Longest failure message reported to the browser. */
    static constexpr size_t kUploadErrorMaxLen = 96;

    /** State shared between the web-server and loop tasks (access via withState()). */
    struct SharedState {
        UploadState state = UploadState::Idle;
        /** True while inside an Update.* call; teardown must be deferred. */
        bool inFlight = false;
        AbortOwner abortOwner = AbortOwner::None;
        /** Outcome of finalizeUpdate(), valid when state == Done. */
        bool succeeded = false;
        /** Partition the upload writes to, valid from the first chunk. */
        OtamaticUpdateTarget target = OtamaticUpdateTarget::Firmware;
        /** Failure message reported to the browser (empty = none). */
        char error[kUploadErrorMaxLen] = {0};
        uint32_t lastChunkAt = 0;
        /** Upload completion time, valid when state == Done. */
        uint32_t doneAt = 0;
        size_t bytesWritten = 0;
    };

    template <typename S, typename F>
    static auto invokeLocked(portMUX_TYPE& lock, S& state, F& fn) -> decltype(fn(state)) {
        using R = decltype(fn(state));
        portENTER_CRITICAL(&lock);
        if constexpr (std::is_void<R>::value) {
            fn(state);
            portEXIT_CRITICAL(&lock);
        } else {
            R result = fn(state);
            portEXIT_CRITICAL(&lock);
            return result;
        }
    }

    template <typename F> auto withState(F&& fn) {
        return invokeLocked(_stateLock, _shared, fn);
    }

    void claimPendingAbort(OtamaticClientError reason);

    /** Store the failure message reported to the browser (empty string clears it). */
    void setUploadError(const char* format, ...) __attribute__((format(printf, 2, 3)));

    /**
     * Refuse an upload before it starts: record and log the reason (shown to the
     * browser by the POST response handler) and emit OperationFailed/InvalidImage.
     */
    void rejectUpload(const char* format, ...) __attribute__((format(printf, 2, 3)));

    OtamaticClient& _client;
    AsyncWebServer* _server = nullptr;

    bool _active = false;
    uint32_t _timeoutMs = 0;
    uint32_t _startedAt = 0;

    mutable portMUX_TYPE _stateLock = portMUX_INITIALIZER_UNLOCKED;
    SharedState _shared;

    /** Returns true when a request passed HTTP Basic auth (or auth disabled). */
    bool checkAuth(AsyncWebServerRequest* request) const;

    static constexpr size_t kAuthMaxLen = 33;  // 32 chars + NUL
    char _authUser[kAuthMaxLen] = {0};
    char _authPass[kAuthMaxLen] = {0};
    static constexpr char kAuthRealm[] = "Otamatic";

    static constexpr uint32_t kUploadStallTimeout = 10000;
    static constexpr uint32_t kPostUploadGrace = 2000;
    static constexpr uint16_t kPortalPort = 80;
};
