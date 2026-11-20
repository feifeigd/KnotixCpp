#include "agent_config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace agent {
namespace {

std::string requireConfigString(JsonValue const& object, std::string const& key,
                                std::string const& context) {
    auto const& value = object[key];
    if (!value.isString() || value.asString().empty())
        throw std::runtime_error(context + " requires a non-empty string '" + key + "'.");
    return value.asString();
}

} // namespace

std::string ProviderConfig::endpoint() const {
    auto base = baseUrl;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base + (chatCompletionsPath.front() == '/'
                       ? chatCompletionsPath
                       : "/" + chatCompletionsPath);
}

ProviderConfig loadProviderConfig() {
    auto configPath = std::getenv("AGENT_CONFIG");
    auto const path = configPath != nullptr && *configPath != '\0'
                          ? std::string{configPath}
                          : std::string{"agent.json"};
    std::ifstream input{path, std::ios::binary};
    if (!input)
        throw std::runtime_error("Unable to open agent config file '" + path + "'.");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
        throw std::runtime_error("Failed to read agent config file '" + path + "'.");

    auto configText = contents.str();
    JsonParser parser{configText};
    auto config = parser.parse();
    if (!config.isObject())
        throw std::runtime_error("Agent config must be a JSON object.");
    auto const profileName = requireConfigString(config, "active_provider", "Agent config");
    auto const& providers = config["providers"];
    if (!providers.isObject() || !providers.hasKey(profileName))
        throw std::runtime_error("Agent config has no provider profile named '"
                                 + profileName + "'.");
    auto const& profile = providers[profileName];
    if (!profile.isObject())
        throw std::runtime_error("Provider profile '" + profileName + "' must be an object.");

    auto const context = "Provider profile '" + profileName + "'";
    auto const protocol = requireConfigString(profile, "protocol", context);
    if (protocol != "openai-compatible")
        throw std::runtime_error("Unsupported provider protocol '" + protocol
                                 + "' in profile '" + profileName + "'.");

    ProviderConfig result;
    result.name = profileName;
    result.baseUrl = requireConfigString(profile, "base_url", context);
    if (result.baseUrl.compare(0, 7, "http://") != 0
        && result.baseUrl.compare(0, 8, "https://") != 0)
        throw std::runtime_error(context + " base_url must use http:// or https://.");
    result.model = requireConfigString(profile, "model", context);

    if (profile.hasKey("chat_completions_path")) {
        result.chatCompletionsPath =
            requireConfigString(profile, "chat_completions_path", context);
    }
    if (profile.hasKey("stream")) {
        auto const& stream = profile["stream"];
        if (!stream.isBool())
            throw std::runtime_error(context + " field 'stream' must be a boolean.");
        result.streaming = stream.asBool();
    }
    if (profile.hasKey("extra_body")) {
        auto const& extraBody = profile["extra_body"];
        if (!extraBody.isObject())
            throw std::runtime_error(context + " field 'extra_body' must be an object.");
        result.extraBody = extraBody;
    }
    if (profile.hasKey("api_key_env")) {
        auto const apiKeyEnv = requireConfigString(profile, "api_key_env", context);
        auto const apiKey = std::getenv(apiKeyEnv.c_str());
        if (apiKey == nullptr || *apiKey == '\0')
            throw std::runtime_error(context + " requires environment variable '"
                                     + apiKeyEnv + "'.");
        result.apiKey = apiKey;
    }
    return result;
}

} // namespace agent
