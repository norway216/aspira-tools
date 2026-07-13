/**
 * @file test_json_rpc_codec.cpp
 * @brief Unit tests for the JSON-RPC 2.0 codec.
 */

#include "src/ipc/json_rpc_codec.h"

#include <nlohmann/json.hpp>

#if __has_include(<gtest/gtest.h>)
#include <gtest/gtest.h>
#else
#include "helpers/minimal_test.h"
#endif

using namespace installer::ipc;

// =========================================================================
//  Request encoding
// =========================================================================

TEST(JsonRpcCodecTest, EncodeRequest) {
    JsonRpcRequest req;
    req.method = "device.list";
    req.params = nlohmann::json::object();
    req.id     = "1";

    auto result = encode_request(req);
    EXPECT_TRUE(result.is_ok());

    auto parsed = nlohmann::json::parse(result.value());
    EXPECT_EQ(parsed["jsonrpc"], "2.0");
    EXPECT_EQ(parsed["method"], "device.list");
    EXPECT_EQ(parsed["id"], "1");
}

TEST(JsonRpcCodecTest, EncodeNotification) {
    JsonRpcRequest req;
    req.method = "job.progress_changed";
    req.params = {{"job_id", "abc"}, {"percent", 50}};

    auto result = encode_request(req);
    EXPECT_TRUE(result.is_ok());

    auto parsed = nlohmann::json::parse(result.value());
    EXPECT_FALSE(parsed.contains("id"));  // notification: no id field
    EXPECT_EQ(parsed["method"], "job.progress_changed");
    EXPECT_EQ(parsed["params"]["job_id"], "abc");
}

// =========================================================================
//  Request decoding
// =========================================================================

TEST(JsonRpcCodecTest, DecodeRequest) {
    std::string json = R"({"jsonrpc":"2.0","method":"ping","id":"1"})";

    auto result = decode_request(json);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().method, "ping");
    EXPECT_EQ(result.value().id, "1");
}

TEST(JsonRpcCodecTest, DecodeNotification) {
    std::string json = R"({"jsonrpc":"2.0","method":"job.log","params":{"msg":"hello"}})";

    auto result = decode_request(json);
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().is_notification());
}

TEST(JsonRpcCodecTest, DecodeInvalidJson) {
    auto result = decode_request("not valid json");
    EXPECT_TRUE(result.is_err());
}

TEST(JsonRpcCodecTest, DecodeMissingMethod) {
    std::string json = R"({"jsonrpc":"2.0","id":"1"})";

    auto result = decode_request(json);
    EXPECT_TRUE(result.is_err());
}

// =========================================================================
//  Response encoding
// =========================================================================

TEST(JsonRpcCodecTest, EncodeResponseSuccess) {
    JsonRpcResponse resp;
    resp.id     = "1";
    resp.result = {{"pong", true}};

    auto result = encode_response(resp);
    EXPECT_TRUE(result.is_ok());

    auto parsed = nlohmann::json::parse(result.value());
    EXPECT_EQ(parsed["id"], "1");
    EXPECT_EQ(parsed["result"]["pong"], true);
    EXPECT_FALSE(parsed.contains("error"));
}

TEST(JsonRpcCodecTest, EncodeResponseError) {
    JsonRpcResponse resp = make_error_response("1", -32601,
                                                 "Method not found");

    auto result = encode_response(resp);
    EXPECT_TRUE(result.is_ok());

    auto parsed = nlohmann::json::parse(result.value());
    EXPECT_EQ(parsed["id"], "1");
    EXPECT_EQ(parsed["error"]["code"], -32601);
    EXPECT_EQ(parsed["error"]["message"], "Method not found");
}

// =========================================================================
//  Response decoding
// =========================================================================

TEST(JsonRpcCodecTest, DecodeResponseSuccess) {
    std::string json = R"({"jsonrpc":"2.0","result":42,"id":"1"})";

    auto result = decode_response(json);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().id, "1");
    EXPECT_EQ(result.value().result, 42);
    EXPECT_FALSE(result.value().is_error());
}

TEST(JsonRpcCodecTest, DecodeResponseError) {
    std::string json = R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"Method not found"},"id":"1"})";

    auto result = decode_response(json);
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().is_error());
    EXPECT_EQ(result.value().error["code"], -32601);
}

