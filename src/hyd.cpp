// hyd - query a hydd read-only PTR mirror.
//
//   hyd [--sha256|--sha1|--md5] FILE   hash FILE and query its tags
//   hyd HASH                           query a SHA-256, SHA-1, or MD5 hash
//   hyd stats|scan|repair|verify|version
//   hyd counts TAG...                  mapping count per tag from the running API
//   hyd hashes [--offset N] [--limit N] [--after ID] TAG...
//                                      paged SHA-256 list (tags are ANDed; -tag excludes)
//   hyd hashes-after TAG ID [LIMIT]    continue after a next_after hash id
//   hyd rebuild-counts                 rebuild counts.bin and tag_lookup.bin
//   hyd check                          verify the store and the derived counts
//
// Set HYD_BIN or HYD_CONFIG to override the defaults.

#include <openssl/evp.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kDefaultBin = "hydd";
constexpr const char* kDefaultConfig = "/etc/hydd.conf";

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "hyd: " << message << "\n";
    std::exit(1);
}

std::string env_or(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

void usage()
{
    std::cout <<
        "usage: hyd [--sha256|--sha1|--md5] FILE|HASH\n"
        "       hyd stats\n"
        "       hyd counts TAG...\n"
        "       hyd hashes [--offset N] [--limit N] [--after ID] TAG...\n"
        "       hyd hashes-after TAG ID [LIMIT]\n"
        "       hyd rebuild-counts\n"
        "       hyd check\n"
        "       hyd scan\n"
        "       hyd repair\n"
        "       hyd verify\n"
        "       hyd version\n"
        "\n"
        "Arguments:\n"
        "  FILE       Hash the file (SHA-256 by default) and query it.\n"
        "  HASH       Query a 64-character SHA-256, 40-character SHA-1, or\n"
        "             32-character MD5 hash directly.\n"
        "  TAG...     One or more storage tags. For `hyd hashes`, multiple tags\n"
        "             are ANDed and a leading '-' excludes (quote tags with spaces).\n"
        "\n"
        "Options:\n"
        "  --sha256   Hash FILE with SHA-256 (default).\n"
        "  --sha1     Hash FILE with SHA-1.\n"
        "  --md5      Hash FILE with MD5.\n"
        "  -h, --help Show usage.\n"
        "\n"
        "Environment:\n"
        "  HYD_BIN     hydd executable (default: hydd)\n"
        "  HYD_CONFIG  config file (default: /etc/hydd.conf)\n"
        "\n"
        "The query commands use the local API, so the daemon must be running.\n"
        "Loopback requests are trusted by default. If the daemon sets\n"
        "require_localhost_key = true, run as the hyd user or set HYDD_API_KEY.\n";
}

bool is_regular_file(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(path), error);
}

