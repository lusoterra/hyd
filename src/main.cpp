#include "api/handlers.hpp"
#include "api/server.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/util.hpp"
#include "hydrus/http.hpp"
#include "hydrus/update.hpp"
#include "stats.hpp"
#include "store/builder.hpp"
#include "store/store.hpp"
#include "store/tag_source.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <memory>
#include <thread>
#include <vector>
#include <sys/stat.h>

namespace {

#ifndef HYDD_VERSION
#define HYDD_VERSION "0.0.0"
#endif

constexpr const char* kVersion = HYDD_VERSION;

std::atomic<bool> g_stop { false };

void signal_handler(int signal)
{
    (void)signal;
    g_stop.store(true);
}

struct Options
{
    std::string config_path = hyd::Config::kDefaultPath;
    std::optional<std::string> data_dir;
    std::optional<std::string> listen_address;
    std::optional<int> listen_port;
    std::optional<bool> sync_enabled;
    std::optional<std::string> log_level;
    bool sync_once = false;
    bool no_sync = false;
    bool query = false;
    bool api_query = false;
    bool verify_store = false;
    bool scan_updates = false;
    bool repair_cache = false;
    bool stats = false;
    bool compact = false;
    bool rebuild_tag_counts = false;
    bool check_tag_counts = false;
    std::vector<std::string> tag_count_tags;
    std::optional<std::string> tag_hashes_tag;
    std::int64_t tag_hashes_offset = 0;
    std::int64_t tag_hashes_limit = 100;
    bool print_access_key = false;
    std::string query_hash;
};

void print_help()
{
    std::cout <<
        "hydd - read-only Hydrus Public Tag Repository daemon\n"
        "\n"
        "Usage: hydd [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -c, --config PATH          Configuration file (default: /etc/hydd.conf)\n"
        "  -d, --data-dir PATH        Override data_dir\n"
        "  -l, --listen ADDRESS       Override listen_address\n"
        "  -p, --port PORT            Override listen_port\n"
        "      --no-sync              Serve the existing store, do not sync\n"
        "      --sync-once            Run one sync pass and exit\n"
        "      --query HASH           Print the tags for a file hash and exit\n"
        "      --api-query HASH       Query the running API for a file hash and exit\n"
        "      --verify-store         Open and validate the store and exit\n"
        "      --scan-updates         Parse every cached update file and report format errors\n"
        "      --repair-cache         Re-download missing or corrupt cached update files and exit\n"
        "      --stats                Query the running local API and print repository statistics\n"
        "      --compact              Merge pending deltas into the base store and exit\n"
        "      --rebuild-tag-counts   Rebuild the derived tag counts and tag name index and exit\n"
        "      --check-tag-counts     Verify the derived tag counts and tag name index and exit\n"
        "      --tag-counts TAG       Query the running API for mapping counts (repeatable)\n"
        "      --tag-hashes TAG       Query the running API for the hashes of one tag\n"
        "      --hash-offset N        Page offset for --tag-hashes (default 0)\n"
        "      --hash-limit N         Page size for --tag-hashes (1-1000, default 100)\n"
        "      --print-access-key     Print every configured API access key and exit\n"
        "      --log-level LEVEL      trace, debug, info, warn, error, off\n"
        "  -h, --help                 Show this help\n"
        "  -V, --version              Show the version\n"
        "\n"
        "The daemon serves the Hydrus Client API read-only subset documented in\n"
        "the hydd(8) man page and README.\n";
}

std::optional<Options> parse_options(int argc, char** argv, std::string& error)
{
    Options options;

    static const option long_options[] = {
        { "config", required_argument, nullptr, 'c' },
        { "data-dir", required_argument, nullptr, 'd' },
        { "listen", required_argument, nullptr, 'l' },
        { "port", required_argument, nullptr, 'p' },
        { "no-sync", no_argument, nullptr, 1000 },
        { "sync-once", no_argument, nullptr, 1001 },
        { "query", required_argument, nullptr, 1002 },
        { "api-query", required_argument, nullptr, 1009 },
        { "verify-store", no_argument, nullptr, 1003 },
        { "print-access-key", no_argument, nullptr, 1004 },
        { "compact", no_argument, nullptr, 1006 },
        { "log-level", required_argument, nullptr, 1005 },
        { "scan-updates", no_argument, nullptr, 1007 },
        { "stats", no_argument, nullptr, 1008 },
        { "repair-cache", no_argument, nullptr, 1010 },
        { "rebuild-tag-counts", no_argument, nullptr, 1011 },
        { "check-tag-counts", no_argument, nullptr, 1012 },
        { "tag-counts", required_argument, nullptr, 1013 },
        { "tag-hashes", required_argument, nullptr, 1014 },
        { "hash-offset", required_argument, nullptr, 1015 },
        { "hash-limit", required_argument, nullptr, 1016 },
        { "help", no_argument, nullptr, 'h' },
        { "version", no_argument, nullptr, 'V' },
        { nullptr, 0, nullptr, 0 },
    };

    while (true)
    {
        const int choice = getopt_long(argc, argv, "c:d:l:p:hV", long_options, nullptr);
        if (choice == -1)
        {
            break;
        }

        switch (choice)
        {
            case 'c':
                options.config_path = optarg;
                break;
            case 'd':
                options.data_dir = std::string(optarg);
                break;
            case 'l':
                options.listen_address = std::string(optarg);
                break;
            case 'p':
            {
                const auto port = hyd::parse_i64(optarg);
                if (!port || *port < 1 || *port > 65535)
                {
                    error = "invalid port";
                    return std::nullopt;
                }
                options.listen_port = static_cast<int>(*port);
                break;
            }
            case 1000:
                options.no_sync = true;
                options.sync_enabled = false;
                break;
            case 1001:
                options.sync_once = true;
                break;
            case 1002:
                options.query = true;
                options.query_hash = optarg;
                break;
            case 1009:
                options.api_query = true;
                options.query_hash = optarg;
                break;
            case 1003:
                options.verify_store = true;
                break;
            case 1004:
                options.print_access_key = true;
                break;
            case 1006:
                options.compact = true;
                break;
            case 1005:
                options.log_level = std::string(optarg);
                break;
            case 1007:
                options.scan_updates = true;
                break;
            case 1008:
                options.stats = true;
                break;
            case 1010:
                options.repair_cache = true;
                break;
            case 1011:
                options.rebuild_tag_counts = true;
                break;
            case 1012:
                options.check_tag_counts = true;
                break;
            case 1013:
                options.tag_count_tags.emplace_back(optarg);
                break;
            case 1014:
                options.tag_hashes_tag = std::string(optarg);
                break;
            case 1015:
            {
                const auto value = hyd::parse_i64(optarg);
                if (!value || *value < 0)
                {
                    error = "invalid --hash-offset";
                    return std::nullopt;
                }
                options.tag_hashes_offset = *value;
                break;
            }
            case 1016:
            {
                const auto value = hyd::parse_i64(optarg);
                if (!value || *value < 1 || *value > 1000)
                {
                    error = "invalid --hash-limit (1-1000)";
                    return std::nullopt;
                }
                options.tag_hashes_limit = *value;
                break;
            }
            case 'h':
                print_help();
                exit(0);
            case 'V':
                std::cout << "hyd v" << kVersion << "\n";
                exit(0);
            default:
                error = "invalid option; use --help";
                return std::nullopt;
        }
    }

    return options;
}

bool read_or_create_secret(const std::filesystem::path& path, std::string& value, std::string& error)
{
    if (hyd::path_exists(path))
    {
        auto bytes = hyd::read_file(path, error);
        if (!bytes)
        {
            return false;
        }
        value = hyd::trim(std::string(bytes->begin(), bytes->end()));
    }

    if (value.empty())
    {
        value = hyd::hex_encode(hyd::random_bytes(32));
        if (!hyd::write_file_atomic(path, value + "\n", error))
        {
            return false;
        }
        if (::chmod(path.c_str(), 0600) != 0)
        {
            error = "cannot chmod " + path.string() + ": " + hyd::errno_string(errno);
            return false;
        }
    }

    if (!hyd::hash_from_hex(value))
    {
        error = path.string() + " does not contain a 64-character hexadecimal secret";
        return false;
    }

    return true;
}

struct ScanVisitor : hyd::UpdateVisitor
{
    std::uint64_t hashes = 0;
    std::uint64_t legacy_hashes = 0;
    std::uint64_t invalid_hashes = 0;
    std::uint64_t tags = 0;
    std::uint64_t mappings = 0;
    std::uint64_t files = 0;
    std::uint64_t siblings = 0;
    std::uint64_t parents = 0;

