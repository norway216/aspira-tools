/**
 * @file package_manager.cpp
 * @brief PackageManager implementation — .espkg reading, validation, and extraction.
 *
 * The .espkg format is a tar archive (optionally zstd-compressed) containing
 * a manifest.json metadata file, an Ed25519 signature, and binary payloads.
 *
 * JSON parsing is handled by an embedded recursive-descent parser in the
 * anonymous namespace below — no external JSON library is required.
 */

#include "src/core/package_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <ftw.h>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace installer {

namespace {

// Compatibility wrapper: converts old-style (args, timeout, token) calls
// to the new ProcessArgs-based IProcessRunner interface.
Result<ProcessResult> run_process(IProcessRunner* runner,
                                   const std::vector<std::string>& args,
                                   std::chrono::milliseconds timeout,
                                   CancellationToken* token = nullptr) {
    ProcessArgs pa;
    pa.program = args[0];
    pa.args.assign(args.begin() + 1, args.end());
    pa.timeout = timeout;
    CancellationToken dummy;
    return runner->run(pa, token ? *token : dummy);
}

} // anonymous namespace

/* =========================================================================
 *  Embedded JSON Parser (anonymous namespace)
 * =========================================================================
 *
 *  A minimal recursive-descent parser sufficient for reading manifest.json.
 *  Handles objects, arrays, strings (with basic escapes), integers,
 *  booleans, and null.  Throws std::runtime_error on parse failures.
 * ========================================================================= */

namespace {

enum class JSONType {
    Null,
    Boolean,
    Number,
    String,
    Object,
    Array
};

struct JSONValue {
    JSONType type = JSONType::Null;

    bool   bool_val   = false;
    int64_t number_val = 0;
    std::string string_val;
    std::vector<JSONValue> array_val;
    std::map<std::string, JSONValue> object_val;
};

class JSONParser {
public:
    explicit JSONParser(const std::string& input)
        : input_(input), pos_(0) {}

    /// Parse the entire input and return the root JSON value.
    JSONValue parse() {
        skip_whitespace();
        if (pos_ >= input_.size()) {
            throw std::runtime_error("JSONParser: empty input");
        }
        JSONValue val = parse_value();
        skip_whitespace();
        if (pos_ < input_.size()) {
            throw std::runtime_error("JSONParser: trailing characters after root value");
        }
        return val;
    }

private:
    const std::string& input_;
    size_t pos_;

    // ---- cursor helpers ----

    void skip_whitespace() {
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const {
        if (pos_ >= input_.size()) return '\0';
        return input_[pos_];
    }

    char advance() {
        if (pos_ >= input_.size()) return '\0';
        return input_[pos_++];
    }

    void expect(char expected) {
        skip_whitespace();
        char c = peek();
        if (c != expected) {
            std::string msg = "JSONParser: expected '";
            msg += expected;
            msg += "' but got '";
            if (c != '\0') msg += c;
            msg += "' at position " + std::to_string(pos_);
            throw std::runtime_error(msg);
        }
        advance();
    }

    // ---- recursive descent ----

    JSONValue parse_value() {
        skip_whitespace();
        char c = peek();

        switch (c) {
        case '{':  return parse_object();
        case '[':  return parse_array();
        case '"':  return parse_string();
        case 't':  // fall through
        case 'f':  return parse_literal();
        case 'n':  return parse_literal();
        case '-':  return parse_number();
        default:
            if (c >= '0' && c <= '9') return parse_number();
            break;
        }

        std::string msg = "JSONParser: unexpected character '";
        if (c != '\0') msg += c;
        msg += "' at position " + std::to_string(pos_);
        throw std::runtime_error(msg);
    }

    JSONValue parse_object() {
        JSONValue val;
        val.type = JSONType::Object;
        expect('{');

        skip_whitespace();
        if (peek() == '}') {
            advance();  // empty object
            return val;
        }

        for (;;) {
            skip_whitespace();
            // key must be a string
            JSONValue key_val = parse_string();
            std::string key = key_val.string_val;
            expect(':');
            val.object_val[key] = parse_value();

            skip_whitespace();
            if (peek() == '}') {
                advance();
                break;
            }
            expect(',');
        }

        return val;
    }

    JSONValue parse_array() {
        JSONValue val;
        val.type = JSONType::Array;
        expect('[');

        skip_whitespace();
        if (peek() == ']') {
            advance();  // empty array
            return val;
        }

        for (;;) {
            val.array_val.push_back(parse_value());

            skip_whitespace();
            if (peek() == ']') {
                advance();
                break;
            }
            expect(',');
        }

        return val;
    }

    JSONValue parse_string() {
        JSONValue val;
        val.type = JSONType::String;
        expect('"');

        std::string result;
        result.reserve(64);

        while (pos_ < input_.size()) {
            char c = advance();
            if (c == '"') {
                val.string_val = std::move(result);
                return val;
            }
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    throw std::runtime_error("JSONParser: unexpected end of input in string escape");
                }
                char esc = advance();
                switch (esc) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                case 'f':  result += '\f'; break;
                case 'b':  result += '\b'; break;
                // \uXXXX not implemented — manifest.json does not use unicode escapes
                default:
                    // Preserve the backslash + escape char verbatim for robustness
                    result += '\\';
                    result += esc;
                    break;
                }
            } else {
                result += c;
            }
        }

