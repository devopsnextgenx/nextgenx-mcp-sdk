#include "cppmcp/transport/http_client.hpp"
#include "cppmcp/client.hpp"

#include <asio.hpp>
#include <llhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace cppmcp {
namespace {

struct Url {
    std::string host;
    std::string port;
    std::string target;
    std::string authority;
};

struct Response {
    int status = 0;
    transport::Headers headers;
    std::string body;
    bool complete = false;
    bool header_field_open = false;
    std::size_t max_body = 0;
};

class HttpStatusError final : public std::runtime_error {
public:
    explicit HttpStatusError(int status)
        : std::runtime_error("HTTP request failed with status " + std::to_string(status)),
          status_(status) {}

    int status() const noexcept {
        return status_;
    }

private:
    int status_;
};

Response& response_state(llhttp_t* parser) {
    return *static_cast<Response*>(parser->data);
}

int on_status(llhttp_t* parser, const char* data, std::size_t length) {
    const auto& status = std::string_view(data, length);
    auto& response = response_state(parser);
    response.status = static_cast<int>(parser->status_code);
    (void)status;
    return 0;
}

int on_header_field(llhttp_t* parser, const char* data, std::size_t length) {
    auto& response = response_state(parser);
    if (response.headers.empty() || !response.header_field_open) {
        response.headers.emplace_back();
    }
    response.headers.back().first.append(data, length);
    response.header_field_open = true;
    return 0;
}

int on_header_field_complete(llhttp_t* parser) {
    response_state(parser).header_field_open = false;
    return 0;
}

int on_header_value(llhttp_t* parser, const char* data, std::size_t length) {
    auto& response = response_state(parser);
    if (response.headers.empty()) {
        response.headers.emplace_back();
    }
    response.headers.back().second.append(data, length);
    return 0;
}

int on_body(llhttp_t* parser, const char* data, std::size_t length) {
    auto& response = response_state(parser);
    if (length > response.max_body - std::min(response.max_body, response.body.size())) {
        return HPE_USER;
    }
    response.body.append(data, length);
    return 0;
}

int on_message_complete(llhttp_t* parser) {
    response_state(parser).complete = true;
    llhttp_pause(parser);
    return 0;
}

llhttp_settings_t response_settings() {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_status = on_status;
    settings.on_header_field = on_header_field;
    settings.on_header_field_complete = on_header_field_complete;
    settings.on_header_value = on_header_value;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;
    return settings;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

Url parse_url(std::string_view value) {
    constexpr std::string_view scheme = "http://";
    if (!value.starts_with(scheme)) {
        throw std::invalid_argument("HTTP client currently requires an http:// URL");
    }
    value.remove_prefix(scheme.size());
    const auto path_start = value.find_first_of("/?#");
    const auto authority = value.substr(0, path_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("HTTP client URL must have a host and no user information");
    }
    Url result;
    result.authority.assign(authority);
    result.target = path_start == std::string_view::npos ? "/" : std::string(value.substr(path_start));
    if (result.target.front() == '?' || result.target.front() == '#') {
        result.target.insert(result.target.begin(), '/');
    }
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            throw std::invalid_argument("invalid bracketed IPv6 host in HTTP URL");
        }
        result.host.assign(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                throw std::invalid_argument("invalid port in HTTP URL");
            }
            result.port.assign(authority.substr(close + 2));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            result.host.assign(authority.substr(0, colon));
            result.port.assign(authority.substr(colon + 1));
        } else {
            result.host.assign(authority);
        }
    }
    if (result.host.empty()) {
        throw std::invalid_argument("HTTP client URL host must not be empty");
    }
    if (result.port.empty()) {
        result.port = "80";
    }
    return result;
}

std::optional<std::string> header_value(const transport::Headers& headers,
                                        std::string_view wanted) {
    for (const auto& [name, value] : headers) {
        if (lower(name) == lower(wanted)) {
            return value;
        }
    }
    return std::nullopt;
}

std::string request_method(const core::Message& message) {
    if (const auto* request = std::get_if<core::Request>(&message)) {
        return request->method;
    }
    if (const auto* notification = std::get_if<core::Notification>(&message)) {
        return notification->method;
    }
    return {};
}

