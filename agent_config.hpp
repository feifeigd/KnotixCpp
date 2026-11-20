#pragma once

#include "json.hpp"

#include <string>

namespace agent {

struct ProviderConfig {
    std::string name;
    std::string baseUrl;
    std::string chatCompletionsPath = "/chat/completions";
    std::string model;
    std::string apiKey;
    bool streaming = true;
    JsonValue extraBody{std::map<std::string, JsonValue>{}};

    std::string endpoint() const;
};

ProviderConfig loadProviderConfig();

} // namespace agent
