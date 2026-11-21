#pragma once

#include "json.hpp"
#include <knotix/schema/message.h>
#include "knotix/schema/tool.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using FunctionHandler = std::function<JsonValue(JsonValue const& args)>;

struct FunctionDefinition {
    std::string                        name;
    std::string                        description;
    std::map<std::string, DataType>    parameters;
    std::vector<std::string>           requiredParams;
    FunctionHandler                    handler;

    JsonValue toSchema() const {
        std::map<std::string, JsonValue> schema;
        schema.emplace("name", name);
        schema["description"] = description;

        std::map<std::string, JsonValue> params;
        params["type"] = "object";
        // params["additionalProperties"] = false;
        std::map<std::string, JsonValue> properties;
        for (auto const& [paramName, paramType] : parameters) {
            std::map<std::string, JsonValue> property;
            property["type"] = std::string{to_string(paramType)};
            properties[paramName] = property;
        }
        params["properties"] = properties;
        params.emplace("required", requiredParams);

        schema["parameters"] = params;
        return schema;
    }
};

class FunctionRegistry {
private:
    std::map<std::string, FunctionDefinition> functions_;

    static bool matchesType(JsonValue const& value, DataType type) {
        switch (type) {
            case DataType::Null: return value.is_null();
            case DataType::Object: return value.is_object();
            case DataType::Number: return value.is_number();
            case DataType::Integer: return isIntegerValue(value);
            case DataType::String: return value.is_string();
            case DataType::Array: return value.is_array();
            case DataType::Boolean: return value.is_boolean();
        }
        return false;
    }

    static bool isIntegerValue(JsonValue const& value) {
        if (!value.is_number())
            return false;
        auto const number = value.get<double>();
        return std::isfinite(number) && std::trunc(number) == number;
    }

    static std::string_view typeOf(JsonValue const& value) {
        using ValueType = JsonValue::value_t;
        switch (value.type()) {
            case ValueType::null: return to_string(DataType::Null);
            case ValueType::object: return to_string(DataType::Object);
            case ValueType::number_integer:
            case ValueType::number_unsigned: return to_string(DataType::Integer);
            case ValueType::number_float:
                return isIntegerValue(value) ? to_string(DataType::Integer)
                                             : to_string(DataType::Number);
            case ValueType::string: return to_string(DataType::String);
            case ValueType::array: return to_string(DataType::Array);
            case ValueType::boolean: return to_string(DataType::Boolean);
            case ValueType::binary: return "binary";
            case ValueType::discarded: return "discarded";
        }
        return "unknown";
    }

public:
    void registerFunction(FunctionDefinition const& definition) {
        functions_.emplace(definition.name, definition);
    }

    bool hasFunction(std::string const& name) const {
        return functions_.count(name) > 0;
    }

    FunctionDefinition const& getFunction(std::string const& name) const {
        return functions_.at(name);
    }

    JsonValue getSchemas() const {
        std::vector<JsonValue> schemas;
        for (auto const& [name, definition] : functions_)
            schemas.push_back(definition.toSchema());
        return schemas;
    }

    JsonValue getTools() const {
        std::vector<JsonValue> tools;
        for (auto const& [name, definition] : functions_) {
            tools.emplace_back(std::map<std::string, JsonValue>{
                {"type", "function"},
                {"function", definition.toSchema()},
            });
        }
        return tools;
    }

    JsonValue call(std::string const& name, JsonValue const& args) const {
        if (!hasFunction(name)) {
            return std::map<std::string, JsonValue>{
                {"error", "Function not found: " + name}
            };
        }

        auto const& definition = getFunction(name);
        for (auto const& required : definition.requiredParams) {
            if (!args.contains(required)) {
                return std::map<std::string, JsonValue>{
                    {"error", "Missing required parameter: " + required}
                };
            }
        }

        for (auto const& [paramName, paramType] : definition.parameters) {
            if (!args.contains(paramName))
                continue;
            auto const& value = jsonGet(args, paramName);
            if (!matchesType(value, paramType)) {
                return std::map<std::string, JsonValue>{
                    {"error", "Invalid type for parameter '" + paramName + "' in function '"
                                  + name + "': expected "
                                  + std::string{to_string(paramType)} + ", got "
                                  + std::string{typeOf(value)}}
                };
            }
        }

        try {
            return definition.handler(args);
        } catch (std::exception const& e) {
            return std::map<std::string, JsonValue>{
                {"error", "Function call error in '" + name + "': " + e.what()}
            };
        }
    }

    JsonValue executeFromToolCall(std::string const& toolCallJson) const {
        try {
            return executeFromToolCall(JsonValue::parse(toolCallJson));
        } catch (std::exception const& e) {
            return std::map<std::string, JsonValue>{
                {"error", std::string{"Invalid tool-call JSON: "} + e.what()}
            };
        }
    }

    JsonValue executeFromToolCall(JsonValue const& toolCall) const {
        return call(jsonGet(toolCall, "name").get<std::string>(),
                    jsonGet(toolCall, "arguments"));
    }

    JsonValue executeOpenAIToolCall(JsonValue const& toolCall) const {
        try {
            auto const function = message::parseFunctionCall(jsonGet(toolCall, "function"));
            
            auto const& arguments = function.Arguments;
            JsonValue args;
            if (!arguments.empty()) {
                args = JsonValue::parse(arguments);
            }
            return call(function.Name, args);
        } catch (std::exception const& e) {
            return std::map<std::string, JsonValue>{
                {"error", std::string{"Invalid OpenAI tool call: "} + e.what()}
            };
        }
    }
};

// 示例
inline FunctionRegistry makeFunctionRegistry() {
    FunctionRegistry registry;
    registry.registerFunction({
        .name           = "add",
        .description    = "将两个数字相加",
        .parameters     = {{"a", DataType::Number}, {"b", DataType::Number}},
        .requiredParams = {"a", "b"},
        .handler        = [](JsonValue const& args) -> JsonValue {
            return jsonGet(args, "a").get<double>() + jsonGet(args, "b").get<double>();
        },
    });
    return registry;
}