core::Json modern_meta(const std::string& name, const std::string& version) {
    return {
        {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
        {"io.modelcontextprotocol/clientCapabilities", core::Json::object()},
        {"io.modelcontextprotocol/clientInfo", {{"name", name}, {"version", version}}},
    };
}

AuthChallenge challenge_for(const std::vector<std::string>& values) {
    auto challenges = parse_www_authenticate(values);
    const auto bearer = std::find_if(challenges.begin(), challenges.end(), [](const auto& item) {
        return lower(item.scheme) == "bearer";
    });
    return bearer == challenges.end() ? AuthChallenge{} : *bearer;
}

}  // namespace

StaticBearerProvider::StaticBearerProvider(std::string token) : token_(std::move(token)) {
    if (token_.empty()) {
        throw std::invalid_argument("bearer token must not be empty");
    }
}

void StaticBearerProvider::apply(transport::Headers& headers) {
    headers.emplace_back("Authorization", "Bearer " + token_);
}

AuthAction StaticBearerProvider::on_challenge(const AuthChallenge&) {
    return AuthAction::fail;
}

AuthRequired::AuthRequired(AuthChallenge challenge)
    : std::runtime_error("HTTP 401 authentication required"), challenge_(std::move(challenge)) {}

const AuthChallenge& AuthRequired::challenge() const noexcept {
    return challenge_;
}

InsufficientScope::InsufficientScope(AuthChallenge challenge)
    : std::runtime_error("HTTP 403 authentication has insufficient scope"),
      challenge_(std::move(challenge)) {}

const AuthChallenge& InsufficientScope::challenge() const noexcept {
    return challenge_;
}

std::vector<AuthChallenge> parse_www_authenticate(const std::vector<std::string>& values) {
    std::vector<AuthChallenge> result;
    for (const auto& header : values) {
        std::vector<std::string_view> parts;
        bool quoted = false;
        bool escaped = false;
        std::size_t start = 0;
        for (std::size_t i = 0; i < header.size(); ++i) {
            const char ch = header[i];
            if (escaped) {
                escaped = false;
            } else if (quoted && ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = !quoted;
            } else if (ch == ',' && !quoted) {
                parts.push_back(trim(std::string_view(header).substr(start, i - start)));
                start = i + 1;
            }
        }
        parts.push_back(trim(std::string_view(header).substr(start)));

        AuthChallenge* current = nullptr;
        for (const auto part : parts) {
            if (part.empty()) {
                continue;
            }
            const auto first_space = part.find_first_of(" \t");
            const auto first_equal = part.find('=');
            const bool parameter = current != nullptr && first_equal != std::string_view::npos &&
                                   (first_space == std::string_view::npos ||
                                    first_equal < first_space);
            if (parameter) {
                const auto key = lower(trim(part.substr(0, first_equal)));
                auto raw = trim(part.substr(first_equal + 1));
                std::string value;
                if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
                    raw.remove_prefix(1);
                    raw.remove_suffix(1);
                    for (std::size_t i = 0; i < raw.size(); ++i) {
                        if (raw[i] == '\\' && i + 1 < raw.size()) {
                            ++i;
                        }
                        value.push_back(raw[i]);
                    }
                } else {
                    value.assign(raw);
                }
                current->parameters[key] = value;
                continue;
            }

            const auto space = part.find_first_of(" \t");
            AuthChallenge challenge;
            challenge.scheme.assign(part.substr(0, space));
            result.push_back(std::move(challenge));
            current = &result.back();
            if (space != std::string_view::npos) {
                const auto rest = trim(part.substr(space));
                const auto equal = rest.find('=');
                if (equal != std::string_view::npos) {
                    const auto key = lower(trim(rest.substr(0, equal)));
                    auto raw = trim(rest.substr(equal + 1));
                    std::string value;
                    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
                        raw.remove_prefix(1);
                        raw.remove_suffix(1);
                        for (std::size_t i = 0; i < raw.size(); ++i) {
                            if (raw[i] == '\\' && i + 1 < raw.size()) {
                                ++i;
                            }
                            value.push_back(raw[i]);
                        }
                    } else {
                        value.assign(raw);
                    }
                    current->parameters[key] = value;
                }
            }
        }
    }

    for (auto& challenge : result) {
        const auto get = [&challenge](const char* key) {
            const auto found = challenge.parameters.find(key);
            return found == challenge.parameters.end() ? std::string{} : found->second;
        };
        challenge.error = get("error");
        challenge.scope = get("scope");
        challenge.resource_metadata = get("resource_metadata");
        challenge.error_description = get("error_description");
    }
    return result;
}