        throw std::runtime_error("JSONParser: unterminated string");
    }

    JSONValue parse_number() {
        JSONValue val;
        val.type = JSONType::Number;

        bool negative = false;
        if (peek() == '-') {
            negative = true;
            advance();
        }

        // Require at least one digit
        if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
            throw std::runtime_error("JSONParser: expected digit in number at position " +
                                     std::to_string(pos_));
        }

        int64_t num = 0;
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c < '0' || c > '9') break;
            int64_t digit = static_cast<int64_t>(c - '0');
            if (num > INT64_MAX / 10) {
                throw std::runtime_error("JSONParser: integer overflow in number at position " +
                                         std::to_string(pos_));
            }
            if (num == INT64_MAX / 10 && digit > INT64_MAX % 10) {
                throw std::runtime_error("JSONParser: integer overflow in number at position " +
                                         std::to_string(pos_));
            }
            num = num * 10 + digit;
            ++pos_;
        }

        // We do not support fractional numbers or exponents — the manifest uses
        // only integers.  If we encounter a '.' or 'e'/'E' we throw.
        if (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == '.' || c == 'e' || c == 'E') {
                throw std::runtime_error("JSONParser: floating-point numbers not supported");
            }
        }

        val.number_val = negative ? -num : num;
        return val;
    }

    JSONValue parse_literal() {
        JSONValue val;

        if (input_.compare(pos_, 4, "true") == 0) {
            val.type = JSONType::Boolean;
            val.bool_val = true;
            pos_ += 4;
        } else if (input_.compare(pos_, 5, "false") == 0) {
            val.type = JSONType::Boolean;
            val.bool_val = false;
            pos_ += 5;
        } else if (input_.compare(pos_, 4, "null") == 0) {
            val.type = JSONType::Null;
            pos_ += 4;
        } else {
            throw std::runtime_error("JSONParser: unexpected literal at position " +
                                     std::to_string(pos_));
        }

        return val;
    }
};

} // anonymous namespace


/* =========================================================================
 *  Directory Helpers
 * ========================================================================= */

namespace {

/// Validate that a file path from the manifest is safe (no path traversal).
static bool is_safe_path(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/') return false;
    if (path.find("..") != std::string::npos) return false;
    for (char c : path) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '-' && c != '_' && c != '.' && c != '/') {
            return false;
        }
        if (c == '\\') return false;
    }
    return true;
}

/// nftw(3) callback — removes a single filesystem entry.
int nftw_remove_callback(const char* fpath,
                         const struct stat* /*sb*/,
                         int typeflag,
                         struct FTW* /*ftwbuf*/) {
    int ret = 0;
    if (typeflag == FTW_DP) {
        ret = rmdir(fpath);
    } else {
        ret = unlink(fpath);
    }
    if (ret != 0) {
        // Best-effort — log would go here in a full build
        std::fprintf(stderr, "PackageManager: warning: could not remove %s: %s\n",
                     fpath, std::strerror(errno));
    }
    return ret;
}

} // anonymous namespace


/* =========================================================================
 *  PackageManager — Construction / Destruction
 * ========================================================================= */

PackageManager::PackageManager(IProcessRunner* proc_runner,
                               ISecurityManager* sec_mgr)
    : proc_runner_(proc_runner)
    , sec_mgr_(sec_mgr)
{
}

PackageManager::~PackageManager() {
    if (is_open_) {
        // Best-effort cleanup; do not throw from destructor.
        if (!temp_dir_.empty()) {
            remove_directory(temp_dir_);
        }
    }
}


/* =========================================================================
 *  PackageManager — open / close
 * ========================================================================= */

Result<void> PackageManager::open(const std::string& package_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (is_open_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Already Open",
            "A package is already open. Close it before opening another.",
            "open() called while is_open_ == true"));
    }

    // ---- Validate the file exists and is a regular file ----
    struct stat st;
    if (::stat(package_path.c_str(), &st) != 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_INVALID_FORMAT,
            "Package Not Found",
            "The package file does not exist or is not accessible: " + package_path,
            std::string("stat() failed: ") + std::strerror(errno)));
    }

    if (!S_ISREG(st.st_mode)) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_INVALID_FORMAT,
            "Invalid Package",
            "The path is not a regular file: " + package_path,
            "S_ISREG returned false"));
    }

    // ---- Create a temporary working directory ----
    std::string tmpl = "/tmp/installer_pkg_XXXXXX";
    char* dir = ::mkdtemp(&tmpl[0]);
    if (dir == nullptr) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Temporary Directory Failed",
            "Could not create a temporary working directory.",
            std::string("mkdtemp() failed: ") + std::strerror(errno)));
    }
    temp_dir_ = dir;

    // ---- Quick sanity: verify tar can list the archive ----
    {
        std::vector<std::string> args = {"tar", "--list", "-f", package_path};
        auto result = run_process(proc_runner_, args,
                                   std::chrono::seconds(30),
                                   nullptr);
        if (!result.is_ok()) {
            remove_directory(temp_dir_);
            temp_dir_.clear();
            return Result<void>::err(result.take_error());
        }

        const auto& pr = result.value();
        if (pr.exit_code != 0) {
            remove_directory(temp_dir_);
            temp_dir_.clear();
            return Result<void>::err(InstallerError::make(
                ErrorCode::PACKAGE_INVALID_FORMAT,
                "Invalid Package Format",
                "The file is not a valid tar archive or is corrupted.",
                "tar --list exit code " + std::to_string(pr.exit_code) +
                    ": " + pr.stderr_data));
        }
    }

    package_path_ = package_path;
    is_open_ = true;
    manifest_loaded_ = false;

    return Result<void>::ok();
}

