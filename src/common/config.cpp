#include "common/config.hpp"

#include "common/log.hpp"
#include "common/util.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace hyd {

namespace {

constexpr const char* kDefaultRepositoryUrl = "https://ptr.hydrus.network:45871";
constexpr const char* kDefaultTrustRoot = kDefaultRepositoryUrl;
constexpr const char* kDefaultRepositoryAccessKey =
    "4a285629721ca442541ef2c15ea17d1f7f7578b0c3f4f5f2a05f8f0ab297786f";
// The current PTR corpus is roughly 21 GiB. A finite default stops a broken
// or hostile upstream from filling the disk before the operator notices.
constexpr std::int64_t kDefaultMaxDownloadBytes = 64ll * 1024 * 1024 * 1024;

std::string lower_ascii(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool repository_url_scheme_and_host(const std::string& url, std::string& scheme, std::string& host)
{
    const auto separator = url.find("://");
    if (separator == std::string::npos || separator == 0)
    {
        return false;
    }

    scheme = lower_ascii(url.substr(0, separator));

    std::string authority = url.substr(separator + 3);
    const auto authority_end = authority.find_first_of("/?#");
    if (authority_end != std::string::npos)
    {
        authority.resize(authority_end);
    }

    const auto userinfo = authority.rfind('@');
    if (userinfo != std::string::npos)
    {
        authority.erase(0, userinfo + 1);
    }

    if (authority.empty())
    {
        return false;
    }

    if (authority.front() == '[')
    {
        const auto close = authority.find(']');
        if (close == std::string::npos)
        {
            return false;
        }
        host = lower_ascii(authority.substr(1, close - 1));
    }
    else
    {
        const auto colon = authority.find(':');
        host = lower_ascii(authority.substr(0, colon));
    }

    return !host.empty();
}

bool is_loopback_host(const std::string& host)
{
    if (host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1")
    {
        return true;
    }

    // Accept 127.x.y.z only as a numeric literal. This deliberately rejects
    // DNS names such as 127.example.com that merely start with "127.".
    if (host.rfind("127.", 0) != 0)
    {
        return false;
    }

    for (std::size_t i = 4; i < host.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(host[i])) && host[i] != '.')
        {
            return false;
        }
    }

    return true;
}

bool validate_repository_url(const std::string& key, const std::string& url, std::string& error)
{
    if (url.empty())
    {
        error = key + " must not be empty";
        return false;
    }

    std::string scheme;
    std::string host;
    if (!repository_url_scheme_and_host(url, scheme, host))
    {
        error = key + " must be an absolute URL such as https://ptr.hydrus.network:45871";
        return false;
    }

    if (scheme == "https")
    {
        // Fine. TLS is the transport authentication the ecosystem relies on.
        return true;
    }

    if (scheme == "http" && is_loopback_host(host))
    {
        HYD_LOGF(warn,
            "%s %s is plain http on loopback; only use this for a local test repository",
            key.c_str(), url.c_str());
        return true;
    }

    error = key + " must use https (got " + scheme + "://" + host
        + "); syncing over plain http lets a network attacker replace repository data";
    return false;
}

std::string strip_inline_comment(const std::string& value)
{
    bool quoted = false;
    char quote = '\0';

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (quoted)
        {
            if (c == quote)
            {
                quoted = false;
            }
            continue;
        }

        if (c == '"' || c == '\'')
        {
            quoted = true;
            quote = c;
            continue;
        }

        if ((c == '#' || c == ';') && (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1]))))
        {
            return value.substr(0, i);
        }
    }

    return value;
}

