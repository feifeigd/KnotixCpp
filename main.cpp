#include "json.hpp"

#include <functional>
#include <print>

using FunctionHandler = std::function<JsonValue(JsonValue const &args)>;

struct FunctionDefinition {
    std::string                        name;
    std::string                        description;
    std::map<std::string, std::string> parameters; // 参数名 -> 类型描述 number|
    std::vector<std::string>           requiredParams;
    FunctionHandler                    handler;

    JsonValue toSchema() const {
        std::map<std::string, JsonValue> schema;
        schema.emplace("name", name);
        schema["description"] = description;

        std::map<std::string, JsonValue> params;
        params["type"] = "object";
        std::map<std::string, JsonValue> properties;
        for (auto const &[pname, ptype] : parameters) {
            std::map<std::string, JsonValue> prop;
            prop["type"]      = ptype;
            properties[pname] = prop;
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

  public:
    void registerFunction(FunctionDefinition const &def) {
        functions_.emplace(def.name, def);
    }

    bool hasFunction(std::string const &name) const {
        return functions_.count(name) > 0;
    }

    FunctionDefinition const &getFunction(std::string const &name) const {
        return functions_.at(name);
    }

    // 获取所有函数定义，用于传给LLM的 schema
    /*
    [
        {
            "name": "函数名",
            "description": "函数描述",
            "parameters": {
                "type": "object",
                "properties": {
                    "参数名": {
                        "type": "参数类型", //
                        “description": "参数描述",
                    },
                }
            }
        },
        ...
    ]
    */
    JsonValue getSchemas() const {
        std::vector<JsonValue> schemas;
        for (auto const &[_name, def] : functions_) {
            schemas.push_back(def.toSchema());
        }
        return schemas;
    }

    JsonValue call(std::string const& name, JsonValue const& args)const{
        if(!hasFunction(name)){
            std::map<std::string, JsonValue> err;
            err.emplace("error", "Function not found: " + name);
            return err;
        }

        auto const& def = getFunction(name);
        for(auto const& req : def.requiredParams){
            if(!args.hasKey(req)){
                std::map<std::string, JsonValue> err;
                err.emplace("error", "Missing required parameter: " + req);
                return err;
            }
        }

        try{
            return def.handler(args);
        }catch(std::exception const& e){
            std::map<std::string, JsonValue> err;
            err.emplace("error", std::string{"Function call error: "} + e.what());
            return err;
        }
    }

    JsonValue executeFromToolCall(std::string const& toolCall_json)const{
        try {
            JsonParser parser{toolCall_json};
            return executeFromToolCall(parser.parse());
        } catch (std::exception const& e) {
            return std::map<std::string, JsonValue>{
                {"error", std::string{"Invalid tool-call JSON: "} + e.what()}
            };
        }
    }

    // 从 LLM 的 function call 响应中执行
    // 支持 OpenAI 格式：tool_call 通常是 {"function":{"name":...,"arguments":"<json string>"}}，注意 arguments 是字符串需二次解析
    JsonValue executeFromToolCall(JsonValue const& toolCall)const{
        return call(toolCall["name"].asString(), toolCall["arguments"]);
    }

    /*
    如果对接 OpenAI API，实际返回格式是：
    {
        "tool_calls": [{
            "id": "call_abc",
            "type": "function",
            "function": {
            "name": "add",
            "arguments": "{\"a\":3.5,\"b\":2.5}"
            }
        }]
    }
    */
    JsonValue executeOpenAIToolCall(const JsonValue& toolCall) {
        try {
            auto& func = toolCall["function"];
            std::string name = func["name"].asString();
            // arguments 是字符串，需要二次解析
            JsonParser parser(func["arguments"].asString());
            JsonValue args = parser.parse();
            return call(name, args);
        } catch (std::exception const& e) {
            return std::map<std::string, JsonValue>{
                {"error", std::string{"Invalid OpenAI tool call: "} + e.what()}
            };
        }
    }
};

int main() {
    {
        JsonValue value(1);
        value.asNumber();
    }
    {
        JsonValue value{1};
        value.asArray();
    }

    FunctionRegistry registry;
    registry.registerFunction({
        .name           = "add",
        .description    = std::string{"将两个数字相加"},
        .parameters     = {{"a", "number"}, {"b", "number"}},
        .requiredParams = {"a", "b"},
        .handler        = [](JsonValue const& args) -> JsonValue {
            return args["a"].asNumber() + args["b"].asNumber();
        },
    });
    auto str = registry.getSchemas().toString();

    std::println("str={}", str);
    return 0;
}
