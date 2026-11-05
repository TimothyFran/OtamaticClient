// On the Arduino framework this header must come first: it declares
// `extern const IPAddress INADDR_NONE;` before lwip (pulled in by
// esp_http_client.h below) defines INADDR_NONE as a macro, which would
// otherwise break the declaration. Plain ESP-IDF builds skip this include.
#if __has_include(<Arduino.h>)
  #include <Arduino.h>
#endif

#include <otamatic/otamatic_client.hpp>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_partition.h>
#include <ArduinoJson.h>
#include <psa/crypto.h>
#include <cstring>
#include <strings.h>

// 'unused' attribute: with CORE_DEBUG_LEVEL=0 (Arduino default) the ESP_LOGx
// macros compile to nothing, so TAG would be flagged as unused.
static const char* TAG __attribute__((unused)) = "otamatic_client";

// Network timeout for the update-check HTTP request (the TLS handshake alone
// can take a few seconds on a slow link).
static constexpr int kCheckTimeoutMs = 10000;

// Maximum time without receiving any data during the firmware download before
// the transfer is considered stalled.
static constexpr uint32_t kDownloadStallTimeoutMs = 30000;

// Validates a SHA-256 hex digest: exactly 64 hex characters (case-insensitive).
static bool isValidHexDigest(const char* s) {
    if (s == nullptr) return false;
    if (strlen(s) != OtamaticUpdate::kIntegrityMaxLen) return false;
    for (size_t i = 0; i < OtamaticUpdate::kIntegrityMaxLen; i++) {
        const char c = s[i];
        const bool isHex = (c >= '0' && c <= '9') ||
                           (c >= 'a' && c <= 'f') ||
                           (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return true;
}


/**
 * @brief Decodes a standard base64 string, ignoring whitespace and '=' padding.
 * @return true on success; outLen holds the decoded length.
 */
static bool base64Decode(const char* in, uint8_t* out, size_t outSize, size_t& outLen) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    int accumulator = 0;
    int bits = 0;
    size_t written = 0;
    for (const char* p = in; *p != '\0'; p++) {
        const char c = *p;
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = value(c);
        if (v < 0) return false;
        accumulator = (accumulator << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= outSize) return false;
            out[written++] = static_cast<uint8_t>((accumulator >> bits) & 0xFF);
        }
    }
    outLen = written;
    return written > 0;
}

/**
 * @brief Converts a hex-encoded DER ECDSA signature (SEQUENCE { INTEGER r, INTEGER s })
 * into the raw fixed-width r||s representation expected by PSA (32+32 bytes for P-256).
 * @return true on success.
 */
static bool decodeEcdsaSignature(const char* hex, uint8_t raw[64]) {
    uint8_t der[80] = {0};
    const size_t hexLen = strlen(hex);
    if (hexLen == 0 || (hexLen % 2) != 0 || hexLen / 2 > sizeof(der)) return false;

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hexLen; i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        der[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }

    const size_t derLen = hexLen / 2;
    if (derLen < 8 || der[0] != 0x30 || der[1] != derLen - 2 || der[2] != 0x02) return false;

    size_t pos = 3;
    size_t rLen = der[pos++];
    if (rLen == 0 || pos + rLen + 2 > derLen || der[pos + rLen] != 0x02) return false;
    const uint8_t* r = der + pos;
    pos += rLen + 1;

    size_t sLen = der[pos++];
    if (sLen == 0 || pos + sLen != derLen) return false;
    const uint8_t* s = der + pos;

    while (rLen > 32 && r[0] == 0x00) { r++; rLen--; }
    while (sLen > 32 && s[0] == 0x00) { s++; sLen--; }
    if (rLen > 32 || sLen > 32) return false;

    memset(raw, 0, 64);
    memcpy(raw + (32 - rLen), r, rLen);
    memcpy(raw + (64 - sLen), s, sLen);
    return true;
}

/**
 * @brief Decodes a P-256 public key into the 65-byte uncompressed point 0x04||X||Y.
 * Accepts a hex-encoded raw point, a base64-encoded raw point, or a PEM /
 * base64-encoded DER SubjectPublicKeyInfo.
 * @return true on success.
 */