std::string unquote(std::string value)
{
    if (value.size() >= 2)
    {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
        {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

} // namespace

bool Config::load(const std::filesystem::path& path, bool required, std::string& error)
{
    path_ = path;
    values_.clear();

    std::ifstream file(path);
    if (!file)
    {
        if (required)
        {
            error = "cannot open config " + path.string() + ": " + errno_string(errno);
            return false;
        }
        return true;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line))
    {
        ++line_number;

        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[')
        {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            error = path.string() + ":" + std::to_string(line_number) + ": expected key = value";
            return false;
        }

        std::string key = to_lower(trim(line.substr(0, equals)));
        std::string value = trim(strip_inline_comment(line.substr(equals + 1)));
        value = unquote(value);

        if (key.empty())
        {
            error = path.string() + ":" + std::to_string(line_number) + ": empty key";
            return false;
        }

        values_[key] = value;
    }

    return true;
}

void Config::apply_command_line_overrides(
    const std::optional<std::string>& data_dir,
    const std::optional<std::string>& listen_address,
    const std::optional<int>& listen_port,
    const std::optional<bool>& sync_enabled,
    const std::optional<std::string>& log_level)
{
    if (data_dir) values_["data_dir"] = *data_dir;
    if (listen_address) values_["listen_address"] = *listen_address;
    if (listen_port) values_["listen_port"] = std::to_string(*listen_port);
    if (sync_enabled) values_["sync_enabled"] = *sync_enabled ? "true" : "false";
    if (log_level) values_["log_level"] = *log_level;
}

bool Config::finalize(std::string& error)
{
    if (values_.contains("compaction_enabled"))
    {
        HYD_LOG(warn,
            "compaction_enabled is no longer supported; compaction always runs when "
            "the delta reaches compact_after_delta_mb");
    }

    data_dir_ = get_string("data_dir", "/var/lib/hyd");
    if (data_dir_.is_relative())
    {
        data_dir_ = std::filesystem::absolute(data_dir_);
    }

    store_dir_ = data_dir_ / "store";
    cache_dir_ = data_dir_ / "cache";
    state_dir_ = data_dir_ / "state";

    if (listen_port() < 1 || listen_port() > 65535)
    {
        error = "listen_port must be between 1 and 65535";
        return false;
    }

    const int parts = partitions();
    if (parts < 1 || parts > 64 || (parts & (parts - 1)) != 0)
    {
        error = "partitions must be a power of two between 1 and 64";
        return false;
    }

    const auto interval = parse_i64(get_string("sync_interval_seconds", "3600"));
    if (!interval || *interval < 60)
    {
        error = "sync_interval_seconds must be at least 60";
        return false;
    }

    if (!validate_repository_url("repository_url", repository_url(), error))
    {
        return false;
    }

    // An empty trust_root is deliberate: it means the download source is also
    // the metadata source (blind trust). A non-empty value must still be a
    // sane repository URL.
    if (!trust_root().empty()
        && !validate_repository_url("trust_root", trust_root(), error))
    {
        return false;
    }

    // Make the upgrade behaviour explicit: changing repository_url no longer
    // changes who publishes the update hashes unless trust_root is cleared.
    if (!values_.contains("trust_root") && repository_url() != kDefaultTrustRoot)
    {
        HYD_LOGF(warn,
            "repository_url is %s but trust_root was not set; update hashes will "
            "be verified against the official PTR. Set trust_root = (empty) to "
            "trust repository_url for both metadata and update bytes.",
            repository_url().c_str());
    }

    const auto max_download = parse_i64(get_string("max_download_bytes", ""));
    if (max_download && *max_download < 0)
    {
        error = "max_download_bytes must be zero (unlimited) or a positive byte count";
        return false;
    }

    const auto max_connections = get_int("max_connections", 128);
    if (max_connections < 1 || max_connections > 4096)
    {
        error = "max_connections must be between 1 and 4096";
        return false;
    }

    if (get_bool("rate_limit_enabled", true))
    {
        const auto per_minute = get_int("rate_limit_requests_per_minute", 600);
        if (per_minute < 1)
        {
            error = "rate_limit_requests_per_minute must be at least 1";
            return false;
        }

        const auto burst = get_int("rate_limit_burst", 120);
        if (burst < 1)
        {
            error = "rate_limit_burst must be at least 1";
            return false;
        }
    }

    if (get_bool("tag_hashes_enabled", false) && !get_bool("tag_counts_enabled", true))
    {
        error = "tag_hashes_enabled requires tag_counts_enabled = true";
        return false;
    }

    return true;
}

std::string Config::get_string(const std::string& key, const std::string& fallback) const
{
    const auto iterator = values_.find(key);
    if (iterator == values_.end() || iterator->second.empty())
    {
        return fallback;
    }
    return iterator->second;
}

std::int64_t Config::get_int(const std::string& key, std::int64_t fallback) const
{
    const auto value = get_string(key);
    if (value.empty())
    {
        return fallback;
    }
    const auto parsed = parse_i64(value);
    return parsed ? *parsed : fallback;
}

bool Config::get_bool(const std::string& key, bool fallback) const
{
    const auto value = get_string(key);
    if (value.empty())
    {
        return fallback;
    }
    const auto parsed = parse_bool(value);
    return parsed ? *parsed : fallback;
}

int Config::listen_port() const
{
    return static_cast<int>(get_int("listen_port", 45869));
}

std::string Config::repository_url() const
{
    return get_string("repository_url", kDefaultRepositoryUrl);
}

std::string Config::repository_access_key() const
{
    return get_string("repository_access_key", kDefaultRepositoryAccessKey);
}

std::string Config::trust_root() const
{
    const auto iterator = values_.find("trust_root");
    if (iterator == values_.end())
    {
        return kDefaultTrustRoot;
    }

    // An explicitly empty value disables split trust. get_string() cannot be
    // used here because it folds an empty value into the fallback.
    return iterator->second;
}

std::string Config::trust_root_access_key() const
{
    return get_string("trust_root_access_key", repository_access_key());
}

std::int64_t Config::sync_interval_seconds() const
{
    return get_int("sync_interval_seconds", 3600);
}

int Config::partitions() const
{
    return static_cast<int>(get_int("partitions", 16));
}

std::uint64_t Config::max_download_bytes() const
{
    const auto text = get_string("max_download_bytes");
    if (text.empty())
    {
        return static_cast<std::uint64_t>(kDefaultMaxDownloadBytes);
    }

    const auto value = parse_i64(text);
    if (!value || *value <= 0)
    {
        // Zero (or an unparsable value already rejected by finalize) means
        // no limit.
        return 0;
    }

    return static_cast<std::uint64_t>(*value);
}

void Config::dump_effective() const
{
    HYD_LOGF(info, "config file: %s", path_.string().c_str());
    HYD_LOGF(info, "data_dir: %s", data_dir_.string().c_str());
    HYD_LOGF(info, "listen: %s:%d", listen_address().c_str(), listen_port());
    HYD_LOGF(info, "repository (downloads): %s", repository_url().c_str());
    const auto trust = trust_root();
    if (trust.empty())
    {
        HYD_LOGF(info, "trust_root: (empty; trusting repository_url for metadata and downloads)");
    }
    else if (trust == repository_url())
    {
        HYD_LOGF(info, "trust_root: %s (same as repository_url)", trust.c_str());
    }
    else
    {
        HYD_LOGF(info, "trust_root: %s (update hashes come from here; downloads come from %s)",
            trust.c_str(), repository_url().c_str());
    }
    HYD_LOGF(info, "sync_enabled: %s", get_bool("sync_enabled", true) ? "true" : "false");
    HYD_LOGF(info, "sync_on_start: %s", get_bool("sync_on_start", true) ? "true" : "false");
    HYD_LOGF(info, "sync_interval_seconds: %lld", static_cast<long long>(sync_interval_seconds()));
    HYD_LOGF(info, "partitions: %d", partitions());
    const auto download_cap = max_download_bytes();
    const std::string download_cap_text = download_cap == 0
        ? std::string("unlimited")
        : human_bytes(download_cap);
    HYD_LOGF(info, "max_download_bytes: %s", download_cap_text.c_str());
    HYD_LOGF(info, "keep_update_cache: %s", get_bool("keep_update_cache", false) ? "true" : "false");
    HYD_LOGF(info, "tag_counts_enabled: %s", get_bool("tag_counts_enabled", true) ? "true" : "false");
    HYD_LOGF(info, "tag_counts_rebuild_after_sync: %s",
        get_bool("tag_counts_rebuild_after_sync", false) ? "true" : "false");
    HYD_LOGF(info, "tag_hashes_enabled: %s", get_bool("tag_hashes_enabled", false) ? "true" : "false");
    HYD_LOGF(info, "require_localhost_key: %s", get_bool("require_localhost_key", false) ? "true" : "false");
    HYD_LOGF(info, "max_connections: %lld", static_cast<long long>(get_int("max_connections", 128)));
    HYD_LOGF(info, "rate_limit_enabled: %s", get_bool("rate_limit_enabled", true) ? "true" : "false");
    HYD_LOGF(info, "rate_limit_requests_per_minute: %lld",
        static_cast<long long>(get_int("rate_limit_requests_per_minute", 600)));
    HYD_LOGF(info, "rate_limit_burst: %lld", static_cast<long long>(get_int("rate_limit_burst", 120)));
    HYD_LOGF(info, "rate_limit_local_bypass: %s",
        get_bool("rate_limit_local_bypass", false) ? "true" : "false");
    HYD_LOGF(info, "log_level: %s", get_string("log_level", "info").c_str());
    HYD_LOGF(info, "log_file: %s", get_string("log_file", "(stderr only)").c_str());
    HYD_LOGF(info, "repository_access_key: %s", repository_access_key().empty() ? "(none)" : "(set)");
    if (!trust.empty() && trust != repository_url())
    {
        HYD_LOGF(info, "trust_root_access_key: %s",
            trust_root_access_key().empty() ? "(none)" : "(set)");
    }
}

} // namespace hyd
