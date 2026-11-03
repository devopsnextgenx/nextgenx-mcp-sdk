#include "cppmcp/client/oauth_provider.hpp"
#include "cppmcp/client/oauth_metadata.hpp"

#include <asio.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#else
#include <windows.h>
#include <shellapi.h>
#endif

namespace cppmcp::client {
namespace {

using Json = nlohmann::json;

struct ParsedUrl {
    std::string scheme;
    std::string authority;
    std::string host;
    std::string path;
    std::string port;
};

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

ParsedUrl parse_url(std::string_view value) {
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("OAuth endpoint must be an absolute HTTP(S) URL");
    }
    ParsedUrl result;
    result.scheme = lower(value.substr(0, scheme_end));
    if (result.scheme != "https" && result.scheme != "http") {
        throw std::invalid_argument("OAuth endpoint must use HTTPS");
    }
    auto rest = value.substr(scheme_end + 3);
    const auto authority_end = rest.find_first_of("/?#");
    const auto authority = rest.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        value.find('#') != std::string_view::npos) {
        throw std::invalid_argument("OAuth URL contains invalid authority or fragment");
    }
    result.authority.assign(authority);
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) throw std::invalid_argument("invalid IPv6 URL host");
        result.host.assign(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') throw std::invalid_argument("invalid URL port");
            result.port.assign(authority.substr(close + 2));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) result.host.assign(authority);
        else {
            result.host.assign(authority.substr(0, colon));
            result.port.assign(authority.substr(colon + 1));
        }
    }
    auto path = authority_end == std::string_view::npos
                    ? std::string_view("/")
                    : rest.substr(authority_end);
    if (path.find('?') != std::string_view::npos) {
        result.path.assign(path.substr(0, path.find('?')));
    } else {
        result.path.assign(path);
    }
    if (result.path.empty()) result.path = "/";
    return result;
}

bool loopback_host(std::string_view host) {
    return host == "localhost" || host == "::1" ||
           host.starts_with("127.");
}

void validate_endpoint_url(std::string_view url, bool allow_loopback_http) {
    const auto parsed = parse_url(url);
    if (parsed.scheme != "https" && !allow_loopback_http) {
        throw std::invalid_argument("OAuth endpoints must use HTTPS");
    }
}

std::string percent_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '*') {
            result.push_back(static_cast<char>(ch));
        } else if (ch == ' ') {
            result.push_back('+');
        } else {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0f]);
        }
    }
    return result;
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto high = hex_digit(value[i + 1]);
            const auto low = hex_digit(value[i + 2]);
            if (high < 0 || low < 0) throw std::invalid_argument("invalid URL encoding");
            result.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            result.push_back(value[i] == '+' ? ' ' : value[i]);
        }
    }
    return result;
}

std::map<std::string, std::string> parse_query(std::string_view url_or_target) {
    const auto question = url_or_target.find('?');
    if (question == std::string_view::npos) return {};
    auto query = url_or_target.substr(question + 1);
    const auto fragment = query.find('#');
    if (fragment != std::string_view::npos) query = query.substr(0, fragment);
    std::map<std::string, std::string> result;
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto equals = pair.find('=');
        const auto key = percent_decode(pair.substr(0, equals));
        const auto value = equals == std::string_view::npos
                               ? std::string{}
                               : percent_decode(pair.substr(equals + 1));
        if (!result.emplace(key, value).second) {
            throw std::invalid_argument("duplicate authorization response parameter");
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return result;
}

std::string append_query(std::string url,
                         const std::vector<std::pair<std::string, std::string>>& values) {
    url.push_back(url.find('?') == std::string::npos ? '?' : '&');
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) url.push_back('&');
        first = false;
        url += percent_encode(key);
        url.push_back('=');
        url += percent_encode(value);
    }
    return url;
}

std::string form_body(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string result;
    for (const auto& [key, value] : values) {
        if (!result.empty()) result.push_back('&');
        result += percent_encode(key);
        result.push_back('=');
        result += percent_encode(value);
    }
    return result;
}

std::string base64url(std::string_view bytes) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char ch : bytes) {
        buffer = (buffer << 8) | ch;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(alphabet[(buffer >> bits) & 63]);
        }
    }
    if (bits != 0) output.push_back(alphabet[(buffer << (6 - bits)) & 63]);
    return output;
}

std::string random_base64url(std::size_t bytes) {
    std::string random(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(random.data()),
                   static_cast<int>(random.size())) != 1) {
        throw std::runtime_error("cryptographic random generation failed");
    }
    return base64url(random);
}

std::string sha256_base64url(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("PKCE SHA-256 calculation failed");
    }
    return base64url(std::string_view(reinterpret_cast<const char*>(digest.data()), length));
}

auth::ScopeSet split_scopes(std::string_view value) {
    return auth::parse_scope_set(value);
}

std::string join_scopes(const auth::ScopeSet& scopes) {
    std::string result;
    for (const auto& scope : scopes) {
        if (!result.empty()) result.push_back(' ');
        result += scope;
    }
    return result;
}

std::string json_header(const net::HttpResponse& response, std::string_view name) {
    const auto found = response.headers.find(lower(name));
    return found == response.headers.end() ? std::string{} : found->second;
}

Json fetch_json(const std::string& url, const net::NetworkPolicy& policy) {
    const auto response = net::fetch_http(url, policy);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("OAuth metadata request failed with HTTP " +
                                 std::to_string(response.status));
    }
    return parse_oauth_metadata_document(response.body);
}

