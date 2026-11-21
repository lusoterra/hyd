#pragma once

#include "common/config.hpp"
#include "common/util.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hyd {

struct DownloadItem
{
    Hash hash {};
    std::filesystem::path cache_path;
};

struct DownloadSummary
{
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    bool stopped = false;
    std::string error;
};

// Downloads update files with a small worker pool. Each worker reuses one TLS
// connection. Existing non-empty cache files are skipped.
DownloadSummary download_update_files(const Config& config, const std::vector<DownloadItem>& items,
    const std::function<bool()>& should_stop);

} // namespace hyd
