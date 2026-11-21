#pragma once

#include "api/server.hpp"
#include "common/config.hpp"
#include "common/util.hpp"
#include "store/builder.hpp"
#include "store/store.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hyd {

class ApiHandlers
{
public:
    ApiHandlers(Config config, std::string access_key, std::string service_key, std::string service_name);

    ApiResponse handle(const ApiRequest& request);

    void set_store(std::shared_ptr<const TagSource> store);
    void set_sync_state(const SyncState& state);
    void set_partial(bool partial) { partial_.store(partial); }

    const std::string& access_key() const { return access_key_; }
    const std::string& service_key() const { return service_key_; }

private:
    ApiResponse route(const ApiRequest& request);
    ApiResponse handle_file_metadata(const ApiRequest& request, const nlohmann::json& params);
    ApiResponse handle_file_hashes(const ApiRequest& request, const nlohmann::json& params);
    ApiResponse handle_services(const ApiRequest& request) const;
    ApiResponse handle_service(const ApiRequest& request, const nlohmann::json& params) const;
    ApiResponse handle_session_key(const ApiRequest& request);
    ApiResponse handle_verify_access_key() const;
    ApiResponse handle_client_info() const;

    bool authorize(const ApiRequest& request, const nlohmann::json& params, ApiResponse& failure);
    std::string access_key_from(const ApiRequest& request, const nlohmann::json& params) const;

    nlohmann::json services_dict() const;
    nlohmann::json services_list() const;
    nlohmann::json metadata_row(const Hash& hash, const TagQueryResult& result,
        bool identifiers_only, bool basic_only, bool include_notes, bool detailed_urls) const;

    nlohmann::json base_response() const;

    Config config_;
    std::string access_key_;
    std::string service_key_;
    std::string service_name_;
    std::string boot_id_hex_;
    double boot_time_ = 0.0;

    std::atomic<std::shared_ptr<const TagSource>> store_ {};
    mutable std::mutex state_mutex_;
    SyncState sync_state_;
    std::atomic<bool> partial_ { false };

    mutable std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::int64_t> sessions_; // key -> expiry unix
};

} // namespace hyd
