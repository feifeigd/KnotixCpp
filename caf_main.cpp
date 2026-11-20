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

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
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

struct StreamToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

class SseStreamParser {
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
            throw std::runtime_error("DeepSeek stream ended before the [DONE] event.");
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
                    throw std::runtime_error("DeepSeek returned an incomplete tool call.");
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
            throw std::runtime_error("DeepSeek stream event has no choices array.");
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
                throw std::runtime_error("DeepSeek stream returned an invalid tool-call index.");
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
            std::cout << "DeepSeek: ";
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

    SseStreamParser parser;
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
            throw std::runtime_error("DeepSeek response body is too large.");
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

class CurlMultiHandle {
public:
    CurlMultiHandle() : value_{curl_multi_init()} {
        if (value_ == nullptr)
            throw std::runtime_error("curl_multi_init failed.");
    }

    ~CurlMultiHandle() {
        if (attached_ != nullptr)
            curl_multi_remove_handle(value_, attached_);
        curl_multi_cleanup(value_);
    }

    CurlMultiHandle(CurlMultiHandle const&) = delete;
    CurlMultiHandle& operator=(CurlMultiHandle const&) = delete;

    CURLM* get() const {
        return value_;
    }

    void add(CURL* handle) {
        checkMultiResult(curl_multi_add_handle(value_, handle), "curl_multi_add_handle");
        attached_ = handle;
    }

    void remove() {
        checkMultiResult(curl_multi_remove_handle(value_, attached_),
                         "curl_multi_remove_handle");
        attached_ = nullptr;
    }

private:
    CURLM* value_;
    CURL* attached_ = nullptr;
};

std::string curlError(CURLcode result, char const* errorBuffer) {
    return errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
}

JsonValue postDeepSeek(std::string const& apiKey, std::string const& payload, bool streaming) {
    if (payload.size() > static_cast<size_t>(std::numeric_limits<curl_off_t>::max()))
        throw std::runtime_error("DeepSeek request body is too large.");

    CurlHandle request;
    CURL* const handle = request.get();
    if (handle == nullptr)
        throw std::runtime_error("curl_easy_init failed.");

    CurlHeaders headers;
    headers.append("Authorization: Bearer " + apiKey);
    headers.append("Content-Type: application/json");
    headers.append(streaming ? "Accept: text/event-stream" : "Accept: application/json");

    StreamContext stream{streaming};
    char errorBuffer[CURL_ERROR_SIZE] = {};
    setCurlOption(handle, CURLOPT_URL, "https://api.deepseek.com/chat/completions");
    setCurlOption(handle, CURLOPT_HTTPHEADER, headers.get());
    setCurlOption(handle, CURLOPT_POST, 1L);
    setCurlOption(handle, CURLOPT_POSTFIELDS, payload.data());
    setCurlOption(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                  static_cast<curl_off_t>(payload.size()));
    setCurlOption(handle, CURLOPT_WRITEFUNCTION, &receiveStream);
    setCurlOption(handle, CURLOPT_WRITEDATA, &stream);
    setCurlOption(handle, CURLOPT_HEADERFUNCTION, &receiveHeaders);
    setCurlOption(handle, CURLOPT_HEADERDATA, &stream);
    setCurlOption(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    setCurlOption(handle, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    setCurlOption(handle, CURLOPT_TIMEOUT_MS, 90000L);
    setCurlOption(handle, CURLOPT_NOSIGNAL, 1L);
    setCurlOption(handle, CURLOPT_ACCEPT_ENCODING, "identity");
    setCurlOption(handle, CURLOPT_USERAGENT, "FunctionCallingCAF/1.0");

    CurlMultiHandle multi;
    multi.add(handle);
    bool completed = false;
    CURLcode transferResult = CURLE_OK;
    while (!completed) {
        int running = 0;
        checkMultiResult(curl_multi_perform(multi.get(), &running), "curl_multi_perform");

        int messagesLeft = 0;
        while (auto* message = curl_multi_info_read(multi.get(), &messagesLeft)) {
            if (message->msg == CURLMSG_DONE && message->easy_handle == handle) {
                transferResult = message->data.result;
                completed = true;
            }
        }

        if (!completed) {
            if (running == 0)
                throw std::runtime_error("libcurl ended the stream without a completion event.");
            int fileDescriptors = 0;
            checkMultiResult(curl_multi_poll(multi.get(), nullptr, 0, 1000, &fileDescriptors),
                             "curl_multi_poll");
        }
    }
    multi.remove();

    if (stream.callbackError) {
        try {
            std::rethrow_exception(stream.callbackError);
        } catch (std::exception const& e) {
            throw std::runtime_error(std::string{"Failed to process DeepSeek stream: "}
                                     + e.what());
        } catch (...) {
            throw std::runtime_error("Failed to process DeepSeek stream.");
        }
    }
    if (transferResult != CURLE_OK)
        throw std::runtime_error("DeepSeek request failed: "
                                 + curlError(transferResult, errorBuffer));
    if (stream.statusCode != 200)
        throw std::runtime_error("DeepSeek HTTP error " + std::to_string(stream.statusCode)
                                 + ": " + stream.errorBody);

    if (streaming) {
        stream.parser.finish();
        return stream.parser.message();
    }

    JsonParser parser{stream.responseBody};
    auto response = parser.parse();
    auto const& choices = response["choices"];
    if (!choices.isArray() || choices.asArray().empty())
        throw std::runtime_error("DeepSeek returned no choices: " + stream.responseBody);
    auto const& message = choices.asArray().front()["message"];
    if (!message.isObject())
        throw std::runtime_error("DeepSeek response has no assistant message: "
                                 + stream.responseBody);
    return message;
}

JsonValue makeRequest(std::string const& model, std::vector<JsonValue> const& messages,
                      JsonValue const& tools, bool streaming) {
    return std::map<std::string, JsonValue>{
        {"model", model},
        {"messages", messages},
        {"tools", tools},
        {"tool_choice", "auto"},
        {"stream", streaming},
        {"thinking", std::map<std::string, JsonValue>{{"type", "disabled"}}},
    };
}

bool getStreamMode() {
    auto const value = std::getenv("DEEPSEEK_STREAM");
    if (value == nullptr || *value == '\0' || std::string{value} == "true")
        return true;
    if (std::string{value} == "false")
        return false;
    throw std::runtime_error("DEEPSEEK_STREAM must be 'true' or 'false'.");
}

int runDeepSeek() {
    auto apiKey = std::getenv("DEEPSEEK_API_KEY");
    if (apiKey == nullptr || *apiKey == '\0') {
        std::cerr << "请先设置 DEEPSEEK_API_KEY 环境变量。\n";
        return EXIT_FAILURE;
    }
    CurlGlobal curlGlobal;
    auto const streaming = getStreamMode();

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
        auto payload = makeRequest(model, messages, tools, streaming).toString();
        auto message = postDeepSeek(apiKey, payload, streaming);
        auto const& toolCalls = message["tool_calls"];
        if (!toolCalls.isArray() || toolCalls.asArray().empty()) {
            if (!streaming) {
                auto const& content = message["content"];
                if (content.isString())
                    std::cout << "DeepSeek: " << content.asString() << '\n';
                else
                    std::cout << "DeepSeek: " << message.toString() << '\n';
            }
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
