/**
 * @file security_manager.cpp
 * @brief SecurityManager implementation — libsodium, OpenSSL, and standalone
 *        backends for SHA-256, Ed25519, and AES-256-GCM.
 */

#include "security_manager.h"
#include "installer/security/isecurity_manager.h"
#include "installer/core/error_codes.h"

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <climits>

// =============================================================================
// Crypto backend detection
// =============================================================================

#if __has_include(<sodium.h>)
#define INSTALLER_HAVE_LIBSODIUM 1
#include <sodium.h>
#elif __has_include(<openssl/evp.h>)
#define INSTALLER_HAVE_OPENSSL 1
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#else
#define INSTALLER_STANDALONE_CRYPTO 1
#endif

// =============================================================================
// Embedded public key (Ed25519 test vector from RFC 8032 / libsodium tests)
// =============================================================================

static const uint8_t EMBEDDED_PUBLIC_KEY[32] = {
    0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a,
    0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
    0x9c, 0x97, 0x2c, 0xcb, 0xb8, 0x2e, 0xba, 0x7e,
    0x0d, 0x2f, 0x5f, 0x2f, 0x6a, 0x7b, 0x9e, 0x3b
};

namespace installer {

// =============================================================================
// Anonymous namespace — internal helpers
// =============================================================================
namespace {

// ---------------------------------------------------------------------------
// Error helper
// ---------------------------------------------------------------------------
static InstallerError make_error(const char* code, const std::string& msg) {
    return InstallerError::make(code, msg, msg, msg, false, false);
}

// ---------------------------------------------------------------------------
// Random bytes — selects best available source
// ---------------------------------------------------------------------------
static bool random_bytes(uint8_t* buf, size_t len) {
#if defined(INSTALLER_HAVE_LIBSODIUM)
    randombytes_buf(buf, len);
    return true;
#elif defined(INSTALLER_HAVE_OPENSSL)
    return RAND_bytes(buf, static_cast<int>(len)) == 1;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, buf + total, len - total);
        if (n <= 0) { close(fd); return false; }
        total += static_cast<size_t>(n);
    }
    close(fd);
    return true;
#endif
}

// =========================================================================
// Standalone SHA-256 (FIPS 180-4) — used when neither libsodium nor OpenSSL
// is available.  Complete and correct implementation with all 64 rounds.
// =========================================================================
#if defined(INSTALLER_STANDALONE_CRYPTO)

static inline uint32_t bswap32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(x);
#else
    return ((x >> 24) & 0x000000ffu) |
           ((x >>  8) & 0x0000ff00u) |
           ((x <<  8) & 0x00ff0000u) |
           ((x << 24) & 0xff000000u);
