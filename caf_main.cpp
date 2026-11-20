#include "function_registry.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <curl/curl.h>

#include <caf/actor_system.hpp>
#include <caf/action.hpp>
#include <caf/caf_main.hpp>
#include <caf/net/middleman.hpp>
#include <caf/net/multiplexer.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

struct StreamToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

struct ProviderConfig {
    std::string name;
    std::string baseUrl;
    std::string chatCompletionsPath = "/chat/completions";
    std::string model;
    std::string apiKey;
    bool streaming = true;
    JsonValue extraBody{std::map<std::string, JsonValue>{}};

    std::string endpoint() const {
        auto base = baseUrl;
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        return base + (chatCompletionsPath.front() == '/'
                           ? chatCompletionsPath
                           : "/" + chatCompletionsPath);
    }
};

std::string requireConfigString(JsonValue const& object, std::string const& key,
                                std::string const& context) {
    auto const& value = object[key];
    if (!value.isString() || value.asString().empty())
        throw std::runtime_error(context + " requires a non-empty string '" + key + "'.");
    return value.asString();
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

class OpenAICompatibleSseParser {
public:
    void feed(char const* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            if (data[i] == '\n')
                processLine();
            else
                lineBuffer_.push_back(data[i]);
        }
    }

    void finish() {
        if (!lineBuffer_.empty())
            processLine();
        dispatchEvent();
        if (!done_)
            throw std::runtime_error("Provider stream ended before the [DONE] event.");
        if (printedText_)
            std::cout << '\n';
    }

    JsonValue message() const {
        std::map<std::string, JsonValue> result{
            {"role", "assistant"},
            {"content", content_},
        };
        if (!toolCalls_.empty()) {
            std::vector<JsonValue> calls;
            calls.reserve(toolCalls_.size());
            for (auto const& [index, call] : toolCalls_) {
                if (call.id.empty() || call.name.empty())
                    throw std::runtime_error("Provider returned an incomplete tool call.");
                std::map<std::string, JsonValue> function{
                    {"name", call.name},
                    {"arguments", call.arguments},
                };
                calls.emplace_back(std::map<std::string, JsonValue>{
                    {"id", call.id},
                    {"type", "function"},
                    {"function", std::move(function)},
                });
            }
            result.emplace("tool_calls", std::move(calls));
        }
        return result;
    }

private:
    void processLine() {
        auto line = std::string_view{lineBuffer_};
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) {
            lineBuffer_.clear();
            dispatchEvent();
            return;
        }
        if (done_ || line.front() == ':' || line.compare(0, 5, "data:") != 0) {
            lineBuffer_.clear();
            return;
        }

        auto data = line.substr(5);
        if (!data.empty() && data.front() == ' ')
            data.remove_prefix(1);
        if (!eventData_.empty())
            eventData_.push_back('\n');
        eventData_.append(data);
        lineBuffer_.clear();
    }

    void dispatchEvent() {
        if (eventData_.empty() || done_)
            return;
        auto data = std::move(eventData_);
        eventData_.clear();
        if (data == "[DONE]") {
            done_ = true;
            return;
        }

        JsonParser parser{data};
        auto event = parser.parse();
        auto const& choices = event["choices"];
        if (!choices.isArray())
            throw std::runtime_error("Provider stream event has no choices array.");
        if (choices.asArray().empty())
            return;

        auto const& delta = choices.asArray().front()["delta"];
        auto const& content = delta["content"];
        if (content.isString())
            appendContent(content.asString());

        auto const& fragments = delta["tool_calls"];
        if (!fragments.isArray())
            return;
        for (auto const& fragment : fragments.asArray()) {
            auto const& indexValue = fragment["index"];
            if (!indexValue.isNumber() || indexValue.asNumber() < 0
                || indexValue.asNumber()
                       > static_cast<double>(std::numeric_limits<int>::max()))
                throw std::runtime_error("Provider stream returned an invalid tool-call index.");
            auto const index = static_cast<size_t>(indexValue.asNumber());
            auto& toolCall = toolCalls_[index];
            auto const& id = fragment["id"];
            if (id.isString())
                toolCall.id.append(id.asString());
            auto const& function = fragment["function"];
            auto const& name = function["name"];
            if (name.isString())
                toolCall.name.append(name.asString());
            auto const& arguments = function["arguments"];
            if (arguments.isString())
                toolCall.arguments.append(arguments.asString());
        }
    }

    void appendContent(std::string const& fragment) {
        if (!printedText_) {
            std::cout << "Assistant: ";
            printedText_ = true;
        }
        content_.append(fragment);
        std::cout << fragment << std::flush;
    }

    std::string lineBuffer_;
    std::string eventData_;
    std::string content_;
    std::map<size_t, StreamToolCall> toolCalls_;
    bool done_ = false;
    bool printedText_ = false;
};