void PackageManager::close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!temp_dir_.empty()) {
        remove_directory(temp_dir_);
        temp_dir_.clear();
    }

    package_path_.clear();
    manifest_ = Manifest{};
    manifest_loaded_ = false;
    is_open_ = false;
}


/* =========================================================================
 *  PackageManager — manifest loading
 * ========================================================================= */

Result<Manifest> PackageManager::load_manifest() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!is_open_) {
        return Result<Manifest>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Package Not Open",
            "Cannot load manifest: no package is open.",
            "load_manifest() called before open()"));
    }

    // ---- Extract manifest.json from the archive ----
    auto extract_result = extract_file(package_path_, "manifest.json");
    if (!extract_result.is_ok()) {
        return Result<Manifest>::err(extract_result.take_error());
    }

    std::string manifest_path = extract_result.value();

    // ---- Parse the JSON ----
    auto parse_result = parse_manifest_json(manifest_path);

    // ---- Clean up the temporary manifest file ----
    ::unlink(manifest_path.c_str());

    if (!parse_result.is_ok()) {
        return parse_result;
    }

    manifest_ = parse_result.value();
    manifest_loaded_ = true;

    return parse_result;
}


/* =========================================================================
 *  PackageManager — IPackageManager: verify_payload_hash
 * ========================================================================= */

Result<void> PackageManager::verify_payload_hash(const std::string& payload_name) {
    if (sec_mgr_ == nullptr) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Security Manager Missing",
            "Cannot verify hash: no ISecurityManager was provided.",
            "sec_mgr_ is nullptr"));
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!is_open_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Package Not Open",
            "Cannot verify payload: no package is open."));
    }

    // Ensure manifest is loaded
    if (!manifest_loaded_) {
        auto load_result = load_manifest();
        if (!load_result.is_ok()) {
            return Result<void>::err(load_result.take_error());
        }
    }

    // Find the payload entry
    const PayloadEntry* entry = nullptr;
    for (const auto& p : manifest_.payloads) {
        if (p.name == payload_name) {
            entry = &p;
            break;
        }
    }
    if (entry == nullptr) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Payload Not Found",
            "Payload '" + payload_name + "' not found in manifest."));
    }

    // Validate the payload file path to prevent path traversal attacks
    if (!is_safe_path(entry->file)) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Path Traversal Detected",
            "The payload file path '" + entry->file + "' contains unsafe characters or patterns.",
            "Payload file path failed is_safe_path() validation"));
    }

    // Extract the payload file
    auto extract_result = extract_file(package_path_, entry->file);
    if (!extract_result.is_ok()) {
        return Result<void>::err(extract_result.take_error());
    }

    std::string extracted_path = extract_result.value();

    // Read file and verify SHA-256 hash
    {
        std::ifstream file(extracted_path, std::ios::binary);
        if (!file.is_open()) {
            ::unlink(extracted_path.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Read Error",
                "Could not open extracted payload for hash verification."));
        }
        std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
        std::string computed = sec_mgr_->compute_sha256(file_data);
        if (computed != entry->sha256) {
            ::unlink(extracted_path.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::PACKAGE_HASH_MISMATCH,
                "Hash Mismatch",
                "Payload '" + payload_name + "' hash verification failed.",
                "expected=" + entry->sha256 + " actual=" + computed));
        }
    }

    // Clean up
    ::unlink(extracted_path.c_str());

    return Result<void>::ok();
}


/* =========================================================================
 *  PackageManager — IPackageManager: extract_payload
 * ========================================================================= */

