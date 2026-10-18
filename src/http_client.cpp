#include "cppmcp/transport/http_client.hpp"
#include "cppmcp/client.hpp"

#include <asio.hpp>
#include <llhttp.h>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#if CPPMCP_WITH_TLS
#include <asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "cppmcp/core/log.hpp"

namespace cppmcp {
namespace {

using NativeSocket = asio::ip::tcp::socket::native_handle_type;
constexpr NativeSocket invalid_native_socket = static_cast<NativeSocket>(-1);

void shutdown_native_socket(NativeSocket socket) {
    if (socket == invalid_native_socket) {
        return;
    }
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

struct Url {
    std::string host;
    std::string port;
    std::string target;
    std::string authority;
    bool tls = false;
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

class AuthRetryRequested final : public std::runtime_error {
public:
    AuthRetryRequested() : std::runtime_error("authentication provider requested a retry") {}
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

struct StreamingResponse {
    int status = 0;
    transport::Headers headers;
    bool header_field_open = false;
    bool headers_complete = false;
    bool message_complete = false;
    bool auth_retry_requested = false;
    transport::SseParser parser;
    std::function<void(const transport::SseEvent&)> event_callback;
    std::function<void(StreamingResponse&)> headers_callback;
    std::string callback_error;

    StreamingResponse(std::size_t limit,
                      std::function<void(const transport::SseEvent&)> callback,
                      std::function<void(StreamingResponse&)> on_headers)
        : parser(limit), event_callback(std::move(callback)),
          headers_callback(std::move(on_headers)) {}
};

StreamingResponse& stream_response_state(llhttp_t* parser) {
    return *static_cast<StreamingResponse*>(parser->data);
}

int on_stream_status(llhttp_t* parser, const char*, std::size_t) {
    stream_response_state(parser).status = static_cast<int>(parser->status_code);
    return 0;
}

int on_stream_header_field(llhttp_t* parser, const char* data, std::size_t length) {
    auto& response = stream_response_state(parser);
    if (response.headers.empty() || !response.header_field_open) {
        response.headers.emplace_back();
    }
    response.headers.back().first.append(data, length);
    response.header_field_open = true;
    return 0;
}

int on_stream_header_field_complete(llhttp_t* parser) {
    stream_response_state(parser).header_field_open = false;
    return 0;
}

int on_stream_header_value(llhttp_t* parser, const char* data, std::size_t length) {
    auto& response = stream_response_state(parser);
    if (response.headers.empty()) {
        response.headers.emplace_back();
    }
    response.headers.back().second.append(data, length);
    return 0;
}

int on_stream_headers_complete(llhttp_t* parser) {
    auto& response = stream_response_state(parser);
    response.headers_complete = true;
    try {
        response.headers_callback(response);
    } catch (const std::exception& error) {
        response.callback_error = error.what();
        return HPE_USER;
    }
    return 0;
}

int on_stream_body(llhttp_t* parser, const char* data, std::size_t length) {
    auto& response = stream_response_state(parser);
    try {
        for (const auto& event : response.parser.feed(std::string_view(data, length))) {
            response.event_callback(event);
        }
    } catch (const std::exception& error) {
        response.callback_error = error.what();
        return HPE_USER;
    }
    return 0;
}

int on_stream_message_complete(llhttp_t* parser) {
    stream_response_state(parser).message_complete = true;
    return 0;
}

llhttp_settings_t streaming_response_settings() {
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_status = on_stream_status;
    settings.on_header_field = on_stream_header_field;
    settings.on_header_field_complete = on_stream_header_field_complete;
    settings.on_header_value = on_stream_header_value;
    settings.on_headers_complete = on_stream_headers_complete;
    settings.on_body = on_stream_body;
    settings.on_message_complete = on_stream_message_complete;
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
    const bool tls = value.starts_with("https://");
    if (tls) {
        value.remove_prefix(8);
    } else if (value.starts_with("http://")) {
        value.remove_prefix(7);
    } else {
        throw std::invalid_argument("HTTP client URL must use http:// or https://");
    }
    const auto path_start = value.find_first_of("/?#");
    const auto authority = value.substr(0, path_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("HTTP client URL must have a host and no user information");
    }
    Url result;
    result.tls = tls;
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
        result.port = tls ? "443" : "80";
    }
    return result;
}

#if CPPMCP_WITH_TLS
void configure_client_tls_context(asio::ssl::context& context,
                                  const HttpClientOptions& options) {
    context.set_default_verify_paths();
    if (!options.ca_file.empty()) {
        context.load_verify_file(options.ca_file);
    }
}

template <typename SslStream>
void prepare_client_tls_stream(SslStream& stream, const Url& url,
                               const HttpClientOptions& options,
                               const std::shared_ptr<std::atomic<bool>>& hostname_mismatch) {
    if (options.dangerous_disable_verification) {
        stream.set_verify_mode(asio::ssl::verify_none);
        CPPMCP_LOG_WARN("DANGEROUS: TLS certificate and hostname verification disabled");
    } else {
        stream.set_verify_mode(asio::ssl::verify_peer);
        const auto verify_hostname = asio::ssl::host_name_verification(url.host);
        stream.set_verify_callback(
            [verify_hostname, hostname_mismatch](bool preverified,
                                                  asio::ssl::verify_context& context) mutable {
                const bool verified = verify_hostname(preverified, context);
                if (preverified && !verified) {
                    hostname_mismatch->store(true, std::memory_order_relaxed);
                }
                return verified;
            });
    }
    if (SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()) != 1) {
        throw std::runtime_error("TLS failed to set the server name");
    }
}

template <typename SslStream>
void perform_client_tls_handshake(SslStream& stream,
                                  std::chrono::milliseconds timeout,
                                  const std::shared_ptr<std::atomic<bool>>& hostname_mismatch) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        asio::error_code error;
        stream.handshake(asio::ssl::stream_base::client, error);
        if (!error) {
            return;
        }
        if (error == asio::error::would_block || error == asio::error::try_again) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("TLS handshake timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const long verify_result = SSL_get_verify_result(stream.native_handle());
        std::string reason = error.message();
        if (hostname_mismatch->load(std::memory_order_relaxed) ||
            verify_result == X509_V_ERR_HOSTNAME_MISMATCH ||
            verify_result == X509_V_ERR_IP_ADDRESS_MISMATCH) {
            reason = "certificate hostname or IP address mismatch";
        } else if (verify_result == X509_V_ERR_CERT_HAS_EXPIRED) {
            reason = "certificate has expired";
        } else if (verify_result != X509_V_OK) {
            reason = "certificate verification failed (untrusted CA or invalid chain): " +
                     std::string(X509_verify_cert_error_string(verify_result));
        }
        throw std::runtime_error("TLS handshake failed: " + reason);
    }
}
#endif

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
          socket(context)
#if CPPMCP_WITH_TLS
          , tls_context(asio::ssl::context::tls_client)
#endif
    {
        if (options.max_response_bytes == 0) {
            throw std::invalid_argument("maximum HTTP response size must be greater than zero");
        }
        if (options.connect_timeout.count() <= 0 || options.request_timeout.count() <= 0) {
            throw std::invalid_argument("HTTP client timeouts must be greater than zero");
        }
        if (url.tls) {
#if CPPMCP_WITH_TLS
            configure_client_tls_context(tls_context, options);
#else
            throw std::runtime_error("HTTPS is unavailable; rebuild with CPPMCP_WITH_TLS");
#endif
        }
    }