namespace transport {

struct HttpClientTransport::Impl {
    explicit Impl(HttpClientOptions client_options)
        : options(std::move(client_options)), url(parse_url(options.url)), resolver(context),
          socket(context) {
        if (options.max_response_bytes == 0) {
            throw std::invalid_argument("maximum HTTP response size must be greater than zero");
        }
        if (options.connect_timeout.count() <= 0 || options.request_timeout.count() <= 0) {
            throw std::invalid_argument("HTTP client timeouts must be greater than zero");
        }
    }

    HttpClientOptions options;
    Url url;
    asio::io_context context;
    asio::ip::tcp::resolver resolver;
    asio::ip::tcp::socket socket;
    std::mutex mutex;
    std::string session_id;
    bool modern = true;
    bool started = false;
    bool closed = false;

    void connect_socket() {
        if (socket.is_open()) {
            return;
        }
        context.restart();
        asio::steady_timer deadline(context);
        deadline.expires_after(options.connect_timeout);
        bool timed_out = false;
        bool connected = false;
        std::string error_text;
        deadline.async_wait([&](const asio::error_code& error) {
            if (!error) {
                timed_out = true;
                resolver.cancel();
                socket.cancel();
            }
        });
        resolver.async_resolve(url.host, url.port,
                               [&](const asio::error_code& error,
                                   const asio::ip::tcp::resolver::results_type& endpoints) {
            if (error) {
                error_text = error.message();
                deadline.cancel();
                return;
            }
            asio::async_connect(socket, endpoints,
                                [&](const asio::error_code& connect_error,
                                    const asio::ip::tcp::endpoint&) {
                if (connect_error) {
                    error_text = connect_error.message();
                } else {
                    connected = true;
                }
                deadline.cancel();
            });
        });
        context.run();
        if (timed_out) {
            throw std::runtime_error("HTTP connection timed out");
        }
        if (!connected) {
            throw std::runtime_error("HTTP connection failed: " + error_text);
        }
        socket.non_blocking(true);
    }

