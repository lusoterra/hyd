#include "hydrus/downloader.hpp"

#include "common/log.hpp"
#include "hydrus/repository.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace hyd {

DownloadSummary download_update_files(const Config& config, const std::vector<DownloadItem>& items,
    const std::function<bool()>& should_stop)
{
    DownloadSummary summary;

    int workers = static_cast<int>(config.get_int("download_workers", 8));
    workers = std::max(1, std::min(32, workers));

    const auto limit = config.max_download_bytes();
    const auto rate_limit = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, config.get_int("download_rate_limit_kib", 0)));

    std::atomic<std::size_t> next { 0 };
    std::atomic<std::uint64_t> downloaded_bytes { 0 };
    std::atomic<std::uint64_t> downloaded_files { 0 };
    std::atomic<bool> abort { false };
    std::atomic<bool> stopped { false };

    std::mutex error_mutex;
    std::string first_error;

    auto set_error = [&](std::string message) {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (first_error.empty())
            {
                first_error = std::move(message);
            }
        }
        abort.store(true);
    };

    auto worker = [&] {
        RepositoryClient repository(config.repository_url(), config.repository_access_key());
        repository.set_timeout_seconds(config.get_int("download_timeout_seconds", 120));
        if (rate_limit > 0)
        {
            repository.set_rate_limit_kib(std::max<std::uint64_t>(1, rate_limit / static_cast<std::uint64_t>(workers)));
        }

        while (!abort.load())
        {
            if (should_stop && should_stop())
            {
                stopped.store(true);
                abort.store(true);
                break;
            }

            const std::size_t index = next.fetch_add(1);
            if (index >= items.size())
            {
                break;
            }

            const auto& item = items[index];
            if (path_exists(item.cache_path) && file_size_or_zero(item.cache_path) > 0)
            {
                std::string verify_error;
                const auto cached = read_file(item.cache_path, verify_error);
                if (cached && sha256(*cached) == item.hash)
                {
                    continue;
                }

                HYD_LOGF(warn, "cached update file %s is corrupt; re-downloading",
                    item.cache_path.string().c_str());
                std::error_code remove_ec;
                std::filesystem::remove(item.cache_path, remove_ec);
            }

            Bytes body;
            std::string error;
            if (!repository.fetch_update(item.hash, body, error))
            {
                set_error("update " + hex_encode(item.hash) + ": " + error);
                break;
            }

            const auto total = downloaded_bytes.fetch_add(body.size()) + body.size();
            if (limit != 0 && total > limit)
            {
                set_error("download limit of " + human_bytes(limit) + " reached; refusing to download more update files");
                break;
            }

            if (!write_file_atomic(item.cache_path, body, error))
            {
                set_error(error);
                break;
            }

            const auto files = downloaded_files.fetch_add(1) + 1;
            if (files % 100 == 0 || files == items.size())
            {
                HYD_LOGF(info, "downloaded %llu/%zu update files (%s)",
                    static_cast<unsigned long long>(files),
                    items.size(),
                    human_bytes(downloaded_bytes.load()).c_str());
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i)
    {
        threads.emplace_back(worker);
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    summary.bytes = downloaded_bytes.load();
    summary.files = downloaded_files.load();

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        summary.error = first_error;
        summary.stopped = stopped.load() && first_error.empty();
    }

    return summary;
}

} // namespace hyd