net::HttpResponse post_form(const std::string& url, const net::NetworkPolicy& policy,
                            const std::vector<std::pair<std::string, std::string>>& fields,
                            std::string authorization = {}) {
    std::map<std::string, std::string> headers{
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept", "application/json"}};
    if (!authorization.empty()) headers["Authorization"] = std::move(authorization);
    return net::fetch_http(url, policy, "POST", std::move(headers), form_body(fields));
}

std::string token_auth_header(std::string_view method,
                              const OAuthClientRegistration& client) {
    if (method != "client_secret_basic") return {};
    const std::string value = client.client_id + ":" + client.client_secret;
    std::string encoded(((value.size() + 2) / 3) * 4, '\0');
    const auto length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<int>(value.size()));
    encoded.resize(static_cast<std::size_t>(length));
    return "Basic " + encoded;
}

std::string make_issuer_metadata_url(std::string_view issuer,
                                     std::string_view well_known) {
    const auto parsed = parse_url(issuer);
    const auto suffix = std::string("/.well-known/") + std::string(well_known);
    if (parsed.path == "/") {
        return parsed.scheme + "://" + parsed.authority + suffix;
    }
    auto path = parsed.path;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (well_known == "openid-configuration") {
        return std::string(issuer) +
               (issuer.ends_with('/') ? "" : "/") + ".well-known/openid-configuration";
    }
    return parsed.scheme + "://" + parsed.authority + suffix + "/" + path;
}

std::vector<std::string> as_metadata_candidates(std::string_view issuer) {
    const auto parsed = parse_url(issuer);
    if (parsed.path == "/") {
        return {make_issuer_metadata_url(issuer, "oauth-authorization-server"),
                make_issuer_metadata_url(issuer, "openid-configuration")};
    }
    auto path = parsed.path;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    return {parsed.scheme + "://" + parsed.authority +
                "/.well-known/oauth-authorization-server/" + path,
            parsed.scheme + "://" + parsed.authority +
                "/.well-known/openid-configuration/" + path,
            make_issuer_metadata_url(issuer, "openid-configuration")};
}

std::string discovery_resource_uri(std::string_view resource_url,
                                   std::string_view path) {
    const auto parsed = parse_url(resource_url);
    return parsed.scheme + "://" + parsed.authority + std::string(path);
}

class LoopbackListener {
public:
    explicit LoopbackListener(unsigned short requested_port, std::string client_name)
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                                 requested_port)) {
        acceptor_.non_blocking(true);
        port_ = acceptor_.local_endpoint().port();
        redirect_uri_ = "http://127.0.0.1:" + std::to_string(port_) + "/callback";
        client_document_url_ =
            "http://127.0.0.1:" + std::to_string(port_) + "/client.json";
        document_ = {{"client_id", client_document_url_},
                     {"client_name", std::move(client_name)},
                     {"redirect_uris", Json::array({redirect_uri_})}};
        worker_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    }

    ~LoopbackListener() {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        asio::error_code error;
        acceptor_.close(error);
    }

    [[nodiscard]] const std::string& redirect_uri() const noexcept { return redirect_uri_; }
    [[nodiscard]] const std::string& client_document_url() const noexcept {
        return client_document_url_;
    }
    [[nodiscard]] unsigned short port() const noexcept { return port_; }

    std::map<std::string, std::string> wait(std::chrono::minutes timeout) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] { return callback_.has_value(); })) {
            throw UserCancelled("authorization callback timed out");
        }
        auto result = std::move(*callback_);
        callback_.reset();
        callback_armed_ = false;
        return result;
    }

    void arm_callback() {
        std::lock_guard lock(mutex_);
        callback_.reset();
        callback_armed_ = true;
    }

    void provide_redirect(std::string_view redirect) {
        const auto values = parse_query(redirect);
        std::lock_guard lock(mutex_);
        if (callback_armed_ && !callback_) {
            callback_ = values;
            cv_.notify_all();
        }
    }

private:
    void serve(std::stop_token stop) {
        while (!stop.stop_requested()) {
            asio::ip::tcp::socket socket(io_);
            asio::error_code error;
            acceptor_.accept(socket, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (error) {
                if (!stop.stop_requested()) {
                    std::lock_guard lock(mutex_);
                    server_error_ = error.message();
                    cv_.notify_all();
                }
                return;
            }
            handle(socket, stop);
        }
    }

    void handle(asio::ip::tcp::socket& socket, std::stop_token stop) {
        socket.non_blocking(true);
        std::string request;
        request.reserve(1024);
        std::array<char, 1024> buffer{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stop.stop_requested() &&
               request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 8192 &&
               std::chrono::steady_clock::now() < deadline) {
            asio::error_code error;
            const auto count = socket.read_some(asio::buffer(buffer), error);
            if (!error) {
                request.append(buffer.data(), count);
                continue;
            }
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return;
        }
        const auto line_end = request.find("\r\n");
        if (line_end == std::string::npos) return;
        const auto first_space = request.find(' ');
        const auto second_space = request.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos ||
            request.substr(0, first_space) != "GET") {
            respond(socket, 404, "Not found");
            return;
        }
        const auto target = request.substr(first_space + 1, second_space - first_space - 1);
        const auto query_start = target.find('?');
        const auto path = target.substr(0, query_start);
        if (path == "/client.json") {
            respond(socket, 200, document_.dump());
        } else if (path == "/callback") {
            try {
                provide_redirect(target);
                respond(socket, 200, "Authorization complete. You may close this window.");
            } catch (const std::exception&) {
                respond(socket, 400, "Invalid authorization response.");
            }
        } else {
            respond(socket, 404, "Not found");
        }
    }

    static void respond(asio::ip::tcp::socket& socket, int status,
                        std::string_view body, std::string_view extra = {}) {
        const std::string reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : "Not Found";
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
            "\r\nContent-Type: text/plain; charset=utf-8\r\n" + std::string(extra) +
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + std::string(body);
        asio::error_code error;
        asio::write(socket, asio::buffer(response), error);
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::jthread worker_;
    unsigned short port_ = 0;
    std::string redirect_uri_;
    std::string client_document_url_;
    Json document_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<std::map<std::string, std::string>> callback_;
    bool callback_armed_ = false;
    std::string server_error_;
};