struct StreamContext {
    explicit StreamContext(bool useStreaming) : streaming{useStreaming} {}

    OpenAICompatibleSseParser parser;
    bool streaming;
    long statusCode = 0;
    std::string errorBody;
    std::string responseBody;
    std::exception_ptr callbackError;

    void appendErrorBody(char const* data, size_t size) {
        constexpr size_t maxErrorBodySize = 64 * 1024;
        auto const remaining = maxErrorBodySize - errorBody.size();
        errorBody.append(data, std::min(size, remaining));
    }

    void appendResponseBody(char const* data, size_t size) {
        if (size > responseBody.max_size() - responseBody.size())
            throw std::runtime_error("Provider response body is too large.");
        responseBody.append(data, size);
    }
};

size_t receiveHeaders(char* data, size_t size, size_t count, void* context) {
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size)
        return 0;
    auto const bytes = size * count;
    auto& stream = *static_cast<StreamContext*>(context);
    std::string_view line{data, bytes};
    if (line.compare(0, 5, "HTTP/") == 0) {
        auto const separator = line.find(' ');
        if (separator != std::string_view::npos) {
            int statusCode = 0;
            auto const first = line.data() + separator + 1;
            auto const last = line.data() + line.size();
            auto const [end, error] = std::from_chars(first, last, statusCode);
            if (error == std::errc{} && end != first)
                stream.statusCode = statusCode;
        }
    }
    return bytes;
}

size_t receiveStream(char* data, size_t size, size_t count, void* context) {
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size)
        return 0;
    auto const bytes = size * count;
    auto& stream = *static_cast<StreamContext*>(context);
    try {
        if (stream.statusCode < 200 || stream.statusCode >= 300) {
            stream.appendErrorBody(data, bytes);
        } else if (stream.streaming) {
            stream.parser.feed(data, bytes);
        } else {
            stream.appendResponseBody(data, bytes);
        }
    } catch (...) {
        stream.callbackError = std::current_exception();
        return 0;
    }
    return bytes;
}

void checkMultiResult(CURLMcode result, char const* operation) {
    if (result != CURLM_OK)
        throw std::runtime_error(std::string{operation} + " failed: "
                                 + curl_multi_strerror(result));
}

std::string curlError(CURLcode result, char const* errorBuffer) {
    return errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
}

class CurlRequest : public std::enable_shared_from_this<CurlRequest> {
public:
    using completion_type = std::function<void(JsonValue, std::string)>;

    CurlRequest(net::multiplexer& mpx, ProviderConfig provider, std::string payload,
                completion_type completion)
        : mpx_{mpx},
          provider_{std::move(provider)},
          payload_{std::move(payload)},
          stream_{provider_.streaming},
          completion_{std::move(completion)} {
        if (payload_.size() > static_cast<size_t>(std::numeric_limits<curl_off_t>::max()))
            throw std::runtime_error("Provider request body is too large.");
    }

    ~CurlRequest() {
        cleanup();
    }

