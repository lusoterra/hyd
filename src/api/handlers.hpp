#pragma once

#include "api/server.hpp"
#include "common/config.hpp"
#include "common/util.hpp"
#include "store/builder.hpp"
#include "store/store.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyd {

class ApiHandlers
{
public:
    ApiHandlers(Config config, std::vector<std::string> access_keys, std::string service_key,
        std::string service_name, std::string version);

    ApiResponse handle(const ApiRequest& request);

    void set_store(std::shared_ptr<const TagSource> store);
    void set_sync_state(const SyncState& state);
    void set_partial(bool partial) { partial_.store(partial); }
    void set_next_check_at(std::int64_t timestamp) { next_check_at_.store(timestamp); }
    void set_build_progress(std::shared_ptr<BuildProgress> progress) { build_progress_ = std::move(progress); }

    // Update-cache repair queue. /update enqueues a hash whose cached file no
    // longer matches its name; /metadata asks for a full verification when the
    // cache is incomplete. The sync thread consumes these requests.
    void request_cache_repair(const Hash& hash);
    void request_cache_repair_full();
    std::vector<Hash> take_cache_repair_hashes();
    bool cache_repair_full_requested() const;
    void clear_cache_repair_full();

    const std::vector<std::string>& access_keys() const { return access_keys_; }
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
    ApiResponse handle_tag_counts(const ApiRequest& request, const nlohmann::json& params);
    ApiResponse handle_tag_hashes(const ApiRequest& request, const nlohmann::json& params);
    ApiResponse handle_repository_metadata(const ApiRequest& request);
    ApiResponse handle_repository_update(const ApiRequest& request);

    bool update_cache_healthy(const RepositoryMetadata& metadata, std::string& error) const;

    bool authorize(const ApiRequest& request, const nlohmann::json& params, ApiResponse& failure);
    std::string access_key_from(const ApiRequest& request, const nlohmann::json& params) const;

    // Per-client token bucket. Returns false and fills `failure` with a 429
    // when the caller has spent more than its budget. The identity is the
    // access/session key when one is supplied, otherwise the peer address.
    bool rate_limit(const ApiRequest& request, ApiResponse& failure);
    bool consume_rate_token(const std::string& key,
        const std::chrono::steady_clock::time_point& now, const ApiRequest& request,
        ApiResponse& failure);
    std::string rate_limit_key(const ApiRequest& request) const;

    nlohmann::json services_dict() const;
    nlohmann::json services_list() const;
    nlohmann::json metadata_row(std::span<const std::uint8_t> hash, const TagQueryResult& result,
        bool identifiers_only, bool basic_only, bool include_notes, bool detailed_urls) const;

    nlohmann::json base_response() const;
    // Shared /health and /stats payload. /health stays unauthenticated for
    // Hydrus parity; /stats requires authorization (loopback still passes by
    // default, so local `hyd --stats` keeps working).
    nlohmann::json health_json() const;

    Config config_;
    std::vector<std::string> access_keys_;
    std::string service_key_;
    std::string service_name_;
    std::string version_;
    std::string boot_id_hex_;
    double boot_time_ = 0.0;

    std::atomic<std::shared_ptr<const TagSource>> store_ {};
    std::shared_ptr<BuildProgress> build_progress_;
    mutable std::mutex state_mutex_;
    SyncState sync_state_;
    std::atomic<bool> partial_ { false };
    std::atomic<std::int64_t> next_check_at_ { 0 };

    mutable std::mutex repair_mutex_;
    std::vector<Hash> repair_hashes_;
    bool repair_full_ = false;

    mutable std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::int64_t> sessions_; // key -> expiry unix
    // DoS bound: at most kMaxSessionsPerKey live sessions per configured
    // access key (loopback-bypass mints share one bucket). New mints beyond
    // the cap fail with 429 instead of growing the map forever.
    static constexpr std::size_t kMaxSessionsPerKey = 32;

    struct RateBucket
    {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point last {};
    };

    mutable std::mutex rate_mutex_;
    std::unordered_map<std::string, RateBucket> rate_buckets_;
    bool rate_limit_enabled_ = false;
    bool rate_limit_local_bypass_ = false;
    double rate_per_second_ = 10.0;
    double rate_burst_ = 120.0;
    static constexpr std::size_t kMaxRateBuckets = 4096;
    static constexpr std::chrono::seconds kRateBucketIdle { 600 };
};

} // namespace hyd
