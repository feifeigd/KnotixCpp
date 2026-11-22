#pragma once

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

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace test {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalidSocket = INVALID_SOCKET;
inline void closeSocket(socket_t fd) {
    closesocket(fd);
}
inline void shutdownSocket(socket_t fd) {
    shutdown(fd, SD_BOTH);
}
#else
using socket_t = int;
constexpr socket_t invalidSocket = -1;
inline void closeSocket(socket_t fd) {
    close(fd);
}
inline void shutdownSocket(socket_t fd) {
    shutdown(fd, SHUT_RDWR);
}
#endif

inline void setEnv(char const* name, char const* value) {
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

struct MockResponse {
    std::string statusLine;
    std::string contentType;
    std::string body;
};

// Minimal HTTP server on 127.0.0.1 that serves one canned response per
// incoming connection, in order, and captures every request.
class MockHttpServer {
public:
    explicit MockHttpServer(std::vector<MockResponse> responses)
        : responses_{std::move(responses)} {
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
        if (listen(listener_, SOMAXCONN) != 0)
            throw std::runtime_error("Failed to listen on the mock server socket.");
        socklen_t len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            throw std::runtime_error("Failed to query the mock server port.");
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    MockHttpServer(MockResponse response) : MockHttpServer(std::vector{std::move(response)}) {
    }

    ~MockHttpServer() {
        // Unblock accept() so the worker thread can exit.
        shutdownSocket(listener_);
        closeSocket(listener_);
        if (worker_.joinable())
            worker_.join();
    }

    uint16_t port() const {
        return port_;
    }

    size_t requestCount() const {
        std::lock_guard lock{mutex_};
        return requests_.size();
    }

    // Blocks until at least `count` requests were captured.
    std::vector<std::string> waitForRequests(size_t count) const {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this, count] { return requests_.size() >= count; });
        return requests_;
    }

private:
    void serve() {
        for (size_t served = 0; served < responses_.size(); ++served) {
            auto conn = accept(listener_, nullptr, nullptr);
            if (conn == invalidSocket)
                return;
            handleConnection(conn, responses_[served]);
            closeSocket(conn);
        }
    }

    void handleConnection(socket_t conn, MockResponse const& response) {
        std::string request;
        char buffer[4096];
        // Read until the header block is complete.
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto const n = recv(conn, buffer, sizeof(buffer), 0);
            if (n <= 0)
                return;
            request.append(buffer, static_cast<size_t>(n));
        }
        // Read the request body announced by Content-Length.
        auto bodyStart = request.find("\r\n\r\n") + 4;
        size_t contentLength = 0;
        if (auto pos = request.find("Content-Length:"); pos != std::string::npos)
            contentLength = std::stoul(request.substr(pos + 15));
        while (request.size() - bodyStart < contentLength) {
            auto const n = recv(conn, buffer, sizeof(buffer), 0);
            if (n <= 0)
                break;
            request.append(buffer, static_cast<size_t>(n));
        }
        {
            std::lock_guard lock{mutex_};
            requests_.push_back(request);
        }
        cv_.notify_all();

        auto const bytes = "HTTP/1.1 " + response.statusLine + "\r\n"
                           + "Content-Type: " + response.contentType + "\r\n"
                           + "Content-Length: " + std::to_string(response.body.size())
                           + "\r\nConnection: close\r\n\r\n" + response.body;
        size_t sent = 0;
        while (sent < bytes.size()) {
            auto const n = send(conn, bytes.data() + sent,
                                static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0)
                break;
            sent += static_cast<size_t>(n);
        }
    }

    std::vector<MockResponse> responses_;
    socket_t listener_ = invalidSocket;
    uint16_t port_ = 0;
    std::thread worker_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<std::string> requests_;
};

} // namespace test