static bool decodeEcPublicKey(const char* key, uint8_t out[65]) {
    if (key == nullptr) return false;

    // Strip PEM armor and all whitespace.
    char compact[600] = {0};
    size_t compactLen = 0;
    const char* bodyStart = key;
    const char* bodyEnd = key + strlen(key);
    const char* begin = strstr(key, "-----BEGIN");
    if (begin != nullptr) {
        const char* newline = strchr(begin, '\n');
        const char* end = strstr(key, "-----END");
        if (newline == nullptr || end == nullptr || end <= newline) return false;
        bodyStart = newline + 1;
        bodyEnd = end;
    }
    for (const char* p = bodyStart; p < bodyEnd; p++) {
        const char c = *p;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (compactLen >= sizeof(compact) - 1) return false;
        compact[compactLen++] = c;
    }
    compact[compactLen] = '\0';
    if (compactLen == 0) return false;

    uint8_t buffer[512] = {0};
    size_t bufferLen = 0;

    // Hex-encoded key?
    bool isHex = (compactLen % 2) == 0;
    for (size_t i = 0; isHex && i < compactLen; i++) {
        const char c = compact[i];
        isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    if (isHex) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (compactLen / 2 > sizeof(buffer)) return false;
        for (size_t i = 0; i < compactLen; i += 2) {
            const int hi = nibble(compact[i]);
            const int lo = nibble(compact[i + 1]);
            if (hi < 0 || lo < 0) return false;
            buffer[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
        }
        bufferLen = compactLen / 2;
    } else if (!base64Decode(compact, buffer, sizeof(buffer), bufferLen)) {
        return false;
    }

    // Already a raw uncompressed point?
    if (bufferLen == 65 && buffer[0] == 0x04) {
        memcpy(out, buffer, 65);
        return true;
    }

    // DER SubjectPublicKeyInfo: BIT STRING (0x03 0x42 0x00) then the point.
    for (size_t i = 0; i + 4 + 65 <= bufferLen; i++) {
        if (buffer[i] == 0x03 && buffer[i + 1] == 0x42 && buffer[i + 2] == 0x00 && buffer[i + 3] == 0x04) {
            memcpy(out, buffer + i + 3, 65);
            return true;
        }
    }

    // Fallback: the trailing 65 bytes.
    if (bufferLen >= 65 && buffer[bufferLen - 65] == 0x04) {
        memcpy(out, buffer + bufferLen - 65, 65);
        return true;
    }
    return false;
}

/**
 * @brief Verifies an ECDSA P-256 signature over a SHA-256 digest using the PSA Crypto API.
 * @param signatureHex Hex-encoded DER signature as returned by the server.
 * @param publicKey Hex/base64 raw P-256 point, or PEM/DER SubjectPublicKeyInfo.
 * @param digest 32-byte SHA-256 digest of the downloaded firmware.
 * @return true if the signature is valid, false otherwise.
 */
static bool verifyFirmwareSignature(const char* signatureHex, const char* publicKey, const uint8_t* digest) {
    if (signatureHex == nullptr || publicKey == nullptr || digest == nullptr) return false;

    uint8_t rawSignature[64] = {0};
    if (!decodeEcdsaSignature(signatureHex, rawSignature)) {
        ESP_LOGE(TAG, "Unsupported or malformed firmware signature");
        return false;
    }

    uint8_t point[65] = {0};
    if (!decodeEcPublicKey(publicKey, point)) {
        ESP_LOGE(TAG, "Unsupported or malformed public key");
        return false;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Crypto");
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));

    psa_key_id_t keyId = 0;
    psa_status_t status = psa_import_key(&attributes, point, sizeof(point), &keyId);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to import the public key: %d", static_cast<int>(status));
        return false;
    }

    status = psa_verify_hash(keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                             digest, 32, rawSignature, sizeof(rawSignature));
    psa_destroy_key(keyId);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Signature verification failed: %d", static_cast<int>(status));
        return false;
    }
    return true;
}


struct HttpJsonReader {
    esp_http_client_handle_t client;

    int read() {
        char c;
        int result = esp_http_client_read(client, &c, 1);
        return (result == 1) ? static_cast<unsigned char>(c) : -1;
    }

    size_t readBytes(char* buffer, size_t length) {
        int result = esp_http_client_read(client, buffer, length);
        return (result > 0) ? static_cast<size_t>(result) : 0;
    }
};



