/**
 * @file isecurity_manager.h
 * @brief Cryptographic operations interface.
 *
 * Centralises all cryptographic primitives used across the installer:
 * SHA-256 hashing (in-memory and streaming), Ed25519 signature
 * verification, and AES-256-GCM authenticated encryption for
 * sensitive data at rest.
 *
 * @see Architecture Doc §9.2, §20
 */

#ifndef INSTALLER_SECURITY_ISECURITY_MANAGER_H
#define INSTALLER_SECURITY_ISECURITY_MANAGER_H

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Cryptographic operations interface.
 *
 * Provides hashing, digital signature verification, and authenticated
 * encryption. Implementations may use OpenSSL, libsodium, mbedTLS, or
 * kernel crypto APIs — the interface hides the backing library.
 *
 * All methods that can fail return Result<T>; methods that cannot fail
 * (in-memory hash computation) return values directly.
 */
class ISecurityManager {
public:
    virtual ~ISecurityManager() = default;

    /**
     * Verify an Ed25519 digital signature over a data buffer.
     *
     * @param data       The raw data whose authenticity is being verified.
     * @param signature  The Ed25519 signature (64 bytes).
     * @param public_key The Ed25519 public key as a PEM string or raw
     *                   32-byte key.
     * @return true if the signature is valid for the given data and key,
     *         false if it does not verify. Returns an error if the key
     *         or signature format is invalid.
     */
    virtual Result<bool> verify_signature(
        const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& signature,
        const std::string& public_key) = 0;

    /**
     * Compute the SHA-256 digest of an in-memory buffer.
     *
     * This is a non-fallible convenience for small data; for large or
     * streaming data use compute_sha256_stream() instead.
     *
     * @param data Input bytes.
     * @return Lowercase hex-encoded SHA-256 digest string (64 chars).
     */
    virtual std::string compute_sha256(
        const std::vector<uint8_t>& data) = 0;

    /**
     * Compute the SHA-256 digest of an entire input stream.
     *
     * Reads the stream from its current position to EOF, updating the
     * hash incrementally. The stream position after this call is
     * unspecified.
     *
     * @param stream Input stream (read from current position to EOF).
     * @return Lowercase hex-encoded SHA-256 digest, or an error if
     *         a read error occurs.
     */
    virtual Result<std::string> compute_sha256_stream(
        std::istream& stream) = 0;

    /**
     * Verify that a data buffer matches an expected SHA-256 digest.
     *
     * Convenience method equivalent to:
     *   compute_sha256(data) == expected_hash
     *
     * @param data          Input bytes.
     * @param expected_hash Lowercase hex-encoded SHA-256 digest.
     * @return true if the computed hash matches.
     */
    virtual bool verify_sha256(const std::vector<uint8_t>& data,
                               const std::string& expected_hash) = 0;

    /**
     * Encrypt plaintext using AES-256-GCM.
     *
     * The returned vector contains: nonce (12 bytes) || ciphertext ||
     * authentication tag (16 bytes). The nonce is randomly generated
     * for each call.
     *
     * @param plaintext Data to encrypt.
     * @param key       32-byte (256-bit) AES key.
     * @return The concatenated nonce + ciphertext + tag, or an error.
     */
    virtual Result<std::vector<uint8_t>> encrypt_aes256_gcm(
        const std::vector<uint8_t>& plaintext,
        const std::vector<uint8_t>& key) = 0;

    /**
     * Decrypt data previously encrypted with encrypt_aes256_gcm().
     *
     * The input format is exactly: nonce (12 bytes) || ciphertext ||
     * authentication tag (16 bytes). The GCM authentication tag is
     * verified before any plaintext is returned.
     *
     * @param ciphertext The concatenated nonce + ciphertext + tag.
     * @param key        32-byte (256-bit) AES key.
     * @return The decrypted plaintext, or an error if authentication
     *         fails (tampered or corrupted data).
     */
    virtual Result<std::vector<uint8_t>> decrypt_aes256_gcm(
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& key) = 0;
};

} // namespace installer

#endif // INSTALLER_SECURITY_ISECURITY_MANAGER_H