    Response perform(std::string_view body, std::string_view method,
                     std::string_view name = {}, bool initialize = false,
                     bool notification = false, const Headers& mirrored = {},
                     bool challenged = false) {
        connect_socket();
        transport::Headers headers{
            {"Content-Type", "application/json"},
            {"Accept", "application/json, text/event-stream"},
            {"Connection", "keep-alive"},
        };
        if (options.auth_provider) {
            options.auth_provider->apply(headers);
        }
        headers.erase(std::remove_if(headers.begin(), headers.end(), [](const auto& header) {
            const auto key = lower(header.first);
            return key == "host" || key == "content-length" || key == "content-type" ||
                   key == "accept" || key == "connection" ||
                   key == "mcp-protocol-version" || key == "mcp-method" ||
                   key == "mcp-name" || key == "mcp-session-id" ||
                   key.starts_with("mcp-param-");
        }), headers.end());
        headers.emplace_back("Host", url.authority);
        headers.emplace_back("Content-Type", "application/json");
        headers.emplace_back("Accept", "application/json, text/event-stream");
        headers.emplace_back("Connection", "keep-alive");
        if (modern) {
            headers.emplace_back("MCP-Protocol-Version", "2026-07-28");
            headers.emplace_back("Mcp-Method", std::string(method));
            if (!name.empty()) {
                headers.emplace_back("Mcp-Name", std::string(name));
            }
        } else if (!initialize) {
            headers.emplace_back("Mcp-Session-Id", session_id);
            headers.emplace_back("MCP-Protocol-Version", "2025-11-25");
        }
        if (modern) {
            headers.insert(headers.end(), mirrored.begin(), mirrored.end());
        }

        std::ostringstream request;
        request << "POST " << url.target << " HTTP/1.1\r\n";
        for (const auto& [key, value] : headers) {
            request << key << ": " << value << "\r\n";
        }
        request << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        const auto request_text = request.str();
        auto deadline = std::chrono::steady_clock::now() + options.request_timeout;
        std::size_t written = 0;
        while (written < request_text.size()) {
            asio::error_code error;
            written += socket.write_some(
                asio::buffer(request_text.data() + written, request_text.size() - written),
                error);
            if (!error) {
                continue;
            }
            if (error != asio::error::would_block && error != asio::error::try_again) {
                socket.close();
                throw std::runtime_error("HTTP request write failed: " + error.message());
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                socket.close();
                throw std::runtime_error("HTTP request timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Response response;
        response.max_body = options.max_response_bytes;
        auto settings = response_settings();
        llhttp_t parser;
        llhttp_init(&parser, HTTP_RESPONSE, &settings);
        parser.data = &response;
        std::array<char, 8192> buffer{};
        while (!response.complete) {
            asio::error_code error;
            const auto count = socket.read_some(asio::buffer(buffer), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("HTTP response timed out");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                socket.close();
                throw std::runtime_error("HTTP response read failed: " + error.message());
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                socket.close();
                throw std::runtime_error("HTTP response timed out");
            }
            const auto parsed = llhttp_execute(&parser, buffer.data(), count);
            if (parsed != HPE_OK && parsed != HPE_PAUSED) {
                socket.close();
                const auto* reason = llhttp_get_error_reason(&parser);
                throw std::runtime_error("invalid HTTP response: " +
                                         std::string(reason == nullptr ? "parse error" : reason));
            }
        }
        if (header_value(response.headers, "Connection") == "close") {
            asio::error_code ignored;
            socket.close(ignored);
        }
        if (response.status == 401 || response.status == 403) {
            std::vector<std::string> challenge_headers;
            for (const auto& [key, value] : response.headers) {
                if (lower(key) == "www-authenticate") {
                    challenge_headers.push_back(value);
                }
            }
            const auto challenge = challenge_for(challenge_headers);
            if (!challenged && options.auth_provider &&
                options.auth_provider->on_challenge(challenge) == AuthAction::retry) {
                asio::error_code ignored;
                socket.close(ignored);
                return perform(body, method, name, initialize, notification, mirrored, true);
            }
            if (response.status == 401) {
                throw AuthRequired(challenge);
            }
            throw InsufficientScope(challenge);
        }
        if (!notification && response.status != 200) {
            throw HttpStatusError(response.status);
        }
        if (notification && response.status != 202 && response.status != 200) {
            throw HttpStatusError(response.status);
        }
        return response;
    }

    Response modern_probe() {
        const core::Request probe{
            std::int64_t{1},
            "server/discover",
            {{"_meta", modern_meta("cppmcp-client", "0.1.0")}},
        };
        modern = true;
        return perform(core::serialize_message(probe), "server/discover");
    }

    void negotiate() {
        if (options.policy != NegotiationPolicy::legacy_only) {
            try {
                auto response = modern_probe();
                const auto parsed = core::parse_message(response.body);
                const auto* message = std::get_if<core::Message>(&parsed);
                if (message == nullptr) {
                    throw std::runtime_error("modern discovery returned invalid JSON-RPC");
                }
                if (const auto* error = std::get_if<core::ErrorResponse>(message)) {
                    if (error->error.code !=
                        static_cast<std::int32_t>(core::ErrorCode::method_not_found)) {
                        throw std::runtime_error("modern discovery returned a JSON-RPC error");
                    }
                    if (options.policy == NegotiationPolicy::modern_only) {
                        throw std::runtime_error("modern protocol negotiation failed");
                    }
                } else {
                    modern = true;
                    return;
                }
            } catch (const AuthRequired&) {
                throw;
            } catch (const InsufficientScope&) {
                throw;
            } catch (const HttpStatusError& error) {
                if (options.policy == NegotiationPolicy::modern_only ||
                    (error.status() != 400 && error.status() != 404 && error.status() != 405)) {
                    throw;
                }
            } catch (const std::exception&) {
                throw;
            }
        }
        if (options.policy == NegotiationPolicy::modern_only) {
            throw std::runtime_error("modern protocol negotiation failed");
        }
        modern = false;
        const core::Request initialize{
            std::int64_t{1},
            "initialize",
            {{"protocolVersion", "2025-11-25"},
             {"capabilities", core::Json::object()},
             {"clientInfo", {{"name", "cppmcp-client"}, {"version", "0.1.0"}}}},
        };
        const auto response = perform(core::serialize_message(initialize), "initialize", {}, true);
        const auto sid = header_value(response.headers, "Mcp-Session-Id");
        if (!sid || sid->empty()) {
            throw std::runtime_error("legacy initialize response omitted Mcp-Session-Id");
        }
        session_id = *sid;
        const core::Notification initialized{"notifications/initialized", core::Json::object()};
        (void)perform(core::serialize_message(initialized), "notifications/initialized", {},
                      false, true);
    }

    void cache_tools(const core::Json& result) {
        const auto tools = result.find("tools");
        if (tools == result.end() || !tools->is_array()) {
            return;
        }
        for (const auto& tool : *tools) {
            if (tool.contains("name") && tool["name"].is_string()) {
                tool_schemas[tool["name"].get<std::string>()] = tool.value(
                    "inputSchema", core::Json::object());
            }
        }
    }

    void ensure_tool_schema(const std::string& name) {
        if (tool_schemas.contains(name)) {
            return;
        }
        auto params = core::Json::object();
        if (modern) {
            params["_meta"] = modern_meta("cppmcp-client", "0.1.0");
        }
        const core::Request list{
            std::int64_t{0},
            "tools/list",
            std::move(params),
        };
        const auto response = perform(core::serialize_message(list), "tools/list");
        const auto parsed = core::parse_message(response.body);
        const auto* message = std::get_if<core::Message>(&parsed);
        const auto* result = message == nullptr
                                 ? nullptr
                                 : std::get_if<core::ResultResponse>(message);
        if (result == nullptr) {
            throw std::runtime_error("tools/list did not return a JSON-RPC result");
        }
        cache_tools(result->result);
    }

    Headers mirrored_tool_headers(const std::string& name, const core::Json& arguments) {
        ensure_tool_schema(name);
        Headers mirrored;
        const auto schema = tool_schemas.find(name);
        if (schema == tool_schemas.end() || !schema->second.is_object()) {
            return mirrored;
        }
        const auto properties = schema->second.find("properties");
        if (properties == schema->second.end() || !properties->is_object()) {
            return mirrored;
        }
        for (auto property = properties->begin(); property != properties->end(); ++property) {
            const auto annotation = property.value().find("x-mcp-header");
            if (annotation == property.value().end() || !annotation->is_string() ||
                !arguments.contains(property.key())) {
                continue;
            }
            const auto& argument = arguments[property.key()];
            std::string value;
            if (argument.is_string()) {
                value = argument.get<std::string>();
            } else if (argument.is_boolean()) {
                value = argument.get<bool>() ? "true" : "false";
            } else if (argument.is_number_integer()) {
                value = std::to_string(argument.get<std::int64_t>());
            } else if (argument.is_number_unsigned()) {
                value = std::to_string(argument.get<std::uint64_t>());
            } else {
                throw std::invalid_argument("Mcp-Param values must be string, integer, or boolean");
            }
            mirrored.emplace_back("Mcp-Param-" + annotation->get<std::string>(),
                                  encode_mirrored_header_value(value));
        }
        return mirrored;
    }

    std::map<std::string, core::Json> tool_schemas;
};

HttpClientTransport::HttpClientTransport(HttpClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

HttpClientTransport::~HttpClientTransport() {
    close();
}

void HttpClientTransport::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) {
        return;
    }
    impl_->negotiate();
    impl_->started = true;
}

void HttpClientTransport::send(const core::Message& message) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->closed) {
        throw std::runtime_error("HTTP client transport is not connected");
    }
    const auto method = request_method(message);
    if (method.empty()) {
        throw std::invalid_argument("HTTP client transport cannot send JSON-RPC responses");
    }
    auto body_message = message;
    if (auto* request = std::get_if<core::Request>(&body_message)) {
        if (impl_->modern) {
            if (!request->params.is_object()) {
                request->params = core::Json::object();
            }
            if (!request->params.contains("_meta")) {
                request->params["_meta"] = modern_meta("cppmcp-client", "0.1.0");
            }
        } else if (request->params.is_object()) {
            request->params.erase("_meta");
        }
    }
    if (auto* notification = std::get_if<core::Notification>(&body_message)) {
        if (!impl_->modern && notification->params.is_object()) {
            notification->params.erase("_meta");
        }
    }