// =========================================================================
//  Message framing
// =========================================================================

TEST(JsonRpcCodecTest, FrameMessage) {
    std::string json = R"({"jsonrpc":"2.0","method":"ping","id":"1"})";

    auto result = frame_message(json);
    EXPECT_TRUE(result.is_ok());

    std::string framed = result.value();
    EXPECT_TRUE(framed.find("Content-Length: ") == 0);
    EXPECT_TRUE(framed.find("\r\n\r\n") != std::string::npos);
    EXPECT_TRUE(framed.find(json) != std::string::npos);
    EXPECT_EQ(framed.back(), '\n');
}

TEST(JsonRpcCodecTest, ParseSingleMessage) {
    std::string json = R"({"jsonrpc":"2.0","method":"ping","id":"1"})";
    auto framed_result = frame_message(json);
    ASSERT_TRUE(framed_result.is_ok());
    std::string buffer = framed_result.value();

    std::vector<std::string> messages;
    auto parse_result = parse_messages(buffer, messages);
    EXPECT_TRUE(parse_result.is_ok());
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0], json);
    EXPECT_TRUE(buffer.empty());  // consumed
}

TEST(JsonRpcCodecTest, ParseMultipleMessages) {
    std::string json1 = R"({"jsonrpc":"2.0","method":"ping","id":"1"})";
    std::string json2 = R"({"jsonrpc":"2.0","method":"device.list","id":"2"})";

    auto f1 = frame_message(json1);
    auto f2 = frame_message(json2);
    ASSERT_TRUE(f1.is_ok());
    ASSERT_TRUE(f2.is_ok());

    std::string buffer = f1.value() + f2.value();

    std::vector<std::string> messages;
    auto result = parse_messages(buffer, messages);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], json1);
    EXPECT_EQ(messages[1], json2);
    EXPECT_TRUE(buffer.empty());
}

TEST(JsonRpcCodecTest, ParsePartialMessage) {
    std::string json = R"({"jsonrpc":"2.0","method":"ping","id":"1"})";
    auto framed = frame_message(json);
    ASSERT_TRUE(framed.is_ok());

    // Truncate half the framed message
    std::string buffer = framed.value().substr(0, framed.value().size() / 2);

    std::vector<std::string> messages;
    auto result = parse_messages(buffer, messages);
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(messages.size(), 0u);  // incomplete frame
    EXPECT_FALSE(buffer.empty());    // data retained
}

TEST(JsonRpcCodecTest, ParseCombineChunks) {
    std::string json = R"({"test":true})";
    auto framed = frame_message(json);
    ASSERT_TRUE(framed.is_ok());

    std::string full  = framed.value();
    size_t      split = full.find("\r\n\r\n") + 4;  // right after blank line

    std::string buffer;
    std::vector<std::string> messages;

    // First chunk: header + blank line
    buffer += full.substr(0, split);
    auto r1 = parse_messages(buffer, messages);
    EXPECT_TRUE(r1.is_ok());
    EXPECT_EQ(messages.size(), 0u);

    // Second chunk: body + trailing newline
    buffer += full.substr(split);
    auto r2 = parse_messages(buffer, messages);
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0], json);
}

// =========================================================================
//  Error response helpers
// =========================================================================

TEST(JsonRpcCodecTest, MakeErrorResponse) {
    auto resp = make_error_response("42", -32601, "Method not found",
                                     nlohmann::json{{"detail", "no handler"}});

    EXPECT_EQ(resp.id, "42");
    EXPECT_EQ(resp.error["code"], -32601);
    EXPECT_EQ(resp.error["message"], "Method not found");
    EXPECT_EQ(resp.error["data"]["detail"], "no handler");
    EXPECT_TRUE(resp.result.is_null());
}

// =========================================================================
//  Make notification helper
// =========================================================================

TEST(JsonRpcCodecTest, MakeNotification) {
    auto notif = make_notification("job.log",
                                    nlohmann::json{{"msg", "hello"}});

    EXPECT_TRUE(notif.is_notification());
    EXPECT_EQ(notif.method, "job.log");
    EXPECT_EQ(notif.params["msg"], "hello");
}
