#pragma once

// Prompts, Resources, Resource Templates, Completions and Tasks for cppmcp servers.
//
// The feature registry is public so transports and applications can reuse the same
// prompt, resource, completion, pagination, and task behavior.
//
// Wire shapes follow MCP 2025-11-25 (prompts/*, resources/*, completion/complete,
// tasks/* and task-augmented tools/call).

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/uri_template.hpp"
#include "cppmcp/server.hpp"

namespace cppmcp::examples {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

struct CacheHint {
    std::int64_t ttl_ms = 60'000;
    std::string scope = "public";
};

struct PromptArgument {
    std::string name;
    std::string description;
    bool required = false;
    std::string title = {};
    std::shared_ptr<const class ICompletionProvider> completion_provider = {};
};

struct PromptDefinition {
    std::string name;
    std::string title;
    std::string description;
    std::vector<PromptArgument> arguments;
    Json icons = Json::array();
    // Returns {"description": "...", "messages": [...]}.
    std::function<Json(const Json& arguments)> render;
    std::function<Json(const Json& arguments, ToolContext& context)> context_render = {};
    std::function<std::variant<PromptResult, InputRequired>(const ToolContext&, const Json&)>
        typed_handler = {};
};

struct ResourceContent {
    std::optional<std::string> text;
    std::optional<std::string> blob_base64;
    std::optional<std::string> mime_type;  // overrides the definition's mime type
};

struct ResourceDefinition {
    std::string uri;
    std::string name;
    std::string title;
    std::string description;
    std::string mime_type;
    std::function<ResourceContent()> read = {};
    std::function<Json(const std::string& uri, ToolContext& context)> context_read = {};
    std::optional<std::size_t> size = std::nullopt;
    Json icons = Json::array();
    Json annotations = Json::object();
    Json meta = Json::object();
    std::optional<CacheHint> cache_hint = std::nullopt;
    std::function<std::variant<ReadResult, InputRequired>(ToolContext&)> typed_read = {};
};

struct ResourceTemplateDefinition {
    std::string uri_template;  // e.g. "demo://cities/{city}"
    std::string name;
    std::string title;
    std::string description;
    std::string mime_type;
    // Return std::nullopt when the variables do not identify an existing resource.
    std::function<std::optional<ResourceContent>(const core::UriTemplate::Variables&)> read;
    std::optional<std::size_t> size = std::nullopt;
    Json icons = Json::array();
    Json annotations = Json::object();
    Json meta = Json::object();
    std::optional<CacheHint> cache_hint = std::nullopt;
    std::function<std::variant<ReadResult, InputRequired>(
        ToolContext&, const core::UriTemplate::Variables&)>
        typed_read = {};
};

// (value typed so far, arguments already resolved by the client) -> candidate values.
using CompletionFn =
    std::function<std::vector<std::string>(const std::string& value, const Json& context)>;

class ICompletionProvider {
public:
    virtual ~ICompletionProvider() = default;
    virtual std::vector<std::string> complete(std::string_view value,
                                              const Json& context) const = 0;
};

inline std::shared_ptr<const ICompletionProvider> prefix_provider(
    std::vector<std::string> values) {
    class PrefixProvider final : public ICompletionProvider {
    public:
        explicit PrefixProvider(std::vector<std::string> values) : values_(std::move(values)) {}
        std::vector<std::string> complete(std::string_view value,
                                          const Json&) const override {
            std::vector<std::string> result;
            std::copy_if(values_.begin(), values_.end(), std::back_inserter(result),
                         [value](const auto& candidate) {
                             return candidate.starts_with(value);
                         });
            return result;
        }

    private:
        std::vector<std::string> values_;
    };
    return std::make_shared<PrefixProvider>(std::move(values));
}

using CallContext = ToolContext;

struct FeatureOptions {
    std::size_t page_size = 0;
    CacheHint default_cache_hint;
    std::string resource_root;
    std::size_t max_resource_bytes = 1024 * 1024;
    std::size_t max_tasks = 32;
    std::int64_t default_task_ttl_ms = 60'000;
    std::int64_t max_task_ttl_ms = 3'600'000;
    std::int64_t task_poll_interval_ms = 200;
    std::chrono::milliseconds task_result_wait{60'000};
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace detail {

inline std::string iso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// Matches RFC 6570 level-1 templates ("scheme://x/{a}/y/{b}"). A variable matches one
// non-empty path segment.
inline std::optional<std::map<std::string, std::string>> match_uri_template(
    std::string_view tmpl, std::string_view uri) {
    try {
        const auto matched = core::UriTemplate::parse(std::string(tmpl)).match(uri);
        if (!matched) return std::nullopt;
        std::map<std::string, std::string> values;
        for (const auto& [name, value] : *matched) {
            values.emplace(name, value.is_string() ? value.get<std::string>() : value.dump());
        }
        return values;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

inline core::Message ok(const core::Request& request, Json result) {
    return core::ResultResponse{request.id, std::move(result)};
}

inline core::Message fail(const core::Request& request, std::int32_t code, std::string message,
                          std::optional<Json> data = std::nullopt) {
    return core::ErrorResponse{request.id, core::RpcError{code, std::move(message), std::move(data)}};
}

inline core::Message invalid_params(const core::Request& request, std::string message) {
    return fail(request, static_cast<std::int32_t>(core::ErrorCode::invalid_params),
                std::move(message));
}

inline core::Message internal_error(const core::Request& request, std::string message) {
    return fail(request, static_cast<std::int32_t>(core::ErrorCode::internal_error),
                std::move(message));
}

inline std::uint64_t next_cursor_namespace() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

inline std::string base64url_encode(std::string_view input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char ch : input) {
        buffer = (buffer << 8) | ch;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(buffer >> bits) & 0x3f]);
        }
    }
    if (bits != 0) output.push_back(alphabet[(buffer << (6 - bits)) & 0x3f]);
    return output;
}

inline std::optional<std::string> base64url_decode(std::string_view input) {
    if (input.empty() || input.size() > 128 || input.find('=') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : input) {
        const auto pos = std::string_view(
                             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
                             .find(ch);
        if (pos == std::string_view::npos) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    if (bits >= 6 || (bits != 0 && (buffer & ((1U << bits) - 1U)) != 0) ||
        base64url_encode(output) != input) {
        return std::nullopt;
    }
    return output;
}

inline std::uint32_t cursor_checksum(std::string_view value) {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char ch : value) {
        hash = (hash ^ ch) * 16777619U;
    }
    return hash;
}

inline std::string encode_cursor(std::uint64_t cursor_namespace, std::string_view kind,
                                 std::uint64_t sequence) {
    const auto body = "1|" + std::to_string(cursor_namespace) + "|" + std::string(kind) + "|" +
                      std::to_string(sequence);
    char checksum[9];
    std::snprintf(checksum, sizeof checksum, "%08x", cursor_checksum(body));
    return base64url_encode(body + "|" + checksum);
}

// Cursor carries its list kind and the last returned registration sequence.
inline std::optional<std::uint64_t> parse_cursor(const Json& params,
                                                 std::uint64_t cursor_namespace,
                                                 std::string_view expected_kind) {
    if (!params.is_object() || !params.contains("cursor") || params["cursor"].is_null()) return 0;
    if (!params["cursor"].is_string()) return std::nullopt;
    const auto text = params["cursor"].get<std::string>();
    const auto decoded = base64url_decode(text);
    if (!decoded) return std::nullopt;
    const auto version_separator = decoded->find('|');
    const auto namespace_separator = decoded->find('|', version_separator + 1);
    const auto kind_separator = decoded->find('|', namespace_separator + 1);
    const auto sequence_separator = decoded->find('|', kind_separator + 1);
    if (version_separator == std::string::npos || namespace_separator == std::string::npos ||
        kind_separator == std::string::npos || sequence_separator == std::string::npos ||
        decoded->find('|', sequence_separator + 1) != std::string::npos ||
        decoded->substr(0, version_separator) != "1" ||
        decoded->substr(namespace_separator + 1,
                        kind_separator - namespace_separator - 1) != expected_kind) {
        return std::nullopt;
    }
    std::uint64_t parsed_namespace = 0;
    const auto namespace_begin = decoded->data() + version_separator + 1;
    const auto namespace_end = decoded->data() + namespace_separator;
    const auto namespace_result =
        std::from_chars(namespace_begin, namespace_end, parsed_namespace);
    std::uint64_t sequence = 0;
    const auto sequence_begin = decoded->data() + kind_separator + 1;
    const auto sequence_end = decoded->data() + sequence_separator;
    const auto sequence_result = std::from_chars(sequence_begin, sequence_end, sequence);
    std::uint32_t supplied_checksum = 0;
    const auto checksum_begin = decoded->data() + sequence_separator + 1;
    const auto checksum_end = decoded->data() + decoded->size();
    const auto checksum_result =
        std::from_chars(checksum_begin, checksum_end, supplied_checksum, 16);
    const auto body = std::string_view(*decoded).substr(0, sequence_separator);
    if (namespace_result.ec != std::errc{} || namespace_result.ptr != namespace_end ||
        sequence_result.ec != std::errc{} || sequence_result.ptr != sequence_end ||
        checksum_result.ec != std::errc{} || checksum_result.ptr != checksum_end ||
        checksum_end - checksum_begin != 8 || parsed_namespace != cursor_namespace ||
        supplied_checksum != cursor_checksum(body)) {
        return std::nullopt;
    }
    return sequence;
}

inline Json page(const std::vector<Json>& items, const std::vector<std::uint64_t>& sequences,
                 std::uint64_t after_sequence, std::size_t page_size, const char* key,
                 const char* cursor_kind, std::uint64_t cursor_namespace) {
    Json result = Json::object();
    Json slice = Json::array();
    const auto begin_it = std::upper_bound(sequences.begin(), sequences.end(), after_sequence);
    const auto begin = static_cast<std::size_t>(begin_it - sequences.begin());
    const auto end = page_size == 0 ? items.size()
                                    : begin + std::min(page_size, items.size() - begin);
    for (auto i = begin; i < end; ++i) slice.push_back(items[i]);
    result[key] = std::move(slice);
    if (end < items.size()) {
        result["nextCursor"] = encode_cursor(cursor_namespace, cursor_kind, sequences[end - 1]);
    }
    return result;
}

inline bool modern_request(const Json& params) {
    if (!params.is_object()) return false;
    const auto meta = params.find("_meta");
    return meta != params.end() && meta->is_object() &&
           meta->value("io.modelcontextprotocol/protocolVersion", std::string{}) == "2026-07-28";
}

inline Json complete_result(const core::Request& request, Json result,
                            std::optional<std::int64_t> ttl_ms = std::nullopt,
                            std::string cache_scope = "public") {
    if (modern_request(request.params) &&
        result.value("resultType", std::string{}) != "input_required") {
        result["resultType"] = "complete";
        if (ttl_ms) {
            result["ttlMs"] = *ttl_ms;
            result["cacheScope"] = std::move(cache_scope);
        }
    }
    return result;
}

inline void put_if(Json& target, const char* key, const std::string& value) {
    if (!value.empty()) target[key] = value;
}

inline bool is_terminal(const std::string& status) {
    return status == "completed" || status == "failed" || status == "cancelled";
}

}  // namespace detail

class FileResourceProvider {
public:
    explicit FileResourceProvider(std::string root, std::size_t max_bytes)
        : root_(std::move(root)), max_bytes_(max_bytes) {}