#endif
}

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline uint32_t Sigma0(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

static inline uint32_t Sigma1(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

static inline uint32_t sigma0(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

static inline uint32_t sigma1(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

// 64 round constants (first 32 bits of the fractional parts of the cube roots
// of the first 64 primes)
static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

struct sha256_ctx {
    uint32_t state[8];
    uint64_t count;       // total bytes processed
    uint8_t  buf[64];     // current partial block
};

static void sha256_init(sha256_ctx* ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->count = 0;
}

/// Load a big-endian uint32_t from bytes (host is little-endian).
static inline uint32_t load_be32(const uint8_t* b) {
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) <<  8) |
           (static_cast<uint32_t>(b[3]));
}

/// Store a uint32_t as big-endian bytes (host is little-endian).
static inline void store_be32(uint8_t* b, uint32_t v) {
    b[0] = static_cast<uint8_t>(v >> 24);
    b[1] = static_cast<uint8_t>(v >> 16);
    b[2] = static_cast<uint8_t>(v >>  8);
    b[3] = static_cast<uint8_t>(v);
}

/// SHA-256 compression function — transform one 64-byte block.
static void sha256_transform(sha256_ctx* ctx, const uint8_t* block) {
    uint32_t W[64];

    // Prepare message schedule: first 16 words from block
    for (int t = 0; t < 16; ++t) {
        W[t] = load_be32(block + t * 4);
    }
    // Extend to 64 words
    for (int t = 16; t < 64; ++t) {
        W[t] = sigma1(W[t - 2]) + W[t - 7] + sigma0(W[t - 15]) + W[t - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int t = 0; t < 64; ++t) {
        uint32_t T1 = h + Sigma1(e) + Ch(e, f, g) + K256[t] + W[t];
        uint32_t T2 = Sigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_update(sha256_ctx* ctx, const uint8_t* data, size_t len) {
    // Process any partial block from previous calls
    size_t buf_fill = static_cast<size_t>(ctx->count % 64);
    ctx->count += len;

    if (buf_fill > 0) {
        size_t copy = 64 - buf_fill;
        if (len < copy) copy = len;
        std::memcpy(ctx->buf + buf_fill, data, copy);
        data += copy;
        len  -= copy;
        if (buf_fill + copy < 64) return;  // buffer not yet full
        sha256_transform(ctx, ctx->buf);
    }

    // Process full 64-byte blocks directly
    while (len >= 64) {
        sha256_transform(ctx, data);
        data += 64;
        len  -= 64;
    }

    // Save leftover bytes
    if (len > 0) {
        std::memcpy(ctx->buf, data, len);
    }
}

static void sha256_final(sha256_ctx* ctx, uint8_t digest[32]) {
    uint64_t total_bits = ctx->count * 8;
    size_t buf_fill = static_cast<size_t>(ctx->count % 64);

    // Padding: append 0x80, then zeros, then 64-bit length in big-endian
    uint8_t pad[64];
    std::memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    if (buf_fill >= 56) {
        // Need an extra block
        std::memcpy(ctx->buf + buf_fill, pad, 64 - buf_fill);
        sha256_transform(ctx, ctx->buf);
        std::memset(ctx->buf, 0, 56);
    } else {
        std::memcpy(ctx->buf + buf_fill, pad, 56 - buf_fill);
    }

    // Append 64-bit length in big-endian (bytes 56–63 of last block)
    for (int i = 0; i < 8; ++i) {
        ctx->buf[56 + i] = static_cast<uint8_t>(total_bits >> (56 - i * 8));
    }
    sha256_transform(ctx, ctx->buf);

    // Write digest as big-endian words
    for (int i = 0; i < 8; ++i) {
        store_be32(digest + i * 4, ctx->state[i]);
    }
}

#endif // INSTALLER_STANDALONE_CRYPTO

// =========================================================================
// Unified SHA-256 incremental hashing
// =========================================================================

struct sha256_hasher {
#if defined(INSTALLER_HAVE_LIBSODIUM)
    crypto_hash_sha256_state state;
#elif defined(INSTALLER_HAVE_OPENSSL)
    SHA256_CTX ctx;
#else
    sha256_ctx ctx;
#endif

    void init() {
#if defined(INSTALLER_HAVE_LIBSODIUM)
        crypto_hash_sha256_init(&state);
#elif defined(INSTALLER_HAVE_OPENSSL)
        SHA256_Init(&ctx);
#else
        sha256_init(&ctx);
#endif
    }

    void update(const uint8_t* data, size_t len) {
#if defined(INSTALLER_HAVE_LIBSODIUM)
        crypto_hash_sha256_update(&state, data, len);
#elif defined(INSTALLER_HAVE_OPENSSL)
        SHA256_Update(&ctx, data, len);
#else
        sha256_update(&ctx, data, len);
#endif
    }

    void final(uint8_t digest[32]) {
#if defined(INSTALLER_HAVE_LIBSODIUM)
        crypto_hash_sha256_final(&state, digest);
#elif defined(INSTALLER_HAVE_OPENSSL)
        SHA256_Final(digest, &ctx);
#else
        sha256_final(&ctx, digest);
#endif
    }
};

// ---------------------------------------------------------------------------
// Compute SHA-256 over a range of bytes, return raw 32-byte digest
// ---------------------------------------------------------------------------
static void compute_sha256_raw(const uint8_t* data, size_t len, uint8_t digest[32]) {
    sha256_hasher h;
    h.init();
    h.update(data, len);
    h.final(digest);
}

// ---------------------------------------------------------------------------
// Hex conversion helpers (used internally and exposed via public API)
// ---------------------------------------------------------------------------
static std::string internal_bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

static Result<std::vector<uint8_t>> internal_hex_to_bytes(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_CONFIG_ERROR,
                       "Hex string has odd length"));
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        char* end = nullptr;
        long val = std::strtol(hex.substr(i, 2).c_str(), &end, 16);
        if (end != hex.substr(i, 2).c_str() + 2) {
            return Result<std::vector<uint8_t>>::err(
                make_error(ErrorCode::INTERNAL_CONFIG_ERROR,
                           "Hex string contains invalid characters at position " +
                               std::to_string(i)));
        }
        bytes.push_back(static_cast<uint8_t>(val));
    }
    return Result<std::vector<uint8_t>>::ok(std::move(bytes));
}

} // anonymous namespace

// =============================================================================
// SecurityManager — public API
// =============================================================================

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SecurityManager::SecurityManager()
    : embedded_public_key_(EMBEDDED_PUBLIC_KEY,
                           EMBEDDED_PUBLIC_KEY + 32) {}

SecurityManager::SecurityManager(const std::string& public_key_path)
    : public_key_path_(public_key_path)
    , embedded_public_key_(EMBEDDED_PUBLIC_KEY,
                           EMBEDDED_PUBLIC_KEY + 32) {}

SecurityManager::~SecurityManager() = default;

// ---------------------------------------------------------------------------
// sha256_file
// ---------------------------------------------------------------------------

Result<std::string> SecurityManager::sha256_file(const std::string& file_path) {
    return sha256_file_chunked(file_path, 65536);
}

Result<std::string> SecurityManager::sha256_file_chunked(const std::string& file_path,
                                                          size_t chunk_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return Result<std::string>::err(
            make_error(ErrorCode::DEVICE_IO_ERROR,
                       "Cannot open file for SHA-256: " + file_path));
    }

    // Ensure a reasonable minimum chunk size
    if (chunk_size < 64) chunk_size = 64;
    if (chunk_size > 16 * 1024 * 1024) chunk_size = 16 * 1024 * 1024;

    sha256_hasher hasher;
    hasher.init();

    std::vector<char> buffer(chunk_size);
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           file.gcount() > 0) {
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            hasher.update(reinterpret_cast<const uint8_t*>(buffer.data()),
                          static_cast<size_t>(bytes_read));
        }
        if (file.bad()) {
            return Result<std::string>::err(
                make_error(ErrorCode::DEVICE_IO_ERROR,
                           "I/O error while reading file for SHA-256: " + file_path));
        }
        if (file.eof()) break;
    }

    uint8_t digest[32];
    hasher.final(digest);

    std::vector<uint8_t> digest_vec(digest, digest + 32);
    return Result<std::string>::ok(internal_bytes_to_hex(digest_vec));
}

