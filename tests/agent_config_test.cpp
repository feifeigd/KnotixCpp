#include "agent_config.hpp"

#include "caf/test/test.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

using agent::ProviderConfig;

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

// Restores an environment variable on destruction.
class EnvGuard {
public:
    EnvGuard(char const* name, char const* value) : name_{name} {
        if (auto const* old = std::getenv(name))
            oldValue_ = old;
        setEnv(name, value);
    }
    ~EnvGuard() {
        if (oldValue_)
            setEnv(name_.c_str(), oldValue_->c_str());
        else
            unsetEnv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> oldValue_;
};

// Unsets an environment variable for the lifetime of the guard.
class UnsetGuard {
public:
    explicit UnsetGuard(char const* name) : name_{name} {
        if (auto const* old = std::getenv(name))
            oldValue_ = old;
        unsetEnv(name);
    }
    ~UnsetGuard() {
        if (oldValue_)
            setEnv(name_.c_str(), oldValue_->c_str());
    }

private:
    std::string name_;
    std::optional<std::string> oldValue_;
};

json validProfile() {
    return json::object({
        {"protocol", "openai-compatible"},
        {"base_url", "https://api.example.com/v1"},
        {"model", "test-model"},
    });
}

} // namespace

SUITE("agent.config") {

TEST("endpoint joins base_url and the chat completions path") {
    ProviderConfig config;
    config.baseUrl = "https://api.example.com/v1";
    check_eq(config.endpoint(), "https://api.example.com/v1/chat/completions");
}

TEST("endpoint strips trailing slashes from base_url") {
    ProviderConfig config;
    config.baseUrl = "https://api.example.com/v1///";
    check_eq(config.endpoint(), "https://api.example.com/v1/chat/completions");
}

TEST("endpoint inserts a slash when the path has no leading one") {
    ProviderConfig config;
    config.baseUrl = "https://api.example.com/v1";
    config.chatCompletionsPath = "chat/completions";
    check_eq(config.endpoint(), "https://api.example.com/v1/chat/completions");
}

TEST("from_json applies the documented defaults") {
    EnvGuard key{"OPENAI_API_KEY", "test-key"};
    auto const config = validProfile().get<ProviderConfig>();
    check(config.name.empty());
    check_eq(config.baseUrl, "https://api.example.com/v1");
    check_eq(config.chatCompletionsPath, "/chat/completions");
    check_eq(config.model, "test-model");
    check(config.streaming);
    check(config.extraBody.is_object());
    check(config.extraBody.empty());
    // The API key defaults to the OPENAI_API_KEY environment variable.
    check_eq(config.chatModelConfig.api_key_env, "OPENAI_API_KEY");
    check_eq(config.chatModelConfig.APIKey, "test-key");
}

TEST("from_json honors optional overrides") {
    EnvGuard key{"OPENAI_API_KEY", "test-key"};
    auto profile = validProfile();
    profile["chat_completions_path"] = "/v2/chat";
    profile["stream"] = false;
    profile["extra_body"] = json::object({{"temperature", 0.2}});
    auto const config = profile.get<ProviderConfig>();
    check_eq(config.chatCompletionsPath, "/v2/chat");
    check(!config.streaming);
    check_eq(config.extraBody, json::object({{"temperature", 0.2}}));
}

TEST("from_json rejects a non-object profile") {
    check_throws<std::runtime_error>([] { return json(42).get<ProviderConfig>(); });
}

TEST("from_json requires the openai-compatible protocol") {
    auto missing = validProfile();
    missing.erase("protocol");
    check_throws<std::runtime_error>([&] { return missing.get<ProviderConfig>(); });

    auto unsupported = validProfile();
    unsupported["protocol"] = "grpc";
    check_throws<std::runtime_error>([&] { return unsupported.get<ProviderConfig>(); });
}

TEST("from_json requires an http or https base_url") {
    auto ftp = validProfile();
    ftp["base_url"] = "ftp://example.com";
    check_throws<std::runtime_error>([&] { return ftp.get<ProviderConfig>(); });

    auto bare = validProfile();
    bare["base_url"] = "api.example.com";
    check_throws<std::runtime_error>([&] { return bare.get<ProviderConfig>(); });

    EnvGuard key{"OPENAI_API_KEY", "test-key"};
    auto http = validProfile();
    http["base_url"] = "http://localhost:8080";
    check_eq(http.get<ProviderConfig>().baseUrl, "http://localhost:8080");
}

TEST("from_json requires a non-empty model string") {
    auto missing = validProfile();
    missing.erase("model");
    check_throws<std::runtime_error>([&] { return missing.get<ProviderConfig>(); });

    auto empty = validProfile();
    empty["model"] = "";
    check_throws<std::runtime_error>([&] { return empty.get<ProviderConfig>(); });
}

TEST("from_json validates the types of optional fields") {
    auto badStream = validProfile();
    badStream["stream"] = "yes";
    check_throws<std::runtime_error>([&] { return badStream.get<ProviderConfig>(); });

    auto badExtra = validProfile();
    badExtra["extra_body"] = json::array();
    check_throws<std::runtime_error>([&] { return badExtra.get<ProviderConfig>(); });
}

TEST("from_json reads the API key from the named environment variable") {
    EnvGuard env{"KNOTIX_TEST_API_KEY", "secret-token"};
    auto profile = validProfile();
    profile["api_key_env"] = "KNOTIX_TEST_API_KEY";
    auto const config = profile.get<ProviderConfig>();
    check_eq(config.chatModelConfig.api_key_env, "KNOTIX_TEST_API_KEY");
    check_eq(config.chatModelConfig.APIKey, "secret-token");
}

TEST("from_json fails when the API key environment variable is missing") {
    unsetEnv("KNOTIX_TEST_MISSING_KEY");
    auto profile = validProfile();
    profile["api_key_env"] = "KNOTIX_TEST_MISSING_KEY";
    check_throws<std::runtime_error>([&] { return profile.get<ProviderConfig>(); });
}

TEST("from_json requires OPENAI_API_KEY when api_key_env is not set") {
    UnsetGuard noKey{"OPENAI_API_KEY"};
    check_throws<std::runtime_error>([&] { return validProfile().get<ProviderConfig>(); });
}

TEST("loadProviderConfig loads the active profile from disk") {
    EnvGuard key{"OPENAI_API_KEY", "test-key"};
    auto const path = std::filesystem::temp_directory_path() / "knotix_test_agent.json";
    {
        std::ofstream out{path};
        out << json::object({
            {"active_provider", "demo"},
            {"providers", json::object({{"demo", validProfile()}})},
        }).dump();
    }
    EnvGuard env{"AGENT_CONFIG", path.string().c_str()};
    auto const config = agent::loadProviderConfig();
    check_eq(config.name, "demo");
    check_eq(config.model, "test-model");
    check_eq(config.endpoint(), "https://api.example.com/v1/chat/completions");
    check_eq(config.chatModelConfig.APIKey, "test-key");
    std::filesystem::remove(path);
}

TEST("loadProviderConfig fails for a missing file") {
    EnvGuard env{"AGENT_CONFIG", "knotix_test_no_such_file.json"};
    check_throws<std::runtime_error>([] { return agent::loadProviderConfig(); });
}

TEST("loadProviderConfig fails when the active profile does not exist") {
    auto const path = std::filesystem::temp_directory_path() / "knotix_test_agent_bad.json";
    {
        std::ofstream out{path};
        out << json::object({
            {"active_provider", "ghost"},
            {"providers", json::object({{"demo", validProfile()}})},
        }).dump();
    }
    EnvGuard env{"AGENT_CONFIG", path.string().c_str()};
    check_throws<std::runtime_error>([] { return agent::loadProviderConfig(); });
    std::filesystem::remove(path);
}

} // SUITE("agent.config")
