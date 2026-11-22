#include "sse_parser.hpp"

#include "check_json.hpp"

#include "caf/test/test.hpp"

#include <sstream>

using agent::OpenAICompatibleSseParser;

namespace {

// A parser that echoes streamed text into a stringstream instead of stdout.
struct TestParser {
    std::ostringstream out;
    OpenAICompatibleSseParser parser{out};

    void feed(std::string_view chunk) {
        parser.feed(chunk.data(), chunk.size());
    }

    // Feeds the stream one byte at a time to exercise partial-line handling.
    void feedBytewise(std::string_view stream) {
        for (char c : stream)
            parser.feed(&c, 1);
    }

    json done() {
        parser.finish();
        return parser.message();
    }
};

std::string contentEvent(std::string_view text) {
    return "data: " + json::object({
                          {"choices",
                           json::array({json::object({
                               {"delta", json::object({{"content", text}})},
                           })})},
                      }).dump()
           + "\n\n";
}

std::string toolCallEvent(json fragment) {
    return "data: " + json::object({
                          {"choices",
                           json::array({json::object({
                               {"delta", json::object({{"tool_calls",
                                                        json::array({std::move(fragment)})}})},
                           })})},
                      }).dump()
           + "\n\n";
}

} // namespace

SUITE("agent.sse_parser") {

TEST("content fragments accumulate into the assistant message") {
    TestParser test;
    test.feed(contentEvent("你好，"));
    test.feed(contentEvent("世界"));
    test.feed("data: [DONE]\n\n");
    auto const message = test.done();
    check_json_eq(jsonGet(message, "role"), json("assistant"));
    check_json_eq(jsonGet(message, "content"), json("你好，世界"));
    check(!message.contains("tool_calls"));
    check_eq(test.out.str(), "Assistant: 你好，世界\n");
}

TEST("partial lines are buffered until the line completes") {
    TestParser test;
    test.feedBytewise(contentEvent("a") + contentEvent("b") + "data: [DONE]\n\n");
    auto const message = test.done();
    check_json_eq(jsonGet(message, "content"), json("ab"));
}

TEST("tool-call deltas without content assemble a complete tool call") {
    TestParser test;
    // First fragment carries id and function name, no content field at all.
    test.feed(toolCallEvent(json::object({
        {"index", 0},
        {"id", "call_1"},
        {"function", json::object({{"name", "add"}})},
    })));
    // Arguments arrive split across several deltas.
    test.feed(toolCallEvent(
        json::object({{"index", 0}, {"function", json::object({{"arguments", "{\"a\":"}})}})));
    test.feed(toolCallEvent(
        json::object({{"index", 0}, {"function", json::object({{"arguments", "1,\"b\":2}"}})}})));
    test.feed("data: [DONE]\n\n");
    auto const message = test.done();
    check_json_eq(jsonGet(message, "content"), json(""));
    auto const& toolCalls = jsonGet(message, "tool_calls");
    require(toolCalls.is_array());
    require_eq(toolCalls.size(), size_t{1});
    auto const& call = toolCalls.front();
    check_json_eq(jsonGet(call, "id"), json("call_1"));
    check_json_eq(jsonGet(call, "type"), json("function"));
    check_json_eq(jsonGet(jsonGet(call, "function"), "name"), json("add"));
    check_json_eq(jsonGet(jsonGet(call, "function"), "arguments"), json("{\"a\":1,\"b\":2}"));
    // Nothing was printed: the stream carried no text.
    check(test.out.str().empty());
}

TEST("interleaved fragments group by their tool-call index") {
    TestParser test;
    test.feed(toolCallEvent(json::object({
        {"index", 0}, {"id", "call_a"}, {"function", json::object({{"name", "f0"}})},
    })));
    test.feed(toolCallEvent(json::object({
        {"index", 1}, {"id", "call_b"}, {"function", json::object({{"name", "f1"}})},
    })));
    test.feed(toolCallEvent(
        json::object({{"index", 1}, {"function", json::object({{"arguments", "{}"}})}})));
    test.feed(toolCallEvent(
        json::object({{"index", 0}, {"function", json::object({{"arguments", "{}"}})}})));
    test.feed("data: [DONE]\n\n");
    auto const message = test.done();
    auto const& toolCalls = jsonGet(message, "tool_calls");
    require(toolCalls.is_array());
    require_eq(toolCalls.size(), size_t{2});
    // Calls come out ordered by index.
    check_json_eq(jsonGet(toolCalls.at(0), "id"), json("call_a"));
    check_json_eq(jsonGet(toolCalls.at(1), "id"), json("call_b"));
    check_json_eq(jsonGet(jsonGet(toolCalls.at(1), "function"), "name"), json("f1"));
}

TEST("CRLF line endings and comment lines are handled") {
    TestParser test;
    test.feed(": ping\r\n\r\n");
    test.feed("event: message\r\n");
    auto event = contentEvent("hi");
    event.replace(event.find('\n'), 1, "\r\n");
    test.feed(event);
    test.feed("\r\ndata: [DONE]\r\n\r\n");
    check_json_eq(jsonGet(test.done(), "content"), json("hi"));
}

TEST("multi-line data fields are joined with newlines before parsing") {
    // A JSON event may span several data: lines. Joined with '\n' the text
    // still parses, because JSON permits whitespace between tokens.
    TestParser test;
    auto const joined = std::string{"data: {\"choices\": [{\"delta\": {}}\n"}
                        + "data: ]}\n\n" + "data: [DONE]\n\n";
    test.feed(joined);
    check(!test.done().contains("tool_calls"));
}

TEST("events with an empty choices array are ignored") {
    TestParser test;
    test.feed("data: {\"choices\": []}\n\n");
    test.feed(contentEvent("x"));
    test.feed("data: [DONE]\n\n");
    check_json_eq(jsonGet(test.done(), "content"), json("x"));
}

TEST("finish without a DONE event throws") {
    TestParser test;
    test.feed(contentEvent("x"));
    check_throws<std::runtime_error>([&] { test.parser.finish(); });
}

TEST("a trailing line without a newline is still processed") {
    TestParser test;
    test.feed(contentEvent("x"));
    test.feed("data: [DONE]"); // no trailing newline
    check_json_eq(jsonGet(test.done(), "content"), json("x"));
}

TEST("lines after DONE are ignored") {
    TestParser test;
    test.feed(contentEvent("x"));
    test.feed("data: [DONE]\n\n");
    test.feed(contentEvent("late"));
    check_json_eq(jsonGet(test.done(), "content"), json("x"));
}

TEST("an event without a choices array throws") {
    TestParser test;
    // The event dispatches as soon as its terminating blank line arrives.
    check_throws<std::runtime_error>(
        [&] { test.feed("data: {\"error\": \"oops\"}\n\n"); });
}

TEST("malformed JSON in a data field throws") {
    TestParser test;
    check_throws<json::exception>([&] { test.feed("data: {oops}\n\n"); });
}

TEST("message with an incomplete tool call throws") {
    TestParser test;
    // No id fragment ever arrives for index 0.
    test.feed(toolCallEvent(
        json::object({{"index", 0}, {"function", json::object({{"name", "add"}})}})));
    test.feed("data: [DONE]\n\n");
    test.parser.finish();
    check_throws<std::runtime_error>([&] { return test.parser.message(); });
}

TEST("an invalid tool-call index throws") {
    TestParser test;
    check_throws<std::runtime_error>([&] {
        test.feed(toolCallEvent(json::object({{"index", -1}})));
    });
    TestParser test2;
    check_throws<std::runtime_error>([&] {
        test2.feed(toolCallEvent(json::object({{"index", "zero"}})));
    });
}

} // SUITE("agent.sse_parser")
