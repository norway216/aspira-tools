/**
 * @file ipc_client.h
 * @brief JSON-RPC 2.0 client over Unix domain socket.
 *
 * Implements IIPCClient with synchronous request-response semantics
 * and a configurable timeout.
 */

#ifndef INSTALLER_IPC_CLIENT_H
#define INSTALLER_IPC_CLIENT_H

#include "installer/IIPCClient.h"
#include "installer/ILogger.h"
#include "json_rpc_codec.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace installer {
namespace ipc {

class UnixSocketJsonRpcClient : public IIPCClient {
public:
    explicit UnixSocketJsonRpcClient(std::shared_ptr<ILogger> logger);
    ~UnixSocketJsonRpcClient() override;

    // ---- IIPCClient interface ----
    Result<void> connect(const std::string& socket_path) override;
    void disconnect() override;
    Result<nlohmann::json> call(const std::string& method,
                                const nlohmann::json& params,
                                std::chrono::milliseconds timeout) override;
    bool is_connected() override;

private:
    /**
     * Read exactly the specified number of bytes from the socket,
     * with a total deadline.
     */
    Result<std::string> read_exact(size_t count,
                                   std::chrono::steady_clock::time_point deadline);

    /**
     * Send a full message (Content-Length framed) to the server.
     */
    Result<void> send_message(const std::string& json);

    /**
     * Receive a full message (Content-Length framed) from the server.
     */
    Result<std::string> recv_message(std::chrono::steady_clock::time_point deadline);

    std::shared_ptr<ILogger> logger_;
    int fd_ = -1;
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;  // serialise call() invocations
    std::atomic<uint64_t> request_counter_{0};
};

} // namespace ipc
} // namespace installer

#endif // INSTALLER_IPC_CLIENT_H
