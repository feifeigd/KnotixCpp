#include "function_registry.hpp"

#include "caf/test/approx.hpp"
#include "check_json.hpp"

#include "caf/test/test.hpp"

using caf::test::approx;

namespace {

FunctionDefinition makeAdd() {
    return {
        .name           = "add",
        .description    = "将两个数字相加",
        .parameters     = {{"a", DataType::Number}, {"b", DataType::Number}},
        .requiredParams = {"a", "b"},
        .handler        = [](json const& args) -> json {
            return jsonGet(args, "a").get<double>() + jsonGet(args, "b").get<double>();
        },
    };
}

FunctionRegistry makeRegistry() {
    FunctionRegistry registry;
    registry.registerFunction(makeAdd());
    registry.registerFunction({
        .name           = "greet",
        .description    = "按名字打招呼",
        .parameters     = {{"name", DataType::String},
                           {"times", DataType::Integer},
                           {"loud", DataType::Boolean}},
        .requiredParams = {"name"},
        .handler        = [](json const& args) -> json {
            return "hello, " + jsonGet(args, "name").get<std::string>();
        },
    });
    registry.registerFunction({
        .name           = "fail",
        .description    = "总是抛出异常",
        .parameters     = {},
        .requiredParams = {},
        .handler        = [](json const&) -> json {
            throw std::runtime_error("boom");
        },
    });
    return registry;
}

} // namespace

