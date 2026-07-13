/**
 * @file loop_device.cpp
 * @brief Implementation of the RAII loop device wrapper for integration tests.
 */

#include "loop_device.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace installer {
namespace test {

// ============================================================================
//  Command execution helper
// ============================================================================

std::string LoopDevice::run_command(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result += buf;
    }

    int status = pclose(pipe);
    (void)status;  // ignore exit code for losetup listing

    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// ============================================================================
//  Create
// ============================================================================

bool LoopDevice::create(uint64_t size_mib, uint32_t sector_size) {
    if (is_valid()) {
        std::cerr << "LoopDevice: already attached to " << device_path_
                  << std::endl;
        return false;
    }

    // Create a unique backing file name
    char tmp_path[] = "/tmp/installer_test_disk_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        std::cerr << "LoopDevice: failed to create temp file: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    close(tmp_fd);
    backing_file_ = tmp_path;

    // Create a sparse file of the requested size
    uint64_t size_bytes = size_mib * 1024 * 1024;
    std::ofstream ofs(backing_file_, std::ios::binary | std::ios::out);
    if (!ofs) {
        std::cerr << "LoopDevice: failed to open backing file for writing"
                  << std::endl;
        remove_backing_file();
        return false;
    }

    // Seek to the last byte and write to create a sparse file
    ofs.seekp(static_cast<std::streamoff>(size_bytes - 1));
    ofs.put('\0');
    ofs.close();

    // Attach as loop device
    std::ostringstream cmd;
    cmd << "losetup --find --show --sector-size " << sector_size
        << " " << backing_file_;
    device_path_ = run_command(cmd.str());

    if (device_path_.empty()) {
        std::cerr << "LoopDevice: losetup failed: " << cmd.str() << std::endl;
        remove_backing_file();
        return false;
    }

    size_bytes_ = size_bytes;
    owns_backing_file_ = true;
    return true;
}

// ============================================================================
//  Attach
// ============================================================================

bool LoopDevice::attach(const std::string& image_path) {
    if (is_valid()) {
        std::cerr << "LoopDevice: already attached to " << device_path_
                  << std::endl;
        return false;
    }

    // Verify the file exists and get its size
    struct stat st {};
    if (stat(image_path.c_str(), &st) != 0) {
        std::cerr << "LoopDevice: cannot stat " << image_path << ": "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    uint64_t file_size = static_cast<uint64_t>(st.st_size);

    backing_file_ = image_path;

    std::ostringstream cmd;
    cmd << "losetup --find --show " << backing_file_;
    device_path_ = run_command(cmd.str());

    if (device_path_.empty()) {
        std::cerr << "LoopDevice: losetup failed for " << backing_file_
                  << std::endl;
        return false;
    }

    size_bytes_ = file_size;
    owns_backing_file_ = false;  // don't delete an existing file
    return true;
}

// ============================================================================
//  Destructor / Move
// ============================================================================

LoopDevice::~LoopDevice() {
    detach();
    remove_backing_file();
}

LoopDevice::LoopDevice(LoopDevice&& other) noexcept
    : backing_file_(std::move(other.backing_file_))
    , device_path_(std::move(other.device_path_))
    , size_bytes_(other.size_bytes_)
    , owns_backing_file_(other.owns_backing_file_)
{
    other.device_path_.clear();
    other.backing_file_.clear();
    other.size_bytes_ = 0;
    other.owns_backing_file_ = false;
}

LoopDevice& LoopDevice::operator=(LoopDevice&& other) noexcept {
    if (this != &other) {
        detach();
        remove_backing_file();
        backing_file_ = std::move(other.backing_file_);
        device_path_ = std::move(other.device_path_);
        size_bytes_ = other.size_bytes_;
        owns_backing_file_ = other.owns_backing_file_;
        other.device_path_.clear();
        other.backing_file_.clear();
        other.size_bytes_ = 0;
        other.owns_backing_file_ = false;
    }
    return *this;
}

// ============================================================================
//  Accessors
// ============================================================================

std::string LoopDevice::device_path() const {
    return device_path_;
}

std::string LoopDevice::backing_file() const {
    return backing_file_;
}

uint64_t LoopDevice::size_bytes() const {
    return size_bytes_;
}

bool LoopDevice::is_valid() const {
    return !device_path_.empty();
}

std::vector<std::string> LoopDevice::partition_paths() const {
    std::vector<std::string> paths;
    if (!is_valid()) return paths;

    // Determine partition suffix
    std::string suffix;
    if (device_path_.find("mmcblk") != std::string::npos ||
        device_path_.find("nvme") != std::string::npos ||
        device_path_.find("loop") != std::string::npos) {
        suffix = "p";
    }

    // Scan for partitions (up to 16)
    for (int i = 1; i <= 16; ++i) {
        std::string part_path = device_path_ + suffix + std::to_string(i);
        struct stat st {};
        if (stat(part_path.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
            paths.push_back(part_path);
        } else {
            break;  // assume partitions are contiguous
        }
    }
    return paths;
}

// ============================================================================
//  Detach / Cleanup
// ============================================================================

void LoopDevice::detach() {
    if (device_path_.empty()) return;

    std::ostringstream cmd;
    cmd << "losetup -d " << device_path_;
    std::string output = run_command(cmd.str());

    // losetup -d can fail if already detached; that's fine
    device_path_.clear();
    size_bytes_ = 0;
}

void LoopDevice::remove_backing_file() {
    if (backing_file_.empty()) return;
    if (!owns_backing_file_) return;

    unlink(backing_file_.c_str());
    backing_file_.clear();
    owns_backing_file_ = false;
}

} // namespace test
} // namespace installer
