#include "api/server.hpp"

#include "common/log.hpp"
#include "common/util.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <thread>

namespace hyd {

namespace {

constexpr std::size_t kMaxHeaderBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxBodyBytes = 16 * 1024 * 1024;

std::string lower(std::string_view value)
{
    return to_lower(value);
}

std::string peer_address(const sockaddr_storage& address)
{
    char buffer[INET6_ADDRSTRLEN] = {};
    if (address.ss_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) == nullptr)
        {
            return {};
        }
    }
    else if (address.ss_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr))
        {
            if (::inet_ntop(AF_INET, &ipv6->sin6_addr.s6_addr[12], buffer, sizeof(buffer)) == nullptr)
            {
                return {};
            }
        }
        else if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer)) == nullptr)
        {
            return {};
        }
    }
    return buffer;
}

std::string status_text(int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 413: return "Payload Too Large";
        case 419: return "Authentication Timeout";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

bool send_all(int fd, const char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        const ssize_t written = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

ApiServer::ApiServer(std::string address, int port, ApiHandler handler)
    : address_(std::move(address)), port_(port), handler_(std::move(handler))
{
}

ApiServer::~ApiServer()
{
    stop();
}

bool ApiServer::start(std::string& error)
{
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = nullptr;
    const std::string port_string = std::to_string(port_);
    const int lookup = ::getaddrinfo(address_.empty() ? nullptr : address_.c_str(), port_string.c_str(),
        &hints, &result);
    if (lookup != 0)
    {
        error = "cannot resolve listen address: " + std::string(gai_strerror(lookup));
        return false;
    }

    int fd = -1;
    for (auto* candidate = result; candidate != nullptr; candidate = candidate->ai_next)
    {
        fd = ::socket(candidate->ai_family, candidate->ai_socktype | SOCK_CLOEXEC, candidate->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (::bind(fd, candidate->ai_addr, candidate->ai_addrlen) == 0 && ::listen(fd, 64) == 0)
        {
            break;
        }

        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(result);

    if (fd < 0)
    {
        error = "cannot bind " + address_ + ":" + port_string + ": " + errno_string(errno);
        return false;
    }

    listen_fd_ = fd;
    running_.store(true);
    accept_thread_ = std::thread(&ApiServer::accept_loop, this);

    HYD_LOGF(info, "API listening on %s:%d", address_.c_str(), port_);
    return true;
}

void ApiServer::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    if (listen_fd_ >= 0)
    {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable())
    {
        accept_thread_.join();
    }
}

void ApiServer::accept_loop()
{
    while (running_.load())
    {
        sockaddr_storage address {};
        socklen_t length = sizeof(address);
        const int client = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length, SOCK_CLOEXEC);
        if (client < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (running_.load())
            {
                HYD_LOGF(warn, "accept failed: %s", errno_string(errno).c_str());
            }
            break;
        }

        std::thread(&ApiServer::handle_connection, this, client, peer_address(address)).detach();
    }
}

void ApiServer::handle_connection(int socket_fd, std::string remote_address)
{
    struct timeval timeout {};
    timeout.tv_sec = 60;
    timeout.tv_usec = 0;
    ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string request;
    request.reserve(8192);

    char buffer[8192];
    std::size_t header_end = std::string::npos;

    while (header_end == std::string::npos)
    {
        const ssize_t received = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            ::close(socket_fd);
            return;
        }
        request.append(buffer, static_cast<std::size_t>(received));
        header_end = request.find("\r\n\r\n");

        if (request.size() > kMaxHeaderBytes)
        {
            const std::string response =
                "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(socket_fd, response.data(), response.size());
            ::close(socket_fd);
            return;
        }
    }

    ApiRequest parsed;
    parsed.remote_address = std::move(remote_address);
    std::size_t body_start = header_end + 4;

    {
        const std::string_view head(request.data(), header_end);
        const std::size_t first_line_end = head.find("\r\n");
        if (first_line_end == std::string_view::npos)
        {
            ::close(socket_fd);
            return;
        }

        const std::string_view first_line = head.substr(0, first_line_end);
        const std::size_t first_space = first_line.find(' ');
        const std::size_t second_space = first_line.rfind(' ');
        if (first_space == std::string_view::npos || second_space == first_space)
        {
            ::close(socket_fd);
            return;
        }

        parsed.method = std::string(first_line.substr(0, first_space));
        std::string target(first_line.substr(first_space + 1, second_space - first_space - 1));

        const std::size_t question = target.find('?');
        if (question != std::string::npos)
        {
            parsed.path = percent_decode(target.substr(0, question));
            const std::string_view query(target.data() + question + 1, target.size() - question - 1);
            std::size_t start = 0;
            while (start <= query.size())
            {
                const std::size_t ampersand = query.find('&', start);
                const std::string_view part = ampersand == std::string_view::npos
                    ? query.substr(start)
                    : query.substr(start, ampersand - start);
                if (!part.empty())
                {
                    const std::size_t equals = part.find('=');
                    if (equals == std::string_view::npos)
                    {
                        parsed.query[percent_decode(part)] = "";
                    }
                    else
                    {
                        parsed.query[percent_decode(part.substr(0, equals))] =
                            percent_decode(part.substr(equals + 1));
                    }
                }
                if (ampersand == std::string_view::npos)
                {
                    break;
                }
                start = ampersand + 1;
            }
        }
        else
        {
            parsed.path = percent_decode(target);
        }

        std::size_t position = first_line_end + 2;
        while (position < head.size())
        {
            if (head.compare(position, 2, "\r\n") == 0)
            {
                break;
            }

            const std::size_t found_end = head.find("\r\n", position);
            const bool is_last = found_end == std::string_view::npos;
            const std::size_t line_end = is_last ? head.size() : found_end;

            const std::string_view line = head.substr(position, line_end - position);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos)
            {
                std::string name = lower(trim(line.substr(0, colon)));
                std::string value = trim(line.substr(colon + 1));
                parsed.headers[name] = value;
            }

            if (is_last)
            {
                break;
            }
            position = line_end + 2;
        }
    }

    HYD_LOGF(debug, "API request %s %s", parsed.method.c_str(), parsed.path.c_str());

    if (parsed.method == "OPTIONS")
    {
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Server: hydd/0.2\r\n"
            "Hydrus-Server: hydd/0.2\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Allow-Methods: GET, POST\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        send_all(socket_fd, response.data(), response.size());
        ::shutdown(socket_fd, SHUT_RDWR);
        ::close(socket_fd);
        return;
    }

    std::size_t content_length = 0;
    if (const auto iterator = parsed.headers.find("content-length"); iterator != parsed.headers.end())
    {
        const auto length = parse_u64(iterator->second);
        if (!length || *length > kMaxBodyBytes)
        {
            const std::string response =
                "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(socket_fd, response.data(), response.size());
            ::close(socket_fd);
            return;
        }
        content_length = static_cast<std::size_t>(*length);
    }

    while (request.size() < body_start + content_length)
    {
        const ssize_t received = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            ::close(socket_fd);
            return;
        }
        request.append(buffer, static_cast<std::size_t>(received));
    }

    parsed.body = request.substr(body_start, content_length);

    ApiResponse response;
    try
    {
        response = handler_(parsed);
    }
    catch (const std::exception& exception)
    {
        response.status = 500;
        response.content_type = "text/plain";
        response.body = std::string("Internal error: ") + exception.what();
        HYD_LOGF(error, "API handler exception: %s", exception.what());
    }

    std::string head;
    head += "HTTP/1.1 " + std::to_string(response.status) + " " + status_text(response.status) + "\r\n";
    head += "Server: hydd/0.2\r\n";
    head += "Hydrus-Server: hydd/0.2\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Access-Control-Allow-Headers: *\r\n";
    head += "Access-Control-Allow-Methods: GET, POST\r\n";
    head += "Content-Type: " + response.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    head += "Connection: close\r\n";
    head += "\r\n";

    send_all(socket_fd, head.data(), head.size());
    send_all(socket_fd, response.body.data(), response.body.size());

    ::shutdown(socket_fd, SHUT_RDWR);
    ::close(socket_fd);
}

} // namespace hyd