std::string read_env_secret(std::string_view variable) {
    if (variable.empty()) return {};
    const auto* value = std::getenv(std::string(variable).c_str());
    return value == nullptr ? std::string{} : std::string(value);
}

std::string hex_encode(std::string_view bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) stream << std::setw(2) << unsigned(byte);
    return stream.str();
}

std::string hex_decode(std::string_view value) {
    if (value.size() % 2 != 0) throw std::runtime_error("invalid encrypted token store");
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const auto high = hex_digit(value[i]);
        const auto low = hex_digit(value[i + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("invalid encrypted token store");
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

std::string encryption_key(std::string_view environment_variable) {
    const auto value = read_env_secret(environment_variable);
    if (value.empty()) throw std::runtime_error("OAuth token-store encryption key is unavailable");
    if (value.size() != 64) {
        throw std::runtime_error("OAuth token-store encryption key must be 64 hexadecimal characters");
    }
    auto decoded = hex_decode(value);
    return decoded;
}

std::string encrypt_store(std::string_view plaintext, std::string_view key) {
    std::array<unsigned char, 12> nonce{};
    std::array<unsigned char, 16> tag{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw std::runtime_error("token-store nonce generation failed");
    }
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) throw std::runtime_error("token-store encryption setup failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw,
                                                                          EVP_CIPHER_CTX_free);
    std::string ciphertext(plaintext.size() + 16, '\0');
    int count = 0;
    int total = 0;
    if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.data()), nonce.data()) != 1 ||
        EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()),
            &count, reinterpret_cast<const unsigned char*>(plaintext.data()),
            static_cast<int>(plaintext.size())) != 1) {
        throw std::runtime_error("token-store encryption failed");
    }
    total = count;
    if (EVP_EncryptFinal_ex(context.get(),
            reinterpret_cast<unsigned char*>(ciphertext.data()) + total, &count) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(tag.size()), tag.data()) != 1) {
        throw std::runtime_error("token-store encryption failed");
    }
    total += count;
    ciphertext.resize(static_cast<std::size_t>(total));
    Json envelope{{"encrypted", true}, {"nonce", hex_encode(
        std::string_view(reinterpret_cast<const char*>(nonce.data()), nonce.size()))},
        {"tag", hex_encode(std::string_view(reinterpret_cast<const char*>(tag.data()), tag.size()))},
        {"ciphertext", hex_encode(ciphertext)}};
    OPENSSL_cleanse(ciphertext.data(), ciphertext.size());
    return envelope.dump();
}

std::string decrypt_store(const Json& envelope, std::string_view key) {
    const auto nonce = hex_decode(envelope.at("nonce").get<std::string>());
    const auto tag = hex_decode(envelope.at("tag").get<std::string>());
    const auto ciphertext = hex_decode(envelope.at("ciphertext").get<std::string>());
    if (nonce.size() != 12 || tag.size() != 16) {
        throw std::runtime_error("invalid encrypted token store");
    }
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) throw std::runtime_error("token-store decryption setup failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw,
                                                                          EVP_CIPHER_CTX_free);
    std::string plaintext(ciphertext.size() + 16, '\0');
    int count = 0;
    int total = 0;
    if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.data()),
            reinterpret_cast<const unsigned char*>(nonce.data())) != 1 ||
        EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()),
            &count, reinterpret_cast<const unsigned char*>(ciphertext.data()),
            static_cast<int>(ciphertext.size())) != 1) {
        throw std::runtime_error("token-store decryption failed");
    }
    total = count;
    auto mutable_tag = tag;
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(mutable_tag.size()), mutable_tag.data()) != 1 ||
        EVP_DecryptFinal_ex(context.get(),
            reinterpret_cast<unsigned char*>(plaintext.data()) + total, &count) != 1) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        throw std::runtime_error("OAuth token-store authentication failed");
    }
    total += count;
    plaintext.resize(static_cast<std::size_t>(total));
    return plaintext;
}

}  // namespace

std::optional<OAuthToken> MemoryTokenStore::get(const OAuthTokenKey& key) const {
    std::lock_guard lock(mutex_);
    const auto found = tokens_.find(key);
    return found == tokens_.end() ? std::nullopt : std::optional<OAuthToken>(found->second);
}

void MemoryTokenStore::put(const OAuthTokenKey& key, const OAuthToken& token) {
    std::lock_guard lock(mutex_);
    tokens_.insert_or_assign(key, token);
}

void MemoryTokenStore::erase(const OAuthTokenKey& key) {
    std::lock_guard lock(mutex_);
    tokens_.erase(key);
}

std::optional<OAuthClientRegistration> MemoryTokenStore::get_registration(
    std::string_view issuer) const {
    std::lock_guard lock(mutex_);
    const auto found = registrations_.find(std::string(issuer));
    return found == registrations_.end()
               ? std::nullopt
               : std::optional<OAuthClientRegistration>(found->second);
}

void MemoryTokenStore::put_registration(std::string issuer,
                                        OAuthClientRegistration registration) {
    std::lock_guard lock(mutex_);
    registrations_.insert_or_assign(std::move(issuer), std::move(registration));
}

FileTokenStore::FileTokenStore(std::string path, std::string encryption_key_env)
    : path_(std::move(path)), encryption_key_env_(std::move(encryption_key_env)) {
    if (path_.empty()) throw std::invalid_argument("token-store path must not be empty");
    load();
}

std::optional<OAuthToken> FileTokenStore::get(const OAuthTokenKey& key) const {
    std::lock_guard lock(mutex_);
    const auto found = tokens_.find(key);
    return found == tokens_.end() ? std::nullopt : std::optional<OAuthToken>(found->second);
}

