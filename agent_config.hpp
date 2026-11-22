#pragma once

#include "knotix/json.hpp"

#include <string>

namespace agent {

struct ProviderConfig {
    std::string name;
    std::string baseUrl;
    std::string chatCompletionsPath = "/chat/completions";
    std::string model;
    std::string apiKey;
    bool streaming = true;
    json extraBody = json::object();

    std::string endpoint() const;
};

ProviderConfig loadProviderConfig();

} // namespace agent
