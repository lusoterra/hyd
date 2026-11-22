#pragma once

#include "common/config.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace hyd {

// Renders the /health payload (and optionally /api_version) as a human-readable
// statistics page. Unknown or missing fields are skipped so this works against
// older daemons.
std::string render_stats(const nlohmann::json& health, const nlohmann::json* api_version,
    const std::string& api_url, const std::string& fallback_service_name);

// Queries the local API and prints repository statistics. Returns a process
// exit code.
int run_stats(const Config& config);

} // namespace hyd