    std::string name;
    if (method == "tools/call") {
        const auto& params = std::get<core::Request>(body_message).params;
        if (params.contains("name") && params["name"].is_string()) {
            name = params["name"].get<std::string>();
        }
    }
    Headers mirrored;
    if (impl_->modern && method == "tools/call") {
        const auto& params = std::get<core::Request>(body_message).params;
        if (!name.empty() && params.contains("arguments") &&
            params["arguments"].is_object()) {
            mirrored = impl_->mirrored_tool_headers(name, params["arguments"]);
        }
    }
    const bool notification = std::holds_alternative<core::Notification>(body_message);
    const auto response = impl_->perform(core::serialize_message(body_message), method, name,
                                         false, notification, mirrored);
    if (notification || response.body.empty()) {
        return;
    }
    const auto content_type = header_value(response.headers, "Content-Type").value_or("");
    if (lower(content_type).find("text/event-stream") != std::string::npos) {
        SseParser parser(impl_->options.max_response_bytes);
        const auto events = parser.feed(response.body);
        for (const auto& event : events) {
            const auto parsed = core::parse_message(event.data);
            if (const auto* message_result = std::get_if<core::Message>(&parsed)) {
                if (method == "tools/list") {
                    if (const auto* result = std::get_if<core::ResultResponse>(message_result)) {
                        impl_->cache_tools(result->result);
                    }
                }
                if (on_message_) {
                    on_message_(*message_result);
                }
            } else if (on_parse_error_) {
                on_parse_error_(std::get<core::RpcError>(parsed));
            }
        }
        return;
    }
    const auto parsed = core::parse_message(response.body);
    if (const auto* message_result = std::get_if<core::Message>(&parsed)) {
        if (method == "tools/list") {
            if (const auto* result = std::get_if<core::ResultResponse>(message_result)) {
                impl_->cache_tools(result->result);
            }
        }
        if (on_message_) {
            on_message_(*message_result);
        }
    } else if (on_parse_error_) {
        on_parse_error_(std::get<core::RpcError>(parsed));
    }
}

void HttpClientTransport::close() {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closed) {
        return;
    }
    if (impl_->started && !impl_->modern && !impl_->session_id.empty()) {
        std::ostringstream request;
        request << "DELETE " << impl_->url.target << " HTTP/1.1\r\n"
                << "Host: " << impl_->url.authority << "\r\n"
                << "Mcp-Session-Id: " << impl_->session_id << "\r\n"
                << "MCP-Protocol-Version: 2025-11-25\r\n"
                << "Connection: close\r\nContent-Length: 0\r\n\r\n";
        const auto request_text = request.str();
        asio::error_code write_error;
        asio::write(impl_->socket, asio::buffer(request_text), write_error);
    }
    asio::error_code ignored;
    impl_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    impl_->socket.close(ignored);
    impl_->closed = true;
    if (on_close_) {
        on_close_(core::CloseReason::normal);
    }
}

core::CancellationStrategy HttpClientTransport::cancellation_strategy() const noexcept {
    return core::CancellationStrategy::abort_stream;
}

void HttpClientTransport::cancel_request(const core::RequestId&) {
    close();
}

bool HttpClientTransport::is_modern() const noexcept {
    return impl_->modern;
}

}  // namespace transport

void Client::connect_http(HttpClientOptions options) {
    policy_ = options.policy;
    connect(std::make_shared<transport::HttpClientTransport>(std::move(options)));
}

std::string Client::http_mode() const {
    const auto http_transport =
        std::dynamic_pointer_cast<transport::HttpClientTransport>(transport_);
    if (!http_transport) {
        throw std::runtime_error("client is not connected with HTTP transport");
    }
    return http_transport->is_modern() ? "modern" : "legacy";
}

}  // namespace cppmcp