// ---------------------------------------------------------------------------
// sha256_stream
// ---------------------------------------------------------------------------

Result<std::string> SecurityManager::sha256_stream(std::istream& stream) {
    std::lock_guard<std::mutex> lock(mutex_);

    sha256_hasher hasher;
    hasher.init();

    std::vector<char> buffer(65536);
    while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           stream.gcount() > 0) {
        std::streamsize bytes_read = stream.gcount();
        if (bytes_read > 0) {
            hasher.update(reinterpret_cast<const uint8_t*>(buffer.data()),
                          static_cast<size_t>(bytes_read));
        }
        if (stream.bad()) {
            return Result<std::string>::err(
                make_error(ErrorCode::DEVICE_IO_ERROR,
                           "I/O error while reading stream for SHA-256"));
        }
        if (stream.eof()) break;
    }

    uint8_t digest[32];
    hasher.final(digest);

    std::vector<uint8_t> digest_vec(digest, digest + 32);
    return Result<std::string>::ok(internal_bytes_to_hex(digest_vec));
}

// ---------------------------------------------------------------------------
// sha256_data
// ---------------------------------------------------------------------------

std::string SecurityManager::sha256_data(const std::vector<uint8_t>& data) {
    uint8_t digest[32];
    compute_sha256_raw(data.data(), data.size(), digest);
    std::vector<uint8_t> digest_vec(digest, digest + 32);
    return internal_bytes_to_hex(digest_vec);
}

// ---------------------------------------------------------------------------
// verify_ed25519
// ---------------------------------------------------------------------------

