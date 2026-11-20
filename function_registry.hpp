#pragma once

#include "json.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

using FunctionHandler = std::function<JsonValue(JsonValue const& args)>;

struct FunctionDefinition {
    std::string                        name;
    std::string                        description;
    std::map<std::string, std::string> parameters;
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
            property["type"] = paramType;
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

    static bool matchesType(JsonValue const& value, std::string const& type) {
        if (type == "object")
            return value.isObject();
        if (type == "array")
            return value.isArray();
        if (type == "string")
            return value.isString();
        if (type == "number" || type == "integer")
            return value.isNumber();
        if (type == "boolean")
            return value.isBool();
        if (type == "null")
            return value.isNull();
        return true;
    }

    static std::string typeOf(JsonValue const& value) {
        if (value.isObject())
            return "object";
        if (value.isArray())
            return "array (" + std::to_string(value.asArray().size()) + " elements)";
        if (value.isString())
            return "string";
        if (value.isNumber())
            return "number";
        if (value.isBool())
            return "boolean";
        return "null";
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
            if (!args.hasKey(required)) {
                return std::map<std::string, JsonValue>{
                    {"error", "Missing required parameter: " + required}
                };
            }
        }

        for (auto const& [paramName, paramType] : definition.parameters) {
            if (!args.hasKey(paramName))
                continue;
            auto const& value = args[paramName];
            if (!matchesType(value, paramType)) {
                return std::map<std::string, JsonValue>{
                    {"error", "Invalid type for parameter '" + paramName + "' in function '"
                                  + name + "': expected " + paramType + ", got "
                                  + typeOf(value)}
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
            JsonParser parser{toolCallJson};
            return executeFromToolCall(parser.parse());
        } catch (std::exception const& e) {
            return std::map<std::string, JsonValue>{
                {"error", std::string{"Invalid tool-call JSON: "} + e.what()}
            };
        }
    }

    JsonValue executeFromToolCall(JsonValue const& toolCall) const {
        return call(toolCall["name"].asString(), toolCall["arguments"]);
    }

    JsonValue executeOpenAIToolCall(JsonValue const& toolCall) const {
        try {
            auto const& function = toolCall["function"];
            auto name = function["name"].asString();
            auto const& arguments = function["arguments"];
            JsonValue args;
            if (arguments.isString()) {
                auto text = arguments.asString();
                if (text.empty())
                    throw std::runtime_error("Empty arguments for function '" + name
                                             + "': " + toolCall.toString());
                JsonParser parser{text};
                args = parser.parse();
            } else if (arguments.isObject()) {
                args = arguments;
            } else {
                throw std::runtime_error("Arguments for function '" + name
                                         + "' must be a JSON string or object: "
                                         + toolCall.toString());
            }
            return call(name, args);
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
        .parameters     = {{"a", "number"}, {"b", "number"}},
        .requiredParams = {"a", "b"},
        .handler        = [](JsonValue const& args) -> JsonValue {
            return args["a"].asNumber() + args["b"].asNumber();
        },
    });
    return registry;
}