    void on_hash_definition(std::uint32_t, const hyd::Hash&) override { ++hashes; }
    void on_unsupported_hash_definition(std::uint32_t, std::string_view) override { ++legacy_hashes; }
    void on_invalid_hash_definition(std::uint32_t, std::string_view) override { ++invalid_hashes; }
    void on_tag_definition(std::uint32_t, std::string_view) override { ++tags; }
    void on_mapping_add(std::uint32_t, std::uint32_t) override { ++mappings; }
    void on_mapping_delete(std::uint32_t, std::uint32_t) override { ++mappings; }
    void on_file_add(std::uint32_t) override { ++files; }
    void on_file_delete(std::uint32_t) override { ++files; }
    void on_sibling_add(std::uint32_t, std::uint32_t) override { ++siblings; }
    void on_sibling_delete(std::uint32_t, std::uint32_t) override { ++siblings; }
    void on_parent_add(std::uint32_t, std::uint32_t) override { ++parents; }
    void on_parent_delete(std::uint32_t, std::uint32_t) override { ++parents; }
};

int scan_update_cache(const hyd::Config& config)
{
    const auto directory = config.cache_dir() / "updates";

    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    if (std::filesystem::exists(directory, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".bin")
            {
                paths.push_back(entry.path());
            }
        }
    }
    std::sort(paths.begin(), paths.end());

