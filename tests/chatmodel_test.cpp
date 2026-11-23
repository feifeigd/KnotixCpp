#include <knotix/ext/components/model/openai/chatmodel.h>

#include "caf/test/test.hpp"

#include <cstdlib>

using openai::ChatModelConfig;

namespace {

void setEnv(char const* name, char const* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(char const* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

SUITE("openai.chatmodel_config") {

TEST("defaults to the OPENAI_API_KEY environment variable") {
    ChatModelConfig const config;
    check_eq(config.api_key_env, "OPENAI_API_KEY");
    check(config.APIKey.empty());
}

TEST("from_json reads the key from the default environment variable") {
    setEnv("OPENAI_API_KEY", "default-key");
    auto const config = json::object({}).get<ChatModelConfig>();
    check_eq(config.api_key_env, "OPENAI_API_KEY");
    check_eq(config.APIKey, "default-key");
}

TEST("from_json honors an explicit api_key_env field") {
    setEnv("KNOTIX_CHATMODEL_KEY", "named-key");
    auto const config = json::object({{"api_key_env", "KNOTIX_CHATMODEL_KEY"}})
                            .get<ChatModelConfig>();
    check_eq(config.api_key_env, "KNOTIX_CHATMODEL_KEY");
    check_eq(config.APIKey, "named-key");
}

TEST("from_json throws when the environment variable is missing or empty") {
    unsetEnv("KNOTIX_CHATMODEL_MISSING");
    auto const named = json::object({{"api_key_env", "KNOTIX_CHATMODEL_MISSING"}});
    check_throws<std::runtime_error>([&] { return named.get<ChatModelConfig>(); });

    setEnv("KNOTIX_CHATMODEL_EMPTY", "");
    auto const empty = json::object({{"api_key_env", "KNOTIX_CHATMODEL_EMPTY"}});
    check_throws<std::runtime_error>([&] { return empty.get<ChatModelConfig>(); });
}

} // SUITE("openai.chatmodel_config")