void FileTokenStore::put(const OAuthTokenKey& key, const OAuthToken& token) {
    std::lock_guard lock(mutex_);
    tokens_.insert_or_assign(key, token);
    save_locked();
}

void FileTokenStore::erase(const OAuthTokenKey& key) {
    std::lock_guard lock(mutex_);
    tokens_.erase(key);
    save_locked();
}

std::optional<OAuthClientRegistration> FileTokenStore::get_registration(
    std::string_view issuer) const {
    std::lock_guard lock(mutex_);
    const auto found = registrations_.find(std::string(issuer));
    return found == registrations_.end()
               ? std::nullopt
               : std::optional<OAuthClientRegistration>(found->second);
}

void FileTokenStore::put_registration(std::string issuer,
                                      OAuthClientRegistration registration) {
    std::lock_guard lock(mutex_);
    registrations_.insert_or_assign(std::move(issuer), std::move(registration));
    save_locked();
}

void FileTokenStore::load() {
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (std::filesystem::exists(path_)) {
            throw std::runtime_error("unable to read OAuth token store");
        }
        return;
    }
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    Json root = Json::parse(contents);
    if (root.value("encrypted", false)) {
        if (encryption_key_env_.empty()) {
            throw std::runtime_error("encrypted OAuth token store requires its environment key");
        }
        contents = decrypt_store(root, encryption_key(encryption_key_env_));
        root = Json::parse(contents);
        OPENSSL_cleanse(contents.data(), contents.size());
    }
    if (!root.is_object() || root.value("version", 0) != 1) {
        throw std::runtime_error("unsupported OAuth token store format");
    }
    for (const auto& item : root.value("tokens", Json::array())) {
        OAuthToken token;
        token.access_token = item.at("access_token").get<std::string>();
        token.token_type = item.value("token_type", std::string("Bearer"));
        token.refresh_token = item.value("refresh_token", std::string{});
        token.scopes = split_scopes(item.value("scope", std::string{}));
        token.expires_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(item.value("expires_at", std::int64_t{})));
        OAuthTokenKey key{item.at("issuer").get<std::string>(),
                          item.at("resource").get<std::string>(),
                          item.at("client_id").get<std::string>()};
        tokens_.insert_or_assign(std::move(key), std::move(token));
    }
    for (const auto& item : root.value("registrations", Json::array())) {
        OAuthClientRegistration registration;
        registration.client_id = item.at("client_id").get<std::string>();
        registration.client_secret = item.value("client_secret", std::string{});
        registration.token_endpoint_auth_method =
            item.value("token_endpoint_auth_method", std::string("none"));
        registrations_.insert_or_assign(item.at("issuer").get<std::string>(),
                                        std::move(registration));
    }
#ifndef _WIN32
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("unable to set OAuth token store permissions");
    }
#endif
}

void FileTokenStore::save_locked() const {
    Json root = {{"version", 1}, {"tokens", Json::array()},
                 {"registrations", Json::array()}};
    for (const auto& [key, token] : tokens_) {
        const auto expires =
            std::chrono::duration_cast<std::chrono::seconds>(
                token.expires_at.time_since_epoch()).count();
        root["tokens"].push_back({
            {"issuer", key.issuer}, {"resource", key.resource}, {"client_id", key.client_id},
            {"access_token", token.access_token}, {"token_type", token.token_type},
            {"refresh_token", token.refresh_token}, {"scope", join_scopes(token.scopes)},
            {"expires_at", expires}});
    }
    for (const auto& [issuer, registration] : registrations_) {
        root["registrations"].push_back({
            {"issuer", issuer}, {"client_id", registration.client_id},
            {"client_secret", registration.client_secret},
            {"token_endpoint_auth_method", registration.token_endpoint_auth_method}});
    }
    std::string contents = root.dump();
    if (!encryption_key_env_.empty()) {
        contents = encrypt_store(contents, encryption_key(encryption_key_env_));
    }
#ifndef _WIN32
    const auto temporary = path_ + ".tmp." + std::to_string(::getpid()) + "." +
                           random_base64url(6);
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) throw std::runtime_error("unable to create temporary OAuth token store");
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto amount = ::write(fd, contents.data() + written, contents.size() - written);
        if (amount < 0) {
            const int error = errno;
            ::close(fd);
            ::unlink(temporary.c_str());
            throw std::runtime_error("unable to write OAuth token store: " +
                                     std::string(std::strerror(error)));
        }
        written += static_cast<std::size_t>(amount);
    }
    int save_error = 0;
    if (::fsync(fd) != 0) save_error = errno;
    if (::close(fd) != 0 && save_error == 0) save_error = errno;
    if (save_error == 0 && ::rename(temporary.c_str(), path_.c_str()) != 0) {
        save_error = errno;
    }
    if (save_error != 0) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("unable to atomically save OAuth token store: " +
                                 std::string(std::strerror(save_error)));
    }
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("unable to secure OAuth token store permissions");
    }
#else
    const auto temporary = path_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("unable to create temporary OAuth token store");
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) throw std::runtime_error("unable to write OAuth token store");
    }
    std::filesystem::rename(temporary, path_);
#endif
    OPENSSL_cleanse(contents.data(), contents.size());
}

void ClientRegistrationStore::put(std::string issuer,
                                  OAuthClientRegistration registration) {
    std::lock_guard lock(mutex_);
    entries_.insert_or_assign(std::move(issuer), std::move(registration));
}

std::optional<OAuthClientRegistration> ClientRegistrationStore::get(
    std::string_view issuer) const {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(std::string(issuer));
    return found == entries_.end()
               ? std::nullopt
               : std::optional<OAuthClientRegistration>(found->second);
}

OAuthError::OAuthError(std::string reason, std::string message)
    : std::runtime_error(std::move(message)), reason_(std::move(reason)) {}

const std::string& OAuthError::reason() const noexcept { return reason_; }

