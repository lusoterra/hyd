#include "stats.hpp"

#include "common/util.hpp"
#include "hydrus/http.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>

namespace hyd {
namespace {

std::optional<std::uint64_t> get_u64(const nlohmann::json& document, const char* key)
{
    const auto iterator = document.find(key);
    if (iterator == document.end() || iterator->is_null())
    {
        return std::nullopt;
    }
    if (iterator->is_number_unsigned())
    {
        return iterator->get<std::uint64_t>();
    }
    if (iterator->is_number_integer())
    {
        const auto value = iterator->get<std::int64_t>();
        if (value >= 0)
        {
            return static_cast<std::uint64_t>(value);
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> get_i64(const nlohmann::json& document, const char* key)
{
    const auto iterator = document.find(key);
    if (iterator == document.end() || iterator->is_null())
    {
        return std::nullopt;
    }
    if (iterator->is_number_unsigned())
    {
        const auto value = iterator->get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return static_cast<std::int64_t>(value);
        }
        return std::nullopt;
    }
    if (iterator->is_number_integer())
    {
        return iterator->get<std::int64_t>();
    }
    return std::nullopt;
}

std::optional<std::uint64_t> first_u64(
    const nlohmann::json& document, const char* first, const char* second)
{
    if (const auto value = get_u64(document, first))
    {
        return value;
    }
    return get_u64(document, second);
}

std::string commas(std::uint64_t value)
{
    const std::string digits = std::to_string(value);
    std::string result;
    result.reserve(digits.size() + digits.size() / 3);

    int count = 0;
    for (auto iterator = digits.rbegin(); iterator != digits.rend(); ++iterator)
    {
        if (count > 0 && count % 3 == 0)
        {
            result.push_back(',');
        }
        result.push_back(*iterator);
        ++count;
    }

    std::reverse(result.begin(), result.end());
    return result;
}

std::string count_value(std::uint64_t value)
{
    std::ostringstream out;
    if (value >= 1000000000ull)
    {
        out << std::fixed << std::setprecision(2)
            << static_cast<double>(value) / 1000000000.0 << " billion";
    }
    else if (value >= 1000000ull)
    {
        out << std::fixed << std::setprecision(2)
            << static_cast<double>(value) / 1000000.0 << " million";
    }
    else
    {
        return commas(value);
    }

    return out.str() + " (" + commas(value) + ")";
}

std::string utc_time(std::int64_t timestamp)
{
    const std::time_t time = static_cast<std::time_t>(timestamp);
    std::tm parts {};
#if defined(_WIN32)
    gmtime_s(&parts, &time);
#else
    gmtime_r(&time, &parts);
#endif

    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &parts) == 0)
    {
        return "unknown";
    }
    return buffer;
}

std::string duration(std::int64_t seconds)
{
    if (seconds < 0)
    {
        seconds = 0;
    }

    const std::int64_t days = seconds / 86400;
    seconds %= 86400;
    const std::int64_t hours = seconds / 3600;
    seconds %= 3600;
    const std::int64_t minutes = seconds / 60;

    std::ostringstream out;
    if (days > 0)
    {
        out << days << "d ";
    }
    if (days > 0 || hours > 0)
    {
        out << hours << "h ";
    }
    out << minutes << "m";
    return out.str();
}

std::string format_bytes(std::uint64_t bytes)
{
    static constexpr const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << " " << units[unit];
    return out.str();
}

std::string format_ratio(std::uint64_t done, std::uint64_t total)
{
    std::ostringstream out;
    out << commas(done) << " / " << commas(total);
    if (total > 0)
    {
        out << " (" << std::fixed << std::setprecision(1)
            << 100.0 * static_cast<double>(done) / static_cast<double>(total) << "%)";
    }
    return out.str();
}

void add_row(std::ostringstream& out, const char* label, const std::string& value)
{
    out << "  " << std::left << std::setw(17) << label << " " << value << "\n";
}

std::string string_field(const nlohmann::json& document, const char* key)
{
    const auto iterator = document.find(key);
    if (iterator == document.end() || !iterator->is_string())
    {
        return {};
    }
    return iterator->get<std::string>();
}

std::string local_api_url(const Config& config)
{
    std::string host = config.listen_address();
    if (host.empty() || host == "0.0.0.0" || host == "*")
    {
        host = "127.0.0.1";
    }
    else if (host == "::" || host == "[::]")
    {
        host = "[::1]";
    }
    else if (host.find(':') != std::string::npos && host.front() != '[')
    {
        host = "[" + host + "]";
    }

    return "http://" + host + ":" + std::to_string(config.listen_port());
}

std::string local_api_key(const Config& config)
{
    std::string key = config.get_string("access_key");
    if (key.empty())
    {
        if (const char* environment = std::getenv("HYDD_API_KEY"); environment != nullptr)
        {
            key = environment;
        }
    }

    if (key.empty())
    {
        const auto path = config.get_string(
            "access_key_file", (config.data_dir() / "api_access_key").string());
        std::string error;
        if (const auto bytes = read_file(path, error))
        {
            key = trim(std::string(bytes->begin(), bytes->end()));
        }
    }

    return key;
}

} // namespace

std::string render_stats(const nlohmann::json& health, const nlohmann::json* api_version,
    const std::string& api_url, const std::string& fallback_service_name)
{
    std::ostringstream out;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    std::string service_name = string_field(health, "service_name");
    if (service_name.empty())
    {
        service_name = fallback_service_name;
    }

    std::string version = string_field(health, "hyd_version");
    if (version.empty() && api_version != nullptr)
    {
        version = string_field(*api_version, "hyd_version");
    }

    const bool built = health.value("built", false);
    const bool store_open = health.value("store_open", false);
    const bool partial = health.value("partial", false);

    const auto build_iterator = health.find("build");
    const bool have_build = build_iterator != health.end() && build_iterator->is_object();
    const bool build_active = have_build && build_iterator->value("active", false);

    out << "hydd statistics for " << api_url << "\n\n";

    add_row(out, "Service", service_name);
    if (!version.empty())
    {
        add_row(out, "Version", version);
    }

    std::string status;
    if (!built)
    {
        status = have_build && !build_active
            ? "not built yet (check the log)"
            : "building initial store";
    }
    else if (!store_open)
    {
        status = "built, store not open";
    }
    else if (partial)
    {
        status = "ready (partial)";
    }
    else
    {
        status = "ready";
    }
    add_row(out, "Status", status);

    if (build_active)
    {
        const std::string phase = string_field(*build_iterator, "phase");
        if (phase == "partitions")
        {
            const auto partition = get_u64(*build_iterator, "partition").value_or(0);
            const auto partitions = get_u64(*build_iterator, "partitions").value_or(0);
            add_row(out, "Build phase",
                "partitions " + std::to_string(partition) + "/" + std::to_string(partitions));
        }
        else if (!phase.empty())
        {
            add_row(out, "Build phase", phase);
        }

        if (const auto done = get_u64(*build_iterator, "files_done"),
            total = get_u64(*build_iterator, "files_total");
            done && total && *total > 0)
        {
            add_row(out, "Build files", format_ratio(*done, *total));
        }

        if (const auto done = get_u64(*build_iterator, "bytes_done"),
            total = get_u64(*build_iterator, "bytes_total");
            done && total && *total > 0)
        {
            add_row(out, "Build data", format_bytes(*done) + " / " + format_bytes(*total));
        }

        if (const auto elapsed = get_i64(*build_iterator, "phase_elapsed_seconds");
            elapsed && *elapsed > 0)
        {
            add_row(out, "Build elapsed", duration(*elapsed));
        }

        if (const auto eta = get_i64(*build_iterator, "eta_seconds"); eta && *eta > 0)
        {
            add_row(out, "Build ETA", "~" + duration(*eta) + " (this pass)");
        }

        if (const auto hashes = get_u64(*build_iterator, "hashes"); hashes && *hashes > 0)
        {
            add_row(out, "Build hashes", commas(*hashes) + " in this partition");
        }

        if (const auto mappings = get_u64(*build_iterator, "mappings"); mappings && *mappings > 0)
        {
            add_row(out, "Build mapped", commas(*mappings) + " rows in this partition");
        }
    }

    std::string backend = string_field(health, "backend");
    if (!backend.empty())
    {
        if (const auto partitions = get_u64(health, "partitions"); partitions && *partitions > 0)
        {
            backend += ", " + std::to_string(*partitions) + " partitions";
        }
        add_row(out, "Backend", backend);
    }

    if (const auto next_index = get_u64(health, "next_index"))
    {
        if (*next_index > 0)
        {
            add_row(out, "Update index",
                std::to_string(*next_index - 1) + " (next " + std::to_string(*next_index) + ")");
        }
        else
        {
            add_row(out, "Update index", "none yet");
        }
    }

    if (const auto last_sync = get_i64(health, "last_sync_at"); last_sync && *last_sync > 0)
    {
        add_row(out, "Last sync",
            utc_time(*last_sync) + " (" + duration(now - *last_sync) + " ago)");
    }
    else if (built)
    {
        add_row(out, "Last sync", "never");
    }

    if (const auto age = get_i64(health, "data_age_seconds"); age && *age > 0)
    {
        add_row(out, "Data age", duration(*age));
    }

    const bool sync_enabled = health.value("sync_enabled", true);
    if (!sync_enabled)
    {
        add_row(out, "Sync", "disabled by configuration");
    }
    else if (const auto interval = get_i64(health, "sync_interval_seconds"); interval && *interval > 0)
    {
        add_row(out, "Polling", "every " + duration(*interval));
    }

    if (const auto due = get_i64(health, "next_update_due"); due && *due > 0)
    {
        std::string due_value = utc_time(*due);
        if (*due <= now)
        {
            due_value += " (overdue by " + duration(now - *due) + ")";
        }
        else
        {
            due_value += " (in " + duration(*due - now) + ")";
        }
        add_row(out, "PTR update due", due_value);
    }

    if (sync_enabled)
    {
        if (const auto next_check = get_i64(health, "next_check_at"); next_check && *next_check > 0)
        {
            std::string check_value = utc_time(*next_check);
            if (*next_check >= now)
            {
                check_value += " (in " + duration(*next_check - now) + ")";
            }
            add_row(out, "Next check", check_value);
        }

        if (const auto applied = get_i64(health, "estimated_update_applied_at");
            applied && *applied > 0)
        {
            std::string applied_value = "~" + utc_time(*applied);
            if (*applied >= now)
            {
                applied_value += " (in " + duration(*applied - now) + ")";
            }
            add_row(out, "Expected applied", applied_value);
        }
        else if (const auto due = get_i64(health, "next_update_due");
                 due && *due > 0 && !get_i64(health, "next_check_at").value_or(0))
        {
            add_row(out, "Expected applied", "in progress (sync running)");
        }
    }

    bool have_counts = false;
    auto start_counts = [&] {
        if (!have_counts)
        {
            out << "\n";
            have_counts = true;
        }
    };

    const auto hashes_field = first_u64(health, "hashes", "hash_count");
    const auto total_hashes = hashes_field.value_or(0);
    const auto legacy_hashes = get_u64(health, "legacy_hashes").value_or(0);
    if (hashes_field)
    {
        start_counts();
        const auto sha256 = get_u64(health, "sha256_hashes").value_or(
            total_hashes >= legacy_hashes ? total_hashes - legacy_hashes : total_hashes);
        add_row(out, "SHA-256", count_value(sha256));
        if (legacy_hashes > 0)
        {
            add_row(out, "Legacy hashes", count_value(legacy_hashes));
        }
    }

    if (const auto tags = first_u64(health, "tags", "tag_count"))
    {
        start_counts();
        add_row(out, "Tags", count_value(*tags));
    }

    if (const auto mappings = first_u64(health, "mappings", "mapping_count"))
    {
        start_counts();
        add_row(out, "Mappings", count_value(*mappings));
    }

    if (const auto siblings = get_u64(health, "siblings"))
    {
        start_counts();
        add_row(out, "Siblings", count_value(*siblings));
    }

    if (const auto parents = get_u64(health, "parents"))
    {
        start_counts();
        add_row(out, "Parents", count_value(*parents));
    }

    if (const auto delta_records = get_u64(health, "delta_records"))
    {
        start_counts();
        std::string value = commas(*delta_records) + " records";
        if (const auto delta_hashes = get_u64(health, "delta_hashes"))
        {
            value += " (" + commas(*delta_hashes) + " hashes)";
        }
        add_row(out, "Delta", value);
    }

    if (!built)
    {
        if (build_active)
        {
            out << "\n  note: the initial build is in progress; counts will grow until it completes\n";
        }
        else
        {
            out << "\n  note: the initial build has not completed; check the log for errors\n";
        }
    }
    else if (store_open
             && (!get_u64(health, "siblings") || !get_u64(health, "parents")
                 || !get_i64(health, "next_update_due")))
    {
        out << "\n  note: some fields are unavailable from this daemon version; "
               "upgrade hydd for a complete report\n";
    }

    if (built && store_open && get_i64(health, "estimated_update_applied_at").value_or(0) > 0)
    {
        out << "\n  note: expected applied assumes the PTR publishes on schedule "
               "and polling stays regular\n";
    }

    return out.str();
}

int run_stats(const Config& config)
{
    const std::string api_url = local_api_url(config);

    int exit_code = 0;
    {
        HttpClient client(api_url, std::string(), "hydd-stats");
        client.set_timeout_seconds(5);
        client.set_retries(0);

        HttpResponse response;
        std::string error;
        bool ok = client.get("/stats", response, error);
        if (!ok && error.rfind("HTTP 404", 0) == 0)
        {
            // Older daemons only have /health; /stats is the richer hydd
            // extension on new ones.
            ok = client.get("/health", response, error);
        }

        if (!ok)
        {
            std::cerr << "hydd: cannot reach the API at " << api_url << ": " << error << "\n";
            std::cerr << "is hydd running? try: systemctl status hydd\n";
            exit_code = 1;
        }
        else
        {
            const std::string body(response.body.begin(), response.body.end());
            auto health = nlohmann::json::parse(body, nullptr, false);
            if (health.is_discarded() || !health.is_object())
            {
                std::cerr << "hydd: the API at " << api_url << " returned invalid JSON\n";
                exit_code = 1;
            }
            else
            {
                nlohmann::json version_document;
                nlohmann::json* version_pointer = nullptr;

                if (string_field(health, "hyd_version").empty())
                {
                    HttpResponse version_response;
                    std::string version_error;
                    if (client.get("/api_version", version_response, version_error))
                    {
                        const std::string version_body(
                            version_response.body.begin(), version_response.body.end());
                        auto version_json = nlohmann::json::parse(version_body, nullptr, false);
                        if (!version_json.is_discarded() && version_json.is_object())
                        {
                            version_document = std::move(version_json);
                            version_pointer = &version_document;
                        }
                    }
                }

                try
                {
                    std::cout << render_stats(health, version_pointer, api_url,
                        config.get_string("service_name", "public tag repository"));
                }
                catch (const std::exception& exception)
                {
                    std::cerr << "hydd: cannot render statistics: " << exception.what() << "\n";
                    exit_code = 1;
                }
            }
        }
    }

    HttpClient::global_cleanup();
    return exit_code;
}

int run_api_query(const Config& config, std::string_view hash)
{
    if (!hash_bytes_from_hex(hash))
    {
        std::cerr << "hydd: invalid file hash (expected sha256, sha1, or md5)\n";
        return 2;
    }

    const std::string api_url = local_api_url(config);
    const std::string key = local_api_key(config);

    int exit_code = 0;
    {
        HttpClient client(api_url, std::string(), "hydd-query");
        client.set_timeout_seconds(10);
        client.set_retries(0);
        if (!key.empty())
        {
            client.set_api_access_key(key);
        }

        HttpResponse response;
        std::string error;
        const std::string path = "/get_files/file_metadata?hashes=" + std::string(hash);

        if (!client.get(path, response, error))
        {
            if (error.rfind("HTTP 401", 0) == 0 || error.rfind("HTTP 403", 0) == 0)
            {
                std::cerr << "hydd: the API at " << api_url << " requires an access key.\n";
                std::cerr << "run as the hyd user, set HYDD_API_KEY, or set "
                             "require_localhost_key = false to trust localhost\n";
            }
            else
            {
                std::cerr << "hydd: cannot reach the API at " << api_url << ": " << error << "\n";
            }
            exit_code = 1;
        }
        else
        {
            const std::string body(response.body.begin(), response.body.end());
            auto document = nlohmann::json::parse(body, nullptr, false);
            if (document.is_discarded() || !document.is_object()
                || !document.contains("metadata") || !document["metadata"].is_array()
                || document["metadata"].empty())
            {
                std::cerr << "hydd: the API at " << api_url << " returned an unexpected response\n";
                exit_code = 1;
            }
            else
            {
                const auto& row = document["metadata"].front();
                if (!row.contains("file_id") || row["file_id"].is_null())
                {
                    std::cout << "not found\n";
                }
                else
                {
                    std::cout << "file_id " << row["file_id"].dump() << "\n";

                    const auto tags_iterator = row.find("tags");
                    if (tags_iterator != row.end() && tags_iterator->is_object())
                    {
                        const nlohmann::json* service = nullptr;
                        for (auto iterator = tags_iterator->begin();
                            iterator != tags_iterator->end(); ++iterator)
                        {
                            if (iterator->is_object() && iterator->value("type", -1) == 0)
                            {
                                service = &(*iterator);
                                break;
                            }
                        }
                        if (service == nullptr)
                        {
                            for (auto iterator = tags_iterator->begin();
                                iterator != tags_iterator->end(); ++iterator)
                            {
                                if (iterator->is_object())
                                {
                                    service = &(*iterator);
                                    break;
                                }
                            }
                        }

                        if (service != nullptr)
                        {
                            auto print_tags = [](const char* label, const nlohmann::json& value) {
                                std::cout << label << "\n";
                                if (!value.is_object())
                                {
                                    return;
                                }
                                for (auto group = value.begin(); group != value.end(); ++group)
                                {
                                    if (!group->is_array())
                                    {
                                        continue;
                                    }
                                    for (const auto& tag : *group)
                                    {
                                        if (tag.is_string())
                                        {
                                            std::cout << "\t" << tag.get<std::string>() << "\n";
                                        }
                                    }
                                }
                            };

                            if (service->contains("storage_tags"))
                            {
                                print_tags("storage_tags", (*service)["storage_tags"]);
                            }
                            else
                            {
                                std::cout << "storage_tags\n";
                            }

                            if (service->contains("display_tags"))
                            {
                                print_tags("display_tags", (*service)["display_tags"]);
                            }
                            else
                            {
                                std::cout << "display_tags\n";
                            }
                        }
                    }
                }
            }
        }
    }

    HttpClient::global_cleanup();
    return exit_code;
}

} // namespace hyd