    HttpClientOptions options;
    Url url;
    asio::io_context context;
    asio::ip::tcp::resolver resolver;
    asio::ip::tcp::socket socket;
#if CPPMCP_WITH_TLS
    asio::ssl::context tls_context;
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> tls_socket;
#endif
    std::mutex mutex;
    std::string session_id;
    bool modern = true;
    bool started = false;
    bool closed = false;

    void close_socket() {
#if CPPMCP_WITH_TLS
        tls_socket.reset();
#endif
        asio::error_code ignored;
        socket.close(ignored);
    }

    std::size_t write_some(asio::const_buffer buffer, asio::error_code& error) {
#if CPPMCP_WITH_TLS
        if (tls_socket) {
            return tls_socket->write_some(buffer, error);
        }
#endif
        return socket.write_some(buffer, error);
    }

    std::size_t read_some(asio::mutable_buffer buffer, asio::error_code& error) {
#if CPPMCP_WITH_TLS
        if (tls_socket) {
            return tls_socket->read_some(buffer, error);
        }
#endif
        return socket.read_some(buffer, error);
    }

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
#if CPPMCP_WITH_TLS
        if (url.tls) {
            tls_socket =
                std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket, tls_context);
            auto hostname_mismatch = std::make_shared<std::atomic<bool>>(false);
            try {
                prepare_client_tls_stream(*tls_socket, url, options, hostname_mismatch);
                perform_client_tls_handshake(*tls_socket, options.connect_timeout,
                                             hostname_mismatch);
            } catch (...) {
                close_socket();
                throw;
            }
        }
#endif
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
            written += write_some(
                asio::buffer(request_text.data() + written, request_text.size() - written),
                error);
            if (!error) {
                continue;
            }
            if (error != asio::error::would_block && error != asio::error::try_again) {
                close_socket();
                throw std::runtime_error("HTTP request write failed: " + error.message());
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                close_socket();
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
            const auto count = read_some(asio::buffer(buffer), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("HTTP response timed out");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                close_socket();
                throw std::runtime_error("HTTP response read failed: " + error.message());
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                close_socket();
                throw std::runtime_error("HTTP response timed out");
            }
            const auto parsed = llhttp_execute(&parser, buffer.data(), count);
            if (parsed != HPE_OK && parsed != HPE_PAUSED) {
                close_socket();
                const auto* reason = llhttp_get_error_reason(&parser);
                throw std::runtime_error("invalid HTTP response: " +
                                         std::string(reason == nullptr ? "parse error" : reason));
            }
        }
        if (header_value(response.headers, "Connection") == "close") {
            close_socket();
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
                close_socket();
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

struct ListenHandle::Impl {
    Impl(HttpClientOptions client_options, Url endpoint, bool modern_mode,
         std::string legacy_session_id, core::Json requested_filter,
         std::function<void(const core::Notification&)> notify_callback,
         ListenCallbacks listen_callbacks, ListenOptions reconnect_options)
        : options(std::move(client_options)), url(std::move(endpoint)),
          modern(modern_mode), session_id(std::move(legacy_session_id)),
          filter(std::move(requested_filter)), callback(std::move(notify_callback)),
          callbacks(std::move(listen_callbacks)), reconnect(std::move(reconnect_options))
#if CPPMCP_WITH_TLS
          , tls_context(asio::ssl::context::tls_client)
#endif
    {
#if CPPMCP_WITH_TLS
        if (url.tls) {
            configure_client_tls_context(tls_context, options);
        }
#endif
    }

    HttpClientOptions options;
    Url url;
    bool modern;
    std::string session_id;
    core::Json filter;
    std::function<void(const core::Notification&)> callback;
    ListenCallbacks callbacks;
    ListenOptions reconnect;
#if CPPMCP_WITH_TLS
    asio::ssl::context tls_context;
#endif
    std::mutex startup_mutex;
    std::condition_variable startup_condition;
    bool startup_ready = false;
    std::string startup_error;
    std::mutex active_mutex;
    std::shared_ptr<asio::io_context> active_context;
    std::shared_ptr<asio::ip::tcp::socket> active_socket;
    std::shared_ptr<asio::ip::tcp::resolver> active_resolver;
    std::atomic<NativeSocket> active_native_socket{invalid_native_socket};
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;
    std::jthread worker;
    std::atomic<bool> stop_requested{false};
    std::int64_t next_request_id = 1;
    std::string last_event_id;
    std::optional<std::chrono::milliseconds> server_retry;
    bool auth_retry_used = false;

    void start() {
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    void close() {
        stop_requested.store(true, std::memory_order_release);
        if (worker.joinable()) {
            worker.request_stop();
        }
        wait_condition.notify_all();
        std::shared_ptr<asio::io_context> context;
        {
            std::lock_guard<std::mutex> lock(active_mutex);
            context = active_context;
        }
        if (context) {
            context->stop();
        }
        shutdown_native_socket(active_native_socket.load(std::memory_order_acquire));
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }

    void signal_ready(bool reconnected) {
        {
            std::lock_guard<std::mutex> lock(startup_mutex);
            startup_ready = true;
        }
        startup_condition.notify_all();
        if (reconnected && callbacks.on_reconnected) {
            callbacks.on_reconnected();
        }
    }

    void signal_startup_error(std::string error) {
        {
            std::lock_guard<std::mutex> lock(startup_mutex);
            if (!startup_ready && startup_error.empty()) {
                startup_error = std::move(error);
            }
        }
        startup_condition.notify_all();
    }

    std::chrono::milliseconds backoff_delay(std::size_t attempt) {
        const auto exponent = std::min<std::size_t>(attempt, 30);
        const auto scaled = static_cast<long double>(reconnect.backoff_base.count()) *
                            static_cast<long double>(std::uint64_t{1} << exponent);
        const auto bounded = std::min<long double>(
            scaled, static_cast<long double>(reconnect.backoff_cap.count()));
        std::random_device device;
        std::mt19937 generator(device());
        std::uniform_real_distribution<double> jitter(
            -reconnect.jitter_fraction, reconnect.jitter_fraction);
        const auto adjusted = std::max<long double>(
            0, bounded * (1 + static_cast<long double>(jitter(generator))));
        return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(adjusted));
    }

    void wait_before_reconnect(std::stop_token stop, std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_condition.wait_for(lock, stop, delay, [] { return false; });
    }

    void run(std::stop_token stop) {
        bool has_connected = false;
        std::size_t reconnect_attempts = 0;
        while (!stop.stop_requested() &&
               !stop_requested.load(std::memory_order_acquire)) {
            bool graceful = false;
            try {
                graceful = run_stream(stop, has_connected);
                if (stop.stop_requested() || stop_requested.load(std::memory_order_acquire) ||
                    graceful) {
                    break;
                }
                throw std::runtime_error("SSE stream ended unexpectedly");
            } catch (const AuthRetryRequested&) {
                if (stop.stop_requested() ||
                    stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
            } catch (const std::exception& error) {
                if (stop.stop_requested() ||
                    stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
                if (!has_connected) {
                    signal_startup_error(error.what());
                    break;
                }
                if (callbacks.on_stream_lost) {
                    callbacks.on_stream_lost();
                }
            }

            ++reconnect_attempts;
            if (reconnect.max_attempts != 0 &&
                reconnect_attempts > reconnect.max_attempts) {
                break;
            }
            wait_before_reconnect(stop, server_retry.value_or(backoff_delay(reconnect_attempts - 1)));
        }
        if (!has_connected && !stop.stop_requested() &&
            !stop_requested.load(std::memory_order_acquire)) {
            signal_startup_error("SSE listener stopped before opening a stream");
        }
    }

    void connect_stream(std::stop_token stop, const std::shared_ptr<asio::io_context>& context,
                       const std::shared_ptr<asio::ip::tcp::resolver>& resolver,
                       const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        asio::steady_timer timer(*context);
        timer.expires_after(options.connect_timeout);
        bool connected = false;
        bool timed_out = false;
        std::string error_message;
        timer.async_wait([&](const asio::error_code& error) {
            if (!error) {
                timed_out = true;
                resolver->cancel();
                asio::error_code ignored;
                socket->cancel(ignored);
            }
        });
        resolver->async_resolve(url.host, url.port,
                                [&, resolver, socket](const asio::error_code& error,
                                      const asio::ip::tcp::resolver::results_type& endpoints) {
            if (error) {
                error_message = error.message();
                timer.cancel();
                return;
            }
            asio::async_connect(*socket, endpoints,
                                [&, socket](const asio::error_code& connect_error,
                                            const asio::ip::tcp::endpoint&) {
                if (connect_error) {
                    error_message = connect_error.message();
                } else {
                    connected = true;
                }
                timer.cancel();
            });
        });
        context->run();
        if (stop.stop_requested() || stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (timed_out) {
            throw std::runtime_error("SSE connection timed out");
        }
        if (!connected) {
            throw std::runtime_error("SSE connection failed: " + error_message);
        }
        socket->non_blocking(true);
    }

    bool run_stream(std::stop_token stop, bool& has_connected) {
        auto context = std::make_shared<asio::io_context>();
        auto resolver = std::make_shared<asio::ip::tcp::resolver>(*context);
        auto socket = std::make_shared<asio::ip::tcp::socket>(*context);
        {
            std::lock_guard<std::mutex> lock(active_mutex);
            active_socket = socket;
            active_resolver = resolver;
            active_context = context;
        }
        if (stop.stop_requested() || stop_requested.load(std::memory_order_acquire)) {
            return false;
        }
        connect_stream(stop, context, resolver, socket);
        if (stop.stop_requested() || stop_requested.load(std::memory_order_acquire)) {
            return false;
        }

        const auto native_socket = socket->native_handle();
        active_native_socket.store(native_socket, std::memory_order_release);
        struct NativeSocketRegistration {
            std::atomic<NativeSocket>& active;
            NativeSocket socket;
            ~NativeSocketRegistration() {
                auto expected = socket;
                active.compare_exchange_strong(expected, invalid_native_socket,
                                               std::memory_order_acq_rel);
            }
        } registration{active_native_socket, native_socket};
        if (stop.stop_requested() || stop_requested.load(std::memory_order_acquire)) {
            shutdown_native_socket(native_socket);
            return false;
        }

#if CPPMCP_WITH_TLS
        std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> tls_socket;
        if (url.tls) {
            tls_socket =
                std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(*socket,
                                                                            tls_context);
            auto hostname_mismatch = std::make_shared<std::atomic<bool>>(false);
            prepare_client_tls_stream(*tls_socket, url, options, hostname_mismatch);
            perform_client_tls_handshake(*tls_socket, options.connect_timeout,
                                         hostname_mismatch);
        }
#endif
        auto write_some = [&](asio::const_buffer buffer, asio::error_code& error) {
#if CPPMCP_WITH_TLS
            if (tls_socket) {
                return tls_socket->write_some(buffer, error);
            }
#endif
            return socket->write_some(buffer, error);
        };
        auto read_some = [&](asio::mutable_buffer buffer, asio::error_code& error) {
#if CPPMCP_WITH_TLS
            if (tls_socket) {
                return tls_socket->read_some(buffer, error);
            }
#endif
            return socket->read_some(buffer, error);
        };

        core::Json request_id;
        std::string request_text;
        transport::Headers headers;
        headers.emplace_back("Host", url.authority);
        headers.emplace_back("Accept", modern ? "application/json, text/event-stream"
                                               : "text/event-stream");
        headers.emplace_back("Connection", "keep-alive");
        if (options.auth_provider) {
            options.auth_provider->apply(headers);
        }
        headers.erase(std::remove_if(headers.begin(), headers.end(), [](const auto& header) {
            const auto key = lower(header.first);
            return key == "host" || key == "accept" || key == "connection" ||
                   key == "content-length" || key == "content-type" ||
                   key == "mcp-protocol-version" || key == "mcp-method" ||
                   key == "mcp-session-id" || key.starts_with("mcp-param-") ||
                   key == "mcp-name";
        }), headers.end());
        headers.emplace_back("Host", url.authority);
        headers.emplace_back("Connection", "keep-alive");
        std::string body;
        if (modern) {
            request_id = next_request_id++;
            const core::Request request{
                request_id.get<std::int64_t>(),
                "subscriptions/listen",
                {{"_meta", modern_meta("cppmcp-client", "0.1.0")},
                 {"notifications", filter}},
            };
            body = core::serialize_message(request);
            headers.emplace_back("Content-Type", "application/json");
            headers.emplace_back("Accept", "application/json, text/event-stream");
            headers.emplace_back("MCP-Protocol-Version", "2026-07-28");
            headers.emplace_back("Mcp-Method", "subscriptions/listen");
            request_text = "POST " + url.target + " HTTP/1.1\r\n";
        } else {
            headers.emplace_back("Accept", "text/event-stream");
            headers.emplace_back("Mcp-Session-Id", session_id);
            headers.emplace_back("MCP-Protocol-Version", "2025-11-25");
            if (!last_event_id.empty()) {
                headers.emplace_back("Last-Event-ID", last_event_id);
            }
            request_text = "GET " + url.target + " HTTP/1.1\r\n";
        }
        for (const auto& [key, value] : headers) {
            request_text += key + ": " + value + "\r\n";
        }
        if (modern) {
            request_text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        request_text += "\r\n";
        request_text += body;

        const auto deadline = std::chrono::steady_clock::now() + options.request_timeout;
        std::size_t written = 0;
        while (written < request_text.size() && !stop.stop_requested() &&
               !stop_requested.load(std::memory_order_acquire)) {
            asio::error_code error;
            written += write_some(
                asio::buffer(request_text.data() + written, request_text.size() - written),
                error);
            if (!error) {
                continue;
            }
            if (error != asio::error::would_block && error != asio::error::try_again) {
                throw std::runtime_error("SSE request write failed: " + error.message());
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("SSE request timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        bool acknowledged = false;
        bool graceful = false;
        bool connected_this_attempt = false;
        auto mark_connected = [&] {
            const bool reconnected = has_connected;
            has_connected = true;
            auth_retry_used = false;
            connected_this_attempt = true;
            signal_ready(reconnected);
        };
        auto process_event = [&](const transport::SseEvent& event) {
            if (!event.last_event_id.empty()) {
                last_event_id = event.last_event_id;
            }
            if (event.retry) {
                server_retry = event.retry;
            }
            const auto parsed = core::parse_message(event.data);
            const auto* message = std::get_if<core::Message>(&parsed);
            if (message == nullptr) {
                throw std::runtime_error("SSE event did not contain a valid JSON-RPC message");
            }
            if (modern) {
                if (!acknowledged) {
                    const auto* ack = std::get_if<core::Notification>(message);
                    if (ack == nullptr ||
                        ack->method != "notifications/subscriptions/acknowledged") {
                        throw std::runtime_error(
                            "listen stream did not begin with its acknowledgement");
                    }
                    const auto meta = ack->params.value("_meta", core::Json::object());
                    if (meta.value("io.modelcontextprotocol/subscriptionId", core::Json{}) !=
                        request_id) {
                        throw std::runtime_error("listen acknowledgement has the wrong subscription id");
                    }
                    acknowledged = true;
                    mark_connected();
                    return;
                }
                if (const auto* result = std::get_if<core::ResultResponse>(message)) {
                    graceful = result->result.value("resultType", std::string{}) == "complete";
                    return;
                }
            }
            if (const auto* notification = std::get_if<core::Notification>(message)) {
                if (callback) {
                    callback(*notification);
                }
            } else if (const auto* request = std::get_if<core::Request>(message)) {
                if (callbacks.on_server_request) {
                    callbacks.on_server_request(*request);
                }
            } else {
                throw std::runtime_error("unexpected JSON-RPC response on a notification stream");
            }
        };
        auto validate_headers = [&](StreamingResponse& response) {
            if (response.status == 401 || response.status == 403) {
                std::vector<std::string> challenges;
                for (const auto& [key, value] : response.headers) {
                    if (lower(key) == "www-authenticate") {
                        challenges.push_back(value);
                    }
                }
                const auto challenge = challenge_for(challenges);
                if (options.auth_provider &&
                    options.auth_provider->on_challenge(challenge) == AuthAction::retry &&
                    !auth_retry_used) {
                    auth_retry_used = true;
                    response.auth_retry_requested = true;
                    throw std::runtime_error("authentication provider requested a retry");
                }
                if (response.status == 401) {
                    throw AuthRequired(challenge);
                }
                throw InsufficientScope(challenge);
            }
            if (response.status != 200) {
                throw HttpStatusError(response.status);
            }
            const auto content_type = header_value(response.headers, "Content-Type");
            if (!content_type ||
                lower(*content_type).find("text/event-stream") == std::string::npos) {
                throw std::runtime_error("listen endpoint did not return text/event-stream");
            }
            if (!modern) {
                mark_connected();
            }
        };
        StreamingResponse response(options.max_response_bytes, process_event, validate_headers);
        auto settings = streaming_response_settings();
        llhttp_t parser;
        llhttp_init(&parser, HTTP_RESPONSE, &settings);
        parser.data = &response;
        std::array<char, 8192> buffer{};
        while (!stop.stop_requested() &&
               !stop_requested.load(std::memory_order_acquire) && !graceful) {
            asio::error_code error;
            const auto count = read_some(asio::buffer(buffer), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                if (!connected_this_attempt &&
                    std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("listen acknowledgement timed out");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                if (!response.callback_error.empty()) {
                    throw std::runtime_error(response.callback_error);
                }
                if (error == asio::error::eof || error == asio::error::connection_reset) {
                    return graceful || response.message_complete;
                }
                throw std::runtime_error("SSE response read failed: " + error.message());
            }
            const auto parsed = llhttp_execute(&parser, buffer.data(), count);
            if (parsed != HPE_OK && parsed != HPE_PAUSED) {
                if (response.auth_retry_requested) {
                    throw AuthRetryRequested();
                }
                if (!response.callback_error.empty()) {
                    throw std::runtime_error(response.callback_error);
                }
                const auto* reason = llhttp_get_error_reason(&parser);
                throw std::runtime_error("invalid SSE HTTP response: " +
                                         std::string(reason == nullptr ? "parse error" : reason));
            }
            if (response.message_complete) {
                return graceful;
            }
        }
        return graceful;
    }
};

ListenHandle::ListenHandle(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ListenHandle::~ListenHandle() {
    close();
}

void ListenHandle::close() {
    if (impl_) {
        impl_->close();
    }
}

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
        const auto deadline = std::chrono::steady_clock::now() +
                              std::min(impl_->options.request_timeout,
                                       std::chrono::milliseconds(1000));
        std::size_t written = 0;
        while (written < request_text.size() &&
               std::chrono::steady_clock::now() < deadline) {
            asio::error_code write_error;
            written += impl_->write_some(
                asio::buffer(request_text.data() + written, request_text.size() - written),
                write_error);
            if (write_error && write_error != asio::error::would_block &&
                write_error != asio::error::try_again) {
                break;
            }
            if (write_error) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    asio::error_code ignored;
    impl_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    impl_->close_socket();
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

std::shared_ptr<ListenHandle> HttpClientTransport::listen(
    nlohmann::json filter,
    std::function<void(const core::Notification&)> callback,
    ListenCallbacks callbacks, ListenOptions options) {
    if (!filter.is_object()) {
        throw std::invalid_argument("listen filter must be a JSON object");
    }
    if (!callback) {
        throw std::invalid_argument("listen notification callback must not be empty");
    }
    if (options.backoff_base.count() <= 0 || options.backoff_cap < options.backoff_base ||
        !std::isfinite(options.jitter_fraction) || options.jitter_fraction < 0 ||
        options.jitter_fraction > 1) {
        throw std::invalid_argument("invalid listen reconnect backoff configuration");
    }
    HttpClientOptions client_options;
    Url url;
    bool modern = false;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->started || impl_->closed) {
            throw std::runtime_error("HTTP client transport is not connected");
        }
        client_options = impl_->options;
        url = impl_->url;
        modern = impl_->modern;
        session_id = impl_->session_id;
    }
    auto listener = std::make_unique<ListenHandle::Impl>(
        std::move(client_options), std::move(url), modern, std::move(session_id),
        std::move(filter), std::move(callback), std::move(callbacks), std::move(options));
    listener->start();
    auto handle = std::shared_ptr<ListenHandle>(new ListenHandle(std::move(listener)));
    std::unique_lock<std::mutex> lock(handle->impl_->startup_mutex);
    const bool started = handle->impl_->startup_condition.wait_for(
        lock, impl_->options.request_timeout, [&] {
            return handle->impl_->startup_ready || !handle->impl_->startup_error.empty();
        });
    if (!started) {
        lock.unlock();
        handle->close();
        throw std::runtime_error("timed out waiting for listen stream acknowledgement");
    }
    if (!handle->impl_->startup_error.empty()) {
        const auto error = handle->impl_->startup_error;
        lock.unlock();
        handle->close();
        throw std::runtime_error(error);
    }
    return handle;
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

std::shared_ptr<transport::ListenHandle> Client::listen(
    nlohmann::json filter,
    std::function<void(const core::Notification&)> callback,
    transport::ListenCallbacks callbacks, transport::ListenOptions options) {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    const auto http_transport =
        std::dynamic_pointer_cast<transport::HttpClientTransport>(transport_);
    if (!http_transport) {
        throw std::runtime_error("listen requires the HTTP client transport");
    }
    auto handle = http_transport->listen(std::move(filter), std::move(callback),
                                         std::move(callbacks), std::move(options));
    listen_closers_.emplace_back([weak_handle = std::weak_ptr<transport::ListenHandle>(handle)] {
        if (const auto active_handle = weak_handle.lock()) {
            active_handle->close();
        }
    });
    return handle;
}

}  // namespace cppmcp