Result<bool> SecurityManager::verify_ed25519(const std::vector<uint8_t>& message_digest,
                                              const std::vector<uint8_t>& signature,
                                              const std::vector<uint8_t>& public_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate input sizes
    if (public_key.size() != 32) {
        return Result<bool>::err(
            make_error(ErrorCode::PACKAGE_SIGNATURE_FAIL,
                       "Ed25519 public key must be 32 bytes, got " +
                           std::to_string(public_key.size())));
    }
    if (signature.size() != 64) {
        return Result<bool>::err(
            make_error(ErrorCode::PACKAGE_SIGNATURE_FAIL,
                       "Ed25519 signature must be 64 bytes, got " +
                           std::to_string(signature.size())));
    }

#if defined(INSTALLER_HAVE_LIBSODIUM)
    int ret = crypto_sign_verify_detached(
        signature.data(),
        message_digest.data(),
        message_digest.size(),
        public_key.data());
    if (ret != 0) {
        return Result<bool>::ok(false);
    }
    return Result<bool>::ok(true);

#elif defined(INSTALLER_HAVE_OPENSSL)
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        public_key.data(), public_key.size());
    if (!pkey) {
        return Result<bool>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to create Ed25519 public key from raw bytes"));
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return Result<bool>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to allocate EVP_MD_CTX for Ed25519 verification"));
    }

    int ret = EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey);
    if (ret != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return Result<bool>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_DigestVerifyInit failed for Ed25519"));
    }

    ret = EVP_DigestVerify(md_ctx, signature.data(), signature.size(),
                            message_digest.data(), message_digest.size());

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    if (ret == 1) {
        return Result<bool>::ok(true);
    } else if (ret == 0) {
        return Result<bool>::ok(false);
    } else {
        return Result<bool>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_DigestVerify error during Ed25519 verification"));
    }

#else
    return Result<bool>::err(
        make_error(ErrorCode::INTERNAL_ERROR,
                   "Ed25519 verification not available: no libsodium or OpenSSL "
                   "with Ed25519 support"));
#endif
}

// ---------------------------------------------------------------------------
// load_public_key
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> SecurityManager::load_public_key() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Try to read from file if a path is set
    if (!public_key_path_.empty()) {
        std::ifstream file(public_key_path_, std::ios::binary);
        if (file.is_open()) {
            // Read the file contents
            file.seekg(0, std::ios::end);
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> key(static_cast<size_t>(size));
            if (file.read(reinterpret_cast<char*>(key.data()), size)) {
                if (key.size() == 32) {
                    return Result<std::vector<uint8_t>>::ok(std::move(key));
                }
                // If it's hex-encoded (64 chars), try to decode
                if (key.size() == 64) {
                    std::string hex_str(key.begin(), key.end());
                    auto decoded = internal_hex_to_bytes(hex_str);
                    if (decoded.is_ok() && decoded.value().size() == 32) {
                        return decoded;
                    }
                }
            }

            // File was read but contents are invalid — log warning and fall
            // through to embedded key
            std::cerr << "[SecurityManager] Public key file " << public_key_path_
                      << " exists but does not contain a valid 32-byte key (size="
                      << size << "). Falling back to embedded key." << std::endl;
        }
    }

    // 2. Fall back to embedded key
    return Result<std::vector<uint8_t>>::ok(embedded_public_key_);
}

// ---------------------------------------------------------------------------
// encrypt_aes256_gcm
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> SecurityManager::encrypt_aes256_gcm(
    const std::vector<uint8_t>& plaintext,
    const std::vector<uint8_t>& key) {

    std::lock_guard<std::mutex> lock(mutex_);

    if (key.size() != 32) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_CONFIG_ERROR,
                       "AES-256-GCM key must be exactly 32 bytes, got " +
                           std::to_string(key.size())));
    }

