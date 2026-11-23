#pragma once

#include "common/util.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hyd {

struct HttpResponse
{
    long status = 0;
    Bytes body;
};

class HttpClient
{
public:
    HttpClient(std::string base_url, std::string access_key, std::string user_agent);

    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    bool get(const std::string& path_and_query, HttpResponse& response, std::string& error);

    void set_timeout_seconds(long seconds) { timeout_seconds_ = seconds; }
    void set_rate_limit_kib(std::uint64_t kib_per_second) { rate_limit_kib_ = kib_per_second; }
    void set_retries(int retries) { retries_ = retries; }
    void set_api_access_key(std::string key) { api_access_key_ = std::move(key); }

    // Bound a single response body. Metadata and update files use different
    // limits; the default is the largest accepted update file.
    void set_max_body_bytes(std::size_t bytes) { max_body_bytes_ = bytes; }

    static void global_init();
    static void global_cleanup();

private:
    bool perform(const std::string& url, HttpResponse& response, std::string& error);

    std::string base_url_;
    std::string access_key_;
    std::string api_access_key_;
    CURL* handle_ = nullptr;
    std::string user_agent_;
    long timeout_seconds_ = 120;
    std::uint64_t rate_limit_kib_ = 0;
    int retries_ = 3;
    std::size_t max_body_bytes_ = 256u * 1024u * 1024u;
};

} // namespace hyd
