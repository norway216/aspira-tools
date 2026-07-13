/**
 * @file file_utils.h
 * @brief Atomic file operations, RAII file descriptors, and temporary directories.
 *
 * Every fallible function returns Result<T> so callers cannot ignore errors.
 * The atomic-write pattern ensures that a file is never observed in a
 * partially-written state: write to .partial, fsync, rename, fsync parent dir.
 */

#ifndef INSTALLER_COMMON_FILE_UTILS_H
#define INSTALLER_COMMON_FILE_UTILS_H

#include "installer/core/result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace installer {

// =============================================================================
// File I/O utilities
// =============================================================================

/**
 * Atomically write binary data to a file.
 *
 * The sequence is:
 *   1. Open  path + ".partial"  with O_WRONLY|O_CREAT|O_TRUNC
 *   2. Write all data (loop for partial writes)
 *   3. fsync() the file descriptor
 *   4. close the file descriptor
 *   5. rename(path + ".partial", path)
 *   6. Open the parent directory, fsync() it, close it
 *
 * If any step fails the .partial file is left on disk for inspection.
 *
 * @param path  Destination file path.
 * @param data  Binary content to write.
 * @param mode  File permissions (default 0644).
 * @return      Ok on success, or an InstallerError.
 */
Result<void> atomic_write_file(const std::string& path,
                               const std::vector<uint8_t>& data,
                               mode_t mode = 0644);

/**
 * Atomically write a string to a file (convenience wrapper).
 *
 * @see atomic_write_file
 */
Result<void> atomic_write_text(const std::string& path,
                               const std::string& content,
                               mode_t mode = 0644);

/**
 * Read an entire file into a string.
 *
 * @param path  File to read.
 * @return      File contents as a string, or an InstallerError.
 */
Result<std::string> read_file(const std::string& path);

/**
 * Check whether a file (or directory) exists at the given path.
 *
 * @return true if the path exists and is accessible.
 */
bool file_exists(const std::string& path);

/**
 * Get the size of a regular file in bytes.
 *
 * @return File size, or an InstallerError (e.g. if the path is a directory).
 */
Result<uint64_t> file_size(const std::string& path);

/**
 * Remove a file and any stale .partial variant.
 *
 * Succeeds (returns Ok) if the file does not exist.
 *
 * @param path  File to remove.
 */
Result<void> remove_file(const std::string& path);

/**
 * Ensure that a directory exists, creating parent directories as needed
 * (equivalent to `mkdir -p`).
 *
 * New directories are created with mode 0755.
 *
 * @param path  Directory path to ensure.
 */
Result<void> ensure_directory(const std::string& path);

// =============================================================================
// RAII Temporary Directory
// =============================================================================

/**
 * A temporary directory that is automatically (recursively) removed when
 * the ScopedTempDir object goes out of scope.
 *
 * Usage:
 *   auto tmp = ScopedTempDir::create("installer_");
 *   if (!tmp) { handle_error(tmp.error()); }
 *   std::string dir = tmp->value()->path();
 *
 * Non-copyable, movable.
 */
class ScopedTempDir {
public:
    /**
     * Create a temporary directory under /tmp.
     *
     * @param prefix  Prefix for the directory name (default "installer_").
     * @return        A unique_ptr to the directory handle, or an error.
     */
    static Result<std::unique_ptr<ScopedTempDir>> create(
        const std::string& prefix = "installer_");

    ~ScopedTempDir();

    /** Return the absolute path to the temporary directory. */
    std::string path() const { return path_; }

    // Non-copyable
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    // Movable
    ScopedTempDir(ScopedTempDir&& other) noexcept;
    ScopedTempDir& operator=(ScopedTempDir&& other) noexcept;

private:
    explicit ScopedTempDir(std::string path);

    static void remove_recursive(const std::string& dir_path);

    std::string path_;
};

// =============================================================================
// RAII File Descriptor
// =============================================================================

/**
 * Owns a POSIX file descriptor and closes it on destruction.
 *
 * Usage:
 *   ScopedFd fd(open("/some/file", O_RDONLY));
 *   if (!fd.is_valid()) { ... }
 *   // fd is automatically closed when it goes out of scope
 */
class ScopedFd {
public:
    /** Construct from a raw fd.  -1 means "invalid / empty". */
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

    ~ScopedFd();

    // Movable
    ScopedFd(ScopedFd&& other) noexcept;
    ScopedFd& operator=(ScopedFd&& other) noexcept;

    // Non-copyable
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    /** Return the raw fd (or -1). */
    int get() const noexcept { return fd_; }

    /** True if the fd is >= 0. */
    bool is_valid() const noexcept { return fd_ >= 0; }

    /** Explicitly close the fd.  Safe to call multiple times. */
    void close() noexcept;

private:
    int fd_;
};

} // namespace installer

#endif // INSTALLER_COMMON_FILE_UTILS_H
