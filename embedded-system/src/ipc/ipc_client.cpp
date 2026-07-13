/**
 * @file ipc_client.cpp
 * @brief Implementation of the JSON-RPC 2.0 Unix socket client.
 */

#include "ipc_client.h"
#include "installer/core/error_codes.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace installer {
namespace ipc {

// =========================================================================
//  Construction / Destruction
// =========================================================================

UnixSocketJsonRpcClient::UnixSocketJsonRpcClient(std::shared_ptr<ILogger> logger)
    : logger_(std::move(logger)) {}

UnixSocketJsonRpcClient::~UnixSocketJsonRpcClient() {
    disconnect();
}

// =========================================================================
//  Connect / Disconnect
// =========================================================================

Result<void> UnixSocketJsonRpcClient::connect(const std::string& socket_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (fd_ >= 0) {
        disconnect();
    }

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Socket Error",
                                 "Failed to create Unix domain socket",
                                 std::strerror(errno)));
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        std::string err_msg = std::strerror(errno);
        close(fd_);
        fd_ = -1;
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Connect Error",
                                 "Failed to connect to " + socket_path,
                                 err_msg));
    }

    connected_.store(true, std::memory_order_release);
    logger_->info("IPC client connected to " + socket_path);
    return Result<void>::ok();
}

void UnixSocketJsonRpcClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);

    connected_.store(false, std::memory_order_release);

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    logger_->info("IPC client disconnected");
}

bool UnixSocketJsonRpcClient::is_connected() {
    return connected_.load(std::memory_order_acquire);
}

// =========================================================================
//  Call
// =========================================================================

Result<nlohmann::json> UnixSocketJsonRpcClient::call(
    const std::string& method,
    const nlohmann::json& params,
    std::chrono::milliseconds timeout) {

    std::lock_guard<std::mutex> lock(mutex_);

    if (fd_ < 0 || !connected_.load(std::memory_order_acquire)) {
        return Result<nlohmann::json>::err(
            InstallerError::make(ErrorCode::INTERNAL_INVALID_STATE,
                                 "Not Connected",
                                 "IPC client is not connected"));
    }

    // Build request
    JsonRpcRequest req;
    req.method = method;
    req.params = params;
    req.id     = std::to_string(
        request_counter_.fetch_add(1, std::memory_order_relaxed));

    auto enc_result = encode_request(req);
    if (enc_result.is_err()) {
        return Result<nlohmann::json>::err(enc_result.take_error());
    }

    // Send
    auto send_result = send_message(enc_result.value());
    if (send_result.is_err()) {
        return Result<nlohmann::json>::err(send_result.take_error());
    }

    // Receive with deadline
    auto deadline = std::chrono::steady_clock::now() + timeout;

    auto recv_result = recv_message(deadline);
    if (recv_result.is_err()) {
        return Result<nlohmann::json>::err(recv_result.take_error());
    }

    // Decode response
    auto resp_result = decode_response(recv_result.value());
    if (resp_result.is_err()) {
        return Result<nlohmann::json>::err(resp_result.take_error());
    }

    auto& resp = resp_result.value();

    // Check for JSON-RPC error
    if (resp.is_error()) {
        int code = INVALID_REQUEST;
        std::string message = "Unknown error";
        if (resp.error.contains("code") && resp.error["code"].is_number()) {
            code = resp.error["code"].get<int>();
        }
        if (resp.error.contains("message") && resp.error["message"].is_string()) {
            message = resp.error["message"].get<std::string>();
        }
        return Result<nlohmann::json>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "RPC Error " + std::to_string(code),
                                 message));
    }

    return Result<nlohmann::json>::ok(std::move(resp.result));
}

// =========================================================================
//  Send
// =========================================================================

Result<void> UnixSocketJsonRpcClient::send_message(const std::string& json) {
    auto framed_result = frame_message(json);
    if (framed_result.is_err()) {
        return Result<void>::err(framed_result.take_error());
    }

    const std::string& data = framed_result.value();
    size_t total_sent = 0;

    while (total_sent < data.size()) {
        ssize_t n = write(fd_, data.data() + total_sent,
                          data.size() - total_sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Result<void>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Send Error",
                                     "Failed to send message",
                                     std::strerror(errno)));
        }
        total_sent += static_cast<size_t>(n);
    }

    return Result<void>::ok();
}

// =========================================================================
//  Receive
// =========================================================================

Result<std::string> UnixSocketJsonRpcClient::recv_message(
    std::chrono::steady_clock::time_point deadline) {

    std::string buffer;
    buffer.reserve(4096);

    // We use poll() with a timeout for non-blocking reads with deadline
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_TIMEOUT,
                                     "Timeout",
                                     "IPC call timed out waiting for response"));
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);

        struct pollfd pfd{};
        pfd.fd      = fd_;
        pfd.events  = POLLIN;
        int poll_ret = poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Poll Error",
                                     "poll() failed",
                                     std::strerror(errno)));
        }

        if (poll_ret == 0) {
            continue;  // timeout, loop around and check deadline again
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Connection Error",
                                     "Socket error or hangup"));
        }

        // Read available data
        char chunk[4096];
        ssize_t n = read(fd_, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Read Error",
                                     "Failed to read response",
                                     std::strerror(errno)));
        }
        if (n == 0) {
            // EOF — server closed connection
            connected_.store(false, std::memory_order_release);
            return Result<std::string>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Connection Closed",
                                     "Server closed connection"));
        }

        buffer.append(chunk, static_cast<size_t>(n));

        // Try to parse a complete message
        std::vector<std::string> messages;
        auto parse_result = parse_messages(buffer, messages);
        if (parse_result.is_err()) {
            return Result<std::string>::err(parse_result.take_error());
        }

        if (!messages.empty()) {
            // Return the first complete message
            return Result<std::string>::ok(std::move(messages[0]));
        }
    }
}

// =========================================================================
//  Read Exact (unused directly, recv_message uses polling approach instead)
// =========================================================================

Result<std::string> UnixSocketJsonRpcClient::read_exact(
    size_t /*count*/,
    std::chrono::steady_clock::time_point /*deadline*/) {
    // Not used in current implementation — recv_message handles framing.
    // Kept for interface completeness.
    return Result<std::string>::err(
        InstallerError::make(ErrorCode::INTERNAL_ERROR,
                             "Not Implemented",
                             "read_exact not used in this implementation"));
}

} // namespace ipc
} // namespace installer