    if (paths.empty())
    {
        std::cerr << "hydd: no cached update files in " << directory << "\n";
        return 1;
    }

    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned workers = std::max(1u, std::min(8u, hardware == 0 ? 1u : hardware));

    std::atomic<std::size_t> next { 0 };
    std::atomic<std::uint64_t> scanned { 0 };
    std::atomic<std::uint64_t> definitions { 0 };
    std::atomic<std::uint64_t> content { 0 };
    std::atomic<std::uint64_t> hashes { 0 };
    std::atomic<std::uint64_t> legacy_hashes { 0 };
    std::atomic<std::uint64_t> invalid_hashes { 0 };
    std::atomic<std::uint64_t> tags { 0 };
    std::atomic<std::uint64_t> mappings { 0 };
    std::atomic<std::uint64_t> files { 0 };
    std::atomic<std::uint64_t> siblings { 0 };
    std::atomic<std::uint64_t> parents { 0 };

    std::mutex output_mutex;
    std::vector<std::string> failures;

    auto worker = [&] {
        hyd::UpdateParser parser;
            while (true)
        {
            const auto index = next.fetch_add(1);
            if (index >= paths.size())
            {
                break;
            }

            std::string error;
            auto bytes = hyd::read_file(paths[index], error);
            hyd::UpdateKind kind = hyd::UpdateKind::unknown;
            ScanVisitor visitor;

            bool ok = bytes.has_value();
            if (ok)
            {
                const auto expected = hyd::hash_from_hex(paths[index].stem().string());
                if (!expected)
                {
                    error = "cache filename is not a sha256";
                    ok = false;
                }
                else if (hyd::sha256(*bytes) != *expected)
                {
                    error = "sha256 does not match the cache filename";
                    ok = false;
                }
            }

            if (ok && parser.parse(*bytes, visitor, kind, error))
            {
                if (kind == hyd::UpdateKind::definitions)
                {
                    definitions.fetch_add(1);
                }
                else if (kind == hyd::UpdateKind::content)
                {
                    content.fetch_add(1);
                }
                hashes += visitor.hashes;
                legacy_hashes += visitor.legacy_hashes;
                invalid_hashes += visitor.invalid_hashes;
                tags += visitor.tags;
                mappings += visitor.mappings;
                files += visitor.files;
                siblings += visitor.siblings;
                parents += visitor.parents;
            }
            else
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                failures.push_back(paths[index].filename().string() + ": " + error);
            }

            const auto count = scanned.fetch_add(1) + 1;
            if (count % 2000 == 0)
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "scanned " << count << "/" << paths.size() << " update files\n";
                std::cout.flush();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned i = 0; i < workers; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    std::cout << "scanned " << scanned.load() << " update files: " << definitions.load()
              << " definitions, " << content.load() << " content, " << hashes.load()
              << " sha256 hashes, " << legacy_hashes.load() << " legacy hashes, "
              << invalid_hashes.load() << " known-invalid hashes, " << tags.load()
              << " tags, " << mappings.load() << " mappings, " << files.load() << " file records, "
              << siblings.load() << " sibling records, " << parents.load() << " parent records\n";

    if (invalid_hashes.load() > 0)
    {
        std::cout << "note: skipped " << invalid_hashes.load()
                  << " known-invalid PTR hash definitions (expected; they cannot be queried)\n";
    }

    if (!failures.empty())
    {
        std::sort(failures.begin(), failures.end());
        for (const auto& failure : failures)
        {
            std::cout << "FAIL " << failure << "\n";
        }
        std::cerr << "hydd: " << failures.size() << " update files failed to parse\n";
        return 1;
    }

    std::cout << "no update format problems found\n";
    return 0;
}

struct Runtime
{
    hyd::Config config;
    std::vector<std::string> access_keys;
    std::string service_key;
    std::shared_ptr<hyd::ApiHandlers> handlers;
    std::shared_ptr<const hyd::TagSource> store;
    std::shared_ptr<hyd::BuildProgress> build_progress = std::make_shared<hyd::BuildProgress>();
    hyd::SyncState sync_state;
    std::mutex store_mutex;
};

bool reload_store(Runtime& runtime, std::string& error)
{
    auto store = std::make_shared<hyd::Store>();
    if (!store->open(runtime.config.store_dir(), error))
    {
        return false;
    }

    runtime.handlers->set_store(store);
    {
        std::lock_guard<std::mutex> lock(runtime.store_mutex);
        runtime.store = store;
    }
    return true;
}

// Builds counts.bin/tag_lookup.bin when they are missing (upgrade from 0.7.x)
// or when force is set. Runs off the API thread so startup stays immediate;
// /hydd/tag_counts answers 503 until this finishes.
void build_tag_metadata_if_needed(Runtime& runtime, bool force)
{
    if (!runtime.config.get_bool("tag_counts_enabled", true))
    {
        return;
    }

    const bool hashes_enabled = runtime.config.get_bool("tag_hashes_enabled", false);

    std::shared_ptr<const hyd::TagSource> store;
    {
        std::lock_guard<std::mutex> lock(runtime.store_mutex);
        store = runtime.store;
    }
    if (!store)
    {
        return;
    }
    if (!force && store->has_tag_counts() && (!hashes_enabled || store->has_tag_hashes()))
    {
        return;
    }

    HYD_LOG(info, "building derived tag counts and tag name index (one time)");

    hyd::SyncCallbacks callbacks;
    callbacks.should_stop = [] { return g_stop.load(); };

    std::string error;
    if (!hyd::rebuild_tag_metadata(runtime.config, callbacks, error))
    {
        if (error != "stopped")
        {
            HYD_LOGF(warn, "cannot build tag metadata: %s", error.c_str());
        }
        return;
    }

    std::string reload_error;
    if (!reload_store(runtime, reload_error))
    {
        HYD_LOGF(warn, "cannot reload store after tag metadata build: %s", reload_error.c_str());
    }
    else
    {
        HYD_LOG(info, "tag counts are ready");
    }
}

void run_sync(Runtime& runtime, bool& stopped, std::string& error)
{
    hyd::SyncCallbacks callbacks;
    callbacks.should_stop = [] { return g_stop.load(); };
    callbacks.progress = runtime.build_progress;
    callbacks.on_store_ready = [&runtime] {
        std::string reload_error;
        if (!reload_store(runtime, reload_error))
        {
            HYD_LOGF(warn, "cannot reload store after sync: %s", reload_error.c_str());
            return;
        }
        HYD_LOGF(info, "store reloaded (%s): %llu hashes, %llu mappings, %llu tags, %llu delta records",
            runtime.store->backend_name().c_str(),
            static_cast<unsigned long long>(runtime.store->total_hash_count()),
            static_cast<unsigned long long>(runtime.store->total_mapping_count()),
            static_cast<unsigned long long>(runtime.store->total_tag_count()),
            static_cast<unsigned long long>(runtime.store->delta_record_count()));
    };

    if (!hyd::sync_repository(runtime.config, callbacks, stopped, error))
    {
        return;
    }

    if (!hyd::load_sync_state(runtime.config.state_dir() / "sync.json", runtime.sync_state, error))
    {
        return;
    }
    runtime.handlers->set_sync_state(runtime.sync_state);
    runtime.handlers->set_partial(runtime.sync_state.partial);
}

} // namespace

