#include "cppmcp/transport/http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
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
using core::Request;
using core::RpcError;

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
        std::string decoded;
        if (argument == nullptr || !argument->is_string() || values.size() != 1 ||
            !decode_mirrored_header_value(values.front(), decoded) ||
            decoded != argument->get<std::string>()) {
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
    return method == "server/discover" || method == "tools/list" || method == "tools/call";
}

void send_response(IHttpExchange& exchange, const HttpHandlerOptions& options,
                   bool accept_json, bool accept_sse, const Message& message,
                   SseStream* stream = nullptr) {
    const auto json = core::serialize_message(message);
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

McpHttpHandler::McpHttpHandler(Server& server, HttpHandlerOptions options)
    : server_(server), options_(std::move(options)) {
    if (options_.path.empty() || options_.path.front() != '/') {
        throw std::invalid_argument("HTTP MCP endpoint path must begin with '/'");
    }
}

void McpHttpHandler::handle(const std::shared_ptr<IHttpExchange>& exchange) const {
    if (!exchange) {
        throw std::invalid_argument("HTTP exchange must not be null");
    }
    const auto& request = exchange->request();
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
    if (options_.authenticator && !options_.authenticator(request)) {
        exchange->respond(401, {{"WWW-Authenticate", "Bearer"}}, "");
        return;
    }
    if (request.method != "POST") {
        exchange->respond(405, {{"Allow", "POST"}}, "");
        return;
    }
    if (std::any_of(request.headers.begin(), request.headers.end(), [](const auto& header) {
            return !valid_header_name(header.first) ||
                   header.second.find_first_of("\r\n") != std::string::npos;
        })) {
        exchange->respond(400, {}, "");
        return;
    }

    const bool accept_json = accepts(request.headers, "application/json");
    const bool accept_sse = accepts(request.headers, "text/event-stream");
    if ((options_.response_mode == HttpResponseMode::json_only && !accept_json) ||
        (options_.response_mode == HttpResponseMode::sse_only && !accept_sse) ||
        (options_.response_mode == HttpResponseMode::automatic && !accept_json && !accept_sse)) {
        exchange->respond(406, {}, "");
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
    if (std::holds_alternative<core::Notification>(message)) {
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
    if (!validate_mirrored_headers(rpc_request, request.headers, header_error)) {
        respond_error(*exchange, 400, std::move(header_error), rpc_request.id);
        return;
    }
    const auto protocol_version = single_header(request.headers, "MCP-Protocol-Version");
    if (!protocol_version || !supported_modern_version(*protocol_version)) {
        const auto version_error =
            core::unsupported_protocol_version_error(protocol_version.value_or(""),
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
    const bool can_emit_sse = options_.response_mode != HttpResponseMode::json_only &&
                              accept_sse;
    ToolContext context(
        cancellation->get_token(),
        [&](Json event) {
            if (!can_emit_sse) {
                throw std::logic_error("request does not accept streamed tool emissions");
            }
            start_sse().write_event(event.dump());
        });
    auto response = server_.handle_message(message, &context);
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
