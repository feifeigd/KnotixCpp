#include "agent_conversation.hpp"

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
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using agent::ProviderConfig;

namespace {

struct ConversationOutcome {
    bool success = false;
    std::string error;
};

// Runs a full conversation against the given provider on a throwaway actor
// system; std::nullopt on timeout.
std::optional<ConversationOutcome> runConversation(ProviderConfig provider,
                                                   std::string const& prompt) {
    test::setEnv("no_proxy", "127.0.0.1,localhost");
    test::setEnv("NO_PROXY", "127.0.0.1,localhost");
    caf::actor_system_config cfg;
    cfg.load<caf::net::middleman>();
    caf::actor_system sys{cfg};
    auto* mpx = caf::net::multiplexer::from(sys);
    if (mpx == nullptr)
        throw std::runtime_error("CAF network multiplexer is unavailable.");

    agent::CurlRuntime curlRuntime;
    std::promise<ConversationOutcome> completion;
    auto result = completion.get_future();
    auto conversation = std::make_shared<agent::AgentConversation>(
        *mpx, std::move(provider), prompt,
        [&completion](std::exception_ptr error) {
            ConversationOutcome outcome;
            if (error == nullptr) {
                outcome.success = true;
            } else {
                try {
                    std::rethrow_exception(error);
                } catch (std::exception const& e) {
                    outcome.error = e.what();
                } catch (...) {
                    outcome.error = "unknown error";
                }
            }
            completion.set_value(std::move(outcome));
        });
    conversation->start();
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
    provider.chatModelConfig.APIKey = "test-key";
    return provider;
}

// A complete non-streaming Chat Completions response carrying `message`.
std::string chatResponse(json const& message) {
    return json::object({
               {"choices",
                json::array({json::object({
                    {"index", 0},
                    {"finish_reason", "stop"},
                    {"message", message},
                })})},
           })
        .dump();
}

// One SSE event carrying `delta`, terminated by the blank line.
std::string sseEvent(json const& delta) {
    return "data: "
           + json::object(
                 {{"choices", json::array({json::object({{"delta", delta}})})}})
                 .dump()
           + "\n\n";
}

json addToolCall() {
    return json::object({
        {"id", "call_1"},
        {"type", "function"},
        {"function", json::object({{"name", "add"}, {"arguments", R"({"a":1,"b":2})"}})},
    });
}

json assistantToolCallMessage() {
    return json::object({
        {"role", "assistant"},
        {"tool_calls", json::array({addToolCall()})},
    });
}

} // namespace

SUITE("agent.conversation") {

TEST("a tool-call round trip completes with the final answer") {
    test::MockHttpServer server{{
        {"200 OK", "application/json", chatResponse(assistantToolCallMessage())},
        {"200 OK", "application/json",
         chatResponse(json::object({{"role", "assistant"}, {"content", "和是 3"}}))},
    }};
    auto const outcome = runConversation(mockProvider(server.port(), false),
                                         "1+2 是多少");
    require(outcome.has_value());
    require(outcome->success);

    auto const requests = server.waitForRequests(2);
    // The first request carries the system prompt, the user prompt and tools.
    check(requests.at(0).find("tool_choice") != std::string::npos);
    // The second request appends the assistant tool call and its tool result.
    auto const& second = requests.at(1);
    check(second.find(R"("tool_call_id":"call_1")") != std::string::npos);
    check(second.find(R"("role":"tool")") != std::string::npos);
    // The add handler returns a double, serialized as "3.0".
    check(second.find(R"("content":"3.0")") != std::string::npos);
}

TEST("a streamed tool-call round trip completes as well") {
    auto const sseToolCall
        = sseEvent(json::object({{"tool_calls", json::array({json::object({
                                     {"index", 0},
                                     {"id", "call_1"},
                                     {"type", "function"},
                                     {"function",
                                      json::object({{"name", "add"},
                                                    {"arguments", R"({"a":1,"b":2})"}})},
                                 })})}}))
          + "data: [DONE]\n\n";
    auto const sseAnswer = sseEvent(json::object({{"content", "和是 3"}}))
                           + "data: [DONE]\n\n";
    test::MockHttpServer server{{
        {"200 OK", "text/event-stream", sseToolCall},
        {"200 OK", "text/event-stream", sseAnswer},
    }};
    auto const outcome = runConversation(mockProvider(server.port(), true),
                                         "1+2 是多少");
    require(outcome.has_value());
    check(outcome->success);
    check_eq(server.requestCount(), size_t{2});
}

TEST("an unknown tool name aborts the conversation with an error") {
    auto const message = json::object({
        {"role", "assistant"},
        {"tool_calls",
         json::array({json::object({
             {"id", "call_1"},
             {"type", "function"},
             {"function", json::object({{"name", "subtract"}, {"arguments", "{}"}})},
         })})},
    });
    test::MockHttpServer server{{{ "200 OK", "application/json", chatResponse(message) }}};
    auto const outcome = runConversation(mockProvider(server.port(), false), "算一下");
    require(outcome.has_value());
    require(!outcome->success);
    check(outcome->error.find("Function not found: subtract") != std::string::npos);
    check_eq(server.requestCount(), size_t{1});
}

TEST("the conversation gives up after eight tool-call rounds") {
    std::vector<test::MockResponse> responses{
        8, {"200 OK", "application/json", chatResponse(assistantToolCallMessage())}};
    test::MockHttpServer server{std::move(responses)};
    auto const outcome = runConversation(mockProvider(server.port(), false), "一直加");
    require(outcome.has_value());
    require(!outcome->success);
    check(outcome->error.find("Exceeded the maximum number of provider tool-call rounds")
          != std::string::npos);
    check_eq(server.requestCount(), size_t{8});
}

TEST("extra_body fields must not override request fields") {
    caf::actor_system_config cfg;
    cfg.load<caf::net::middleman>();
    caf::actor_system sys{cfg};
    auto* mpx = caf::net::multiplexer::from(sys);
    require(mpx != nullptr);
    auto provider = mockProvider(1, false);
    provider.extraBody = json::object({{"stream", true}});
    auto conversation = std::make_shared<agent::AgentConversation>(
        *mpx, std::move(provider), "hi", [](std::exception_ptr) {});
    check_throws<std::runtime_error>([&] { conversation->start(); });
}

} // SUITE("agent.conversation")
