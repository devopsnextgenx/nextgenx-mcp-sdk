#include "cppmcp/net/network_policy.hpp"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>

namespace cppmcp::net {
namespace {

using Tcp = asio::ip::tcp;

struct Url {
    bool https = false;
    std::string host;
    std::string port;
    std::string target;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

Url parse_url(const std::string& value) {
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string::npos) throw std::invalid_argument("invalid URL scheme");
    const auto scheme = lower(value.substr(0, scheme_end));
    if (scheme != "https" && scheme != "http") throw std::invalid_argument("unsupported URL scheme");
    const auto authority_start = scheme_end + 3;
    const auto authority_end = value.find_first_of("/?#", authority_start);
    auto authority = value.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        throw std::invalid_argument("invalid URL authority");
    }

    Url result;
    result.https = scheme == "https";
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) throw std::invalid_argument("invalid IPv6 URL host");
        result.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') throw std::invalid_argument("invalid URL port");
            result.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            if (authority.find(':') != colon) throw std::invalid_argument("IPv6 hosts require brackets");
            result.host = authority.substr(0, colon);
            result.port = authority.substr(colon + 1);
        } else {
            result.host = authority;
        }
    }
    if (result.host.empty()) throw std::invalid_argument("invalid URL host");
    if (result.port.empty()) result.port = result.https ? "443" : "80";
    unsigned port = 0;
    const auto parsed_port = std::from_chars(result.port.data(),
                                             result.port.data() + result.port.size(), port);
    if (parsed_port.ec != std::errc{} || parsed_port.ptr != result.port.data() + result.port.size() ||
        port == 0 || port > 65535) {
        throw std::invalid_argument("invalid URL port");
    }
    if (authority_end == std::string::npos) {
        result.target = "/";
    } else {
        const auto fragment = value.find('#', authority_end);
        if (fragment != std::string::npos) throw std::invalid_argument("URL fragments are forbidden");
        result.target = value[authority_end] == '?' ? "/" + value.substr(authority_end)
                                                    : value.substr(authority_end);
    }
    if (result.target.find_first_of("\r\n ") != std::string::npos) {
        throw std::invalid_argument("invalid URL request target");
    }
    return result;
}

bool ipv4_private(const asio::ip::address_v4& address) {
    const auto bytes = address.to_bytes();
    const auto a = bytes[0];
    const auto b = bytes[1];
    return a == 0 || a == 10 || a == 127 || a >= 224 ||
           (a == 100 && b >= 64 && b <= 127) ||
           (a == 169 && b == 254) ||
           (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && (b == 0 || b == 168)) ||
           (a == 198 && (b == 18 || b == 19 || b == 51)) ||
           (a == 203 && b == 0);
}

bool blocked_address(const asio::ip::address& address,
                     const NetworkPolicy& policy) {
    if (address.is_v4()) {
        if (address.is_loopback()) return !policy.allow_http_loopback;
        return policy.block_private_ranges && ipv4_private(address.to_v4());
    }
    const auto v6 = address.to_v6();
    if (v6.is_v4_mapped()) {
        if (!policy.allow_ipv4_mapped) return true;
        const auto bytes = v6.to_bytes();
        asio::ip::address_v4::bytes_type mapped_bytes{};
        std::copy(bytes.begin() + 12, bytes.end(), mapped_bytes.begin());
        const auto mapped = asio::ip::make_address_v4(mapped_bytes);
        if (mapped.is_loopback()) return !policy.allow_http_loopback;
        return policy.block_private_ranges && ipv4_private(mapped);
    }
    if (v6.is_loopback()) return !policy.allow_http_loopback;
    const auto bytes = v6.to_bytes();
    const bool unique_local = (bytes[0] & 0xfe) == 0xfc;
    return policy.block_private_ranges &&
           (v6.is_unspecified() || v6.is_link_local() || v6.is_multicast() ||
            unique_local || v6.is_site_local());
}

