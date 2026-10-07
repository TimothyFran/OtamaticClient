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
 *
 * The web server is owned by the user and must outlive this client: start()
 * only attaches the routes (GET/POST <endpoint> and GET /api/info) to it and
 * stop() detaches them. Call start() again to reconfigure (new server or
 * endpoint). Managed through OtamaticClient::startPortal()/stopPortal().
 */
class OtamaticWebPortal {
public:
    explicit OtamaticWebPortal(OtamaticClient& client);
    ~OtamaticWebPortal();

    OtamaticWebPortal(const OtamaticWebPortal&) = delete;
    OtamaticWebPortal& operator=(const OtamaticWebPortal&) = delete;

    /**
     * Attach the portal routes to a user-owned server. Call from loop().
     * Registers GET <endpoint> (the page), POST <endpoint> (the upload) and
     * GET /api/info (device info). The server is not started here: the caller
     * owns it and is responsible for server.begin()/end(). Call again to
     * reconfigure with a different server or endpoint.
     * @param server User-owned server (must not be nullptr).
     * @param endpoint Path of the page and upload ("/" if null/empty).
     * @param timeoutMs Inactivity timeout in ms (0 = disabled).
     */
    bool start(AsyncWebServer* server, const char* endpoint = "/", uint32_t timeoutMs = 0);

    /** Detach the portal routes, aborting any upload in progress. Call from loop(). */
    void stop();

    /** @return true if the portal routes are attached. */
    bool isActive() const { return _active; }

    /** Service timeouts and post-upload restart. Call from loop(). */
    void handleLoop();

private:
    void handleUpload(AsyncWebServerRequest* request, const String& filename,
                      size_t index, uint8_t* data, size_t len, bool final);

    /** Remove the routes registered by start() from the current server. */
    void detachRoutes();

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

    /** User-owned server (borrowed: never created, started or deleted here). */
    AsyncWebServer* _server = nullptr;
    /** Handlers registered by start(), kept so stop() can detach them. */
    AsyncWebHandler* _pageHandler = nullptr;
    AsyncWebHandler* _uploadHandler = nullptr;
    AsyncWebHandler* _infoHandler = nullptr;

    bool _active = false;
    uint32_t _timeoutMs = 0;
    uint32_t _startedAt = 0;

    mutable portMUX_TYPE _stateLock = portMUX_INITIALIZER_UNLOCKED;
    SharedState _shared;

    static constexpr uint32_t kUploadStallTimeout = 10000;
    static constexpr uint32_t kPostUploadGrace = 2000;
};
