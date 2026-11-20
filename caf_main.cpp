#include "function_registry.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <curl/curl.h>

#include <caf/actor_system.hpp>
#include <caf/caf_main.hpp>
#include <caf/net/middleman.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace caf;

CAF_BEGIN_TYPE_ID_BLOCK(function_calling, first_custom_type_id)
    CAF_ADD_ATOM(function_calling, call_request_atom)
    CAF_ADD_ATOM(function_calling, call_result_atom)
    CAF_ADD_TYPE_ID(function_calling, (JsonValue), "JsonValue")
CAF_END_TYPE_ID_BLOCK(function_calling)

namespace {

class CurlGlobal {
public:
    CurlGlobal() {
        auto const result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK)
            throw std::runtime_error(std::string{"curl_global_init failed: "}
                                     + curl_easy_strerror(result));
    }

    ~CurlGlobal() {
        curl_global_cleanup();
    }

    CurlGlobal(CurlGlobal const&) = delete;
    CurlGlobal& operator=(CurlGlobal const&) = delete;
};

class CurlHandle {
public:
    CurlHandle() : value_{curl_easy_init()} {}

    ~CurlHandle() {
        if (value_ != nullptr)
            curl_easy_cleanup(value_);
    }

    CurlHandle(CurlHandle const&) = delete;
    CurlHandle& operator=(CurlHandle const&) = delete;

    CURL* get() const {
        return value_;
    }

private:
    CURL* value_;
};

class CurlHeaders {
public:
    ~CurlHeaders() {
        curl_slist_free_all(value_);
    }

    CurlHeaders(CurlHeaders const&) = delete;
    CurlHeaders& operator=(CurlHeaders const&) = delete;

    CurlHeaders() = default;

    void append(std::string const& header) {
        auto* updated = curl_slist_append(value_, header.c_str());
        if (updated == nullptr)
            throw std::runtime_error("Failed to allocate HTTP request headers.");
        value_ = updated;
    }

    curl_slist* get() const {
        return value_;
    }

private:
    curl_slist* value_ = nullptr;
};

template <class T>
void setCurlOption(CURL* handle, CURLoption option, T value) {
    auto const result = curl_easy_setopt(handle, option, value);
    if (result != CURLE_OK)
        throw std::runtime_error(std::string{"curl_easy_setopt failed: "}
                                 + curl_easy_strerror(result));
}

size_t appendResponse(char* data, size_t size, size_t count, void* context) {
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size)
        return 0;
    auto const bytes = size * count;
    auto& response = *static_cast<std::string*>(context);
    if (bytes > response.max_size() - response.size())
        return 0;
    response.append(data, bytes);
    return bytes;
}

