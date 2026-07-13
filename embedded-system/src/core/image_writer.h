/**
 * @file image_writer.h
 * @brief High-concurrency image writer with producer-consumer pipeline.
 *
 * Implements IImageWriter using a multi-threaded pipeline:
 *   Reader -> BoundedQueue -> Writer (O_DIRECT) -> flush -> Verifier (read-back)
 *
 * The pipeline streams data from an istream to a block device with
 * O_DIRECT for zero-copy I/O, then reads back from the device to
 * verify the SHA-256 digest matches the expected value.
 */

#ifndef INSTALLER_CORE_IMAGE_WRITER_H
#define INSTALLER_CORE_IMAGE_WRITER_H

#include "installer/image/iimage_writer.h"
#include "installer/core/types.h"
#include "installer/core/result.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace installer {

/**
 * Thread-safe bounded queue for the producer-consumer pipeline.
 *
 * Supports blocking push/pop with back-pressure. When the queue is
 * closed, blocked threads are woken and operations return immediately.
 *
 * @tparam T  The type of items stored in the queue. Must be movable.
 */
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity);
    ~BoundedQueue() = default;

    // Non-copyable
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    // Movable
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;

    /**
     * Blocking push — waits if the queue is full.
     * @return false if the queue was closed before the item could be pushed.
     */
    bool push(T item);

    /**
     * Blocking pop — waits if the queue is empty.
     * @return false if the queue is closed and empty.
     */
    bool pop(T& item);

    /**
     * Non-blocking pop.
     * @return false if the queue is empty (even if still open).
     */
    bool try_pop(T& item);

    /** Close the queue — wakes all waiting threads. */
    void close();

    /** True once close() has been called. */
    bool is_closed() const;

    size_t size() const;
    bool empty() const;

private:
    std::queue<T> queue_;
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    bool closed_ = false;
};

/**
 * A chunk of data in the pipeline.
 *
 * Memory is managed by std::vector. The writer thread copies data
 * into a posix_memalign'd buffer before issuing O_DIRECT writes.
 */
struct DataChunk {
    std::vector<uint8_t> data;  ///< The chunk payload
    size_t chunk_index = 0;     ///< Sequential index (0-based)
    bool is_last = false;       ///< True for the final chunk
};

/**
 * ImageWriter — high-concurrency image writer with read-back verification.
 *
 * Pipeline architecture:
 *   1. Reader thread reads chunks from the source istream, pushes to queue.
 *   2. Writer thread pops from queue, writes to the block device via O_DIRECT.
 *   3. Verifier thread waits for the writer to flush, then reads back from the
 *      device and compares the SHA-256 digest.
 *
 * All three threads run concurrently to saturate I/O bandwidth.
 */
class ImageWriter : public IImageWriter {
public:
    ImageWriter();
    ~ImageWriter() override;

    Result<void> write(const std::string& device_path,
                       std::istream& source,
                       const std::string& expected_sha256,
                       const CancellationToken& cancel = CancellationToken(),
                       ProgressCallback progress = nullptr) override;

    Result<bool> verify(const std::string& device_path,
                        const std::string& expected_sha256,
                        uint64_t size_bytes,
                        const CancellationToken& cancel = CancellationToken(),
                        ProgressCallback progress = nullptr) override;

    /**
     * Set the I/O chunk size.
     * The value is rounded up to the nearest 512-byte boundary to satisfy
     * O_DIRECT block-alignment requirements.
     */
    void set_chunk_size(size_t bytes) override;

private:
    // Forward declaration of the SHA-256 context (defined in the .cpp file).
    struct SHA256Context;

    /** Open a block device with O_WRONLY | O_DIRECT | O_SYNC. */
    Result<int> open_device_direct(const std::string& device_path);

    /** Flush device buffers: fsync() + BLKFLSBUF ioctl. */
    Result<void> flush_device(int fd);

    /** Finalize a SHA-256 context and return the hex-encoded digest string. */
    static std::string sha256_hex_string(SHA256Context& ctx);

    size_t chunk_size_ = 4 * 1024 * 1024; // 4 MiB default
};

// =========================================================================
// BoundedQueue<T> — Template Method Implementations
// =========================================================================

template <typename T>
BoundedQueue<T>::BoundedQueue(size_t capacity)
    : capacity_(capacity)
{
}

template <typename T>
bool BoundedQueue<T>::push(T item)
{
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this]() {
        return queue_.size() < capacity_ || closed_;
    });
    if (closed_) {
        return false;
    }
    queue_.push(std::move(item));
    not_empty_.notify_one();
    return true;
}

template <typename T>
bool BoundedQueue<T>::pop(T& item)
{
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this]() {
        return !queue_.empty() || closed_;
    });
    if (queue_.empty() && closed_) {
        return false;
    }
    item = std::move(queue_.front());
    queue_.pop();
    not_full_.notify_one();
    return true;
}

template <typename T>
bool BoundedQueue<T>::try_pop(T& item)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    item = std::move(queue_.front());
    queue_.pop();
    not_full_.notify_one();
    return true;
}

template <typename T>
void BoundedQueue<T>::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
}

template <typename T>
bool BoundedQueue<T>::is_closed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

template <typename T>
size_t BoundedQueue<T>::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

template <typename T>
bool BoundedQueue<T>::empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

} // namespace installer

#endif // INSTALLER_CORE_IMAGE_WRITER_H
