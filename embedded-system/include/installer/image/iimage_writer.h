/**
 * @file iimage_writer.h
 * @brief Streaming image writer with on-the-fly verification.
 *
 * Writes a raw disk image stream to a block device. Supports a
 * producer-consumer pipeline where a decompressor feeds the writer,
 * which in turn feeds a verifier that recomputes the hash as data
 * flows through. All operations honour the cancellation token and
 * report progress through a callback.
 *
 * @see Architecture Doc §6.6
 */

#ifndef INSTALLER_IMAGE_IIMAGE_WRITER_H
#define INSTALLER_IMAGE_IIMAGE_WRITER_H

#include <cstdint>
#include <iosfwd>
#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Configuration options for an image write operation.
 */
struct WriteOptions {
    /** Expected uncompressed size of the image in bytes.
     *  Zero means skip the size-in-bytes precondition check. */
    uint64_t expected_size = 0;

    /** Expected SHA-256 hex digest of the uncompressed image data.
     *  An empty string disables the hash precondition check
     *  (but verify_after_write can still compute and compare). */
    std::string expected_sha256;

    /** When true, read back the written data after the write completes
     *  and compare its SHA-256 against expected_sha256. */
    bool verify_after_write = true;

    /** I/O buffer size used for read/write cycles.
     *  The default of 4 MiB balances throughput against memory usage
     *  for most eMMC / NVMe / SATA devices. */
    size_t buffer_size = 4 * 1024 * 1024;

    /** When true, open the target block device with O_DIRECT to bypass
     *  the kernel page cache. Recommended for images larger than RAM
     *  to avoid cache thrashing. */
    bool use_direct_io = true;
};

/**
 * Streaming image writer with built-in verification.
 *
 * Writes a sequential byte stream to a raw block device. The interface
 * is designed for a three-stage pipeline:
 *   1. Decompress (producer) — reads compressed data, decompresses.
 *   2. Write (consumer / producer) — writes blocks to the device.
 *   3. Verify (consumer) — reads back and recomputes SHA-256 on the fly.
 *
 * Thread-safety: write() and verify() must not be called concurrently
 * on the same instance.
 */
class IImageWriter {
public:
    virtual ~IImageWriter() = default;

    /**
     * Write a source stream to a target block device.
     *
     * Reads sequentially from @p source_stream and writes each block to
     * @p target_device. When WriteOptions::verify_after_write is true,
     * the implementation reads back the written ranges and compares
     * against the expected SHA-256 before returning.
     *
     * Required implementation behaviour:
     * - Open the device with O_DIRECT when use_direct_io is set.
     * - Invoke @p callback at regular intervals with current progress.
     * - Poll @p token before every I/O operation; return
     *   INTERNAL_CANCELLED if cancellation is requested.
     * - Issue fsync() (or BLKFLSBUF) before returning successfully.
     *
     * @param source_stream Input stream supplying raw (uncompressed)
     *                      image bytes.
     * @param target_device Absolute path to the destination block device
     *                      (e.g. /dev/mmcblk0).
     * @param options       Buffer sizes, expected hash/size, I/O flags.
     * @param callback      Invoked periodically with ProgressInfo.
     * @param token         Cancellation token polled throughout.
     * @return Result<void> — ok on successful write (and optional verify),
     *         IMAGE_WRITE_FAILED, IMAGE_VERIFY_FAILED, or
     *         INTERNAL_CANCELLED on error.
     */
    virtual Result<void> write(std::istream& source_stream,
                               const std::string& target_device,
                               const WriteOptions& options,
                               ProgressCallback callback,
                               CancellationToken& token) = 0;

    /**
     * Independently verify that the data on a block device matches an
     * expected hash and size.
     *
     * Reads @p expected_size bytes from the start of @p target_device,
     * recomputes the SHA-256 digest, and compares it against
     * @p expected_sha256. This is useful for post-reboot verification or
     * when verify_after_write was set to false during write().
     *
     * @param target_device   Absolute path to the block device.
     * @param expected_sha256 Expected SHA-256 hex digest.
     * @param expected_size   Number of bytes to read and hash
     *                        (0 = read until EOF).
     * @param callback        Progress callback.
     * @param token           Cancellation token.
     * @return Result<void> — ok if the computed hash matches,
     *         IMAGE_VERIFY_FAILED on mismatch.
     */
    virtual Result<void> verify(const std::string& target_device,
                                const std::string& expected_sha256,
                                uint64_t expected_size,
                                ProgressCallback callback,
                                CancellationToken& token) = 0;
};

} // namespace installer

#endif // INSTALLER_IMAGE_IIMAGE_WRITER_H
