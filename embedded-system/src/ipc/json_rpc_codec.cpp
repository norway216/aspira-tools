/**
 * @file json_rpc_codec.cpp
 * @brief Implementation of JSON-RPC 2.0 encoding/decoding and message framing.
 */

#include "json_rpc_codec.h"
#include "installer/core/error_codes.h"
#include <cstdlib>
#include <cstring>

namespace installer {
namespace ipc {

// =========================================================================
//  Encode
// =========================================================================

Result<std::string> encode_request(const JsonRpcRequest& req) {
    try {
        nlohmann::json j;
        j["jsonrpc"] = req.jsonrpc;
        j["method"]  = req.method;

        if (!req.params.is_null()) {
            j["params"] = req.params;
        }

        if (!req.id.empty()) {
            j["id"] = req.id;
        }
        // Leave "id" out entirely for notifications per JSON-RPC 2.0 spec

        return Result<std::string>::ok(j.dump());
    } catch (const std::exception& e) {
        return Result<std::string>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Encode Error",
                                 "Failed to encode JSON-RPC request",
                                 e.what()));
    }
}

Result<std::string> encode_response(const JsonRpcResponse& resp) {
    try {
        nlohmann::json j;
        j["jsonrpc"] = resp.jsonrpc;

        if (resp.is_error()) {
            j["error"] = resp.error;
        } else {
            j["result"] = resp.result.is_null()
                              ? nlohmann::json::object()
                              : resp.result;
        }

        j["id"] = resp.id;

        return Result<std::string>::ok(j.dump());
    } catch (const std::exception& e) {
        return Result<std::string>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Encode Error",
                                 "Failed to encode JSON-RPC response",
                                 e.what()));
    }
}

// =========================================================================
//  Decode
// =========================================================================

Result<JsonRpcRequest> decode_request(const std::string& json) {
    try {
        nlohmann::json j = nlohmann::json::parse(json);

        if (!j.contains("jsonrpc") || j["jsonrpc"] != "2.0") {
            return Result<JsonRpcRequest>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Invalid Request",
                                     "Missing or invalid 'jsonrpc' field"));
        }

        if (!j.contains("method") || !j["method"].is_string()) {
            return Result<JsonRpcRequest>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Invalid Request",
                                     "Missing or invalid 'method' field"));
        }

        JsonRpcRequest req;
        req.jsonrpc = j["jsonrpc"].get<std::string>();
        req.method  = j["method"].get<std::string>();

        if (j.contains("params")) {
            req.params = j["params"];
        }

        if (j.contains("id")) {
            // id can be string, number, or null
            if (j["id"].is_string()) {
                req.id = j["id"].get<std::string>();
            } else if (j["id"].is_number()) {
                req.id = std::to_string(j["id"].get<int64_t>());
            } else if (j["id"].is_null()) {
                // null id => notification in strict JSON-RPC 2.0
                req.id.clear();
            } else {
                req.id = j["id"].dump();
            }
        }
        // If "id" is absent entirely, it's a notification

        return Result<JsonRpcRequest>::ok(std::move(req));
    } catch (const nlohmann::json::parse_error& e) {
        return Result<JsonRpcRequest>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Parse Error",
                                 "Failed to parse JSON request",
                                 e.what()));
    } catch (const std::exception& e) {
        return Result<JsonRpcRequest>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Decode Error",
                                 "Failed to decode JSON-RPC request",
                                 e.what()));
    }
}

Result<JsonRpcResponse> decode_response(const std::string& json) {
    try {
        nlohmann::json j = nlohmann::json::parse(json);

        if (!j.contains("jsonrpc") || j["jsonrpc"] != "2.0") {
            return Result<JsonRpcResponse>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Invalid Response",
                                     "Missing or invalid 'jsonrpc' field"));
        }

        if (!j.contains("id")) {
            return Result<JsonRpcResponse>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Invalid Response",
                                     "Missing 'id' field in response"));
        }

        JsonRpcResponse resp;
        resp.jsonrpc = j["jsonrpc"].get<std::string>();

        if (j["id"].is_string()) {
            resp.id = j["id"].get<std::string>();
        } else if (j["id"].is_number()) {
            resp.id = std::to_string(j["id"].get<int64_t>());
        } else {
            resp.id = j["id"].dump();
        }

        if (j.contains("error") && !j["error"].is_null()) {
            resp.error = j["error"];
        } else if (j.contains("result")) {
            resp.result = j["result"];
        } else {
            // Neither "result" nor "error" present
            return Result<JsonRpcResponse>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Invalid Response",
                                     "Response must contain 'result' or 'error'"));
        }

        return Result<JsonRpcResponse>::ok(std::move(resp));
    } catch (const nlohmann::json::parse_error& e) {
        return Result<JsonRpcResponse>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Parse Error",
                                 "Failed to parse JSON response",
                                 e.what()));
    } catch (const std::exception& e) {
        return Result<JsonRpcResponse>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Decode Error",
                                 "Failed to decode JSON-RPC response",
                                 e.what()));
    }
}

