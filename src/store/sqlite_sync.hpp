#pragma once

#include "common/config.hpp"
#include "store/builder.hpp"

#include <string>

namespace hyd {

// Plain-SQLite backend used when compression = false. Shares the repository
// protocol and sync policy with the custom store.
bool sync_sqlite(const Config& config, const SyncCallbacks& callbacks, bool& stopped,
    SyncState& state, std::string& error);

bool sqlite_vacuum(const Config& config, std::string& error);

std::filesystem::path sqlite_store_path(const Config& config);

} // namespace hyd
