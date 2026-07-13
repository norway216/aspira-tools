/**
 * @file ipc_server.h
 * @brief Unix Domain Socket JSON-RPC 2.0 server using epoll.
 *
 * Implements IIPCServer with a single-threaded event loop based on
 * edge-triggered epoll, non-blocking sockets, and length-prefixed
 * message framing.
 */

#ifndef INSTALLER_IPC_SERVER_H
#define INSTALLER_IPC_SERVER_H

#include "installer/IIPCServer.h"
#include "installer/ILogger.h"
#include "json_rpc_codec.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace installer {
namespace ipc {

class UnixSocketJsonRpcServer : public IIPCServer {
public:
    explicit UnixSocketJsonRpcServer(std::shared_ptr<ILogger> logger);
    ~UnixSocketJsonRpcServer() override;

    // ---- IIPCServer interface ----
    Result<void> start(const std::string& socket_path) override;
    void stop() override;
    bool is_running() override;

    void register_method(const std::string& method,
                         MethodHandler handler) override;

    void push_event(const std::string& event_name,
                    const nlohmann::json& data) override;

private:
    void event_loop();
    void handle_accept();
    void handle_client_read(int fd);
    void handle_client_write(int fd);
    void handle_client_close(int fd);
    void dispatch_request(int fd, const JsonRpcRequest& req);
    void broadcast_event(const std::string& event, const nlohmann::json& data);

    /**
     * Per-client connection state.
     */
    struct Client {
        int fd = -1;
        std::string read_buffer;
        std::string write_buffer;
        bool write_pending = false;
    };

    // ---- Constants ----
    static constexpr int MAX_CLIENTS      = 32;
    static constexpr int EPOLL_TIMEOUT_MS = 100;

    // ---- State ----
    std::shared_ptr<ILogger> logger_;
    std::string socket_path_;

    int listen_fd_ = -1;
    int epoll_fd_  = -1;

    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    // Client table: fd -> Client. Protected by clients_mutex_.
    mutable std::mutex clients_mutex_;
    std::unordered_map<int, std::unique_ptr<Client>> clients_;

    // Method dispatch table. Protected by handlers_mutex_.
    mutable std::mutex handlers_mutex_;
    std::unordered_map<std::string, MethodHandler> handlers_;
};

} // namespace ipc
} // namespace installer

#endif // INSTALLER_IPC_SERVER_H
