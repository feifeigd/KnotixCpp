#pragma once

#include "agent_config.hpp"
#include "function_registry.hpp"

#include <caf/net/multiplexer.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {

class AgentConversation : public std::enable_shared_from_this<AgentConversation> {
public:
    using completion_type = std::function<void(std::exception_ptr)>;

    AgentConversation(caf::net::multiplexer& mpx, ProviderConfig provider, std::string prompt,
                      completion_type completion);

    void start();

private:
    void requestNext();
    void handleResponse(json message);
    void finish(std::exception_ptr error = {});

    caf::net::multiplexer& mpx_;
    ProviderConfig provider_;
    FunctionRegistry registry_;
    json tools_;
    std::vector<json> messages_;
    completion_type completion_;
    size_t requestCount_ = 0;
    bool finished_ = false;
};

} // namespace agent
