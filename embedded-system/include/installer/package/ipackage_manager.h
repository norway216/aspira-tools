/**
 * @file ipackage_manager.h
 * @brief Installation package handling interface.
 *
 * Manages the lifecycle of an installation package: loading its manifest,
 * verifying cryptographic signatures and content integrity, checking
 * hardware compatibility, and providing streaming access to individual
 * payloads within the package.
 *
 * @see Architecture Doc §6.3, §9
 */

#ifndef INSTALLER_PACKAGE_IPACKAGE_MANAGER_H
#define INSTALLER_PACKAGE_IPACKAGE_MANAGER_H

#include <istream>
#include <memory>
#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Installation package handling interface.
 *
 * A package is a self-contained archive (tar, SquashFS, or custom format)
 * that bundles:
 * - A manifest describing the package identity, target hardware profiles,
 *   minimum disk size, and the set of enclosed payloads.
 * - One or more payloads (kernel image, root filesystem, bootloader, etc.).
 * - A digital signature over the manifest for authenticity verification.
 *
 * Implementations must validate all metadata before granting access to
 * payload data.
 */
class IPackageManager {
public:
    virtual ~IPackageManager() = default;

    /**
     * Load and parse the manifest from a package.
     *
     * The manifest is the authoritative description of the package
     * contents: identity, version, hardware compatibility, and the list
     * of payloads with their expected sizes and hashes.
     *
     * @param package_path Path to the package file or directory.
     * @return The parsed Manifest structure, or an InstallerError
     *         (PACKAGE_INVALID_FORMAT, PACKAGE_MANIFEST_ERROR).
     */
    virtual Result<Manifest> load_manifest(const std::string& package_path) = 0;

    /**
     * Verify the integrity of every payload listed in the manifest.
     *
     * Recomputes the SHA-256 hash of each payload and compares it
     * against the expected value recorded in the manifest. This check
     * is purely about data integrity (corruption detection), not
     * authenticity (see verify_signature).
     *
     * @param package_path Path to the package file or directory.
     * @return Result<void> — ok if all hashes match,
     *         PACKAGE_HASH_MISMATCH if any payload is corrupted.
     */
    virtual Result<void> verify_package(const std::string& package_path) = 0;

    /**
     * Verify the cryptographic signature over the package manifest.
     *
     * Uses the project's public key infrastructure (Ed25519) to confirm
     * that the manifest was authored by a trusted party and has not been
     * tampered with.
     *
     * @param package_path Path to the package file or directory.
     * @return Result<void> — ok if the signature is valid,
     *         PACKAGE_SIGNATURE_FAIL otherwise.
     */
    virtual Result<void> verify_signature(const std::string& package_path) = 0;

    /**
     * Open a named payload for sequential, read-only streaming access.
     *
     * The caller owns the returned stream and must destroy it to release
     * underlying resources (file descriptors, decompression contexts).
     *
     * @param package_path Path to the package file or directory.
     * @param payload_name Logical name of the payload as listed in the
     *                     manifest (e.g. "kernel_b", "rootfs_a").
     * @return A unique_ptr to an input stream positioned at the start
     *         of the payload data, or an error if the payload is not
     *         found or cannot be opened.
     */
    virtual Result<std::unique_ptr<std::istream>> open_payload(
        const std::string& package_path,
        const std::string& payload_name) = 0;

    /**
     * Check whether the package is compatible with a specific hardware
     * profile.
     *
     * Verifies that the target hardware_profile_id appears in the
     * manifest's hardware_profiles list and that the architecture field
     * matches the running system.
     *
     * @param manifest            The previously loaded package manifest.
     * @param hardware_profile_id Identifier of the target hardware
     *                            (e.g. "acme-gen2-revB").
     * @return Result<void> — ok if compatible,
     *         PACKAGE_HW_INCOMPATIBLE otherwise.
     */
    virtual Result<void> check_compatibility(
        const Manifest& manifest,
        const std::string& hardware_profile_id) = 0;
};

} // namespace installer

#endif // INSTALLER_PACKAGE_IPACKAGE_MANAGER_H