int main(int argc, char** argv)
{
    std::string error;
    auto options = parse_options(argc, argv, error);
    if (!options)
    {
        std::cerr << "hydd: " << error << "\n";
        return 2;
    }

    hyd::log_set_level(hyd::LogLevel::info);

    Runtime runtime;
    if (!runtime.config.load(options->config_path, options->config_path != hyd::Config::kDefaultPath, error))
    {
        std::cerr << "hydd: " << error << "\n";
        return 2;
    }

    runtime.config.apply_command_line_overrides(
        options->data_dir, options->listen_address, options->listen_port,
        options->sync_enabled, options->log_level);

    if (!runtime.config.finalize(error))
    {
        std::cerr << "hydd: " << error << "\n";
        return 2;
    }

    if (options->stats)
    {
        // Statistics only need the config and the local HTTP API. Dispatch
        // before any data-dir, key, or log-file work so it works unprivileged.
        return hyd::run_stats(runtime.config);
    }

    if (options->api_query)
    {
        return hyd::run_api_query(runtime.config, options->query_hash);
    }

    if (!options->tag_count_tags.empty())
    {
        return hyd::run_tag_counts(runtime.config, options->tag_count_tags);
    }

    if (options->tag_hashes_tag)
    {
        return hyd::run_tag_hashes(runtime.config, *options->tag_hashes_tag,
            options->tag_hashes_offset, options->tag_hashes_limit);
    }

    hyd::log_set_level(hyd::log_level_from_string(runtime.config.get_string("log_level", "info")));

    const auto log_file = runtime.config.get_string("log_file");
    if (!log_file.empty())
    {
        const auto rotate_mb = runtime.config.get_int("log_rotate_mb", 10);
        std::string log_error;
        if (!hyd::log_init_file(
                log_file,
                rotate_mb > 0 ? static_cast<std::uint64_t>(rotate_mb) * 1024 * 1024 : 0,
                static_cast<int>(runtime.config.get_int("log_rotate_keep", 5)),
                runtime.config.get_bool("log_rotate_compress", true),
                log_error))
        {
            HYD_LOGF(warn, "%s", log_error.c_str());
        }
    }

    if (!hyd::mkdir_p(runtime.config.data_dir(), error)
        || !hyd::mkdir_p(runtime.config.state_dir(), error))
    {
        HYD_LOGF(error, "%s", error.c_str());
        return 1;
    }

    std::vector<std::string> access_keys;
    const std::string configured_access_key = runtime.config.get_string("access_key");
    if (!configured_access_key.empty())
    {
        if (!hyd::parse_key_list(configured_access_key, access_keys, error))
        {
            HYD_LOGF(error, "%s", error.c_str());
            return 1;
        }
    }
    else
    {
        const auto access_key_file = runtime.config.get_string(
            "access_key_file", (runtime.config.data_dir() / "api_access_key").string());

        if (!hyd::load_key_file(access_key_file, access_keys, error))
        {
            HYD_LOGF(error, "%s", error.c_str());
            return 1;
        }

        if (access_keys.empty())
        {
            const std::string generated = hyd::hex_encode(hyd::random_bytes(32));
            if (!hyd::write_file_atomic(access_key_file, generated + "\n", error))
            {
                HYD_LOGF(error, "%s", error.c_str());
                return 1;
            }
            if (::chmod(access_key_file.c_str(), 0600) != 0)
            {
                HYD_LOGF(error, "cannot chmod %s: %s", access_key_file.c_str(),
                    hyd::errno_string(errno).c_str());
                return 1;
            }
            access_keys.push_back(generated);
        }
    }

    if (access_keys.empty())
    {
        HYD_LOGF(error, "no API access keys configured");
        return 1;
    }

    runtime.access_keys = std::move(access_keys);
    HYD_LOGF(info, "API access keys: %zu", runtime.access_keys.size());

    std::string service_key = runtime.config.get_string("service_key");
    if (service_key.empty())
    {
        const auto service_key_file = runtime.config.get_string(
            "service_key_file", (runtime.config.data_dir() / "service_key").string());
        if (!read_or_create_secret(service_key_file, service_key, error))
        {
            HYD_LOGF(error, "%s", error.c_str());
            return 1;
        }
    }
    runtime.service_key = service_key;

    if (options->print_access_key)
    {
        for (const auto& key : runtime.access_keys)
        {
            std::cout << key << "\n";
        }
        return 0;
    }

    if (options->scan_updates)
    {
        return scan_update_cache(runtime.config);
    }

    const std::string service_name = runtime.config.get_string("service_name", "public tag repository");

    runtime.handlers = std::make_shared<hyd::ApiHandlers>(
        runtime.config, runtime.access_keys, runtime.service_key, service_name, kVersion);
    runtime.handlers->set_build_progress(runtime.build_progress);

    if (!hyd::load_sync_state(runtime.config.state_dir() / "sync.json", runtime.sync_state, error))
    {
        HYD_LOGF(error, "%s", error.c_str());
        return 1;
    }

    const bool inspection_only = options->query || options->verify_store
        || options->check_tag_counts;

    // Held open for the rest of the process lifetime: closing the fd would
    // release the flock. Must live outside the block scope below.
    int lock_fd = -1;
    if (!inspection_only)
    {
        const auto lock_path = runtime.config.data_dir() / "hyd.lock";
        lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lock_fd < 0)
        {
            HYD_LOGF(error, "cannot open lock file %s: %s", lock_path.string().c_str(),
                hyd::errno_string(errno).c_str());
            return 1;
        }
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
        {
            HYD_LOGF(error, "another hydd process is using %s", runtime.config.data_dir().string().c_str());
            ::close(lock_fd);
            return 1;
        }
        // lock_fd intentionally leaks to process exit so the flock is held.

        if (!hyd::validate_storage_backend(runtime.config, runtime.sync_state, error))
        {
            HYD_LOGF(error, "%s", error.c_str());
            return 1;
        }
    }

    if (runtime.sync_state.built)
    {
        std::string reload_error;
        if (reload_store(runtime, reload_error))
        {
            HYD_LOGF(info, "store ready (%s): %llu hashes, %llu mappings, %llu tags",
                runtime.store->backend_name().c_str(),
                static_cast<unsigned long long>(runtime.store->total_hash_count()),
                static_cast<unsigned long long>(runtime.store->total_mapping_count()),
                static_cast<unsigned long long>(runtime.store->total_tag_count()));
        }
        else
        {
            HYD_LOGF(warn, "store is marked built but cannot be opened: %s", reload_error.c_str());
        }
    }

    runtime.handlers->set_sync_state(runtime.sync_state);
    runtime.handlers->set_partial(runtime.sync_state.partial);

    if (options->query)
    {
        const auto hash = hyd::hash_bytes_from_hex(options->query_hash);
        if (!hash)
        {
            std::cerr << "hydd: invalid file hash (expected sha256, sha1, or md5)\n";
            return 2;
        }
        if (!runtime.store)
        {
            std::cerr << "hydd: store is not available\n";
            return 1;
        }
        const auto result = runtime.store->lookup(*hash);
        if (!result.found || result.deleted)
        {
            std::cout << "not found\n";
            return 0;
        }
        std::cout << "file_id " << result.hash_id << "\n";
        std::cout << "storage_tags\n";
        for (const auto& tag : result.storage_tags)
        {
            std::cout << "\t" << tag << "\n";
        }
        std::cout << "display_tags\n";
        for (const auto& tag : result.display_tags)
        {
            std::cout << "\t" << tag << "\n";
        }
        return 0;
    }

    if (options->verify_store)
    {
        if (!runtime.store)
        {
            std::cerr << "hydd: store is not available\n";
            return 1;
        }
        std::cout << "store ok (" << runtime.store->backend_name() << "): "
                  << runtime.store->total_hash_count() << " hashes, "
                  << runtime.store->total_mapping_count() << " mappings, "
                  << runtime.store->total_tag_count() << " tags, "
                  << runtime.store->delta_record_count() << " delta records "
                  << "(base: " << runtime.store->stored_hash_count() << " hashes, "
                  << runtime.store->stored_mapping_count() << " mappings, "
                  << runtime.store->stored_tag_count() << " tags)\n";
        return 0;
    }

    runtime.config.dump_effective();

    if (options->repair_cache)
    {
        hyd::HttpClient::global_init();
        hyd::SyncCallbacks callbacks;
        callbacks.should_stop = [] { return g_stop.load(); };

        hyd::CacheRepairResult result;
        const bool ok = hyd::repair_update_cache(runtime.config, callbacks, result, error);
        hyd::HttpClient::global_cleanup();

        if (!ok)
        {
            HYD_LOGF(error, "update cache repair failed: %s", error.c_str());
            return 1;
        }

        std::cout << "update cache checked " << result.checked << " files: "
                  << result.missing << " missing, " << result.corrupt << " corrupt, "
                  << result.repaired << " repaired\n";
        return 0;
    }

    if (options->compact)
    {
        hyd::SyncCallbacks callbacks;
        callbacks.should_stop = [] { return g_stop.load(); };
        callbacks.on_store_ready = [&runtime] {
            std::string reload_error;
            if (!reload_store(runtime, reload_error))
            {
                HYD_LOGF(warn, "cannot reload store after compaction: %s", reload_error.c_str());
            }
        };

        bool stopped = false;
        if (!hyd::compact_store(runtime.config, callbacks, runtime.sync_state, error))
        {
            HYD_LOGF(error, "compaction failed: %s", error.c_str());
            return 1;
        }
        if (stopped)
        {
            HYD_LOGF(info, "compaction stopped");
        }
        return 0;
    }

    if (options->rebuild_tag_counts)
    {
        if (!runtime.sync_state.built)
        {
            std::cerr << "hydd: the store has not been built yet\n";
            return 1;
        }

        hyd::SyncCallbacks callbacks;
        callbacks.should_stop = [] { return g_stop.load(); };

        if (!hyd::rebuild_tag_metadata(runtime.config, callbacks, error))
        {
            HYD_LOGF(error, "tag metadata rebuild failed: %s", error.c_str());
            return 1;
        }
        return 0;
    }

    if (options->check_tag_counts)
    {
        if (!runtime.sync_state.built)
        {
            std::cerr << "hydd: the store has not been built yet\n";
            return 1;
        }
        if (!runtime.config.get_bool("tag_counts_enabled", true))
        {
            std::cout << "tag counts are disabled (tag_counts_enabled = false)\n";
            return 0;
        }

        hyd::SyncCallbacks callbacks;
        callbacks.should_stop = [] { return g_stop.load(); };

        hyd::TagMetadataCheck result;
        if (!hyd::check_tag_metadata(runtime.config, callbacks, result, error))
        {
            HYD_LOGF(error, "tag metadata check failed: %s", error.c_str());
            return 1;
        }

        std::cout << "tag metadata: " << result.checked << " counts checked, "
                  << result.mismatches << " mismatches, " << result.lookup_entries
                  << "/" << result.expected_lookup_entries << " name entries";
        if (result.expected_postings > 0)
        {
            std::cout << ", " << result.postings << "/" << result.expected_postings
                      << " postings, " << result.hash_index_entries << " hash ids";
        }
        std::cout << "\n";

        bool ok = result.mismatches == 0
            && result.lookup_entries == result.expected_lookup_entries;
        if (result.expected_postings > 0)
        {
            ok = ok && result.postings == result.expected_postings;
        }
        return ok ? 0 : 1;
    }

    if (options->sync_once)
    {
        hyd::HttpClient::global_init();
        bool stopped = false;
        run_sync(runtime, stopped, error);
        if (!error.empty())
        {
            HYD_LOGF(error, "sync failed: %s", error.c_str());
            hyd::HttpClient::global_cleanup();
            return 1;
        }
        build_tag_metadata_if_needed(runtime, false);
        if (stopped)
        {
            HYD_LOGF(info, "sync stopped");
        }
        hyd::HttpClient::global_cleanup();
        return 0;
    }

    struct sigaction action {};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
    // Configuration is intentionally reloaded by restarting the service.
    signal(SIGHUP, SIG_IGN);

    hyd::HttpClient::global_init();

    hyd::ApiServer server(runtime.config.listen_address(), runtime.config.listen_port(),
        static_cast<std::size_t>(runtime.config.get_int("max_connections", 128)),
        [&runtime](const hyd::ApiRequest& request) { return runtime.handlers->handle(request); });

    if (!server.start(error))
    {
        HYD_LOGF(error, "%s", error.c_str());
        hyd::HttpClient::global_cleanup();
        return 1;
    }

    const bool sync_enabled = runtime.config.get_bool("sync_enabled", true);
    std::thread sync_thread;
    std::thread tag_thread;

    if (!sync_enabled && runtime.config.get_bool("tag_counts_enabled", true))
    {
        // No sync thread will run compaction, so a one-shot metadata build is
        // safe to do in parallel with serving requests.
        tag_thread = std::thread([&runtime] { build_tag_metadata_if_needed(runtime, false); });
    }

    if (sync_enabled)
    {
        sync_thread = std::thread([&runtime] {
            const bool repair_enabled = runtime.config.get_bool("repair_enabled", true);
            const bool cache_enabled = runtime.config.get_bool("keep_update_cache", false);
            std::int64_t next_full_repair_at = 0;

            auto run_repairs = [&] {
                if (!repair_enabled || !cache_enabled)
                {
                    // Drop queued requests rather than letting them accumulate.
                    runtime.handlers->take_cache_repair_hashes();
                    runtime.handlers->clear_cache_repair_full();
                    return;
                }

                hyd::SyncCallbacks repair_callbacks;
                repair_callbacks.should_stop = [] { return g_stop.load(); };

                auto hashes = runtime.handlers->take_cache_repair_hashes();
                if (!hashes.empty())
                {
                    hyd::CacheRepairResult result;
                    std::string repair_error;
                    if (!hyd::repair_update_files(
                            runtime.config, hashes, repair_callbacks, result, repair_error))
                    {
                        if (repair_error != "stopped")
                        {
                            HYD_LOGF(warn, "targeted update-cache repair failed: %s",
                                repair_error.c_str());
                        }
                    }
                }

                if (runtime.handlers->cache_repair_full_requested()
                    && hyd::now_unix() >= next_full_repair_at)
                {
                    runtime.handlers->clear_cache_repair_full();
                    // Do not rescan a multi-GB cache more than once per ten
                    // minutes, even if clients keep requesting broken files.
                    next_full_repair_at = hyd::now_unix() + 600;

                    hyd::CacheRepairResult result;
                    std::string repair_error;
                    HYD_LOG(info, "checking the update cache after a relay health failure");
                    if (!hyd::repair_update_cache(
                            runtime.config, repair_callbacks, result, repair_error))
                    {
                        if (repair_error != "stopped")
                        {
                            HYD_LOGF(warn, "update-cache repair failed: %s",
                                repair_error.c_str());
                        }
                    }
                }
            };

            const bool sync_on_start = runtime.config.get_bool("sync_on_start", true);
            runtime.handlers->set_next_check_at(0);

            // Upgrade path, and the explicit refresh knob. Doing this on the
            // sync thread means it can never race with compaction.
            build_tag_metadata_if_needed(runtime, false);

            if (sync_on_start || !runtime.sync_state.built)
            {
                std::string sync_error;
                bool stopped = false;
                run_sync(runtime, stopped, sync_error);
                if (!sync_error.empty())
                {
                    HYD_LOGF(error, "sync failed: %s", sync_error.c_str());
                }
                if (runtime.config.get_bool("tag_counts_rebuild_after_sync", false))
                {
                    build_tag_metadata_if_needed(runtime, true);
                }
            }
            run_repairs();

            while (!g_stop.load())
            {
                const auto interval = std::chrono::seconds(runtime.config.sync_interval_seconds());
                const auto deadline = std::chrono::steady_clock::now() + interval;
                runtime.handlers->set_next_check_at(hyd::now_unix() + interval.count());
                while (!g_stop.load() && std::chrono::steady_clock::now() < deadline)
                {
                    run_repairs();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                if (g_stop.load())
                {
                    break;
                }

                runtime.handlers->set_next_check_at(0);
                std::string sync_error;
                bool stopped = false;
                run_sync(runtime, stopped, sync_error);
                if (!sync_error.empty())
                {
                    HYD_LOGF(error, "sync failed: %s", sync_error.c_str());
                }
                if (runtime.config.get_bool("tag_counts_rebuild_after_sync", false))
                {
                    build_tag_metadata_if_needed(runtime, true);
                }
                run_repairs();
            }
        });
    }

    HYD_LOG(info, "hydd is running; press Ctrl-C or send SIGTERM to stop");

    while (!g_stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.stop();
    if (sync_thread.joinable())
    {
        sync_thread.join();
    }
    if (tag_thread.joinable())
    {
        tag_thread.join();
    }

    hyd::HttpClient::global_cleanup();
    HYD_LOG(info, "hydd stopped");
    hyd::log_close_file();
    hyd::log_shutdown();
    return 0;
}
