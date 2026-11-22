#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

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
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

using ApiHandler = std::function<ApiResponse(const ApiRequest&)>;

class ApiServer
{
public:
    ApiServer(std::string address, int port, ApiHandler handler);
    ~ApiServer();

    bool start(std::string& error);
    void stop();

private:
    void accept_loop();
    void handle_connection(int socket_fd, std::string remote_address);

    std::string address_;
    int port_ = 0;
    ApiHandler handler_;
    int listen_fd_ = -1;
    std::atomic<bool> running_ { false };
    std::thread accept_thread_;
};

} // namespace hyd