#if defined(INSTALLER_HAVE_LIBSODIUM)
    // libsodium path
    if (crypto_aead_aes256gcm_is_available() == 0) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "AES-256-GCM is not available on this hardware "
                       "(requires AESNI or ARM Crypto Extensions)"));
    }

    // Generate 12-byte nonce
    uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    // Output: nonce(12) || ciphertext || tag(16)
    size_t ct_len = plaintext.size() + crypto_aead_aes256gcm_ABYTES;
    std::vector<uint8_t> result(crypto_aead_aes256gcm_NPUBBYTES + ct_len);

    // Copy nonce to front
    std::memcpy(result.data(), nonce, crypto_aead_aes256gcm_NPUBBYTES);

    unsigned long long actual_ct_len = 0;
    int ret = crypto_aead_aes256gcm_encrypt(
        result.data() + crypto_aead_aes256gcm_NPUBBYTES,  // ciphertext + tag
        &actual_ct_len,
        plaintext.data(), plaintext.size(),
        nullptr, 0,      // no additional data
        nullptr,          // nsec (unused)
        nonce,
        key.data());

    if (ret != 0) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "crypto_aead_aes256gcm_encrypt failed"));
    }

    // Trim to actual size (should match NPUBBYTES + ct_len)
    result.resize(crypto_aead_aes256gcm_NPUBBYTES + actual_ct_len);
    return Result<std::vector<uint8_t>>::ok(std::move(result));

#elif defined(INSTALLER_HAVE_OPENSSL)
    // OpenSSL path
    const size_t NONCE_LEN = 12;
    const size_t TAG_LEN   = 16;

    // Generate 12-byte nonce
    uint8_t nonce[NONCE_LEN];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to generate random nonce for AES-256-GCM"));
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to allocate EVP_CIPHER_CTX"));
    }

    int ret = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_EncryptInit_ex failed for AES-256-GCM"));
    }

    // Set IV (nonce) length
    ret = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, nullptr);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to set GCM IV length"));
    }

    // Set key and nonce
    ret = EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_EncryptInit_ex (key/IV) failed for AES-256-GCM"));
    }

    // Allocate output: nonce(12) || ciphertext(plaintext_len) || tag(16)
    std::vector<uint8_t> result(NONCE_LEN + plaintext.size() + TAG_LEN);
    std::memcpy(result.data(), nonce, NONCE_LEN);

    int out_len = 0;
    ret = EVP_EncryptUpdate(ctx,
                            result.data() + NONCE_LEN,
                            &out_len,
                            plaintext.data(),
                            static_cast<int>(plaintext.size()));
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_EncryptUpdate failed for AES-256-GCM"));
    }
    int ciphertext_len = out_len;

    ret = EVP_EncryptFinal_ex(ctx,
                               result.data() + NONCE_LEN + ciphertext_len,
                               &out_len);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_EncryptFinal_ex failed for AES-256-GCM"));
    }
    ciphertext_len += out_len;

    // Get the 16-byte tag
    ret = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN,
                              result.data() + NONCE_LEN + ciphertext_len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret != 1) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to get GCM authentication tag"));
    }

    // Resize to actual used size
    result.resize(NONCE_LEN + static_cast<size_t>(ciphertext_len) + TAG_LEN);
    return Result<std::vector<uint8_t>>::ok(std::move(result));

#else
    // Standalone: AES-256-GCM not available without a crypto library
    return Result<std::vector<uint8_t>>::err(
        make_error(ErrorCode::INTERNAL_ERROR,
                   "AES-256-GCM encryption not available: neither libsodium "
                   "nor OpenSSL is present"));
#endif
}

// ---------------------------------------------------------------------------
// decrypt_aes256_gcm
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> SecurityManager::decrypt_aes256_gcm(
    const std::vector<uint8_t>& ciphertext,
    const std::vector<uint8_t>& key) {

    std::lock_guard<std::mutex> lock(mutex_);

    const size_t NONCE_LEN = 12;
    const size_t TAG_LEN   = 16;
    const size_t MIN_LEN   = NONCE_LEN + TAG_LEN;  // must have at least nonce+tag

    if (key.size() != 32) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_CONFIG_ERROR,
                       "AES-256-GCM key must be exactly 32 bytes, got " +
                           std::to_string(key.size())));
    }

    if (ciphertext.size() < MIN_LEN) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::PACKAGE_CORRUPTED,
                       "AES-256-GCM ciphertext too short: expected at least " +
                           std::to_string(MIN_LEN) + " bytes, got " +
                           std::to_string(ciphertext.size())));
    }

    // Extract parts
    const uint8_t* nonce_ptr = ciphertext.data();
    const size_t   enc_len   = ciphertext.size() - NONCE_LEN - TAG_LEN;
    const uint8_t* enc_ptr   = ciphertext.data() + NONCE_LEN;
    const uint8_t* tag_ptr   = ciphertext.data() + NONCE_LEN + enc_len;

