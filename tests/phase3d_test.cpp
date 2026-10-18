#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#if CPPMCP_WITH_TLS
#include <asio/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#endif

#include "cppmcp/server.hpp"
#include "cppmcp/transport/http.hpp"
#include "cppmcp/transport/http_client.hpp"

#ifndef CPPMCP_WITH_TLS
#define CPPMCP_WITH_TLS 0
#endif

namespace {

using cppmcp::transport::SseEvent;
using cppmcp::transport::SseParser;
using cppmcp::transport::SubscriptionFilter;
using cppmcp::transport::SubscriptionReadStatus;
using cppmcp::transport::SubscriptionRegistry;

#if CPPMCP_WITH_TLS
class TestTlsCertificate {
public:
    explicit TestTlsCertificate(bool expired = false) {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        directory_ = std::filesystem::temp_directory_path() / ("cppmcp-tls-" + suffix);
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("could not create temporary TLS test directory");
        }
        cert_path_ = directory_ / "server.pem";
        key_path_ = directory_ / "server.key";
        create_certificate(expired);
    }

    ~TestTlsCertificate() {
        std::error_code ignored;
        std::filesystem::remove(cert_path_, ignored);
        std::filesystem::remove(key_path_, ignored);
        std::filesystem::remove(directory_, ignored);
    }

    std::string cert_path() const { return cert_path_.string(); }
    std::string key_path() const { return key_path_.string(); }

private:
    struct FileCloser {
        void operator()(FILE* file) const {
            if (file != nullptr) {
                std::fclose(file);
            }
        }
    };

    static void add_extension(X509* certificate, X509V3_CTX& context,
                              int nid, const char* value) {
        auto* extension = X509V3_EXT_conf_nid(
            nullptr, &context, nid, const_cast<char*>(value));
        if (extension == nullptr || X509_add_ext(certificate, extension, -1) != 1) {
            X509_EXTENSION_free(extension);
            throw std::runtime_error("could not add TLS test certificate extension");
        }
        X509_EXTENSION_free(extension);
    }

    void create_certificate(bool expired) {
        using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
        using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
        using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
        PkeyContext key_context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                               EVP_PKEY_CTX_free);
        EVP_PKEY* raw_key = nullptr;
        if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0 ||
            EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0) {
            throw std::runtime_error("could not generate TLS test private key");
        }
        Pkey key(raw_key, EVP_PKEY_free);
        Certificate certificate(X509_new(), X509_free);
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("could not create TLS test certificate");
        }
        const long not_before_offset = expired ? -3L * 24L * 60L * 60L : -60L;
        const long not_after_offset = expired ? -24L * 60L * 60L : 30L * 24L * 60L * 60L;
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), not_before_offset);
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), not_after_offset);
        X509_NAME* subject = X509_get_subject_name(certificate.get());
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"),
                                   -1, -1, 0);
        X509_set_issuer_name(certificate.get(), subject);
        X509V3_CTX extension_context;
        X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(),
                       nullptr, nullptr, 0);
        add_extension(certificate.get(), extension_context, NID_basic_constraints,
                      "critical,CA:TRUE");
        add_extension(certificate.get(), extension_context, NID_key_usage,
                      "critical,digitalSignature,keyEncipherment,keyCertSign");
        add_extension(certificate.get(), extension_context, NID_ext_key_usage, "serverAuth");
        add_extension(certificate.get(), extension_context, NID_subject_alt_name,
                      "DNS:localhost");
        if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
            throw std::runtime_error("could not sign TLS test certificate");
        }
        std::unique_ptr<FILE, FileCloser> cert_file(std::fopen(cert_path_.c_str(), "wb"));
        std::unique_ptr<FILE, FileCloser> key_file(std::fopen(key_path_.c_str(), "wb"));
        if (!cert_file || !key_file ||
            PEM_write_X509(cert_file.get(), certificate.get()) != 1 ||
            PEM_write_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0,
                                 nullptr, nullptr) != 1) {
            throw std::runtime_error("could not write TLS test certificate");
        }
    }

    std::filesystem::path directory_;
    std::filesystem::path cert_path_;
    std::filesystem::path key_path_;
};

class TestTlsServer {
public:
    explicit TestTlsServer(const TestTlsCertificate& certificate)
        : acceptor_(context_, {asio::ip::make_address("127.0.0.1"), 0}),
          tls_context_(asio::ssl::context::tls_server) {
        tls_context_.use_certificate_chain_file(certificate.cert_path());
        tls_context_.use_private_key_file(certificate.key_path(), asio::ssl::context::pem);
    }

