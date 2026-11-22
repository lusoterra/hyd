#include "hydrus/http.hpp"

#include "common/log.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace hyd {

namespace {

std::atomic<bool> g_curl_initialized { false };
std::once_flag g_curl_init_once;

std::size_t write_callback(char* data, std::size_t size, std::size_t count, void* userdata)
{
    auto* body = static_cast<Bytes*>(userdata);
    const std::size_t total = size * count;
    body->insert(body->end(),
        reinterpret_cast<std::uint8_t*>(data),
        reinterpret_cast<std::uint8_t*>(data) + total);
    return total;
}

bool is_retryable_status(long status)
{
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

} // namespace

void HttpClient::global_init()
{
    g_curl_initialized.store(true);
    std::call_once(g_curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

void HttpClient::global_cleanup()
{
    if (g_curl_initialized.exchange(false))
    {
        curl_global_cleanup();
    }
}

HttpClient::HttpClient(std::string base_url, std::string access_key, std::string user_agent)
    : base_url_(std::move(base_url)),
      access_key_(std::move(access_key)),
      user_agent_(std::move(user_agent))
{
    while (!base_url_.empty() && base_url_.back() == '/')
    {
        base_url_.pop_back();
    }
    global_init();
}

HttpClient::~HttpClient()
{
    if (handle_ != nullptr)
    {
        curl_easy_cleanup(handle_);
        handle_ = nullptr;
    }
}

bool HttpClient::perform(const std::string& url, HttpResponse& response, std::string& error)
{
    if (handle_ == nullptr)
    {
        handle_ = curl_easy_init();
    }

    if (handle_ == nullptr)
    {
        error = "curl_easy_init failed";
        return false;
    }

    // Keep the same easy handle so libcurl can reuse the TLS connection. This
    // is the difference between one connection per update file and one per
    // worker for the whole sync.
    curl_easy_reset(handle_);
    CURL* handle = handle_;

    response.body.clear();
    response.status = 0;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Hydrus-Key: " + access_key_).c_str());
    if (!api_access_key_.empty())
    {
        headers = curl_slist_append(headers,
            ("Hydrus-Client-API-Access-Key: " + api_access_key_).c_str());
    }
    headers = curl_slist_append(headers, "Accept: application/octet-stream");

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_seconds_);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, user_agent_.c_str());
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");

    if (rate_limit_kib_ > 0)
    {
        curl_easy_setopt(handle, CURLOPT_MAX_RECV_SPEED_LARGE,
            static_cast<curl_off_t>(rate_limit_kib_ * 1024));
    }

    char error_buffer[CURL_ERROR_SIZE];
    error_buffer[0] = '\0';
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);

    const CURLcode result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);

    if (result != CURLE_OK)
    {
        error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(result);
    }
    else
    {
        error.clear();
    }

    curl_slist_free_all(headers);

    return result == CURLE_OK;
}

bool HttpClient::get(const std::string& path_and_query, HttpResponse& response, std::string& error)
{
    const std::string url = base_url_ + path_and_query;

    std::string last_error;
    for (int attempt = 0; attempt <= retries_; ++attempt)
    {
        if (attempt > 0)
        {
            const int delay = std::min(30, 1 << attempt);
            HYD_LOGF(warn, "retrying %s in %ds (attempt %d/%d): %s",
                path_and_query.c_str(), delay, attempt + 1, retries_ + 1, last_error.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }

        if (perform(url, response, last_error))
        {
            if (response.status == 200)
            {
                return true;
            }

            if (!is_retryable_status(response.status))
            {
                char body_preview[512] = {};
                const std::size_t preview = std::min<std::size_t>(response.body.size(), sizeof(body_preview) - 1);
                std::memcpy(body_preview, response.body.data(), preview);
                error = "HTTP " + std::to_string(response.status) + ": " + trim(body_preview);
                return false;
            }

            last_error = "HTTP " + std::to_string(response.status);
        }
    }

    error = last_error;
    return false;
}

} // namespace hyd