OtamaticClient::OtamaticClient() {
}

OtamaticClient::~OtamaticClient() {
}

bool OtamaticClient::begin(const OtamaticConfig& config) {
    if (config.apiKey == nullptr || config.apiBaseUrl == nullptr) {
        ESP_LOGE(TAG, "Invalid configuration: apiKey and apiBaseUrl are required");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConfigInvalid);
        return false;
    }

    this->config = config;

    if (this->config.deviceId == 0) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        uint64_t mac64 = 0;
        for (int i = 0; i < 6; i++) {
            mac64 |= ((uint64_t)mac[i]) << (i * 8);
        }
        this->config.deviceId = mac64;
    }

    ESP_LOGI(TAG, "Initializing Otamatic client with Device Id: %llu - Current Version: %s (%lu)",
            this->config.deviceId,
            this->config.versionName != nullptr ? this->config.versionName : "unknown",
            static_cast<unsigned long>(this->config.firmwareVersion));

    if (this->config.enableAutoCheckMode) {
        startAutoCheck();
    }

    return true;
}

void OtamaticClient::deinit() {
    ESP_LOGI(TAG, "Deinitializing Otamatic client");
    stopAutoCheck();
}

bool OtamaticClient::startAutoCheck() {
    uint32_t stackSize = this->config.autoCheckTaskStackSize;

    ESP_LOGI(TAG, "Starting auto-check task (check interval: %lu ms, stack size: %lu bytes)",
             static_cast<unsigned long>(this->config.checkInterval),
             static_cast<unsigned long>(stackSize));

    BaseType_t xReturned = xTaskCreate(
        autoCheckTaskWrapper,
        "otamatic-client",
        stackSize,
        this,
        1,
        &this->taskHandle
    );

    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-check task");
        return false;
    }

    return true;
}

void OtamaticClient::stopAutoCheck() {
    if (this->taskHandle == nullptr) {
        ESP_LOGV(TAG, "Auto-check task is not running");
        return;
    }

    ESP_LOGI(TAG, "Stopping auto-check task");
    vTaskDelete(this->taskHandle);
    this->taskHandle = nullptr;
}

bool OtamaticClient::isAutoCheckRunning() {
    return this->taskHandle != nullptr;
}

void OtamaticClient::onEvent(OtamaticClientEventCallback callback) {
    this->eventCallback = callback;
    if (callback != nullptr) {
        this->eventCallbackEx = nullptr;
        this->eventUserData = nullptr;
    }
}

void OtamaticClient::onEvent(OtamaticClientEventCallbackEx callback, void* userData) {
    this->eventCallbackEx = callback;
    this->eventUserData = callback != nullptr ? userData : nullptr;
    if (callback != nullptr) {
        this->eventCallback = nullptr;
    }
}

void OtamaticClient::transmitEvent(OtamaticClientEvent event, OtamaticClientError error,
                                   uint8_t progress, uint32_t version, uint32_t size) {
    if (this->eventCallbackEx == nullptr && this->eventCallback == nullptr) return;

    OtamaticClientEventData data;
    data.event = event;
    data.error = error;
    data.progress = progress;
    data.version = version;
    data.size = size;

    if (this->eventCallbackEx != nullptr) {
        this->eventCallbackEx(data, this->eventUserData);
    } else {
        this->eventCallback(data);
    }
}

const char* OtamaticClient::getEventName(OtamaticClientEvent event) {
    switch (event) {
        case OtamaticClientEvent::CheckingForUpdate: return "Checking for update";
        case OtamaticClientEvent::UpdateAvailable: return "Update available";
        case OtamaticClientEvent::UpdateNotNeeded: return "Update not needed";
        case OtamaticClientEvent::UpdateIgnored: return "Update ignored";
        case OtamaticClientEvent::UpdateStarted: return "Update started";
        case OtamaticClientEvent::UpdateProgress: return "Update progress";
        case OtamaticClientEvent::UpdateCompleted: return "Update completed";
        case OtamaticClientEvent::UpdateConfirmed: return "Update confirmed";
        case OtamaticClientEvent::UpdateRolledBack: return "Update rolled back";
        case OtamaticClientEvent::OperationFailed: return "Operation failed";
    }
    return "Unknown event";
}

