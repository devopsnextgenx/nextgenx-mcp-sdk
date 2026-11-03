#include "cppmcp/transport/http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>

#include "cppmcp/core/version.hpp"

namespace cppmcp::transport {
namespace {

using Json = nlohmann::json;
using core::ErrorCode;
using core::Message;
using core::Notification;
using core::Request;
using core::RpcError;

struct LegacySession {
    std::string id;
    std::string protocol_version = "2025-03-26";
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::now();
    bool active = true;
    bool principal_bound = false;
    std::string issuer;
    std::string subject;
    std::string client_id;
    Json capabilities = Json::object();
    std::map<core::RequestId, std::shared_ptr<std::promise<Json>>> pending_inputs{};
};

bool session_principal_matches(const LegacySession& session,
                               const std::optional<auth::AuthContext>& context) {
    if (!session.principal_bound) return true;
    return context && context->issuer == session.issuer &&
           context->subject == session.subject &&
           context->client_id == session.client_id;
}

std::mutex& legacy_session_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, LegacySession>& legacy_sessions() {
    static std::map<std::string, LegacySession> sessions;
    return sessions;
}

std::string lower_ascii(std::string_view value);
std::string_view trim(std::string_view value);
std::vector<std::string_view> header_values(const Headers& headers, std::string_view name);
double media_quality(std::string_view accept, std::string_view wanted);
bool method_header_has_value(const Headers& headers);

std::string generate_session_id() {
    static const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<std::size_t> dist(0, alphabet.size() - 1);
    std::string result;
    result.reserve(32);
    for (std::size_t i = 0; i < 32; ++i) {
        result.push_back(alphabet[dist(rng)]);
    }
    return result;
}

std::string legacy_protocol_error_message() {
    return "This request is not a modern MCP request. Call initialize or send _meta.io.modelcontextprotocol.protocolVersion.";
}

bool is_legacy_protocol_version(std::string_view version) {
    return version == "2025-11-25" || version == "2025-06-18" || version == "2025-03-26" ||
           version == "2024-11-05";
}

bool is_modern_protocol_version(std::string_view version) {
    return version == "2026-07-28";
}

[[maybe_unused]] bool needs_legacy_session(const HttpRequest& request) {
    for (const auto& [name, value] : request.headers) {
        if (lower_ascii(name) == "mcp-session-id") {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] bool is_modern_request_body(const Request& request) {
    const auto meta = request.params.find("_meta");
    if (meta != request.params.end() && meta->is_object()) {
        const auto version = meta->value("io.modelcontextprotocol/protocolVersion", Json{});
        return version.is_string() && !version.get<std::string>().empty();
    }
    return request.method == "server/discover";
}

[[maybe_unused]] std::string request_jsonrpc_method(const std::string& body) {
    const auto parsed = core::parse_message(body, false);
    if (const auto* message = std::get_if<Message>(&parsed)) {
        if (const auto* request = std::get_if<Request>(message)) {
            return request->method;
        }
        if (const auto* notification = std::get_if<Notification>(message)) {
            return notification->method;
        }
    }
    return {};
}

std::optional<std::string> request_session_id(const HttpRequest& request) {
    const auto values = header_values(request.headers, "Mcp-Session-Id");
    if (values.empty()) {
        return std::nullopt;
    }
    return std::string(trim(values.front()));
}

[[maybe_unused]] bool response_is_acceptable(const HttpRequest& request) {
    const auto accept = header_values(request.headers, "Accept");
    if (accept.empty()) {
        return true;
    }
    return std::any_of(accept.begin(), accept.end(), [](const auto& value) {
        return media_quality(value, "application/json") > 0.0 ||
               media_quality(value, "text/event-stream") > 0.0;
    });
}

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::string path_from_target(std::string_view target) {
    const auto query = target.find_first_of("?#");
    return std::string(target.substr(0, query));
}

std::string oauth_prm_path(std::string_view endpoint_path) {
    while (!endpoint_path.empty() && endpoint_path.front() == '/') {
        endpoint_path.remove_prefix(1);
    }
    return "/.well-known/oauth-protected-resource/" + std::string(endpoint_path);
}

std::string oauth_prm_url(std::string_view resource, std::string_view prm_path) {
    const auto scheme = resource.find("://");
    if (scheme == std::string_view::npos) return {};
    const auto authority_start = scheme + 3;
    const auto authority_end = resource.find_first_of("/?#", authority_start);
    return std::string(resource.substr(0, authority_end)) + std::string(prm_path);
}

std::vector<std::string_view> header_values(const Headers& headers, std::string_view name) {
    std::vector<std::string_view> values;
    const auto wanted = lower_ascii(name);
    for (const auto& [header_name, value] : headers) {
        if (lower_ascii(header_name) == wanted) {
            values.push_back(value);
        }
    }
    return values;
}

bool has_token_char(unsigned char ch) {
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z')) {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.find(static_cast<char>(ch)) != std::string_view::npos;
}

bool valid_header_name(std::string_view name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return has_token_char(ch);
    });
}

bool method_header_has_value(const Headers& headers) {
    const auto values = header_values(headers, "Mcp-Method");
    return !values.empty() && !values.front().empty();
}

std::string normalize_origin(std::string_view origin) {
    origin = trim(origin);
    const auto scheme_end = origin.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return {};
    }
    const auto scheme = lower_ascii(origin.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
        return {};
    }
    auto authority = origin.substr(scheme_end + 3);
    if (authority.empty() || authority.find_first_of("/?#@") != std::string_view::npos) {
        return {};
    }
    return scheme + "://" + lower_ascii(authority);
}

bool is_loopback_origin(std::string_view normalized) {
    const auto scheme_end = normalized.find("://");
    if (scheme_end == std::string_view::npos) {
        return false;
    }
    auto authority = normalized.substr(scheme_end + 3);
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.front() != '[') {
        authority = authority.substr(0, colon);
    } else if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        authority = authority.substr(0, close + 1);
    }
    return authority == "localhost" || authority == "127.0.0.1" || authority == "[::1]";
}

bool origin_allowed(const HttpRequest& request, const std::vector<std::string>& allowed_origins) {
    const auto origins = header_values(request.headers, "Origin");
    if (origins.empty()) {
        return true;
    }
    if (origins.size() != 1) {
        return false;
    }
    const auto normalized = normalize_origin(origins.front());
    if (normalized.empty()) {
        return false;
    }
    if (is_loopback_origin(normalized)) {
        return true;
    }
    return std::any_of(allowed_origins.begin(), allowed_origins.end(), [&](const auto& allowed) {
        return normalize_origin(allowed) == normalized;
    });
}

