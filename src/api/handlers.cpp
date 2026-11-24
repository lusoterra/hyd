#include "api/handlers.hpp"

#include "common/log.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>

namespace hyd {

namespace {

constexpr int kClientApiVersion = 97;
constexpr int kHydrusVersion = 696;
constexpr const char* kAllKnownTagsKey = "616c6c206b6e6f776e2074616773";
constexpr std::int64_t kSessionLifetimeSeconds = 24 * 60 * 60;

bool is_loopback_address(std::string_view address)
{
    return address == "::1"
        || address.rfind("127.", 0) == 0
        || address.rfind("::ffff:127.", 0) == 0;
}

ApiResponse text_response(int status, std::string body, std::string content_type = "text/plain; charset=utf-8")
{
    ApiResponse response;
    response.status = status;
    response.content_type = std::move(content_type);
    response.body = std::move(body);
    return response;
}

std::string dump_json(const nlohmann::json& value)
{
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

nlohmann::json parse_param_value(const std::string& value)
{
    if (value.empty())
    {
        return std::string();
    }

    try
    {
        return nlohmann::json::parse(value);
    }
    catch (const std::exception&)
    {
        return value;
    }
}

nlohmann::json collect_params(const ApiRequest& request)
{
    nlohmann::json params = nlohmann::json::object();

    for (const auto& [key, value] : request.query)
    {
        params[key] = parse_param_value(value);
    }

    if (!request.body.empty())
    {
        const auto content_type = request.headers.find("content-type");
        if (content_type != request.headers.end()
            && to_lower(content_type->second).find("application/json") != std::string::npos)
        {
            try
            {
                const auto body = nlohmann::json::parse(request.body);
                if (body.is_object())
                {
                    for (auto iterator = body.begin(); iterator != body.end(); ++iterator)
                    {
                        params[iterator.key()] = iterator.value();
                    }
                }
            }
            catch (const std::exception& exception)
            {
                throw std::runtime_error(std::string("cannot parse JSON body: ") + exception.what());
            }
        }
    }

    return params;
}

bool json_param_bool(const nlohmann::json& params, const char* key, bool fallback)
{
    if (!params.contains(key))
    {
        return fallback;
    }
    const auto& value = params.at(key);
    if (value.is_boolean())
    {
        return value.get<bool>();
    }
    if (value.is_string())
    {
        const auto parsed = parse_bool(value.get<std::string>());
        return parsed ? *parsed : fallback;
    }
    if (value.is_number_integer())
    {
        return value.get<std::int64_t>() != 0;
    }
    return fallback;
}

std::optional<std::string> json_param_string(const nlohmann::json& params, const char* key)
{
    if (!params.contains(key))
    {
        return std::nullopt;
    }
    const auto& value = params.at(key);
    if (value.is_string())
    {
        return value.get<std::string>();
    }
    if (value.is_number_integer())
    {
        return std::to_string(value.get<std::int64_t>());
    }
    return std::nullopt;
}

bool json_param_i64(const nlohmann::json& params, const char* key, std::int64_t& out)
{
    if (!params.contains(key))
    {
        return false;
    }
    const auto& value = params.at(key);
    if (value.is_number_integer())
    {
        out = value.get<std::int64_t>();
        return true;
    }
    if (value.is_string())
    {
        const auto parsed = parse_i64(value.get<std::string>());
        if (parsed)
        {
            out = *parsed;
            return true;
        }
    }
    throw std::runtime_error(std::string(key) + " must be an integer");
}

std::vector<std::string> hashes_from_params(const nlohmann::json& params, bool& malformed)
{
    malformed = false;
    std::vector<std::string> result;
    std::set<std::string> seen;

    auto add = [&](const nlohmann::json& value) {
        if (value.is_string())
        {
            if (seen.insert(value.get<std::string>()).second)
            {
                result.push_back(value.get<std::string>());
            }
        }
        else if (value.is_array())
        {
            for (const auto& item : value)
            {
                if (!item.is_string())
                {
                    malformed = true;
                    return;
                }
                if (seen.insert(item.get<std::string>()).second)
                {
                    result.push_back(item.get<std::string>());
                }
            }
        }
        else
        {
            malformed = true;
        }
    };

    if (params.contains("hash"))
    {
        add(params.at("hash"));
    }
    if (params.contains("hashes"))
    {
        add(params.at("hashes"));
    }

    return result;
}

std::vector<std::string> tags_from_params(const nlohmann::json& params, bool& malformed)
{
    malformed = false;
    std::vector<std::string> result;
    std::set<std::string> seen;

    auto add = [&](const nlohmann::json& value) {
        if (value.is_string())
        {
            const auto text = value.get<std::string>();
            if (!text.empty() && seen.insert(text).second)
            {
                result.push_back(text);
            }
        }
        else if (value.is_array())
        {
            for (const auto& item : value)
            {
                if (!item.is_string())
                {
                    malformed = true;
                    return;
                }
                const auto text = item.get<std::string>();
                if (!text.empty() && seen.insert(text).second)
                {
                    result.push_back(text);
                }
            }
        }
        else
        {
            malformed = true;
        }
    };

    if (params.contains("tag"))
    {
        add(params.at("tag"));
    }
    if (params.contains("tags"))
    {
        add(params.at("tags"));
    }

    return result;
}

std::string method_path(const ApiRequest& request)
{
    std::string path = request.path;
    while (path.size() > 1 && path.back() == '/')
    {
        path.pop_back();
    }
    return path;
}

} // namespace

ApiHandlers::ApiHandlers(Config config, std::vector<std::string> access_keys, std::string service_key,
    std::string service_name, std::string version)
    : config_(std::move(config)),
      access_keys_(std::move(access_keys)),
      service_key_(std::move(service_key)),
      service_name_(std::move(service_name)),
      version_(std::move(version))
{
    boot_id_hex_ = hex_encode(random_bytes(32));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    boot_time_ = std::chrono::duration<double>(now).count();

    rate_limit_enabled_ = config_.get_bool("rate_limit_enabled", true);
    rate_limit_local_bypass_ = config_.get_bool("rate_limit_local_bypass", false);

    const auto per_minute = config_.get_int("rate_limit_requests_per_minute", 600);
    rate_per_second_ = per_minute > 0 ? static_cast<double>(per_minute) / 60.0 : 0.0;

    const auto burst = config_.get_int("rate_limit_burst", 120);
    rate_burst_ = static_cast<double>(std::max<std::int64_t>(1, burst));

    if (rate_limit_enabled_ && rate_per_second_ <= 0.0)
    {
        HYD_LOG(warn, "rate_limit_requests_per_minute is not positive; disabling rate limiting");
        rate_limit_enabled_ = false;
    }
}

void ApiHandlers::set_store(std::shared_ptr<const TagSource> store)
{
    store_.store(std::move(store));
}

void ApiHandlers::set_sync_state(const SyncState& state)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    sync_state_ = state;
}

void ApiHandlers::request_cache_repair(const Hash& hash)
{
    std::lock_guard<std::mutex> lock(repair_mutex_);
    if (repair_hashes_.size() >= 1024)
    {
        repair_full_ = true;
        return;
    }
    repair_hashes_.push_back(hash);
}

void ApiHandlers::request_cache_repair_full()
{
    std::lock_guard<std::mutex> lock(repair_mutex_);
    repair_full_ = true;
}

std::vector<Hash> ApiHandlers::take_cache_repair_hashes()
{
    std::lock_guard<std::mutex> lock(repair_mutex_);
    std::vector<Hash> hashes;
    hashes.swap(repair_hashes_);
    return hashes;
}

bool ApiHandlers::cache_repair_full_requested() const
{
    std::lock_guard<std::mutex> lock(repair_mutex_);
    return repair_full_;
}

void ApiHandlers::clear_cache_repair_full()
{
    std::lock_guard<std::mutex> lock(repair_mutex_);
    repair_full_ = false;
}

nlohmann::json ApiHandlers::base_response() const
{
    nlohmann::json root;
    root["version"] = kClientApiVersion;
    root["hydrus_version"] = kHydrusVersion;
    return root;
}

nlohmann::json ApiHandlers::services_dict() const
{
    nlohmann::json services = nlohmann::json::object();

    services[service_key_] = {
        { "name", service_name_ },
        { "type", 0 },
        { "type_pretty", "hydrus tag repository" },
    };

    services[kAllKnownTagsKey] = {
        { "name", "all known tags" },
        { "type", 10 },
        { "type_pretty", "combined tag domain" },
    };

    return services;
}

nlohmann::json ApiHandlers::services_list() const
{
    nlohmann::json services = nlohmann::json::array();

    services.push_back({
        { "name", service_name_ },
        { "type", 0 },
        { "type_pretty", "hydrus tag repository" },
        { "service_key", service_key_ },
    });

    services.push_back({
        { "name", "all known tags" },
        { "type", 10 },
        { "type_pretty", "combined tag domain" },
        { "service_key", kAllKnownTagsKey },
    });

    return services;
}

std::string ApiHandlers::access_key_from(const ApiRequest& request, const nlohmann::json& params) const
{
    // Repository clients send Hydrus-Key. Accept it for the Client API too.
    const auto repository_header = request.headers.find("hydrus-key");
    if (repository_header != request.headers.end())
    {
        return repository_header->second;
    }

    const auto access_header = request.headers.find("hydrus-client-api-access-key");
    if (access_header != request.headers.end())
    {
        return access_header->second;
    }

    const auto session_header = request.headers.find("hydrus-client-api-session-key");
    if (session_header != request.headers.end())
    {
        return session_header->second;
    }

    for (const char* key : { "Hydrus-Client-API-Access-Key", "Hydrus-Client-API-Session-Key" })
    {
        const auto iterator = request.query.find(key);
        if (iterator != request.query.end())
        {
            return iterator->second;
        }
    }

    for (const char* key : { "Hydrus-Client-API-Access-Key", "Hydrus-Client-API-Session-Key" })
    {
        const auto value = json_param_string(params, key);
        if (value)
        {
            return *value;
        }
    }

    return {};
}

bool ApiHandlers::authorize(const ApiRequest& request, const nlohmann::json& params, ApiResponse& failure)
{
    if (is_loopback_address(request.remote_address)
        && !config_.get_bool("require_localhost_key", false))
    {
        return true;
    }

    const std::string provided = access_key_from(request, params);
    if (provided.empty())
    {
        failure = text_response(401, "No access key provided.");
        return false;
    }

    auto constant_time_equal = [](const std::string& left, const std::string& right) {
        if (left.size() != right.size())
        {
            return false;
        }
        return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
    };

    bool access_key_matched = false;
    for (const auto& key : access_keys_)
    {
        access_key_matched |= constant_time_equal(provided, key);
    }

    if (access_key_matched)
    {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto iterator = sessions_.find(provided);
        if (iterator != sessions_.end())
        {
            if (iterator->second >= now_unix())
            {
                iterator->second = now_unix() + kSessionLifetimeSeconds;
                return true;
            }
            sessions_.erase(iterator);
            failure = text_response(419, "This session key has expired.");
            return false;
        }
    }

    failure = text_response(403, "Invalid access key.");
    return false;
}

ApiResponse ApiHandlers::handle(const ApiRequest& request)
{
    ApiResponse limited;
    if (!rate_limit(request, limited))
    {
        return limited;
    }

    try
    {
        return route(request);
    }
    catch (const std::exception& exception)
    {
        HYD_LOGF(error, "API error: %s", exception.what());
        return text_response(500, std::string("Internal error: ") + exception.what());
    }
}

std::string ApiHandlers::rate_limit_key(const ApiRequest& request) const
{
    for (const auto* name : { "hydrus-client-api-access-key", "hydrus-key",
             "hydrus-client-api-session-key" })
    {
        if (const auto iterator = request.headers.find(name); iterator != request.headers.end())
        {
            if (!iterator->second.empty())
            {
                return "key:" + iterator->second;
            }
        }
    }

    for (const auto* name : { "access_key", "session_key" })
    {
        if (const auto iterator = request.query.find(name); iterator != request.query.end())
        {
            if (!iterator->second.empty())
            {
                return "key:" + iterator->second;
            }
        }
    }

    return {};
}

bool ApiHandlers::rate_limit(const ApiRequest& request, ApiResponse& failure)
{
    if (!rate_limit_enabled_)
    {
        return true;
    }

    if (rate_limit_local_bypass_ && is_loopback_address(request.remote_address))
    {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto key = rate_limit_key(request);

    std::lock_guard<std::mutex> lock(rate_mutex_);

    if (rate_buckets_.size() >= kMaxRateBuckets)
    {
        for (auto iterator = rate_buckets_.begin(); iterator != rate_buckets_.end();)
        {
            if (now - iterator->second.last > kRateBucketIdle)
            {
                iterator = rate_buckets_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        // Still full: drop everything rather than growing without bound. The
        // worst case is a burst of fresh buckets for active clients.
        if (rate_buckets_.size() >= kMaxRateBuckets)
        {
            rate_buckets_.clear();
        }
    }

    // Charge the peer address always, and the access key too when one is
    // supplied. Bucketing only by key would let a client rotate made-up keys
    // to bypass the limit, since this check runs before authentication.
    std::string peer_key = "ip:" + request.remote_address;
    if (!consume_rate_token(peer_key, now, request, failure))
    {
        return false;
    }

    if (!key.empty() && !consume_rate_token(key, now, request, failure))
    {
        return false;
    }

    return true;
}

bool ApiHandlers::consume_rate_token(const std::string& key,
    const std::chrono::steady_clock::time_point& now, const ApiRequest& request,
    ApiResponse& failure)
{
    auto& bucket = rate_buckets_[key];
    if (bucket.last == std::chrono::steady_clock::time_point {})
    {
        bucket.tokens = rate_burst_;
    }
    else
    {
        const double elapsed = std::chrono::duration<double>(now - bucket.last).count();
        bucket.tokens = std::min(rate_burst_, bucket.tokens + elapsed * rate_per_second_);
    }
    bucket.last = now;

    if (bucket.tokens >= 1.0)
    {
        bucket.tokens -= 1.0;
        return true;
    }

    const double wait_seconds = (1.0 - bucket.tokens) / rate_per_second_;
    const auto retry_after = static_cast<std::int64_t>(std::ceil(wait_seconds)) + 1;

    failure = text_response(429, "rate limit exceeded; retry later");
    failure.headers["Retry-After"] = std::to_string(std::max<std::int64_t>(1, retry_after));
    HYD_LOGF(debug, "rate limit hit for %s (%.2f tokens)", request.remote_address.c_str(),
        bucket.tokens);
    return false;
}

ApiResponse ApiHandlers::route(const ApiRequest& request)
{
    const std::string path = method_path(request);

    if (request.method != "GET" && request.method != "POST")
    {
        return text_response(405, "Only GET and POST are supported.");
    }

    if (request.query.count("cbor") != 0)
    {
        return text_response(406, "hydd speaks JSON only; CBOR is not implemented.");
    }

    if (const auto content_type = request.headers.find("content-type");
        content_type != request.headers.end()
        && to_lower(content_type->second).find("application/cbor") != std::string::npos)
    {
        return text_response(406, "hydd speaks JSON only; CBOR is not implemented.");
    }

    if (const auto accept = request.headers.find("accept"); accept != request.headers.end()
        && to_lower(accept->second).find("application/cbor") != std::string::npos)
    {
        return text_response(406, "hydd speaks JSON only; CBOR is not implemented.");
    }

    if (path == "/" || path.empty())
    {
        return text_response(200,
            "<!DOCTYPE html><html><head><title>hyd</title></head><body>"
            "<h1>hyd</h1>"
            "<p>read-only Hydrus Public Tag Repository mirror</p>"
            "<p><a href=\"https://github.com/lusoterra/hyd\">github.com/lusoterra/hyd</a></p>"
            "</body></html>",
            "text/html; charset=utf-8");
    }

    if (path == "/api_version")
    {
        auto body = base_response();
        body["hyd_version"] = version_;
        return ApiResponse { 200, "application/json", dump_json(body) };
    }

    if (path == "/health")
    {
        return ApiResponse { 200, "application/json", dump_json(health_json()) };
    }

    if (path == "/hyd/stats")
    {
        ApiResponse failure;
        if (!authorize(request, nlohmann::json::object(), failure))
        {
            return failure;
        }
        return ApiResponse { 200, "application/json", dump_json(health_json()) };
    }

    if (path == "/stats")
    {
        return text_response(404,
            "hydd extension moved under /hyd/ in 0.8.0: use /hyd/stats.");
    }

    if (path == "/metadata")
    {
        return handle_repository_metadata(request);
    }

    if (path == "/update")
    {
        return handle_repository_update(request);
    }

    nlohmann::json params;
    try
    {
        params = collect_params(request);
    }
    catch (const std::exception& exception)
    {
        return text_response(400, exception.what());
    }

    if (path == "/session_key")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_session_key(request);
    }

    if (path == "/verify_access_key")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_verify_access_key();
    }

    if (path == "/client_info")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_client_info();
    }

    if (path == "/get_services")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_services(request);
    }

