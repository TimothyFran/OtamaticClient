#include "OtamaticWebPortal.h"
#include "OtamaticClient.h"
#include "PortalPage.h"
#include <Update.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

// First interface that is up with a valid IPv4 address (station, soft-AP, wired).
// netif_default is not enough: with station disconnected and soft-AP up it
// still points at the station.
static IPAddress getLocalIp() {
    struct netif* iface = nullptr;
    NETIF_FOREACH(iface) {
        if (! netif_is_up(iface)) continue;
        const ip4_addr_t* addr = netif_ip4_addr(iface);
        if (addr != nullptr && addr->addr != 0) {
            return IPAddress(addr->addr);
        }
    }
    // No interface up with a valid address yet.
    return IPAddress(IPADDR_NONE);
}

/** Hidden form field with the firmware size (keep in sync with PortalPage.h). */
static const char kSizeField[] = "filesize";

/** Parse the browser-declared firmware size (0 = absent or invalid). */
static uint32_t parseDeclaredSize(const String& value) {
    if (value.length() == 0 || value.length() > 10) return 0;
    uint32_t size = 0;
    for (size_t i = 0; i < value.length(); i++) {
        const char c = value[i];
        if (c < '0' || c > '9') return 0;
        const uint32_t digit = (uint32_t)(c - '0');
        if (size > (0xFFFFFFFFUL - digit) / 10) return 0;  // overflow guard
        size = size * 10 + digit;
    }
    return size;
}

/**
 * Append src escaped for JSON (" -> \", backslash, control chars -> \u00XX)
 * into dst (always NUL-terminated). Returns false when truncated.
 */
static bool appendJsonEscaped(char* dst, size_t dstSize, size_t& pos, const char* src) {
    for (const char* p = src; *p != '\0'; p++) {
        const char c = *p;
        const char* esc = nullptr;
        char uni[7] = {0};
        if (c == '"' || c == '\\') {
            if (pos + 2 >= dstSize) return false;
            dst[pos++] = '\\';
            dst[pos++] = c;
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c == '\t') {
            esc = "\\t";
        } else if ((unsigned char)c < 0x20) {
            snprintf(uni, sizeof(uni), "\\u%04x", (unsigned)c);
            esc = uni;
        }
        if (esc != nullptr) {
            const size_t len = strlen(esc);
            if (pos + len >= dstSize) return false;
            memcpy(dst + pos, esc, len);
            pos += len;
        } else if (esc == nullptr && c != '"' && c != '\\') {
            if (pos + 1 >= dstSize) return false;
            dst[pos++] = c;
        }
    }
    dst[pos] = '\0';
    return true;
}

OtamaticWebPortal::OtamaticWebPortal(OtamaticClient& client) : _client(client) {}

OtamaticWebPortal::~OtamaticWebPortal() {
    stop();
}