Result<void> PackageManager::extract_payload(const std::string& payload_name,
                                             const std::string& target_path,
                                             ProgressCallback progress,
                                             CancellationToken& cancel) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!is_open_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Package Not Open",
            "Cannot extract payload: no package is open."));
    }

    // Ensure manifest is loaded
    if (!manifest_loaded_) {
        auto load_result = load_manifest();
        if (!load_result.is_ok()) {
            return Result<void>::err(load_result.take_error());
        }
    }

    // Find the payload entry
    const PayloadEntry* entry = nullptr;
    for (const auto& p : manifest_.payloads) {
        if (p.name == payload_name) {
            entry = &p;
            break;
        }
    }
    if (entry == nullptr) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Payload Not Found",
            "Payload '" + payload_name + "' not found in manifest."));
    }

    // Validate the payload file path to prevent path traversal attacks
    if (!is_safe_path(entry->file)) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Path Traversal Detected",
            "The payload file path '" + entry->file + "' contains unsafe characters or patterns.",
            "Payload file path failed is_safe_path() validation"));
    }

    // ---- Phase 1: extract the file from the archive into temp_dir_ ----
    if (progress) {
        ProgressInfo info;
        info.percent = 0;
        info.step_description = "Extracting " + payload_name + " from package";
        info.current_file = entry->file;
        info.bytes_total = entry->size;
        progress(info);
    }

    // Determine whether target_path is a directory
    bool target_is_dir = false;
    struct stat target_st;
    if (::stat(target_path.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode)) {
        target_is_dir = true;
    } else if (!target_path.empty() && target_path.back() == '/') {
        target_is_dir = true;
    }

    // Extract to temp_dir_ first, then copy to final destination
    // Try zstd-compressed first, fall back to plain tar
    std::string temp_extracted;
    {
        std::vector<std::string> args = {
            "tar", "--zstd", "-xf", package_path_,
            "-C", temp_dir_,
            entry->file
        };
        auto run_result = run_process(proc_runner_, args,
                                       std::chrono::minutes(10),
                                       &cancel);

        if (!run_result.is_ok() || run_result.value().exit_code != 0) {
            // Fallback: try without --zstd
            std::vector<std::string> args2 = {
                "tar", "-xf", package_path_,
                "-C", temp_dir_,
                entry->file
            };
            run_result = run_process(proc_runner_, args2,
                                      std::chrono::minutes(10),
                                      &cancel);
        }

        if (!run_result.is_ok()) {
            return Result<void>::err(run_result.take_error());
        }

        if (run_result.value().cancelled) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED,
                "Cancelled",
                "Payload extraction was cancelled.",
                "", false, false));
        }

        if (run_result.value().exit_code != 0) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::IMAGE_DECOMPRESS_FAIL,
                "Extraction Failed",
                "Failed to extract payload '" + payload_name + "' from package.",
                "tar exit code " + std::to_string(run_result.value().exit_code) +
                    ": " + run_result.value().stderr_data));
        }

        // The extracted file should be at temp_dir_/<entry->file>
        temp_extracted = temp_dir_ + "/" + entry->file;
    }

    // Verify temp file exists
    {
        struct stat st2;
        if (::stat(temp_extracted.c_str(), &st2) != 0 || !S_ISREG(st2.st_mode)) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::IMAGE_DECOMPRESS_FAIL,
                "Extraction Failed",
                "Payload '" + payload_name + "' was not found after extraction.",
                "Expected file not created: " + temp_extracted));
        }
    }

    if (progress) {
        ProgressInfo info;
        info.percent = 50;
        info.step_description = "Copying " + payload_name + " to target";
        info.current_file = entry->file;
        info.bytes_total = entry->size;
        progress(info);
    }

    // ---- Phase 2: copy to target location ----
    std::string final_path;
    if (target_is_dir) {
        // Extract the base filename from the payload file path
        std::string base_name = entry->file;
        size_t slash_pos = base_name.rfind('/');
        if (slash_pos != std::string::npos) {
            base_name = base_name.substr(slash_pos + 1);
        }
        // Ensure trailing slash
        std::string dir = target_path;
        if (dir.back() != '/') dir += '/';
        final_path = dir + base_name;
    } else {
        final_path = target_path;
    }

    // Copy with progress
    {
        std::ifstream src(temp_extracted, std::ios::binary);
        if (!src.is_open()) {
            ::unlink(temp_extracted.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Copy Failed",
                "Could not open extracted payload for reading: " + temp_extracted));
        }

        std::ofstream dst(final_path, std::ios::binary | std::ios::trunc);
        if (!dst.is_open()) {
            ::unlink(temp_extracted.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::IMAGE_WRITE_FAILED,
                "Copy Failed",
                "Could not open target file for writing: " + final_path));
        }

        // Get source file size for progress reporting
        src.seekg(0, std::ios::end);
        uint64_t src_size = static_cast<uint64_t>(src.tellg());
        src.seekg(0, std::ios::beg);

        std::vector<char> buffer(65536);
        uint64_t copied = 0;

        while (src.good() && !cancel.is_cancelled()) {
            src.read(buffer.data(), buffer.size());
            std::streamsize bytes_read = src.gcount();
            if (bytes_read <= 0) break;

            dst.write(buffer.data(), bytes_read);
            copied += static_cast<uint64_t>(bytes_read);

            if (progress && src_size > 0) {
                ProgressInfo info;
                info.percent = static_cast<int>(50 + (copied * 50 / src_size));
                info.step_description = "Copying " + payload_name;
                info.current_file = entry->file;
                info.bytes_processed = copied;
                info.bytes_total = src_size;
                progress(info);
            }
        }

        if (cancel.is_cancelled()) {
            ::unlink(temp_extracted.c_str());
            ::unlink(final_path.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED,
                "Cancelled",
                "Payload extraction was cancelled during copy."));
        }

        if (src.bad()) {
            ::unlink(temp_extracted.c_str());
            dst.close();
            ::unlink(final_path.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::IMAGE_WRITE_FAILED,
                "Read Error",
                "Error reading extracted payload data."));
        }

        if (!dst.good()) {
            ::unlink(temp_extracted.c_str());
            dst.close();
            ::unlink(final_path.c_str());
            return Result<void>::err(InstallerError::make(
                ErrorCode::IMAGE_WRITE_FAILED,
                "Write Error",
                "Error writing payload to target: " + final_path));
        }

        dst.close();
        src.close();
    }

    // Clean up temp file
    ::unlink(temp_extracted.c_str());

    if (progress) {
        ProgressInfo info;
        info.percent = 100;
        info.step_description = "Payload " + payload_name + " extracted successfully";
        info.current_file = entry->file;
        info.bytes_total = entry->size;
        info.bytes_processed = entry->size;
        progress(info);
    }

    return Result<void>::ok();
}


