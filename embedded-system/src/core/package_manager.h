/**
 * @file package_manager.h
 * @brief PackageManager implementation for the .espkg format (tar+zstd container).
 *
 * Implements IPackageManager using external tar/zstd via IProcessRunner
 * and libsodium-backed cryptographic verification via ISecurityManager.
 *
 * Manifest parsing is done with an embedded recursive-descent JSON parser
 * (no external JSON library dependency).
 */

#ifndef INSTALLER_CORE_PACKAGE_MANAGER_H
#define INSTALLER_CORE_PACKAGE_MANAGER_H

#include "installer/IPackageManager.h"
#include "installer/security/isecurity_manager.h"
#include "installer/platform/iprocess_runner.h"
#include "installer/core/types.h"
#include "installer/core/result.h"

#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <istream>
#include <cstdint>

namespace installer {

/**
 * PackageManager — reads, validates, and extracts .espkg packages.
 *
 * .espkg is a tar archive (optionally zstd-compressed) containing:
 *   - manifest.json   — machine-readable metadata
 *   - manifest.sig    — Ed25519 signature over manifest.json
 *   - payload files   — kernel images, rootfs archives, etc.
 *
 * This implementation uses external CLI tools (tar) via IProcessRunner
 * and cryptographic operations via ISecurityManager.
 */
class PackageManager : public IPackageManager {
public:
    /**
     * @param proc_runner  Process runner for invoking tar/zstd commands.
     * @param sec_mgr      Security manager for hash/signature verification.
     *                     May be nullptr if verification is not needed.
     */
    explicit PackageManager(IProcessRunner* proc_runner,
                            ISecurityManager* sec_mgr = nullptr);
    ~PackageManager() override;

    // ---- IPackageManager interface ----

    Result<void> open(const std::string& package_path) override;
    Result<Manifest> load_manifest() override;
    Result<void> verify_payload_hash(const std::string& payload_name) override;
    Result<void> extract_payload(const std::string& payload_name,
                                 const std::string& target_path,
                                 ProgressCallback progress,
                                 CancellationToken& cancel) override;
    Result<uint64_t> get_payload_size(const std::string& payload_name) override;
    void close() override;

    // ---- Extended operations ----

    /**
     * Verify all payload hashes against the manifest.
     *
     * Extracts each payload entry listed in the manifest one at a time,
     * computes its SHA-256 hash via ISecurityManager, and compares it
     * against the expected value.  Individual files are deleted as soon
     * as they are verified to keep temporary space usage minimal.
     *
     * @param cancel  Cancellation token checked between payloads.
     * @return        true if every payload matches; false with error detail
     *                if any hash mismatch is found.
     */
    Result<bool> verify_package(const CancellationToken& cancel = CancellationToken());

    /**
     * Verify the Ed25519 signature over manifest.json.
     *
     * Extracts manifest.sig from the package, computes the SHA-256 digest
     * of the manifest.json content, and verifies the signature using the
     * public key obtained from ISecurityManager.
     *
     * @return  true if the signature is valid, false otherwise.
     */
    Result<bool> verify_signature();

    /**
     * Open a named payload as an input stream.
     *
     * Extracts the payload file to a temporary location and returns a
     * std::istream that reads the uncompressed data.  The stream is
     * backed by a std::ifstream and the temporary file is removed when
     * the stream object is destroyed (via a custom deleter).
     *
     * @param payload_name  Logical payload name as listed in the manifest.
     * @return              A unique_ptr to an istream reading the payload data.
     */
    Result<std::unique_ptr<std::istream>> open_payload(const std::string& payload_name);

    /**
     * Check whether a manifest is compatible with the current hardware.
     *
     * Verifies:
     *  - Architecture matches the running system (uname -m).
     *  - The supplied hw_profile is listed in manifest.hardware_profiles.
     *  - min_disk_size_bytes is a positive, reasonable value.
     *
     * @param manifest    The manifest to check.
     * @param hw_profile  Hardware profile identifier to validate.
     * @return            true if compatible, false otherwise.
     */
    Result<bool> check_compatibility(const Manifest& manifest,
                                     const std::string& hw_profile);

private:
    // ---- Internal helpers (caller must hold mutex_) ----

    /// Extract a single file from the package archive into temp_dir_.
    Result<std::string> extract_file(const std::string& archive_path,
                                     const std::string& file_name);

    /// Parse a manifest JSON file on disk and populate a Manifest struct.
    Result<Manifest> parse_manifest_json(const std::string& json_path);

    /// Recursively remove a directory tree.
    static bool remove_directory(const std::string& path);

    /// Resolve payload file name from logical payload name using cached manifest.
    Result<std::string> resolve_payload_file(const std::string& payload_name);

    // ---- Data members ----

    IProcessRunner* proc_runner_;
    ISecurityManager* sec_mgr_;

    std::string package_path_;
    std::string temp_dir_;
    bool is_open_ = false;

    Manifest manifest_;
    bool manifest_loaded_ = false;

    mutable std::recursive_mutex mutex_;
};

} // namespace installer

#endif // INSTALLER_CORE_PACKAGE_MANAGER_H
