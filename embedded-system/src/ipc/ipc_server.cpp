/**
 * @file ipc_server.cpp
 * @brief Implementation of the Unix Domain Socket JSON-RPC 2.0 server.
 */

#include "ipc_server.h"
#include "installer/core/error_codes.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace installer {
namespace ipc {

// =========================================================================
//  Helper: set a fd to non-blocking mode
// =========================================================================

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// =========================================================================
//  Helper: generate a unique request id
// =========================================================================

static std::string generate_id() {
    // Simple: microsecond timestamp + counter would be better in production.
    // For now use clock-based id.
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::to_string(static_cast<int64_t>(ts.tv_sec) * 1000000 +
                          static_cast<int64_t>(ts.tv_nsec) / 1000);
}

// =========================================================================
//  Construction / Destruction
// =========================================================================

UnixSocketJsonRpcServer::UnixSocketJsonRpcServer(std::shared_ptr<ILogger> logger)
    : logger_(std::move(logger)) {}

UnixSocketJsonRpcServer::~UnixSocketJsonRpcServer() {
    stop();
}

// =========================================================================
//  Start / Stop
// =========================================================================

Result<void> UnixSocketJsonRpcServer::start(const std::string& socket_path) {
    if (running_.load(std::memory_order_acquire)) {
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_INVALID_STATE,
                                 "Server Error",
                                 "IPC server is already running"));
    }

    socket_path_ = socket_path;

    // Remove any stale socket file
    unlink(socket_path_.c_str());

    // Create Unix domain socket
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Socket Error",
                                 "Failed to create Unix domain socket",
                                 std::strerror(errno)));
    }

    if (!set_nonblocking(listen_fd_)) {
        close(listen_fd_);
        listen_fd_ = -1;
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Socket Error",
                                 "Failed to set socket non-blocking",
                                 std::strerror(errno)));
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Bind Error",
                                 "Failed to bind Unix domain socket: " + socket_path_,
                                 std::strerror(errno)));
    }

    // Set socket file permissions
    chmod(socket_path_.c_str(), 0660);

    // Listen
    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Listen Error",
                                 "Failed to listen on socket",
                                 std::strerror(errno)));
    }

    // Create epoll instance
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Epoll Error",
                                 "Failed to create epoll instance",
                                 std::strerror(errno)));
    }

    // Register listen fd with epoll (edge-triggered)
    struct epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
        return Result<void>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Epoll Error",
                                 "Failed to register listen fd with epoll",
                                 std::strerror(errno)));
    }

    // Start event loop thread
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&UnixSocketJsonRpcServer::event_loop, this);

    logger_->info("IPC server started on " + socket_path_);
    return Result<void>::ok();
}

void UnixSocketJsonRpcServer::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);

    // Wake up epoll by writing to a self-pipe or just join with timeout.
    // For simplicity, we close the listen fd to unblock accept().
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& kv : clients_) {
            if (kv.second->fd >= 0) {
                close(kv.second->fd);
            }
        }
        clients_.clear();
    }

    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    unlink(socket_path_.c_str());
    logger_->info("IPC server stopped");
}

bool UnixSocketJsonRpcServer::is_running() {
    return running_.load(std::memory_order_acquire);
}

// =========================================================================
//  Method Registration
// =========================================================================

void UnixSocketJsonRpcServer::register_method(const std::string& method,
                                               MethodHandler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_[method] = std::move(handler);
    logger_->debug("Registered IPC method: " + method);
}

// =========================================================================
//  Event Push
// =========================================================================

void UnixSocketJsonRpcServer::push_event(const std::string& event_name,
                                          const nlohmann::json& data) {
    broadcast_event(event_name, data);
}

// =========================================================================
//  Event Loop
// =========================================================================

void UnixSocketJsonRpcServer::event_loop() {
    constexpr int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    logger_->info("IPC event loop started");

    while (running_.load(std::memory_order_acquire)) {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by signal
            }
            logger_->error(std::string("epoll_wait error: ") + std::strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // New connection
                handle_accept();
            } else {
                // Client socket event
                uint32_t ev = events[i].events;

                if ((ev & (EPOLLERR | EPOLLHUP)) != 0u) {
                    handle_client_close(fd);
                } else {
                    if ((ev & EPOLLIN) != 0u) {
                        handle_client_read(fd);
                    }
                    if ((ev & EPOLLOUT) != 0u) {
                        handle_client_write(fd);
                    }
                }
            }
        }
    }

    logger_->info("IPC event loop exited");
}

// =========================================================================
//  Accept
// =========================================================================

void UnixSocketJsonRpcServer::handle_accept() {
    while (true) {
        struct sockaddr_un addr{};
        socklen_t addr_len = sizeof(addr);
        int client_fd = accept4(listen_fd_,
                                 reinterpret_cast<struct sockaddr*>(&addr),
                                 &addr_len, SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // no more pending connections
            }
            logger_->error(std::string("accept error: ") + std::strerror(errno));
            break;
        }

        // Check client limit
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (clients_.size() >= static_cast<size_t>(MAX_CLIENTS)) {
                logger_->warn("Max clients reached, rejecting connection");
                close(client_fd);
                continue;
            }
        }

        // Register with epoll (edge-triggered, read + write monitoring)
        struct epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
        ev.data.fd = client_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            logger_->error(std::string("epoll_ctl add client error: ") +
                           std::strerror(errno));
            close(client_fd);
            continue;
        }

        // Add to client table
        auto client = std::make_unique<Client>();
        client->fd = client_fd;

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_[client_fd] = std::move(client);
        }

        logger_->debug(std::string("Client connected: fd=") +
                       std::to_string(client_fd));
    }
}

