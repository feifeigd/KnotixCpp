#include "agent_config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace agent {
namespace {

std::string requireConfigString(json const& object, std::string const& key,
                                std::string const& context) {
    auto const& value = jsonGet(object, key);
    if (!value.is_string() || value.get_ref<std::string const&>().empty())
        throw std::runtime_error(context + " requires a non-empty string '" + key + "'.");
    return value.get<std::string>();
}

ProviderConfig parseProviderConfig(json const& profile, std::string const& context) {
    if (!profile.is_object())
        throw std::runtime_error(context + " must be an object.");

    auto const protocol = requireConfigString(profile, "protocol", context);
    if (protocol != "openai-compatible")
        throw std::runtime_error("Unsupported provider protocol '" + protocol
                                 + "' in " + context + ".");

    ProviderConfig result;
    result.baseUrl = requireConfigString(profile, "base_url", context);
    if (result.baseUrl.compare(0, 7, "http://") != 0
        && result.baseUrl.compare(0, 8, "https://") != 0)
        throw std::runtime_error(context + " base_url must use http:// or https://.");
    result.model = requireConfigString(profile, "model", context);

    if (profile.contains("chat_completions_path")) {
        result.chatCompletionsPath =
            requireConfigString(profile, "chat_completions_path", context);
    }
    if (profile.contains("stream")) {
        auto const& stream = jsonGet(profile, "stream");
        if (!stream.is_boolean())
            throw std::runtime_error(context + " field 'stream' must be a boolean.");
        result.streaming = stream.get<bool>();
    }
    if (profile.contains("extra_body")) {
        auto const& extraBody = jsonGet(profile, "extra_body");
        if (!extraBody.is_object())
            throw std::runtime_error(context + " field 'extra_body' must be an object.");
        result.extraBody = extraBody;
    }

    result.chatModelConfig = profile.get<openai::ChatModelConfig>();
    
    return result;
}

} // namespace

void from_json(json const& value, ProviderConfig& config) {
    config = parseProviderConfig(value, "Provider config");
}

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

    auto config = json::parse(input);
    if (!config.is_object())
        throw std::runtime_error("Agent config must be a JSON object.");
    auto const profileName = requireConfigString(config, "active_provider", "Agent config");
    auto const& providers = jsonGet(config, "providers");
    if (!providers.is_object() || !providers.contains(profileName))
        throw std::runtime_error("Agent config has no provider profile named '"
                                 + profileName + "'.");
    auto const& profile = jsonGet(providers, profileName);
    auto const context = "Provider profile '" + profileName + "'";
    auto result = parseProviderConfig(profile, context);
    result.name = profileName;
    return result;
}

} // namespace agent