    std::optional<ResourceContent> read(std::string path) const {
        if (!safe_path(path)) return std::nullopt;
#ifndef _WIN32
        const int root_fd = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (root_fd < 0) return std::nullopt;
        int current_fd = root_fd;
        std::size_t start = 0;
        int file_fd = -1;
        while (start < path.size()) {
            const auto slash = path.find('/', start);
            const bool final = slash == std::string::npos;
            const auto component = path.substr(start, final ? std::string::npos : slash - start);
            const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                              (final ? 0 : O_DIRECTORY);
            const int next_fd = ::openat(current_fd, component.c_str(), flags);
            if (current_fd != root_fd) ::close(current_fd);
            if (next_fd < 0) {
                ::close(root_fd);
                return std::nullopt;
            }
            if (final) {
                file_fd = next_fd;
                break;
            }
            current_fd = next_fd;
            start = slash + 1;
        }
        if (file_fd < 0) {
            if (current_fd != root_fd) ::close(current_fd);
            ::close(root_fd);
            return std::nullopt;
        }
        struct stat info {};
        if (::fstat(file_fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
            static_cast<std::uintmax_t>(info.st_size) > max_bytes_) {
            ::close(file_fd);
            ::close(root_fd);
            return std::nullopt;
        }
        std::string bytes;
        bytes.reserve(static_cast<std::size_t>(info.st_size));
        char buffer[8192];
        for (;;) {
            const auto count = ::read(file_fd, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR) continue;
                ::close(file_fd);
                ::close(root_fd);
                return std::nullopt;
            }
            if (count == 0) break;
            const auto amount = static_cast<std::size_t>(count);
            if (amount > max_bytes_ || bytes.size() > max_bytes_ - amount) {
                ::close(file_fd);
                ::close(root_fd);
                return std::nullopt;
            }
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
        ::close(file_fd);
        ::close(root_fd);
#else
        const auto root_path = std::filesystem::weakly_canonical(root_);
        const auto candidate = std::filesystem::weakly_canonical(root_path / path);
        auto root_text = root_path.native();
        auto candidate_text = candidate.native();
        if (candidate_text.size() <= root_text.size() ||
            candidate_text.compare(0, root_text.size(), root_text) != 0 ||
            candidate_text[root_text.size()] != std::filesystem::path::preferred_separator) {
            return std::nullopt;
        }
        std::ifstream input(candidate, std::ios::binary);
        if (!input) return std::nullopt;
        std::string bytes;
        bytes.resize(max_bytes_ + 1);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        if (count < 0 || static_cast<std::size_t>(count) > max_bytes_) return std::nullopt;
        bytes.resize(static_cast<std::size_t>(count));
#endif
        const auto mime = mime_type(path);
        if (text_like(mime) && valid_utf8(bytes)) {
            return ResourceContent{std::move(bytes), std::nullopt, mime};
        }
        return ResourceContent{std::nullopt, encode_base64(bytes), mime};
    }

private:
    static bool safe_path(const std::string& path) {
        if (path.empty() || path.size() > 4096 || path.front() == '/' ||
            path.front() == '\\' || path.find('\\') != std::string::npos ||
            path.find('\0') != std::string::npos ||
            (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
             path[1] == ':')) return false;
        for (const unsigned char ch : path) {
            if (ch < 0x20 || ch == 0x7f) return false;
        }
        for (std::size_t i = 0; i + 2 < path.size(); ++i) {
            if (path[i] != '%' || !std::isxdigit(static_cast<unsigned char>(path[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(path[i + 2]))) continue;
            const auto code = static_cast<unsigned char>(
                std::stoi(path.substr(i + 1, 2), nullptr, 16));
            if (code == '.' || code == '/' || code == '\\') return false;
        }
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto slash = path.find('/', start);
            const auto segment = path.substr(start, slash == std::string::npos
                                                        ? std::string::npos
                                                        : slash - start);
            if (segment.empty() || segment == "." || segment == ".." ||
                segment.size() > 255 || segment.front() == '.') return false;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return true;
    }

    static bool valid_utf8(std::string_view text) {
        std::size_t i = 0;
        while (i < text.size()) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c <= 0x7f) { ++i; continue; }
            std::size_t extra = c >= 0xc2 && c <= 0xdf ? 1 :
                                c >= 0xe0 && c <= 0xef ? 2 :
                                c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            if (extra == 0 || i + extra >= text.size()) return false;
            for (std::size_t j = 1; j <= extra; ++j) {
                if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) return false;
            }
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
                (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) return false;
            i += extra + 1;
        }
        return true;
    }