std::optional<std::string> normalize_host(std::string_view host_header) {
    auto host = trim(host_header);
    if (host.empty() || host.find_first_of(" \t\r\n/@?#") != std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view hostname;
    std::string_view port;
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        hostname = host.substr(1, close - 1);
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') {
                return std::nullopt;
            }
            port = host.substr(close + 2);
        }
    } else {
        const auto colon = host.find(':');
        if (colon != std::string_view::npos) {
            if (host.find(':', colon + 1) != std::string_view::npos) {
                return std::nullopt;
            }
            hostname = host.substr(0, colon);
            port = host.substr(colon + 1);
        } else {
            hostname = host;
        }
    }
    if (hostname.empty()) {
        return std::nullopt;
    }
    if (!port.empty()) {
        unsigned parsed_port = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), parsed_port);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
            parsed_port > 65535) {
            return std::nullopt;
        }
    } else if (host.back() == ':') {
        return std::nullopt;
    }
    return lower_ascii(hostname);
}

bool host_allowed(const HttpRequest& request, const std::vector<std::string>& allowed_hosts) {
    const auto hosts = header_values(request.headers, "Host");
    if (hosts.empty()) {
        return request.peer.remote_address.empty();
    }
    if (hosts.size() != 1) {
        return false;
    }
    const auto normalized = normalize_host(hosts.front());
    if (!normalized) {
        return false;
    }
    for (const auto& allowed : allowed_hosts) {
        const auto normalized_allowed = normalize_host(allowed);
        if (normalized_allowed && *normalized_allowed == *normalized) {
            return true;
        }
    }
    if (request.peer.remote_address.empty()) {
        return true;
    }
    const auto remote = lower_ascii(request.peer.remote_address);
    if (remote == "127.0.0.1" || remote == "::1") {
        return *normalized == "localhost" || *normalized == "127.0.0.1" ||
               *normalized == "::1";
    }
    return *normalized == remote;
}

double quality(std::string_view parameter) {
    const auto equal = parameter.find('=');
    if (equal == std::string_view::npos || lower_ascii(trim(parameter.substr(0, equal))) != "q") {
        return 1.0;
    }
    const auto value = trim(parameter.substr(equal + 1));
    double parsed = 0.0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
        return 0.0;
    }
    return parsed;
}

double media_quality(std::string_view accept, std::string_view wanted) {
    int best_specificity = -1;
    double best_quality = 0.0;
    while (!accept.empty()) {
        const auto comma = accept.find(',');
        auto item = trim(accept.substr(0, comma));
        accept = comma == std::string_view::npos ? std::string_view{} : accept.substr(comma + 1);
        const auto semicolon = item.find(';');
        const auto media_type = lower_ascii(trim(item.substr(0, semicolon)));
        int specificity = -1;
        if (media_type == wanted) {
            specificity = 2;
        } else if ((media_type == "application/*" && wanted == "application/json") ||
                   (media_type == "text/*" && wanted == "text/event-stream")) {
            specificity = 1;
        } else if (media_type == "*/*") {
            specificity = 0;
        }
        if (specificity >= 0) {
            double item_quality = 1.0;
            if (semicolon != std::string_view::npos) {
                auto parameters = item.substr(semicolon + 1);
                while (!parameters.empty()) {
                    const auto next = parameters.find(';');
                    const auto parameter = trim(parameters.substr(0, next));
                    if (lower_ascii(trim(parameter.substr(0, parameter.find('=')))) == "q") {
                        item_quality = quality(parameter);
                    }
                    if (next == std::string_view::npos) {
                        break;
                    }
                    parameters.remove_prefix(next + 1);
                }
            }
            if (specificity > best_specificity) {
                best_specificity = specificity;
                best_quality = item_quality;
            } else if (specificity == best_specificity) {
                best_quality = std::max(best_quality, item_quality);
            }
        }
    }
    return best_quality;
}

bool accepts(const Headers& headers, std::string_view media_type) {
    const auto values = header_values(headers, "Accept");
    if (values.empty()) {
        return false;
    }
    return std::any_of(values.begin(), values.end(), [&](auto value) {
        return media_quality(value, media_type) > 0.0;
    });
}

bool content_type_is_json(const Headers& headers) {
    const auto values = header_values(headers, "Content-Type");
    if (values.size() != 1) {
        return false;
    }
    const auto value = trim(values.front());
    const auto semicolon = value.find(';');
    if (lower_ascii(trim(value.substr(0, semicolon))) != "application/json") {
        return false;
    }
    if (semicolon == std::string_view::npos) {
        return true;
    }
    auto parameters = value.substr(semicolon + 1);
    bool charset_seen = false;
    while (!parameters.empty()) {
        const auto next = parameters.find(';');
        auto parameter = trim(parameters.substr(0, next));
        const auto equal = parameter.find('=');
        if (equal == std::string_view::npos) {
            return false;
        }
        if (lower_ascii(trim(parameter.substr(0, equal))) == "charset") {
            if (charset_seen) {
                return false;
            }
            charset_seen = true;
            auto charset = lower_ascii(trim(parameter.substr(equal + 1)));
            if (charset.size() >= 2 && charset.front() == '"' && charset.back() == '"') {
                charset = charset.substr(1, charset.size() - 2);
            }
            if (charset != "utf-8") {
                return false;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        parameters.remove_prefix(next + 1);
    }
    return true;
}

std::optional<std::string> single_header(const Headers& headers, std::string_view name) {
    const auto values = header_values(headers, name);
    if (values.size() != 1) {
        return std::nullopt;
    }
    return std::string(trim(values.front()));
}

bool valid_plain_header_value(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '\t' || (ch >= 0x20 && ch <= 0x7e);
    });
}

std::string json_string(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string{};
}

RpcError mismatch(std::string field) {
    return core::protocol_error(ErrorCode::header_mismatch,
                                "HTTP header does not match the JSON-RPC request: " + field,
                                Json{{"field", std::move(field)}});
}

void respond_json(IHttpExchange& exchange, int status, const Json& body) {
    exchange.respond(status, {{"Content-Type", "application/json"}}, body.dump());
}

void respond_error(IHttpExchange& exchange, int status, RpcError error,
                   std::optional<core::RequestId> id = std::nullopt) {
    respond_json(exchange, status, core::error_response(error, std::move(id)));
}