void validate_host(const Url& url, const NetworkPolicy& policy) {
    if (!policy.allowed_hosts.empty() &&
        std::none_of(policy.allowed_hosts.begin(), policy.allowed_hosts.end(),
                     [&](const auto& host) { return lower(host) == lower(url.host); })) {
        throw BlockedAddress("destination host is not allow-listed");
    }
    if (policy.require_https && !url.https && !policy.allow_http_loopback) {
        throw BlockedAddress("HTTPS is required by network policy");
    }
}

template <typename Initiate, typename Cancel>
void await_operation(asio::io_context& io, std::chrono::milliseconds timeout,
                     Initiate initiate, Cancel cancel) {
    asio::steady_timer timer(io);
    asio::error_code operation_error;
    bool completed = false;
    bool timed_out = false;
    timer.expires_after(timeout);
    timer.async_wait([&](const asio::error_code& error) {
        if (!error) {
            timed_out = true;
            cancel();
        }
    });
    initiate([&](const asio::error_code& error, auto&&...) {
        operation_error = error;
        completed = true;
        timer.cancel();
    });
    io.restart();
    io.run();
    if (timed_out || !completed) throw std::runtime_error("network operation timed out");
    if (operation_error) throw std::runtime_error("network operation failed");
}

template <typename Stream>
HttpResponse exchange(asio::io_context& io, Stream& stream,
                      Tcp::socket& socket, const NetworkPolicy& policy,
                      const std::string& request) {
    await_operation(io, policy.read_timeout,
                    [&](auto completion) {
                        asio::async_write(stream, asio::buffer(request), completion);
                    },
                    [&] { socket.cancel(); });

    asio::streambuf response_buffer(policy.max_response_bytes + 16 * 1024);
    await_operation(io, policy.read_timeout,
                    [&](auto completion) {
                        asio::async_read_until(stream, response_buffer, "\r\n\r\n", completion);
                    },
                    [&] { socket.cancel(); });

    std::istream input(&response_buffer);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream status_line(line);
    std::string protocol;
    HttpResponse response;
    status_line >> protocol >> response.status;
    if (!protocol.starts_with("HTTP/") || response.status < 100 || response.status > 599) {
        throw std::runtime_error("invalid HTTP response status");
    }
    std::size_t content_length = 0;
    bool content_length_seen = false;
    while (std::getline(input, line) && line != "\r") {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("invalid HTTP response header");
        auto name = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string{} : value.substr(first);
        if (name == "transfer-encoding" && lower(value) != "identity") {
            throw std::runtime_error("unsupported HTTP transfer encoding");
        }
        if (name == "content-length") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                                 content_length);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                throw std::runtime_error("invalid HTTP content length");
            }
            content_length_seen = true;
        }
        response.headers.insert_or_assign(std::move(name), std::move(value));
    }
    if (!content_length_seen || content_length > policy.max_response_bytes) {
        throw std::runtime_error("HTTP response size is missing or exceeds policy");
    }
    if (response_buffer.size() > content_length) {
        throw std::runtime_error("HTTP response exceeds declared content length");
    }
    const auto missing = content_length - response_buffer.size();
    if (missing > 0) {
        await_operation(io, policy.read_timeout,
                        [&](auto completion) {
                            asio::async_read(stream, response_buffer,
                                             asio::transfer_exactly(missing), completion);
                        },
                        [&] { socket.cancel(); });
    }
    response.body.resize(content_length);
    input.read(response.body.data(), static_cast<std::streamsize>(content_length));
    if (static_cast<std::size_t>(input.gcount()) != content_length) {
        throw std::runtime_error("truncated HTTP response body");
    }
    return response;
}

std::string host_header(const Url& url) {
    const auto parsed_port = static_cast<unsigned>(std::stoul(url.port));
    const bool default_port = (url.https && parsed_port == 443) ||
                              (!url.https && parsed_port == 80);
    const auto host = url.host.find(':') == std::string::npos
                          ? url.host
                          : "[" + url.host + "]";
    return default_port ? host : host + ":" + url.port;
}

}  // namespace