/* =========================================================================
 *  PackageManager — IPackageManager: get_payload_size
 * ========================================================================= */

Result<uint64_t> PackageManager::get_payload_size(const std::string& payload_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!manifest_loaded_) {
        return Result<uint64_t>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Manifest Not Loaded",
            "Manifest must be loaded before querying payload sizes.",
            "get_payload_size() called before load_manifest()"));
    }

    for (const auto& p : manifest_.payloads) {
        if (p.name == payload_name) {
            return Result<uint64_t>::ok(p.size);
        }
    }

    return Result<uint64_t>::err(InstallerError::make(
        ErrorCode::PACKAGE_MANIFEST_ERROR,
        "Payload Not Found",
        "Payload '" + payload_name + "' not found in manifest."));
}


/* =========================================================================
 *  PackageManager — Extended: verify_package
 * ========================================================================= */

Result<bool> PackageManager::verify_package(const CancellationToken& cancel) {
    if (sec_mgr_ == nullptr) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Security Manager Missing",
            "Cannot verify package: no ISecurityManager was provided.",
            "sec_mgr_ is nullptr"));
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!is_open_) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Package Not Open",
            "Cannot verify package: no package is open."));
    }

    // Ensure manifest is loaded
    if (!manifest_loaded_) {
        auto load_result = load_manifest();
        if (!load_result.is_ok()) {
            return Result<bool>::err(load_result.take_error());
        }
    }

    if (manifest_.payloads.empty()) {
        // No payloads to verify — technically valid
        return Result<bool>::ok(true);
    }

    // Verify each payload one at a time to limit temp space usage
    for (size_t i = 0; i < manifest_.payloads.size(); ++i) {
        // Check cancellation between payloads
        if (cancel.is_cancelled()) {
            return Result<bool>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED,
                "Cancelled",
                "Package verification was cancelled.",
                "", false, false));
        }

        const auto& payload = manifest_.payloads[i];

        // Validate the payload file path to prevent path traversal attacks
        if (!is_safe_path(payload.file)) {
            return Result<bool>::err(InstallerError::make(
                ErrorCode::PACKAGE_MANIFEST_ERROR,
                "Path Traversal Detected",
                "The payload file path '" + payload.file + "' contains unsafe characters or patterns.",
                "Payload file path failed is_safe_path() validation"));
        }

        // Extract this payload to temp
        auto extract_result = extract_file(package_path_, payload.file);
        if (!extract_result.is_ok()) {
            // Build detailed error before the Result is consumed
            InstallerError err = InstallerError::make(
                ErrorCode::PACKAGE_CORRUPTED,
                "Extraction Failed",
                "Failed to extract payload '" + payload.name +
                    "' (" + payload.file + ") for verification.",
                extract_result.error().technical_message);
            // We don't consume extract_result since we're returning a different error
            return Result<bool>::err(std::move(err));
        }

        std::string extracted_path = extract_result.value();

        // Read file and verify SHA-256 hash
        bool hash_ok = false;
        {
            std::ifstream file(extracted_path, std::ios::binary);
            if (file.is_open()) {
                std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
                std::string computed = sec_mgr_->compute_sha256(file_data);
                hash_ok = (computed == payload.sha256);
            }
        }

        // Clean up immediately
        ::unlink(extracted_path.c_str());

        if (!hash_ok) {
            return Result<bool>::ok(false);  // Hash mismatch — not an error, just invalid
        }
    }

    return Result<bool>::ok(true);
}


/* =========================================================================
 *  PackageManager — Extended: verify_signature
 * ========================================================================= */

