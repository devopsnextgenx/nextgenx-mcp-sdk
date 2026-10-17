#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/transport.hpp"

namespace cppmcp::core {

class Connection {
public:
    // `executor` is kept for API compatibility but is no longer used for
    // request timeouts (those run on a dedicated cancellable timer thread).
    explicit Connection(std::shared_ptr<ITransport> transport,
                       std::shared_ptr<IExecutor> executor = std::make_shared<ThreadPoolExecutor>(2));
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

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
    void timer_loop();
    void fail_all_pending_locked(const std::string& reason);

    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<IExecutor> executor_;
    std::function<void(const Request&)> request_handler_;
    std::function<void(const Notification&)> notification_handler_;
    std::function<void(const ResultResponse&)> result_handler_;
    std::function<void(const ErrorResponse&)> error_handler_;

    std::mutex mutex_;  // guards everything below
    std::atomic<std::int64_t> next_id_{1};
    std::unordered_map<std::int64_t, std::shared_ptr<std::promise<Json>>> pending_requests_;
    std::map<std::int64_t, std::chrono::steady_clock::time_point> deadlines_;
    bool closed_ = false;

    std::condition_variable timer_cv_;
    std::thread timer_thread_;
};

}  // namespace cppmcp::core