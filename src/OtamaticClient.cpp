#include "OtamaticClient.h"
#include "OtamaticWebPortal.h"
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <string.h>

// Shared OTA pipeline state (HTTP download and portal upload).
struct OtamaticClient::UpdateState {
    mbedtls_sha256_context sha = {};
    uint32_t expectedSize = 0;
    uint32_t progressTotal = 0;
    size_t written = 0;
    uint32_t lastProgress = 0;
};

OtamaticClient::~OtamaticClient() {
    delete _updateState;
    delete _portal;
}

bool OtamaticClient::setClient(Client* client) {
    if (client == nullptr) {
        log_w("setClient(nullptr) ignored, keeping the current binding");
        return false;
    }
    _client = client;
    _client->setTimeout(1000);
    log_i("Client bound");
    return true;
}

bool OtamaticClient::begin(uint32_t currentFwVersion, const char* serviceKey) {
    if (_client == nullptr) {
        log_e("No client bound, call setClient() before begin()");
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::ConfigInvalid);
        return false;
    }

    size_t serviceKeyLen = serviceKey != nullptr ? strnlen(serviceKey, sizeof(_serviceKey)) : 0;
    if (serviceKeyLen != 48) {
        log_e("Invalid service key length: %u, expected 48 characters", (unsigned)serviceKeyLen);
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::ConfigInvalid);
        return false;
    }

    _firmwareVersion = currentFwVersion;
    strlcpy(_serviceKey, serviceKey, sizeof(_serviceKey));

    _client->setTimeout(1000);

    log_i("Initialized, firmware version: %lu", (unsigned long)_firmwareVersion);
    return true;
}

void OtamaticClient::buildBearerToken(char* buffer, size_t bufferSize) const {
    int written = snprintf(buffer, bufferSize, "%s%s", kBearerPrefix, _serviceKey);
    if (written < 0 || (size_t)written >= bufferSize) {
        log_w("Bearer token truncated (key length: %u, buffer: %u), requests will likely be rejected",
              (unsigned)strnlen(_serviceKey, sizeof(_serviceKey)), (unsigned)bufferSize);
    }
}

void OtamaticClient::loop() {
    if (_portal != nullptr) _portal->handleLoop();

    // Deferred only while an update is writing to flash: a portal that is
    // merely active does not stop the checks.
    if (! _busy.load() && ! _transportBusy.load() &&
        millis() - _lastCheck >= _checkInterval) {
        requestCheckNow();
    }
}

bool OtamaticClient::startPortal(uint32_t timeoutMs, const char* username, const char* password) {
    if (_busy.load()) {
        log_w("Check or update in progress, portal start ignored");
        return false;
    }
    if (_portal == nullptr) _portal = new OtamaticWebPortal(*this);
    return _portal->start(timeoutMs, username, password);
}

void OtamaticClient::stopPortal() {
    if (_portal != nullptr) _portal->stop();
}

bool OtamaticClient::isPortalActive() const {
    return _portal != nullptr && _portal->isActive();
}

void OtamaticClient::onEvent(void (*callback)(OtamaticClientEventData)) {
    _onEvent = callback;
}

void OtamaticClient::setPublicKey(const char* key) {
    if (key == nullptr) key = "";
    while (*key == ' ' || *key == '\t' || *key == '\r' || *key == '\n') key++;
    strlcpy(_publicKey, key, sizeof(_publicKey));
    if (key[0] != '\0' && strnlen(key, sizeof(_publicKey)) >= sizeof(_publicKey) - 1) {
        log_w("Public key truncated to %u characters, verification will likely fail", (unsigned)(sizeof(_publicKey) - 1));
    }
}

/**
 * Hex-encoded DER signature (SEQUENCE { INTEGER r, INTEGER s }).
 * @return true on success; outLen holds the decoded length.
 */
