#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "openai_compatible_client.hpp"

#include "sse_parser.hpp"

#include <curl/curl.h>

#include <caf/action.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agent {
namespace {

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

struct StreamContext {
    explicit StreamContext(bool useStreaming) : streaming{useStreaming} {}

    OpenAICompatibleSseParser parser;   // 流式回复的时候使用
    bool streaming; // 流式
    long statusCode = 0;
    std::string errorBody;
    std::string responseBody;
    std::exception_ptr callbackError;   // 发生了异常

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

// 给 curl 的回调函数
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
    return bytes;   // 消费了多少字节
}

// 给 curl 的回调函数
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
    return bytes;   // 消费了多少字节
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
    using completion_type = ChatCompletionCallback;

    CurlRequest(caf::net::multiplexer& mpx, ProviderConfig provider, std::string payload,
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
                      caf::make_single_shot_action([self] { self->startOnMultiplexer(); }));
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
                      caf::make_single_shot_action([self] { self->pollOnMultiplexer(); }));
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
                    finishTransfer(message->data.result);   // 请求回应结束
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
            if (stream_.callbackError) {    // 非正常结束
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

            json message;
            if (provider_.streaming) {
                stream_.parser.finish();
                message = stream_.parser.message();
            } else {
                auto response = json::parse(stream_.responseBody);
                auto const& choices = jsonGet(response, "choices");
                if (!choices.is_array() || choices.empty())
                    throw std::runtime_error("Provider returned no choices: "
                                             + stream_.responseBody);
                message = jsonGet(choices.front(), "message");
                if (!message.is_object())
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

    void finish(json message, std::string error) {
        cleanup();
        auto completion = std::move(completion_); // 只能回调一次
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

    caf::net::multiplexer& mpx_;
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

} // namespace

CurlRuntime::CurlRuntime() {
    auto const result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK)
        throw std::runtime_error(std::string{"curl_global_init failed: "}
                                 + curl_easy_strerror(result));
}

CurlRuntime::~CurlRuntime() {
    curl_global_cleanup();
}

void requestChatCompletion(caf::net::multiplexer& mpx, ProviderConfig const& provider,
                           std::string payload, ChatCompletionCallback completion) {
    auto request = std::make_shared<CurlRequest>(mpx, provider, std::move(payload),
                                                 std::move(completion));
    request->start();
}

} // namespace agent