#if defined(INSTALLER_HAVE_LIBSODIUM)
    // libsodium path
    if (crypto_aead_aes256gcm_is_available() == 0) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "AES-256-GCM is not available on this hardware"));
    }

    if (enc_len + crypto_aead_aes256gcm_ABYTES !=
        ciphertext.size() - crypto_aead_aes256gcm_NPUBBYTES) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::PACKAGE_CORRUPTED,
                       "AES-256-GCM ciphertext size mismatch"));
    }

    std::vector<uint8_t> combined(enc_len + crypto_aead_aes256gcm_ABYTES);
    std::memcpy(combined.data(), enc_ptr, enc_len);
    std::memcpy(combined.data() + enc_len, tag_ptr, crypto_aead_aes256gcm_ABYTES);

    std::vector<uint8_t> plaintext(enc_len);
    unsigned long long actual_pt_len = 0;

    int ret = crypto_aead_aes256gcm_decrypt(
        plaintext.data(), &actual_pt_len,
        nullptr,                         // nsec (unused)
        combined.data(), combined.size(),
        nullptr, 0,                      // no additional data
        nonce_ptr,
        key.data());

    if (ret != 0) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::PACKAGE_SIGNATURE_FAIL,
                       "AES-256-GCM decryption/authentication failed: "
                       "ciphertext may be corrupted or key is incorrect"));
    }

    plaintext.resize(actual_pt_len);
    return Result<std::vector<uint8_t>>::ok(std::move(plaintext));

#elif defined(INSTALLER_HAVE_OPENSSL)
    // OpenSSL path
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to allocate EVP_CIPHER_CTX for decryption"));
    }

    int ret = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_DecryptInit_ex failed for AES-256-GCM"));
    }

    // Set IV length
    ret = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                              static_cast<int>(NONCE_LEN), nullptr);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to set GCM IV length for decryption"));
    }

    // Set key and nonce
    ret = EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce_ptr);
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_DecryptInit_ex (key/IV) failed for AES-256-GCM"));
    }

    // Set expected tag BEFORE decryption
    ret = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                              static_cast<int>(TAG_LEN),
                              const_cast<uint8_t*>(tag_ptr));
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "Failed to set GCM tag for decryption"));
    }

    std::vector<uint8_t> plaintext(enc_len);
    int out_len = 0;

    ret = EVP_DecryptUpdate(ctx, plaintext.data(), &out_len,
                            enc_ptr, static_cast<int>(enc_len));
    if (ret != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::INTERNAL_ERROR,
                       "EVP_DecryptUpdate failed for AES-256-GCM"));
    }
    int pt_len = out_len;

    // EVP_DecryptFinal_ex performs GCM tag verification
    ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + pt_len, &out_len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret != 1) {
        return Result<std::vector<uint8_t>>::err(
            make_error(ErrorCode::PACKAGE_SIGNATURE_FAIL,
                       "AES-256-GCM decryption/authentication failed: "
                       "tag mismatch — ciphertext may be corrupted or key is "
                       "incorrect"));
    }
    pt_len += out_len;
    plaintext.resize(static_cast<size_t>(pt_len));

    return Result<std::vector<uint8_t>>::ok(std::move(plaintext));

#else
    // Standalone: AES-256-GCM not available
    return Result<std::vector<uint8_t>>::err(
        make_error(ErrorCode::INTERNAL_ERROR,
                   "AES-256-GCM decryption not available: neither libsodium "
                   "nor OpenSSL is present"));
#endif
}

// ---------------------------------------------------------------------------
// bytes_to_hex (static)
// ---------------------------------------------------------------------------

std::string SecurityManager::bytes_to_hex(const std::vector<uint8_t>& bytes) {
    return internal_bytes_to_hex(bytes);
}

// ---------------------------------------------------------------------------
// hex_to_bytes (static)
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> SecurityManager::hex_to_bytes(const std::string& hex) {
    return internal_hex_to_bytes(hex);
}

} // namespace installer