struct OAuthProvider::Impl {
    explicit Impl(OAuthProviderOptions source) : options(std::move(source)) {
        if (options.resource_url.empty()) {
            throw std::invalid_argument("OAuth resource URL must not be empty");
        }
        if (options.resource_url.find('#') != std::string::npos) {
            throw std::invalid_argument("OAuth resource URL must not contain a fragment");
        }
        if (options.step_up_limit == 0) {
            throw std::invalid_argument("OAuth step-up limit must be positive");
        }
        auth::validate_scope_set(options.requested_scopes);
        auto resource = parse_url(options.resource_url);
        if (resource.scheme != "https" &&
            !(options.allow_insecure_loopback && loopback_host(resource.host))) {
            throw std::invalid_argument("MCP resource URL must use HTTPS");
        }
        options.network_policy.allow_http_loopback =
            options.network_policy.allow_http_loopback || options.allow_insecure_loopback;
        options.network_policy.require_https = true;
        if (!options.token_store) options.token_store = std::make_shared<MemoryTokenStore>();
        if (!options.registrations) {
            options.registrations = std::make_shared<ClientRegistrationStore>();
        }
        if (options.grant_type != "authorization_code" &&
            options.grant_type != "client_credentials") {
            throw std::invalid_argument("OAuth grant type must be authorization_code or client_credentials");
        }
    }

    OAuthProviderOptions options;
    std::mutex mutex;
    std::optional<std::string> issuer;
    Json prm;
    Json metadata;
    std::string metadata_url;
    OAuthClientRegistration registration;
    bool registration_ready = false;
    OAuthTokenKey token_key;
    std::optional<OAuthToken> token;
    std::unique_ptr<LoopbackListener> callback_listener;
    auth::ScopeSet previously_requested;
    std::map<std::string, std::size_t> step_up_attempts;
    std::optional<std::string> bound_pre_registered_issuer;

    void emit(std::string text) const {
        if (options.event_handler) options.event_handler(text);
    }

    [[noreturn]] void fail(std::string reason, std::string message) const {
        throw OAuthError(std::move(reason), std::move(message));
    }

    Json fetch_resource_metadata(const AuthChallenge& challenge) {
        const std::string path_inserted =
            discovery_resource_uri(options.resource_url,
                "/.well-known/oauth-protected-resource" + parse_url(options.resource_url).path);
        std::vector<std::string> candidates;
        if (!challenge.resource_metadata.empty()) {
            candidates.push_back(challenge.resource_metadata);
        } else {
            candidates.push_back(path_inserted);
            candidates.push_back(discovery_resource_uri(
                options.resource_url, "/.well-known/oauth-protected-resource"));
        }
        for (const auto& url : candidates) {
            emit("AUTH discovery prm " + url);
            try {
                auto result = fetch_json(url, options.network_policy);
                if (result.value("resource", std::string{}) != options.resource_url) {
                    fail("resource_mismatch", "Protected Resource Metadata resource mismatch");
                }
                const auto servers = result.find("authorization_servers");
                if (servers == result.end() || !servers->is_array() || servers->empty()) {
                    fail("prm_missing", "Protected Resource Metadata has no authorization server");
                }
                return result;
            } catch (const net::BlockedAddress&) {
                throw;
            } catch (const OAuthError&) {
                throw;
            } catch (const std::exception&) {
                if (challenge.resource_metadata.size()) {
                    fail("prm_missing", "Protected Resource Metadata could not be fetched");
                }
            }
        }
        fail("prm_missing", "Protected Resource Metadata could not be discovered");
    }

    Json discover_as(std::string_view as_issuer) {
        for (const auto& url : as_metadata_candidates(as_issuer)) {
            try {
                const auto result = fetch_json(url, options.network_policy);
                if (result.value("issuer", std::string{}) != as_issuer) {
                    fail("issuer_mismatch", "authorization-server metadata issuer mismatch");
                }
                return result;
            } catch (const net::BlockedAddress&) {
                throw;
            } catch (const OAuthError&) {
                throw;
            } catch (const std::exception&) {
                continue;
            }
        }
        fail("issuer_mismatch", "authorization-server metadata could not be discovered");
    }

    void ensure_discovery(const AuthChallenge& challenge) {
        if (issuer) return;
        prm = fetch_resource_metadata(challenge);
        const auto& servers = prm.at("authorization_servers");
        std::vector<std::string> available_issuers;
        for (const auto& server : servers) {
            if (!server.is_string()) {
                fail("prm_missing", "authorization server issuer is invalid");
            }
            available_issuers.push_back(server.get<std::string>());
        }
        const auto selected_issuer =
            options.authorization_server_selector
                ? options.authorization_server_selector(available_issuers)
                : available_issuers.front();
        if (std::find(available_issuers.begin(), available_issuers.end(),
                      selected_issuer) == available_issuers.end()) {
            fail("prm_missing", "authorization server selection is not in PRM metadata");
        }
        validate_endpoint_url(selected_issuer, options.allow_insecure_loopback);
        auto discovered_metadata = discover_as(selected_issuer);
        const auto authorization =
            discovered_metadata.value("authorization_endpoint", std::string{});
        const auto token_endpoint =
            discovered_metadata.value("token_endpoint", std::string{});
        if (authorization.empty() || token_endpoint.empty()) {
            fail("issuer_mismatch", "authorization-server metadata lacks required endpoints");
        }
        validate_endpoint_url(authorization, options.allow_insecure_loopback);
        validate_endpoint_url(token_endpoint, options.allow_insecure_loopback);
        if (options.grant_type == "authorization_code") {
            const auto methods = discovered_metadata.find("code_challenge_methods_supported");
            if (methods == discovered_metadata.end() || !methods->is_array() ||
                std::find(methods->begin(), methods->end(), Json("S256")) == methods->end()) {
                throw PkceUnsupported();
            }
        }
        issuer = selected_issuer;
        metadata = std::move(discovered_metadata);
        metadata_url = token_endpoint;
        emit("AUTH as " + *issuer);
    }