    ~TestTlsServer() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    unsigned short port() const { return acceptor_.local_endpoint().port(); }

    template <typename Handler>
    void start(Handler handler) {
        worker_ = std::thread([this, handler = std::move(handler)]() mutable {
            asio::ip::tcp::socket socket(context_);
            asio::error_code error;
            acceptor_.accept(socket, error);
            if (error) {
                return;
            }
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), tls_context_);
            stream.handshake(asio::ssl::stream_base::server, error);
            if (!error) {
                handler(stream);
            }
        });
    }

private:
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ssl::context tls_context_;
    std::thread worker_;
};

std::string attempt_https_request(const std::string& url, const std::string& ca_file) {
    cppmcp::HttpClientOptions options;
    options.url = url;
    options.ca_file = ca_file;
    options.connect_timeout = std::chrono::seconds(3);
    options.request_timeout = std::chrono::seconds(3);
    cppmcp::transport::HttpClientTransport transport(std::move(options));
    try {
        transport.start();
    } catch (const std::exception& error) {
        return error.what();
    }
    transport.close();
    return {};
}
#endif

class RotatingBearerProvider final : public cppmcp::IAuthProvider {
public:
    void apply(cppmcp::transport::Headers& headers) override {
        std::lock_guard<std::mutex> lock(mutex_);
        headers.emplace_back("Authorization", "Bearer " + token_);
    }

    cppmcp::AuthAction on_challenge(const cppmcp::AuthChallenge&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = "valid";
        ++challenge_count_;
        return cppmcp::AuthAction::retry;
    }

    void set_token(std::string token) {
        std::lock_guard<std::mutex> lock(mutex_);
        token_ = std::move(token);
    }

    int challenge_count() const {
        return challenge_count_.load(std::memory_order_relaxed);
    }

private:
    std::mutex mutex_;
    std::string token_ = "invalid";
    std::atomic<int> challenge_count_{0};
};

std::vector<SseEvent> parse_sse(std::string_view input) {
    SseParser parser;
    return parser.feed(input);
}

}  // namespace

void test_sse_parser_cr_endings() {
    const auto lf = parse_sse("data: one\n\n");
    const auto cr = parse_sse("data: one\r\r");
    const auto crlf = parse_sse("data: one\r\n\r\n");
    CPPMCP_CHECK(lf.size() == 1);
    CPPMCP_CHECK(cr.size() == 1);
    CPPMCP_CHECK(crlf.size() == 1);
    if (lf.size() == 1 && cr.size() == 1 && crlf.size() == 1) {
        CPPMCP_CHECK(lf[0].data == "one");
        CPPMCP_CHECK(cr[0].data == "one");
        CPPMCP_CHECK(crlf[0].data == "one");
    }
}

void test_sse_parser_field_semantics() {
    const auto events = parse_sse("event: update\ndata: a\ndata: b\n\n");
    CPPMCP_CHECK(events.size() == 1);
    if (events.size() == 1) {
        CPPMCP_CHECK(events[0].event == "update");
        CPPMCP_CHECK(events[0].data == "a\nb");
    }
    CPPMCP_CHECK(parse_sse(": comment\n\n").empty());
    CPPMCP_CHECK(parse_sse("data: no-blank-line").empty());
}

void test_sse_parser_id_persistence() {
    SseParser parser;
    const auto first = parser.feed("id: stable\ndata: one\n\n");
    const auto second = parser.feed("data: two\n\n");
    CPPMCP_CHECK(first.size() == 1);
    CPPMCP_CHECK(second.size() == 1);
    if (first.size() == 1 && second.size() == 1) {
        CPPMCP_CHECK(first[0].last_event_id == "stable");
        CPPMCP_CHECK(second[0].last_event_id == "stable");
    }
    std::string nul_id = "id: invalid";
    nul_id.push_back('\0');
    nul_id += "value\ndata: ignored-id\n\n";
    const auto invalid = parser.feed(nul_id);
    CPPMCP_CHECK(invalid.size() == 1);
    if (!invalid.empty()) {
        CPPMCP_CHECK(invalid[0].last_event_id == "stable");
    }
}

void test_sse_parser_retry_digits() {
    const auto valid = parse_sse("retry: 42\ndata: ready\n\n");
    const auto invalid = parse_sse("retry: 42x\ndata: ready\n\n");
    CPPMCP_CHECK(valid.size() == 1);
    CPPMCP_CHECK(invalid.size() == 1);
    if (valid.size() == 1 && invalid.size() == 1) {
        CPPMCP_CHECK(valid[0].retry == std::chrono::milliseconds(42));
        CPPMCP_CHECK(!invalid[0].retry.has_value());
    }
}