std::optional<std::string> find_executable(const std::string& name)
{
    if (name.empty())
    {
        return std::nullopt;
    }
    if (name.find('/') != std::string::npos)
    {
        if (access(name.c_str(), X_OK) == 0)
        {
            return name;
        }
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    const std::string path = path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (true)
    {
        const std::size_t end = path.find(':', start);
        std::string directory = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (directory.empty())
        {
            directory = ".";
        }
        const std::string candidate = directory + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::vector<char*> to_argv(std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
    {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return argv;
}

int wait_for(pid_t pid)
{
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno == EINTR)
        {
            continue;
        }
        return 1;
    }
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

struct RunResult
{
    int status = 1;
    std::string output;
};

// Runs the resolved executable with inherited stdio. argv[0] is the command
// name as the shell would pass it.
int run_inherit(const std::string& program, const std::vector<std::string>& args)
{
    auto argv_storage = args;
    auto argv = to_argv(argv_storage);

    std::cout.flush();
    std::cerr.flush();

    const pid_t pid = fork();
    if (pid < 0)
    {
        std::cerr << "hyd: fork: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0)
    {
        execv(program.c_str(), argv.data());
        std::cerr << "hyd: cannot execute " << program << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }
    return wait_for(pid);
}

// Runs the resolved executable and captures its output. When merge_stderr is
// true the child's stderr is redirected into the pipe as well, matching the
// shell's `2>&1` command substitution.
RunResult run_capture(const std::string& program, const std::vector<std::string>& args, bool merge_stderr)
{
    auto argv_storage = args;
    auto argv = to_argv(argv_storage);

    std::cout.flush();
    std::cerr.flush();

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
    {
        std::cerr << "hyd: pipe: " << std::strerror(errno) << "\n";
        return { 1, {} };
    }

    const pid_t pid = fork();
    if (pid < 0)
    {
        std::cerr << "hyd: fork: " << std::strerror(errno) << "\n";
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return { 1, {} };
    }
    if (pid == 0)
    {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        if (merge_stderr)
        {
            dup2(pipe_fds[1], STDERR_FILENO);
        }
        close(pipe_fds[1]);
        execv(program.c_str(), argv.data());
        std::fprintf(stderr, "hyd: cannot execute %s: %s\n", program.c_str(), std::strerror(errno));
        _exit(127);
    }

    close(pipe_fds[1]);
    RunResult result;
    std::array<char, 65536> buffer {};
    while (true)
    {
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0)
        {
            result.output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        break;
    }
    close(pipe_fds[0]);
    result.status = wait_for(pid);
    return result;
}

// Matches the shell's `printf '%s\n' "$output"`, which drops trailing
// newlines from the command substitution and adds one back.
void print_trimmed(std::ostream& stream, const std::string& text)
{
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == '\n')
    {
        --end;
    }
    if (end == 0)
    {
        return;
    }
    stream.write(text.data(), static_cast<std::streamsize>(end));
    stream.put('\n');
}

class Client
{
public:
    Client()
        : bin_(env_or("HYD_BIN", kDefaultBin))
        , config_(env_or("HYD_CONFIG", kDefaultConfig))
    {
    }

    void require_bin()
    {
        const auto resolved = find_executable(bin_);
        if (!resolved)
        {
            fail("cannot find " + bin_ + " (set HYD_BIN)");
        }
        path_ = *resolved;
    }

    int stats()
    {
        return run({ "-c", config_, "--stats" });
    }

    int counts(const std::vector<std::string>& tags)
    {
        if (tags.empty())
        {
            fail("no tag given");
        }
        int status = 0;
        for (const auto& tag : tags)
        {
            if (run({ "-c", config_, "--tag-counts", tag }) != 0)
            {
                status = 1;
            }
        }
        return status;
    }

    int hashes(const std::vector<std::string>& args)
    {
        std::string offset = "0";
        std::string limit = "100";
        std::string after;

        std::size_t index = 0;
        while (index < args.size())
        {
            const std::string& argument = args[index];
            if (argument == "--offset")
            {
                if (index + 1 >= args.size())
                {
                    fail("--offset needs a value");
                }
                offset = args[index + 1];
                index += 2;
            }
            else if (argument == "--limit")
            {
                if (index + 1 >= args.size())
                {
                    fail("--limit needs a value");
                }
                limit = args[index + 1];
                index += 2;
            }
            else if (argument == "--after")
            {
                if (index + 1 >= args.size())
                {
                    fail("--after needs a value");
                }
                after = args[index + 1];
                index += 2;
            }
            else if (argument == "--")
            {
                ++index;
                break;
            }
            else
            {
                break;
            }
        }

        if (index >= args.size())
        {
            fail("no tag given");
        }

        std::vector<std::string> command { "-c", config_, "--tag-hashes" };
        if (!after.empty())
        {
            command.push_back("--hash-after");
            command.push_back(after);
        }
        else
        {
            command.push_back("--hash-offset");
            command.push_back(offset);
        }
        command.push_back("--hash-limit");
        command.push_back(limit);
        command.push_back("--");
        command.insert(command.end(), args.begin() + static_cast<std::ptrdiff_t>(index), args.end());
        return run(command);
    }

    int hashes_after(const std::vector<std::string>& args)
    {
        if (args.empty())
        {
            fail("no tag given");
        }
        if (args.size() < 2)
        {
            fail("no hash id given");
        }
        const std::string& tag = args[0];
        const std::string& after = args[1];
        const std::string limit = args.size() >= 3 ? args[2] : "100";
        if (args.size() > 3)
        {
            fail("usage: hyd hashes-after TAG ID [LIMIT]");
        }

        return run({ "-c", config_, "--tag-hashes",
            "--hash-after", after, "--hash-limit", limit, "--", tag });
    }

    int rebuild_counts()
    {
        return run_privileged({ "-c", config_, "--rebuild-tag-counts" });
    }

    int check()
    {
        if (run_privileged({ "-c", config_, "--verify-store" }) != 0)
        {
            return 1;
        }
        return run_privileged({ "-c", config_, "--check-tag-counts" });
    }

    int scan()
    {
        if (run({ "-c", config_, "--scan-updates" }) != 0)
        {
            std::cerr << "hyd: hint: the update cache may need read access; try sudo -u hyd\n";
            return 1;
        }
        return 0;
    }

    int repair()
    {
        if (run({ "-c", config_, "--repair-cache" }) != 0)
        {
            std::cerr << "hyd: hint: the update cache may need write access; try sudo -u hyd\n";
            return 1;
        }
        return 0;
    }

    int verify()
    {
        if (run({ "-c", config_, "--verify-store" }) != 0)
        {
            std::cerr << "hyd: hint: the store may need read access; try sudo -u hyd\n";
            return 1;
        }
        return 0;
    }

    int version()
    {
        const RunResult result = run_capture(path_, { bin_, "--version" }, false);
        std::string output = result.output;
        std::size_t end = output.size();
        while (end > 0 && output[end - 1] == '\n')
        {
            --end;
        }
        output.resize(end);

        if (output.rfind("hyd v", 0) == 0)
        {
            std::cout << output << "\n";
        }
        else if (output.rfind("hydd ", 0) == 0)
        {
            std::cout << "hyd v" << output.substr(5) << "\n";
        }
        else if (output.rfind("hyd ", 0) == 0)
        {
            std::cout << "hyd v" << output.substr(4) << "\n";
        }
        else
        {
            std::cout << "hyd v" << output << "\n";
        }
        return 0;
    }

    int query(const std::string& hash)
    {
        return run({ "-c", config_, "--api-query", hash });
    }

private:
    int run(const std::vector<std::string>& args)
    {
        std::vector<std::string> command { bin_ };
        command.insert(command.end(), args.begin(), args.end());
        return run_inherit(path_, command);
    }

    int run_privileged(const std::vector<std::string>& args)
    {
        std::vector<std::string> command { bin_ };
        command.insert(command.end(), args.begin(), args.end());

        const RunResult result = run_capture(path_, command, true);
        if (result.status == 0)
        {
            print_trimmed(std::cout, result.output);
            return 0;
        }

        const bool permission_problem =
            result.output.find("Permission denied") != std::string::npos ||
            result.output.find("cannot open") != std::string::npos ||
            result.output.find("cannot create") != std::string::npos ||
            result.output.find("cannot chmod") != std::string::npos ||
            result.output.find("another hydd process") != std::string::npos;

        if (permission_problem && getuid() != 0)
        {
            std::cerr << "hyd: retrying with elevated privileges\n";

            for (const char* tool : { "doas", "sudo" })
            {
                const auto resolved = find_executable(tool);
                if (!resolved)
                {
                    continue;
                }
                std::vector<std::string> elevated { tool };
                elevated.insert(elevated.end(), command.begin(), command.end());
                return run_inherit(*resolved, elevated);
            }

            print_trimmed(std::cerr, result.output);
            fail("need root (or the hyd user); install doas or sudo, or run it yourself");
        }

        print_trimmed(std::cerr, result.output);
        return result.status;
    }

    std::string bin_;
    std::string config_;
    std::string path_;
};

bool is_hex(std::string_view value)
{
    for (const char character : value)
    {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper)
        {
            return false;
        }
    }
    return true;
}

bool is_hash(const std::string& value)
{
    const std::size_t length = value.size();
    if (length != 64 && length != 40 && length != 32)
    {
        return false;
    }
    return is_hex(value);
}

std::string hash_file(const std::string& algorithm, const std::string& file)
{
    if (!is_regular_file(file))
    {
        fail("not a file: " + file);
    }

    const EVP_MD* digest = nullptr;
    if (algorithm == "sha256")
    {
        digest = EVP_sha256();
    }
    else if (algorithm == "sha1")
    {
        digest = EVP_sha1();
    }
    else if (algorithm == "md5")
    {
        digest = EVP_md5();
    }
    else
    {
        fail("unknown hash algorithm: " + algorithm);
    }

    std::FILE* handle = std::fopen(file.c_str(), "rb");
    if (handle == nullptr)
    {
        fail("cannot read " + file + ": " + std::strerror(errno));
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, digest, nullptr) != 1)
    {
        EVP_MD_CTX_free(context);
        std::fclose(handle);
        fail("cannot initialize " + algorithm);
    }

    std::array<char, 65536> buffer {};
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), handle)) > 0)
    {
        EVP_DigestUpdate(context, buffer.data(), count);
    }
    const bool read_failed = std::ferror(handle) != 0;
    const int read_errno = errno;
    std::fclose(handle);

    if (read_failed)
    {
        EVP_MD_CTX_free(context);
        fail("cannot read " + file + ": " + std::strerror(read_errno));
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> value {};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, value.data(), &length) != 1)
    {
        EVP_MD_CTX_free(context);
        fail("cannot finalize " + algorithm);
    }
    EVP_MD_CTX_free(context);

    static const char* hex = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index)
    {
        encoded.push_back(hex[value[index] >> 4]);
        encoded.push_back(hex[value[index] & 0x0F]);
    }
    return encoded;
}

} // namespace