static bool decodeSignature(const char* hex, uint8_t* out, size_t outSize, size_t& outLen) {
    size_t len = strlen(hex);
    if (len == 0 || (len % 2) != 0 || len / 2 > outSize) return false;

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < len; i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    outLen = len / 2;

    if (outLen < 8 || out[0] != 0x30 || out[1] != outLen - 2 || out[2] != 0x02) {
        return false;
    }

    size_t pos = 3;
    size_t rLen = out[pos++];
    if (rLen == 0 || pos + rLen + 2 > outLen || out[pos + rLen] != 0x02) {
        return false;
    }

    pos += rLen + 1;
    size_t sLen = out[pos++];
    return sLen > 0 && pos + sLen == outLen;
}

uint64_t OtamaticClient::getDeviceId() {
    if (_deviceId != 0) return _deviceId;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint64_t mac64 = 0;
    for (int i = 0; i < 6; i++) {
        mac64 |= ((uint64_t)mac[i]) << (i * 8);
    }
    return mac64;
}

uint32_t OtamaticClient::getOtaPartitionSize() const {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return part != nullptr ? part->size : 0;
}

void OtamaticClient::requestCheckNow(bool restartCounter) {
    if (_client == nullptr) {
        log_e("No client bound, call setClient() first");
        return;
    }
    // Do not start a new transport operation while an update is writing to
    // flash: the portal upload (if any) is left to finish undisturbed.
    if (_busy.load()) {
        log_w("Update in progress, check deferred");
        return;
    }
    if (_transportBusy.exchange(true)) {
        log_w("Check already in progress, ignoring re-entrant call");
        return;
    }
    requestCheckNowInternal(restartCounter);
    _transportBusy = false;
}

void OtamaticClient::requestCheckNowInternal(bool restartCounter) {
    if (restartCounter) _lastCheck = millis();
    _checkData.clear();

    transmitEvent(OtamaticClientEvent::CheckingForUpdate);

    HTTPClient http;

    char _checkPath[75] = {0};
    snprintf(_checkPath, sizeof(_checkPath), "/api/v1/ota/latest?currentVersion=%lu&deviceId=%llu",
             _firmwareVersion, getDeviceId());

    NetworkClient& netClient = *static_cast<NetworkClient*>(_client);
    if(! http.begin(netClient, _serverHost, _serverPort, _checkPath)) {
        log_e("HTTP begin failed (check, host: %s:%u)", _serverHost, _serverPort);
        return;
    }

    char bearer[kBearerTokenMaxLen] = {0};
    buildBearerToken(bearer, sizeof(bearer));

    const char* transferEncKeys[] = {"Transfer-Encoding"};
    http.collectHeaders(transferEncKeys, 1);
    http.addHeader("Authorization", bearer);

    int httpCode = http.GET();
    if (httpCode != 200) {
        log_e("Version check HTTP request failed, code: %d", httpCode);
        http.end();
        return;
    }

    // The raw stream does not decode chunked bodies; decode them first.
    JsonDocument doc;
    DeserializationError error;
    if (http.header("Transfer-Encoding") == String("chunked")) {
        ChunkDecodingStream decodedStream(http.getStream());
        error = deserializeJson(doc, decodedStream);
    } else {
        error = deserializeJson(doc, http.getStream());
    }
    http.end();
    if (error) {
        log_e("JSON deserialization failed: %s", error.c_str());
        return;
    }

    uint32_t newVersion = doc["version"];
    if (newVersion <= _firmwareVersion) {
        log_i("No update needed (current: %lu, remote: %lu)", (unsigned long)_firmwareVersion, (unsigned long)newVersion);
        transmitEvent(OtamaticClientEvent::UpdateNotNeeded);
        return;
    }

    // Reject non-positive or overflowing sizes from malformed JSON.
    const int32_t sizeValue = doc["size"] | 0;
    if (sizeValue <= 0) {
        log_e("Invalid firmware size advertised by the server: %d", (int)sizeValue);
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::InvalidFirmware);
        return;
    }

    const uint32_t otaPartitionSize = getOtaPartitionSize();
    if (otaPartitionSize > 0 && (uint32_t)sizeValue > otaPartitionSize) {
        log_e("Firmware size %lu exceeds the OTA partition size %lu",
              (unsigned long)sizeValue, (unsigned long)otaPartitionSize);
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::InvalidFirmware);
        return;
    }

    log_i("Update available, version: %lu, size: %lu", (unsigned long)newVersion, (unsigned long)sizeValue);

    _checkData.version = newVersion;
    _checkData.size = (uint32_t)sizeValue;
    _checkData.setSignature(doc["signature"] | "");
    _checkData.setIntegrity(doc["integrity"] | "");
    log_i("Signature: %s", _checkData.getSignature());
    log_i("Integrity: %s", _checkData.getIntegrity());

    // Integrity is mandatory; signature is mandatory in strict mode.
    if (_checkData.integrity[0] == '\0') {
        log_e("Server did not provide an integrity hash, rejecting update before download");
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::IntegrityFailed);
        _checkData.clear();
        return;
    }
    if (_requireSignature && _checkData.signature[0] == '\0') {
        log_e("Signature enforcement enabled but the server did not provide a signature, rejecting update before download");
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::SignatureFailed);
        _checkData.clear();
        return;
    }

    transmitEvent(OtamaticClientEvent::UpdateAvailable);

    if (!_autoUpdate) return;

    // A browser upload may have claimed the pipeline while this check was in
    // flight: the portal wins and the update stays pending for the next check
    // (or for a manual applyUpdate()).
    if (_busy.exchange(true)) {
        log_w("Upload in progress on the portal, skipping the automatic download");
        return;
    }
    applyUpdateInternal();
    _busy = false;
}