// =========================================================================
//  Read
// =========================================================================

void UnixSocketJsonRpcServer::handle_client_read(int fd) {
    Client* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;
        client = it->second.get();
    }

    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            client->read_buffer.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            // EOF — client disconnected
            handle_client_close(fd);
            return;
        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // no more data right now
            }
            if (errno == EINTR) {
                continue;  // interrupted, try again
            }
            logger_->error(std::string("Read error on fd ") +
                           std::to_string(fd) + ": " + std::strerror(errno));
            handle_client_close(fd);
            return;
        }
    }

    // Try to parse complete messages from the buffer
    std::vector<std::string> messages;
    auto result = parse_messages(client->read_buffer, messages);
    if (result.is_err()) {
        logger_->error(std::string("Message parse error: ") +
                       result.error().technical_message);
        handle_client_close(fd);
        return;
    }

    for (auto& msg : messages) {
        auto req_result = decode_request(msg);
        if (req_result.is_ok()) {
            dispatch_request(fd, req_result.value());
        } else {
            // Send parse error response
            logger_->warn(std::string("Invalid request from fd ") +
                          std::to_string(fd) + ": " +
                          req_result.error().technical_message);

            auto err_resp = make_error_response("", PARSE_ERROR,
                                                  "Parse error");
            auto enc_result = encode_response(err_resp);
            if (enc_result.is_ok()) {
                auto framed = frame_message(enc_result.value());
                if (framed.is_ok()) {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    auto it = clients_.find(fd);
                    if (it != clients_.end()) {
                        it->second->write_buffer += framed.value();
                        it->second->write_pending = true;
                    }
                }
            }
        }
    }
}

// =========================================================================
//  Write
// =========================================================================

void UnixSocketJsonRpcServer::handle_client_write(int fd) {
    Client* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;
        client = it->second.get();
    }

    if (client->write_buffer.empty()) {
        return;
    }

    while (!client->write_buffer.empty()) {
        ssize_t n = write(fd, client->write_buffer.data(),
                          client->write_buffer.size());
        if (n > 0) {
            client->write_buffer.erase(0, static_cast<size_t>(n));
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // socket buffer full, try again later
            }
            if (errno == EINTR) {
                continue;
            }
            logger_->error(std::string("Write error on fd ") +
                           std::to_string(fd) + ": " + std::strerror(errno));
            handle_client_close(fd);
            return;
        }
    }

    client->write_pending = !client->write_buffer.empty();
}

// =========================================================================
//  Close
// =========================================================================

void UnixSocketJsonRpcServer::handle_client_close(int fd) {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            clients_.erase(it);
        }
    }

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    logger_->debug(std::string("Client disconnected: fd=") +
                   std::to_string(fd));
}

// =========================================================================
//  Dispatch
// =========================================================================

void UnixSocketJsonRpcServer::dispatch_request(int fd,
                                                const JsonRpcRequest& req) {
    // Look up handler
    MethodHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        auto it = handlers_.find(req.method);
        if (it != handlers_.end()) {
            handler = it->second;
        }
    }

    // If this is a notification (no id), fire-and-forget
    if (req.is_notification()) {
        if (handler) {
            try {
                handler(req.params);
            } catch (const std::exception& e) {
                logger_->error(std::string("Notification handler error for '") +
                               req.method + "': " + e.what());
            }
        }
        return;
    }

    // Build response
    JsonRpcResponse resp;
    resp.id = req.id;

    if (!handler) {
        resp = make_error_response(req.id, METHOD_NOT_FOUND,
                                    "Method not found: " + req.method);
    } else {
        try {
            resp.result = handler(req.params);
        } catch (const std::exception& e) {
            logger_->error(std::string("Method handler error for '") +
                           req.method + "': " + e.what());
            resp = make_error_response(req.id, INTERNAL_ERROR,
                                        std::string("Internal error: ") + e.what());
        }
    }

    auto enc_result = encode_response(resp);
    if (enc_result.is_err()) {
        logger_->error("Failed to encode response: " +
                       enc_result.error().technical_message);
        return;
    }

    auto framed = frame_message(enc_result.value());
    if (framed.is_err()) {
        logger_->error("Failed to frame response: " +
                       framed.error().technical_message);
        return;
    }

    // Queue response to client's write buffer
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            it->second->write_buffer += framed.value();
            it->second->write_pending = true;
        }
    }
}

// =========================================================================
//  Broadcast
// =========================================================================

void UnixSocketJsonRpcServer::broadcast_event(const std::string& event,
                                               const nlohmann::json& data) {
    auto notif = make_notification(event, data);
    auto enc_result = encode_request(notif);
    if (enc_result.is_err()) {
        logger_->error("Failed to encode notification: " +
                       enc_result.error().technical_message);
        return;
    }

    auto framed = frame_message(enc_result.value());
    if (framed.is_err()) {
        logger_->error("Failed to frame notification: " +
                       framed.error().technical_message);
        return;
    }

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& kv : clients_) {
        kv.second->write_buffer += framed.value();
        kv.second->write_pending = true;
    }
}

} // namespace ipc
} // namespace installer
