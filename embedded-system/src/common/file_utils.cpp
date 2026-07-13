/**
 * @file file_utils.cpp
 * @brief Implementation of atomic file operations and RAII helpers.
 */

#include "file_utils.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace installer {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/**
 * Extract the parent directory from an absolute or relative path.
 * Returns "." if there is no parent component.
 */
std::string parent_dir(const std::string& path) {
    // Use a copy because dirname() may modify its argument.
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    std::string parent(::dirname(buf.data()));
    return parent;
}

/**
 * Create a single directory (non-recursive).
 * Returns 0 on success, -1 on error (errno set).
 */
int make_dir(const char* path, mode_t mode = 0755) {
    return ::mkdir(path, mode);
}

/**
 * Build an InstallerError from errno with a descriptive message.
 */
InstallerError make_errno_error(const std::string& context) {
    const char* msg = ::strerror(errno);
    return InstallerError::make(
        ErrorCode::INTERNAL_ERROR,
        "File Operation Failed",
        context + ": " + std::string(msg),
        context + " (errno=" + std::to_string(errno) + ": " + msg + ")",
        true, false);
}

} // anonymous namespace

// =============================================================================
// ScopedFd
// =============================================================================

ScopedFd::~ScopedFd() {
    close();
}

ScopedFd::ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void ScopedFd::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// =============================================================================
// ScopedTempDir
// =============================================================================

ScopedTempDir::ScopedTempDir(std::string path) : path_(std::move(path)) {}

ScopedTempDir::ScopedTempDir(ScopedTempDir&& other) noexcept
    : path_(std::move(other.path_)) {
    other.path_.clear();
}

ScopedTempDir& ScopedTempDir::operator=(ScopedTempDir&& other) noexcept {
    if (this != &other) {
        remove_recursive(path_);
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

ScopedTempDir::~ScopedTempDir() {
    if (!path_.empty()) {
        remove_recursive(path_);
    }
}

Result<std::unique_ptr<ScopedTempDir>> ScopedTempDir::create(
    const std::string& prefix) {
    std::string tmpl = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    if (::mkdtemp(buf.data()) == nullptr) {
        return Result<std::unique_ptr<ScopedTempDir>>::err(
            make_errno_error("mkdtemp failed for prefix '" + prefix + "'"));
    }

    auto dir = std::unique_ptr<ScopedTempDir>(
        new ScopedTempDir(std::string(buf.data())));
    return Result<std::unique_ptr<ScopedTempDir>>::ok(std::move(dir));
}

void ScopedTempDir::remove_recursive(const std::string& dir_path) {
    DIR* dir = ::opendir(dir_path.c_str());
    if (dir == nullptr) {
        return; // already gone or inaccessible — nothing to do
    }

    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (::strcmp(entry->d_name, ".") == 0 ||
            ::strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full = dir_path + "/" + entry->d_name;
        struct stat st;
        if (::lstat(full.c_str(), &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            remove_recursive(full);
        } else {
            ::unlink(full.c_str());
        }
    }
    ::closedir(dir);
    ::rmdir(dir_path.c_str());
}

// =============================================================================
// file_exists
// =============================================================================

bool file_exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

// =============================================================================
// file_size
// =============================================================================

Result<uint64_t> file_size(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return Result<uint64_t>::err(make_errno_error("stat failed for '" + path + "'"));
    }
    if (!S_ISREG(st.st_mode)) {
        return Result<uint64_t>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Not a regular file",
            "Cannot get size: '" + path + "' is not a regular file.",
            path + " is not a regular file (mode=" + std::to_string(st.st_mode) + ")",
            false, false));
    }
    return Result<uint64_t>::ok(static_cast<uint64_t>(st.st_size));
}

// =============================================================================
// read_file
// =============================================================================

Result<std::string> read_file(const std::string& path) {
    ScopedFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.is_valid()) {
        return Result<std::string>::err(
            make_errno_error("open failed for reading '" + path + "'"));
    }

    // Get file size
    struct stat st;
    if (::fstat(fd.get(), &st) != 0) {
        return Result<std::string>::err(
            make_errno_error("fstat failed for '" + path + "'"));
    }

    if (!S_ISREG(st.st_mode)) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Not a regular file",
            "Cannot read '" + path + "': not a regular file.",
            path + " is not a regular file.",
            false, false));
    }

    std::string content;
    content.resize(static_cast<size_t>(st.st_size));

    size_t bytes_read = 0;
    while (bytes_read < content.size()) {
        ssize_t n = ::read(fd.get(), &content[bytes_read],
                           content.size() - bytes_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Result<std::string>::err(
                make_errno_error("read failed for '" + path + "'"));
        }
        if (n == 0) break; // unexpected EOF
        bytes_read += static_cast<size_t>(n);
    }
    content.resize(bytes_read);

    return Result<std::string>::ok(std::move(content));
}