const char* OtamaticClient::getErrorName(OtamaticClientError error) {
    switch (error) {
        case OtamaticClientError::None: return "No error";
        case OtamaticClientError::ConfigInvalid: return "Invalid configuration";
        case OtamaticClientError::ConnectionFailed: return "Connection failed";
        case OtamaticClientError::HttpFailed: return "HTTP error";
        case OtamaticClientError::InvalidCheckData: return "Invalid check data";
        case OtamaticClientError::InvalidFirmware: return "Invalid firmware";
        case OtamaticClientError::BeginFailed: return "Update begin failed";
        case OtamaticClientError::DownloadFailed: return "Download failed";
        case OtamaticClientError::WriteFailed: return "Write failed";
        case OtamaticClientError::EndFailed: return "Update finalization failed";
        case OtamaticClientError::IntegrityFailed: return "Integrity check failed";
        case OtamaticClientError::SignatureFailed: return "Signature check failed";
        case OtamaticClientError::InvalidImage: return "Invalid image";
        case OtamaticClientError::VerificationFailed: return "Verification failed";
    }
    return "Unknown error";
}

void OtamaticClient::autoCheckTask(void* arg) {
    ESP_LOGI(TAG, "Auto-check task started");

    while(true) {
        if (checkForUpdate()) {
            if (this->config.autoUpdateEnabled) {
                applyUpdate();
            }
        }

        // TODO: Remove after stack size tune
        ESP_LOGD(TAG, "Auto-check task stack high water mark: %lu bytes free",
                 static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)) *
                 sizeof(StackType_t));

        vTaskDelay(pdMS_TO_TICKS(config.checkInterval));
    }
}

bool OtamaticClient::checkForUpdate() {
    esp_err_t err = checkForUpdateInternal();
    if (err != ESP_OK || !pendingUpdate.isValid()) {
        return false;
    }
    
    return true;
}

