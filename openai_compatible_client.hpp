#pragma once

#include "agent_config.hpp"

#include <caf/net/multiplexer.hpp>

#include <exception>
#include <functional>
#include <string>

namespace agent {

class CurlRuntime {
public:
    CurlRuntime();
    ~CurlRuntime();

    CurlRuntime(CurlRuntime const&) = delete;
    CurlRuntime& operator=(CurlRuntime const&) = delete;
};

using ChatCompletionCallback = std::function<void(json, std::string)>;

void requestChatCompletion(caf::net::multiplexer& mpx, ProviderConfig const& provider,
                           std::string payload, ChatCompletionCallback completion);

} // namespace agent
