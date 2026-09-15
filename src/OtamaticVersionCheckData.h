#pragma once

#include <Arduino.h>

/**
 * Holds the data returned by a firmware version check, so it can be
 * used later (e.g. to download and verify the new firmware).
 */
class OtamaticVersionCheckData {
public:
    /**
     * Maximum length (excluding the null terminator) of the signature buffer.
     * Sized to hold the hex encoding of an ECDSA P-256 signature in DER form
     * (max ~72 bytes -> 144 hex chars).
     */
    static constexpr size_t kSignatureMaxLen = 144;

    /** Maximum length (excluding the null terminator) of the integrity buffer. */
    static constexpr size_t kIntegrityMaxLen = 64;

    /** The new firmware version number. */
    uint32_t version = 0;

    /** The size in bytes of the new firmware binary. */
    uint32_t size = 0;

    /** The hex-encoded signature of the new firmware. */
    char signature[kSignatureMaxLen + 1] = {0};

    /** The integrity hash (e.g. SHA-256) of the new firmware. */
    char integrity[kIntegrityMaxLen + 1] = {0};

    /**
     * Reset all the fields to their default (empty) values.
     */
    void clear() {
        version = 0;
        size = 0;
        signature[0] = '\0';
        integrity[0] = '\0';
    }

    /**
     * Copy the given signature into the internal buffer.
     * @param signature The hex-encoded signature.
     */
    void setSignature(const char* signature) {
        strlcpy(this->signature, signature != nullptr ? signature : "", sizeof(this->signature));
    }

    /**
     * Copy the given integrity hash into the internal buffer.
     * @param integrity The integrity hash (e.g. SHA-256).
     */
    void setIntegrity(const char* integrity) {
        strlcpy(this->integrity, integrity != nullptr ? integrity : "", sizeof(this->integrity));
    }

    /**
     * Get the signature as a read-only string.
     * @return The hex-encoded signature, or an empty string if not available.
     */
    const char* getSignature() const { return signature; }

    /**
     * Get the integrity hash as a read-only string.
     * @return The integrity hash, or an empty string if not available.
     */
    const char* getIntegrity() const { return integrity; }

    /**
     * Check if the data contains a valid version check result.
     * @return true if a version number is stored.
     */
    bool isValid() const { return version > 0; }
};