esp_err_t OtamaticClient::checkForUpdateInternal() {
    pendingUpdate.clear();

    transmitEvent(OtamaticClientEvent::CheckingForUpdate);

    if (this->config.apiKey == nullptr || this->config.apiBaseUrl == nullptr) {
        ESP_LOGE(TAG, "Invalid configuration: apiKey and apiBaseUrl are required");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConfigInvalid);
        return ESP_ERR_INVALID_ARG;
    }

    char url[256] = {0};
    int urlLen = snprintf(
        url,
        sizeof(url),
        "%s%s?currentVersion=%lu&deviceId=%llu",
        this->config.apiBaseUrl,
        "/v1/ota/latest",
        this->config.firmwareVersion,
        this->config.deviceId
    );
    if (urlLen < 0 || (size_t)urlLen >= sizeof(url)) {
        ESP_LOGE(TAG, "Update check URL truncated (apiBaseUrl too long)");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConfigInvalid);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Checking for updates at %s", url);

    char bearerToken[256] = {0};
    int bearerLen = snprintf(
        bearerToken,
        sizeof(bearerToken),
        "Bearer %s",
        this->config.apiKey
    );
    if (bearerLen < 0 || (size_t)bearerLen >= sizeof(bearerToken)) {
        ESP_LOGW(TAG, "Bearer token truncated (apiKey too long), request will likely be rejected");
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kCheckTimeoutMs;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConnectionFailed);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", bearerToken);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConnectionFailed);
        return err;
    }

    int64_t contentLength = esp_http_client_fetch_headers(client);
    if (contentLength < 0) {
        ESP_LOGE(TAG, "Failed to fetch HTTP headers");
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConnectionFailed);
        return ESP_FAIL;
    }
    int httpCode = esp_http_client_get_status_code(client);

    if (httpCode != 200) {
        ESP_LOGE(TAG, "Update check failed with HTTP status %d", httpCode);
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::HttpFailed);
        return ESP_FAIL;
    }

    JsonDocument doc;
    HttpJsonReader reader{client};
    DeserializationError error = deserializeJson(doc, reader);

    esp_http_client_cleanup(client);

    if (error) {
        ESP_LOGE(TAG, "Failed to parse update info JSON: %s", error.c_str());
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidCheckData);
        return ESP_FAIL;
    }

    // A missing or non-numeric 'version' means a malformed response, not an
    // up-to-date device: fail explicitly instead of silently returning.
    if (!doc["version"].is<uint32_t>()) {
        ESP_LOGE(TAG, "Update info JSON is missing a valid 'version' field");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidCheckData);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t newVersion = doc["version"].as<uint32_t>();
    if (this->config.firmwareVersion >= newVersion) {
        ESP_LOGD(TAG, "Firmware is up to date (version %lu)",
                 static_cast<unsigned long>(this->config.firmwareVersion));
        transmitEvent(OtamaticClientEvent::UpdateNotNeeded, OtamaticClientError::None, 0, newVersion);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "New firmware available: version %lu (current: %lu)",
             static_cast<unsigned long>(newVersion),
             static_cast<unsigned long>(this->config.firmwareVersion));

    // TODO: Handle ignored versions

    // Reject missing, non-integer, non-positive or overflowing sizes.
    if (!doc["size"].is<int32_t>()) {
        ESP_LOGE(TAG, "Update info JSON is missing a valid 'size' field");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidCheckData);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int32_t sizeValue = doc["size"].as<int32_t>();
    if (sizeValue <= 0) {
        ESP_LOGE(TAG, "Invalid firmware size in update info: %ld", static_cast<long>(sizeValue));
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidFirmware);
        return ESP_FAIL;
    }

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (part == nullptr) {
        ESP_LOGW(TAG, "No OTA update partition found");
    }
    uint32_t otaPartitionSize = part != nullptr ? part->size : 0;
    if (otaPartitionSize > 0 && (uint32_t)sizeValue > otaPartitionSize) {
        ESP_LOGE(TAG, "Firmware size %lu exceeds OTA partition size %lu",
                 static_cast<unsigned long>(sizeValue),
                 static_cast<unsigned long>(otaPartitionSize));
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidFirmware);
        return ESP_FAIL;
    }

    // Integrity is mandatory and must be a SHA-256 hex digest (64 chars).
    const char* integrity = doc["integrity"] | "";
    if (!isValidHexDigest(integrity)) {
        ESP_LOGE(TAG, "Firmware update integrity hash is missing or not a %u-char hex digest",
                 static_cast<unsigned>(OtamaticUpdate::kIntegrityMaxLen));
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::IntegrityFailed);
        return ESP_FAIL;
    }

    // Signature is optional unless requireSignedUpdates is set; when present
    // it must fit the fixed buffer of the update struct.
    const char* signature = doc["signature"] | "";
    if (strlen(signature) == 0 && this->config.requireSignedUpdates) {
        ESP_LOGE(TAG, "Firmware update signature is missing");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::SignatureFailed);
        return ESP_FAIL;
    }
    if (strlen(signature) > OtamaticUpdate::kSignatureMaxLen) {
        ESP_LOGE(TAG, "Firmware update signature is too long (%u chars, max %u)",
                 static_cast<unsigned>(strlen(signature)),
                 static_cast<unsigned>(OtamaticUpdate::kSignatureMaxLen));
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidCheckData);
        return ESP_FAIL;
    }

    pendingUpdate.version = newVersion;
    pendingUpdate.size = static_cast<uint32_t>(sizeValue);
    strlcpy(pendingUpdate.integrity, integrity, sizeof(pendingUpdate.integrity));
    strlcpy(pendingUpdate.signature, signature, sizeof(pendingUpdate.signature));

    transmitEvent(OtamaticClientEvent::UpdateAvailable, OtamaticClientError::None, 0,
                  pendingUpdate.version, pendingUpdate.size);

    return ESP_OK;
}

bool OtamaticClient::applyUpdate() {
    if (!pendingUpdate.isValid()) {
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidCheckData);
        return false;
    }
    return applyUpdateInternal() == ESP_OK;
}