    std::string resource_scope_fallback() const {
        const auto found = prm.find("scopes_supported");
        if (found != prm.end() && found->is_array()) {
            auth::ScopeSet scopes;
            for (const auto& item : *found) {
                if (!item.is_string()) fail("prm_missing", "PRM scopes_supported is malformed");
                scopes.insert(item.get<std::string>());
            }
            auth::validate_scope_set(scopes);
            return join_scopes(scopes);
        }
        return {};
    }

    OAuthClientRegistration register_client(LoopbackListener* listener) {
        if (const auto existing = options.registrations->get(*issuer)) {
            emit("AUTH registration prereg");
            return *existing;
        }
        if (const auto existing = options.token_store->get_registration(*issuer)) {
            emit("AUTH registration prereg");
            options.registrations->put(*issuer, *existing);
            return *existing;
        }
        if (!options.client_id.empty()) {
            if (bound_pre_registered_issuer && *bound_pre_registered_issuer != *issuer) {
                fail("issuer_mismatch", "pre-registered client credentials are bound to another issuer");
            }
            bound_pre_registered_issuer = *issuer;
            OAuthClientRegistration result{options.client_id, options.client_secret,
                options.client_secret_auth_method.empty()
                    ? (options.client_secret.empty() ? "none" : "client_secret_basic")
                    : options.client_secret_auth_method};
            options.registrations->put(*issuer, result);
            options.token_store->put_registration(*issuer, result);
            emit("AUTH registration prereg");
            return result;
        }
        if (options.grant_type == "client_credentials") {
            throw NoRegistrationMethod(
                "client_credentials requires issuer-bound pre-registered credentials");
        }
        if (metadata.value("client_id_metadata_document_supported", false)) {
            std::string client_id = options.cimd_url;
            if (client_id.empty()) {
                if (!listener || !options.allow_insecure_loopback) {
                    throw NoRegistrationMethod("CIMD requires an HTTPS client metadata URL");
                }
                client_id = listener->client_document_url();
            }
            validate_endpoint_url(client_id, options.allow_insecure_loopback);
            if (parse_url(client_id).scheme != "https" &&
                !loopback_host(parse_url(client_id).host)) {
                throw NoRegistrationMethod(
                    "test-only HTTP client metadata documents must use loopback");
            }
            OAuthClientRegistration result{client_id, {}, "none"};
            options.registrations->put(*issuer, result);
            options.token_store->put_registration(*issuer, result);
            emit("AUTH registration cimd");
            return result;
        }
        const auto endpoint = metadata.value("registration_endpoint", std::string{});
        if (options.enable_dcr && !endpoint.empty()) {
            validate_endpoint_url(endpoint, options.allow_insecure_loopback);
            if (!listener) throw NoRegistrationMethod("DCR requires a loopback redirect listener");
            const Json redirects = Json::array({listener->redirect_uri()});
            const auto response = post_form(endpoint, options.network_policy,
                {{"client_name", options.client_name}, {"application_type", "native"},
                 {"redirect_uris", redirects.dump()}, {"response_types", "[\"code\"]"},
                 {"grant_types", "[\"authorization_code\",\"refresh_token\"]"}});
            if (response.status < 200 || response.status >= 300) {
                fail("no_registration", "dynamic client registration failed");
            }
            const auto result_json = Json::parse(response.body);
            OAuthClientRegistration result{
                result_json.value("client_id", std::string{}),
                result_json.value("client_secret", std::string{}),
                result_json.value("token_endpoint_auth_method", std::string("none"))};
            if (result.client_id.empty()) fail("no_registration", "DCR response has no client_id");
            options.registrations->put(*issuer, result);
            options.token_store->put_registration(*issuer, result);
            emit("AUTH registration dcr");
            return result;
        }
        if (options.registration_prompt) {
            auto result = options.registration_prompt->prompt(*issuer);
            if (result.client_id.empty()) throw NoRegistrationMethod("registration prompt returned no client_id");
            options.registrations->put(*issuer, result);
            options.token_store->put_registration(*issuer, result);
            emit("AUTH registration prereg");
            return result;
        }
        throw NoRegistrationMethod();
    }

    OAuthTokenKey prepare_registration_and_key(LoopbackListener* listener) {
        registration = register_client(listener);
        registration_ready = true;
        token_key = {*issuer, options.resource_url, registration.client_id};
        if (auto saved = options.token_store->get(token_key)) token = std::move(saved);
        return token_key;
    }

    OAuthToken parse_token_response(const net::HttpResponse& response,
                                    const auth::ScopeSet& requested) {
        if (response.status < 200 || response.status >= 300) {
            const auto body = Json::parse(response.body, nullptr, false);
            if (body.is_object() && body.value("error", std::string{}) == "invalid_grant") {
                token.reset();
                options.token_store->erase(token_key);
                return {};
            }
            throw std::runtime_error("OAuth token endpoint failed with HTTP " +
                                     std::to_string(response.status));
        }
        const auto value = Json::parse(response.body);
        OAuthToken result;
        result.access_token = value.value("access_token", std::string{});
        result.token_type = value.value("token_type", std::string("Bearer"));
        result.refresh_token = value.value("refresh_token", std::string{});
        result.scopes = value.contains("scope") && value["scope"].is_string()
                            ? split_scopes(value["scope"].get<std::string>())
                            : requested;
        const auto lifetime = value.value("expires_in", 3600);
        if (!value.contains("access_token") || result.access_token.empty() ||
            lower(result.token_type) != "bearer" || lifetime <= 0) {
            throw std::runtime_error("OAuth token response is malformed");
        }
        result.expires_at = std::chrono::system_clock::now() +
                            std::chrono::seconds(lifetime);
        options.token_store->put(token_key, result);
        token = result;
        emit("AUTH token granted_scope=" + join_scopes(result.scopes));
        return result;
    }