bool OtamaticWebPortal::start(uint32_t timeoutMs) {
    if (_active) {
        log_w("Portal already active, keeping the current instance");
        return true;
    }

    const IPAddress localIp = getLocalIp();
    if (localIp == IPAddress(IPADDR_NONE)) {
        log_e("Portal requires an active network interface (Wi-Fi or wired)");
        _client.transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::ConnectionFailed);
        return false;
    }

    _server = new AsyncWebServer(kPortalPort);

    // Update form, served gzipped from flash.
    _server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html", index_html_gz, index_html_gz_len);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    _server->on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        // Fixed numeric part: {"deviceId":"<20>","firmwareVersion":<10>}
        // plus optional ,"versionName":"<31*6>" ,"portalTitle":"<31*6>"
        // (worst case: every char escaped as \u00XX). Stack-only, no JSON lib.
        // 64B covers the numeric part with margin; snprintf never truncates it.
        char payload[64 + 2 * (15 + 31 * 6)];
        size_t pos = (size_t)snprintf(payload, sizeof(payload), "{\"deviceId\":\"%llu\",\"firmwareVersion\":%lu",
                 (unsigned long long)_client.getDeviceId(),
                 (unsigned long)_client.firmwareVersion());
            const char* extra[2] = {_client.versionName(), _client.portalTitle()};
            const char* keys[2] = {"versionName", "portalTitle"};
            for (int i = 0; i < 2; i++) {
                if (extra[i][0] == '\0') continue;
                const size_t keyLen = strlen(keys[i]);
                if (pos + 4 + keyLen >= sizeof(payload)) break;
                payload[pos++] = ',';
                payload[pos++] = '"';
                memcpy(payload + pos, keys[i], keyLen);
                pos += keyLen;
                payload[pos++] = '"';
                payload[pos++] = ':';
                payload[pos++] = '"';
                if (! appendJsonEscaped(payload, sizeof(payload), pos, extra[i])) break;
                if (pos + 1 >= sizeof(payload)) break;
                payload[pos++] = '"';
                payload[pos] = '\0';
            }
            if (pos + 1 < sizeof(payload)) {
                payload[pos++] = '}';
                payload[pos] = '\0';
            }
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", payload);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Firmware upload: the request handler reports the final outcome.
    _server->on("/update", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            bool done = false;
            bool succeeded = false;
            withState([&](SharedState& s) {
                done = s.state == UploadState::Done;
                succeeded = s.succeeded;
            });
            if (! done) {
                request->send(503, "text/plain", "No completed upload for this request");
                return;
            }
            AsyncWebServerResponse* response = request->beginResponse(
                succeeded ? 200 : 500, "text/plain",
                succeeded ? "OK - firmware written, the device will restart"
                          : "FAIL - update error");
            response->addHeader("Connection", "close");
            request->send(response);
        },
        [this](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleUpload(request, filename, index, data, len, final);
        });

    _server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "Not found");
    });

    _server->begin();

    _active = true;
    _timeoutMs = timeoutMs;
    _startedAt = millis();
    withState([&](SharedState& s) {
        s.state = UploadState::Idle;
        s.inFlight = false;
        s.abortOwner = AbortOwner::None;
        s.succeeded = false;
        s.bytesWritten = 0;
    });
    log_i("Update portal started on http://%s/ (timeout: %lu ms)",
          localIp.toString().c_str(), (unsigned long)_timeoutMs);
    return true;
}

void OtamaticWebPortal::stop() {
    if (! _active) return;

    // Defer teardown to the async task when it is inside an Update.* call.
    bool inProgress = false;
    bool teardownNow = false;
    withState([&](SharedState& s) {
        if (s.state == UploadState::InProgress || s.state == UploadState::AbortRequested) {
            inProgress = true;
            if (s.inFlight) {
                s.state = UploadState::AbortRequested;
                s.abortOwner = AbortOwner::Upload;
            } else {
                s.state = UploadState::Idle;
                s.abortOwner = AbortOwner::None;
                _client._busy = false;
                teardownNow = true;
            }
        }
    });

    if (inProgress && ! teardownNow) {
        log_w("Portal stop deferred: the update pipeline is owned by the async task, the teardown will be performed there");
    } else if (teardownNow) {
        log_w("Portal stopped while an upload was in progress, aborting the update");
        _client.abortUpdate(OtamaticClientError::DownloadFailed);
    }

    if (_server != nullptr) {
        _server->end();
        delete _server;
        _server = nullptr;
    }
    _active = false;
    log_i("Update portal stopped");
}

void OtamaticWebPortal::handleLoop() {
    if (! _active) return;

    // Abort stalled uploads so the pipeline does not stay busy forever.
    bool teardownNow = false;
    uint32_t stalledForMs = 0;
    withState([&](SharedState& s) {
        if (s.state != UploadState::InProgress) return;
        const uint32_t elapsed = millis() - s.lastChunkAt;
        if (elapsed <= kUploadStallTimeout) return;
        stalledForMs = elapsed;
        s.state = UploadState::AbortRequested;
        s.abortOwner = s.inFlight ? AbortOwner::Upload : AbortOwner::Loop;
        teardownNow = s.abortOwner == AbortOwner::Loop;
    });

    if (teardownNow) {
        log_e("Upload stalled (no chunks for %lu ms), aborting", (unsigned long)stalledForMs);
        _client.abortUpdate(OtamaticClientError::DownloadFailed);
        withState([&](SharedState& s) {
            s.state = UploadState::Idle;
            s.abortOwner = AbortOwner::None;
            _client._busy = false;
        });
        return;
    }

    // Give the HTTP response time to reach the browser before stopping or rebooting.
    bool done = false;
    bool restart = false;
    uint32_t doneAt = 0;
    withState([&](SharedState& s) {
        if (s.state != UploadState::Done) return;
        done = true;
        doneAt = s.doneAt;
        restart = s.succeeded && _client._autoRestart;
    });
    if (done) {
        if (millis() - doneAt >= kPostUploadGrace) {
            if (restart) {
                log_i("Rebooting after portal update");
                ESP.restart();
            }
            stop();
        }
        return;
    }

    if (_timeoutMs > 0 && millis() - _startedAt >= _timeoutMs) {
        log_w("Portal timeout reached, stopping");
        stop();
    }
}

