/**
 * @file image_writer.cpp
 * @brief Implementation of the high-concurrency ImageWriter pipeline.
 *
 * Pipeline:  Reader -> BoundedQueue -> Writer (O_DIRECT) -> Verifier (read-back)
 *
 * Includes a self-contained SHA-256 implementation (FIPS 180-4) so the
 * module has zero external crypto dependencies.
 */

#include "src/core/image_writer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <linux/fs.h>
#include <optional>
#include <sstream>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

// BLKFLSBUF may not be defined in all kernel header versions.
#ifndef BLKFLSBUF
#define BLKFLSBUF _IO(0x12, 97)
#endif

// =========================================================================
// ScopedThread — RAII wrapper that joins on destruction.
//
// Prevents std::terminate if a thread is destroyed while joinable (e.g.
// when creating the 2nd or 3rd thread throws after previous threads have
// already been started).
// =========================================================================
class ScopedThread {
public:
    ScopedThread() = default;
    template<typename F>
    explicit ScopedThread(F&& f) : t_(std::forward<F>(f)) {}
    ~ScopedThread() { if (t_.joinable()) t_.join(); }
    ScopedThread(ScopedThread&&) = default;
    ScopedThread& operator=(ScopedThread&&) = default;
private:
    std::thread t_;
};

namespace installer {

// =========================================================================
// Anonymous namespace — Self-contained SHA-256 (FIPS 180-4)
// =========================================================================
namespace {

// This is the concrete definition of ImageWriter::SHA256Context.
struct SHA256Context {
    uint8_t  block[64];     // Current 64-byte message block
    uint32_t datalen;       // Bytes accumulated in `block`
    uint64_t bitlen;        // Total message length in bits (excl. current block)
    uint32_t state[8];      // Hash state (H0–H7)
};

// First 32 bits of the fractional parts of the cube roots of the first 64 primes.
static const uint32_t kSHA256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// ---- Logical functions ----
inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t sha_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t sha_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sha_ep0(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

inline uint32_t sha_ep1(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

inline uint32_t sha_sig0(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

inline uint32_t sha_sig1(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

/**
 * Big-endian byte read: interpret 4 bytes as a uint32_t.
 */
inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
           (static_cast<uint32_t>(p[3]));
}

/**
 * The SHA-256 compression function.
 *
 * Processes one 64-byte block through 64 rounds of the compression loop.
 */
void sha256_transform(SHA256Context* ctx, const uint8_t data[64]) {
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = read_be32(data + j);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = sha_sig1(m[i - 2]) + m[i - 7] + sha_sig0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + sha_ep1(e) + sha_ch(e, f, g) + kSHA256K[i] + m[i];
        uint32_t t2 = sha_ep0(a) + sha_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
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

void sha256_init(SHA256Context* ctx) {
    ctx->datalen = 0;
    ctx->bitlen  = 0;
    // Initial hash values (first 32 bits of the fractional parts of the
    // square roots of the first 8 primes 2..19).
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

void sha256_update(SHA256Context* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->block[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->block);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha256_final(SHA256Context* ctx, uint8_t hash[32]) {
    // Compute total message length in bits BEFORE padding.
    uint64_t total_bits = ctx->bitlen + (static_cast<uint64_t>(ctx->datalen) * 8);

    // Append the '1' bit (0x80 byte).
    uint32_t i = ctx->datalen;
    ctx->block[i++] = 0x80;

    // If there is no room for the 64-bit length, pad this block and start a new one.
    if (i > 56) {
        while (i < 64) {
            ctx->block[i++] = 0x00;
        }
        sha256_transform(ctx, ctx->block);
        i = 0;
    }
    // Pad with zeros up to byte 56.
    while (i < 56) {
        ctx->block[i++] = 0x00;
    }

    // Append the original message length as a 64-bit big-endian integer.
    ctx->block[56] = static_cast<uint8_t>(total_bits >> 56);
    ctx->block[57] = static_cast<uint8_t>(total_bits >> 48);
    ctx->block[58] = static_cast<uint8_t>(total_bits >> 40);
    ctx->block[59] = static_cast<uint8_t>(total_bits >> 32);
    ctx->block[60] = static_cast<uint8_t>(total_bits >> 24);
    ctx->block[61] = static_cast<uint8_t>(total_bits >> 16);
    ctx->block[62] = static_cast<uint8_t>(total_bits >>  8);
    ctx->block[63] = static_cast<uint8_t>(total_bits);

    // Final compression.
    sha256_transform(ctx, ctx->block);

    // Copy state to output (big-endian).
    for (i = 0; i < 4; ++i) {
        hash[i]      = static_cast<uint8_t>((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4]  = static_cast<uint8_t>((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8]  = static_cast<uint8_t>((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = static_cast<uint8_t>((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = static_cast<uint8_t>((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = static_cast<uint8_t>((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = static_cast<uint8_t>((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = static_cast<uint8_t>((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

} // anonymous namespace

// =========================================================================
// ImageWriter — Construction / Destruction
// =========================================================================

ImageWriter::ImageWriter() = default;
ImageWriter::~ImageWriter() = default;

// =========================================================================
// set_chunk_size
// =========================================================================

void ImageWriter::set_chunk_size(size_t bytes) {
    // Round up to 512-byte alignment for O_DIRECT compatibility.
    constexpr size_t kBlockAlign = 512;
    if (bytes % kBlockAlign != 0) {
        bytes = ((bytes + kBlockAlign - 1) / kBlockAlign) * kBlockAlign;
    }
    // Enforce a reasonable minimum.
    if (bytes < kBlockAlign) {
        bytes = kBlockAlign;
    }
    chunk_size_ = bytes;
}

// =========================================================================
// open_device_direct
// =========================================================================

Result<int> ImageWriter::open_device_direct(const std::string& target_device) {
    int fd = open(target_device.c_str(), O_WRONLY | O_DIRECT | O_SYNC);
    if (fd < 0) {
        return Result<int>::err(InstallerError::make(
            ErrorCode::DEVICE_IO_ERROR,
            "Device Open Error",
            "Failed to open block device for writing",
            "device=" + target_device + " errno=" + std::to_string(errno),
            true));  // retryable
    }
    return Result<int>::ok(fd);
}

// =========================================================================
// flush_device
// =========================================================================

Result<void> ImageWriter::flush_device(int fd) {
    if (fsync(fd) != 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_IO_ERROR,
            "Device Flush Error",
            "fsync failed on block device",
            "errno=" + std::to_string(errno)));
    }

    // Flush the kernel's buffer cache for the block device.
    if (ioctl(fd, BLKFLSBUF) != 0) {
        // BLKFLSBUF may fail on some devices (e.g. loopback) — this is
        // not fatal if fsync already succeeded, but we still report it.
        // Only fail hard if the error is something other than ENOTTY / EINVAL.
        if (errno != ENOTTY && errno != EINVAL) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::DEVICE_IO_ERROR,
                "Device Flush Error",
                "BLKFLSBUF ioctl failed on block device",
                "errno=" + std::to_string(errno)));
        }
    }

    return Result<void>::ok();
}

// =========================================================================
// sha256_hex_string
// =========================================================================

static std::string sha256_hex_string(SHA256Context& ctx) {
    uint8_t hash[32];
    sha256_final(&ctx, hash);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 32; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// =========================================================================
// write — High-concurrency producer-consumer pipeline
// =========================================================================

Result<void> ImageWriter::write(std::istream& source_stream,
                                 const std::string& target_device,
                                 const WriteOptions& options,
                                 ProgressCallback callback,
                                 CancellationToken& token) {
    // ---- Pipeline shared state ----

    // Capacity of 8 chunks allows reasonable pipelining (~32 MiB in flight
    // at the default 4 MiB chunk size) without consuming excessive memory.
    BoundedQueue<DataChunk> queue(8);

    // How many bytes were actually written to the device (unpadded count).
    std::atomic<uint64_t> total_bytes_written{0};

    // Error propagation: the first error wins.
    std::atomic<bool> pipeline_error{false};
    std::mutex error_mutex;
    std::optional<InstallerError> first_error;

    // Set the error state. Closes the queue to wake blocked threads.
    // Thread-safe: only the first call stores the error.
    auto set_error = [&](InstallerError err) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!first_error.has_value()) {
            first_error = std::move(err);
        }
        pipeline_error.store(true, std::memory_order_release);
        queue.close();
    };

    // Writer -> Verifier hand-off.
    std::mutex writer_done_mutex;
    std::condition_variable writer_done_cv;
    bool writer_done = false;

    // =====================================================================
    // Reader Thread — source istream -> BoundedQueue
    // =====================================================================
    ScopedThread reader_thread([&]() {
        size_t chunk_index = 0;

        while (!token.is_cancelled() && !pipeline_error.load(std::memory_order_acquire)) {
            DataChunk chunk;
            chunk.data.resize(options.buffer_size);

            source_stream.read(reinterpret_cast<char*>(chunk.data.data()),
                        static_cast<std::streamsize>(options.buffer_size));
            size_t bytes_read = static_cast<size_t>(source_stream.gcount());

            if (bytes_read == 0) {
                break;  // EOF or error
            }

            chunk.data.resize(bytes_read);
            chunk.chunk_index = chunk_index++;

            // Determine whether this is the final chunk.
            if (bytes_read < options.buffer_size) {
                // Partial read always means EOF.
                chunk.is_last = true;
            } else {
                // Full chunk — peek one more byte to detect trailing EOF.
                // We use get()+unget() instead of peek() because unget() is
                // guaranteed by the standard for a single character.
                char probe = 0;
                if (source_stream.get(probe)) {
                    source_stream.unget();   // Put the byte back — more data remains.
                } else {
                    chunk.is_last = true;  // EOF reached.
                }
                // Clear any eof/fail bits from the probe so the next read()
                // call on the final chunk (which will be empty) behaves
                // correctly.
                if (chunk.is_last) {
                    source_stream.clear();
                }
            }

            // Check cancellation / error before blocking on push.
            // If we proceed into a blocking push after cancellation has been
            // requested, we may never wake up unless the queue is closed.
            if (token.is_cancelled() || pipeline_error.load(std::memory_order_acquire)) {
                queue.close();
                break;
            }

            if (!queue.push(std::move(chunk))) {
                // Queue was closed (error or cancellation).
                break;
            }

            if (chunk.is_last) {
                break;
            }
        }

        // Always close the queue so the writer knows no more data is coming.
        queue.close();
    });

    // =====================================================================
    // Writer Thread — BoundedQueue -> block device (O_DIRECT)
    // =====================================================================
    ScopedThread writer_thread([&]() {
        // Open the block device with O_DIRECT | O_SYNC.
        auto fd_result = open_device_direct(target_device);
        if (!fd_result.is_ok()) {
            set_error(fd_result.take_error());
            queue.close();
            return;
        }
        int fd = fd_result.value();

        // Allocate a reusable aligned buffer for O_DIRECT writes.
        // O_DIRECT requires the user-space buffer to be aligned to the
        // logical block size. We use 4096-byte alignment (page size) which
        // satisfies the requirement for all common block sizes (512 and 4096).
        // The buffer is sized to hold one chunk plus up to 511 bytes of
        // zero-padding for the last chunk.
        constexpr size_t kBlockAlign = 512;
        size_t aligned_buf_size = ((options.buffer_size + kBlockAlign - 1) / kBlockAlign) * kBlockAlign;
        void* aligned_raw = nullptr;
        if (posix_memalign(&aligned_raw, 4096, aligned_buf_size) != 0) {
            close(fd);
            set_error(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Memory Allocation Failed",
                "posix_memalign failed for O_DIRECT write buffer"));
            return;
        }
        uint8_t* write_buf = static_cast<uint8_t*>(aligned_raw);

        auto write_start = std::chrono::steady_clock::now();
        uint64_t local_bytes_written = 0;

        DataChunk chunk;
        while (true) {
            // Check cancellation / error before blocking on pop.
            // If we proceed into a blocking pop after cancellation has been
            // requested, we may never wake up unless the queue is closed.
            if (token.is_cancelled() || pipeline_error.load(std::memory_order_acquire)) {
                queue.close();
                break;
            }
            if (!queue.pop(chunk)) {
                break;
            }
            // Double-check after receiving a chunk — cancellation may have
            // been requested while we were blocked in pop().
            if (token.is_cancelled() || pipeline_error.load(std::memory_order_acquire)) {
                break;
            }

            // --- Copy chunk data into the aligned buffer ---
            size_t data_size = chunk.data.size();
            std::memcpy(write_buf, chunk.data.data(), data_size);

            // Determine the actual write size. The last chunk may need
            // zero-padding to satisfy O_DIRECT's block-alignment requirement.
            size_t write_size = data_size;
            if (chunk.is_last && (write_size % kBlockAlign) != 0) {
                size_t padded_size =
                    ((write_size + kBlockAlign - 1) / kBlockAlign) * kBlockAlign;
                std::memset(write_buf + write_size, 0, padded_size - write_size);
                write_size = padded_size;
            }

            // --- Write at the correct offset using pwrite() ---
            // pwrite() does not update the file offset, which avoids
            // seeking issues with O_DIRECT.
            off_t offset = static_cast<off_t>(chunk.chunk_index) * static_cast<off_t>(options.buffer_size);

            size_t total_written = 0;
            while (total_written < write_size) {
                ssize_t n = pwrite(fd,
                                   write_buf + total_written,
                                   write_size - total_written,
                                   offset + static_cast<off_t>(total_written));
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    // If the error is EINVAL and this is the last (padded) chunk,
                    // the device may not support O_DIRECT. Fall back to
                    // a non-O_DIRECT write as a last resort.
                    if (errno == EINVAL && chunk.is_last) {
                        // Close the O_DIRECT fd and re-open without O_DIRECT.
                        close(fd);
                        fd = open(target_device.c_str(), O_WRONLY | O_SYNC);
                        if (fd < 0) {
                            free(write_buf);
                            set_error(InstallerError::make(
                                ErrorCode::IMAGE_WRITE_FAILED,
                                "Write Error",
                                "Failed to re-open device without O_DIRECT for final chunk",
                                "errno=" + std::to_string(errno)));
                            return;
                        }
                        // Retry the write without O_DIRECT.
                        continue;
                    }
                    free(write_buf);
                    close(fd);
                    set_error(InstallerError::make(
                        ErrorCode::IMAGE_WRITE_FAILED,
                        "Write Error",
                        "pwrite failed on block device",
                        "offset=" + std::to_string(offset) +
                        " size=" + std::to_string(write_size) +
                        " errno=" + std::to_string(errno),
                        true));  // retryable
                    return;
                }
                if (n == 0) {
                    // pwrite returned 0 — device full or end-of-device
                    free(write_buf);
                    close(fd);
                    set_error(InstallerError::make(
                        ErrorCode::IMAGE_WRITE_FAILED,
                        "Write Error",
                        "pwrite returned 0 — device may be full",
                        "offset=" + std::to_string(offset) +
                        " size=" + std::to_string(write_size)));
                    return;
                }
                total_written += static_cast<size_t>(n);
            }

            // Track actual (unpadded) bytes written for verification.
            local_bytes_written += data_size;
            total_bytes_written.store(local_bytes_written, std::memory_order_release);

            // --- Progress reporting ---
            if (callback) {
                auto now = std::chrono::steady_clock::now();
                double elapsed =
                    std::chrono::duration<double>(now - write_start).count();
                ProgressInfo info;
                info.bytes_processed = local_bytes_written;
                info.bytes_total = 0;  // Unknown until the reader finishes.
                info.percent = -1;     // Indeterminate.
                info.speed_bytes_per_sec =
                    (elapsed > 0.0) ? static_cast<double>(local_bytes_written) / elapsed : 0.0;
                info.step_description = "Writing image to device";
                callback(info);
            }

            if (chunk.is_last) {
                break;
            }
        }

        // --- Flush device buffers ---
        auto flush_result = flush_device(fd);
        free(write_buf);
        close(fd);

        if (!flush_result.is_ok()) {
            set_error(flush_result.take_error());
            return;
        }

        // --- Signal the verifier ---
        {
            std::lock_guard<std::mutex> lock(writer_done_mutex);
            writer_done = true;
        }
        writer_done_cv.notify_one();
    });

    // =====================================================================
    // Verifier Thread — read-back from device + SHA-256 comparison
    // =====================================================================
    ScopedThread verifier_thread([&]() {
        // Wait until the writer has completed (or the pipeline has failed /
        // been cancelled).
        {
            std::unique_lock<std::mutex> lock(writer_done_mutex);
            while (!writer_done &&
                   !token.is_cancelled() &&
                   !pipeline_error.load(std::memory_order_acquire)) {
                writer_done_cv.wait_for(lock, std::chrono::milliseconds(100));
            }
        }

        if (token.is_cancelled() || pipeline_error.load(std::memory_order_acquire)) {
            return;
        }

        uint64_t total = total_bytes_written.load(std::memory_order_acquire);
        if (total == 0) {
            // Empty image written — hash of empty data should match the
            // expected SHA-256 of empty input.
            SHA256Context ctx;
            sha256_init(&ctx);
            std::string actual = sha256_hex_string(ctx);
            if (actual != options.expected_sha256) {
                set_error(InstallerError::make(
                    ErrorCode::IMAGE_VERIFY_FAILED,
                    "Hash Mismatch",
                    "Written data hash does not match expected hash",
                    "expected=" + options.expected_sha256 + " actual=" + actual));
            }
            return;
        }

        // --- Open the device for read-back ---
        int fd = open(target_device.c_str(), O_RDONLY);
        if (fd < 0) {
            set_error(InstallerError::make(
                ErrorCode::DEVICE_IO_ERROR,
                "Device Open Error",
                "Failed to open device for read-back verification",
                "errno=" + std::to_string(errno)));
            return;
        }

        // --- Streaming SHA-256 over the on-device data ---
        SHA256Context sha_ctx;
        sha256_init(&sha_ctx);

        std::vector<uint8_t> read_buf(chunk_size_);
        uint64_t bytes_read = 0;
        auto verify_start = std::chrono::steady_clock::now();

        while (bytes_read < total && !token.is_cancelled()) {
            size_t to_read = static_cast<size_t>(
                std::min(static_cast<uint64_t>(options.buffer_size), total - bytes_read));

            ssize_t n = read(fd, read_buf.data(), to_read);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                set_error(InstallerError::make(
                    ErrorCode::DEVICE_IO_ERROR,
                    "Verify Read Error",
                    "Failed to read back data for verification",
                    "offset=" + std::to_string(bytes_read) +
                    " errno=" + std::to_string(errno)));
                return;
            }
            if (n == 0) {
                // Unexpected EOF — the device has fewer bytes than written.
                close(fd);
                set_error(InstallerError::make(
                    ErrorCode::IMAGE_VERIFY_FAILED,
                    "Verify Read Error",
                    "Unexpected EOF during read-back verification",
                    "expected=" + std::to_string(total) +
                    " read=" + std::to_string(bytes_read)));
                return;
            }

            sha256_update(&sha_ctx, read_buf.data(), static_cast<size_t>(n));
            bytes_read += static_cast<uint64_t>(n);

            // --- Progress reporting ---
            if (callback) {
                auto now = std::chrono::steady_clock::now();
                double elapsed =
                    std::chrono::duration<double>(now - verify_start).count();
                ProgressInfo info;
                info.bytes_processed = bytes_read;
                info.bytes_total = total;
                info.percent = static_cast<int>(bytes_read * 100 / total);
                info.speed_bytes_per_sec =
                    (elapsed > 0.0) ? static_cast<double>(bytes_read) / elapsed : 0.0;
                info.step_description = "Verifying written data";
                callback(info);
            }
        }

        close(fd);

        if (token.is_cancelled()) {
            return;
        }

        // --- Finalize and compare ---
        std::string actual_hash = sha256_hex_string(sha_ctx);
        if (actual_hash != options.expected_sha256) {
            set_error(InstallerError::make(
                ErrorCode::IMAGE_VERIFY_FAILED,
                "Hash Mismatch",
                "Written data hash does not match expected hash",
                "expected=" + options.expected_sha256 + " actual=" + actual_hash));
        }
    });

    // ---- Propagate errors ----
    if (first_error.has_value()) {
        return Result<void>::err(std::move(*first_error));
    }

    if (token.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED,
            "Operation Cancelled",
            "Image write was cancelled by user request"));
    }

    return Result<void>::ok();
}

// =========================================================================
// verify — Standalone device verification
// =========================================================================

Result<void> ImageWriter::verify(const std::string& target_device,
                                  const std::string& expected_sha256,
                                  uint64_t expected_size,
                                  ProgressCallback callback,
                                  CancellationToken& token) {
    int fd = open(target_device.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_IO_ERROR,
            "Device Open Error",
            "Failed to open device for verification",
            "device=" + target_device + " errno=" + std::to_string(errno)));
    }

    SHA256Context sha_ctx;
    sha256_init(&sha_ctx);

    std::vector<uint8_t> read_buf(chunk_size_);
    uint64_t bytes_read = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (bytes_read < expected_size && !token.is_cancelled()) {
        size_t to_read = static_cast<size_t>(
            std::min(static_cast<uint64_t>(chunk_size_), expected_size - bytes_read));

        ssize_t n = read(fd, read_buf.data(), to_read);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return Result<void>::err(InstallerError::make(
                ErrorCode::DEVICE_IO_ERROR,
                "Read Error",
                "Failed to read from device during verification",
                "errno=" + std::to_string(errno)));
        }
        if (n == 0) {
            // Unexpected EOF.
            break;
        }

        sha256_update(&sha_ctx, read_buf.data(), static_cast<size_t>(n));
        bytes_read += static_cast<uint64_t>(n);

        if (callback) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            ProgressInfo info;
            info.bytes_processed = bytes_read;
            info.bytes_total = expected_size;
            info.percent = (expected_size > 0)
                               ? static_cast<int>(bytes_read * 100 / expected_size)
                               : 100;
            info.speed_bytes_per_sec =
                (elapsed > 0.0) ? static_cast<double>(bytes_read) / elapsed : 0.0;
            info.step_description = "Verifying device data";
            callback(info);
        }
    }

    close(fd);

    if (token.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED,
            "Operation Cancelled",
            "Verification was cancelled by user request"));
    }

    // Compute the hash even if we read fewer bytes than expected — the
    // comparison will catch that mismatch.
    std::string actual_hash = sha256_hex_string(sha_ctx);

    // Both hex strings should be lower-case for a case-sensitive comparison.
    // The hex output from sha256_hex_string is always lower-case.
    if (actual_hash != expected_sha256) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::IMAGE_VERIFY_FAILED,
            "Hash Mismatch",
            "Device data hash does not match expected hash",
            "expected=" + expected_sha256 + " actual=" + actual_hash));
    }
    return Result<void>::ok();
}

} // namespace installer
