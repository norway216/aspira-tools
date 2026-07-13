/**
 * @file json_rpc_codec.h
 * @brief JSON-RPC 2.0 message encoding and decoding using nlohmann/json.
 *
 * Implements request/response serialization and length-prefixed
 * message framing suitable for stream-oriented transports
 * (Unix domain sockets).
 */

#ifndef INSTALLER_JSON_RPC_CODEC_H
#define INSTALLER_JSON_RPC_CODEC_H

#include "installer/core/result.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace installer {
namespace ipc {

// ---- Data structures ----

struct JsonRpcRequest {
    std::string jsonrpc = "2.0";
    std::string method;
    nlohmann::json params;
    std::string id;  // empty string => notification (no response expected)

    bool is_notification() const { return id.empty(); }
};

struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    nlohmann::json result;
    nlohmann::json error;   // object with "code", "message", optional "data"
    std::string id;

    bool is_error() const { return !error.is_null(); }
};

struct JsonRpcError {
    int code = 0;
    std::string message;
    nlohmann::json data;
};

// ---- Standard JSON-RPC 2.0 error codes ----
constexpr int PARSE_ERROR      = -32700;
constexpr int INVALID_REQUEST  = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS   = -32602;
constexpr int INTERNAL_ERROR   = -32603;

// Application-level error code range: -32000 to -32099
constexpr int APP_ERROR_START  = -32000;

// ---- Encode functions ----

/**
 * Serialize a request to a JSON string.
 */
Result<std::string> encode_request(const JsonRpcRequest& req);

/**
 * Serialize a response to a JSON string.
 */
Result<std::string> encode_response(const JsonRpcResponse& resp);

// ---- Decode functions ----

/**
 * Parse a JSON string into a JsonRpcRequest.
 * Returns an error if the JSON is invalid or not a valid request.
 */
Result<JsonRpcRequest> decode_request(const std::string& json);

/**
 * Parse a JSON string into a JsonRpcResponse.
 * Returns an error if the JSON is invalid or not a valid response.
 */
Result<JsonRpcResponse> decode_response(const std::string& json);

// ---- Helpers ----

/**
 * Create a JSON-RPC error response with the given id and error details.
 */
JsonRpcResponse make_error_response(const std::string& id, int code,
                                    const std::string& message,
                                    const nlohmann::json& data = nlohmann::json{});

/**
 * Create a JSON-RPC notification (event).
 */
JsonRpcRequest make_notification(const std::string& method,
                                 const nlohmann::json& params);

// ---- Message Framing ----
//
// Uses HTTP-like length-prefixed framing:
//   "Content-Length: <N>\r\n\r\n<JSON payload>\n"
//
// This allows streaming multiple messages on the same connection
// without a delimiter character that could appear in the payload.

/**
 * Wrap a JSON payload string with a Content-Length header.
 */
Result<std::string> frame_message(const std::string& json);

/**
 * Parse accumulated buffer data into zero or more complete messages.
 *
 * @param buffer    The accumulated read buffer (modified in-place:
 *                   consumed data is removed, trailing partial data remains).
 * @param messages  Output: complete JSON payload strings (one per message).
 * @return          Ok on success (even if zero messages were extracted).
 *                  Err if framing is malformed.
 */
Result<void> parse_messages(std::string& buffer,
                            std::vector<std::string>& messages);

} // namespace ipc
} // namespace installer

#endif // INSTALLER_JSON_RPC_CODEC_H