bool OtamaticClient::applyUpdate() {
    if (_client == nullptr) {
        log_e("No client bound, call setClient() first");
        return false;
    }
    // The transport is shared with the version check: never from inside an
    // event callback fired by a running check.
    if (_transportBusy.exchange(true)) {
        log_w("Check or update already in progress, ignoring re-entrant call");
        return false;
    }
    if (_busy.exchange(true)) {
        log_w("Update already in progress (remote or portal), ignoring re-entrant call");
        _transportBusy = false;
        return false;
    }
    bool result = applyUpdateInternal();
    _busy = false;
    _transportBusy = false;
    return result;
}

bool OtamaticClient::applyUpdateInternal() {
    if (! _checkData.isValid()) {
        log_e("%s", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::InvalidCheckData)));
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::InvalidCheckData);
        return false;
    }

    HTTPClient http;

    char _fetchPath[51] = {0};
    snprintf(_fetchPath, sizeof(_fetchPath), "/api/v1/ota/download?deviceId=%llu",
             getDeviceId());

    NetworkClient& netClient = *static_cast<NetworkClient*>(_client);
    if(! http.begin(netClient, _serverHost, _serverPort, _fetchPath)) {
        log_e("%s (host: %s:%u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::ConnectionFailed)), _serverHost, _serverPort);
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::ConnectionFailed);
        return false;
    }

    char bearer[kBearerTokenMaxLen] = {0};
    buildBearerToken(bearer, sizeof(bearer));

    const char* transferEncKeys[] = {"Transfer-Encoding"};
    http.collectHeaders(transferEncKeys, 1);

    http.addHeader("Authorization", bearer);

    int httpCode = http.GET();
    if (httpCode != 200) {
        log_e("%s, HTTP code: %d", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::HttpFailed)), httpCode);
        http.end();
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::HttpFailed);
        return false;
    }

    const bool isChunked = http.header("Transfer-Encoding") == String("chunked");
    ChunkDecodingStream decodedStream(http.getStream());
    Stream* stream = isChunked ? static_cast<Stream*>(&decodedStream) : http.getStreamPtr();

    if (! beginUpdate(_checkData.size, _checkData.size)) {
        http.end();
        return false;
    }

    // Signature is checked only when both signature and public key are available.
    const bool checkSignature = _checkData.signature[0] != '\0' && _publicKey[0] != '\0';
    if (_checkData.signature[0] != '\0' && ! checkSignature) {
        log_w("Signature provided by the server but no public key set, skipping signature verification");
    }

    uint8_t buffer[1024];
    uint32_t stallStart = millis();
    bool failed = false;
    OtamaticClientError failedError = OtamaticClientError::DownloadFailed;

    while (_updateState->written < _checkData.size && ! failed) {
        int avail = stream->available();
        if (avail <= 0) {
            if (! http.connected() || millis() - stallStart > kUpdateStallTimeout) {
                log_w("Download stalled for %lu ms or connection dropped (written: %u/%lu)",
                      (unsigned long)(millis() - stallStart), (unsigned)_updateState->written, (unsigned long)_checkData.size);
                failed = true;
                failedError = OtamaticClientError::DownloadFailed;
                break;
            }
            delay(1);
            continue;
        }
        stallStart = millis();

        size_t chunk = (size_t)avail < sizeof(buffer) ? (size_t)avail : sizeof(buffer);
        int read = stream->readBytes(buffer, chunk);
        if (read <= 0) {
            log_e("%s (readBytes returned %d)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::DownloadFailed)), read);
            failed = true;
            failedError = OtamaticClientError::DownloadFailed;
            break;
        }

        if (! writeUpdateChunk(buffer, (size_t)read)) {
            failed = true;
            failedError = OtamaticClientError::WriteFailed;
            break;
        }
    }

    http.end();

    if (failed) {
        abortUpdate(failedError);
        return false;
    }

    const bool success = finalizeUpdate(true, checkSignature);
    if (success && _autoRestart) ESP.restart();

    return success;
}