    void start() {
        auto self = shared_from_this();
        mpx_.schedule(std::chrono::steady_clock::now(),
                      make_single_shot_action([self] { self->startOnMultiplexer(); }));
    }

private:
    void startOnMultiplexer() {
        try {
            easy_ = curl_easy_init();
            if (easy_ == nullptr)
                throw std::runtime_error("curl_easy_init failed.");
            multi_ = curl_multi_init();
            if (multi_ == nullptr)
                throw std::runtime_error("curl_multi_init failed.");

            if (!provider_.apiKey.empty())
                headers_.append("Authorization: Bearer " + provider_.apiKey);
            headers_.append("Content-Type: application/json");
            headers_.append(provider_.streaming ? "Accept: text/event-stream"
                                                : "Accept: application/json");
            auto const endpoint = provider_.endpoint();
            setCurlOption(easy_, CURLOPT_URL, endpoint.c_str());
            setCurlOption(easy_, CURLOPT_HTTPHEADER, headers_.get());
            setCurlOption(easy_, CURLOPT_POST, 1L);
            setCurlOption(easy_, CURLOPT_POSTFIELDS, payload_.data());
            setCurlOption(easy_, CURLOPT_POSTFIELDSIZE_LARGE,
                          static_cast<curl_off_t>(payload_.size()));
            setCurlOption(easy_, CURLOPT_WRITEFUNCTION, &receiveStream);
            setCurlOption(easy_, CURLOPT_WRITEDATA, &stream_);
            setCurlOption(easy_, CURLOPT_HEADERFUNCTION, &receiveHeaders);
            setCurlOption(easy_, CURLOPT_HEADERDATA, &stream_);
            setCurlOption(easy_, CURLOPT_ERRORBUFFER, errorBuffer_);
            setCurlOption(easy_, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
            setCurlOption(easy_, CURLOPT_TIMEOUT_MS, 90000L);
            setCurlOption(easy_, CURLOPT_NOSIGNAL, 1L);
            setCurlOption(easy_, CURLOPT_ACCEPT_ENCODING, "identity");
            setCurlOption(easy_, CURLOPT_USERAGENT, "FunctionCallingCAF/1.0");
            checkMultiResult(curl_multi_add_handle(multi_, easy_), "curl_multi_add_handle");
            attached_ = true;
            pollOnMultiplexer();
        } catch (std::exception const& e) {
            finish({}, e.what());
        } catch (...) {
            finish({}, "Unknown error while starting provider request.");
        }
    }

    void schedulePoll() {
        auto self = shared_from_this();
        mpx_.schedule(std::chrono::steady_clock::now() + std::chrono::milliseconds{10},
                      make_single_shot_action([self] { self->pollOnMultiplexer(); }));
    }

    void perform(int& running) {
        CURLMcode result;
        do {
            result = curl_multi_perform(multi_, &running);
        } while (result == CURLM_CALL_MULTI_PERFORM);
        checkMultiResult(result, "curl_multi_perform");
    }

    void pollOnMultiplexer() {
        try {
            int running = 0;
            perform(running);
            int fileDescriptors = 0;
            checkMultiResult(curl_multi_poll(multi_, nullptr, 0, 0, &fileDescriptors),
                             "curl_multi_poll");
            perform(running);

            int messagesLeft = 0;
            while (auto* message = curl_multi_info_read(multi_, &messagesLeft)) {
                if (message->msg == CURLMSG_DONE && message->easy_handle == easy_) {
                    finishTransfer(message->data.result);
                    return;
                }
            }
            if (running == 0)
                throw std::runtime_error("libcurl ended without a completion event.");
            schedulePoll();
        } catch (std::exception const& e) {
            finish({}, e.what());
        } catch (...) {
            finish({}, "Unknown error while processing provider response.");
        }
    }

    void finishTransfer(CURLcode result) {
        try {
            if (stream_.callbackError) {
                try {
                    std::rethrow_exception(stream_.callbackError);
                } catch (std::exception const& e) {
                    throw std::runtime_error(std::string{"Failed to process provider stream: "}
                                             + e.what());
                } catch (...) {
                    throw std::runtime_error("Failed to process provider stream.");
                }
            }
            if (result != CURLE_OK)
                throw std::runtime_error("Provider '" + provider_.name
                                         + "' request failed: "
                                         + curlError(result, errorBuffer_));
            if (stream_.statusCode != 200)
                throw std::runtime_error("Provider HTTP error "
                                         + std::to_string(stream_.statusCode) + ": "
                                         + stream_.errorBody);

            JsonValue message;
            if (provider_.streaming) {
                stream_.parser.finish();
                message = stream_.parser.message();
            } else {
                JsonParser parser{stream_.responseBody};
                auto response = parser.parse();
                auto const& choices = response["choices"];
                if (!choices.isArray() || choices.asArray().empty())
                    throw std::runtime_error("Provider returned no choices: "
                                             + stream_.responseBody);
                message = choices.asArray().front()["message"];
                if (!message.isObject())
                    throw std::runtime_error("Provider response has no assistant message: "
                                             + stream_.responseBody);
            }
            finish(std::move(message), {});
        } catch (std::exception const& e) {
            finish({}, e.what());
        } catch (...) {
            finish({}, "Unknown error while processing provider response.");
        }
    }

    void finish(JsonValue message, std::string error) {
        cleanup();
        auto completion = std::move(completion_);
        if (completion)
            completion(std::move(message), std::move(error));
    }

    void cleanup() {
        if (multi_ != nullptr) {
            if (attached_)
                curl_multi_remove_handle(multi_, easy_);
            attached_ = false;
            curl_multi_cleanup(multi_);
            multi_ = nullptr;
        }
        if (easy_ != nullptr) {
            curl_easy_cleanup(easy_);
            easy_ = nullptr;
        }
    }

    net::multiplexer& mpx_;
    ProviderConfig provider_;
    std::string payload_;
    StreamContext stream_;
    completion_type completion_;
    CURL* easy_ = nullptr;
    CURLM* multi_ = nullptr;
    CurlHeaders headers_;
    bool attached_ = false;
    char errorBuffer_[CURL_ERROR_SIZE] = {};
};

JsonValue makeRequest(ProviderConfig const& provider,
                      std::vector<JsonValue> const& messages, JsonValue const& tools) {
    std::map<std::string, JsonValue> request{
        {"model", provider.model},
        {"messages", messages},
        {"tools", tools},
        {"tool_choice", "auto"},
        {"stream", provider.streaming},
    };
    for (auto const& [key, value] : provider.extraBody.asObject()) {
        if (request.count(key) > 0)
            throw std::runtime_error("Provider extra_body cannot override request field '"
                                     + key + "'.");
        request.emplace(key, value);
    }
    return request;
}

class AgentConversation : public std::enable_shared_from_this<AgentConversation> {
public:
    using completion_type = std::function<void(std::exception_ptr)>;

