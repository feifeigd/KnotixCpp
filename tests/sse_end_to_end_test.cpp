#include "openai_compatible_client.hpp"

#include "check_json.hpp"
#include "mock_http_server.hpp"

#include "caf/test/test.hpp"

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/net/middleman.hpp>
#include <caf/net/multiplexer.hpp>

#include <chrono>
#include <future>
#include <optional>
#include <string>

using namespace std::chrono_literals;
using agent::ProviderConfig;

namespace {

struct CompletionResult {
    json message;
    std::string error;
};

// Runs one chat completion against the given provider on a throwaway actor
// system and returns the callback result; std::nullopt on timeout.
std::optional<CompletionResult> runChatCompletion(ProviderConfig provider,
                                                  std::string const& payload) {
    // Make sure libcurl talks to the mock server directly even when the
    // environment configures a proxy.
    test::setEnv("no_proxy", "127.0.0.1,localhost");
    test::setEnv("NO_PROXY", "127.0.0.1,localhost");
    caf::actor_system_config cfg;
    cfg.load<caf::net::middleman>();
    caf::actor_system sys{cfg};
    auto* mpx = caf::net::multiplexer::from(sys);
    if (mpx == nullptr)
        throw std::runtime_error("CAF network multiplexer is unavailable.");

    agent::CurlRuntime curlRuntime;
    std::promise<CompletionResult> completion;
    auto result = completion.get_future();
    agent::requestChatCompletion(
        *mpx, std::move(provider), payload,
        [&completion](json message, std::string error) {
            completion.set_value({std::move(message), std::move(error)});
        });
    if (result.wait_for(30s) != std::future_status::ready)
        return std::nullopt;
    return result.get();
}

ProviderConfig mockProvider(uint16_t port, bool streaming) {
    ProviderConfig provider;
    provider.name = "mock";
    provider.baseUrl = "http://127.0.0.1:" + std::to_string(port);
    provider.model = "mock-model";
    provider.streaming = streaming;
    provider.apiKey = "test-key";
    return provider;
}

const json requestPayload = json::object({
    {"model", "mock-model"},
    {"messages", json::array({json::object({{"role", "user"}, {"content", "hi"}})})},
});

} // namespace

SUITE("agent.openai_compatible_client") {

TEST("a streamed tool call is assembled end to end") {
    auto const sseBody
        = std::string{"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                      "\"id\":\"call_1\",\"function\":{\"name\":\"add\"}}]}}]}\n\n"}
          + "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"{\\\"a\\\":\"}}]}}]}\n\n"
          + "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"1,\\\"b\\\":2}\"}}]}}]}\n\n"
          + "data: [DONE]\n\n";
    test::MockHttpServer server{{"200 OK", "text/event-stream", sseBody}};
    auto const result = runChatCompletion(mockProvider(server.port(), true),
                                          requestPayload.dump());
    require(result.has_value());
    require(result->error.empty());
    auto const& toolCalls = jsonGet(result->message, "tool_calls");
    require(toolCalls.is_array());
    require_eq(toolCalls.size(), size_t{1});
    auto const& call = toolCalls.front();
    check_json_eq(jsonGet(call, "id"), json("call_1"));
    check_json_eq(jsonGet(jsonGet(call, "function"), "name"), json("add"));
    check_json_eq(jsonGet(jsonGet(call, "function"), "arguments"),
                  json("{\"a\":1,\"b\":2}"));

    auto const requests = server.waitForRequests(1);
    check(requests.front().find("POST /chat/completions HTTP/1.1") == 0);
    check(requests.front().find("Authorization: Bearer test-key") != std::string::npos);
    check(requests.front().find("Accept: text/event-stream") != std::string::npos);
}

TEST("a non-streamed response returns the assistant message") {
    auto const body = json::object({
        {"choices",
         json::array({json::object({
             {"message", json::object({{"role", "assistant"}, {"content", "pong"}})},
         })})},
    }).dump();
    test::MockHttpServer server{{"200 OK", "application/json", body}};
    auto const result = runChatCompletion(mockProvider(server.port(), false),
                                          requestPayload.dump());
    require(result.has_value());
    require(result->error.empty());
    check_json_eq(jsonGet(result->message, "content"), json("pong"));

    auto const requests = server.waitForRequests(1);
    check(requests.front().find("Accept: application/json") != std::string::npos);
}

TEST("an HTTP error surfaces status code and error body") {
    test::MockHttpServer server{{"500 Internal Server Error", "application/json",
                                 R"({"error":"backend exploded"})"}};
    auto const result = runChatCompletion(mockProvider(server.port(), true),
                                          requestPayload.dump());
    require(result.has_value());
    require(!result->error.empty());
    check(result->error.find("500") != std::string::npos);
    check(result->error.find("backend exploded") != std::string::npos);
}

TEST("a stream without a DONE event is reported as an error") {
    test::MockHttpServer server{
        {"200 OK", "text/event-stream",
         "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n"}};
    auto const result = runChatCompletion(mockProvider(server.port(), true),
                                          requestPayload.dump());
    require(result.has_value());
    require(!result->error.empty());
    check(result->error.find("[DONE]") != std::string::npos);
}

} // SUITE("agent.openai_compatible_client")