// Shared OTA update pipeline (all transports).

bool OtamaticClient::beginUpdate(uint32_t size, uint32_t progressTotal) {
    const bool sizeKnown = size != UPDATE_SIZE_UNKNOWN;
    const uint32_t otaPartitionSize = getOtaPartitionSize();
    if (sizeKnown && (size == 0 || (otaPartitionSize > 0 && size > otaPartitionSize))) {
        log_e("%s (size: %lu, OTA partition: %lu)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::InvalidFirmware)),
              (unsigned long)size, (unsigned long)otaPartitionSize);
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::InvalidFirmware);
        return false;
    }

    if (! Update.begin(size, U_FLASH)) {
        log_e("%s (Update.begin error: %u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::BeginFailed)), Update.getError());
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::BeginFailed);
        return false;
    }

    _updateState = new UpdateState();
    _updateState->expectedSize = size;
    _updateState->progressTotal = progressTotal;

    mbedtls_sha256_init(&_updateState->sha);
    if (mbedtls_sha256_starts(&_updateState->sha, 0) != 0) {
        mbedtls_sha256_free(&_updateState->sha);
        Update.abort();
        delete _updateState;
        _updateState = nullptr;
        log_e("Failed to initialize SHA-256 context");
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::IntegrityFailed);
        return false;
    }

    log_i("Update started, size: %lu", (unsigned long)(sizeKnown ? size : otaPartitionSize));
    transmitEvent(OtamaticClientEvent::UpdateStarted);
    return true;
}

bool OtamaticClient::writeUpdateChunk(const uint8_t* data, size_t len) {
    if (_updateState == nullptr || data == nullptr || len == 0) return false;

    // Update.write() takes a non-const buffer (it does not modify it).
    const size_t w = Update.write(const_cast<uint8_t*>(data), len);
    if (w != len) {
        log_e("%s (requested: %u, written: %u)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::WriteFailed)),
              (unsigned)len, (unsigned)w);
        return false;
    }

    _updateState->written += w;
    mbedtls_sha256_update(&_updateState->sha, data, len);

    if (_updateState->progressTotal > 0) {
        uint32_t progress = (uint32_t)((uint64_t)_updateState->written * 100 / _updateState->progressTotal);
        // With unknown size the denominator is an estimate: cap at 99% until completion.
        if (progress > 100 || (progress == 100 && _updateState->expectedSize == UPDATE_SIZE_UNKNOWN)) {
            progress = 99;
        }
        if (progress != _updateState->lastProgress) {
            _updateState->lastProgress = progress;
            log_d("Update progress: %lu%% (%u bytes)", (unsigned long)progress, (unsigned)_updateState->written);
            transmitEvent(OtamaticClientEvent::UpdateProgress, (uint8_t)progress);
        }
    }

    return true;
}