int main(int argc, char** argv)
{
    Client client;
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty())
    {
        usage();
        return 1;
    }

    const std::string command = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "stats")
    {
        client.require_bin();
        client.stats();
        return 0;
    }
    if (command == "counts")
    {
        client.require_bin();
        return client.counts(rest);
    }
    if (command == "hashes")
    {
        client.require_bin();
        return client.hashes(rest);
    }
    if (command == "hashes-after")
    {
        client.require_bin();
        return client.hashes_after(rest);
    }
    if (command == "rebuild-counts")
    {
        client.require_bin();
        return client.rebuild_counts();
    }
    if (command == "check")
    {
        client.require_bin();
        return client.check();
    }
    if (command == "scan")
    {
        client.require_bin();
        return client.scan();
    }
    if (command == "repair")
    {
        client.require_bin();
        return client.repair();
    }
    if (command == "verify")
    {
        client.require_bin();
        return client.verify();
    }
    if (command == "version" || command == "--version" || command == "-V")
    {
        client.require_bin();
        return client.version();
    }
    if (command == "help" || command == "-h" || command == "--help")
    {
        usage();
        return 0;
    }

    std::string algorithm = "sha256";
    std::string argument;
    for (const auto& value : args)
    {
        if (value == "--sha256")
        {
            algorithm = "sha256";
        }
        else if (value == "--sha1")
        {
            algorithm = "sha1";
        }
        else if (value == "--md5")
        {
            algorithm = "md5";
        }
        else if (!value.empty() && value[0] == '-')
        {
            fail("unknown option: " + value);
        }
        else
        {
            if (!argument.empty())
            {
                fail("only one file or hash is supported");
            }
            argument = value;
        }
    }

    if (argument.empty())
    {
        fail("no file or hash given");
    }

    std::string target_hash;
    std::string header;
    if (is_hash(argument))
    {
        if (is_regular_file(argument))
        {
            fail("ambiguous argument: '" + argument + "' is both a hash and an existing file; use ./" + argument + " for the file");
        }
        target_hash = argument;
    }
    else if (is_regular_file(argument))
    {
        target_hash = hash_file(algorithm, argument);
        header = "# " + argument + " (" + algorithm + " " + target_hash + ")";
    }
    else
    {
        fail("not a file or a SHA-256, SHA-1, or MD5 hash: " + argument);
    }

    client.require_bin();

    if (!header.empty())
    {
        std::cout << header << "\n";
    }

    return client.query(target_hash);
}