bool supported_modern_version(std::string_view version) {
    for (const auto& candidate : core::known_protocol_versions()) {
        if (candidate.value == version && candidate.era == core::ProtocolEra::modern) {
            return version == "2026-07-28";
        }
    }
    return false;
}

bool requires_name_header(std::string_view method) {
    return method == "tools/call" || method == "resources/read" || method == "prompts/get";
}

bool validate_mirrored_headers(const Request& request, const Headers& headers, RpcError& error) {
    const auto version_header = single_header(headers, "MCP-Protocol-Version");
    const auto meta = request.params.find("_meta");
    const auto body_version = meta != request.params.end() && meta->is_object()
                                  ? json_string(meta->value("io.modelcontextprotocol/protocolVersion", Json{}))
                                  : std::string{};
    if (!version_header || !valid_plain_header_value(*version_header) || body_version.empty() ||
        *version_header != body_version) {
        error = mismatch("MCP-Protocol-Version");
        return false;
    }

    const auto method_header = single_header(headers, "Mcp-Method");
    if (!method_header || !valid_plain_header_value(*method_header) ||
        *method_header != request.method) {
        error = mismatch("Mcp-Method");
        return false;
    }

    if (requires_name_header(request.method)) {
        const auto name_header = single_header(headers, "Mcp-Name");
        const auto name = request.params.find("name");
        const auto uri = request.params.find("uri");
        const Json* source = name != request.params.end() ? &*name
                              : uri != request.params.end() ? &*uri
                                                           : nullptr;
        std::string decoded;
        if (!name_header || source == nullptr || !source->is_string() ||
            !decode_mirrored_header_value(*name_header, decoded) ||
            decoded != source->get<std::string>()) {
            error = mismatch("Mcp-Name");
            return false;
        }
    }
    return true;
}

bool validate_mirrored_params(const Server& server, const Request& request,
                              const Headers& headers, RpcError& error) {
    const auto tool_name = request.params.value("name", std::string{});
    const auto declarations = server.tool_header_params(tool_name);
    const auto arguments = request.params.find("arguments");
    std::vector<std::string> expected_headers;
    expected_headers.reserve(declarations.size());
    for (const auto& declaration : declarations) {
        const auto header_name = "Mcp-Param-" + declaration.name;
        expected_headers.push_back(lower_ascii(header_name));
        const auto values = header_values(headers, header_name);
        const Json* argument = nullptr;
        if (arguments != request.params.end() && arguments->is_object()) {
            const auto argument_it = arguments->find(declaration.name);
            if (argument_it != arguments->end()) {
                argument = &*argument_it;
            }
        }
        const bool has_argument = argument != nullptr;
        if (!has_argument) {
            if (declaration.required || !values.empty()) {
                error = mismatch(header_name);
                return false;
            }
            continue;
        }
        std::optional<std::string> expected;
        if (argument->is_string() && declaration.type == "string") {
            expected = argument->get<std::string>();
        } else if (argument->is_boolean() && declaration.type == "boolean") {
            expected = argument->get<bool>() ? "true" : "false";
        } else if (declaration.type == "integer" && argument->is_number_unsigned()) {
            constexpr std::uint64_t max_safe_integer = 9007199254740991ULL;
            const auto integer = argument->get<std::uint64_t>();
            if (integer <= max_safe_integer) {
                expected = std::to_string(integer);
            }
        } else if (declaration.type == "integer" && argument->is_number_integer()) {
            constexpr std::int64_t max_safe_integer = 9007199254740991LL;
            const auto integer = argument->get<std::int64_t>();
            if (integer >= -max_safe_integer && integer <= max_safe_integer) {
                expected = std::to_string(integer);
            }
        }
        std::string decoded;
        if (!expected || values.size() != 1 ||
            !decode_mirrored_header_value(values.front(), decoded) ||
            decoded != *expected) {
            error = mismatch(header_name);
            return false;
        }
    }
    for (const auto& [name, _] : headers) {
        const auto normalized = lower_ascii(name);
        constexpr std::string_view prefix = "mcp-param-";
        if (normalized.starts_with(prefix) &&
            std::find(expected_headers.begin(), expected_headers.end(), normalized) ==
                expected_headers.end()) {
            error = mismatch(name);
            return false;
        }
    }
    return true;
}

bool has_client_capabilities(const Request& request) {
    const auto meta = request.params.find("_meta");
    if (meta == request.params.end() || !meta->is_object()) {
        return false;
    }
    const auto capabilities = meta->find("io.modelcontextprotocol/clientCapabilities");
    return capabilities != meta->end() && capabilities->is_object();
}

bool is_supported_method(std::string_view method) {
    return method == "server/discover" || method == "tools/list" || method == "tools/call" ||
           method == "subscriptions/listen" || method == "prompts/get" ||
           method == "resources/read";
}

void send_response(IHttpExchange& exchange, const HttpHandlerOptions& options,
                   bool accept_json, bool accept_sse, const Message& message,
                   SseStream* stream = nullptr) {
    const auto json = core::serialize_message(message);
    const auto* error = std::get_if<core::ErrorResponse>(&message);
    if (stream == nullptr && error != nullptr && error->error.code ==
        static_cast<std::int32_t>(ErrorCode::missing_required_client_capability)) {
        exchange.respond(400, {{"Content-Type", "application/json"}}, json);
        return;
    }
    if (stream != nullptr) {
        stream->write_event(json);
        stream->close();
        return;
    }
    const bool use_sse = options.response_mode == HttpResponseMode::sse_only ||
                         (options.response_mode == HttpResponseMode::automatic &&
                          !accept_json && accept_sse);
    if (use_sse) {
        auto& stream = exchange.begin_sse(
            200, {{"Content-Type", "text/event-stream"},
                  {"Cache-Control", "no-cache"},
                  {"X-Accel-Buffering", "no"}});
        stream.write_event(json);
        stream.close();
        return;
    }
    exchange.respond(200, {{"Content-Type", "application/json"}}, json);
}

std::string base64_encode(std::string_view input) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto a = static_cast<unsigned char>(input[i]);
        const auto b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        const auto c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
        const unsigned triple = (static_cast<unsigned>(a) << 16) |
                                (static_cast<unsigned>(b) << 8) | static_cast<unsigned>(c);
        output.push_back(alphabet[(triple >> 18) & 63]);
        output.push_back(alphabet[(triple >> 12) & 63]);
        output.push_back(i + 1 < input.size() ? alphabet[(triple >> 6) & 63] : '=');
        output.push_back(i + 2 < input.size() ? alphabet[triple & 63] : '=');
    }
    return output;
}

