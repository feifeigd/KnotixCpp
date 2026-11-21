#pragma once

#include "../../json.hpp"

namespace message {

enum class RoleType {
    Assistang,
    User,
    System,
    Tool,
};

// FunctionCall is the function call in a message.
// It's used in Assistant Message.
struct FunctionCall {
    std::string Name; // name
    std::string Arguments; // arguments
};

inline FunctionCall const parseFunctionCall(JsonValue const& value) {
    if (!value.isObject())
        throw std::invalid_argument("FunctionCall must be a JSON object");

    auto const& name = value["name"];
    auto const& arguments = value["arguments"];

    if (!name.isString())
        throw std::invalid_argument("FunctionCall.name must be a string");

    std::string args;
    if (arguments.isString())
        args = arguments.asString(); // 常见的 OpenAI 格式
    else if (arguments.isObject() || arguments.isArray())
        args = arguments.toString(); // 若 arguments 已经是 JSON 值，序列化后存入 string
    else
        throw std::invalid_argument("FunctionCall.arguments must be a string, object, or array");

    return {.Name = name.asString(), .Arguments = std::move(args)};
}

}