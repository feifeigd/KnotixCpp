#pragma once

#include <knotix/json.hpp>
#include <stdexcept>
#include <string>

namespace openai {

struct ChatModelConfig {
    std::string api_key_env = "OPENAI_API_KEY";
    std::string APIKey;
};

inline void from_json(const json& j, ChatModelConfig& c) {
    if (j.contains("api_key_env")) {
        j.at("api_key_env").get_to(c.api_key_env);
    }

    auto const apiKey = std::getenv(c.api_key_env.c_str());
    if (apiKey == nullptr || *apiKey == '\0')
        throw std::runtime_error("ChatModelConfig requires environment variable '"
                                    + c.api_key_env + "'.");
    
    c.APIKey = apiKey;
}

} // namespace openai