HttpResponse fetch_http(const std::string& url_value, const NetworkPolicy& policy,
                        std::string method, std::map<std::string, std::string> headers,
                        std::string body) {
    if (policy.max_response_bytes == 0 || policy.connect_timeout.count() <= 0 ||
        policy.read_timeout.count() <= 0) {
        throw std::invalid_argument("network policy limits and timeouts must be positive");
    }
    const auto url = parse_url(url_value);
    validate_host(url, policy);
    asio::io_context io;
    Tcp::resolver resolver(io);
    Tcp::resolver::results_type endpoints;
    asio::steady_timer resolve_timer(io);
    asio::error_code resolve_error;
    bool resolve_complete = false;
    bool resolve_timed_out = false;
    resolve_timer.expires_after(policy.connect_timeout);
    resolve_timer.async_wait([&](const asio::error_code& error) {
        if (!error) {
            resolve_timed_out = true;
            resolver.cancel();
        }
    });
    resolver.async_resolve(url.host, url.port,
                           [&](const asio::error_code& error,
                               const Tcp::resolver::results_type& resolved) {
        resolve_error = error;
        if (!error) endpoints = resolved;
        resolve_complete = true;
        resolve_timer.cancel();
    });
    io.restart();
    io.run();
    if (resolve_timed_out || !resolve_complete) {
        throw std::runtime_error("DNS resolution timed out");
    }
    if (resolve_error) throw std::runtime_error("DNS resolution failed");
    bool has_endpoint = false;
    bool all_loopback = true;
    for (const auto& endpoint : endpoints) {
        has_endpoint = true;
        const auto address = endpoint.endpoint().address();
        bool loopback = address.is_loopback();
        if (address.is_v6() && address.to_v6().is_v4_mapped() &&
            policy.allow_ipv4_mapped) {
            const auto bytes = address.to_v6().to_bytes();
            asio::ip::address_v4::bytes_type mapped_bytes{};
            std::copy(bytes.begin() + 12, bytes.end(), mapped_bytes.begin());
            loopback = asio::ip::make_address_v4(mapped_bytes).is_loopback();
        }
        all_loopback = all_loopback && loopback;
        if (blocked_address(endpoint.endpoint().address(), policy)) {
            throw BlockedAddress("destination resolves to a blocked address");
        }
    }
    if (!has_endpoint) throw BlockedAddress("destination has no resolved addresses");
    if (!url.https && policy.require_https && !all_loopback) {
        throw BlockedAddress("insecure HTTP is permitted only for loopback destinations");
    }

    Tcp::socket socket(io);
    await_operation(io, policy.connect_timeout,
                    [&](auto completion) {
                        asio::async_connect(socket, endpoints, completion);
                    },
                    [&] { socket.cancel(); });

    std::string request = std::move(method) + " " + url.target + " HTTP/1.1\r\nHost: " +
                          host_header(url) + "\r\nConnection: close\r\n";
    for (const auto& [name, value] : headers) {
        if (name.find_first_of("\r\n:") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("invalid outbound HTTP header");
        }
        request += name + ": " + value + "\r\n";
    }
    if (!body.empty()) {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n" + body;

    if (url.https) {
#if CPPMCP_WITH_TLS
        asio::ssl::context ssl_context(asio::ssl::context::tls_client);
        ssl_context.set_default_verify_paths();
        asio::ssl::stream<Tcp::socket> stream(std::move(socket), ssl_context);
        stream.set_verify_mode(asio::ssl::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification(url.host));
        if (SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()) != 1) {
            throw std::runtime_error("failed to configure TLS server name");
        }
        auto& next_layer = stream.next_layer();
        await_operation(io, policy.connect_timeout,
                        [&](auto completion) {
                            stream.async_handshake(asio::ssl::stream_base::client, completion);
                        },
                        [&] { next_layer.cancel(); });
        return exchange(io, stream, next_layer, policy, request);
#else
        throw std::runtime_error("HTTPS networking requires TLS-enabled build");
#endif
    }
    return exchange(io, socket, socket, policy, request);
}

}  // namespace cppmcp::net
