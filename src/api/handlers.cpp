#include "api/handlers.hpp"

#include "common/log.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace hyd {

namespace {

constexpr int kClientApiVersion = 97;
constexpr int kHydrusVersion = 696;
constexpr const char* kAllKnownTagsKey = "616c6c206b6e6f776e2074616773";
constexpr std::int64_t kSessionLifetimeSeconds = 24 * 60 * 60;

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

ApiHandlers::ApiHandlers(Config config, std::string access_key, std::string service_key, std::string service_name)
    : config_(std::move(config)),
      access_key_(std::move(access_key)),
      service_key_(std::move(service_key)),
      service_name_(std::move(service_name))
{
    boot_id_hex_ = hex_encode(random_bytes(32));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    boot_time_ = std::chrono::duration<double>(now).count();
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

    if (constant_time_equal(provided, access_key_))
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

    if (const auto accept = request.headers.find("accept"); accept != request.headers.end()
        && to_lower(accept->second).find("application/cbor") != std::string::npos)
    {
        return text_response(406, "hydd speaks JSON only; CBOR is not implemented.");
    }

    if (path == "/" || path.empty())
    {
        return text_response(200,
            "<html><body><h1>hydd</h1><p>read-only Hydrus Public Tag Repository mirror</p></body></html>",
            "text/html; charset=utf-8");
    }

    if (path == "/api_version")
    {
        auto body = base_response();
        body["hyd_version"] = "0.1.0";
        return ApiResponse { 200, "application/json", dump_json(body) };
    }

    if (path == "/health")
    {
        auto body = base_response();
        std::lock_guard<std::mutex> lock(state_mutex_);
        body["built"] = sync_state_.built;
        body["partial"] = partial_.load();
        body["next_index"] = sync_state_.next_index;
        body["last_sync_at"] = sync_state_.last_sync_at;
        body["delta_generation"] = sync_state_.delta_generation;
        const auto store = store_.load();
        body["store_open"] = static_cast<bool>(store);
        body["hash_count"] = sync_state_.hash_count;
        body["mapping_count"] = sync_state_.mapping_count;
        body["tag_count"] = sync_state_.tag_count;
        if (store)
        {
            body["hashes"] = store->total_hash_count();
            body["mappings"] = store->total_mapping_count();
            body["tags"] = store->total_tag_count();
            body["delta_records"] = store->delta_record_count();
            body["delta_hashes"] = store->delta_hash_count();
            body["backend"] = store->backend_name();
        }
        return ApiResponse { 200, "application/json", dump_json(body) };
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

    if (path == "/get_files/file_hashes")
    {
        ApiResponse failure;
        if (!authorize(request, params, failure))
        {
            return failure;
        }
        return handle_file_hashes(request, params);
    }

    if (path == "/get_files/file" || path == "/get_files/thumbnail"
        || path == "/get_files/render" || path == "/get_files/file_seed")
    {
        return text_response(404, "hydd stores tags only; it does not store files or thumbnails.");
    }

    if (path.rfind("/add_", 0) == 0 || path.rfind("/edit_", 0) == 0
        || path.rfind("/manage_", 0) == 0 || path == "/get_files/search_files"
        || path == "/request_new_permissions" || path == "/commit_pending")
    {
        return text_response(501,
            "hydd is a read-only Public Tag Repository mirror; this endpoint is not implemented.");
    }

    return text_response(404, "Unknown endpoint.");
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

    const bool identifiers_only = json_param_bool(params, "only_return_identifiers", false);
    const bool basic_only = json_param_bool(params, "only_return_basic_information", false);
    const bool include_notes = json_param_bool(params, "include_notes", false);
    const bool detailed_urls = json_param_bool(params, "detailed_url_information", false);
    const bool include_services = json_param_bool(params, "include_services_object", true);

    nlohmann::json metadata = nlohmann::json::array();

    for (const auto& encoded : encoded_hashes)
    {
        const auto hash = hash_from_hex(encoded);
        if (!hash)
        {
            return text_response(400, "Invalid SHA-256 hash: " + encoded);
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

nlohmann::json ApiHandlers::metadata_row(const Hash& hash, const TagQueryResult& result,
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

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[key] = now_unix() + kSessionLifetimeSeconds;

        // Opportunistically drop expired sessions.
        for (auto iterator = sessions_.begin(); iterator != sessions_.end();)
        {
            if (iterator->second < now_unix())
            {
                iterator = sessions_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
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

} // namespace hyd