std::string postDeepSeek(std::string const& apiKey, std::string const& payload) {
    if (payload.size() > static_cast<size_t>(std::numeric_limits<curl_off_t>::max()))
        throw std::runtime_error("DeepSeek request body is too large.");

    CurlHandle request;
    CURL* const handle = request.get();
    if (handle == nullptr)
        throw std::runtime_error("curl_easy_init failed.");

    CurlHeaders headers;
    headers.append("Authorization: Bearer " + apiKey);
    headers.append("Content-Type: application/json");
    headers.append("Accept: application/json");

    std::string bodyText;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    setCurlOption(handle, CURLOPT_URL,
                  "https://api.deepseek.com/chat/completions");
    setCurlOption(handle, CURLOPT_HTTPHEADER, headers.get());
    setCurlOption(handle, CURLOPT_POST, 1L);
    setCurlOption(handle, CURLOPT_POSTFIELDS, payload.data());
    setCurlOption(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                  static_cast<curl_off_t>(payload.size()));
    setCurlOption(handle, CURLOPT_WRITEFUNCTION, &appendResponse);
    setCurlOption(handle, CURLOPT_WRITEDATA, &bodyText);
    setCurlOption(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    setCurlOption(handle, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    setCurlOption(handle, CURLOPT_TIMEOUT_MS, 90000L);
    setCurlOption(handle, CURLOPT_NOSIGNAL, 1L);
    setCurlOption(handle, CURLOPT_ACCEPT_ENCODING, "identity");
    setCurlOption(handle, CURLOPT_USERAGENT, "FunctionCallingCAF/1.0");

    auto const result = curl_easy_perform(handle);
    if (result != CURLE_OK) {
        auto const* detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
        throw std::runtime_error(std::string{"DeepSeek request failed: "} + detail);
    }

    long statusCode = 0;
    auto const infoResult = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &statusCode);
    if (infoResult != CURLE_OK)
        throw std::runtime_error(std::string{"curl_easy_getinfo failed: "}
                                 + curl_easy_strerror(infoResult));
    if (statusCode != 200)
        throw std::runtime_error("DeepSeek HTTP error " + std::to_string(statusCode)
                                 + ": " + bodyText);

    return bodyText;
}

JsonValue makeRequest(std::string const& model, std::vector<JsonValue> const& messages,
                      JsonValue const& tools) {
    return std::map<std::string, JsonValue>{
        {"model", model},
        {"messages", messages},
        {"tools", tools},
        {"tool_choice", "auto"},
        {"thinking", std::map<std::string, JsonValue>{{"type", "disabled"}}},
    };
}

int runDeepSeek() {
    auto apiKey = std::getenv("DEEPSEEK_API_KEY");
    if (apiKey == nullptr || *apiKey == '\0') {
        std::cerr << "请先设置 DEEPSEEK_API_KEY 环境变量。\n";
        return EXIT_FAILURE;
    }
    CurlGlobal curlGlobal;

    auto modelEnv = std::getenv("DEEPSEEK_MODEL");
    std::string model = modelEnv != nullptr && *modelEnv != '\0'
                            ? modelEnv
                            : "deepseek-flash";

    std::string prompt;
    std::cout << "请输入问题：";
    if (!std::getline(std::cin, prompt) || prompt.empty()) {
        std::cerr << "问题不能为空。\n";
        return EXIT_FAILURE;
    }

    auto registry = makeFunctionRegistry();
    auto tools = registry.getTools();
    std::vector<JsonValue> messages;
    messages.emplace_back(std::map<std::string, JsonValue>{
        {"role", "system"},
        {"content", "调用工具时必须严格遵循工具的 JSON Schema。"},
    });
    messages.emplace_back(std::map<std::string, JsonValue>{
        {"role", "user"},
        {"content", prompt},
    });

    constexpr size_t maxToolRounds = 8;
    for (size_t round = 0; round < maxToolRounds; ++round) {
        auto payload = makeRequest(model, messages, tools).toString();
        // std::cout << "payload" << payload << "\n";
        auto responseBody = postDeepSeek(apiKey, payload);
        // std::cout << "模型回复：" << responseBody << std::endl;
        JsonParser parser{responseBody};
        auto response = parser.parse();
        auto const& choices = response["choices"].asArray();
        if (choices.empty())
            throw std::runtime_error("DeepSeek returned no choices: " + responseBody);

        auto const& message = choices.front()["message"];
        auto const& toolCalls = message["tool_calls"];
        if (!toolCalls.isArray() || toolCalls.asArray().empty()) {
            auto const& content = message["content"];
            if (content.isString())
                std::cout << "DeepSeek: " << content.asString() << '\n';
            else
                std::cout << "DeepSeek: " << message.toString() << '\n';
            return EXIT_SUCCESS;
        }

        messages.push_back(message);
        for (auto const& toolCall : toolCalls.asArray()) {
            auto const& id = toolCall["id"].asString();
            auto result = registry.executeOpenAIToolCall(toolCall);
            if (result.isObject() && result.hasKey("error"))
                throw std::runtime_error(result["error"].asString());
            messages.emplace_back(std::map<std::string, JsonValue>{
                {"role", "tool"},
                {"tool_call_id", id},
                {"content", result.toString()},
            });
        }
    }

    throw std::runtime_error("Exceeded the maximum number of DeepSeek tool-call rounds.");
}

} // namespace

int caf_main(actor_system& sys) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    try {
        return runDeepSeek();
    } catch (std::exception const& e) {
        std::cerr << "DeepSeek 请求失败: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

CAF_MAIN(caf::net::middleman, caf::id_block::function_calling)