// =============================================================================
// write_all — loop to handle partial writes
// =============================================================================

namespace {

Result<void> write_all(int fd, const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Result<void>::err(
                make_errno_error("write failed after " +
                                 std::to_string(written) + " of " +
                                 std::to_string(size) + " bytes"));
        }
        written += static_cast<size_t>(n);
    }
    return Result<void>::ok();
}

} // anonymous namespace

// =============================================================================
// atomic_write_file
// =============================================================================

Result<void> atomic_write_file(const std::string& path,
                               const std::vector<uint8_t>& data,
                               mode_t mode) {
    std::string partial_path = path + ".partial";

    // Step 1: Open .partial for writing
    ScopedFd fd(::open(partial_path.c_str(),
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    if (!fd.is_valid()) {
        return Result<void>::err(
            make_errno_error("open failed for '" + partial_path + "'"));
    }

    // Step 2: Write all data (loop for partial writes)
    auto write_result = write_all(fd.get(), data.data(), data.size());
    if (write_result.is_err()) {
        return write_result;
    }

    // Step 3: fsync the file
    if (::fsync(fd.get()) != 0) {
        return Result<void>::err(
            make_errno_error("fsync failed for '" + partial_path + "'"));
    }

    // Step 4: Close the file descriptor
    fd.close();

    // Step 5: Atomic rename
    if (::rename(partial_path.c_str(), path.c_str()) != 0) {
        return Result<void>::err(make_errno_error(
            "rename failed: '" + partial_path + "' -> '" + path + "'"));
    }

    // Step 6: fsync the parent directory to ensure rename is durable
    std::string parent = parent_dir(path);
    ScopedFd dir_fd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir_fd.is_valid()) {
        return Result<void>::err(
            make_errno_error("open parent dir failed for '" + parent + "'"));
    }
    if (::fsync(dir_fd.get()) != 0) {
        return Result<void>::err(
            make_errno_error("fsync parent dir failed for '" + parent + "'"));
    }
    // dir_fd closed automatically

    return Result<void>::ok();
}

// =============================================================================
// atomic_write_text
// =============================================================================

Result<void> atomic_write_text(const std::string& path,
                               const std::string& content,
                               mode_t mode) {
    std::vector<uint8_t> data(content.begin(), content.end());
    return atomic_write_file(path, data, mode);
}

// =============================================================================
// remove_file
// =============================================================================

Result<void> remove_file(const std::string& path) {
    // Remove the regular file (ignore ENOENT)
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return Result<void>::err(
            make_errno_error("unlink failed for '" + path + "'"));
    }

    // Also remove any stale .partial
    std::string partial_path = path + ".partial";
    if (::unlink(partial_path.c_str()) != 0 && errno != ENOENT) {
        // Not a hard error — the main file was removed successfully
    }

    return Result<void>::ok();
}

// =============================================================================
// ensure_directory
// =============================================================================

Result<void> ensure_directory(const std::string& path) {
    if (path.empty()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Invalid Path",
            "Cannot create directory: path is empty.",
            "ensure_directory called with empty path.",
            false, false));
    }

    // Try to create the directory directly
    if (::mkdir(path.c_str(), 0755) == 0) {
        return Result<void>::ok();
    }

    // If it already exists and is a directory, we are done
    if (errno == EEXIST) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return Result<void>::ok();
        }
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Path exists but is not a directory",
            "'" + path + "' already exists but is not a directory.",
            path + " exists with mode " + std::to_string(st.st_mode) +
                " (not a directory)",
            false, false));
    }

    // Parent directory missing — recurse
    if (errno == ENOENT) {
        std::string parent = parent_dir(path);

        // Do not recurse if parent is the same as path (root edge case)
        if (parent != path) {
            auto result = ensure_directory(parent);
            if (result.is_err()) {
                return result;
            }
        }

        // Retry mkdir now that the parent exists
        if (::mkdir(path.c_str(), 0755) == 0) {
            return Result<void>::ok();
        }
        if (errno == EEXIST) {
            struct stat st;
            if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                return Result<void>::ok();
            }
        }
        return Result<void>::err(
            make_errno_error("mkdir failed for '" + path + "'"));
    }

    // Other unexpected error
    return Result<void>::err(
        make_errno_error("mkdir failed for '" + path + "'"));
}

} // namespace installer