    bool refresh_token() {
        if (!token || token->refresh_token.empty()) return false;
        auto fields = std::vector<std::pair<std::string, std::string>>{
            {"grant_type", "refresh_token"},
            {"refresh_token", token->refresh_token},
            {"client_id", registration.client_id},
            {"resource", options.resource_url}};
        const auto auth_method = registration.token_endpoint_auth_method;
        std::string authorization;
        if (auth_method == "client_secret_basic") {
            authorization = token_auth_header(auth_method, registration);
        } else if (auth_method == "client_secret_post") {
            fields.emplace_back("client_secret", registration.client_secret);
        } else if (auth_method != "none") {
            throw std::runtime_error("unsupported token endpoint client authentication method");
        }
        const auto response = post_form(metadata_url, options.network_policy, fields,
                                        std::move(authorization));
        auto refreshed = parse_token_response(response, previously_requested);
        if (refreshed.access_token.empty()) return false;
        emit("AUTH refresh");
        return true;
    }

    std::string authorize_url(const auth::ScopeSet& scopes, std::string_view redirect,
                              std::string_view state, std::string_view challenge) const {
        std::vector<std::pair<std::string, std::string>> parameters{
            {"response_type", "code"}, {"client_id", registration.client_id},
            {"redirect_uri", std::string(redirect)}, {"state", std::string(state)},
            {"code_challenge", std::string(challenge)},
            {"code_challenge_method", "S256"}, {"resource", options.resource_url}};
        if (!scopes.empty()) parameters.emplace_back("scope", join_scopes(scopes));
        return append_query(metadata.at("authorization_endpoint").get<std::string>(),
                            parameters);
    }

