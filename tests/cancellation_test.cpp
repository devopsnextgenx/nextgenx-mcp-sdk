#include "cppmcp/client.hpp"
#include "cppmcp/core/transport.hpp"
#include "test.hpp"

#include <memory>
#include <optional>
#include <stdexcept>

namespace {

class AbortStreamTransport final : public cppmcp::core::ITransport {
public:
    void start() override {}
    void send(const cppmcp::core::Message& message) override { sent.push_back(message); }
    void close() override {}

    cppmcp::core::CancellationStrategy cancellation_strategy() const noexcept override {
        return cppmcp::core::CancellationStrategy::abort_stream;
    }

    void cancel_request(const cppmcp::core::RequestId& request_id) override {
        cancelled = request_id;
    }

    std::vector<cppmcp::core::Message> sent;
    std::optional<cppmcp::core::RequestId> cancelled;
};

}  // namespace

void test_cancellation_strategy() {
    using namespace cppmcp;
    using namespace cppmcp::core;

    auto [client_transport, server_transport] = make_memory_transport_pair();
    std::optional<Notification> notification;
    server_transport->on_message([&](Message message) {
        if (const auto* received = std::get_if<Notification>(&message)) {
            notification = *received;
        }
    });
    Client stdio_style_client;
    stdio_style_client.connect(client_transport);
    stdio_style_client.cancel(RequestId{std::int64_t{41}});
    CPPMCP_CHECK(notification.has_value());
    if (notification.has_value()) {
        CPPMCP_CHECK(notification->method == "notifications/cancelled");
        CPPMCP_CHECK(notification->params["requestId"] == 41);
    }
    stdio_style_client.close();

    auto [request_transport, peer_transport] = make_memory_transport_pair();
    peer_transport->on_message([](Message) {});
    Connection connection(request_transport);
    connection.start();
    auto pending = connection.send_request("slow/count");
    connection.cancel(RequestId{std::int64_t{1}});
    bool cancelled = false;
    try {
        (void)pending.get();
    } catch (const std::runtime_error& error) {
        cancelled = std::string(error.what()) == "request cancelled";
    }
    CPPMCP_CHECK(cancelled);
    connection.close();

    auto abort_transport = std::make_shared<AbortStreamTransport>();
    Client http_style_client;
    http_style_client.connect(abort_transport);
    http_style_client.cancel(RequestId{std::int64_t{73}});
    CPPMCP_CHECK(abort_transport->cancelled.has_value());
    CPPMCP_CHECK(abort_transport->cancelled ==
                 std::optional<RequestId>{RequestId{std::int64_t{73}}});
    CPPMCP_CHECK(abort_transport->sent.empty());
    http_style_client.close();
}