void OtamaticWebPortal::claimPendingAbort(OtamaticClientError reason) {
    const bool claimed = withState([&](SharedState& s) {
        if (s.state != UploadState::AbortRequested || s.abortOwner != AbortOwner::Upload) return false;
        s.state = UploadState::Idle;
        s.abortOwner = AbortOwner::None;
        s.inFlight = false;
        _client._busy = false;
        return true;
    });
    if (claimed) _client.abortUpdate(reason);
}

void OtamaticWebPortal::handleUpload(AsyncWebServerRequest* request, const String& filename,
                                     size_t index, uint8_t* data, size_t len, bool final) {
    (void)filename;

    if (index == 0) {
        bool rejected = false;
        withState([&](SharedState& s) {
            if (s.state != UploadState::Idle) {
                rejected = true;
                return;
            }

            if (_client._busy.exchange(true)) {
                rejected = true;
                return;
            }

            s.state = UploadState::InProgress;
            s.lastChunkAt = millis();
            s.bytesWritten = 0;
            s.succeeded = false;
            s.abortOwner = AbortOwner::None;
        });
        if (rejected) {
            log_w("Upload rejected, another update is in progress");
            return;
        }

        // Optional browser-declared size; falls back to unknown size when absent.
        const uint32_t declaredSize = parseDeclaredSize(request->arg(kSizeField));
        const uint32_t firmwareSize = declaredSize > 0 ? declaredSize : UPDATE_SIZE_UNKNOWN;

        const uint32_t progressTotal = declaredSize > 0 ? declaredSize : request->contentLength();

        if (! _client.beginUpdate(firmwareSize, progressTotal)) {
            withState([&](SharedState& s) {
                s.state = UploadState::Idle;
                s.abortOwner = AbortOwner::None;
                _client._busy = false;
            });
            return;
        }
    }

    // ---------------- chunk write phase ----------------
    if (len > 0) {
        bool proceed = false;
        withState([&](SharedState& s) {
            if (s.state == UploadState::InProgress) {
                s.inFlight = true;
                s.lastChunkAt = millis();
                proceed = true;
            }
        });
        if (! proceed) {
            claimPendingAbort(OtamaticClientError::DownloadFailed);
            return;
        }

        const bool ok = _client.writeUpdateChunk(data, len);

        bool doAbort = false;
        OtamaticClientError abortReason = OtamaticClientError::DownloadFailed;
        withState([&](SharedState& s) {
            s.inFlight = false;
            if (s.state == UploadState::AbortRequested) {
                s.state = UploadState::Idle;
                s.abortOwner = AbortOwner::None;
                _client._busy = false;
                doAbort = true;
                abortReason = ok ? OtamaticClientError::DownloadFailed
                                 : OtamaticClientError::WriteFailed;
            } else if (! ok && s.state == UploadState::InProgress) {
                s.state = UploadState::Idle;
                _client._busy = false;
                doAbort = true;
                abortReason = OtamaticClientError::WriteFailed;
            } else if (ok) {
                s.bytesWritten += len;
            }
        });
        if (doAbort) {
            _client.abortUpdate(abortReason);
            return;
        }
        if (! ok) return;  // teardown handled by the abort owner
    }

    if (final) {
        bool startFinalize = false;
        withState([&](SharedState& s) {
            if (s.state == UploadState::InProgress) {
                s.state = UploadState::Finalizing;
                s.inFlight = true;
                startFinalize = true;
            }
        });
        if (! startFinalize) {
            claimPendingAbort(OtamaticClientError::DownloadFailed);
            return;
        }

        // With unknown size progress stops at 99%; emit the final 100% here.
        if (parseDeclaredSize(request->arg(kSizeField)) == 0) {
            _client.transmitEvent(OtamaticClientEvent::UpdateProgress, 100);
        }
        const bool succeeded = _client.finalizeUpdate(false, false);

        withState([&](SharedState& s) {
            s.state = UploadState::Done;
            s.inFlight = false;
            s.succeeded = succeeded;
            s.doneAt = millis();
            s.abortOwner = AbortOwner::None;
            _client._busy = false;
        });
        const size_t written = withState([](SharedState& s) { return s.bytesWritten; });
        log_i("Portal upload %s (%u bytes)", succeeded ? "completed" : "failed", (unsigned)written);
    }
}