    AgentConversation(net::multiplexer& mpx, ProviderConfig provider, std::string prompt,
                     completion_type completion)
        : mpx_{mpx},
          provider_{std::move(provider)},
          registry_{makeFunctionRegistry()},
          tools_{registry_.getTools()},
          completion_{std::move(completion)} {
        messages_.emplace_back(std::map<std::string, JsonValue>{
            {"role", "system"},
            {"content", "调用工具时必须严格遵循工具的 JSON Schema。"},
        });
        messages_.emplace_back(std::map<std::string, JsonValue>{
            {"role", "user"},
            {"content", std::move(prompt)},
        });
    }

    void start() {
        requestNext();
    }

private:
    void requestNext() {
        constexpr size_t maxToolRounds = 8;
        if (requestCount_ >= maxToolRounds)
            throw std::runtime_error("Exceeded the maximum number of provider tool-call rounds.");

        auto payload = makeRequest(provider_, messages_, tools_).toString();
        ++requestCount_;
        std::weak_ptr<AgentConversation> weakSelf = shared_from_this();
        activeRequest_ = std::make_shared<CurlRequest>(
            mpx_, provider_, std::move(payload),
            [weakSelf](JsonValue message, std::string error) {
                if (auto self = weakSelf.lock()) {
                    self->activeRequest_.reset();
                    try {
                        if (!error.empty())
                            throw std::runtime_error(std::move(error));
                        self->handleResponse(std::move(message));
                    } catch (...) {
                        self->finish(std::current_exception());
                    }
                }
            });
        activeRequest_->start();
    }

    void handleResponse(JsonValue message) {
        auto const& toolCalls = message["tool_calls"];
        if (!toolCalls.isArray() || toolCalls.asArray().empty()) {
            if (!provider_.streaming) {
                auto const& content = message["content"];
                if (content.isString())
                    std::cout << "Assistant: " << content.asString() << '\n';
                else
                    std::cout << "Assistant: " << message.toString() << '\n';
            }
            finish();
            return;
        }

        messages_.push_back(message);
        for (auto const& toolCall : toolCalls.asArray()) {
            auto const& id = toolCall["id"].asString();
            auto result = registry_.executeOpenAIToolCall(toolCall);
            if (result.isObject() && result.hasKey("error"))
                throw std::runtime_error(result["error"].asString());
            messages_.emplace_back(std::map<std::string, JsonValue>{
                {"role", "tool"},
                {"tool_call_id", id},
                {"content", result.toString()},
            });
        }
        requestNext();
    }

    void finish(std::exception_ptr error = {}) {
        if (finished_)
            return;
        finished_ = true;
        auto completion = std::move(completion_);
        if (completion)
            completion(std::move(error));
    }

    net::multiplexer& mpx_;
    ProviderConfig provider_;
    FunctionRegistry registry_;
    JsonValue tools_;
    std::vector<JsonValue> messages_;
    std::shared_ptr<CurlRequest> activeRequest_;
    completion_type completion_;
    size_t requestCount_ = 0;
    bool finished_ = false;
};

int runAgent(actor_system& sys) {
    auto provider = loadProviderConfig();
    CurlGlobal curlGlobal;

    std::string prompt;
    std::cout << "请输入问题：";
    if (!std::getline(std::cin, prompt) || prompt.empty()) {
        std::cerr << "问题不能为空。\n";
        return EXIT_FAILURE;
    }

    auto* mpx = net::multiplexer::from(sys);
    if (mpx == nullptr)
        throw std::runtime_error("CAF network multiplexer is unavailable.");
    std::promise<void> completion;
    auto result = completion.get_future();
    auto conversation = std::make_shared<AgentConversation>(
        *mpx, std::move(provider), std::move(prompt),
        [&completion](std::exception_ptr error) {
            if (error)
                completion.set_exception(std::move(error));
            else
                completion.set_value();
        });
    conversation->start();
    result.get();
    return EXIT_SUCCESS;
}

} // namespace

int caf_main(actor_system& sys) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    try {
        return runAgent(sys);
    } catch (std::exception const& e) {
        std::cerr << "Agent 请求失败: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

CAF_MAIN(caf::net::middleman, caf::id_block::function_calling)
