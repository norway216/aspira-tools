/**
 * @file iipc_server.h
 * @brief Abstract IPC server interface.
 *
 * Provides a Unix-domain-socket-based RPC mechanism for external
 * processes (CLI, GUI, system monitor) to communicate with the
 * installer daemon. Methods are registered by name and dispatched
 * when a client sends a matching request. Events are pushed to all
 * connected clients (pub/sub broadcast).
 */

#ifndef INSTALLER_IPC_IIPC_SERVER_H
#define INSTALLER_IPC_IIPC_SERVER_H

#include <functional>
#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Abstract IPC server interface.
 *
 * Listens on a Unix-domain socket for client connections. Supports
 * two communication patterns:
 * 1. **Request/Response** — clients call named methods with JSON
 *    params; the server dispatches to registered handlers and returns
 *    the JSON result.
 * 2. **Event push** — the server broadcasts named events with JSON
 *    payloads to all currently connected clients.
 *
 * The JSON data is passed as strings. Implementations may use a
 * dedicated JSON library (nlohmann/json, rapidjson, etc.) internally;
 * the interface deliberately uses std::string to avoid coupling the
 * entire project to a specific JSON implementation.
 */
class IIPCServer {
public:
    virtual ~IIPCServer() = default;

    /**
     * Signature for a registered method handler.
     *
     * Receives the JSON params string and returns a JSON result string.
     * If the handler encounters an error, it should encode the error
     * information into the returned JSON string (e.g. {"error": "..."})
     * rather than returning an empty or malformed string.
     */
    using MethodHandler = std::function<std::string(const std::string& params)>;

    /**
     * Start listening on the given Unix-domain socket path.
     *
     * Creates the socket, binds it, and begins accepting client
     * connections. If a stale socket file exists at @p socket_path it
     * is unlinked before binding.
     *
     * @param socket_path Absolute path for the Unix-domain socket
     *                    (e.g. "/run/installer/ipc.sock").
     * @return Result<void> — ok when listening, error if the socket
     *         cannot be created or bound.
     */
    virtual Result<void> start(const std::string& socket_path) = 0;

    /**
     * Stop listening and disconnect all clients.
     *
     * Closes the listening socket, disconnects all connected clients,
     * and unlinks the socket file. After this call, start() may be
     * called again to re-bind (possibly on a different path).
     */
    virtual void stop() = 0;

    /**
     * Return whether the server is currently listening for connections.
     *
     * @return true if start() has been called and stop() has not.
     */
    virtual bool is_running() const = 0;

    /**
     * Register a named method handler.
     *
     * When a client sends a request with a matching method name, the
     * handler is invoked with the request's params string and its
     * return value is sent back to the client as the response.
     *
     * Registering a handler for a name that already has one replaces
     * the previous handler.
     *
     * @param name    Method name (e.g. "get_status", "start_install").
     * @param handler Function to invoke for this method.
     */
    virtual void register_method(const std::string& name,
                                 MethodHandler handler) = 0;

    /**
     * Push an event to all connected clients.
     *
     * Sends a broadcast message with the given event name and JSON
     * payload. This is a fire-and-forget operation — there is no
     * response from clients.
     *
     * @param event_name Machine-readable event identifier
     *                   (e.g. "progress_update", "job_completed").
     * @param data       JSON object string with event payload.
     */
    virtual void push_event(const std::string& event_name,
                            const std::string& data) = 0;
};

} // namespace installer

#endif // INSTALLER_IPC_IIPC_SERVER_H
