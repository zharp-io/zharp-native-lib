// A stand-in for Zharp's OTLP endpoint on 127.0.0.1: keeps each POST's path, headers and body.
#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET socket_t;
#  define CLOSE_SOCKET closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int socket_t;
#  define INVALID_SOCKET (-1)
#  define CLOSE_SOCKET close
#endif

/** A port on 127.0.0.1 that refuses connections: bound (so nothing else gets it) but not listening. */
class ClosedPort {
public:
    ClosedPort() {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        socket_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(socket_, (sockaddr *)&address, sizeof address);
        socklen_t size = sizeof address;
        getsockname(socket_, (sockaddr *)&address, &size);
        port_ = ntohs(address.sin_port);
    }
    ~ClosedPort() { CLOSE_SOCKET(socket_); }
    ClosedPort(const ClosedPort &) = delete;
    ClosedPort &operator=(const ClosedPort &) = delete;
    int port() const { return port_; }

private:
    socket_t socket_;
    int port_ = 0;
};

struct Request {
    std::string path;
    std::string headers;
    std::string body;
};

class Receiver {
public:
    explicit Receiver(int port = 0, int status = 200) : status_(status) {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((unsigned short)port);
        bind(listener_, (sockaddr *)&address, sizeof address);
        listen(listener_, 16);
        socklen_t size = sizeof address;
        getsockname(listener_, (sockaddr *)&address, &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~Receiver() { stop(); }

    void stop() {
        if (stopped_.exchange(true)) return;
        shutdown(listener_, 2);  // wakes accept() on Linux, where close() alone does not
        CLOSE_SOCKET(listener_);
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }

    std::vector<Request> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    /** All bodies POSTed to a path ending with `suffix`, joined. */
    std::string bodies(const std::string &suffix) {
        std::string out;
        for (const Request &r : requests()) {
            if (r.path.size() >= suffix.size() && r.path.compare(r.path.size() - suffix.size(), suffix.size(), suffix) == 0) out += r.body + "\n";
        }
        return out;
    }

    /** Waits until `predicate` holds, up to `ms`. */
    template <class P>
    bool wait(P predicate, int ms) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return predicate();
    }

private:
    void serve() {
        while (!stopped_) {
            socket_t client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            std::string data;
            char buffer[65536];
            size_t header_end = std::string::npos;
            size_t length = 0;
            for (;;) {
                int n = (int)recv(client, buffer, sizeof buffer, 0);
                if (n <= 0) break;
                data.append(buffer, (size_t)n);
                if (header_end == std::string::npos) {
                    header_end = data.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        std::string head = data.substr(0, header_end);
                        for (char &c : head) c = (char)tolower((unsigned char)c);
                        size_t at = head.find("content-length:");
                        if (at != std::string::npos) length = (size_t)strtoul(head.c_str() + at + 15, nullptr, 10);
                    }
                }
                if (header_end != std::string::npos && data.size() >= header_end + 4 + length) break;
            }
            Request request;
            if (header_end != std::string::npos) {
                size_t line_end = data.find("\r\n");
                std::string line = data.substr(0, line_end);
                size_t a = line.find(' '), b = line.rfind(' ');
                if (a != std::string::npos && b > a) request.path = line.substr(a + 1, b - a - 1);
                request.headers = data.substr(0, header_end);
                request.body = data.substr(header_end + 4, length);
            }
            {
                // Kept before the reply: once the SDK hears back, the request is here.
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            std::string reply = "HTTP/1.1 " + std::to_string(status_) + " OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, reply.data(), (int)reply.size(), 0);
            CLOSE_SOCKET(client);
        }
    }

    socket_t listener_;
    int port_ = 0;
    int status_;
    std::atomic<bool> stopped_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Request> requests_;
};