// =========================================================================
//  Helpers
// =========================================================================

JsonRpcResponse make_error_response(const std::string& id, int code,
                                     const std::string& message,
                                     const nlohmann::json& data) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.error["code"]    = code;
    resp.error["message"] = message;
    if (!data.is_null()) {
        resp.error["data"] = data;
    }
    return resp;
}

JsonRpcRequest make_notification(const std::string& method,
                                  const nlohmann::json& params) {
    JsonRpcRequest req;
    req.method = method;
    req.params = params;
    // id is empty => notification
    return req;
}

// =========================================================================
//  Message Framing
// =========================================================================

Result<std::string> frame_message(const std::string& json) {
    try {
        std::string framed;
        framed.reserve(json.size() + 64);
        framed += "Content-Length: ";
        framed += std::to_string(json.size());
        framed += "\r\n\r\n";
        framed += json;
        framed += "\n";
        return Result<std::string>::ok(std::move(framed));
    } catch (const std::exception& e) {
        return Result<std::string>::err(
            InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                 "Frame Error",
                                 "Failed to frame message",
                                 e.what()));
    }
}

Result<void> parse_messages(std::string& buffer,
                            std::vector<std::string>& messages) {
    messages.clear();

    while (true) {
        // Look for "Content-Length: "
        const char header_prefix[] = "Content-Length: ";
        const size_t prefix_len    = sizeof(header_prefix) - 1;

        size_t pos = buffer.find(header_prefix);
        if (pos == std::string::npos) {
            // No header found — discard if buffer is getting too large
            // without useful data
            if (buffer.size() > 65536) {
                return Result<void>::err(
                    InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                         "Frame Error",
                                         "No Content-Length header found "
                                         "in buffer",
                                         "Buffer too large without framing"));
            }
            break;
        }

        // If there's garbage before the header, discard it
        if (pos > 0) {
            buffer.erase(0, pos);
            pos = 0;
        }

        // Find end of header line (\r\n)
        size_t header_end = buffer.find("\r\n", pos);
        if (header_end == std::string::npos) {
            // Header not yet complete — wait for more data
            break;
        }

        // Parse the content length
        std::string len_str = buffer.substr(pos + prefix_len,
                                              header_end - pos - prefix_len);
        char* end_ptr = nullptr;
        long content_length = std::strtol(len_str.c_str(), &end_ptr, 10);
        if (end_ptr == len_str.c_str() || content_length < 0 ||
            content_length > 256L * 1024 * 1024) {  // max 256 MB
            return Result<void>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Frame Error",
                                     "Invalid Content-Length value: " + len_str));
        }

        // Check for the blank line separator (\r\n\r\n after header)
        size_t body_start = header_end + 2;  // skip \r\n
        if (buffer.size() < body_start + 2) {
            break;  // wait for more data
        }
        if (buffer[body_start] != '\r' || buffer[body_start + 1] != '\n') {
            return Result<void>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Frame Error",
                                     "Missing blank line after Content-Length header"));
        }
        body_start += 2;  // skip \r\n

        // Check if we have the full body plus trailing newline
        size_t required = body_start + static_cast<size_t>(content_length) + 1; // +1 for trailing \n
        if (buffer.size() < required) {
            break;  // wait for more data
        }

        // Verify trailing newline
        if (buffer[body_start + static_cast<size_t>(content_length)] != '\n') {
            return Result<void>::err(
                InstallerError::make(ErrorCode::INTERNAL_ERROR,
                                     "Frame Error",
                                     "Missing trailing newline after message body"));
        }

        // Extract the message
        std::string message = buffer.substr(body_start,
                                             static_cast<size_t>(content_length));
        messages.push_back(std::move(message));

        // Consume the parsed message from the buffer
        buffer.erase(0, required);
    }

    return Result<void>::ok();
}

} // namespace ipc
} // namespace installer
