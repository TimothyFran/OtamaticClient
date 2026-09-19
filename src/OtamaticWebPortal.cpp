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

/** Hidden form fields (keep in sync with extras/index.html). */
static const char kSizeField[] = "filesize";
static const char kTargetField[] = "target";

/** First byte of an ESP32 application image (ESP_IMAGE_HEADER_MAGIC). */
static constexpr uint8_t kAppImageMagic = 0xE9;

/**
 * LittleFS magic. The littlefs specification guarantees that the "littlefs"
 * string of the superblock always sits at offset 8 of a valid image, so it
 * identifies the image without relying on its extension.
 */
static const char kLittlefsMagic[] = "littlefs";
static constexpr size_t kLittlefsMagicOffset = 8;

/** Target selected on the portal page. */
enum class RequestedTarget : uint8_t { Auto, Firmware, Filesystem };

/** Image kind recognized from the first bytes of an upload. */
enum class ImageKind : uint8_t { Unknown, Firmware, Filesystem };

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

/** Parse the "target" form field: anything but firmware/filesystem means auto. */
static RequestedTarget parseRequestedTarget(const String& value) {
    if (value.length() == 0 || value.length() > 10) return RequestedTarget::Auto;
    if (strcasecmp(value.c_str(), "firmware") == 0) return RequestedTarget::Firmware;
    if (strcasecmp(value.c_str(), "filesystem") == 0) return RequestedTarget::Filesystem;
    return RequestedTarget::Auto;
}

/**
 * Recognize an image from its first bytes (mirrored by the portal page
 * JavaScript, keep the two in sync):
 * - ESP32 application images start with the ESP_IMAGE_HEADER_MAGIC byte;
 * - LittleFS images carry the "littlefs" magic at offset 8;
 * - SPIFFS and FAT images have no marker at all: they stay Unknown and the page
 *   has to state the target explicitly.
 */
static ImageKind identifyImage(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return ImageKind::Unknown;
    if (data[0] == kAppImageMagic) return ImageKind::Firmware;
    if (len >= kLittlefsMagicOffset + sizeof(kLittlefsMagic) - 1 &&
        memcmp(data + kLittlefsMagicOffset, kLittlefsMagic, sizeof(kLittlefsMagic) - 1) == 0) {
        return ImageKind::Filesystem;
    }
    return ImageKind::Unknown;
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

bool OtamaticWebPortal::start(uint32_t timeoutMs, const char* username, const char* password) {
    if (_active) {
        log_w("Portal already active, keeping the current instance");
        return true;
    }

    const bool authEnabled = username != nullptr && username[0] != '\0' &&
                             password != nullptr && password[0] != '\0';
    if ((username != nullptr || password != nullptr) && ! authEnabled) {
        log_w("Portal auth needs both username and password, starting without auth");
    }
    strlcpy(_authUser, authEnabled ? username : "", sizeof(_authUser));
    strlcpy(_authPass, authEnabled ? password : "", sizeof(_authPass));

    const IPAddress localIp = getLocalIp();
    if (localIp == IPAddress(IPADDR_NONE)) {
        log_e("Portal requires an active network interface (Wi-Fi or wired)");
        _client.transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::ConnectionFailed);
        return false;
    }

    _server = new AsyncWebServer(kPortalPort);

    // Update form, served gzipped from flash.
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (! checkAuth(request)) {
            request->requestAuthentication(AsyncAuthType::AUTH_BASIC, kAuthRealm);
            return;
        }
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html", index_html_gz, index_html_gz_len);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    _server->on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (! checkAuth(request)) {
            request->requestAuthentication(AsyncAuthType::AUTH_BASIC, kAuthRealm);
            return;
        }
        // Fixed numeric part: {"deviceId":"<20>","firmwareVersion":<10>,
        //   "filesystemPartitionSize":<10>} -> 101 bytes worst case.
        // Optional: ,"versionName":"<31*6>" ,"portalTitle":"<31*6>"
        // (worst case: every char escaped as \u00XX). Stack-only, no JSON lib.
        // 128B covers the numeric part with margin; snprintf never truncates it.
        char payload[128 + 2 * (15 + 31 * 6)];
        size_t pos = (size_t)snprintf(payload, sizeof(payload),
                 "{\"deviceId\":\"%llu\",\"firmwareVersion\":%lu,\"filesystemPartitionSize\":%lu",
                 (unsigned long long)_client.getDeviceId(),
                 (unsigned long)_client.firmwareVersion(),
                 (unsigned long)_client.getFilesystemPartitionSize());
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

    // Image upload: the request handler reports the final outcome and, when the
    // upload was refused or failed, the reason collected by the upload handler.
    // NOTE: the multipart body chunks (handleUpload) cannot be auth-gated per
    // chunk without buffering, so auth is checked on the final POST handler and
    // the upload handler rejects chunks when the session was never authed.
    _server->on("/update", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            if (! checkAuth(request)) {
                request->requestAuthentication(AsyncAuthType::AUTH_BASIC, kAuthRealm);
                return;
            }
            bool done = false;
            bool succeeded = false;
            OtamaticUpdateTarget target = OtamaticUpdateTarget::Firmware;
            char error[kUploadErrorMaxLen] = {0};
            withState([&](SharedState& s) {
                done = s.state == UploadState::Done;
                succeeded = s.succeeded;
                target = s.target;
                strlcpy(error, s.error, sizeof(error));
            });

            String body;
            int status = 200;
            if (! done) {
                status = 503;
                body = error[0] != '\0' ? error : "No completed upload for this request";
            } else if (succeeded) {
                body = target == OtamaticUpdateTarget::Filesystem
                           ? "OK - filesystem written, the device will restart"
                           : "OK - firmware written, the device will restart";
            } else {
                status = 500;
                body = error[0] != '\0' ? error : "FAIL - update error";
            }

            AsyncWebServerResponse* response = request->beginResponse(status, "text/plain", body);
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
    log_i("Update portal started on http://%s/ (timeout: %lu ms%s)",
          localIp.toString().c_str(), (unsigned long)_timeoutMs,
          _authUser[0] != '\0' ? ", auth: on" : "");
    return true;
}

