#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Update Data for the Otamatic OTA client.
 */
struct OtamaticUpdate {
    
    static constexpr size_t kSignatureMaxLen = 144;
    static constexpr size_t kIntegrityMaxLen = 64;

    uint32_t version = 0;
    uint32_t size = 0;
    char signature[kSignatureMaxLen + 1] = {0};
    char integrity[kIntegrityMaxLen + 1] = {0};

    /// @brief Resets the update data to an empty, invalid state.
    void clear() {
        version = 0;
        size = 0;
        signature[0] = '\0';
        integrity[0] = '\0';
    }

    bool isValid() const {
        if (version == 0) return false;
        if (size == 0) return false;
        if (integrity[0] == '\0') return false;
        return true;
    }

};