bool OtamaticClient::finalizeUpdate(bool checkIntegrity, bool checkSignature) {
    if (_updateState == nullptr) {
        log_e("%s (no update in progress)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::WriteFailed)));
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::WriteFailed);
        return false;
    }

    uint8_t integrityHash[32] = {0};
    const bool ctxOk = mbedtls_sha256_finish(&_updateState->sha, integrityHash) == 0;
    mbedtls_sha256_free(&_updateState->sha);

    bool failed = ! ctxOk;
    OtamaticClientError failedError = OtamaticClientError::IntegrityFailed;

    if (! failed && _updateState->expectedSize != UPDATE_SIZE_UNKNOWN &&
        _updateState->written != _updateState->expectedSize) {
        failed = true;
        failedError = OtamaticClientError::DownloadFailed;
    }

    if (failed) {
        Update.abort();
        log_e("%s (written: %u/%lu)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(failedError)),
              (unsigned)_updateState->written,
              (unsigned long)(_updateState->expectedSize == UPDATE_SIZE_UNKNOWN ? 0 : _updateState->expectedSize));
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)failedError);
        delete _updateState;
        _updateState = nullptr;
        return false;
    }

    if (checkIntegrity && ! verifyIntegrity(integrityHash)) {
        Update.abort();
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::IntegrityFailed);
        delete _updateState;
        _updateState = nullptr;
        return false;
    }

    if (checkSignature && ! verifySignature(integrityHash)) {
        Update.abort();
        transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)OtamaticClientError::SignatureFailed);
        delete _updateState;
        _updateState = nullptr;
        return false;
    }

    // With unknown size, finalize with the bytes actually written.
    const bool evenIfRemaining = _updateState->expectedSize == UPDATE_SIZE_UNKNOWN;
    const bool success = Update.end(evenIfRemaining);
    if (! success) {
        log_e("%s (Update.end error: %u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::EndFailed)), Update.getError());
    } else {
        log_i("Update completed, %u bytes written", (unsigned)_updateState->written);
    }
    transmitEvent(success ? OtamaticClientEvent::UpdateCompleted : OtamaticClientEvent::OperationFailed,
                  success ? (uint8_t)OtamaticClientError::None : (uint8_t)OtamaticClientError::EndFailed);

    delete _updateState;
    _updateState = nullptr;
    return success;
}

void OtamaticClient::abortUpdate(OtamaticClientError reason) {
    Update.abort();
    delete _updateState;
    _updateState = nullptr;
    log_e("%s", reinterpret_cast<const char*>(OtamaticClient::getErrorName(reason)));
    transmitEvent(OtamaticClientEvent::OperationFailed, (uint8_t)reason);
}



