#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>

namespace hyd {

struct ApiRequest
{
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
    std::string remote_address;
};

struct ApiResponse
{
    ApiResponse() = default;
    ApiResponse(int status_code, std::string type, std::string payload)
        : status(status_code), content_type(std::move(type)), body(std::move(payload))
    {
    }

    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::map<std::string, std::string> headers; // extra response headers
};

using ApiHandler = std::function<ApiResponse(const ApiRequest&)>;

class ApiServer
{
public:
    ApiServer(std::string address, int port, std::size_t max_connections, ApiHandler handler);
    ~ApiServer();

    bool start(std::string& error);
    void stop();

private:
    void accept_loop();
    void handle_connection(int socket_fd, std::string remote_address);

    std::string address_;
    int port_ = 0;
    std::size_t max_connections_ = 128;
    ApiHandler handler_;
    int listen_fd_ = -1;
    std::atomic<bool> running_ { false };
    std::thread accept_thread_;
    std::atomic<int> active_ { 0 };
    static constexpr int kDefaultMaxConcurrent = 128;
};

} // namespace hyd
