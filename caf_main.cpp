

#include "json.hpp"

#include <caf/actor_system.hpp>
#include <caf/caf_main.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/typed_actor.hpp>
#include <caf/typed_event_based_actor.hpp>

#include <chrono>
#include <string>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace caf;
using namespace std::literals;

CAF_BEGIN_TYPE_ID_BLOCK(function_calling, first_custom_type_id)
    CAF_ADD_ATOM(function_calling, call_request_atom)
    CAF_ADD_ATOM(function_calling, call_result_atom)
    CAF_ADD_TYPE_ID(function_calling, (JsonValue), "JsonValue")
CAF_END_TYPE_ID_BLOCK(function_calling)

using FunctionActor = typed_actor<result<JsonValue>(call_request_atom, JsonValue)>;

FunctionActor::behavior_type add_actor(FunctionActor::pointer){
    return {
        [](call_request_atom, JsonValue args) -> JsonValue {
            return std::map<std::string, JsonValue>{{"result", 6.0}};
        }
    };
}

behavior dispatcher(event_based_actor* self){
    // 函数注册: 名称-> Actor 句柄
    auto functions = std::make_shared<std::map<std::string, FunctionActor>>();
    (*functions)["add"] = self->spawn(add_actor);

    return {
        [=](std::string const& func_name, std::string const& args_json) -> result<JsonValue> {
            auto it = functions->find(func_name);
            if (functions->end() == it)
                return make_error(sec::runtime_error, "Function not found: " + func_name);

            JsonValue args;
            try {
                JsonParser parser{args_json};
                args = parser.parse();
            } catch (std::exception const& e) {
                return make_error(sec::runtime_error, "Invalid JSON arguments: " + std::string{e.what()});
            }

            auto rp = self->make_response_promise<JsonValue>();

            self->request(it->second, std::chrono::seconds(5), call_request_atom_v, args)
                .then(
                    [rp](JsonValue const& result) mutable {
                        rp.deliver(result);
                    },
                    [rp](error const& err)mutable{
                        rp.deliver(make_error(sec::runtime_error, "Function callfailed " + to_string(err)));
                    }
                );

            return rp;
        }
    };
}

behavior mirror(event_based_actor *self) {
    return {
        [self](const std::string &text) -> std::string {
            self->println("{}", text);
            return std::string{text.rbegin(), text.rend()};
        },
    };
}

void hello_world(event_based_actor *self, const actor &buddy) {
    self->mail("Hello World!")
        .request(buddy, 10s)
        .then([self](const std::string &text) { self->println("{}", text); });
}

void caf_main(actor_system &sys) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // auto mirror_actor = system.spawn(mirror);
    // system.spawn(hello_world, mirror_actor);
    auto disp = sys.spawn(dispatcher);
    scoped_actor self{sys};
    
    // 模拟 LLM 的调用请求
    std::vector<std::pair<std::string, std::string>> calls = {
        {"add", R"({"a": 3.5, "b": 2.5})"},
        {"get_weather", R"({"city": "Beijing"})"},
        {"unknown_func", R"({})"}
    };
    
    for (auto const& call : calls) {
        self->request(disp, std::chrono::seconds(10), call.first, call.second)
            .receive(
                [&](const JsonValue& result) {
                    std::cout << "调用 " << call.first << " 成功: " << result.toString() << std::endl;
                },
                [&](const error& err) {
                    std::cout << "调用 " << call.first << " 失败: " << to_string(err) << std::endl;
                }
            );
    }
}

CAF_MAIN(caf::id_block::function_calling)
