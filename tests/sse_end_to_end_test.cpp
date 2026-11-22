#include "openai_compatible_client.hpp"

#include "check_json.hpp"

#include "caf/test/test.hpp"

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/net/middleman.hpp>
#include <caf/net/multiplexer.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using agent::ProviderConfig;

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalidSocket = INVALID_SOCKET;
void closeSocket(socket_t fd) {
    closesocket(fd);
}
#else
using socket_t = int;
constexpr socket_t invalidSocket = -1;
void closeSocket(socket_t fd) {
    close(fd);
}
#endif

void setEnv(char const* name, char const* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct SocketRuntime {
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("WSAStartup failed.");
#endif
    }
    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

// Minimal one-shot HTTP server on 127.0.0.1 that captures the request and
// replies with a canned response.
class MockHttpServer {
public:
    MockHttpServer(std::string statusLine, std::string contentType, std::string body)
        : statusLine_{std::move(statusLine)},
          contentType_{std::move(contentType)},
          body_{std::move(body)} {
        // Winsock must be initialized before the first socket() call.
        static SocketRuntime const runtime;
        static_cast<void>(runtime);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == invalidSocket)
            throw std::runtime_error("Failed to create the mock server socket.");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // let the OS pick a free port
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("Failed to bind the mock server socket.");
        if (listen(listener_, 1) != 0)
            throw std::runtime_error("Failed to listen on the mock server socket.");
        socklen_t len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            throw std::runtime_error("Failed to query the mock server port.");
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { serveOnce(); });
    }

    ~MockHttpServer() {
        if (worker_.joinable())
            worker_.join();
        closeSocket(listener_);
    }

    uint16_t port() const {
        return port_;
    }

    // Blocks until the request was captured; empty on failure.
    std::string capturedRequest() const {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this] { return requestReceived_; });
        return request_;
    }

private:
    void serveOnce() {
        auto conn = accept(listener_, nullptr, nullptr);
        if (conn == invalidSocket)
            return;
        std::string request;
        char buffer[4096];
        // Read until the header block is complete.
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto const n = recv(conn, buffer, sizeof(buffer), 0);
            if (n <= 0)
                break;
            request.append(buffer, static_cast<size_t>(n));
        }
        // Read the request body announced by Content-Length.
        auto bodyStart = request.find("\r\n\r\n");
        size_t contentLength = 0;
        {
            auto pos = request.find("Content-Length:");
            if (pos != std::string::npos)
                contentLength = std::stoul(request.substr(pos + 15));
        }
        if (bodyStart != std::string::npos) {
            bodyStart += 4;
            while (request.size() - bodyStart < contentLength) {
                auto const n = recv(conn, buffer, sizeof(buffer), 0);
                if (n <= 0)
                    break;
                request.append(buffer, static_cast<size_t>(n));
            }
        }
        {
            std::lock_guard lock{mutex_};
            request_ = request;
            requestReceived_ = true;
        }
        cv_.notify_all();

        auto response = "HTTP/1.1 " + statusLine_ + "\r\n"
                        + "Content-Type: " + contentType_ + "\r\n"
                        + "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                        + "Connection: close\r\n\r\n" + body_;
        size_t sent = 0;
        while (sent < response.size()) {
            auto const n = send(conn, response.data() + sent,
                                static_cast<int>(response.size() - sent), 0);
            if (n <= 0)
                break;
            sent += static_cast<size_t>(n);
        }
        closeSocket(conn);
    }

    std::string statusLine_;
    std::string contentType_;
    std::string body_;
    socket_t listener_ = invalidSocket;
    uint16_t port_ = 0;
    std::thread worker_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::string request_;
    bool requestReceived_ = false;
};

struct CompletionResult {
    json message;
    std::string error;
};

