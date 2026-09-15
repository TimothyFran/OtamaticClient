#include "OtamaticClient.h"
#include <esp_mac.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

OtamaticClient::OtamaticClient(Client& client) : _client(&client) {}

OtamaticClient::~OtamaticClient() {}

bool OtamaticClient::begin(uint32_t currentFwVersion, const char* serviceKey) {
    _firmwareVersion = currentFwVersion;

    strncpy(_serviceKey, serviceKey, sizeof(_serviceKey));
    _serviceKey[sizeof(_serviceKey) - 1] = 0;

    _client->setTimeout(1000);

    log_i("Initialized, firmware version: %lu", (unsigned long)_firmwareVersion);
    return true;
}

void OtamaticClient::loop() {
    if (millis() - _lastCheck < _checkInterval) return;
    requestCheckNow(true);
}

void OtamaticClient::onEvent(void (*callback)(OtamaticClientEventData)) {
    _onEvent = callback;
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

void OtamaticClient::requestCheckNow(bool restartCounter) {
    uint32_t prevCheck = _lastCheck;
    if (restartCounter) _lastCheck = millis();

    // Clear the previously stored version data at the start of every check
    _checkData.clear();

    transmitEvent(OtamaticClientEvent::CheckingForUpdate);

    HTTPClient http;

    char _checkPath[75] = {0};
    snprintf(_checkPath, sizeof(_checkPath), "/api/v1/ota/latest?currentVersion=%lu&deviceId=%llu",
             _firmwareVersion, getDeviceId());

    NetworkClient& netClient = *static_cast<NetworkClient*>(_client);
    if(! http.begin(netClient, _serverHost, _serverPort, _checkPath)) {
        log_e("HTTP begin failed (check, host: %s:%u)", _serverHost, _serverPort);
        _lastCheck = prevCheck;
        return;
    }

    char bearer[64];
    sprintf(bearer, "Bearer %s", _serviceKey);

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", bearer);

    int httpCode = http.GET();
    if (httpCode != 200) {
        log_e("Version check HTTP request failed, code: %d", httpCode);
        http.end();
        _lastCheck = prevCheck;
        return;
    }

    // Ex: {"version":42,"size":425984,"signature":"c2VjcmV0LXN0YXRpYy10ZXN0LXNpZ25hdHVyZQ==","integrity":"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"}
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();
    if (error) {
        log_e("JSON deserialization failed: %s", error.c_str());
        _lastCheck = prevCheck;
        return;
    }

    uint32_t newVersion = doc["version"];
    if (newVersion <= _firmwareVersion) {
        log_i("No update needed (current: %lu, remote: %lu)", (unsigned long)_firmwareVersion, (unsigned long)newVersion);
        transmitEvent(OtamaticClientEvent::UpdateNotNeeded);
        return;
    }

    log_i("Update available, version: %lu, size: %lu", (unsigned long)newVersion, (unsigned long)doc["size"] | 0UL);

    // Memorize new version data for later use (e.g. download and verification)
    _checkData.version = newVersion;
    _checkData.size = doc["size"] | 0;
    _checkData.setSignature(doc["signature"] | "");
    _checkData.setIntegrity(doc["integrity"] | "");

    transmitEvent(OtamaticClientEvent::UpdateAvailable);

    if (!_autoUpdate) return;

    applyUpdate();
}

bool OtamaticClient::applyUpdate() {
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

    char bearer[64] = {0};
    snprintf(bearer, sizeof(bearer), "Bearer %s", _serviceKey);

    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("Authorization", bearer);

    int httpCode = http.GET();
    if (httpCode != 200) {
        log_e("%s, HTTP code: %d", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::HttpFailed)), httpCode);
        http.end();
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)OtamaticClientError::HttpFailed);
        return false;
    }

    if (_checkData.size == 0) {
        log_e("%s (size: 0)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(OtamaticClientError::InvalidFirmware)));
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

    // Download the binary in chunks and write it to the OTA partition
    Stream* stream = http.getStreamPtr();
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

        // Emit the progress event only when the percentage changes
        uint32_t progress = (uint32_t)((uint64_t)written * 100 / _checkData.size);
        if (progress != lastProgress) {
            lastProgress = progress;
            log_d("Update progress: %lu%% (%lu/%lu bytes)", (unsigned long)progress, (unsigned long)written, (unsigned long)_checkData.size);
            transmitEvent(OtamaticClientEvent::UpdateProgress, (uint8_t)progress);
        }
    }

    http.end();

    if (failed || written != _checkData.size) {
        Update.abort();
        log_e("%s (written: %lu/%lu)", reinterpret_cast<const char*>(OtamaticClient::getErrorName(failedError)), (unsigned long)written, (unsigned long)_checkData.size);
        transmitEvent(OtamaticClientEvent::UpdateFailed, (uint8_t)failedError);
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
        default: return F("Unknown error");
    }
}