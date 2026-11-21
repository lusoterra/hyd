#include "api/handlers.hpp"
#include "api/server.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/util.hpp"
#include "hydrus/http.hpp"
#include "store/builder.hpp"
#include "store/store.hpp"
#include "store/sqlite_store.hpp"
#include "store/tag_source.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <memory>
#include <thread>
#include <sys/stat.h>

namespace {

constexpr const char* kVersion = "0.1.0";

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
    bool verify_store = false;
    bool compact = false;
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
        "      --query HASH           Print the tags for a SHA-256 and exit\n"
        "      --verify-store         Open and validate the store and exit\n"
        "      --compact              Merge pending deltas into the base store and exit\n"
        "      --print-access-key     Print the configured API access key and exit\n"
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
        { "verify-store", no_argument, nullptr, 1003 },
        { "print-access-key", no_argument, nullptr, 1004 },
        { "compact", no_argument, nullptr, 1006 },
        { "log-level", required_argument, nullptr, 1005 },
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
            case 'h':
                print_help();
                exit(0);
            case 'V':
                std::cout << "hydd " << kVersion << "\n";
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

struct Runtime
{
    hyd::Config config;
    std::string access_key;
    std::string service_key;
    std::shared_ptr<hyd::ApiHandlers> handlers;
    std::shared_ptr<const hyd::TagSource> store;
    hyd::SyncState sync_state;
    std::mutex store_mutex;
};

bool reload_store(Runtime& runtime, std::string& error)
{
    std::shared_ptr<hyd::TagSource> store;

    if (runtime.config.get_bool("compression", true))
    {
        auto custom = std::make_shared<hyd::Store>();
        if (!custom->open(runtime.config.store_dir(), error))
        {
            return false;
        }
        store = custom;
    }
    else
    {
        auto sqlite = std::make_shared<hyd::SqliteStore>();
        if (!sqlite->open(runtime.config.data_dir() / "sqlite" / "hyd.sqlite3", error))
        {
            return false;
        }
        store = sqlite;
    }

    runtime.handlers->set_store(store);
    {
        std::lock_guard<std::mutex> lock(runtime.store_mutex);
        runtime.store = store;
    }
    return true;
}

void run_sync(Runtime& runtime, bool& stopped, std::string& error)
{
    hyd::SyncCallbacks callbacks;
    callbacks.should_stop = [] { return g_stop.load(); };
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

    hyd::log_set_level(hyd::log_level_from_string(runtime.config.get_string("log_level", "info")));

    if (!hyd::mkdir_p(runtime.config.data_dir(), error)
        || !hyd::mkdir_p(runtime.config.state_dir(), error))
    {
        HYD_LOGF(error, "%s", error.c_str());
        return 1;
    }

    std::string access_key = runtime.config.get_string("access_key");
    if (access_key.empty())
    {
        const auto access_key_file = runtime.config.get_string(
            "access_key_file", (runtime.config.data_dir() / "api_access_key").string());
        if (!read_or_create_secret(access_key_file, access_key, error))
        {
            HYD_LOGF(error, "%s", error.c_str());
            return 1;
        }
    }
    runtime.access_key = access_key;

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
        std::cout << runtime.access_key << "\n";
        return 0;
    }

    const std::string service_name = runtime.config.get_string("service_name", "public tag repository");

    runtime.handlers = std::make_shared<hyd::ApiHandlers>(
        runtime.config, runtime.access_key, runtime.service_key, service_name);

    if (!hyd::load_sync_state(runtime.config.state_dir() / "sync.json", runtime.sync_state, error))
    {
        HYD_LOGF(error, "%s", error.c_str());
        return 1;
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
        const auto hash = hyd::hash_from_hex(options->query_hash);
        if (!hash)
        {
            std::cerr << "hydd: invalid SHA-256 hash\n";
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

    {
        const auto lock_path = runtime.config.data_dir() / "hyd.lock";
        const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lock_fd < 0)
        {
            HYD_LOGF(error, "cannot open lock file %s: %s", lock_path.string().c_str(),
                hyd::errno_string(errno).c_str());
            return 1;
        }
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
        {
            HYD_LOGF(error, "another hydd process is using %s", runtime.config.data_dir().string().c_str());
            return 1;
        }
        // Intentionally keep lock_fd open for the lifetime of the process.
    }

    runtime.config.dump_effective();

    if (options->compact)
    {
        if (!runtime.config.get_bool("compression", true))
        {
            runtime.store.reset();
            runtime.handlers->set_store(nullptr);
            if (!hyd::sqlite_vacuum(runtime.config, error))
            {
                HYD_LOGF(error, "SQLite vacuum failed: %s", error.c_str());
                return 1;
            }
            return 0;
        }

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
        [&runtime](const hyd::ApiRequest& request) { return runtime.handlers->handle(request); });

    if (!server.start(error))
    {
        HYD_LOGF(error, "%s", error.c_str());
        hyd::HttpClient::global_cleanup();
        return 1;
    }

    const bool sync_enabled = runtime.config.get_bool("sync_enabled", true);
    std::thread sync_thread;

    if (sync_enabled)
    {
        sync_thread = std::thread([&runtime] {
            const bool sync_on_start = runtime.config.get_bool("sync_on_start", true);
            if (sync_on_start || !runtime.sync_state.built)
            {
                std::string sync_error;
                bool stopped = false;
                run_sync(runtime, stopped, sync_error);
                if (!sync_error.empty())
                {
                    HYD_LOGF(error, "sync failed: %s", sync_error.c_str());
                }
            }

            while (!g_stop.load())
            {
                const auto interval = std::chrono::seconds(runtime.config.sync_interval_seconds());
                const auto deadline = std::chrono::steady_clock::now() + interval;
                while (!g_stop.load() && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                if (g_stop.load())
                {
                    break;
                }

                std::string sync_error;
                bool stopped = false;
                run_sync(runtime, stopped, sync_error);
                if (!sync_error.empty())
                {
                    HYD_LOGF(error, "sync failed: %s", sync_error.c_str());
                }
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

    hyd::HttpClient::global_cleanup();
    HYD_LOG(info, "hydd stopped");
    hyd::log_shutdown();
    return 0;
}
