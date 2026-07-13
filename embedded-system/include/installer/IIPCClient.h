/**
 * @file IIPCClient.h
 * @brief IPC client interface for the embedded Linux installer.
 *
 * Defines the contract for JSON-RPC 2.0 clients that connect
 * to the installer-core daemon over a Unix domain socket.
 */

#ifndef INSTALLER_IIPCCLIENT_H
#define INSTALLER_IIPCCLIENT_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <chrono>
#include <memory>
#include <string>

#ifndef NLOHMANN_JSON_FWD_HPP
namespace nlohmann {
    class json;
}
#endif

namespace installer {

class IIPCClient {
public:
    virtual ~IIPCClient() = default;

    /**
     * Connect to the daemon at the given Unix domain socket path.
     */
    virtual Result<void> connect(const std::string& socket_path) = 0;

    /**
     * Disconnect and release resources.
     */
    virtual void disconnect() = 0;

    /**
     * Invoke a remote method and wait for the result.
     *
     * @param method   The JSON-RPC method name, e.g. "device.list".
     * @param params   The parameters object.
     * @param timeout  Maximum time to wait for the response.
     * @return         The "result" field of the JSON-RPC response on success,
     *                 or an error if the call fails, times out, or the
     *                 server returned a JSON-RPC error.
     */
    virtual Result<nlohmann::json> call(const std::string& method,
                                        const nlohmann::json& params,
                                        std::chrono::milliseconds timeout) = 0;

    /**
     * Returns true if the client is currently connected to the daemon.
     */
    virtual bool is_connected() = 0;
};

} // namespace installer

#endif // INSTALLER_IIPCCLIENT_H