bool OtamaticClient::verifyIntegrity(const uint8_t* integrityHash) {
    if (_checkData.integrity[0] == '\0' || strlen(_checkData.integrity) != 64) {
        log_e("%s (invalid integrity value in check data)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::IntegrityFailed)));
        return false;
    }

    char computedHex[65] = {0};
    for (size_t i = 0; i < 32; i++) {
        snprintf(computedHex + i * 2, 3, "%02x", integrityHash[i]);
    }
    if (strcasecmp(computedHex, _checkData.integrity) != 0) {
        log_e("%s (expected: %s, computed: %s)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::IntegrityFailed)),
              _checkData.integrity, computedHex);
        return false;
    }

    log_d("Integrity check passed");
    return true;
}

bool OtamaticClient::verifySignature(const uint8_t* integrityHash) {

    uint8_t sig[80] = {0};
    size_t sigLen = 0;

    if (!decodeSignature(_checkData.signature, sig, sizeof(sig), sigLen)) {
        log_e("Unsupported or malformed signature");
        return false;
    }

    mbedtls_pk_context pkCtx;
    mbedtls_pk_init(&pkCtx);
    int pkRes;
    if (strncmp(_publicKey, "-----BEGIN", 10) == 0) {
        if (strstr(_publicKey, "PUBLIC KEY") != nullptr) {
            pkRes = mbedtls_pk_parse_public_key(&pkCtx, (const uint8_t*)_publicKey, strlen(_publicKey) + 1);
        } else {
            log_e("Unsupported PEM key format");
            mbedtls_pk_free(&pkCtx);
            return false;
        }
    } else {
        uint8_t der[256] = {0};
        size_t derLen = 0;
        if (mbedtls_base64_decode(der, sizeof(der), &derLen,
                                  (const uint8_t*)_publicKey, strlen(_publicKey)) != 0) {
            derLen = strlen(_publicKey);
            if (derLen > sizeof(der)) {
                log_e("%s (raw DER key too long: %u bytes, max: %u)",
                      reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::SignatureFailed)),
                      (unsigned)derLen, (unsigned)sizeof(der));
                mbedtls_pk_free(&pkCtx);
                return false;
            }
            memcpy(der, _publicKey, derLen);
        }
        pkRes = mbedtls_pk_parse_public_key(&pkCtx, der, derLen);
        memset(der, 0, sizeof(der));
    }
    if (pkRes != 0) {
        log_e("%s (public key parse error: -0x%04x)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::SignatureFailed)), -pkRes);
        mbedtls_pk_free(&pkCtx);
        return false;
    }

    int verRes = mbedtls_pk_verify(&pkCtx, MBEDTLS_MD_SHA256,
                                   integrityHash, 32, sig, sigLen);
    mbedtls_pk_free(&pkCtx);
    if (verRes != 0) {
        log_e("%s (mbedtls_pk_verify error: -0x%04x)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::SignatureFailed)), -verRes);
        return false;
    }

    log_d("Signature check passed");
    return true;
}

const __FlashStringHelper* OtamaticClient::getEventName(OtamaticClientEvent event) {
    switch (event) {
        case OtamaticClientEvent::CheckingForUpdate: return F("Checking for update");
        case OtamaticClientEvent::UpdateAvailable: return F("Update available");
        case OtamaticClientEvent::UpdateNotNeeded: return F("Update not needed");
        case OtamaticClientEvent::UpdateStarted: return F("Update started");
        case OtamaticClientEvent::UpdateProgress: return F("Update progress");
        case OtamaticClientEvent::UpdateCompleted: return F("Update completed");
        case OtamaticClientEvent::OperationFailed: return F("Operation failed");
        default: return F("Unknown event");
    }
}

const __FlashStringHelper* OtamaticClient::getErrorName(OtamaticClientError error) {
    switch (error) {
        case OtamaticClientError::None: return F("No error");
        case OtamaticClientError::InvalidCheckData: return F("Invalid check data");
        case OtamaticClientError::ConnectionFailed: return F("Connection failed");
        case OtamaticClientError::HttpFailed: return F("HTTP error");
        case OtamaticClientError::InvalidFirmware: return F("Invalid firmware");
        case OtamaticClientError::BeginFailed: return F("OTA partition error");
        case OtamaticClientError::DownloadFailed: return F("Download failed");
        case OtamaticClientError::WriteFailed: return F("Write failed");
        case OtamaticClientError::EndFailed: return F("Verification failed");
        case OtamaticClientError::IntegrityFailed: return F("Integrity check failed");
        case OtamaticClientError::SignatureFailed: return F("Signature check failed");
        case OtamaticClientError::ConfigInvalid: return F("Invalid configuration");
        default: return F("Unknown error");
    }
}