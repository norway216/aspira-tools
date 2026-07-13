/**
 * @file test_package_builder.cpp
 * @brief Implementation of the test package builder.
 *
 * Creates a minimal .espkg file format:
 *   [4-byte magic: "ESPK"]
 *   [4-byte manifest_length LE]
 *   [manifest JSON]
 *   [payload entries... each: 4-byte name_len LE, name, 8-byte data_len LE, data]
 *   [32-byte SHA256 of all above]
 *   [64-byte signature placeholder]
 */

#include "test_package_builder.h"
#include "installer/core/error_codes.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <sstream>

namespace installer {
namespace test {

TestPackageBuilder& TestPackageBuilder::with_manifest(const Manifest& manifest) {
    manifest_ = manifest;
    manifest_set_ = true;
    return *this;
}

TestPackageBuilder& TestPackageBuilder::with_payload(
    const std::string& name,
    const std::vector<uint8_t>& data,
    const std::string& type) {
    payloads_.push_back({name, data, type});
    return *this;
}

TestPackageBuilder& TestPackageBuilder::sign(const std::string& private_key_path) {
    signing_key_path_ = private_key_path;
    signed_ = true;
    return *this;
}

Result<std::string> TestPackageBuilder::build() {
    try {
        // Serialize manifest to JSON
        nlohmann::json manifest_json;
        manifest_json["format_version"]        = manifest_.format_version;
        manifest_json["package_id"]            = manifest_.package_id;
        manifest_json["product"]               = manifest_.product;
        manifest_json["version"]               = manifest_.version;
        manifest_json["build_id"]              = manifest_.build_id;
        manifest_json["architecture"]          = manifest_.architecture;
        manifest_json["min_disk_size_bytes"]   = manifest_.min_disk_size_bytes;
        manifest_json["allow_downgrade"]       = manifest_.allow_downgrade;

        auto& hw = manifest_json["hardware_profiles"];
        hw = nlohmann::json::array();
        for (const auto& p : manifest_.hardware_profiles) {
            hw.push_back(p);
        }

        auto& pl_arr = manifest_json["payloads"];
        pl_arr = nlohmann::json::array();
        for (const auto& p : payloads_) {
            nlohmann::json entry;
            entry["name"] = p.name;
            entry["type"] = p.type;
            entry["size"] = p.data.size();

            // Simple SHA256 — replace with real hash for production tests
            entry["sha256"] = "00000000000000000000000000000000"
                              "00000000000000000000000000000000";
            pl_arr.push_back(entry);
        }

        std::string manifest_str = manifest_json.dump();

        // Build the binary package
        std::ostringstream pkg(std::ios::binary);

        // Magic
        const char magic[] = "ESPK";
        pkg.write(magic, 4);

        // Manifest length (LE u32)
        uint32_t manifest_len = static_cast<uint32_t>(manifest_str.size());
        pkg.write(reinterpret_cast<const char*>(&manifest_len), 4);

        // Manifest JSON
        pkg.write(manifest_str.data(),
                  static_cast<std::streamsize>(manifest_str.size()));

        // Payload entries
        for (const auto& pl : payloads_) {
            // Name length (LE u32)
            uint32_t name_len = static_cast<uint32_t>(pl.name.size());
            pkg.write(reinterpret_cast<const char*>(&name_len), 4);

            // Name
            pkg.write(pl.name.data(),
                      static_cast<std::streamsize>(pl.name.size()));

            // Data length (LE u64)
            uint64_t data_len = static_cast<uint64_t>(pl.data.size());
            pkg.write(reinterpret_cast<const char*>(&data_len), 8);

            // Data
            pkg.write(reinterpret_cast<const char*>(pl.data.data()),
                      static_cast<std::streamsize>(pl.data.size()));
        }

        // Write to temp file
        char tmp_path[] = "/tmp/installer-test-package-XXXXXX";
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd < 0) {
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Package Builder",
                                     "Failed to create temp file",
                                     std::strerror(errno)));
        }
        close(tmp_fd);

        std::string file_path(tmp_path);
        // Rename to .espkg extension
        std::string final_path = file_path + ".espkg";
        std::rename(file_path.c_str(), final_path.c_str());

        std::ofstream ofs(final_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Package Builder",
                                     "Failed to open output file: " + final_path));
        }

        std::string pkg_data = pkg.str();

        // Append SHA-256 placeholder (32 zero bytes)
        const char zero_hash[32] = {};
        pkg_data.append(zero_hash, 32);

        // Append signature placeholder (64 zero bytes)
        const char zero_sig[64] = {};
        pkg_data.append(zero_sig, 64);

        ofs.write(pkg_data.data(),
                  static_cast<std::streamsize>(pkg_data.size()));
        ofs.close();

        if (!ofs) {
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Package Builder",
                                     "Failed to write package file"));
        }

        return Result<std::string>::ok(final_path);
    } catch (const std::exception& e) {
        return Result<std::string>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Package Builder",
                                 "Failed to build package",
                                 e.what()));
    }
}

} // namespace test
} // namespace installer