void test_sse_parser_truncated_dispatch() {
    SseParser parser;
    CPPMCP_CHECK(parser.feed("data: incomplete").empty());
    CPPMCP_CHECK(parser.feed("\n").empty());
}

void test_sse_parser_size_limit() {
    SseParser parser(4);
    bool threw = false;
    try {
        (void)parser.feed("data: too long\n\n");
    } catch (const std::length_error&) {
        threw = true;
    }
    CPPMCP_CHECK(threw);
}

void test_www_auth_multi_challenge() {
    const auto parsed = cppmcp::parse_www_authenticate({
        "Basic realm=\"legacy\", Bearer error=\"invalid_token\", "
        "Bearer scope=\"read write\"",
    });
    CPPMCP_CHECK(parsed.size() == 3);
    if (parsed.size() == 3) {
        CPPMCP_CHECK(parsed[0].scheme == "Basic");
        CPPMCP_CHECK(parsed[1].error == "invalid_token");
        CPPMCP_CHECK(parsed[2].scope == "read write");
    }
}

void test_www_auth_quoted_escapes() {
    const auto parsed = cppmcp::parse_www_authenticate(
        {"Bearer error_description=\"a comma, and escaped \\\"quote\\\"\""});
    CPPMCP_CHECK(parsed.size() == 1);
    if (!parsed.empty()) {
        CPPMCP_CHECK(parsed[0].error_description == "a comma, and escaped \"quote\"");
    }
}

void test_www_auth_fields() {
    const auto parsed = cppmcp::parse_www_authenticate(
        {"bEaReR ERROR=\"insufficient_scope\", scope=\"tools:read\", "
         "resource_metadata=\"https://example.test/meta\""});
    CPPMCP_CHECK(parsed.size() == 1);
    if (!parsed.empty()) {
        CPPMCP_CHECK(parsed[0].error == "insufficient_scope");
        CPPMCP_CHECK(parsed[0].scope == "tools:read");
        CPPMCP_CHECK(parsed[0].resource_metadata == "https://example.test/meta");
    }
}

void test_listen_tools_filter() {
    SubscriptionRegistry registry;
    SubscriptionFilter filter;
    filter.tools_list_changed = true;
    const auto id = registry.subscribe("tools", 1, filter);
    registry.notify_tools_list_changed();
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(event["method"] == "notifications/tools/list_changed");
}

void test_listen_prompts_filter() {
    SubscriptionRegistry registry;
    SubscriptionFilter filter;
    filter.prompts_list_changed = true;
    const auto id = registry.subscribe("prompts", 2, filter);
    registry.notify_tools_list_changed();
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(0)) ==
                 SubscriptionReadStatus::timeout);
    registry.notify_prompts_list_changed();
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(event["method"] == "notifications/prompts/list_changed");
}

void test_listen_resources_filter() {
    SubscriptionRegistry registry;
    SubscriptionFilter filter;
    filter.resources_list_changed = true;
    const auto id = registry.subscribe("resources", 3, filter);
    registry.notify_resources_list_changed();
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(event["method"] == "notifications/resources/list_changed");
}

void test_listen_resource_uri() {
    SubscriptionRegistry registry;
    SubscriptionFilter filter;
    filter.resource_subscriptions = {"file:///wanted"};
    const auto id = registry.subscribe("resource-uri", 4, filter);
    registry.notify_resource_updated("file:///other");
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(0)) ==
                 SubscriptionReadStatus::timeout);
    registry.notify_resource_updated("file:///wanted");
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(event["method"] == "notifications/resources/updated");
    CPPMCP_CHECK(event["params"]["uri"] == "file:///wanted");
}

void test_listen_fanout() {
    SubscriptionRegistry registry(128, 128, 1024);
    SubscriptionFilter filter;
    filter.tools_list_changed = true;
    std::vector<std::uint64_t> subscribers;
    for (std::uint64_t i = 0; i < 64; ++i) {
        subscribers.push_back(registry.subscribe("fanout-" + std::to_string(i), i, filter));
    }
    registry.notify_tools_list_changed();
    std::size_t delivered = 0;
    for (const auto id : subscribers) {
        nlohmann::json event;
        delivered += registry.next(id, event, std::chrono::milliseconds(10)) ==
                     SubscriptionReadStatus::event;
    }
    CPPMCP_CHECK(delivered == subscribers.size());
}