Result<bool> PackageManager::verify_signature() {
    if (sec_mgr_ == nullptr) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Security Manager Missing",
            "Cannot verify signature: no ISecurityManager was provided.",
            "sec_mgr_ is nullptr"));
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!is_open_) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Package Not Open",
            "Cannot verify signature: no package is open."));
    }

    // ---- Extract manifest.sig ----
    auto sig_result = extract_file(package_path_, "manifest.sig");
    if (!sig_result.is_ok()) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::PACKAGE_SIGNATURE_FAIL,
            "No Signature",
            "The package does not contain a manifest.sig signature file.",
            sig_result.error().technical_message));
    }

    std::string sig_path = sig_result.value();

    // Read the signature file (should be 64 bytes for Ed25519)
    std::string signature_bytes;
    {
        std::ifstream sig_file(sig_path, std::ios::binary);
        if (!sig_file.is_open()) {
            ::unlink(sig_path.c_str());
            return Result<bool>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Read Error",
                "Could not open extracted manifest.sig for reading."));
        }

        // Read entire file content
        std::ostringstream oss;
        oss << sig_file.rdbuf();
        signature_bytes = oss.str();
        sig_file.close();
    }
    ::unlink(sig_path.c_str());

    if (signature_bytes.empty()) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::PACKAGE_SIGNATURE_FAIL,
            "Empty Signature",
            "The manifest.sig file is empty."));
    }

    // ---- Extract manifest.json for its raw content ----
    auto json_result = extract_file(package_path_, "manifest.json");
    if (!json_result.is_ok()) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "No manifest.json",
            "Cannot verify signature: manifest.json not found in package.",
            json_result.error().technical_message));
    }

    std::string json_path = json_result.value();

    // Read the entire manifest.json as a raw string
    std::string manifest_content;
    {
        std::ifstream json_file(json_path, std::ios::binary);
        if (!json_file.is_open()) {
            ::unlink(json_path.c_str());
            return Result<bool>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Read Error",
                "Could not open extracted manifest.json for reading."));
        }

        std::ostringstream oss;
        oss << json_file.rdbuf();
        manifest_content = oss.str();
        json_file.close();
    }
    ::unlink(json_path.c_str());

    if (manifest_content.empty()) {
        return Result<bool>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Empty Manifest",
            "The manifest.json file is empty."));
    }

    // ---- Verify signature ----
    // Pass empty public_key string; the SecurityManager uses its embedded key.
    std::vector<uint8_t> manifest_bytes(manifest_content.begin(), manifest_content.end());
    std::vector<uint8_t> sig_bytes(signature_bytes.begin(), signature_bytes.end());
    auto verify_result = sec_mgr_->verify_signature(manifest_bytes, sig_bytes, "");

    if (!verify_result.is_ok()) {
        // Signature verification itself returned an error
        return Result<bool>::err(InstallerError::make(
            ErrorCode::PACKAGE_SIGNATURE_FAIL,
            "Signature Verification Error",
            "An error occurred during signature verification.",
            verify_result.error().technical_message));
    }

    return Result<bool>::ok(verify_result.value());
}


/* =========================================================================
 *  PackageManager — Extended: open_payload
 * ========================================================================= */

Result<std::unique_ptr<std::istream>> PackageManager::open_payload(
        const std::string& payload_name) {

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!is_open_) {
        return Result<std::unique_ptr<std::istream>>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Package Not Open",
            "Cannot open payload: no package is open."));
    }

    // Ensure manifest is loaded
    if (!manifest_loaded_) {
        auto load_result = load_manifest();
        if (!load_result.is_ok()) {
            return Result<std::unique_ptr<std::istream>>::err(load_result.take_error());
        }
    }

    // Resolve logical name to file name in archive
    auto resolve_result = resolve_payload_file(payload_name);
    if (!resolve_result.is_ok()) {
        return Result<std::unique_ptr<std::istream>>::err(resolve_result.take_error());
    }

    std::string file_name = resolve_result.value();

    // Validate the payload file path to prevent path traversal attacks
    if (!is_safe_path(file_name)) {
        return Result<std::unique_ptr<std::istream>>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Path Traversal Detected",
            "The payload file path '" + file_name + "' contains unsafe characters or patterns.",
            "Payload file path failed is_safe_path() validation"));
    }

    // Extract the payload file to temp_dir_
    auto extract_result = extract_file(package_path_, file_name);
    if (!extract_result.is_ok()) {
        return Result<std::unique_ptr<std::istream>>::err(extract_result.take_error());
    }

    std::string extracted_path = extract_result.value();

    // Open as ifstream.  The unique_ptr will use a custom deleter that
    // closes the stream and removes the temporary file.
    auto file_stream = std::unique_ptr<std::istream>(new std::ifstream(extracted_path, std::ios::binary));

    if (!file_stream->good()) {
        return Result<std::unique_ptr<std::istream>>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Stream Error",
            "Could not open extracted payload as a stream: " + extracted_path));
    }

    return Result<std::unique_ptr<std::istream>>::ok(std::move(file_stream));
}


/* =========================================================================
 *  PackageManager — Extended: check_compatibility
 * ========================================================================= */

Result<bool> PackageManager::check_compatibility(const Manifest& manifest,
                                                  const std::string& hw_profile) {
    // ---- Check architecture via uname ----
    {
        std::vector<std::string> args = {"uname", "-m"};
        auto run_result = run_process(proc_runner_, args,
                                       std::chrono::seconds(5),
                                       nullptr);
        if (!run_result.is_ok()) {
            return Result<bool>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "System Query Failed",
                "Could not determine current system architecture.",
                "uname -m failed: " + run_result.error().technical_message));
        }

        std::string current_arch = run_result.value().stdout_data;
        // Trim trailing newline
        while (!current_arch.empty() &&
               (current_arch.back() == '\n' || current_arch.back() == '\r')) {
            current_arch.pop_back();
        }

        if (current_arch.empty()) {
            return Result<bool>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "System Query Failed",
                "uname -m returned empty output."));
        }

        if (current_arch != manifest.architecture) {
            return Result<bool>::ok(false);
        }
    }

    // ---- Check min_disk_size_bytes is positive and reasonable ----
    if (manifest.min_disk_size_bytes == 0) {
        return Result<bool>::ok(false);
    }

    // ---- Check hardware profile ----
    if (!hw_profile.empty()) {
        bool profile_found = false;
        for (const auto& profile : manifest.hardware_profiles) {
            if (profile == hw_profile) {
                profile_found = true;
                break;
            }
        }
        if (!profile_found) {
            return Result<bool>::ok(false);
        }
    }

    return Result<bool>::ok(true);
}