bool base64_decode(std::string_view input, std::string& output) {
    if (input.size() % 4 != 0) {
        return false;
    }
    auto digit = [](unsigned char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    output.clear();
    output.reserve((input.size() / 4) * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const bool pad2 = input[i + 2] == '=';
        const bool pad3 = input[i + 3] == '=';
        if ((pad2 && !pad3) || (i + 4 != input.size() && (pad2 || pad3))) {
            return false;
        }
        const auto a = digit(static_cast<unsigned char>(input[i]));
        const auto b = digit(static_cast<unsigned char>(input[i + 1]));
        const auto c = pad2 ? 0 : digit(static_cast<unsigned char>(input[i + 2]));
        const auto d = pad3 ? 0 : digit(static_cast<unsigned char>(input[i + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad2 && (b & 15) != 0) || (pad3 && !pad2 && (c & 3) != 0)) {
            return false;
        }
        const unsigned triple = (static_cast<unsigned>(a) << 18) |
                                (static_cast<unsigned>(b) << 12) |
                                (static_cast<unsigned>(c) << 6) | static_cast<unsigned>(d);
        output.push_back(static_cast<char>((triple >> 16) & 255));
        if (!pad2) {
            output.push_back(static_cast<char>((triple >> 8) & 255));
        }
        if (!pad3) {
            output.push_back(static_cast<char>(triple & 255));
        }
    }
    return true;
}

bool header_value_is_plain_safe(std::string_view value) {
    if (value.empty()) {
        return true;
    }
    if (value.front() == ' ' || value.front() == '\t' ||
        value.back() == ' ' || value.back() == '\t') {
        return false;
    }
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte != '\t' && (byte < 0x20 || byte > 0x7e)) {
            return false;
        }
    }
    return true;
}

}  // namespace

McpHttpHandler::McpHttpHandler(Server& server, HttpHandlerOptions options,
                               std::optional<OAuthResourceOptions> oauth)
    : server_(server), options_(std::move(options)),
      oauth_(oauth ? std::move(oauth) : options_.oauth) {
    if (options_.path.empty() || options_.path.front() != '/') {
        throw std::invalid_argument("HTTP MCP endpoint path must begin with '/'");
    }
    if (oauth_ && (oauth_->resource.empty() || oauth_->authorization_servers.empty() ||
                   oauth_->resource.find('#') != std::string::npos ||
                   std::find(oauth_->scopes_supported.begin(),
                             oauth_->scopes_supported.end(), "offline_access") !=
                       oauth_->scopes_supported.end())) {
        throw std::invalid_argument("OAuth resource metadata is invalid");
    }
}

