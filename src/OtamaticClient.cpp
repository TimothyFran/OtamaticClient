#include "OtamaticClient.h"
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <string.h>

OtamaticClient::OtamaticClient(Client& client) : _client(&client) {}

OtamaticClient::~OtamaticClient() {}

bool OtamaticClient::begin(uint32_t currentFwVersion, const char* serviceKey) {
    _firmwareVersion = currentFwVersion;

    strlcpy(_serviceKey, serviceKey != nullptr ? serviceKey : "", sizeof(_serviceKey));
    size_t serviceKeyLen = strnlen(_serviceKey, sizeof(_serviceKey));
    if (serviceKeyLen != 48) {
        log_w("Service key length is %u, expected 48 characters, requests will likely be rejected", (unsigned)serviceKeyLen);
    }

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
    if (millis() - _lastCheck >= _checkInterval) requestCheckNow();
}

void OtamaticClient::onEvent(void (*callback)(OtamaticClientEventData)) {
    _onEvent = callback;
}

void OtamaticClient::setPublicKey(const char* key) {
    if (key == nullptr) key = "";
    // Allow PEM keys to be supplied with leading whitespace.
    while (*key == ' ' || *key == '\t' || *key == '\r' || *key == '\n') key++;
    strlcpy(_publicKey, key, sizeof(_publicKey));
    if (key[0] != '\0' && strnlen(key, sizeof(_publicKey)) >= sizeof(_publicKey) - 1) {
        log_w("Public key truncated to %u characters, verification will likely fail", (unsigned)(sizeof(_publicKey) - 1));
    }
}

