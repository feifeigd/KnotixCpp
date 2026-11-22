#include "agent_conversation.hpp"
#include "agent_config.hpp"
#include "openai_compatible_client.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <caf/actor_system.hpp>
#include <caf/caf_main.hpp>
#include <caf/net/middleman.hpp>
#include <caf/net/multiplexer.hpp>

#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

CAF_BEGIN_TYPE_ID_BLOCK(function_calling, first_custom_type_id)
    CAF_ADD_ATOM   (function_calling, call_request_atom)
    CAF_ADD_ATOM   (function_calling, call_result_atom)
    CAF_ADD_TYPE_ID(function_calling, (json), "json")
CAF_END_TYPE_ID_BLOCK(function_calling)

namespace {

int runAgent(caf::actor_system& sys) {
    auto provider = agent::loadProviderConfig();
    agent::CurlRuntime curlRuntime; // 初始化 curl

    std::string prompt;
    std::cout << "请输入问题：";
    if (!std::getline(std::cin, prompt) || prompt.empty()) {
        std::cerr << "问题不能为空。\n";
        return EXIT_FAILURE;
    }

    auto* mpx = caf::net::multiplexer::from(sys);
    if (mpx == nullptr)
        throw std::runtime_error("CAF network multiplexer is unavailable.");
    std::promise<void> completion;
    auto result = completion.get_future();
    auto conversation = std::make_shared<agent::AgentConversation>(
        *mpx, std::move(provider), std::move(prompt),
        [&completion](std::exception_ptr error) {
            if (error)
                completion.set_exception(std::move(error));
            else
                completion.set_value();
        });
    conversation->start();
    result.get();
    return EXIT_SUCCESS;
}

} // namespace

int caf_main(caf::actor_system& sys) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    try {
        return runAgent(sys);
    } catch (std::exception const& e) {
        std::cerr << "Agent 请求失败: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

CAF_MAIN(caf::net::middleman, caf::id_block::function_calling)
