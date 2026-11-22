#include "knotix/schema/message.h"

#include "check_json.hpp"

#include "caf/test/test.hpp"

using message::FunctionCall;
using message::Message;
using message::RoleType;

SUITE("knotix.schema.message") {

TEST("RoleType serializes to the OpenAI role names") {
    check_json_eq(json(RoleType::Assistant), json("assistant"));
    check_json_eq(json(RoleType::User), json("user"));
    check_json_eq(json(RoleType::System), json("system"));
    check_json_eq(json(RoleType::Tool), json("tool"));
}

TEST("RoleType round-trips through JSON") {
    for (auto role : {RoleType::Assistant, RoleType::User, RoleType::System, RoleType::Tool})
        check_json_eq(json(role).get<RoleType>(), role);
}

TEST("FunctionCall parses name and arguments from a tool-call object") {
    auto const raw = json::object({
        {"name", "add"},
        {"arguments", R"({"a":1,"b":2})"},
    });
    auto const call = raw.get<FunctionCall>();
    check_eq(call.Name, "add");
    check_eq(call.Arguments, R"({"a":1,"b":2})");
    // Arguments stay a JSON-encoded string that parses independently.
    check_eq(json::parse(call.Arguments), json::object({{"a", 1}, {"b", 2}}));
}

TEST("FunctionCall ignores unrelated fields of the tool-call object") {
    auto const raw = json::object({
        {"id", "call_1"},
        {"type", "function"},
        {"name", "noop"},
        {"arguments", ""},
    });
    auto const call = raw.get<FunctionCall>();
    check_eq(call.Name, "noop");
    check(call.Arguments.empty());
}

TEST("FunctionCall throws when a required field is missing") {
    check_throws<std::exception>([] {
        return json::object({{"arguments", "{}"}}).get<FunctionCall>();
    });
    check_throws<std::exception>([] {
        return json::object({{"name", "add"}}).get<FunctionCall>();
    });
}

TEST("Message serializes role and content") {
    auto const value = json(Message{RoleType::User, "你好"});
    check_json_eq(value, json::object({{"role", "user"}, {"content", "你好"}}));
}

TEST("Message parses role and content") {
    auto const parsed = json::object({{"role", "assistant"}, {"content", "hi"}})
                            .get<Message>();
    check(parsed.Role == RoleType::Assistant);
    check_eq(parsed.Content, "hi");
}

TEST("Message round-trips through JSON") {
    auto const original = Message{RoleType::Tool, R"({"result":42})"};
    auto const restored = json(original).get<Message>();
    check(restored.Role == original.Role);
    check_eq(restored.Content, original.Content);
}

} // SUITE("knotix.schema.message")
