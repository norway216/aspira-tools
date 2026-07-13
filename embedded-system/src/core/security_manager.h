/**
 * @file security_manager.h
 * @brief SecurityManager implementation of ISecurityManager using libsodium,
 *        with OpenSSL and standalone fallbacks for embedded Linux targets.
 */

#ifndef INSTALLER_CORE_SECURITY_MANAGER_H
#define INSTALLER_CORE_SECURITY_MANAGER_H

#include "installer/security/isecurity_manager.h"
#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>
#include <vector>
#include <mutex>

namespace installer {

class SecurityManager : public ISecurityManager {
public:
    SecurityManager();
    explicit SecurityManager(const std::string& public_key_path);
    ~SecurityManager() override;

    // ---- ISecurityManager interface ----

    Result<std::string> sha256_file(const std::string& file_path) override;
    Result<std::string> sha256_stream(std::istream& stream) override;
    std::string sha256_data(const std::vector<uint8_t>& data) override;

    Result<bool> verify_ed25519(const std::vector<uint8_t>& message_digest,
                                 const std::vector<uint8_t>& signature,
                                 const std::vector<uint8_t>& public_key) override;

    Result<std::vector<uint8_t>> load_public_key() override;

    Result<std::vector<uint8_t>> encrypt_aes256_gcm(const std::vector<uint8_t>& plaintext,
                                                      const std::vector<uint8_t>& key) override;

    Result<std::vector<uint8_t>> decrypt_aes256_gcm(const std::vector<uint8_t>& ciphertext,
                                                      const std::vector<uint8_t>& key) override;

    // ---- Additional helpers ----

    /// Compute SHA-256 of a file reading in user-specified chunk sizes.
    Result<std::string> sha256_file_chunked(const std::string& file_path, size_t chunk_size = 65536);

    /// Convert raw bytes to a lowercase hex string.
    static std::string bytes_to_hex(const std::vector<uint8_t>& bytes);

    /// Convert a hex string to raw bytes. Returns error on invalid input.
    static Result<std::vector<uint8_t>> hex_to_bytes(const std::string& hex);

private:
    std::string public_key_path_;
    std::vector<uint8_t> embedded_public_key_;  // Built-in fallback key
    mutable std::mutex mutex_;
};

} // namespace installer

#endif // INSTALLER_CORE_SECURITY_MANAGER_H