void McpHttpHandler::handle(const std::shared_ptr<IHttpExchange>& exchange) const {
    if (!exchange) {
        throw std::invalid_argument("HTTP exchange must not be null");
    }
    const auto& request = exchange->request();
    std::optional<auth::AuthContext> auth_context;
    if (oauth_) {
        const auto target_path = path_from_target(request.target);
        const auto inserted_path = oauth_prm_path(options_.path);
        if (target_path == inserted_path ||
            target_path == "/.well-known/oauth-protected-resource") {
            const Headers cors{{"Access-Control-Allow-Origin", "*"},
                               {"Access-Control-Allow-Methods", "GET, OPTIONS"},
                               {"Access-Control-Allow-Headers", "Content-Type"},
                               {"Cache-Control", "public, max-age=3600"}};
            if (request.method == "OPTIONS") {
                exchange->respond(204, cors, "");
                return;
            }
            if (request.method != "GET") {
                exchange->respond(405, {{"Allow", "GET, OPTIONS"}}, "");
                return;
            }
            Json metadata = {
                {"resource", oauth_->resource},
                {"authorization_servers", oauth_->authorization_servers},
                {"bearer_methods_supported", oauth_->bearer_methods_supported}};
            if (!oauth_->scopes_supported.empty()) {
                metadata["scopes_supported"] = oauth_->scopes_supported;
            }
            if (!oauth_->resource_name.empty()) {
                metadata["resource_name"] = oauth_->resource_name;
            }
            if (!oauth_->resource_documentation.empty()) {
                metadata["resource_documentation"] = oauth_->resource_documentation;
            }
            auto headers = cors;
            headers.emplace_back("Content-Type", "application/json");
            exchange->respond(200, std::move(headers), metadata.dump());
            return;
        }
    }
    if (path_from_target(request.target) != options_.path) {
        respond_error(*exchange, 404,
                      core::protocol_error(ErrorCode::method_not_found, "MCP endpoint not found"));
        return;
    }
    if (!origin_allowed(request, options_.allowed_origins)) {
        respond_error(*exchange, 403,
                      core::protocol_error(ErrorCode::invalid_request, "Origin is not allowed"));
        return;
    }
    if (!host_allowed(request, options_.allowed_hosts)) {
        respond_error(*exchange, 403,
                      core::protocol_error(ErrorCode::invalid_request, "Host is not allowed"));
        return;
    }
    const auto authorization_values = header_values(request.headers, "Authorization");
    const bool authentication_enabled = oauth_.has_value() ||
                                        static_cast<bool>(options_.authenticator) ||
                                        static_cast<bool>(options_.token_verifier);
    auto send_auth_challenge = [&](int status, bool invalid_token) {
        std::string challenge = "Bearer";
        if (oauth_) {
            const auto metadata_url =
                oauth_prm_url(oauth_->resource, oauth_prm_path(options_.path));
            if (!metadata_url.empty() &&
                !oauth_->omit_resource_metadata_parameter) {
                challenge += " resource_metadata=\"" + metadata_url + "\"";
            }
            if (std::find(oauth_->scopes_supported.begin(),
                          oauth_->scopes_supported.end(), "mcp:use") !=
                oauth_->scopes_supported.end()) {
                challenge += " scope=\"mcp:use\"";
            } else if (!oauth_->scopes_supported.empty()) {
                challenge += " scope=\"" + oauth_->scopes_supported.front() + "\"";
            }
        }
        if (invalid_token) challenge += " error=\"invalid_token\"";
        exchange->respond(status, {{"WWW-Authenticate", std::move(challenge)}}, "");
    };
    if (authentication_enabled && authorization_values.size() > 1) {
        exchange->respond(400, {}, "");
        return;
    }
    const auto query_start = request.target.find('?');
    bool query_token = false;
    if (query_start != std::string::npos) {
        auto query = request.target.substr(query_start + 1);
        std::transform(query.begin(), query.end(), query.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        query_token = query.find("access_token=") != std::string::npos;
    }
    if (authentication_enabled && query_token) {
        send_auth_challenge(401, false);
        return;
    }
    if (authentication_enabled) {
        std::string_view bearer_token;
        bool bearer_scheme = false;
        if (!authorization_values.empty()) {
            const auto authorization = trim(authorization_values.front());
            const auto separator = authorization.find_first_of(" \t");
            const auto scheme = authorization.substr(0, separator);
            bearer_scheme = lower_ascii(scheme) == "bearer";
            if (bearer_scheme && separator != std::string_view::npos) {
                bearer_token = trim(authorization.substr(separator + 1));
                if (bearer_token.find_first_of(" \t") != std::string_view::npos) {
                    bearer_token = {};
                }
            }
        }
        if (options_.token_verifier) {
            if (!bearer_scheme || bearer_token.empty()) {
                send_auth_challenge(401, bearer_scheme && !authorization_values.empty());
                return;
            }
            if (!oauth_) {
                send_auth_challenge(503, false);
                return;
            }
            try {
                auth_context = options_.token_verifier->verify(
                    bearer_token, oauth_->resource);
            } catch (const auth::TokenVerificationError&) {
                send_auth_challenge(401, true);
                return;
            } catch (const std::exception&) {
                send_auth_challenge(503, false);
                return;
            }
        } else if (options_.authenticator) {
            if (!options_.authenticator(request)) {
                send_auth_challenge(401, bearer_scheme && !bearer_token.empty());
                return;
            }
        } else {
            send_auth_challenge(authorization_values.empty() ? 401 : 503,
                                bearer_scheme && !bearer_token.empty());
            return;
        }
    }
    if (request.method != "POST" && request.method != "GET" && request.method != "DELETE") {
        exchange->respond(405, {{"Allow", "POST, GET, DELETE"}}, "");
        return;
    }
    if (std::any_of(request.headers.begin(), request.headers.end(), [](const auto& header) {
            return !valid_header_name(header.first) ||
                   header.second.find_first_of("\r\n") != std::string::npos;
        })) {
        exchange->respond(400, {}, "");
        return;
    }

    const auto session_id = request_session_id(request);
    const auto version_header = single_header(request.headers, "MCP-Protocol-Version");
    const auto accept_json = accepts(request.headers, "application/json");
    const auto accept_sse = accepts(request.headers, "text/event-stream");
    const auto legacy_header_present = bool(version_header) && is_legacy_protocol_version(*version_header);
    const auto modern_header_present = bool(version_header) && is_modern_protocol_version(*version_header);
    const bool accept_present = std::any_of(request.headers.begin(), request.headers.end(),
                                           [](const auto& header) {
                                               return lower_ascii(header.first) == "accept";
                                           });
    if (accept_present && !accept_json && !accept_sse) {
        exchange->respond(406, {{"Content-Type", "application/json"}}, "");
        return;
    }

    if (request.method == "GET") {
        const bool modern_get = method_header_has_value(request.headers) ||
                                (version_header && modern_header_present);
        if (options_.mode == HttpMode::modern || modern_get) {
            exchange->respond(405, {{"Allow", "POST"}}, "");
            return;
        }
        if (!session_id && !version_header && !accept_present) {
            exchange->respond(405, {{"Allow", "POST"}}, "");
            return;
        }
        if (!session_id || !version_header || !legacy_header_present) {
            respond_error(*exchange, 400,
                          core::protocol_error(ErrorCode::invalid_request,
                                               "Mcp-Session-Id and legacy MCP-Protocol-Version are required for GET"));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(legacy_session_mutex());
            const auto found = legacy_sessions().find(*session_id);
            if (found == legacy_sessions().end() || !found->second.active ||
                !session_principal_matches(found->second, auth_context)) {
                respond_error(*exchange, 404,
                              core::protocol_error(ErrorCode::invalid_request,
                                                   "Legacy session not found"));
                return;
            }
            found->second.last_seen = std::chrono::steady_clock::now();
        }
        const auto registry = server_.subscriptions_;
        const auto stream_token = registry->open_legacy_stream(*session_id);
        if (stream_token.stream_id == 0) {
            respond_error(*exchange, 404,
                          core::protocol_error(ErrorCode::invalid_request,
                                               "Legacy session not found"));
            return;
        }
        const std::weak_ptr<SubscriptionRegistry> weak_registry = registry;
        exchange->on_disconnect([weak_registry, stream_token] {
            if (const auto active_registry = weak_registry.lock()) {
                active_registry->close_legacy_stream(stream_token);
            }
        });
        auto& sse = exchange->begin_sse(200, {{"Content-Type", "text/event-stream"},
                                              {"Cache-Control", "no-cache"},
                                              {"X-Accel-Buffering", "no"}});
        sse.write_event(R"({"jsonrpc":"2.0","method":"ping","params":{}})");
        for (;;) {
            Json event;
            const auto status = registry->next_legacy_stream(
                stream_token, event, std::chrono::seconds(15));
            if (status == SubscriptionReadStatus::event) {
                sse.write_event(event.dump());
                continue;
            }
            if (status == SubscriptionReadStatus::timeout) {
                sse.write_comment("keep-alive");
                continue;
            }
            break;
        }
        registry->close_legacy_stream(stream_token);
        sse.close();
        return;
    }

    if (request.method == "DELETE") {
        if (!session_id && !version_header && !method_header_has_value(request.headers) && !accept_present) {
            exchange->respond(405, {{"Allow", "POST"}}, "");
            return;
        }
        if (!session_id) {
            respond_error(*exchange, 400,
                          core::protocol_error(ErrorCode::invalid_request, "Mcp-Session-Id is required for DELETE"));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(legacy_session_mutex());
            const auto found = legacy_sessions().find(*session_id);
            if (found == legacy_sessions().end() ||
                !session_principal_matches(found->second, auth_context)) {
                respond_error(*exchange, 404,
                              core::protocol_error(ErrorCode::invalid_request,
                                                   "Legacy session not found"));
                return;
            }
            legacy_sessions().erase(found);
        }
        server_.subscriptions_->remove_legacy_session(*session_id);
        exchange->respond(200, {{"Content-Type", "application/json"}}, "{}\n");
        return;
    }

    if (!content_type_is_json(request.headers)) {
        exchange->respond(415, {}, "");
        return;
    }

    const auto parsed = core::parse_message(request.body);
    if (const auto* error = std::get_if<RpcError>(&parsed)) {
        respond_error(*exchange, 400, *error);
        return;
    }
    const auto& message = std::get<Message>(parsed);

    const auto* input_result = std::get_if<core::ResultResponse>(&message);
    const auto* input_error = std::get_if<core::ErrorResponse>(&message);
    if (input_result != nullptr || input_error != nullptr) {
        const auto id = input_result != nullptr ? std::optional<core::RequestId>(input_result->id)
                                                 : input_error->id;
        std::shared_ptr<std::promise<Json>> pending;
        if (session_id && legacy_header_present && options_.mode != HttpMode::modern && id) {
            std::lock_guard<std::mutex> lock(legacy_session_mutex());
            const auto session = legacy_sessions().find(*session_id);
            if (session != legacy_sessions().end()) {
                const auto found = session->second.pending_inputs.find(*id);
                if (found != session->second.pending_inputs.end()) {
                    pending = found->second;
                    session->second.pending_inputs.erase(found);
                }
            }
        }
        if (!pending) {
            respond_error(*exchange, 400, core::protocol_error(ErrorCode::invalid_request,
                "No matching legacy input request"));
            return;
        }
        if (input_result != nullptr) pending->set_value(input_result->result);
        else pending->set_exception(std::make_exception_ptr(std::runtime_error("Client rejected input request")));
        exchange->respond(202, {}, "");
        return;
    }

    if (std::holds_alternative<core::Notification>(message)) {
        const auto& notification = std::get<core::Notification>(message);
        if (notification.method == "notifications/initialized") {
            exchange->respond(202, {}, "");
            return;
        }
        if (session_id && version_header && legacy_header_present) {
            exchange->respond(202, {}, "");
            return;
        }
        respond_error(*exchange, 400,
                      core::protocol_error(ErrorCode::invalid_request,
                                           "Client notifications are not defined for this protocol version"));
        return;
    }
    if (!std::holds_alternative<Request>(message)) {
        respond_error(*exchange, 400,
                      core::protocol_error(ErrorCode::invalid_request,
                                           "The HTTP request body must contain a JSON-RPC request"));
        return;
    }
    const auto& rpc_request = std::get<Request>(message);
    RpcError header_error{};

    const bool legacy_request = session_id.has_value() || rpc_request.method == "initialize" ||
                                (version_header && legacy_header_present) ||
                                (request.body.find("initialize") != std::string::npos && !modern_header_present);
    const bool modern_request = method_header_has_value(request.headers) ||
                                (version_header && modern_header_present) ||
                                (rpc_request.params.is_object() &&
                                 rpc_request.params.contains("_meta") &&
                                 rpc_request.params["_meta"].contains("io.modelcontextprotocol/protocolVersion"));

    if (accept_present && !accept_json && !accept_sse) {
        exchange->respond(406, {{"Content-Type", "application/json"}}, "");
        return;
    }

    if (options_.mode == HttpMode::modern && !modern_request) {
        respond_error(*exchange, 400,
                      core::protocol_error(ErrorCode::invalid_request,
                                           legacy_protocol_error_message()),
                      rpc_request.id);
        return;
    }

    if (legacy_request && !modern_request) {
        if (rpc_request.method == "initialize") {
            const std::string sid = generate_session_id();
            Json result = { {"protocolVersion", "2025-11-25"},
                            {"capabilities", {{"tools", {{"listChanged", true}}},
                                               {"logging", Json::object()}}},
                            {"serverInfo", {{"name", "cppmcp"}, {"title", "cppmcp"}, {"version", "0.1.0"}}} };
            {
                std::lock_guard<std::mutex> lock(legacy_session_mutex());
                LegacySession created_session;
                created_session.id = sid;
                created_session.protocol_version = "2025-11-25";
                created_session.capabilities = rpc_request.params.value(
                    "capabilities", Json::object());
                if (auth_context) {
                    created_session.principal_bound = true;
                    created_session.issuer = auth_context->issuer;
                    created_session.subject = auth_context->subject;
                    created_session.client_id = auth_context->client_id;
                }
                legacy_sessions().insert_or_assign(sid, std::move(created_session));
            }
            bool prompts_list_changed = false;
            bool resources_list_changed = false;
            {
                std::lock_guard<std::mutex> server_lock(server_.mutex_);
                prompts_list_changed = server_.request_handlers_.contains("prompts/get");
                resources_list_changed = server_.request_handlers_.contains("resources/read");
            }
            server_.subscriptions_->register_legacy_session(
                sid, true, prompts_list_changed, resources_list_changed);
            const auto response = core::ResultResponse{rpc_request.id, std::move(result)};
            exchange->respond(200, {{"Mcp-Session-Id", sid}, {"Content-Type", "application/json"}},
                             core::serialize_message(response));
            return;
        }
        if (!session_id) {
            respond_error(*exchange, 400,
                          core::protocol_error(ErrorCode::invalid_request, "Missing Mcp-Session-Id"));
            return;
        }
        Json legacy_capabilities = Json::object();
        {
            std::lock_guard<std::mutex> lock(legacy_session_mutex());
            const auto it = legacy_sessions().find(*session_id);
            if (it == legacy_sessions().end() ||
                !session_principal_matches(it->second, auth_context)) {
                respond_error(*exchange, 404,
                              core::protocol_error(ErrorCode::invalid_request,
                                                   "Unknown session"), rpc_request.id);
                return;
            }
            it->second.last_seen = std::chrono::steady_clock::now();
            legacy_capabilities = it->second.capabilities;
        }
        if (version_header && *version_header == "1999-01-01") {
            respond_error(*exchange, 400,
                          core::protocol_error(ErrorCode::unsupported_protocol_version,
                                               "Unsupported protocol version"));
            return;
        }
        if (rpc_request.method == "resources/subscribe" ||
            rpc_request.method == "resources/unsubscribe") {
            const auto uri = rpc_request.params.is_object()
                                 ? rpc_request.params.value("uri", std::string{})
                                 : std::string{};
            if (uri.empty()) {
                respond_error(*exchange, 200,
                              core::protocol_error(ErrorCode::invalid_params,
                                                   "uri is required"),
                              rpc_request.id);
                return;
            }
            try {
                const bool on = rpc_request.method == "resources/subscribe";
                if (!server_.subscriptions_->subscribe_legacy_resource(*session_id, uri, on)) {
                    respond_error(*exchange, 404,
                                  core::protocol_error(ErrorCode::invalid_request,
                                                       "Unknown legacy session"),
                                  rpc_request.id);
                    return;
                }
            } catch (const SubscriptionLimitError& error) {
                respond_error(*exchange, 429,
                              core::protocol_error(ErrorCode::invalid_request, error.what()),
                              rpc_request.id);
                return;
            }
            const core::ResultResponse result{rpc_request.id, Json::object()};
            exchange->respond(200, {{"Content-Type", "application/json"}},
                              core::serialize_message(result));
            return;
        }
        auto cancellation = std::make_shared<std::stop_source>();
        exchange->on_disconnect([cancellation] { cancellation->request_stop(); });
        SseStream* stream = nullptr;
        const auto start_sse = [&]() -> SseStream& {
            if (stream == nullptr) {
                stream = &exchange->begin_sse(
                    200, {{"Content-Type", "text/event-stream"},
                          {"Cache-Control", "no-cache"},
                          {"X-Accel-Buffering", "no"}});
            }
            return *stream;
        };
        const bool can_emit_sse = accept_sse &&
                                  options_.response_mode != HttpResponseMode::json_only;
        ToolContext context(cancellation->get_token(), [&](Json event) {
            if (!can_emit_sse) {
                throw std::logic_error("legacy request does not accept streamed emissions");
            }
            start_sse().write_event(event.dump());
        }, rpc_request.params, options_.notification_min_interval,
              options_.max_notifications_per_request, std::move(legacy_capabilities));
        if (auth_context) context.set_auth_context(*auth_context);
        else if (options_.request_state_subject) context.set_request_subject(options_.request_state_subject(request));
        if (can_emit_sse) {
            context.set_input_request_handler([&, sid = *session_id](const Json& input) {
                const core::RequestId id = "server-" + generate_session_id();
                auto pending = std::make_shared<std::promise<Json>>();
                auto future = pending->get_future();
                {
                    std::lock_guard<std::mutex> lock(legacy_session_mutex());
                    auto& session = legacy_sessions().at(sid);
                    if (session.pending_inputs.size() >= 32) throw std::runtime_error("Too many input requests");
                    session.pending_inputs.emplace(id, pending);
                }
                const auto remove_pending = [&] {
                    std::lock_guard<std::mutex> lock(legacy_session_mutex());
                    const auto session = legacy_sessions().find(sid);
                    if (session != legacy_sessions().end()) session->second.pending_inputs.erase(id);
                };
                try {
                    auto params = input.value("params", Json::object());
                    if (params.value("mode", std::string{}) == "url") {
                        params["elicitationId"] = std::get<std::string>(id);
                    }
                    start_sse().write_event(core::serialize_message(core::Request{
                        id, input.at("method").get<std::string>(), std::move(params)}));
                    const auto deadline = std::chrono::steady_clock::now() + options_.input_request_timeout;
                    while (future.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
                        if (cancellation->stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                            throw std::runtime_error("Input request cancelled or timed out");
                        }
                        std::lock_guard<std::mutex> lock(legacy_session_mutex());
                        if (!legacy_sessions().contains(sid)) throw std::runtime_error("Session closed");
                    }
                    auto result = future.get();
                    remove_pending();
                    return result;
                } catch (...) {
                    remove_pending();
                    throw;
                }
            });
        }
        auto response = server_.handle_message(message, &context);
        context.finish();
        if (auto* result = std::get_if<core::ResultResponse>(&response)) {
            const auto jsonrpc = core::ResultResponse{rpc_request.id, result->result};
            if (stream != nullptr) {
                stream->write_event(core::serialize_message(jsonrpc));
                stream->close();
            } else {
                exchange->respond(200, {{"Content-Type", "application/json"}},
                                  core::serialize_message(jsonrpc));
            }
            return;
        }
        if (auto* error = std::get_if<core::ErrorResponse>(&response)) {
            if (stream != nullptr) {
                stream->write_event(core::serialize_message(*error));
                stream->close();
            } else {
                respond_error(*exchange, 400, error->error,
                              error->id.value_or(rpc_request.id));
            }
            return;
        }
        exchange->respond(202, {}, "");
        return;
    }

    if (options_.mode == HttpMode::legacy && modern_request) {
        respond_error(*exchange, 400,
                      core::protocol_error(ErrorCode::invalid_request,
                                           legacy_protocol_error_message()),
                      rpc_request.id);
        return;
    }

    if (!validate_mirrored_headers(rpc_request, request.headers, header_error)) {
        respond_error(*exchange, 400, std::move(header_error), rpc_request.id);
        return;
    }
    if (!version_header || !supported_modern_version(*version_header)) {
        const auto version_error =
            core::unsupported_protocol_version_error(version_header.value_or(""),
                                                     {"2026-07-28"});
        respond_error(*exchange, 400, version_error, rpc_request.id);
        return;
    }
    if (rpc_request.method == "tools/call") {
        RpcError param_error{};
        if (!validate_mirrored_params(server_, rpc_request, request.headers, param_error)) {
            respond_error(*exchange, 400, std::move(param_error), rpc_request.id);
            return;
        }
    }
    if (!is_supported_method(rpc_request.method)) {
        respond_error(*exchange, 404,
                      core::protocol_error(ErrorCode::method_not_found, "Method not found"),
                      rpc_request.id);
        return;
    }
    if (!has_client_capabilities(rpc_request)) {
        respond_error(*exchange, 400,
                      core::protocol_error(ErrorCode::invalid_request,
                                           "Request metadata must include client capabilities"),
                      rpc_request.id);
        return;
    }
    if (rpc_request.method == "subscriptions/listen") {
        if (!accept_sse) {
            exchange->respond(406, {{"Content-Type", "application/json"}}, "");
            return;
        }
        const auto notifications = rpc_request.params.find("notifications");
        if (notifications == rpc_request.params.end() || !notifications->is_object()) {
            respond_error(*exchange, 400,
                          core::protocol_error(ErrorCode::invalid_params,
                                               "subscriptions/listen requires a notifications filter"),
                          rpc_request.id);
            return;
        }
        for (const auto* field : {"toolsListChanged", "promptsListChanged",
                                  "resourcesListChanged"}) {
            if (notifications->contains(field) && !(*notifications)[field].is_boolean()) {
                respond_error(*exchange, 400,
                              core::protocol_error(ErrorCode::invalid_params,
                                                   std::string(field) + " must be a boolean"),
                              rpc_request.id);
                return;
            }
        }
        if (notifications->contains("resourceSubscriptions")) {
            const auto& resources = (*notifications)["resourceSubscriptions"];
            if (!resources.is_array() ||
                !std::all_of(resources.begin(), resources.end(),
                             [](const auto& uri) { return uri.is_string(); })) {
                respond_error(*exchange, 400,
                              core::protocol_error(
                                  ErrorCode::invalid_params,
                                  "resourceSubscriptions must be an array of strings"),
                              rpc_request.id);
                return;
            }
        }

        const bool tools_requested = notifications->value("toolsListChanged", false);
        const bool prompts_requested = notifications->value("promptsListChanged", false);
        const bool resources_requested =
            notifications->value("resourcesListChanged", false);
        SubscriptionFilter filter;
        filter.tools_list_changed = tools_requested;
        filter.prompts_list_changed = prompts_requested;
        filter.resources_list_changed = resources_requested;
        core::Json accepted = core::Json::object();
        if (tools_requested) {
            accepted["toolsListChanged"] = true;
        }
        if (prompts_requested) accepted["promptsListChanged"] = true;
        if (resources_requested) accepted["resourcesListChanged"] = true;
        if (notifications->contains("resourceSubscriptions")) {
            std::set<std::string> seen;
            auto& accepted_uris = filter.resource_subscriptions;
            for (const auto& uri : (*notifications)["resourceSubscriptions"]) {
                if (accepted_uris.size() == 256) break;
                const auto value = uri.get<std::string>();
                if (seen.insert(value).second) accepted_uris.push_back(value);
            }
            accepted["resourceSubscriptions"] = accepted_uris;
        }

        const auto registry = server_.subscriptions_;
        std::uint64_t subscriber_id = 0;
        try {
            subscriber_id = registry->subscribe(
                request.peer.remote_address.empty() ? "unknown" : request.peer.remote_address,
                std::visit([](const auto& id) -> core::Json { return id; }, rpc_request.id),
                std::move(filter));
        } catch (const SubscriptionLimitError& error) {
            respond_error(*exchange, 429,
                          core::protocol_error(ErrorCode::invalid_request, error.what()),
                          rpc_request.id);
            return;
        }

        const auto request_id =
            std::visit([](const auto& id) -> core::Json { return id; }, rpc_request.id);
        const core::Json acknowledgement{
            {"jsonrpc", "2.0"},
            {"method", "notifications/subscriptions/acknowledged"},
            {"params", {{"_meta", {{"io.modelcontextprotocol/subscriptionId", request_id}}},
                        {"notifications", accepted}}},
        };
        const std::weak_ptr<SubscriptionRegistry> weak_registry = registry;
        exchange->on_disconnect([weak_registry, subscriber_id] {
            if (const auto active_registry = weak_registry.lock()) {
                active_registry->unsubscribe(subscriber_id);
            }
        });
        auto& stream = exchange->begin_sse(
            200, {{"Content-Type", "text/event-stream"},
                  {"Cache-Control", "no-cache"},
                  {"X-Accel-Buffering", "no"}});
        stream.write_event(acknowledgement.dump());

        for (;;) {
            core::Json event;
            const auto status = registry->next(subscriber_id, event,
                                               std::chrono::seconds(15));
            if (status == SubscriptionReadStatus::event) {
                stream.write_event(event.dump());
                continue;
            }
            if (status == SubscriptionReadStatus::timeout) {
                stream.write_comment("keep-alive");
                continue;
            }
            if (status == SubscriptionReadStatus::shutdown) {
                const core::Json completion{
                    {"jsonrpc", "2.0"},
                    {"id", request_id},
                    {"result", {{"resultType", "complete"},
                                {"_meta", {{"io.modelcontextprotocol/subscriptionId",
                                            request_id}}}}},
                };
                stream.write_event(completion.dump());
            }
            break;
        }
        registry->unsubscribe(subscriber_id);
        stream.close();
        return;
    }

    auto cancellation = std::make_shared<std::stop_source>();
    exchange->on_disconnect([cancellation] { cancellation->request_stop(); });
    SseStream* stream = nullptr;
    const auto start_sse = [&]() -> SseStream& {
        if (stream == nullptr) {
            stream = &exchange->begin_sse(
                200, {{"Content-Type", "text/event-stream"},
                      {"Cache-Control", "no-cache"},
                      {"X-Accel-Buffering", "no"}});
        }
        return *stream;
    };
    if (options_.response_mode == HttpResponseMode::sse_only) {
        start_sse();
    }
    const bool can_emit_sse = options_.response_mode != HttpResponseMode::json_only && accept_sse;
    ToolContext context(cancellation->get_token(), [&](Json event) {
        if (!can_emit_sse) {
            throw std::logic_error("request does not accept streamed tool emissions");
        }
        start_sse().write_event(event.dump());
    }, rpc_request.params, options_.notification_min_interval,
       options_.max_notifications_per_request);
    if (auth_context) context.set_auth_context(*auth_context);
    else if (options_.request_state_subject) context.set_request_subject(options_.request_state_subject(request));
    auto response = server_.handle_message(message, &context);
    context.finish();
    if (rpc_request.method == "server/discover") {
        if (auto* result = std::get_if<core::ResultResponse>(&response)) {
            result->result["protocolVersions"] = Json::array({"2026-07-28"});
        }
    }
    send_response(*exchange, options_, accept_json, accept_sse, response, stream);
}

std::string encode_mirrored_header_value(std::string_view value) {
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    const bool sentinel = value.starts_with(prefix) && value.ends_with(suffix);
    if (header_value_is_plain_safe(value) && !sentinel) {
        return std::string(value);
    }
    return std::string(prefix) + base64_encode(value) + std::string(suffix);
}

bool decode_mirrored_header_value(std::string_view value, std::string& decoded) {
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    if (!value.starts_with(prefix) || !value.ends_with(suffix)) {
        if (!header_value_is_plain_safe(value)) {
            return false;
        }
        decoded.assign(value);
        return true;
    }
    const auto encoded = value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
    return base64_decode(encoded, decoded);
}

}  // namespace cppmcp::transport
