#pragma once

#include "knotix/json.hpp"
#include <knotix/schema/message.h>
#include "knotix/schema/tool.h"

#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using FunctionHandler = std::function<json(json const& args)>;

struct FunctionDefinition {
    std::string                        name;
    std::string                        description;
    std::map<std::string, DataType>    parameters;
    std::vector<std::string>           requiredParams;
    FunctionHandler                    handler;

    json toSchema() const {
        auto schema = json::object({
            {"name", name},
            {"description", description},
        });

        std::map<std::string, json> params;
        params["type"] = "object";
        // params["additionalProperties"] = false;
        std::map<std::string, json> properties;
        for (auto const& [paramName, paramType] : parameters) {
            properties[paramName] = json::object({
                {"type", paramType},
            });
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

    static bool matchesType(json const& value, DataType type) {
        switch (type) {
            case DataType::Null: return value.is_null();
            case DataType::Object: return value.is_object();
            case DataType::Number: return value.is_number();
            case DataType::Integer: return knotix::isIntegerValue(value);
            case DataType::String: return value.is_string();
            case DataType::Array: return value.is_array();
            case DataType::Boolean: return value.is_boolean();
        }
        return false;
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

    json getSchemas() const {
        auto schemas = json::array();
        for (auto const& [name, definition] : functions_)
            schemas.push_back(definition.toSchema());
        return schemas;
    }

    json getTools() const {
        auto tools = json::array();
        for (auto const& entry : functions_) {
            tools.push_back(json::object({
                {"type", "function"},
                {"function", entry.second.toSchema()},
            }));
        }
        return tools;
    }

    json call(std::string const& name, json const& args) const {
        if (!hasFunction(name)) {
            return json::object({
                {"error", "Function not found: " + name},
            });
        }

        auto const& definition = getFunction(name);
        for (auto const& required : definition.requiredParams) {
            if (!args.contains(required)) {
                return json::object({
                    {"error", "Missing required parameter: " + required}
                });
            }
        }

        for (auto const& [paramName, paramType] : definition.parameters) {
            if (!args.contains(paramName))
                continue;
            auto const& value = jsonGet(args, paramName);
            if (!matchesType(value, paramType)) {
                return json::object({
                    {"error", "Invalid type for parameter '" + paramName + "' in function '"
                                  + name + "': expected "
                                  + std::string{knotix::dataTypeName(paramType)} + ", got "
                                  + std::string{knotix::typeOf(value)}}
                });
            }
        }

        try {
            return definition.handler(args);
        } catch (std::exception const& e) {
            return json::object({
                {"error", "Function call error in '" + name + "': " + e.what()}
            });
        }
    }

    json executeFromToolCall(std::string const& toolCallJson) const {
        try {
            return executeFromToolCall(json::parse(toolCallJson));
        } catch (std::exception const& e) {
            return json::object({
                {"error", std::string{"Invalid tool-call JSON: "} + e.what()}
            });
        }
    }

    json executeFromToolCall(json const& toolCall) const {
        return call(jsonGet(toolCall, "name").get<std::string>(),
                    jsonGet(toolCall, "arguments"));
    }

    json executeOpenAIToolCall(json const& toolCall) const {
        try {
            auto const function = jsonGet(toolCall, "function").get<message::FunctionCall>();
            
            auto const& arguments = function.Arguments;
            auto args = json::object();
            if (!arguments.empty()) {
                args = json::parse(arguments);
            }
            return call(function.Name, args);
        } catch (std::exception const& e) {
            return json::object({
                {"error", std::string{"Invalid OpenAI tool call: "} + e.what()}
            });
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
        .handler        = [](json const& args) -> json {
            return jsonGet(args, "a").get<double>() + jsonGet(args, "b").get<double>();
        },
    });
    return registry;
}
