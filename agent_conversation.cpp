#include "agent_conversation.hpp"

#include "openai_compatible_client.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

namespace agent {
namespace {

JsonValue makeRequest(ProviderConfig const& provider,
                      std::vector<JsonValue> const& messages, JsonValue const& tools) {
    std::map<std::string, JsonValue> request{
        {"model",       provider.model},
        {"messages",    messages},
        {"tools",       tools},
        {"tool_choice", "auto"},
        {"stream",      provider.streaming},
    };
    for (auto const& [key, value] : provider.extraBody.asObject()) {
        if (request.count(key) > 0)
            throw std::runtime_error("Provider extra_body cannot override request field '"
                                     + key + "'.");
        request.emplace(key, value);
    }
    return request;
}

} // namespace

AgentConversation::AgentConversation(caf::net::multiplexer& mpx, ProviderConfig provider,
                                     std::string prompt, completion_type completion)
    : mpx_{mpx},
      provider_{std::move(provider)},
      registry_{makeFunctionRegistry()},
      tools_{registry_.getTools()},
      completion_{std::move(completion)} {
    messages_.emplace_back(std::map<std::string, JsonValue>{
        {"role", "system"},
        {"content", "调用工具时必须严格遵循工具的 JSON Schema。"},
    });
    messages_.emplace_back(std::map<std::string, JsonValue>{
        {"role", "user"},
        {"content", std::move(prompt)},
    });
}

void AgentConversation::start() {
    requestNext();
}

void AgentConversation::requestNext() {
    constexpr size_t maxToolRounds = 8;
    if (requestCount_ >= maxToolRounds)
        throw std::runtime_error("Exceeded the maximum number of provider tool-call rounds.");

    auto payload = makeRequest(provider_, messages_, tools_).toString();
    ++requestCount_;
    std::weak_ptr<AgentConversation> weakSelf = shared_from_this();
    requestChatCompletion(
        mpx_, provider_, std::move(payload),
        [weakSelf](JsonValue message, std::string error) {
            if (auto self = weakSelf.lock()) {
                try {
                    if (!error.empty())
                        throw std::runtime_error(std::move(error));
                    self->handleResponse(std::move(message));
                } catch (...) {
                    self->finish(std::current_exception());
                }
            }
        });
}

// LLM 的回复
void AgentConversation::handleResponse(JsonValue message) {
    auto const& toolCalls = message["tool_calls"];
    if (!toolCalls.isArray() || toolCalls.asArray().empty()) {
        if (!provider_.streaming) {
            auto const& content = message["content"];
            if (content.isString())
                std::cout << "Assistant: " << content.asString() << '\n';
            else
                std::cout << "Assistant: " << message.toString() << '\n';
        }
        finish();
        return;
    }

    messages_.push_back(message);
    for (auto const& toolCall : toolCalls.asArray()) {
        auto const& id = toolCall["id"].asString();
        auto result = registry_.executeOpenAIToolCall(toolCall);
        if (result.isObject() && result.hasKey("error"))
            throw std::runtime_error(result["error"].asString());
        messages_.emplace_back(std::map<std::string, JsonValue>{
            {"role",         "tool"},
            {"tool_call_id", id},
            {"content",      result.toString()},
        });
    }
    requestNext();
}

void AgentConversation::finish(std::exception_ptr error) {
    if (finished_)
        return;
    finished_ = true;
    auto completion = std::move(completion_);
    if (completion)
        completion(std::move(error));
}

} // namespace agent