    static std::string encode_base64(std::string_view data) {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < data.size(); i += 3) {
            const auto a = static_cast<unsigned char>(data[i]);
            const auto b = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
            const auto c = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
            out.push_back(alphabet[a >> 2]);
            out.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
            out.push_back(i + 1 < data.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
            out.push_back(i + 2 < data.size() ? alphabet[c & 63] : '=');
        }
        return out;
    }

    static std::string mime_type(const std::string& path) {
        const auto dot = path.find_last_of('.');
        const auto ext = dot == std::string::npos ? std::string{} : detail::lower(path.substr(dot));
        static const std::map<std::string, std::string> types{
            {".txt", "text/plain"}, {".md", "text/markdown"}, {".json", "application/json"},
            {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
            {".js", "text/javascript"}, {".xml", "application/xml"},
            {".csv", "text/csv"}, {".yaml", "application/yaml"}, {".yml", "application/yaml"},
            {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
            {".gif", "image/gif"}, {".pdf", "application/pdf"}};
        const auto it = types.find(ext);
        return it == types.end() ? "application/octet-stream" : it->second;
    }

    static bool text_like(const std::string& mime) {
        return mime.starts_with("text/") || mime == "application/json" ||
               mime == "application/xml" || mime == "application/yaml" ||
               mime == "text/javascript";
    }

    std::string root_;
    std::size_t max_bytes_;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

class FeatureRegistry {
public:
    // Executes a (task-less) tools/call. Typically `make_tool_executor(server)`.
    using ToolExecutor = std::function<core::Message(const core::Request&, std::stop_token)>;
    // Pushes a server->client notification on the current connection (stdio).
    using Notifier = std::function<void(const std::string& method, const Json& params)>;
    using ListChangedNotifier = std::function<void(const std::string& resource_kind)>;

    explicit FeatureRegistry(FeatureOptions options = {})
        : options_(options), cursor_namespace_(detail::next_cursor_namespace()) {}
    ~FeatureRegistry() { shutdown(); }
    FeatureRegistry(const FeatureRegistry&) = delete;
    FeatureRegistry& operator=(const FeatureRegistry&) = delete;

    void configure(FeatureOptions options) {
        std::lock_guard lock(mutex_);
        if (!prompts_.empty() || !resources_.empty() || !templates_.empty() || !tasks_.empty()) {
            throw std::logic_error("feature options must be configured before registration");
        }
        options_ = std::move(options);
    }

    // -- registration ------------------------------------------------------
    void add_prompt(PromptDefinition prompt) {
        {
            std::lock_guard lock(mutex_);
            const auto existing = std::find_if(prompts_.begin(), prompts_.end(), [&](const auto& item) {
                return item.name == prompt.name;
            });
            if (existing == prompts_.end()) {
                prompt_sequences_[prompt.name] = next_registration_sequence_++;
                prompts_.push_back(std::move(prompt));
            } else {
                std::erase_if(prompt_completions_, [&](const auto& entry) {
                    return entry.first.first == prompt.name;
                });
                *existing = std::move(prompt);
            }
        }
        emit_list_changed("prompts");
    }
    void add_resource(ResourceDefinition resource) {
        {
            std::lock_guard lock(mutex_);
            const auto existing = std::find_if(resources_.begin(), resources_.end(),
                                               [&](const auto& item) {
                                                   return item.uri == resource.uri;
                                               });
            if (existing == resources_.end()) {
                resource_sequences_[resource.uri] = next_registration_sequence_++;
                resources_.push_back(std::move(resource));
            } else {
                *existing = std::move(resource);
            }
        }
        emit_list_changed("resources");
    }
    void add_resource_template(ResourceTemplateDefinition tmpl) {
        (void)core::UriTemplate::parse(tmpl.uri_template);
        {
            std::lock_guard lock(mutex_);
            const auto existing = std::find_if(templates_.begin(), templates_.end(),
                                               [&](const auto& item) {
                                                   return item.uri_template == tmpl.uri_template;
                                               });
            if (existing == templates_.end()) {
                template_sequences_[tmpl.uri_template] = next_registration_sequence_++;
                templates_.push_back(std::move(tmpl));
            } else {
                std::erase_if(resource_completions_, [&](const auto& entry) {
                    return entry.first.first == tmpl.uri_template;
                });
                *existing = std::move(tmpl);
            }
        }
        emit_list_changed("resources");
    }
    bool remove_prompt(std::string_view name) {
        bool removed = false;
        {
            std::lock_guard lock(mutex_);
            const auto before = prompts_.size();
            std::erase_if(prompts_, [name](const auto& item) { return item.name == name; });
            removed = prompts_.size() != before;
            if (removed) {
                prompt_sequences_.erase(std::string(name));
                std::erase_if(prompt_completions_, [name](const auto& entry) {
                    return entry.first.first == name;
                });
            }
        }
        if (removed) emit_list_changed("prompts");
        return removed;
    }
    bool remove_resource(std::string_view uri) {
        bool removed = false;
        {
            std::lock_guard lock(mutex_);
            const auto before = resources_.size();
            std::erase_if(resources_, [uri](const auto& item) { return item.uri == uri; });
            removed = resources_.size() != before;
            if (removed) resource_sequences_.erase(std::string(uri));
        }
        if (removed) emit_list_changed("resources");
        return removed;
    }
    bool remove_resource_template(std::string_view uri_template) {
        bool removed = false;
        {
            std::lock_guard lock(mutex_);
            const auto before = templates_.size();
            std::erase_if(templates_, [uri_template](const auto& item) {
                return item.uri_template == uri_template;
            });
            removed = templates_.size() != before;
            if (removed) {
                template_sequences_.erase(std::string(uri_template));
                std::erase_if(resource_completions_, [uri_template](const auto& entry) {
                    return entry.first.first == uri_template;
                });
            }
        }
        if (removed) emit_list_changed("resources");
        return removed;
    }
    void add_file_resource_template() {
        if (options_.resource_root.empty()) return;
        auto provider = std::make_shared<FileResourceProvider>(
            options_.resource_root, options_.max_resource_bytes);
        add_resource_template({
            .uri_template = "demo://files/{+path}",
            .name = "file",
            .title = "Sandboxed file",
            .description = "Read a file beneath the configured resource root.",
            .mime_type = "application/octet-stream",
            .read = [provider](const core::UriTemplate::Variables& vars) {
                return provider->read(vars.at("path").get<std::string>());
            },
        });
    }
    void add_prompt_completion(std::string prompt, std::string argument, CompletionFn fn) {
        std::lock_guard lock(mutex_);
        prompt_completions_[{std::move(prompt), std::move(argument)}] = std::move(fn);
    }
    void add_resource_completion(std::string uri_template, std::string variable, CompletionFn fn) {
        std::lock_guard lock(mutex_);
        resource_completions_[{std::move(uri_template), std::move(variable)}] = std::move(fn);
    }
    void set_tool_executor(ToolExecutor executor) {
        std::lock_guard lock(mutex_);
        executor_ = std::move(executor);
    }
    void set_notifier(Notifier notifier) {
        std::lock_guard lock(mutex_);
        notifier_ = std::move(notifier);
    }
    void set_list_changed_notifier(ListChangedNotifier notifier) {
        std::lock_guard lock(mutex_);
        list_changed_notifier_ = std::move(notifier);
    }

    // Call before tearing down whatever the notifier / executor refer to.
    void shutdown() {
        std::map<std::string, std::shared_ptr<TaskRecord>> tasks;
        {
            std::lock_guard lock(mutex_);
            notifier_ = nullptr;
            tasks.swap(tasks_);
        }
        tasks.clear();  // ~TaskRecord requests stop and joins the worker
    }

    // -- capability advertisement -----------------------------------------
    // Adds prompts/resources/completions/tasks to a discover/initialize result when it
    // looks like one (has "capabilities" and "serverInfo"). Existing entries are kept.
    void augment_discover(Json& result) const {
        if (!result.is_object() || !result.contains("capabilities") ||
            !result["capabilities"].is_object() || !result.contains("serverInfo")) {
            return;
        }
        auto& caps = result["capabilities"];
        std::lock_guard lock(mutex_);
        if (!prompts_.empty() && !caps.contains("prompts")) caps["prompts"] = {{"listChanged", true}};
        if ((!resources_.empty() || !templates_.empty()) && !caps.contains("resources")) {
            caps["resources"] = {{"subscribe", true}, {"listChanged", true}};
        }
        if (!caps.contains("completions")) caps["completions"] = Json::object();
        if (!caps.contains("tasks")) {
            caps["tasks"] = {{"list", Json::object()},
                             {"cancel", Json::object()},
                             {"requests", {{"tools", {{"call", Json::object()}}}}}};
        }
    }

    // -- server-side change notifications (stdio) -------------------------
    void resource_updated(const std::string& uri) {
        Notifier notifier;
        {
            std::lock_guard lock(mutex_);
            if (!subscriptions_.count(uri)) return;
            notifier = notifier_;
        }
        if (notifier) notifier("notifications/resources/updated", {{"uri", uri}});
    }

    // -- dispatch ----------------------------------------------------------
    std::optional<core::Message> try_handle(const core::Request& request,
                                            ToolContext* context = nullptr) {
        const auto& m = request.method;
        try {
            if (m == "prompts/list") return prompts_list(request);
            if (m == "prompts/get") return prompts_get(request, context);
            if (m == "resources/list") return resources_list(request);
            if (m == "resources/templates/list") return templates_list(request);
            if (m == "resources/read") return resources_read(request, context);
            if (m == "resources/subscribe") return subscribe(request, true);
            if (m == "resources/unsubscribe") return subscribe(request, false);
            if (m == "completion/complete") return complete(request);
            if (m == "tasks/get") return tasks_get(request);
            if (m == "tasks/result") return tasks_result(request);
            if (m == "tasks/list") return tasks_list(request);
            if (m == "tasks/cancel") return tasks_cancel(request);
            if (m == "tools/call" && request.params.is_object() && request.params.contains("task") &&
                request.params["task"].is_object()) {
                return create_task(request);
            }
        } catch (const std::exception& error) {
            return detail::internal_error(request, error.what());
        }
        return std::nullopt;
    }

private:
    void emit_list_changed(const std::string& kind) {
        ListChangedNotifier notifier;
        {
            std::lock_guard lock(mutex_);
            notifier = list_changed_notifier_;
        }
        if (notifier) notifier(kind);
    }

    struct TaskRecord {
        std::string id;
        std::uint64_t registration_sequence = 0;
        std::string status = "working";
        std::string status_message;
        std::chrono::system_clock::time_point created;
        std::chrono::system_clock::time_point updated;
        std::int64_t ttl_ms = 0;
        std::int64_t poll_interval_ms = 200;
        std::stop_source stop;
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<core::Message> outcome;
        std::jthread worker;  // declared last: joined first on destruction

        ~TaskRecord() { stop.request_stop(); }

        Json to_json() const {  // caller holds `mutex`
            Json task = {{"taskId", id},
                         {"status", status},
                         {"createdAt", detail::iso8601(created)},
                         {"lastUpdatedAt", detail::iso8601(updated)},
                         {"ttl", ttl_ms},
                         {"pollInterval", poll_interval_ms}};
            detail::put_if(task, "statusMessage", status_message);
            return task;
        }
    };

    // ---- prompts ---------------------------------------------------------
    core::Message prompts_list(const core::Request& request) {
        const auto after_sequence = detail::parse_cursor(request.params, cursor_namespace_, "prompts");
        if (!after_sequence) return detail::invalid_params(request, "Invalid cursor");
        std::vector<Json> items;
        std::vector<std::uint64_t> sequences;
        {
            std::lock_guard lock(mutex_);
            for (const auto& prompt : prompts_) {
                sequences.push_back(prompt_sequences_.at(prompt.name));
                Json entry = {{"name", prompt.name}};
                detail::put_if(entry, "title", prompt.title);
                detail::put_if(entry, "description", prompt.description);
                if (!prompt.icons.empty()) entry["icons"] = prompt.icons;
                Json args = Json::array();
                for (const auto& a : prompt.arguments) {
                    Json arg = {{"name", a.name}, {"required", a.required}};
                    detail::put_if(arg, "title", a.title);
                    detail::put_if(arg, "description", a.description);
                    args.push_back(std::move(arg));
                }
                entry["arguments"] = std::move(args);
                items.push_back(std::move(entry));
            }
        }
        return detail::ok(request, detail::complete_result(
                                       request, detail::page(items, sequences, *after_sequence,
                                                             options_.page_size, "prompts", "prompts",
                                                             cursor_namespace_),
                                       60'000));
    }

    core::Message prompts_get(const core::Request& request, ToolContext* context = nullptr) {
        const auto name = request.params.value("name", std::string{});
        PromptDefinition prompt;
        {
            std::lock_guard lock(mutex_);
            const auto it = std::find_if(prompts_.begin(), prompts_.end(),
                                         [&](const auto& p) { return p.name == name; });
            if (it == prompts_.end()) {
                return detail::invalid_params(request, "Unknown prompt: " + name);
            }
            prompt = *it;
        }
        Json arguments = request.params.value("arguments", Json::object());
        if (!arguments.is_object()) return detail::invalid_params(request, "arguments must be an object");
        for (const auto& a : prompt.arguments) {
            if (a.required && !arguments.contains(a.name)) {
                return detail::invalid_params(request, "Missing required argument: " + a.name);
            }
        }
        Json rendered;
        if (prompt.typed_handler) {
            ToolContext fallback({}, {}, request.params);
            auto typed = prompt.typed_handler(context == nullptr ? fallback : *context, arguments);
            if (const auto* required = std::get_if<InputRequired>(&typed)) {
                rendered = required->to_json();
            } else {
                rendered = std::get<PromptResult>(std::move(typed)).payload;
            }
        } else {
            rendered = context != nullptr && prompt.context_render
                           ? prompt.context_render(arguments, *context)
                           : prompt.render ? prompt.render(arguments) : Json::object();
        }
        if (!rendered.contains("messages")) rendered["messages"] = Json::array();
        if (!rendered.contains("description") && !prompt.description.empty()) {
            rendered["description"] = prompt.description;
        }
        return detail::ok(request, detail::complete_result(request, std::move(rendered)));
    }

    // ---- resources -------------------------------------------------------
    static Json content_json(const std::string& uri, const std::string& default_mime,
                             const ResourceContent& content) {
        Json out = {{"uri", uri}};
        const auto mime = content.mime_type ? *content.mime_type : default_mime;
        detail::put_if(out, "mimeType", mime);
        if (content.blob_base64) out["blob"] = *content.blob_base64;
        else out["text"] = content.text.value_or("");
        return out;
    }

    core::Message resources_list(const core::Request& request) {
        const auto after_sequence = detail::parse_cursor(request.params, cursor_namespace_, "resources");
        if (!after_sequence) return detail::invalid_params(request, "Invalid cursor");
        std::vector<Json> items;
        std::vector<std::uint64_t> sequences;
        {
            std::lock_guard lock(mutex_);
            for (const auto& r : resources_) {
                sequences.push_back(resource_sequences_.at(r.uri));
                Json entry = {{"uri", r.uri}, {"name", r.name}};
                detail::put_if(entry, "title", r.title);
                detail::put_if(entry, "description", r.description);
                detail::put_if(entry, "mimeType", r.mime_type);
                if (r.size) entry["size"] = *r.size;
                if (!r.icons.empty()) entry["icons"] = r.icons;
                if (!r.annotations.empty()) entry["annotations"] = r.annotations;
                if (!r.meta.empty()) entry["_meta"] = r.meta;
                items.push_back(std::move(entry));
            }
        }
        return detail::ok(request, detail::complete_result(
                                       request, detail::page(items, sequences, *after_sequence,
                                                             options_.page_size, "resources", "resources",
                                                             cursor_namespace_),
                                       60'000));
    }

    core::Message templates_list(const core::Request& request) {
        const auto after_sequence = detail::parse_cursor(request.params, cursor_namespace_, "templates");
        if (!after_sequence) return detail::invalid_params(request, "Invalid cursor");
        std::vector<Json> items;
        std::vector<std::uint64_t> sequences;
        {
            std::lock_guard lock(mutex_);
            for (const auto& t : templates_) {
                sequences.push_back(template_sequences_.at(t.uri_template));
                Json entry = {{"uriTemplate", t.uri_template}, {"name", t.name}};
                detail::put_if(entry, "title", t.title);
                detail::put_if(entry, "description", t.description);
                detail::put_if(entry, "mimeType", t.mime_type);
                if (t.size) entry["size"] = *t.size;
                if (!t.icons.empty()) entry["icons"] = t.icons;
                if (!t.annotations.empty()) entry["annotations"] = t.annotations;
                if (!t.meta.empty()) entry["_meta"] = t.meta;
                items.push_back(std::move(entry));
            }
        }
        return detail::ok(request,
                          detail::complete_result(
                              request, detail::page(items, sequences, *after_sequence,
                                                    options_.page_size, "resourceTemplates", "templates",
                                                    cursor_namespace_),
                              60'000));
    }

    core::Message resources_read(const core::Request& request, ToolContext* context = nullptr) {
        const auto uri = request.params.value("uri", std::string{});
        if (uri.empty()) return detail::invalid_params(request, "uri is required");
        std::function<ResourceContent()> reader;
        std::function<Json(const std::string&, ToolContext&)> context_reader;
        std::string mime;
        std::function<std::optional<ResourceContent>()> template_reader;
        std::function<std::variant<ReadResult, InputRequired>(ToolContext&)> typed_reader;
        std::function<std::variant<ReadResult, InputRequired>(
            ToolContext&, const core::UriTemplate::Variables&)> typed_template_reader;
        std::optional<CacheHint> cache_hint;
        core::UriTemplate::Variables template_variables;
        bool matched_static = false;
        {
            std::lock_guard lock(mutex_);
            for (const auto& r : resources_) {
                if (r.uri == uri) {
                    matched_static = true;
                    reader = r.read;
                    context_reader = r.context_read;
                    typed_reader = r.typed_read;
                    cache_hint = r.cache_hint;
                    mime = r.mime_type;
                    break;
                }
            }
            if (!matched_static) {
                for (const auto& t : templates_) {
                    if (auto vars = core::UriTemplate::parse(t.uri_template).match(uri)) {
                        mime = t.mime_type;
                        cache_hint = t.cache_hint;
                        typed_template_reader = t.typed_read;
                        template_variables = *vars;
                        template_reader = [fn = t.read, v = *vars] { return fn(v); };
                        break;
                    }
                }
            }
        }
        if (typed_reader || typed_template_reader) {
            ToolContext fallback({}, {}, request.params);
            auto typed = typed_reader
                             ? typed_reader(context == nullptr ? fallback : *context)
                             : typed_template_reader(context == nullptr ? fallback : *context,
                                                     template_variables);
            if (const auto* required = std::get_if<InputRequired>(&typed)) {
                return detail::ok(request, required->to_json());
            }
            auto payload = std::get<ReadResult>(std::move(typed)).payload;
            const auto hint = cache_hint.value_or(options_.default_cache_hint);
            return detail::ok(request, detail::complete_result(request, std::move(payload),
                                                               hint.ttl_ms, hint.scope));
        }
        std::optional<ResourceContent> content;
        if (context_reader) {
            ToolContext fallback({}, {}, request.params);
            auto result = context_reader(uri, context == nullptr ? fallback : *context);
            if (result.value("resultType", std::string{}) == "input_required") {
                return detail::ok(request, std::move(result));
            }
            return detail::ok(request, detail::complete_result(
                                           request, std::move(result),
                                           uri == "demo://counter" ? 0 : 60'000));
        }
        if (reader) content = reader();
        else if (template_reader) content = template_reader();
        if (!content) {
            return detail::invalid_params(request, "Resource not found");
        }
        const std::int64_t ttl_ms = uri == "demo://counter" ? 0 : 60'000;
        return detail::ok(request, detail::complete_result(
                                       request,
                                       {{"contents", Json::array({content_json(uri, mime, *content)})}},
                                       ttl_ms));
    }

    core::Message subscribe(const core::Request& request, bool on) {
        const auto uri = request.params.value("uri", std::string{});
        if (uri.empty()) return detail::invalid_params(request, "uri is required");
        std::lock_guard lock(mutex_);
        if (on) subscriptions_.insert(uri);
        else subscriptions_.erase(uri);
        return detail::ok(request, Json::object());
    }

    // ---- completion ------------------------------------------------------
    core::Message complete(const core::Request& request) {
        const auto& p = request.params;
        if (!p.contains("ref") || !p["ref"].is_object() || !p.contains("argument") ||
            !p["argument"].is_object()) {
            return detail::invalid_params(request, "ref and argument are required");
        }
        const auto ref_type = p["ref"].value("type", std::string{});
        const auto arg_name = p["argument"].value("name", std::string{});
        const auto value = p["argument"].value("value", std::string{});
        const Json context = p.contains("context") && p["context"].is_object()
                                 ? p["context"].value("arguments", Json::object())
                                 : Json::object();
        CompletionFn fn;
        {
            std::lock_guard lock(mutex_);
            if (ref_type == "ref/prompt") {
                const auto name = p["ref"].value("name", std::string{});
                const bool known = std::any_of(prompts_.begin(), prompts_.end(),
                                               [&](const auto& pr) { return pr.name == name; });
                if (!known) return detail::invalid_params(request, "Unknown prompt: " + name);
                if (auto it = prompt_completions_.find({name, arg_name}); it != prompt_completions_.end())
                    fn = it->second;
            } else if (ref_type == "ref/resource") {
                const auto uri = p["ref"].value("uri", std::string{});
                const bool known = std::any_of(templates_.begin(), templates_.end(), [&](const auto& t) {
                    return t.uri_template == uri;
                });
                if (!known) return detail::invalid_params(request, "Unknown resource template: " + uri);
                if (auto it = resource_completions_.find({uri, arg_name}); it != resource_completions_.end())
                    fn = it->second;
            } else {
                return detail::invalid_params(request, "Unsupported ref type: " + ref_type);
            }
        }
        if (!fn) return detail::invalid_params(request, "Unknown completion argument: " + arg_name);
        std::vector<std::string> values;
        try {
            values = fn(value, context);
        } catch (const std::exception&) {
            return detail::internal_error(request, "Completion provider failed");
        }
        constexpr std::size_t kMax = 100;  // MCP limit per response
        const auto total = values.size();
        if (values.size() > kMax) values.resize(kMax);
        return detail::ok(request, {{"completion",
                                     {{"values", values},
                                      {"total", total},
                                      {"hasMore", total > kMax}}}});
    }

    // ---- tasks -----------------------------------------------------------
    static std::string new_task_id() {
        static std::mutex m;
        static std::mt19937_64 rng{std::random_device{}()};
        std::lock_guard lock(m);
        char buffer[33];
        std::snprintf(buffer, sizeof buffer, "%016llx%016llx",
                      static_cast<unsigned long long>(rng()), static_cast<unsigned long long>(rng()));
        return buffer;
    }

    // Expired records are moved to `graveyard` instead of destroyed: ~TaskRecord joins the
    // worker, which may itself be waiting for `mutex_`. Callers declare `graveyard`
    // *before* taking the lock so it is destroyed after the lock is released.
    void purge_expired_locked(std::vector<std::shared_ptr<TaskRecord>>& graveyard) {
        const auto now = std::chrono::system_clock::now();
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            bool drop = false;
            {
                std::lock_guard task_lock(it->second->mutex);
                drop = detail::is_terminal(it->second->status) &&
                       now > it->second->created + std::chrono::milliseconds(it->second->ttl_ms);
            }
            if (drop) {
                graveyard.push_back(std::move(it->second));
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::shared_ptr<TaskRecord> find_task(const std::string& id) {
        std::vector<std::shared_ptr<TaskRecord>> graveyard;
        std::lock_guard lock(mutex_);
        purge_expired_locked(graveyard);
        const auto it = tasks_.find(id);
        return it == tasks_.end() ? nullptr : it->second;
    }

    void emit_status(const Json& task) {
        Notifier notifier;
        {
            std::lock_guard lock(mutex_);
            notifier = notifier_;
        }
        if (notifier) notifier("notifications/tasks/status", task);
    }

    core::Message create_task(const core::Request& request) {
        const auto& task_params = request.params["task"];
        std::int64_t ttl = options_.default_task_ttl_ms;
        if (task_params.contains("ttl")) {
            if (!task_params["ttl"].is_number_integer() || task_params["ttl"].get<std::int64_t>() < 0) {
                return detail::invalid_params(request, "task.ttl must be a non-negative integer");
            }
            ttl = std::min(task_params["ttl"].get<std::int64_t>(), options_.max_task_ttl_ms);
        }

        ToolExecutor executor;
        std::shared_ptr<TaskRecord> record;
        std::vector<std::shared_ptr<TaskRecord>> graveyard;
        {
            std::lock_guard lock(mutex_);
            if (!executor_) return detail::internal_error(request, "Task execution is not configured");
            executor = executor_;
            purge_expired_locked(graveyard);
            std::size_t active = 0;
            for (auto& [id, t] : tasks_) {
                std::lock_guard task_lock(t->mutex);
                if (!detail::is_terminal(t->status)) ++active;
            }
            if (active >= options_.max_tasks) {
                return detail::internal_error(request, "Too many active tasks");
            }
            record = std::make_shared<TaskRecord>();
            record->id = new_task_id();
            record->registration_sequence = next_registration_sequence_++;
            record->created = record->updated = std::chrono::system_clock::now();
            record->ttl_ms = ttl;
            record->poll_interval_ms = options_.task_poll_interval_ms;
            tasks_[record->id] = record;
        }

        Json tool_params = request.params;
        tool_params.erase("task");
        core::Request tool_request{request.id, "tools/call", std::move(tool_params)};
        Json created;
        {
            std::lock_guard task_lock(record->mutex);
            created = record->to_json();
        }
        // The worker only touches `*record`, which outlives it (see ~TaskRecord).
        record->worker = std::jthread(
            [this, rec = record.get(), executor = std::move(executor),
             tool_request = std::move(tool_request), token = record->stop.get_token()] {
                run_task(*rec, executor, tool_request, token);
            });
        return detail::ok(request, {{"task", std::move(created)}});
    }

    void run_task(TaskRecord& record, const ToolExecutor& executor, const core::Request& request,
                  std::stop_token token) {
        core::Message outcome;
        try {
            outcome = executor(request, token);
        } catch (const std::exception& error) {
            outcome = detail::internal_error(request, error.what());
        }
        Json final_task;
        {
            std::lock_guard lock(record.mutex);
            if (record.status == "working") {
                if (const auto* error = std::get_if<core::ErrorResponse>(&outcome)) {
                    record.status = "failed";
                    record.status_message = error->error.message;
                } else if (const auto* result = std::get_if<core::ResultResponse>(&outcome);
                           result && result->result.is_object() &&
                           result->result.value("isError", false)) {
                    record.status = "failed";
                    record.status_message = "Tool reported an error";
                } else {
                    record.status = "completed";
                }
                record.outcome = std::move(outcome);
                record.updated = std::chrono::system_clock::now();
                final_task = record.to_json();
            }
        }
        record.cv.notify_all();
        if (!final_task.is_null()) emit_status(final_task);
    }

    core::Message tasks_get(const core::Request& request) {
        const auto task = find_task(request.params.value("taskId", std::string{}));
        if (!task) return detail::invalid_params(request, "Task not found");
        std::lock_guard lock(task->mutex);
        return detail::ok(request, task->to_json());
    }

    core::Message tasks_list(const core::Request& request) {
        const auto after_sequence = detail::parse_cursor(request.params, cursor_namespace_, "tasks");
        if (!after_sequence) return detail::invalid_params(request, "Invalid cursor");
        std::vector<std::shared_ptr<TaskRecord>> records;
        std::vector<std::shared_ptr<TaskRecord>> graveyard;
        {
            std::lock_guard lock(mutex_);
            purge_expired_locked(graveyard);
            for (auto& [id, t] : tasks_) records.push_back(t);
        }
        std::sort(records.begin(), records.end(),
                  [](const auto& a, const auto& b) {
                      return a->registration_sequence < b->registration_sequence;
                  });
        std::vector<Json> items;
        std::vector<std::uint64_t> sequences;
        for (const auto& t : records) {
            std::lock_guard lock(t->mutex);
            items.push_back(t->to_json());
            sequences.push_back(t->registration_sequence);
        }
        return detail::ok(request, detail::page(items, sequences, *after_sequence,
                                                options_.page_size, "tasks", "tasks",
                                                cursor_namespace_));
    }

    core::Message tasks_cancel(const core::Request& request) {
        const auto task = find_task(request.params.value("taskId", std::string{}));
        if (!task) return detail::invalid_params(request, "Task not found");
        Json snapshot;
        {
            std::lock_guard lock(task->mutex);
            if (detail::is_terminal(task->status)) {
                return detail::invalid_params(
                    request, "Cannot cancel task: already in terminal status '" + task->status + "'");
            }
            task->status = "cancelled";
            task->status_message = "Cancelled by client request";
            task->updated = std::chrono::system_clock::now();
            snapshot = task->to_json();
        }
        task->stop.request_stop();
        task->cv.notify_all();
        emit_status(snapshot);
        return detail::ok(request, std::move(snapshot));
    }

    core::Message tasks_result(const core::Request& request) {
        const auto task = find_task(request.params.value("taskId", std::string{}));
        if (!task) return detail::invalid_params(request, "Task not found");
        std::unique_lock lock(task->mutex);
        if (!task->cv.wait_for(lock, options_.task_result_wait,
                               [&] { return detail::is_terminal(task->status); })) {
            return detail::internal_error(request, "Timed out waiting for task to finish");
        }
        if (task->status == "cancelled" || !task->outcome) {
            return detail::fail(request, static_cast<std::int32_t>(core::ErrorCode::invalid_request),
                                "Task was cancelled");
        }
        const Json related = {{"io.modelcontextprotocol/related-task", {{"taskId", task->id}}}};
        if (const auto* result = std::get_if<core::ResultResponse>(&*task->outcome)) {
            Json payload = result->result;
            if (payload.is_object()) {
                auto& meta = payload["_meta"];
                if (!meta.is_object()) meta = Json::object();
                meta.update(related);
            }
            return detail::ok(request, std::move(payload));
        }
        const auto& error = std::get<core::ErrorResponse>(*task->outcome).error;
        return core::ErrorResponse{request.id, error};
    }

    FeatureOptions options_;
    const std::uint64_t cursor_namespace_;
    mutable std::mutex mutex_;
    std::uint64_t next_registration_sequence_ = 1;
    std::vector<PromptDefinition> prompts_;
    std::vector<ResourceDefinition> resources_;
    std::vector<ResourceTemplateDefinition> templates_;
    std::map<std::string, std::uint64_t> prompt_sequences_;
    std::map<std::string, std::uint64_t> resource_sequences_;
    std::map<std::string, std::uint64_t> template_sequences_;
    std::map<std::pair<std::string, std::string>, CompletionFn> prompt_completions_;
    std::map<std::pair<std::string, std::string>, CompletionFn> resource_completions_;
    std::set<std::string> subscriptions_;
    std::map<std::string, std::shared_ptr<TaskRecord>> tasks_;
    ToolExecutor executor_;
    Notifier notifier_;
    ListChangedNotifier list_changed_notifier_;
};

class PromptBuilder {
public:
    PromptBuilder(FeatureRegistry& registry, std::string name) : registry_(&registry) {
        prompt_.name = std::move(name);
    }

    PromptBuilder& title(std::string value) {
        prompt_.title = std::move(value);
        return *this;
    }
    PromptBuilder& description(std::string value) {
        prompt_.description = std::move(value);
        return *this;
    }
    PromptBuilder& icons(Json value) {
        prompt_.icons = std::move(value);
        return *this;
    }
    PromptBuilder& argument(std::string name, std::string title, std::string description,
                            bool required,
                            std::shared_ptr<const ICompletionProvider> completion = {}) {
        prompt_.arguments.push_back({std::move(name), std::move(description), required,
                                     std::move(title), std::move(completion)});
        return *this;
    }
    PromptBuilder& handler(
        std::function<std::variant<PromptResult, InputRequired>(
            const CallContext&, const Json&)> callback) {
        if (!callback) throw std::invalid_argument("prompt handler must not be empty");
        prompt_.typed_handler = [callback = std::move(callback)](
                                    const ToolContext& context, const Json& arguments) {
            return std::variant<PromptResult, InputRequired>(callback(context, arguments));
        };
        const auto prompt_name = prompt_.name;
        const auto arguments = prompt_.arguments;
        registry_->add_prompt(std::move(prompt_));
        for (const auto& argument : arguments) {
            if (argument.completion_provider) {
                const auto provider = argument.completion_provider;
                registry_->add_prompt_completion(
                    prompt_name, argument.name,
                    [provider](const std::string& value, const Json& context) {
                        return provider->complete(value, context);
                    });
            }
        }
        return *this;
    }

private:
    FeatureRegistry* registry_;
    PromptDefinition prompt_;
};

class ResourceBuilder {
public:
    ResourceBuilder(FeatureRegistry& registry, std::string uri) : registry_(&registry) {
        resource_.uri = std::move(uri);
    }

    ResourceBuilder& name(std::string value) {
        resource_.name = std::move(value);
        return *this;
    }
    ResourceBuilder& title(std::string value) {
        resource_.title = std::move(value);
        return *this;
    }
    ResourceBuilder& description(std::string value) {
        resource_.description = std::move(value);
        return *this;
    }
    ResourceBuilder& mime_type(std::string value) {
        resource_.mime_type = std::move(value);
        return *this;
    }
    ResourceBuilder& size(std::size_t value) {
        resource_.size = value;
        return *this;
    }
    ResourceBuilder& icons(Json value) {
        resource_.icons = std::move(value);
        return *this;
    }
    ResourceBuilder& annotations(Json value) {
        validate_annotations(value);
        resource_.annotations = std::move(value);
        return *this;
    }
    ResourceBuilder& meta(Json value) {
        if (!value.is_object()) throw std::invalid_argument("resource _meta must be an object");
        resource_.meta = std::move(value);
        return *this;
    }
    ResourceBuilder& cache_hint(CacheHint value) {
        validate_cache_hint(value);
        resource_.cache_hint = std::move(value);
        return *this;
    }
    ResourceBuilder& handler(
        std::function<std::variant<ReadResult, InputRequired>(const CallContext&)> callback) {
        if (!callback) throw std::invalid_argument("resource handler must not be empty");
        resource_.typed_read = [callback = std::move(callback)](ToolContext& context) {
            return callback(context);
        };
        registry_->add_resource(std::move(resource_));
        return *this;
    }

private:
    friend class ResourceTemplateBuilder;

    static void validate_cache_hint(const CacheHint& value) {
        if (value.ttl_ms < 0 || (value.scope != "public" && value.scope != "private")) {
            throw std::invalid_argument("cache hint requires a non-negative ttl and public/private scope");
        }
    }
    static void validate_annotations(const Json& value) {
        if (!value.is_object()) throw std::invalid_argument("resource annotations must be an object");
        if (const auto audience = value.find("audience"); audience != value.end()) {
            if (!audience->is_array()) {
                throw std::invalid_argument("annotations.audience must be an array");
            }
            for (const auto& entry : *audience) {
                if (!entry.is_string() || (entry != "user" && entry != "assistant")) {
                    throw std::invalid_argument("annotations.audience contains an invalid value");
                }
            }
        }
        if (const auto priority = value.find("priority"); priority != value.end()) {
            if (!priority->is_number() || priority->get<double>() < 0.0 ||
                priority->get<double>() > 1.0) {
                throw std::invalid_argument("annotations.priority must be between 0 and 1");
            }
        }
        if (const auto modified = value.find("lastModified"); modified != value.end()) {
            if (!modified->is_string()) {
                throw std::invalid_argument("annotations.lastModified must be an ISO 8601 string");
            }
            const auto text = modified->get<std::string>();
            if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
                text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
                throw std::invalid_argument("annotations.lastModified must be an ISO 8601 UTC timestamp");
            }
        }
    }

    FeatureRegistry* registry_;
    ResourceDefinition resource_;
};

class ResourceTemplateBuilder {
public:
    ResourceTemplateBuilder(FeatureRegistry& registry, std::string uri_template)
        : registry_(&registry) {
        definition_.uri_template = std::move(uri_template);
    }

    ResourceTemplateBuilder& name(std::string value) {
        definition_.name = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& title(std::string value) {
        definition_.title = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& description(std::string value) {
        definition_.description = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& mime_type(std::string value) {
        definition_.mime_type = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& size(std::size_t value) {
        definition_.size = value;
        return *this;
    }
    ResourceTemplateBuilder& icons(Json value) {
        definition_.icons = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& annotations(Json value) {
        ResourceBuilder::validate_annotations(value);
        definition_.annotations = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& meta(Json value) {
        if (!value.is_object()) throw std::invalid_argument("resource-template _meta must be an object");
        definition_.meta = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& cache_hint(CacheHint value) {
        ResourceBuilder::validate_cache_hint(value);
        definition_.cache_hint = std::move(value);
        return *this;
    }
    ResourceTemplateBuilder& completion(
        std::string variable, std::shared_ptr<const ICompletionProvider> provider) {
        if (!provider) throw std::invalid_argument("completion provider must not be empty");
        completions_.emplace_back(std::move(variable), std::move(provider));
        return *this;
    }
    ResourceTemplateBuilder& handler(
        std::function<std::variant<ReadResult, InputRequired>(
            const CallContext&, const core::UriTemplate::Variables&)> callback) {
        if (!callback) throw std::invalid_argument("resource-template handler must not be empty");
        definition_.typed_read =
            [callback = std::move(callback)](ToolContext& context,
                                             const core::UriTemplate::Variables& variables) {
                return callback(context, variables);
            };
        const auto uri_template = definition_.uri_template;
        registry_->add_resource_template(std::move(definition_));
        for (const auto& [variable, provider] : completions_) {
            registry_->add_resource_completion(
                uri_template, variable,
                [provider](const std::string& value, const Json& context) {
                    return provider->complete(value, context);
                });
        }
        return *this;
    }

private:
    FeatureRegistry* registry_;
    ResourceTemplateDefinition definition_;
    std::vector<std::pair<std::string, std::shared_ptr<const ICompletionProvider>>> completions_;
};

// Runs tools/call through the real Server. Used for task-augmented calls.
inline FeatureRegistry::ToolExecutor make_tool_executor(Server& server) {
    return [&server](const core::Request& request, std::stop_token token) -> core::Message {
        ToolContext context(token, [](Json) {}, request.params);
        auto response = server.handle_message(request, &context);
        context.finish();
        return response;
    };
}

}  // namespace cppmcp::examples