/* =========================================================================
 *  PackageManager — Private Helpers
 * ========================================================================= */

Result<std::string> PackageManager::extract_file(const std::string& archive_path,
                                                  const std::string& file_name) {
    // Caller must hold mutex_

    // Validate the file name to prevent path traversal attacks
    if (!is_safe_path(file_name)) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::PACKAGE_CORRUPTED,
            "Path Traversal Detected",
            "The file path '" + file_name + "' contains unsafe characters or patterns.",
            "Path failed is_safe_path() validation"));
    }

    // Try zstd-compressed extraction first, since .espkg uses tar+zstd
    {
        std::vector<std::string> args = {
            "tar", "--zstd", "-xf", archive_path,
            "-C", temp_dir_,
            file_name
        };
        auto result = run_process(proc_runner_, args,
                                   std::chrono::seconds(30),
                                   nullptr);

        if (result.is_ok() && result.value().exit_code == 0) {
            std::string extracted_path = temp_dir_ + "/" + file_name;
            struct stat st;
            if (::stat(extracted_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                return Result<std::string>::ok(extracted_path);
            }
        }
    }

    // Fallback: try without explicit --zstd (tar auto-detection or plain tar)
    {
        std::vector<std::string> args = {
            "tar", "-xf", archive_path,
            "-C", temp_dir_,
            file_name
        };
        auto result = run_process(proc_runner_, args,
                                   std::chrono::seconds(30),
                                   nullptr);

        if (!result.is_ok()) {
            return Result<std::string>::err(result.take_error());
        }

        if (result.value().exit_code != 0) {
            return Result<std::string>::err(InstallerError::make(
                ErrorCode::PACKAGE_CORRUPTED,
                "Extraction Failed",
                "Could not extract '" + file_name + "' from package.",
                "tar exit code " + std::to_string(result.value().exit_code) +
                    ": " + result.value().stderr_data));
        }
    }

    std::string extracted_path = temp_dir_ + "/" + file_name;
    struct stat st;
    if (::stat(extracted_path.c_str(), &st) != 0) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::PACKAGE_CORRUPTED,
            "File Not Found",
            "'" + file_name + "' was not found in the package after extraction.",
            "stat() failed for " + extracted_path + ": " + std::strerror(errno)));
    }

    return Result<std::string>::ok(extracted_path);
}


