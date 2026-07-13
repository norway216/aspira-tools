/**
 * @file iipc_client.h
 * @brief Abstract IPC client interface.
 *
 * Connects to the installer daemon's Unix-domain socket and provides
 * a simple request/response RPC call interface. Designed for use by
 * CLI tools, GUI frontends, and system monitoring agents.
 */

#ifndef INSTALLER_IPC_IIPC_CLIENT_H
#define INSTALLER_IPC_IIPC_CLIENT_H

#include <chrono>
#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Abstract IPC client interface.
 *
 * Connects to the installer daemon over a Unix-domain socket to issue
 * RPC calls. JSON is passed as strings — the client is responsible for
 * serialising its parameters into JSON and deserialising the response.
 * This keeps the interface independent of any particular JSON library.
 */
class IIPCClient {
public:
    virtual ~IIPCClient() = default;

    /**
     * Connect to the installer daemon at the given socket path.
     *
     * Opens a Unix-domain stream socket and performs any necessary
     * handshake. Only one connection can be active at a time; calling
     * connect() while already connected implicitly calls disconnect()
     * first.
     *
     * @param socket_path Absolute path to the daemon's listening socket
     *                    (e.g. "/run/installer/ipc.sock").
     * @return Result<void> — ok when connected, error if the socket
     *         does not exist or the connection is refused.
     */
    virtual Result<void> connect(const std::string& socket_path) = 0;

    /**
     * Disconnect from the daemon.
     *
     * Closes the socket. Safe to call even if not currently connected
     * (no-op). After this call, is_connected() returns false.
     */
    virtual void disconnect() = 0;

    /**
     * Issue a synchronous RPC call to the daemon.
     *
     * Sends a request with the given method name and params, then blocks
     * until the daemon responds or the timeout expires.
     *
     * @param method  Name of the method to invoke (must match a handler
     *                registered on the server side).
     * @param params  JSON object string containing the call parameters.
     * @param timeout Maximum time to wait for a response. A value of
     *                zero means wait indefinitely.
     * @return The JSON response string from the server, or an error
     *         if the connection is lost or the timeout expires.
     */
    virtual Result<std::string> call(const std::string& method,
                                     const std::string& params,
                                     std::chrono::milliseconds timeout) = 0;

    /**
     * Return whether the client is currently connected to the daemon.
     *
     * @return true if connect() succeeded and disconnect() has not
     *         been called (and the connection has not been lost).
     */
    virtual bool is_connected() const = 0;
};

} // namespace installer

#endif // INSTALLER_IPC_IIPC_CLIENT_H