SUITE("function_registry") {

TEST("toSchema describes the function with a JSON Schema object") {
    auto const schema = makeAdd().toSchema();
    check_json_eq(jsonGet(schema, "name"), json("add"));
    check_json_eq(jsonGet(schema, "description"), json("将两个数字相加"));

    auto const& params = jsonGet(schema, "parameters");
    require(params.is_object());
    check_json_eq(jsonGet(params, "type"), json("object"));
    check_json_eq(jsonGet(jsonGet(params, "properties"), "a"),
             json::object({{"type", "number"}}));
    check_json_eq(jsonGet(jsonGet(params, "properties"), "b"),
             json::object({{"type", "number"}}));
    check_json_eq(jsonGet(params, "required"), json::array({"a", "b"}));
}

TEST("toSchema marks every DataType with its JSON name") {
    auto const schema = makeRegistry().getFunction("greet").toSchema();
    auto const& properties = jsonGet(jsonGet(schema, "parameters"), "properties");
    check_json_eq(jsonGet(properties, "name"), json::object({{"type", "string"}}));
    check_json_eq(jsonGet(properties, "times"), json::object({{"type", "integer"}}));
    check_json_eq(jsonGet(properties, "loud"), json::object({{"type", "boolean"}}));
    check_json_eq(jsonGet(jsonGet(schema, "parameters"), "required"), json::array({"name"}));
}

TEST("getSchemas and getTools wrap definitions for the Chat Completions API") {
    auto const registry = makeRegistry();
    auto const schemas = registry.getSchemas();
    require(schemas.is_array());
    check_eq(schemas.size(), size_t{3});

    auto const tools = registry.getTools();
    require(tools.is_array());
    require_eq(tools.size(), size_t{3});
    for (auto const& tool : tools) {
        check_json_eq(jsonGet(tool, "type"), json("function"));
        check(jsonGet(tool, "function").is_object());
    }
    // Functions are stored in a map, so iteration order is alphabetical.
    check_json_eq(jsonGet(jsonGet(tools.at(0), "function"), "name"), json("add"));
    check_json_eq(jsonGet(jsonGet(tools.at(1), "function"), "name"), json("fail"));
    check_json_eq(jsonGet(jsonGet(tools.at(2), "function"), "name"), json("greet"));
}

TEST("hasFunction and getFunction reflect registration") {
    auto const registry = makeRegistry();
    check(registry.hasFunction("add"));
    check(!registry.hasFunction("subtract"));
    check_eq(registry.getFunction("add").name, "add");
    check_throws<std::out_of_range>(
        [&] { static_cast<void>(registry.getFunction("subtract")); });
}

TEST("call invokes the handler with valid arguments") {
    auto const registry = makeRegistry();
    auto const result = registry.call("add", json::object({{"a", 1}, {"b", 2}}));
    require(result.is_number());
    check_eq(result.get<double>(), approx(3.0));
}

TEST("call reports an unknown function") {
    auto const registry = makeRegistry();
    auto const result = registry.call("subtract", json::object());
    check_json_eq(jsonGet(result, "error"), json("Function not found: subtract"));
}

TEST("call reports the first missing required parameter") {
    auto const registry = makeRegistry();
    auto const result = registry.call("add", json::object({{"a", 1}}));
    check_json_eq(jsonGet(result, "error"), json("Missing required parameter: b"));
}

TEST("call reports a type mismatch with expected and actual type names") {
    auto const registry = makeRegistry();
    auto const result = registry.call("greet", json::object({{"name", 42}}));
    check_json_eq(jsonGet(result, "error"),
             json("Invalid type for parameter 'name' in function 'greet': expected "
                  "string, got integer"));
}

TEST("call accepts integral-valued floats for integer parameters") {
    auto const registry = makeRegistry();
    auto ok = registry.call("greet",
                            json::object({{"name", "kimi"}, {"times", 2.0}}));
    check(!ok.contains("error"));
    auto bad = registry.call("greet",
                             json::object({{"name", "kimi"}, {"times", 2.5}}));
    check_json_eq(jsonGet(bad, "error"),
             json("Invalid type for parameter 'times' in function 'greet': expected "
                  "integer, got number"));
}

TEST("call ignores extra arguments not declared as parameters") {
    auto const registry = makeRegistry();
    auto const result = registry.call("greet",
                                      json::object({{"name", "kimi"}, {"extra", true}}));
    check_json_eq(result, json("hello, kimi"));
}

TEST("call converts handler exceptions into error objects") {
    auto const registry = makeRegistry();
    auto const result = registry.call("fail", json::object());
    check_json_eq(jsonGet(result, "error"), json("Function call error in 'fail': boom"));
}

TEST("executeFromToolCall dispatches on a parsed tool-call object") {
    auto const registry = makeRegistry();
    auto const toolCall = json::object({
        {"name", "add"},
        {"arguments", json::object({{"a", 2}, {"b", 40}})},
    });
    auto const result = registry.executeFromToolCall(toolCall);
    require(result.is_number());
    check_eq(result.get<double>(), approx(42.0));
}

TEST("executeFromToolCall parses a JSON-encoded string") {
    auto const registry = makeRegistry();
    auto const result = registry.executeFromToolCall(
        std::string{R"({"name":"greet","arguments":{"name":"kimi"}})"});
    check_json_eq(result, json("hello, kimi"));
}

TEST("executeFromToolCall reports malformed JSON input") {
    auto const registry = makeRegistry();
    auto const result = registry.executeFromToolCall(std::string{"{not json"});
    require(result.is_object());
    auto const& error = jsonGet(result, "error");
    require(error.is_string());
    check(error.get<std::string>().starts_with("Invalid tool-call JSON: "));
}

TEST("executeOpenAIToolCall unwraps the function object and string arguments") {
    auto const registry = makeRegistry();
    auto const toolCall = json::object({
        {"id", "call_1"},
        {"type", "function"},
        {"function", json::object({
            {"name", "add"},
            {"arguments", R"({"a":1,"b":2})"},
        })},
    });
    auto const result = registry.executeOpenAIToolCall(toolCall);
    require(result.is_number());
    check_eq(result.get<double>(), approx(3.0));
}

TEST("executeOpenAIToolCall treats empty arguments as an empty object") {
    FunctionRegistry registry;
    registry.registerFunction({
        .name           = "noop",
        .description    = "无参数",
        .parameters     = {},
        .requiredParams = {},
        .handler        = [](json const& args) -> json { return args; },
    });
    auto const toolCall = json::object({
        {"function", json::object({{"name", "noop"}, {"arguments", ""}})},
    });
    check_json_eq(registry.executeOpenAIToolCall(toolCall), json::object());
}

TEST("executeOpenAIToolCall reports invalid arguments JSON") {
    auto const registry = makeRegistry();
    auto const toolCall = json::object({
        {"function", json::object({{"name", "add"}, {"arguments", "{oops}"}})},
    });
    auto const result = registry.executeOpenAIToolCall(toolCall);
    require(result.is_object());
    auto const& error = jsonGet(result, "error");
    require(error.is_string());
    check(error.get<std::string>().starts_with("Invalid OpenAI tool call: "));
}

TEST("executeOpenAIToolCall reports a malformed tool-call object") {
    auto const registry = makeRegistry();
    // Missing "function" field: FunctionCall parsing must fail gracefully.
    auto const result = registry.executeOpenAIToolCall(json::object());
    require(result.is_object());
    check(jsonGet(result, "error").is_string());
}

TEST("makeFunctionRegistry registers the example add function") {
    auto const registry = makeFunctionRegistry();
    require(registry.hasFunction("add"));
    auto const result = registry.call("add", json::object({{"a", 0.5}, {"b", 0.25}}));
    require(result.is_number());
    check_eq(result.get<double>(), approx(0.75));
}

} // SUITE("function_registry")