// Runs one chat completion against `baseUrl` on a throwaway actor system and
// returns the callback result; std::nullopt on timeout.
std::optional<CompletionResult> runChatCompletion(ProviderConfig provider,
                                                  std::string const& payload) {
    // Make sure libcurl talks to the mock server directly even when the
    // environment configures a proxy.
    setEnv("no_proxy", "127.0.0.1,localhost");
    setEnv("NO_PROXY", "127.0.0.1,localhost");
    caf::actor_system_config cfg;
    cfg.load<caf::net::middleman>();
    caf::actor_system sys{cfg};
    auto* mpx = caf::net::multiplexer::from(sys);
    if (mpx == nullptr)
        throw std::runtime_error("CAF network multiplexer is unavailable.");

    agent::CurlRuntime curlRuntime;
    std::promise<CompletionResult> completion;
    auto result = completion.get_future();
    agent::requestChatCompletion(
        *mpx, std::move(provider), payload,
        [&completion](json message, std::string error) {
            completion.set_value({std::move(message), std::move(error)});
        });
    if (result.wait_for(30s) != std::future_status::ready)
        return std::nullopt;
    return result.get();
}

ProviderConfig mockProvider(uint16_t port, bool streaming) {
    ProviderConfig provider;
    provider.name = "mock";
    provider.baseUrl = "http://127.0.0.1:" + std::to_string(port);
    provider.model = "mock-model";
    provider.streaming = streaming;
    provider.apiKey = "test-key";
    return provider;
}

const json requestPayload = json::object({
    {"model", "mock-model"},
    {"messages", json::array({json::object({{"role", "user"}, {"content", "hi"}})})},
});

} // namespace

SUITE("agent.openai_compatible_client") {

TEST("a streamed tool call is assembled end to end") {
    auto const sseBody
        = std::string{"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                      "\"id\":\"call_1\",\"function\":{\"name\":\"add\"}}]}}]}\n\n"}
          + "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"{\\\"a\\\":\"}}]}}]}\n\n"
          + "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"1,\\\"b\\\":2}\"}}]}}]}\n\n"
          + "data: [DONE]\n\n";
    MockHttpServer server{"200 OK", "text/event-stream", sseBody};
    auto const result = runChatCompletion(mockProvider(server.port(), true),
                                          requestPayload.dump());
    require(result.has_value());
    require(result->error.empty());
    auto const& toolCalls = jsonGet(result->message, "tool_calls");
    require(toolCalls.is_array());
    require_eq(toolCalls.size(), size_t{1});
    auto const& call = toolCalls.front();
    check_json_eq(jsonGet(call, "id"), json("call_1"));
    check_json_eq(jsonGet(jsonGet(call, "function"), "name"), json("add"));
    check_json_eq(jsonGet(jsonGet(call, "function"), "arguments"),
                  json("{\"a\":1,\"b\":2}"));

    auto const request = server.capturedRequest();
    check(request.find("POST /chat/completions HTTP/1.1") == 0);
    check(request.find("Authorization: Bearer test-key") != std::string::npos);
    check(request.find("Accept: text/event-stream") != std::string::npos);
}

TEST("a non-streamed response returns the assistant message") {
    auto const body = json::object({
        {"choices",
         json::array({json::object({
             {"message", json::object({{"role", "assistant"}, {"content", "pong"}})},
         })})},
    }).dump();
    MockHttpServer server{"200 OK", "application/json", body};
    auto const result = runChatCompletion(mockProvider(server.port(), false),
                                          requestPayload.dump());
    require(result.has_value());
    require(result->error.empty());
    check_json_eq(jsonGet(result->message, "content"), json("pong"));

    auto const request = server.capturedRequest();
    check(request.find("Accept: application/json") != std::string::npos);
}

TEST("an HTTP error surfaces status code and error body") {
    MockHttpServer server{"500 Internal Server Error", "application/json",
                          R"({"error":"backend exploded"})"};
    auto const result = runChatCompletion(mockProvider(server.port(), true),
                                          requestPayload.dump());
    require(result.has_value());
    require(!result->error.empty());
    check(result->error.find("500") != std::string::npos);
    check(result->error.find("backend exploded") != std::string::npos);
}

TEST("a stream without a DONE event is reported as an error") {
    MockHttpServer server{"200 OK", "text/event-stream",
                          "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n"};
    auto const result = runChatCompletion(mockProvider(server.port(), true),
                                          requestPayload.dump());
    require(result.has_value());
    require(!result->error.empty());
    check(result->error.find("[DONE]") != std::string::npos);
}

} // SUITE("agent.openai_compatible_client")