    if (path == "/get_service")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_service(request, params);
    }

    if (path == "/get_files/file_metadata")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_file_metadata(request, params);
    }

    if (path == "/hyd/tag_counts")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_tag_counts(request, params);
    }

    if (path == "/hyd/tag_hashes")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_tag_hashes(request, params);
    }

    if (path == "/get_files/file_hashes")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_file_hashes(request, params);
    }

    // Known Hydrus endpoints this mirror deliberately does not implement get
    // an explicit 501 instead of a generic 404, so clients can tell a missing
    // feature from a wrong URL.
    auto not_implemented = [&](const char* reason) {
        auto body = base_response();
        body["error"] = "not implemented";
        body["path"] = path;
        body["reason"] = reason;
        return ApiResponse { 501, "application/json", dump_json(body) };
    };

    if (path == "/get_files/file" || path == "/get_files/thumbnail"
        || path == "/get_files/render" || path == "/get_files/file_seed")
    {
        return not_implemented("the mirror stores tags only; files and thumbnails are not served");
    }

    if (path.rfind("/add_", 0) == 0 || path.rfind("/edit_", 0) == 0
        || path.rfind("/manage_", 0) == 0 || path == "/get_files/search_files"
        || path == "/request_new_permissions" || path == "/commit_pending")
    {
        return not_implemented("the mirror is read-only; search and mutation endpoints are not supported");
    }

    auto body = base_response();
    body["error"] = "unknown endpoint";
    body["path"] = path;
    return ApiResponse { 404, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_file_metadata(const ApiRequest& request, const nlohmann::json& params)
{
    const auto store = store_.load();
    if (!store)
    {
        return text_response(503,
            "hydd is still building its initial Public Tag Repository snapshot; try again later.");
    }

    bool malformed = false;
    const auto encoded_hashes = hashes_from_params(params, malformed);
    if (malformed)
    {
        return text_response(400, "hashes must be a string or a list of strings.");
    }
    if (encoded_hashes.empty())
    {
        return text_response(400, "You have to specify a hash or hashes to look up.");
    }
    if (encoded_hashes.size() > 1000)
    {
        return text_response(400, "too many hashes in one request (max 1000).");
    }

    const bool identifiers_only = json_param_bool(params, "only_return_identifiers", false);
    const bool basic_only = json_param_bool(params, "only_return_basic_information", false);
    const bool include_notes = json_param_bool(params, "include_notes", false);
    const bool detailed_urls = json_param_bool(params, "detailed_url_information", false);
    const bool include_services = json_param_bool(params, "include_services_object", true);

    nlohmann::json metadata = nlohmann::json::array();

    for (const auto& encoded : encoded_hashes)
    {
        const auto hash = hash_bytes_from_hex(encoded);
        if (!hash)
        {
            return text_response(400, "Invalid file hash (expected sha256, sha1, or md5): " + encoded);
        }

        const auto result = store->lookup(*hash);
        if (!result.found || result.deleted)
        {
            metadata.push_back({
                { "file_id", nullptr },
                { "hash", encoded },
            });
            continue;
        }

        metadata.push_back(metadata_row(*hash, result, identifiers_only, basic_only,
            include_notes, detailed_urls));
    }

    auto body = base_response();
    body["metadata"] = std::move(metadata);

    if (include_services)
    {
        body["services"] = services_dict();
        body["services_v2"] = services_list();
    }

    return ApiResponse { 200, "application/json", dump_json(body) };
}

nlohmann::json ApiHandlers::metadata_row(std::span<const std::uint8_t> hash, const TagQueryResult& result,
    bool identifiers_only, bool basic_only, bool include_notes, bool detailed_urls) const
{
    nlohmann::json row;
    row["file_id"] = result.hash_id;
    row["hash"] = hex_encode(hash);

    if (identifiers_only)
    {
        return row;
    }

    row["size"] = nullptr;
    row["mime"] = nullptr;
    row["filetype_human"] = nullptr;
    row["filetype_enum"] = nullptr;
    row["ext"] = nullptr;
    row["width"] = nullptr;
    row["height"] = nullptr;
    row["duration"] = nullptr;
    row["num_frames"] = nullptr;
    row["num_words"] = nullptr;
    row["has_audio"] = false;
    row["filetype_forced"] = false;

    if (basic_only)
    {
        return row;
    }

    row["blurhash"] = nullptr;
    row["pixel_hash"] = nullptr;
    row["file_services"] = { { "current", nlohmann::json::object() }, { "deleted", nlohmann::json::object() } };
    row["time_modified"] = nullptr;
    row["time_modified_details"] = nlohmann::json::object();
    row["is_inbox"] = false;
    row["is_local"] = false;
    row["is_trashed"] = false;
    row["is_deleted"] = false;
    row["has_transparency"] = false;
    row["has_exif"] = false;
    row["has_xmp"] = false;
    row["has_iptc"] = false;
    row["has_human_readable_embedded_metadata"] = false;
    row["has_software_source"] = false;
    row["has_icc_profile"] = false;
    row["known_urls"] = nlohmann::json::array();
    row["ipfs_multihashes"] = nlohmann::json::object();
    row["ratings"] = nlohmann::json::object();

    nlohmann::json tags = nlohmann::json::object();

    auto add_tag_service = [&](const std::string& name, int type, const char* pretty, const std::string& key) {
        nlohmann::json service;
        service["name"] = name;
        service["type"] = type;
        service["type_pretty"] = pretty;

        nlohmann::json storage = nlohmann::json::object();
        if (!result.storage_tags.empty())
        {
            storage["0"] = result.storage_tags;
        }

        nlohmann::json display = nlohmann::json::object();
        if (!result.display_tags.empty())
        {
            display["0"] = result.display_tags;
        }

        service["storage_tags"] = std::move(storage);
        service["display_tags"] = std::move(display);
        tags[key] = std::move(service);
    };

    add_tag_service(service_name_, 0, "hydrus tag repository", service_key_);
    add_tag_service("all known tags", 10, "combined tag domain", kAllKnownTagsKey);
    row["tags"] = std::move(tags);

    row["file_viewing_statistics"] = nlohmann::json::array({
        { { "canvas_type", 0 }, { "canvas_type_pretty", "media viewer" }, { "views", 0 }, { "viewtime", 0.0 }, { "last_viewed_timestamp", nullptr } },
        { { "canvas_type", 1 }, { "canvas_type_pretty", "preview viewer" }, { "views", 0 }, { "viewtime", 0.0 }, { "last_viewed_timestamp", nullptr } },
        { { "canvas_type", 4 }, { "canvas_type_pretty", "client api viewer" }, { "views", 0 }, { "viewtime", 0.0 }, { "last_viewed_timestamp", nullptr } },
    });

    if (include_notes)
    {
        row["notes"] = nlohmann::json::object();
    }

    if (detailed_urls)
    {
        row["detailed_known_urls"] = nlohmann::json::array();
    }

    return row;
}

ApiResponse ApiHandlers::handle_tag_counts(const ApiRequest& request, const nlohmann::json& params)
{
    (void)request;

    if (!config_.get_bool("tag_counts_enabled", true))
    {
        return text_response(404,
            "tag counts are disabled on this mirror; set tag_counts_enabled = true to enable them.");
    }

    const auto store = store_.load();
    if (!store)
    {
        return text_response(503,
            "hydd is still building its initial Public Tag Repository snapshot; try again later.");
    }

    if (!store->has_tag_counts())
    {
        return text_response(503,
            "hydd is still building its tag counts; try again later.");
    }

    bool malformed = false;
    const auto tags = tags_from_params(params, malformed);
    if (malformed)
    {
        return text_response(400, "tags must be a string or a list of strings.");
    }
    if (tags.empty())
    {
        return text_response(400, "You have to specify a tag or tags to look up.");
    }
    if (tags.size() > 1000)
    {
        return text_response(400, "too many tags in one request (max 1000).");
    }

    nlohmann::json counts = nlohmann::json::object();
    nlohmann::json unknown = nlohmann::json::array();

    for (const auto& tag : tags)
    {
        if (tag.size() > 4096)
        {
            return text_response(400, "tag is too long.");
        }

        const auto count = store->tag_count(tag);
        if (count)
        {
            counts[tag] = *count;
        }
        else
        {
            unknown.push_back(tag);
        }
    }

    std::int64_t last_sync_at = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_sync_at = sync_state_.last_sync_at;
    }

    nlohmann::json body;
    body["counts"] = std::move(counts);
    body["unknown"] = std::move(unknown);
    body["as_of"] = store->tag_counts_built_at();
    body["last_sync_at"] = last_sync_at > 0 ? nlohmann::json(last_sync_at) : nlohmann::json(nullptr);

    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_tag_hashes(const ApiRequest& request, const nlohmann::json& params)
{
    (void)request;

    if (!config_.get_bool("tag_hashes_enabled", false))
    {
        return text_response(404,
            "reverse tag search is disabled on this mirror; set tag_hashes_enabled = true to enable it.");
    }

    const auto store = store_.load();
    if (!store)
    {
        return text_response(503,
            "hydd is still building its initial Public Tag Repository snapshot; try again later.");
    }

    if (!store->has_tag_hashes())
    {
        return text_response(503,
            "hydd is still building its reverse tag index; try again later.");
    }

    bool malformed = false;
    auto entries = tags_from_params(params, malformed);
    if (malformed)
    {
        return text_response(400, "tags must be a string or a list of strings.");
    }
    if (entries.empty())
    {
        return text_response(400, "You have to specify at least one tag to look up.");
    }
    if (entries.size() > 16)
    {
        return text_response(400, "too many tags in one reverse lookup (max 16).");
    }

    TagHashesQuery query;
    for (const auto& entry : entries)
    {
        if (entry.size() > 4096)
        {
            return text_response(400, "tag is too long.");
        }
        if (entry.front() == '-' && entry.size() > 1)
        {
            query.exclude.push_back(entry.substr(1));
        }
        else
        {
            query.include.push_back(entry);
        }
    }
    if (query.include.empty())
    {
        return text_response(400,
            "at least one positive tag is required; exclusion-only queries are not supported.");
    }

    std::int64_t offset = 0;
    std::int64_t limit = 100;
    std::int64_t after_value = 0;
    bool has_after = false;
    try
    {
        if (json_param_i64(params, "offset", offset) && offset < 0)
        {
            return text_response(400, "offset must not be negative.");
        }
        if (json_param_i64(params, "limit", limit))
        {
            if (limit < 1 || limit > 1000)
            {
                return text_response(400, "limit must be between 1 and 1000.");
            }
        }
        has_after = json_param_i64(params, "after", after_value);
    }
    catch (const std::exception& exception)
    {
        return text_response(400, exception.what());
    }

    if (has_after && params.contains("offset"))
    {
        return text_response(400, "use either offset or after, not both.");
    }
    if (has_after && (after_value < 0 || after_value > 0xFFFFFFFFll))
    {
        return text_response(400, "after must be a hash id between 0 and 4294967295.");
    }
    if (!has_after && offset > 10000000)
    {
        return text_response(400,
            "offset is too large for a single page; use the next_after cursor instead.");
    }

    std::optional<std::uint32_t> after;
    if (has_after)
    {
        after = static_cast<std::uint32_t>(after_value);
    }

    const auto page = store->tag_hashes(query, static_cast<std::uint64_t>(offset),
        static_cast<std::uint32_t>(limit), after);
    if (!page)
    {
        return text_response(404, "one of the tags is not defined in this mirror.");
    }

    nlohmann::json hashes = nlohmann::json::array();
    for (const auto& hash : page->hashes)
    {
        hashes.push_back(hash);
    }

    nlohmann::json body;
    body["tags"] = query.include;
    body["excluded"] = query.exclude;
    body["offset"] = page->offset;
    body["limit"] = limit;
    body["total"] = page->total_known ? nlohmann::json(page->total) : nlohmann::json(nullptr);
    body["hashes"] = std::move(hashes);
    body["next_offset"] = page->has_more ? nlohmann::json(page->next_offset) : nlohmann::json(nullptr);
    body["next_after"] = page->next_after
        ? nlohmann::json(*page->next_after)
        : nlohmann::json(nullptr);

    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_file_hashes(const ApiRequest& request, const nlohmann::json& params)
{
    const auto store = store_.load();
    if (!store)
    {
        return text_response(503,
            "hydd is still building its initial Public Tag Repository snapshot; try again later.");
    }

    const auto source_type = json_param_string(params, "source_hash_type").value_or("sha256");
    const auto desired_type = json_param_string(params, "desired_hash_type").value_or("sha256");
    if (source_type != "sha256" || desired_type != "sha256")
    {
        return text_response(400,
            "hydd only stores SHA-256 hashes; source_hash_type and desired_hash_type must both be sha256.");
    }

    bool malformed = false;
    const auto encoded_hashes = hashes_from_params(params, malformed);
    if (malformed)
    {
        return text_response(400, "hashes must be a string or a list of strings.");
    }
    if (encoded_hashes.empty())
    {
        return text_response(400, "You have to specify a hash or hashes to look up.");
    }
    if (encoded_hashes.size() > 1000)
    {
        return text_response(400, "too many hashes in one request (max 1000).");
    }

    nlohmann::json mapping = nlohmann::json::object();
    for (const auto& encoded : encoded_hashes)
    {
        const auto hash = hash_from_hex(encoded);
        if (!hash)
        {
            return text_response(400, "Invalid SHA-256 hash: " + encoded);
        }
        const auto result = store->lookup(*hash);
        if (result.found && !result.deleted)
        {
            mapping[encoded] = encoded;
        }
    }

    auto body = base_response();
    body["hashes"] = std::move(mapping);
    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_services(const ApiRequest& request) const
{
    (void)request;
    auto body = base_response();

    for (const char* name : { "local_tags", "local_files", "local_updates", "file_repositories",
             "all_local_files", "all_local_media", "all_known_files", "trash" })
    {
        body[name] = nlohmann::json::array();
    }

    body["tag_repositories"] = nlohmann::json::array({
        { { "name", service_name_ }, { "type", 0 }, { "type_pretty", "hydrus tag repository" }, { "service_key", service_key_ } },
    });
    body["all_known_tags"] = nlohmann::json::array({
        { { "name", "all known tags" }, { "type", 10 }, { "type_pretty", "combined tag domain" }, { "service_key", kAllKnownTagsKey } },
    });

    body["services"] = services_dict();
    body["services_v2"] = services_list();
    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_service(const ApiRequest& request, const nlohmann::json& params) const
{
    (void)request;

    const auto name = json_param_string(params, "service_name");
    const auto key = json_param_string(params, "service_key");

    nlohmann::json service;
    bool found = false;

    if (key && *key == service_key_)
    {
        service = { { "name", service_name_ }, { "type", 0 }, { "type_pretty", "hydrus tag repository" }, { "service_key", service_key_ } };
        found = true;
    }
    else if (key && *key == kAllKnownTagsKey)
    {
        service = { { "name", "all known tags" }, { "type", 10 }, { "type_pretty", "combined tag domain" }, { "service_key", kAllKnownTagsKey } };
        found = true;
    }
    else if (name && *name == service_name_)
    {
        service = { { "name", service_name_ }, { "type", 0 }, { "type_pretty", "hydrus tag repository" }, { "service_key", service_key_ } };
        found = true;
    }
    else if (name && *name == "all known tags")
    {
        service = { { "name", "all known tags" }, { "type", 10 }, { "type_pretty", "combined tag domain" }, { "service_key", kAllKnownTagsKey } };
        found = true;
    }

    if (!found)
    {
        return text_response(404, "No such service.");
    }

    auto body = base_response();
    body["service"] = std::move(service);
    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_session_key(const ApiRequest& request)
{
    (void)request;
    const auto key = hex_encode(random_bytes(32));
    const auto now = now_unix();

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);

        // Opportunistically drop expired sessions.
        for (auto iterator = sessions_.begin(); iterator != sessions_.end();)
        {
            if (iterator->second < now)
            {
                iterator = sessions_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }

        const std::size_t cap =
            kMaxSessionsPerKey * std::max<std::size_t>(1, access_keys_.size());
        if (sessions_.size() >= cap)
        {
            return text_response(429, "Too many active sessions; try again later.");
        }

        sessions_[key] = now + kSessionLifetimeSeconds;
    }

    auto body = base_response();
    body["session_key"] = key;
    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_verify_access_key() const
{
    auto body = base_response();
    body["name"] = "hydd";
    body["permits_everything"] = false;
    body["basic_permissions"] = nlohmann::json::array({ 3 });
    body["human_description"] = "hydd read-only PTR mirror: search for and fetch files";
    return ApiResponse { 200, "application/json", dump_json(body) };
}

ApiResponse ApiHandlers::handle_client_info() const
{
    auto body = base_response();
    body["boot_id"] = boot_id_hex_;
    body["boot_time"] = boot_time_;
    body["currently_idle"] = true;
    return ApiResponse { 200, "application/json", dump_json(body) };
}

bool ApiHandlers::update_cache_healthy(const RepositoryMetadata& metadata, std::string& error) const
{
    const auto directory = config_.cache_dir() / "updates";
    if (!path_exists(directory))
    {
        error = "update cache directory is missing: " + directory.string();
        return false;
    }

    std::uint64_t total = 0;
    std::uint64_t missing = 0;
    std::string first_missing;

    for (const auto& [index, rows] : metadata.updates)
    {
        (void)index;
        for (const auto& update : rows)
        {
            ++total;
            const auto path = directory / (hex_encode(update.hash) + ".bin");
            if (!path_exists(path) || file_size_or_zero(path) == 0)
            {
                ++missing;
                if (first_missing.empty())
                {
                    first_missing = path.string();
                }
            }
        }
    }

    if (total == 0)
    {
        error = "repository metadata lists no update files";
        return false;
    }

    if (missing > 0)
    {
        error = "update cache is incomplete: " + std::to_string(missing) + " of "
            + std::to_string(total) + " files missing (first: " + first_missing + ")";
        return false;
    }

    return true;
}

nlohmann::json ApiHandlers::health_json() const
{
    auto body = base_response();
    std::lock_guard<std::mutex> lock(state_mutex_);
        const bool sync_enabled = config_.get_bool("sync_enabled", true);
        const std::int64_t interval = config_.sync_interval_seconds();
        const std::int64_t now = now_unix();
        const std::int64_t next_check = next_check_at_.load();

        body["hyd_version"] = version_;
        body["service_name"] = service_name_;
        body["built"] = sync_state_.built;
        body["partial"] = partial_.load();
        body["next_index"] = sync_state_.next_index;
        body["next_update_due"] = sync_state_.next_update_due;
        body["last_sync_at"] = sync_state_.last_sync_at;
        body["delta_generation"] = sync_state_.delta_generation;
        body["sync_enabled"] = sync_enabled;
        body["sync_interval_seconds"] = interval;
        body["next_check_at"] = next_check;
        body["data_age_seconds"] = sync_state_.last_sync_at > 0 ? now - sync_state_.last_sync_at : 0;
        body["update_overdue_seconds"] = sync_state_.next_update_due > 0 && now > sync_state_.next_update_due
            ? now - sync_state_.next_update_due
            : 0;

        std::int64_t estimated_applied = 0;
        if (sync_enabled && sync_state_.next_update_due > 0 && next_check > 0)
        {
            if (sync_state_.next_update_due <= next_check)
            {
                estimated_applied = next_check;
            }
            else
            {
                const std::int64_t polling_interval = std::max<std::int64_t>(60, interval);
                const std::int64_t remaining = sync_state_.next_update_due - next_check;
                const std::int64_t checks = (remaining + polling_interval - 1) / polling_interval;
                estimated_applied = next_check + checks * polling_interval;
            }

            if (estimated_applied < now)
            {
                estimated_applied = now;
            }
        }
        body["estimated_update_applied_at"] = estimated_applied;

        if (build_progress_)
        {
            const auto* progress = build_progress_.get();
            const int phase = progress->phase.load();
            const auto started_at = progress->started_at.load();
            const auto updated_at = progress->updated_at.load();
            const auto files_done = progress->files_done.load();
            const auto files_total = progress->files_total.load();
            const auto bytes_done = progress->bytes_done.load();
            const auto bytes_total = progress->bytes_total.load();
            const auto phase_started_at = phase == BuildProgress::phase_partitions
                ? progress->partition_started_at.load()
                : started_at;
            const auto phase_elapsed = phase_started_at > 0 && now > phase_started_at
                ? now - phase_started_at
                : 0;
            const auto eta = files_done > 0 && files_total > files_done && phase_elapsed > 0
                ? phase_elapsed * static_cast<std::int64_t>(files_total - files_done)
                    / static_cast<std::int64_t>(files_done)
                : 0;

            nlohmann::json build;
            build["active"] = phase != BuildProgress::phase_idle;
            if (phase == BuildProgress::phase_definitions)
            {
                build["phase"] = "definitions";
            }
            else if (phase == BuildProgress::phase_partitions)
            {
                build["phase"] = "partitions";
            }
            else
            {
                build["phase"] = "idle";
            }
            build["partition"] = progress->partition.load();
            build["partitions"] = progress->partitions.load();
            build["files_done"] = files_done;
            build["files_total"] = files_total;
            build["bytes_done"] = bytes_done;
            build["bytes_total"] = bytes_total;
            build["hashes"] = progress->hashes.load();
            build["mappings"] = progress->mappings.load();
            build["percent"] = files_total > 0
                ? 100.0 * static_cast<double>(files_done) / static_cast<double>(files_total)
                : 0.0;
            build["bytes_percent"] = bytes_total > 0
                ? 100.0 * static_cast<double>(bytes_done) / static_cast<double>(bytes_total)
                : 0.0;
            build["started_at"] = started_at;
            build["updated_at"] = updated_at;
            build["elapsed_seconds"] = started_at > 0 && now > started_at ? now - started_at : 0;
            build["phase_elapsed_seconds"] = phase_elapsed;
            build["eta_seconds"] = eta;
            body["build"] = std::move(build);
        }

        const auto store = store_.load();
        body["store_open"] = static_cast<bool>(store);
        body["hash_count"] = sync_state_.hash_count;
        body["mapping_count"] = sync_state_.mapping_count;
        body["tag_count"] = sync_state_.tag_count;
        if (store)
        {
            const std::uint64_t legacy_hashes = store->legacy_hash_count();
            const std::uint64_t total_hashes = store->total_hash_count();
            body["hashes"] = total_hashes;
            body["mappings"] = store->total_mapping_count();
            body["tags"] = store->total_tag_count();
            body["siblings"] = store->total_sibling_count();
            body["parents"] = store->total_parent_count();
            body["legacy_hashes"] = legacy_hashes;
            body["sha256_hashes"] = total_hashes >= legacy_hashes
                ? total_hashes - legacy_hashes
                : total_hashes;
            if (store->partition_count() > 0)
            {
                body["partitions"] = store->partition_count();
            }
            body["delta_records"] = store->delta_record_count();
            body["delta_hashes"] = store->delta_hash_count();
            body["backend"] = store->backend_name();
        }

        nlohmann::json features = nlohmann::json::object();
        features["seeding"] = {
            { "enabled", config_.get_bool("keep_update_cache", false) },
        };
        features["tag_counts"] = {
            { "enabled", config_.get_bool("tag_counts_enabled", true) },
            { "ready", store && store->has_tag_counts() },
            { "defined_tags", store ? store->tag_counts_defined_tags() : 0 },
            { "bytes", store ? store->tag_counts_size_bytes() : 0 },
            { "as_of", store ? store->tag_counts_built_at() : 0 },
            { "delta_applied", store && store->tag_counts_delta_applied() },
        };
        features["tag_hashes"] = {
            { "enabled", config_.get_bool("tag_hashes_enabled", false) },
            { "ready", store && store->has_tag_hashes() },
            { "bytes", store ? store->tag_hashes_size_bytes() : 0 },
            { "as_of", store ? store->tag_hashes_built_at() : 0 },
        };
        body["features"] = std::move(features);

        return body;
}

ApiResponse ApiHandlers::handle_repository_metadata(const ApiRequest& request)
{
    if (!config_.get_bool("keep_update_cache", false))
    {
        return text_response(404,
            "repository relay is disabled; set keep_update_cache = true to enable it");
    }

    ApiResponse failure;
    if (!authorize(request, nlohmann::json::object(), failure))
    {
        return failure;
    }

    RepositoryMetadata metadata;
    std::string error;
    if (!load_repository_metadata_file(config_.state_dir() / "repository.json", metadata, error))
    {
        return text_response(500, error);
    }

    if (!update_cache_healthy(metadata, error))
    {
        request_cache_repair_full();
        return text_response(503, error);
    }

    std::int64_t since = 0;
    if (const auto iterator = request.query.find("since"); iterator != request.query.end())
    {
        const auto parsed = parse_i64(iterator->second);
        if (!parsed || *parsed < 0)
        {
            return text_response(400, "since must be a non-negative update index");
        }
        since = *parsed;
    }

    nlohmann::json rows = nlohmann::json::array();
    for (const auto& [index, updates] : metadata.updates)
    {
        if (index < since)
        {
            continue;
        }

        std::int64_t begin = 0;
        std::int64_t end = 0;
        if (!updates.empty())
        {
            begin = updates.front().begin;
            end = updates.front().end;
        }

        nlohmann::json hashes = nlohmann::json::array();
        for (const auto& update : updates)
        {
            hashes.push_back(hex_encode(update.hash));
        }

        rows.push_back(nlohmann::json::array({ index, hashes, begin, end }));
    }

    const nlohmann::json metadata_slice = nlohmann::json::array({
        37, 1, nlohmann::json::array({ rows, metadata.next_update_due })
    });
    const nlohmann::json value_meta = nlohmann::json::array({ 2, metadata_slice });
    const nlohmann::json key_meta = nlohmann::json::array({ 0, "metadata_slice" });
    const nlohmann::json argument = nlohmann::json::array({ key_meta, value_meta });
    const nlohmann::json payload = nlohmann::json::array({
        21, 2, nlohmann::json::array({ argument })
    });

    const std::string text = payload.dump();
    Bytes compressed;
    if (!zlib_deflate(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
            compressed, error))
    {
        return text_response(500, error);
    }

    return ApiResponse {
        200, "application/octet-stream",
        std::string(compressed.begin(), compressed.end())
    };
}

ApiResponse ApiHandlers::handle_repository_update(const ApiRequest& request)
{
    if (!config_.get_bool("keep_update_cache", false))
    {
        return text_response(404,
            "repository relay is disabled; set keep_update_cache = true to enable it");
    }

    ApiResponse failure;
    if (!authorize(request, nlohmann::json::object(), failure))
    {
        return failure;
    }

    const auto iterator = request.query.find("update_hash");
    if (iterator == request.query.end())
    {
        return text_response(400, "update_hash is required");
    }

    const auto hash = hash_from_hex(iterator->second);
    if (!hash)
    {
        return text_response(400, "update_hash must be a 64-character sha256");
    }

    const auto path = config_.cache_dir() / "updates" / (hex_encode(*hash) + ".bin");
    if (!path_exists(path))
    {
        return text_response(404, "update file is not cached");
    }

    std::string error;
    auto bytes = read_file(path, error);
    if (!bytes)
    {
        return text_response(500, error);
    }

    if (sha256(*bytes) != *hash)
    {
        HYD_LOGF(warn, "cached update file %s does not match its name; removing it",
            path.string().c_str());
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        request_cache_repair(*hash);
        return text_response(500, "cached update file is corrupt and was removed");
    }

    return ApiResponse {
        200, "application/octet-stream",
        std::string(bytes->begin(), bytes->end())
    };
}

} // namespace hyd