    void launch_authorization(const std::string& url, LoopbackListener& listener) {
        if (options.launcher == "http-follow") {
            const auto response = net::fetch_http(url, options.network_policy);
            if (response.status < 300 || response.status >= 400) {
                fail("user_cancelled", "authorization server did not redirect");
            }
            const auto location = json_header(response, "Location");
            const auto callback_prefix = listener.redirect_uri() + "?";
            if (!location.starts_with(callback_prefix)) {
                fail("user_cancelled", "authorization server returned an invalid callback URL");
            }
            const auto callback_response = net::fetch_http(location, options.network_policy);
            if (callback_response.status != 200) {
                fail("user_cancelled", "authorization callback failed");
            }
            return;
        }
        if (options.launcher == "print") {
            std::cout << url << std::endl;
            std::string redirected;
            if (!std::getline(std::cin, redirected) || redirected.empty()) {
                throw UserCancelled("authorization URL was not provided");
            }
            listener.provide_redirect(redirected);
            return;
        }
        if (options.user_agent_launcher) {
            options.user_agent_launcher->launch(url);
            return;
        }
#ifdef _WIN32
        const auto result = reinterpret_cast<std::intptr_t>(
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) throw UserCancelled("unable to launch the system browser");
#elif __APPLE__
        const char* executable = "open";
        char* const arguments[] = {const_cast<char*>(executable),
                                   const_cast<char*>(url.c_str()), nullptr};
#else
        const char* executable = "xdg-open";
        char* const arguments[] = {const_cast<char*>(executable),
                                   const_cast<char*>(url.c_str()), nullptr};
#endif
#ifndef _WIN32
        pid_t process = 0;
        const int spawn_error = posix_spawnp(&process, executable, nullptr, nullptr,
                                            arguments, environ);
        if (spawn_error != 0) throw UserCancelled("unable to launch the system browser");
        int status = 0;
        if (::waitpid(process, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            throw UserCancelled("unable to launch the system browser");
        }
#endif
    }

    void authorization_code(const auth::ScopeSet& scopes) {
        if (!callback_listener) {
            callback_listener = std::make_unique<LoopbackListener>(options.redirect_port,
                                                                   options.client_name);
        }
        auto& listener = *callback_listener;
        if (!registration_ready) prepare_registration_and_key(&listener);
        previously_requested = scopes;
        const auto state = random_base64url(32);
        const auto verifier = random_base64url(32);
        if (verifier.size() < 43 || verifier.size() > 128) {
            throw std::runtime_error("generated PKCE verifier has invalid length");
        }
        const auto challenge = sha256_base64url(verifier);
        const auto url = authorize_url(scopes, listener.redirect_uri(), state, challenge);
        listener.arm_callback();
        emit("AUTH authorize scope=" + join_scopes(scopes) +
             " resource=" + options.resource_url);
        launch_authorization(url, listener);
        const auto response = listener.wait(std::chrono::minutes(5));
        const auto returned_state = response.find("state");
        if (returned_state == response.end() || returned_state->second != state) {
            throw StateMismatch();
        }
        const auto advertised =
            metadata.value("authorization_response_iss_parameter_supported", false);
        const auto returned_issuer = response.find("iss");
        if (returned_issuer == response.end()) {
            if (advertised) throw IssMissing();
        } else if (returned_issuer->second != *issuer) {
            throw IssMismatch();
        }
        const auto error = response.find("error");
        if (error != response.end()) {
            throw UserCancelled("authorization request was denied");
        }
        const auto code = response.find("code");
        if (code == response.end() || code->second.empty()) {
            throw UserCancelled("authorization response has no code");
        }
        auto fields = std::vector<std::pair<std::string, std::string>>{
            {"grant_type", "authorization_code"}, {"code", code->second},
            {"redirect_uri", listener.redirect_uri()}, {"code_verifier", verifier},
            {"client_id", registration.client_id}, {"resource", options.resource_url}};
        std::string authorization;
        const auto auth_method = registration.token_endpoint_auth_method;
        if (auth_method == "client_secret_basic") {
            authorization = token_auth_header(auth_method, registration);
        } else if (auth_method == "client_secret_post") {
            fields.emplace_back("client_secret", registration.client_secret);
        } else if (auth_method != "none") {
            throw std::runtime_error("unsupported token endpoint client authentication method");
        }
        const auto token_response = post_form(metadata_url, options.network_policy, fields,
                                              std::move(authorization));
        const auto issued = parse_token_response(token_response, scopes);
        if (issued.access_token.empty()) {
            throw std::runtime_error("authorization code grant returned no access token");
        }
    }

    void client_credentials(const auth::ScopeSet& scopes) {
        if (!registration_ready) prepare_registration_and_key(nullptr);
        previously_requested = scopes;
        auto fields = std::vector<std::pair<std::string, std::string>>{
            {"grant_type", "client_credentials"}, {"client_id", registration.client_id},
            {"resource", options.resource_url}};
        if (!scopes.empty()) fields.emplace_back("scope", join_scopes(scopes));
        std::string authorization;
        const auto auth_method = registration.token_endpoint_auth_method;
        if (auth_method == "client_secret_basic") {
            authorization = token_auth_header(auth_method, registration);
        } else if (auth_method == "client_secret_post") {
            fields.emplace_back("client_secret", registration.client_secret);
        } else if (auth_method != "none") {
            throw std::runtime_error("unsupported token endpoint client authentication method");
        }
        const auto response = post_form(metadata_url, options.network_policy, fields,
                                        std::move(authorization));
        const auto issued = parse_token_response(response, scopes);
        if (issued.access_token.empty()) {
            throw std::runtime_error("client_credentials grant returned no access token");
        }
    }

    auth::ScopeSet challenged_scopes(const AuthChallenge& challenge) const {
        if (!challenge.scope.empty()) return split_scopes(challenge.scope);
        const auto fallback = resource_scope_fallback();
        if (!fallback.empty()) return split_scopes(fallback);
        return {};
    }

    void authorize(const auth::ScopeSet& scopes) {
        if (options.grant_type == "client_credentials") client_credentials(scopes);
        else authorization_code(scopes);
    }

    void authorize_or_refresh(const AuthChallenge& challenge) {
        ensure_discovery(challenge);
        if (!registration_ready) prepare_registration_and_key(nullptr);
        if (token) {
            const auto now = std::chrono::system_clock::now();
            if (token->expires_at > now + std::chrono::seconds(30)) return;
            if (!token->refresh_token.empty() && refresh_token()) return;
        }
        auto scopes = challenged_scopes(challenge);
        authorize(scopes);
    }

    void apply(transport::Headers& headers) {
        std::lock_guard lock(mutex);
        if (!issuer || !registration_ready || !token) return;
        const auto now = std::chrono::system_clock::now();
        if (token->expires_at <= now + std::chrono::seconds(30)) {
            if (token->refresh_token.empty() || !refresh_token()) {
                token.reset();
                options.token_store->erase(token_key);
                return;
            }
        }
        headers.emplace_back("Authorization", token->token_type + " " + token->access_token);
    }

    AuthAction on_challenge(const AuthChallenge& challenge) {
        std::lock_guard lock(mutex);
        try {
            ensure_discovery(challenge);
            if (!registration_ready) {
                const bool has_registration =
                    !options.client_id.empty() ||
                    options.registrations->get(*issuer).has_value() ||
                    options.token_store->get_registration(*issuer).has_value();
                if (!has_registration && options.grant_type == "authorization_code") {
                    authorize(challenged_scopes(challenge));
                    return AuthAction::retry;
                }
                prepare_registration_and_key(nullptr);
            }
            if (token) {
                const auto now = std::chrono::system_clock::now();
                if (token->expires_at <= now + std::chrono::seconds(30) &&
                    !token->refresh_token.empty() && refresh_token()) {
                    return AuthAction::retry;
                }
            }
            auto desired = challenged_scopes(challenge);
            if (challenge.error == "insufficient_scope") {
                desired.insert(previously_requested.begin(), previously_requested.end());
                const auto operation = challenge.operation.empty()
                                           ? options.resource_url
                                           : challenge.operation;
                const bool new_scope = !token ||
                    !std::includes(token->scopes.begin(), token->scopes.end(),
                                   desired.begin(), desired.end());
                if (!new_scope || ++step_up_attempts[operation] >= options.step_up_limit) {
                    emit("AUTH failed stepup_limit");
                    throw StepUpLimitExceeded();
                }
                previously_requested = desired;
                emit("AUTH stepup scope=" + join_scopes(desired));
            } else if (challenge.error == "invalid_token" && token &&
                       !token->refresh_token.empty()) {
                if (refresh_token()) return AuthAction::retry;
                desired = previously_requested.empty() ? desired : previously_requested;
            } else if (token && token->expires_at > std::chrono::system_clock::now() &&
                       challenge.error.empty()) {
                return AuthAction::retry;
            }
            authorize(desired);
            return AuthAction::retry;
        } catch (const net::BlockedAddress& error) {
            emit("AUTH failed blocked_address");
            throw OAuthError("blocked_address", error.what());
        } catch (const OAuthError& error) {
            if (error.reason() != "stepup_limit") emit("AUTH failed " + error.reason());
            throw;
        } catch (const std::exception& error) {
            emit("AUTH failed auth_required");
            throw OAuthError("auth_required", error.what());
        }
    }
};

OAuthProvider::OAuthProvider(OAuthProviderOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

OAuthProvider::~OAuthProvider() = default;

void OAuthProvider::apply(transport::Headers& headers) {
    impl_->apply(headers);
}

AuthAction OAuthProvider::on_challenge(const AuthChallenge& challenge) {
    return impl_->on_challenge(challenge);
}

}  // namespace cppmcp::client