bool OtamaticWebPortal::checkAuth(AsyncWebServerRequest* request) const {
    if (_authUser[0] == '\0' || _authPass[0] == '\0') return true;  // auth disabled
    return request->authenticate(_authUser, _authPass);
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
    _authUser[0] = '\0';
    _authPass[0] = '\0';
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

void OtamaticWebPortal::setUploadError(const char* format, ...) {
    char message[kUploadErrorMaxLen];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    withState([&](SharedState& s) { strlcpy(s.error, message, sizeof(s.error)); });
}

void OtamaticWebPortal::rejectUpload(const char* format, ...) {
    char message[kUploadErrorMaxLen];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    withState([&](SharedState& s) { strlcpy(s.error, message, sizeof(s.error)); });
    log_e("Upload rejected: %s", message);
    _client.transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::InvalidImage);
}

void OtamaticWebPortal::handleUpload(AsyncWebServerRequest* request, const String& filename,
                                     size_t index, uint8_t* data, size_t len, bool final) {
    (void)filename;

    // The upload body handler runs per chunk outside the POST response handler:
    // reject unauthenticated streams before touching flash (defense in depth:
    // the POST response handler re-checks auth before reporting success).
    if (! checkAuth(request)) {
        if (index == 0) log_w("Upload rejected, authentication required");
        return;
    }
    if (index == 0) {
        // Decide what is being written before touching flash: the page can force
        // the target, "auto" reads it from the image header.
        const RequestedTarget requested = parseRequestedTarget(request->arg(kTargetField));
        const ImageKind kind = identifyImage(data, len);
        OtamaticUpdateTarget target = OtamaticUpdateTarget::Firmware;

        if (kind == ImageKind::Firmware) {
            // ESP32 application image: only the OTA partition can hold it.
            if (requested == RequestedTarget::Filesystem) {
                rejectUpload("This is an ESP32 firmware image: it cannot be written to the filesystem partition.");
                return;
            }
        } else if (kind == ImageKind::Filesystem) {
            if (requested == RequestedTarget::Firmware) {
                rejectUpload("This is a filesystem image, not a firmware image (no 0xE9 magic byte).");
                return;
            }
            target = OtamaticUpdateTarget::Filesystem;
        } else if (requested == RequestedTarget::Filesystem) {
            // SPIFFS and FAT images carry no marker: trust the page.
            log_w("Filesystem image not recognized (no LittleFS magic), writing it as selected");
            target = OtamaticUpdateTarget::Filesystem;
        } else if (requested == RequestedTarget::Auto) {
            rejectUpload("Cannot tell what this image is: no ESP32 firmware magic (0xE9) nor LittleFS magic.");
            return;
        } else {
            rejectUpload("This file is not an ESP32 firmware image (no 0xE9 magic byte).");
            return;
        }

        // Optional browser-declared size; falls back to unknown size when absent.
        const uint32_t declaredSize = parseDeclaredSize(request->arg(kSizeField));

        if (target == OtamaticUpdateTarget::Filesystem) {
            // The write goes to the first SPIFFS/LittleFS partition of the table.
            const uint32_t partitionSize = _client.getFilesystemPartitionSize();
            if (partitionSize == 0) {
                rejectUpload("This device has no filesystem partition (add spiffs/littlefs to the partition table).");
                return;
            }
            if (declaredSize > partitionSize) {
                rejectUpload("The image is %lu bytes, larger than the filesystem partition (%lu bytes).",
                             (unsigned long)declaredSize, (unsigned long)partitionSize);
                return;
            }
        }

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
            s.target = target;
            s.error[0] = '\0';
            s.lastChunkAt = millis();
            s.bytesWritten = 0;
            s.succeeded = false;
            s.abortOwner = AbortOwner::None;
        });
        if (rejected) {
            log_w("Upload rejected, another update is in progress");
            setUploadError("Another update is already in progress: try again in a few seconds.");
            return;
        }

        const uint32_t imageSize = declaredSize > 0 ? declaredSize : UPDATE_SIZE_UNKNOWN;

        const uint32_t progressTotal = declaredSize > 0 ? declaredSize : request->contentLength();

        if (! _client.beginUpdate(imageSize, progressTotal, target)) {
            setUploadError("Cannot start the write: %s", Update.errorString());
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
            if (ok) setUploadError("The write was interrupted before it completed.");
            else setUploadError("Write failed: %s", Update.errorString());
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
        if (! succeeded) setUploadError("Write failed: %s", Update.errorString());

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