Result<Manifest> PackageManager::parse_manifest_json(const std::string& json_path) {
    // Read the entire file into a string
    std::string json_text;
    {
        std::ifstream file(json_path, std::ios::binary);
        if (!file.is_open()) {
            return Result<Manifest>::err(InstallerError::make(
                ErrorCode::PACKAGE_MANIFEST_ERROR,
                "Manifest Read Error",
                "Could not open manifest file: " + json_path));
        }

        std::ostringstream oss;
        oss << file.rdbuf();
        json_text = oss.str();
    }

    if (json_text.empty()) {
        return Result<Manifest>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Empty Manifest",
            "The manifest file is empty."));
    }

    // ---- Parse JSON ----
    JSONParser parser(json_text);
    JSONValue root;
    try {
        root = parser.parse();
    } catch (const std::runtime_error& e) {
        return Result<Manifest>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Manifest Parse Error",
            "Failed to parse manifest.json. The file may be malformed.",
            e.what()));
    }

    if (root.type != JSONType::Object) {
        return Result<Manifest>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Manifest Format Error",
            "Manifest root is not a JSON object."));
    }

    // ---- Extract fields ----
    Manifest manifest;

    const auto& obj = root.object_val;

    // format_version
    auto it_fv = obj.find("format_version");
    if (it_fv != obj.end() && it_fv->second.type == JSONType::Number) {
        manifest.format_version = static_cast<int>(it_fv->second.number_val);
    }

    // package_id
    auto it_pid = obj.find("package_id");
    if (it_pid != obj.end() && it_pid->second.type == JSONType::String) {
        manifest.package_id = it_pid->second.string_val;
    }

    // product
    auto it_prod = obj.find("product");
    if (it_prod != obj.end() && it_prod->second.type == JSONType::String) {
        manifest.product = it_prod->second.string_val;
    }

    // version
    auto it_ver = obj.find("version");
    if (it_ver != obj.end() && it_ver->second.type == JSONType::String) {
        manifest.version = it_ver->second.string_val;
    }

    // build_id
    auto it_bid = obj.find("build_id");
    if (it_bid != obj.end() && it_bid->second.type == JSONType::String) {
        manifest.build_id = it_bid->second.string_val;
    }

    // architecture
    auto it_arch = obj.find("architecture");
    if (it_arch != obj.end() && it_arch->second.type == JSONType::String) {
        manifest.architecture = it_arch->second.string_val;
    }

    // hardware_profiles (string array)
    auto it_hw = obj.find("hardware_profiles");
    if (it_hw != obj.end() && it_hw->second.type == JSONType::Array) {
        for (const auto& elem : it_hw->second.array_val) {
            if (elem.type == JSONType::String) {
                manifest.hardware_profiles.push_back(elem.string_val);
            }
        }
    }

    // min_installer_version
    auto it_miv = obj.find("min_installer_version");
    if (it_miv != obj.end() && it_miv->second.type == JSONType::String) {
        manifest.min_installer_version = it_miv->second.string_val;
    }

    // min_disk_size_bytes
    auto it_mds = obj.find("min_disk_size_bytes");
    if (it_mds != obj.end() && it_mds->second.type == JSONType::Number) {
        if (it_mds->second.number_val < 0) {
            return Result<Manifest>::err(InstallerError::make(
                ErrorCode::PACKAGE_MANIFEST_ERROR,
                "Manifest Format Error",
                "min_disk_size_bytes must be non-negative."));
        }
        manifest.min_disk_size_bytes = static_cast<uint64_t>(it_mds->second.number_val);
    }

    // allow_downgrade
    auto it_ad = obj.find("allow_downgrade");
    if (it_ad != obj.end() && it_ad->second.type == JSONType::Boolean) {
        manifest.allow_downgrade = it_ad->second.bool_val;
    }

    // payloads (array of objects)
    auto it_pl = obj.find("payloads");
    if (it_pl != obj.end() && it_pl->second.type == JSONType::Array) {
        for (const auto& pl_val : it_pl->second.array_val) {
            if (pl_val.type != JSONType::Object) continue;

            PayloadEntry entry;
            const auto& pl_obj = pl_val.object_val;

            auto p_name = pl_obj.find("name");
            if (p_name != pl_obj.end() && p_name->second.type == JSONType::String) {
                entry.name = p_name->second.string_val;
            }

            auto p_file = pl_obj.find("file");
            if (p_file != pl_obj.end() && p_file->second.type == JSONType::String) {
                entry.file = p_file->second.string_val;
            }

            auto p_target = pl_obj.find("target");
            if (p_target != pl_obj.end() && p_target->second.type == JSONType::String) {
                entry.target = p_target->second.string_val;
            }

            auto p_type = pl_obj.find("type");
            if (p_type != pl_obj.end() && p_type->second.type == JSONType::String) {
                entry.type = p_type->second.string_val;
            }

            auto p_size = pl_obj.find("size");
            if (p_size != pl_obj.end() && p_size->second.type == JSONType::Number) {
                if (p_size->second.number_val < 0) {
                    return Result<Manifest>::err(InstallerError::make(
                        ErrorCode::PACKAGE_MANIFEST_ERROR,
                        "Manifest Format Error",
                        "Payload 'size' field must be non-negative."));
                }
                entry.size = static_cast<uint64_t>(p_size->second.number_val);
            }

            auto p_usize = pl_obj.find("uncompressed_size");
            if (p_usize != pl_obj.end() && p_usize->second.type == JSONType::Number) {
                if (p_usize->second.number_val < 0) {
                    return Result<Manifest>::err(InstallerError::make(
                        ErrorCode::PACKAGE_MANIFEST_ERROR,
                        "Manifest Format Error",
                        "Payload 'uncompressed_size' field must be non-negative."));
                }
                entry.uncompressed_size = static_cast<uint64_t>(p_usize->second.number_val);
            }

            auto p_sha = pl_obj.find("sha256");
            if (p_sha != pl_obj.end() && p_sha->second.type == JSONType::String) {
                entry.sha256 = p_sha->second.string_val;
            }

            // Only add if at minimum the name is present
            if (!entry.name.empty()) {
                manifest.payloads.push_back(std::move(entry));
            }
        }
    }

    return Result<Manifest>::ok(manifest);
}


Result<std::string> PackageManager::resolve_payload_file(
        const std::string& payload_name) {

    for (const auto& p : manifest_.payloads) {
        if (p.name == payload_name) {
            if (p.file.empty()) {
                return Result<std::string>::err(InstallerError::make(
                    ErrorCode::PACKAGE_MANIFEST_ERROR,
                    "Manifest Error",
                    "Payload '" + payload_name + "' has no file field in manifest."));
            }
            return Result<std::string>::ok(p.file);
        }
    }

    return Result<std::string>::err(InstallerError::make(
        ErrorCode::PACKAGE_MANIFEST_ERROR,
        "Payload Not Found",
        "Payload '" + payload_name + "' not found in manifest."));
}


bool PackageManager::remove_directory(const std::string& path) {
    // nftw walks the tree bottom-up with FTW_DEPTH, so files are removed
    // before their parent directories.
    int ret = ::nftw(path.c_str(),
                     nftw_remove_callback,
                     64,                  // max open file descriptors
                     FTW_DEPTH | FTW_PHYS);
    return ret == 0;
}

} // namespace installer
