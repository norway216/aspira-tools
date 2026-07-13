/**
 * @file IIPCServer.h
 * @brief IPC server interface for the embedded Linux installer.
 *
 * Defines the contract for JSON-RPC 2.0 servers that accept
 * client connections and dispatch method calls.
 */

#ifndef INSTALLER_IIPCSERVER_H
#define INSTALLER_IIPCSERVER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <functional>
#include <memory>
#include <string>

// Forward-declare nlohmann::json
#ifndef NLOHMANN_JSON_FWD_HPP
namespace nlohmann {
    class json;
}
#endif

namespace installer {

class IIPCServer {
public:
    virtual ~IIPCServer() = default;

    /**
     * Start listening on the given Unix domain socket path.
     * The directory containing the socket must already exist.
     */
    virtual Result<void> start(const std::string& socket_path) = 0;

    /**
     * Gracefully stop the server: close all client connections,
     * unlink the socket file, join the event-loop thread.
     */
    virtual void stop() = 0;

    /**
     * Returns true if the server is currently accepting connections.
     */
    virtual bool is_running() = 0;

    /**
     * Signature for a method handler.
     * @param params  The JSON-RPC "params" object.
     * @return        The result value (will be placed under "result" in the response).
     *                Throw or return an error-structured json to signal failure.
     */
    using MethodHandler = std::function<nlohmann::json(const nlohmann::json& params)>;

    /**
     * Register a handler for a named method (e.g. "device.list").
     * If a handler is already registered for the method, it is replaced.
     */
    virtual void register_method(const std::string& method, MethodHandler handler) = 0;

    /**
     * Push a JSON-RPC notification event to all connected clients.
     * @param event_name  The event name, e.g. "job.progress_changed".
     * @param data        The event payload.
     */
    virtual void push_event(const std::string& event_name,
                            const nlohmann::json& data) = 0;
};

} // namespace installer

#endif // INSTALLER_IIPCSERVER_H
