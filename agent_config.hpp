#pragma once

#include <knotix/ext/components/model/openai/chatmodel.h>
#include <knotix/json.hpp>

#include <string>

namespace agent {

struct ProviderConfig {
    std::string name;
    std::string baseUrl;
    std::string chatCompletionsPath = "/chat/completions";
    std::string model;
    
    bool streaming = true;
    json extraBody = json::object();

    std::string endpoint() const;

    openai::ChatModelConfig chatModelConfig;
};

void from_json(json const& value, ProviderConfig& config);

ProviderConfig loadProviderConfig();

} // namespace agent
