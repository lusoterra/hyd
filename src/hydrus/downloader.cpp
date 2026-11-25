#include "hydrus/downloader.hpp"

#include "common/log.hpp"
#include "hydrus/repository.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace hyd {

namespace {

std::string without_trailing_slashes(std::string value)
{
    while (!value.empty() && value.back() == '/')
    {
        value.pop_back();
    }
    return value;
}

} // namespace

DownloadSummary download_update_files(const Config& config, const std::vector<DownloadItem>& items,
    const std::function<bool()>& should_stop)
{
    DownloadSummary summary;

    int workers = static_cast<int>(config.get_int("download_workers", 8));
    workers = std::max(1, std::min(32, workers));

    const auto limit = config.max_download_bytes();
    const auto rate_limit = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, config.get_int("download_rate_limit_kib", 0)));

    const std::string download_url = config.repository_url();
    const std::string download_access_key = config.repository_access_key();
    const std::string trust_root = config.trust_root();
    const std::string trust_root_access_key = config.trust_root_access_key();

    // The trust root is only a fallback when it is a different repository. An
    // empty trust_root means repository_url is both the metadata source and
    // the download source, which is the pre-0.8.0 blind-trust behaviour.
    const bool have_trust_root_fallback = !trust_root.empty()
        && without_trailing_slashes(trust_root) != without_trailing_slashes(download_url);

    std::atomic<std::size_t> next { 0 };
    std::atomic<std::uint64_t> downloaded_bytes { 0 };
    std::atomic<std::uint64_t> downloaded_files { 0 };
    std::atomic<std::uint64_t> fallback_files { 0 };
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
        RepositoryClient repository(download_url, download_access_key);
        repository.set_timeout_seconds(config.get_int("download_timeout_seconds", 120));

        RepositoryClient trust_repository(have_trust_root_fallback ? trust_root : download_url,
            have_trust_root_fallback ? trust_root_access_key : download_access_key);
        trust_repository.set_timeout_seconds(config.get_int("download_timeout_seconds", 120));

        if (rate_limit > 0)
        {
            const auto per_worker = std::max<std::uint64_t>(
                1, rate_limit / static_cast<std::uint64_t>(workers));
            repository.set_rate_limit_kib(per_worker);
            trust_repository.set_rate_limit_kib(per_worker);
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
            const auto fetch_result = repository.fetch_update(item.hash, body, error);

            if (fetch_result == UpdateFetchResult::hash_mismatch)
            {
                // A mirror that serves the wrong bytes for a hash the trust
                // root has already committed to is corrupt or compromised.
                // Do not silently paper over that with the trust root: abort
                // so the operator can switch to another instance. Verified
                // cache files are untouched, so a later run resumes.
                const std::string source = have_trust_root_fallback
                    ? "the download source " + download_url
                        + " does not match the SHA-256 published by the trust root "
                        + trust_root
                    : "the repository " + download_url
                        + " served bytes that do not match its own metadata";
                set_error("update " + hex_encode(item.hash) + ": " + source + " ("
                    + error + "). Refusing to use it: the download source may be "
                      "corrupt or compromised. Switch repository_url to another "
                      "instance, or set trust_root = (empty) if you accept this "
                      "source as authoritative.");
                break;
            }

            if (fetch_result == UpdateFetchResult::error)
            {
                if (!have_trust_root_fallback)
                {
                    set_error("update " + hex_encode(item.hash) + ": " + error);
                    break;
                }

                // A mirror that lags the trust root returns 404s (or 5xx) for
                // hashes the metadata already lists. That is not a trust
                // failure, so fetch this one file from the trust root and
                // continue; the mirror can catch up for the next sync.
                HYD_LOGF(warn, "update %s was not available from %s (%s); fetching it from the trust root %s",
                    hex_encode(item.hash).c_str(), download_url.c_str(), error.c_str(),
                    trust_root.c_str());

                Bytes fallback_body;
                std::string fallback_error;
                if (trust_repository.fetch_update(item.hash, fallback_body, fallback_error)
                    != UpdateFetchResult::ok)
                {
                    set_error("update " + hex_encode(item.hash) + ": " + error
                        + "; trust root " + trust_root + " also failed: " + fallback_error);
                    break;
                }

                body = std::move(fallback_body);
                fallback_files.fetch_add(1);
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
    summary.fallback_files = fallback_files.load();

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        summary.error = first_error;
        summary.stopped = stopped.load() && first_error.empty();
    }

    if (summary.fallback_files > 0)
    {
        HYD_LOGF(info,
            "%llu update files came from the trust root %s because %s did not have them yet",
            static_cast<unsigned long long>(summary.fallback_files),
            trust_root.c_str(), download_url.c_str());
    }

    return summary;
}

} // namespace hyd