/**
 * Decode the server-provided signature.
 * Expected format: hex-encoded DER signature, i.e. the DER bytes of
 * SEQUENCE { INTEGER r, INTEGER s } serialized as a hex string.
 * @return true on success; outLen is the length of the decoded DER signature.
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

    // Validate the DER envelope: SEQUENCE { INTEGER r, INTEGER s }.
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
    // If user has set a custom device ID, return it
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
    if (_busy) {
        log_w("Check already in progress, ignoring re-entrant call");
        return;
    }
    _busy = true;
    requestCheckNowInternal(restartCounter);
    _busy = false;
}

void OtamaticClient::requestCheckNowInternal(bool restartCounter) {
    if (restartCounter) _lastCheck = millis();

    // Discard metadata from the previous update check.
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

    // Collect the Transfer-Encoding header so we can detect chunked responses
    // (HTTP 1.1) before parsing the body. By default, HTTPClient discards headers.
    const char* transferEncKeys[] = {"Transfer-Encoding"};
    http.collectHeaders(transferEncKeys, 1);
    http.addHeader("Authorization", bearer);

    int httpCode = http.GET();
    if (httpCode != 200) {
        log_e("Version check HTTP request failed, code: %d", httpCode);
        http.end();
        return;
    }

    // Ex: {"version":42,"size":425984,...}
    // Reading the stream directly bypasses HTTPClient's internal chunked
    // transfer-encoding handling, so when the server answers with
    // "Transfer-Encoding: chunked" we must decode the chunks first
    // (see https://github.com/bblanchon/ArduinoJson/issues/1506), otherwise
    // deserializeJson() fails with an invalid JSON input error.
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

    // Validate the advertised size before storing it: reading as int32_t
    // rejects negative or overflowing values coming from malformed JSON.
    const int32_t sizeValue = doc["size"] | 0;
    if (sizeValue <= 0) {
        log_e("Invalid firmware size advertised by the server: %d", (int)sizeValue);
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::InvalidFirmware);
        return;
    }

    const uint32_t otaPartitionSize = getOtaPartitionSize();
    if (otaPartitionSize > 0 && (uint32_t)sizeValue > otaPartitionSize) {
        log_e("Firmware size %lu exceeds the OTA partition size %lu",
              (unsigned long)sizeValue, (unsigned long)otaPartitionSize);
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::InvalidFirmware);
        return;
    }

    log_i("Update available, version: %lu, size: %lu", (unsigned long)newVersion, (unsigned long)sizeValue);

    // Store metadata for the update download and verification.
    _checkData.version = newVersion;
    _checkData.size = (uint32_t)sizeValue;
    _checkData.setSignature(doc["signature"] | "");
    _checkData.setIntegrity(doc["integrity"] | "");
    log_i("Signature: %s", _checkData.getSignature());
    log_i("Integrity: %s", _checkData.getIntegrity());

    transmitEvent(OtamaticClientEvent::UpdateAvailable);

    if (!_autoUpdate) return;

    applyUpdateInternal();
}

bool OtamaticClient::applyUpdate() {
    if (_busy) {
        log_w("Check or update already in progress, ignoring re-entrant call");
        return false;
    }
    _busy = true;
    bool result = applyUpdateInternal();
    _busy = false;
    return result;
}

bool OtamaticClient::applyUpdateInternal() {
    if (! _checkData.isValid()) {
        log_e("%s", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::InvalidCheckData)));
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::InvalidCheckData);
        return false;
    }

    transmitEvent(OtamaticClientEvent::UpdateStarted);

    HTTPClient http;

    char _fetchPath[51] = {0};
    snprintf(_fetchPath, sizeof(_fetchPath), "/api/v1/ota/download?deviceId=%llu",
             getDeviceId());

    NetworkClient& netClient = *static_cast<NetworkClient*>(_client);
    if(! http.begin(netClient, _serverHost, _serverPort, _fetchPath)) {
        log_e("%s (host: %s:%u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::ConnectionFailed)), _serverHost, _serverPort);
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::ConnectionFailed);
        return false;
    }

    char bearer[kBearerTokenMaxLen] = {0};
    buildBearerToken(bearer, sizeof(bearer));

    // Collect the Transfer-Encoding header to detect chunked responses (HTTP 1.1)
    const char* transferEncKeys[] = {"Transfer-Encoding"};
    http.collectHeaders(transferEncKeys, 1);

    http.addHeader("Authorization", bearer);

    int httpCode = http.GET();
    if (httpCode != 200) {
        log_e("%s, HTTP code: %d", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::HttpFailed)), httpCode);
        http.end();
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::HttpFailed);
        return false;
    }

    const uint32_t otaPartitionSize = getOtaPartitionSize();
    if (_checkData.size == 0 || (otaPartitionSize > 0 && _checkData.size > otaPartitionSize)) {
        log_e("%s (size: %lu, OTA partition: %lu)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::InvalidFirmware)),
              (unsigned long)_checkData.size, (unsigned long)otaPartitionSize);
        http.end();
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::InvalidFirmware);
        return false;
    }

    // Reserve the OTA partition for the expected firmware size
    if (! Update.begin(_checkData.size, U_FLASH)) {
        log_e("%s (Update.begin error: %u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::BeginFailed)), Update.getError());
        http.end();
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::BeginFailed);
        return false;
    }

    log_i("Update started, version: %lu, size: %lu", (unsigned long)_checkData.version, (unsigned long)_checkData.size);

    // Integrity is always verified. Signature verification is performed when
    // both the server provided a signature and a public key has been configured.
    const bool checkSignature = _checkData.signature[0] != '\0' && _publicKey[0] != '\0';
    if (_checkData.signature[0] != '\0' && ! checkSignature) {
        log_w("Signature provided by the server but no public key set, skipping signature verification");
    }

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    if (mbedtls_sha256_starts(&shaCtx, 0) != 0) {
        mbedtls_sha256_free(&shaCtx);
        Update.abort();
        log_e("Failed to initialize SHA-256 context");
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::IntegrityFailed);
        return false;
    }

    // Download the binary in chunks and write it to the OTA partition.
    // Reading the raw stream bypasses HTTPClient's chunked transfer-encoding
    // handling: if the body is chunked, the chunk markers would corrupt the
    // firmware image written to flash, so decode the chunks first.
    const bool isChunked = http.header("Transfer-Encoding") == String("chunked");
    ChunkDecodingStream decodedStream(http.getStream());
    Stream* stream = isChunked ? static_cast<Stream*>(&decodedStream) : http.getStreamPtr();
    uint8_t buffer[1024];
    size_t written = 0;
    uint32_t lastProgress = 0;
    uint32_t stallStart = millis();
    bool failed = false;
    OtamaticClientError failedError = OtamaticClientError::DownloadFailed;

    while (written < _checkData.size && ! failed) {
        int avail = stream->available();
        if (avail <= 0) {
            // No data available: bail out if the connection dropped or stalled
            if (! http.connected() || millis() - stallStart > kUpdateStallTimeout) {
                log_w("Download stalled for %lu ms or connection dropped (written: %lu/%lu)",
                      (unsigned long)(millis() - stallStart), (unsigned long)written, (unsigned long)_checkData.size);
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

        size_t w = Update.write(buffer, (size_t)read);
        if (w != (size_t)read) {
            log_e("%s (requested: %d, written: %u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::WriteFailed)), read, (unsigned)w);
            failed = true;
            failedError = OtamaticClientError::WriteFailed;
            break;
        }
        written += w;

        mbedtls_sha256_update(&shaCtx, buffer, (size_t)read);

        // Emit the progress event only when the percentage changes
        uint32_t progress = (uint32_t)((uint64_t)written * 100 / _checkData.size);
        if (progress != lastProgress) {
            lastProgress = progress;
            log_d("Update progress: %lu%% (%lu/%lu bytes)", (unsigned long)progress, (unsigned long)written, (unsigned long)_checkData.size);
            transmitEvent(OtamaticClientEvent::UpdateProgress, (uint8_t)progress);
        }
    }

    http.end();

    // Finalize the SHA-256 digest. The same digest is used for integrity and signature verification.
    uint8_t integrityHash[32] = {0};
    bool ctxOk = mbedtls_sha256_finish(&shaCtx, integrityHash) == 0;
    mbedtls_sha256_free(&shaCtx);

    if (! ctxOk) {
        failed = true;
        failedError = OtamaticClientError::IntegrityFailed;
    }

    if (failed || written != _checkData.size) {
        Update.abort();
        log_e("%s (written: %lu/%lu)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(failedError)), (unsigned long)written, (unsigned long)_checkData.size);
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)failedError);
        return false;
    }

    // Verify the firmware integrity.
    if (! verifyIntegrity(integrityHash)) {
        Update.abort();
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::IntegrityFailed);
        return false;
    }

    // Verify the firmware signature when both signature and public key are available.
    if (checkSignature && ! verifySignature(integrityHash)) {
        Update.abort();
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::SignatureFailed);
        return false;
    }

    // Finalize: verifies the written image and switches the boot partition
    bool success = Update.end();
    if (! success) {
        log_e("%s (Update.end error: %u)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::EndFailed)), Update.getError());
    } else {
        log_i("Update completed, version: %lu", (unsigned long)_checkData.version);
    }
    transmitEvent(success ? OtamaticClientEvent::UpdateCompleted : OtamaticClientEvent::UpdateFailed,
                  success ? (uint8_t)OtamaticClientError::None : (uint8_t)OtamaticClientError::EndFailed);

    if (_autoRestart) ESP.restart();

    return success;
}



bool OtamaticClient::verifyIntegrity(const uint8_t* integrityHash) {
    if (_checkData.integrity[0] == '\0' || strlen(_checkData.integrity) != 64) {
        log_e("%s (invalid integrity value in check data)",
              reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::IntegrityFailed)));
        return false;
    }

    // Convert the computed hash to lowercase hex and compare
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

    // Parse the key (PEM or Base64-encoded DER/SPKI)
    mbedtls_pk_context pkCtx;
    mbedtls_pk_init(&pkCtx);
    int pkRes;
    if (strncmp(_publicKey, "-----BEGIN", 10) == 0) {
        if (strstr(_publicKey, "PUBLIC KEY") != nullptr) {
            // PEM public key: must be null-terminated, length includes the terminator
            pkRes = mbedtls_pk_parse_public_key(&pkCtx, (const uint8_t*)_publicKey, strlen(_publicKey) + 1);
        } else {
            log_e("Unsupported PEM key format");
            mbedtls_pk_free(&pkCtx);
            return false;
        }
    } else {
        // DER or Base64-encoded DER
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

    // Verify the ECDSA P-256 signature using the SHA-256 digest computed from the firmware.
    // The digest passed to mbedtls_pk_verify() is already SHA-256; it is not hashed again.
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
        case OtamaticClientEvent::UpdateFailed: return F("Update failed");
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
        default: return F("Unknown error");
    }
}