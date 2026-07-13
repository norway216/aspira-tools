/**
 * @file test_package_builder.h
 * @brief Builder for creating test .espkg files in integration tests.
 */

#ifndef INSTALLER_TEST_PACKAGE_BUILDER_H
#define INSTALLER_TEST_PACKAGE_BUILDER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <cstdint>
#include <string>
#include <vector>

namespace installer {
namespace test {

class TestPackageBuilder {
public:
    TestPackageBuilder() = default;

    /**
     * Set the manifest for this package.
     */
    TestPackageBuilder& with_manifest(const Manifest& manifest);

    /**
     * Add a payload entry.
     *
     * @param name  Payload name, e.g. "kernel_b".
     * @param data  Raw bytes of the payload.
     * @param type  Payload type, e.g. "raw", "ext4_zstd", "tar_zst".
     */
    TestPackageBuilder& with_payload(const std::string& name,
                                     const std::vector<uint8_t>& data,
                                     const std::string& type = "raw");

    /**
     * Sign the package with the given Ed25519 private key file.
     * (Stub — always succeeds; replace with real libsodium signing.)
     */
    TestPackageBuilder& sign(const std::string& private_key_path);

    /**
     * Build the .espkg file and return its path.
     * The file is created in /tmp with a unique name.
     */
    Result<std::string> build();

private:
    Manifest manifest_;
    bool manifest_set_ = false;

    struct Payload {
        std::string name;
        std::vector<uint8_t> data;
        std::string type;
    };
    std::vector<Payload> payloads_;

    std::string signing_key_path_;
    bool signed_ = false;
};

} // namespace test
} // namespace installer

#endif // INSTALLER_TEST_PACKAGE_BUILDER_H
