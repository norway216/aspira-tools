/**
 * @file loop_device.h
 * @brief RAII wrapper for Linux loop devices used in integration tests.
 *
 * LoopDevice creates and manages a loop device backed by a file or
 * a sparse image. The loop device is automatically detached on
 * destruction, making it safe for use in test fixtures.
 */

#ifndef INSTALLER_TESTS_LOOP_DEVICE_H
#define INSTALLER_TESTS_LOOP_DEVICE_H

#include <cstdint>
#include <string>
#include <vector>

namespace installer {
namespace test {

/**
 * RAII loop device for integration testing.
 *
 * Usage:
 *   LoopDevice loop;
 *   auto result = loop.create(512);       // 512 MiB disk
 *   if (result) {
 *       std::string dev = loop.device_path();
 *       // ... use the loop device ...
 *   }
 *   // Loop device and backing file are cleaned up on scope exit.
 */
class LoopDevice {
public:
    /**
     * Create a loop device backed by a file of the given size.
     *
     * The backing file is created as a sparse file in /tmp.
     *
     * @param size_mib       Size of the disk image in MiB.
     * @param sector_size    Logical sector size in bytes (default 512).
     * @return               true on success, false on failure.
     */
    bool create(uint64_t size_mib, uint32_t sector_size = 512);

    /**
     * Create a loop device backed by an existing file.
     *
     * @param image_path     Path to the existing disk image file.
     * @return               true on success, false on failure.
     */
    bool attach(const std::string& image_path);

    /**
     * Detach the loop device and optionally remove the backing file.
     */
    ~LoopDevice();

    // Non-copyable
    LoopDevice(const LoopDevice&) = delete;
    LoopDevice& operator=(const LoopDevice&) = delete;

    // Movable
    LoopDevice(LoopDevice&& other) noexcept;
    LoopDevice& operator=(LoopDevice&& other) noexcept;

    LoopDevice() = default;

    /**
     * Returns the loop device path, e.g. "/dev/loop0".
     * Returns an empty string if not attached.
     */
    std::string device_path() const;

    /**
     * Returns the backing file path. Empty if not created.
     */
    std::string backing_file() const;

    /**
     * Returns the device size in bytes.
     */
    uint64_t size_bytes() const;

    /**
     * Returns true if the loop device is successfully attached.
     */
    bool is_valid() const;

    /**
     * Returns the partition paths for this loop device.
     * e.g. {"/dev/loop0p1", "/dev/loop0p2", ...}
     */
    std::vector<std::string> partition_paths() const;

    /**
     * Force detach the loop device (ignoring errors).
     * Called automatically by the destructor.
     */
    void detach();

    /**
     * Removes the backing file if one was created by create().
     */
    void remove_backing_file();

private:
    /**
     * Execute a command and capture stdout.
     * @return The first line of stdout on success, empty string on failure.
     */
    static std::string run_command(const std::string& cmd);

    std::string backing_file_;
    std::string device_path_;
    uint64_t size_bytes_ = 0;
    bool owns_backing_file_ = false;
};

} // namespace test
} // namespace installer

#endif // INSTALLER_TESTS_LOOP_DEVICE_H
