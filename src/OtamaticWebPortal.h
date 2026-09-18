#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <type_traits>
#include "OtamaticClientEvent.h"

class OtamaticClient;

/**
 * Local firmware upload portal. Serves a page from flash (no filesystem)
 * to upload a firmware binary from a browser.
 * Integrity and signature checks are skipped; update events are emitted
 * as usual. Managed through OtamaticClient::startPortal()/stopPortal().
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
     */
    bool start(uint32_t timeoutMs = 0);

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

    /** State shared between the web-server and loop tasks (access via withState()). */
    struct SharedState {
        UploadState state = UploadState::Idle;
        /** True while inside an Update.* call; teardown must be deferred. */
        bool inFlight = false;
        AbortOwner abortOwner = AbortOwner::None;
        /** Outcome of finalizeUpdate(), valid when state == Done. */
        bool succeeded = false;
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

    OtamaticClient& _client;
    AsyncWebServer* _server = nullptr;

    bool _active = false;
    uint32_t _timeoutMs = 0;
    uint32_t _startedAt = 0;

    mutable portMUX_TYPE _stateLock = portMUX_INITIALIZER_UNLOCKED;
    SharedState _shared;

    static constexpr uint32_t kUploadStallTimeout = 10000;
    static constexpr uint32_t kPostUploadGrace = 2000;
    static constexpr uint16_t kPortalPort = 80;
};
