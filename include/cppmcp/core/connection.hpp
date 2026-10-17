#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/transport.hpp"

namespace cppmcp::core {

class Connection {
public:
    explicit Connection(std::shared_ptr<ITransport> transport,
                       std::shared_ptr<IExecutor> executor = std::make_shared<ThreadPoolExecutor>(2));

    void set_request_handler(std::function<void(const Request&)> handler);
    void set_notification_handler(std::function<void(const Notification&)> handler);
    void set_result_handler(std::function<void(const ResultResponse&)> handler);
    void set_error_handler(std::function<void(const ErrorResponse&)> handler);

    void start();
    void close();

    std::future<Json> send_request(std::string method, Json params = Json::object(),
                                  std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
    void send_notification(std::string method, Json params = Json::object());

    std::int64_t next_request_id();

private:
    void handle_message(const Message& message);

    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<IExecutor> executor_;
    std::function<void(const Request&)> request_handler_;
    std::function<void(const Notification&)> notification_handler_;
    std::function<void(const ResultResponse&)> result_handler_;
    std::function<void(const ErrorResponse&)> error_handler_;

    std::mutex mutex_;
    std::atomic<std::int64_t> next_id_{1};
    std::unordered_map<std::int64_t, std::shared_ptr<std::promise<Json>>> pending_requests_;
};

}  // namespace cppmcp::core