void test_listen_overflow() {
    SubscriptionRegistry registry(4, 4, 8);
    SubscriptionFilter filter;
    filter.tools_list_changed = true;
    const auto id = registry.subscribe("overflow", 1, filter);
    registry.notify_tools_list_changed();
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(0)) ==
                 SubscriptionReadStatus::overflow);
}

void test_listen_global_limit() {
    SubscriptionRegistry registry(1, 4, 1024);
    const auto first = registry.subscribe("one", 1, {});
    bool limited = false;
    try {
        (void)registry.subscribe("two", 2, {});
    } catch (const cppmcp::transport::SubscriptionLimitError&) {
        limited = true;
    }
    CPPMCP_CHECK(first != 0);
    CPPMCP_CHECK(limited);
}

void test_listen_peer_limit() {
    SubscriptionRegistry registry(4, 1, 1024);
    (void)registry.subscribe("same-peer", 1, {});
    bool limited = false;
    try {
        (void)registry.subscribe("same-peer", 2, {});
    } catch (const cppmcp::transport::SubscriptionLimitError&) {
        limited = true;
    }
    CPPMCP_CHECK(limited);
}

void test_listen_cleanup() {
    SubscriptionRegistry registry;
    const auto id = registry.subscribe("cleanup", 1, {});
    CPPMCP_CHECK(registry.size() == 1);
    registry.unsubscribe(id);
    CPPMCP_CHECK(registry.size() == 0);
}

void test_listen_shutdown() {
    SubscriptionRegistry registry;
    const auto id = registry.subscribe("shutdown", 1, {});
    registry.close_all();
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::shutdown);
}