esp_err_t OtamaticClient::applyUpdateInternal() {

    // TODO: Handle ignored versions

    if (this->config.apiKey == nullptr || this->config.apiBaseUrl == nullptr) {
        ESP_LOGE(TAG, "Invalid configuration: apiKey and apiBaseUrl are required");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConfigInvalid);
        return ESP_ERR_INVALID_ARG;
    }

    if (!pendingUpdate.isValid()) {
        ESP_LOGE(TAG, "No valid pending update to apply");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::InvalidCheckData);
        return ESP_ERR_INVALID_STATE;
    }

    char url[256] = {0};
    int urlLen = snprintf(
        url,
        sizeof(url),
        "%s%s?deviceId=%llu",
        this->config.apiBaseUrl,
        "/v1/ota/download",
        this->config.deviceId
    );
    if (urlLen < 0 || (size_t)urlLen >= sizeof(url)) {
        ESP_LOGE(TAG, "Update check URL truncated (apiBaseUrl too long)");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConfigInvalid);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Downloading newest firmware at %s", url);

    char bearerToken[256] = {0};
    int bearerLen = snprintf(
        bearerToken,
        sizeof(bearerToken),
        "Bearer %s",
        this->config.apiKey
    );
    if (bearerLen < 0 || (size_t)bearerLen >= sizeof(bearerToken)) {
        ESP_LOGW(TAG, "Bearer token truncated (apiKey too long), request will likely be rejected");
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kCheckTimeoutMs;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConnectionFailed);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", bearerToken);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::ConnectionFailed);
        return err;
    }

    int64_t contentLength = esp_http_client_fetch_headers(client);
    int httpCode = esp_http_client_get_status_code(client);
    if (httpCode != 200) {
        ESP_LOGE(TAG, "Firmware download failed with HTTP status %d", httpCode);
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::HttpFailed);
        return ESP_FAIL;
    }
    // The body must be exactly the size announced by the check endpoint; a
    // known Content-Length that does not match is rejected before writing.
    if (contentLength > 0 && static_cast<uint32_t>(contentLength) != pendingUpdate.size) {
        ESP_LOGE(TAG, "Firmware size mismatch (headers: %lld, expected: %lu)",
                 static_cast<long long>(contentLength),
                 static_cast<unsigned long>(pendingUpdate.size));
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::DownloadFailed);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (updatePartition == nullptr) {
        ESP_LOGE(TAG, "No OTA update partition found");
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::BeginFailed);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Writing firmware version %lu (%lu bytes) to partition '%s'",
             static_cast<unsigned long>(pendingUpdate.version),
             static_cast<unsigned long>(pendingUpdate.size),
             updatePartition->label);

    esp_ota_handle_t otaHandle = 0;
    err = esp_ota_begin(updatePartition, pendingUpdate.size, &otaHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin OTA update: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::BeginFailed);
        return err;
    }

    // SHA-256 is computed incrementally while streaming the image to flash.
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Crypto");
        esp_ota_abort(otaHandle);
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::IntegrityFailed);
        return ESP_FAIL;
    }
    psa_hash_operation_t hashOp = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hashOp, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize SHA-256 context");
        esp_ota_abort(otaHandle);
        esp_http_client_cleanup(client);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::IntegrityFailed);
        return ESP_FAIL;
    }

    transmitEvent(OtamaticClientEvent::UpdateStarted, OtamaticClientError::None, 0,
                  pendingUpdate.version, pendingUpdate.size);

    uint8_t buffer[1024];
    uint32_t written = 0;
    int lastProgress = -1;
    bool downloadFailed = false;
    TickType_t stallStart = xTaskGetTickCount();

    while (written < pendingUpdate.size) {
        const int read = esp_http_client_read(client, reinterpret_cast<char*>(buffer), sizeof(buffer));
        if (read < 0) {
            ESP_LOGE(TAG, "Firmware download read error after %lu bytes",
                     static_cast<unsigned long>(written));
            downloadFailed = true;
            transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::DownloadFailed);
            break;
        }
        if (read == 0) {
            // No data available: the body ended early or the socket is idle.
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGE(TAG, "Firmware download ended early (%lu/%lu bytes)",
                         static_cast<unsigned long>(written),
                         static_cast<unsigned long>(pendingUpdate.size));
                downloadFailed = true;
                transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::DownloadFailed);
                break;
            }
            if ((uint32_t)((xTaskGetTickCount() - stallStart) * portTICK_PERIOD_MS) > kDownloadStallTimeoutMs) {
                ESP_LOGE(TAG, "Firmware download stalled (%lu/%lu bytes)",
                         static_cast<unsigned long>(written),
                         static_cast<unsigned long>(pendingUpdate.size));
                downloadFailed = true;
                transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::DownloadFailed);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        stallStart = xTaskGetTickCount();

        err = esp_ota_write(otaHandle, buffer, static_cast<size_t>(read));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write firmware to flash: %s", esp_err_to_name(err));
            downloadFailed = true;
            transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::WriteFailed);
            break;
        }

        if (psa_hash_update(&hashOp, buffer, static_cast<size_t>(read)) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Failed to update SHA-256 context");
            downloadFailed = true;
            transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::IntegrityFailed);
            break;
        }
        written += static_cast<uint32_t>(read);

        const int progress = static_cast<int>((static_cast<uint64_t>(written) * 100) / pendingUpdate.size);
        if (progress != lastProgress) {
            lastProgress = progress;
            ESP_LOGD(TAG, "Update progress: %d%% (%lu bytes)", progress,
                     static_cast<unsigned long>(written));
            transmitEvent(OtamaticClientEvent::UpdateProgress, OtamaticClientError::None,
                          static_cast<uint8_t>(progress), pendingUpdate.version);
        }
    }


    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (downloadFailed || written != pendingUpdate.size) {
        psa_hash_abort(&hashOp);
        esp_ota_abort(otaHandle);
        if (!downloadFailed) {
            ESP_LOGE(TAG, "Firmware size mismatch after download (received %lu/%lu bytes)",
                     static_cast<unsigned long>(written),
                     static_cast<unsigned long>(pendingUpdate.size));
            transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::DownloadFailed);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32] = {0};
    size_t digestLen = 0;
    if (psa_hash_finish(&hashOp, digest, sizeof(digest), &digestLen) != PSA_SUCCESS ||
        digestLen != sizeof(digest)) {
        psa_hash_abort(&hashOp);
        esp_ota_abort(otaHandle);
        ESP_LOGE(TAG, "Failed to finalize SHA-256 context");
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::IntegrityFailed);
        return ESP_FAIL;
    }

    char digestHex[OtamaticUpdate::kIntegrityMaxLen + 1] = {0};
    for (size_t i = 0; i < 32; i++) {
        snprintf(digestHex + i * 2, 3, "%02x", digest[i]);
    }
    if (strcasecmp(digestHex, pendingUpdate.integrity) != 0) {
        ESP_LOGE(TAG, "Firmware integrity check failed (expected %s, computed %s)",
                 pendingUpdate.integrity, digestHex);
        esp_ota_abort(otaHandle);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::IntegrityFailed);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGD(TAG, "Firmware integrity check passed");

    // Signature verification: only possible when a public key is configured.
    const bool hasSignature = pendingUpdate.signature[0] != '\0';
    const char* publicKey = this->config.publicKey;
    const bool hasPublicKey = publicKey != nullptr && publicKey[0] != '\0';
    if (hasSignature && hasPublicKey) {
        if (!verifyFirmwareSignature(pendingUpdate.signature, publicKey, digest)) {
            ESP_LOGE(TAG, "Firmware signature verification failed");
            esp_ota_abort(otaHandle);
            transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::SignatureFailed);
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGD(TAG, "Firmware signature check passed");
    } else if (this->config.requireSignedUpdates) {
        ESP_LOGE(TAG, "Signed updates are required but %s, aborting update",
                 hasSignature ? "no public key is configured" : "the update has no signature");
        esp_ota_abort(otaHandle);
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::SignatureFailed);
        return ESP_ERR_INVALID_STATE;
    } else if (hasSignature) {
        ESP_LOGW(TAG, "Update is signed but no public key is configured, skipping signature verification");
    }

    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to finalize OTA update: %s", esp_err_to_name(err));
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::EndFailed);
        return err;
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        transmitEvent(OtamaticClientEvent::OperationFailed, OtamaticClientError::EndFailed);
        return err;
    }

    ESP_LOGI(TAG, "Firmware version %lu installed successfully",
             static_cast<unsigned long>(pendingUpdate.version));

    transmitEvent(OtamaticClientEvent::UpdateCompleted, OtamaticClientError::None, 0,
                  pendingUpdate.version, pendingUpdate.size);

    if (this->config.autoRestartEnabled) {
        ESP_LOGI(TAG, "Restarting to boot the new firmware");
        esp_restart();
    }

    return ESP_OK;

}