void test_listen_1000_subscribers() {
    constexpr std::size_t count = 1000;
    SubscriptionRegistry registry(count, count, 1024);
    SubscriptionFilter filter;
    filter.tools_list_changed = true;
    std::vector<std::uint64_t> ids(count);
    std::vector<std::thread> workers;
    workers.reserve(10);
    for (std::size_t worker = 0; worker < 10; ++worker) {
        workers.emplace_back([&, worker] {
            const auto begin = worker * (count / 10);
            const auto end = begin + count / 10;
            for (std::size_t i = begin; i < end; ++i) {
                ids[i] = registry.subscribe("soak-" + std::to_string(i), i, filter);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    registry.notify_tools_list_changed();
    std::atomic<std::size_t> delivered{0};
    workers.clear();
    for (std::size_t worker = 0; worker < 10; ++worker) {
        workers.emplace_back([&, worker] {
            const auto begin = worker * (count / 10);
            const auto end = begin + count / 10;
            for (std::size_t i = begin; i < end; ++i) {
                nlohmann::json event;
                if (registry.next(ids[i], event, std::chrono::seconds(1)) ==
                    SubscriptionReadStatus::event) {
                    delivered.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CPPMCP_CHECK(delivered.load(std::memory_order_relaxed) == count);
}

void test_listen_no_unrequested_types() {
    SubscriptionRegistry registry;
    const auto id = registry.subscribe("opt-in", 1, {});
    registry.notify_tools_list_changed();
    registry.notify_prompts_list_changed();
    registry.notify_resources_list_changed();
    nlohmann::json event;
    CPPMCP_CHECK(registry.next(id, event, std::chrono::milliseconds(0)) ==
                 SubscriptionReadStatus::timeout);
}

void test_backoff_base_validation() {
    cppmcp::HttpClientOptions client_options;
    client_options.url = "http://127.0.0.1:1";
    cppmcp::transport::HttpClientTransport transport(std::move(client_options));
    bool invalid = false;
    try {
        (void)transport.listen({}, [](const auto&) {}, {},
                               {.backoff_base = std::chrono::milliseconds(0)});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    CPPMCP_CHECK(invalid);
}

void test_backoff_cap_validation() {
    cppmcp::HttpClientOptions client_options;
    client_options.url = "http://127.0.0.1:1";
    cppmcp::transport::HttpClientTransport transport(std::move(client_options));
    bool invalid = false;
    try {
        (void)transport.listen(
            {}, [](const auto&) {}, {},
            {.backoff_base = std::chrono::milliseconds(100),
             .backoff_cap = std::chrono::milliseconds(50)});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    CPPMCP_CHECK(invalid);
}

void test_backoff_jitter_validation() {
    cppmcp::HttpClientOptions client_options;
    client_options.url = "http://127.0.0.1:1";
    cppmcp::transport::HttpClientTransport transport(std::move(client_options));
    bool invalid = false;
    try {
        (void)transport.listen({}, [](const auto&) {}, {},
                               {.jitter_fraction = 1.01});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    CPPMCP_CHECK(invalid);
}

void test_tls_min_version() {
    const cppmcp::transport::HttpTlsOptions options;
    CPPMCP_CHECK(options.min_version == cppmcp::transport::TlsVersion::tls_1_2);
}

void test_tls_missing_certificate() {
    cppmcp::Server server;
    cppmcp::transport::HttpOptions options;
    options.port = 0;
    options.tls = cppmcp::transport::HttpTlsOptions{
        .cert_chain_file = "/nonexistent/cppmcp-test-cert.pem",
        .private_key_file = "/nonexistent/cppmcp-test-key.pem",
    };
    bool rejected = false;
    try {
        (void)server.start_http(options);
    } catch (const std::exception&) {
        rejected = true;
    }
    CPPMCP_CHECK(rejected);
}

void test_tls_hostname_verification() {
#if CPPMCP_WITH_TLS
    try {
        TestTlsCertificate certificate;
        TestTlsServer server(certificate);
        server.start([](auto&) {});
        const auto error = attempt_https_request(
            "https://127.0.0.1:" + std::to_string(server.port()) + "/mcp",
            certificate.cert_path());
        CPPMCP_LOG_INFO("hostname verification error: " << error);
        CPPMCP_CHECK(error.find("hostname") != std::string::npos ||
                     error.find("address mismatch") != std::string::npos);
    } catch (const std::exception&) {
        CPPMCP_CHECK(false);
    }
#else
    CPPMCP_CHECK(CPPMCP_WITH_TLS == 0);
#endif
}

void test_tls_expired_certificate() {
#if CPPMCP_WITH_TLS
    try {
        TestTlsCertificate certificate(true);
        TestTlsServer server(certificate);
        server.start([](auto&) {});
        const auto error = attempt_https_request(
            "https://localhost:" + std::to_string(server.port()) + "/mcp",
            certificate.cert_path());
        CPPMCP_CHECK(error.find("expired") != std::string::npos);
    } catch (const std::exception&) {
        CPPMCP_CHECK(false);
    }
#else
    CPPMCP_CHECK(CPPMCP_WITH_TLS == 0);
#endif
}

void test_tls_truncated_stream() {
#if CPPMCP_WITH_TLS
    try {
        TestTlsCertificate certificate;
        TestTlsServer server(certificate);
        server.start([](auto& stream) {
            asio::streambuf request;
            asio::error_code error;
            asio::read_until(stream, request, "\r\n\r\n", error);
            if (error) {
                return;
            }
            const std::string partial_response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: 100\r\nConnection: close\r\n\r\n{\"partial\":";
            asio::write(stream, asio::buffer(partial_response), error);
            asio::error_code ignored;
            stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            stream.next_layer().close(ignored);
        });
        const auto error = attempt_https_request(
            "https://localhost:" + std::to_string(server.port()) + "/mcp",
            certificate.cert_path());
        CPPMCP_CHECK(error.find("truncated") != std::string::npos ||
                     error.find("read failed") != std::string::npos);
    } catch (const std::exception&) {
        CPPMCP_CHECK(false);
    }
#else
    CPPMCP_CHECK(CPPMCP_WITH_TLS == 0);
#endif
}

void test_www_auth_listener_retry() {
    cppmcp::Server server;
    cppmcp::transport::HttpOptions server_options;
    server_options.port = 0;
    server_options.mode = cppmcp::transport::HttpMode::modern;
    server_options.handler.mode = server_options.mode;
    server_options.bearer_token = "valid";
    auto http_server = server.start_http(server_options);

    auto auth_provider = std::make_shared<RotatingBearerProvider>();
    cppmcp::HttpClientOptions client_options;
    client_options.url = "http://127.0.0.1:" + std::to_string(http_server->port()) + "/mcp";
    client_options.policy = cppmcp::NegotiationPolicy::modern_only;
    client_options.auth_provider = auth_provider;
    cppmcp::transport::HttpClientTransport client(std::move(client_options));
    client.start();
    auth_provider->set_token("invalid");
    auto listener = client.listen(
        nlohmann::json{{"toolsListChanged", true}},
        [](const cppmcp::core::Notification&) {});
    listener->close();
    client.close();
    http_server->stop();
    CPPMCP_CHECK(auth_provider->challenge_count() >= 2);
}
