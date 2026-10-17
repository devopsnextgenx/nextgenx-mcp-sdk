#include "cppmcp/core/connection.hpp"

#include <utility>

namespace cppmcp::core {

Connection::Connection(std::shared_ptr<ITransport> transport,
                       std::shared_ptr<IExecutor> executor)
    : transport_(std::move(transport)), executor_(std::move(executor)) {
    if (!transport_) {
        throw std::invalid_argument("transport must not be null");
    }
    transport_->on_message([this](const Message& message) { handle_message(message); });
    transport_->on_parse_error([this](const RpcError& error) {
        if (error_handler_) {
            error_handler_(ErrorResponse{std::nullopt, error});
        }
    });
}

void Connection::set_request_handler(std::function<void(const Request&)> handler) {
    request_handler_ = std::move(handler);
}

void Connection::set_notification_handler(std::function<void(const Notification&)> handler) {
    notification_handler_ = std::move(handler);
}

void Connection::set_result_handler(std::function<void(const ResultResponse&)> handler) {
    result_handler_ = std::move(handler);
}

void Connection::set_error_handler(std::function<void(const ErrorResponse&)> handler) {
    error_handler_ = std::move(handler);
}

void Connection::start() { transport_->start(); }

void Connection::close() { transport_->close(); }

std::int64_t Connection::next_request_id() { return next_id_.fetch_add(1, std::memory_order_relaxed); }

std::future<Json> Connection::send_request(std::string method, Json params,
                                          std::chrono::milliseconds timeout) {
    const auto id = next_request_id();
    auto promise = std::make_shared<std::promise<Json>>();
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_requests_[id] = promise;
    }
    transport_->send(Request{id, std::move(method), std::move(params)});
    if (timeout.count() > 0) {
        executor_->post([this, id, promise, timeout]() {
            std::this_thread::sleep_for(timeout);
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                it->second->set_exception(std::make_exception_ptr(std::runtime_error("request timeout")));
                pending_requests_.erase(it);
            }
        });
    }
    return future;
}

void Connection::send_notification(std::string method, Json params) {
    transport_->send(Notification{std::move(method), std::move(params)});
}

void Connection::handle_message(const Message& message) {
    std::visit(
        [this](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Request>) {
                if (request_handler_) {
                    request_handler_(value);
                }
            } else if constexpr (std::is_same_v<T, Notification>) {
                if (notification_handler_) {
                    notification_handler_(value);
                }
            } else if constexpr (std::is_same_v<T, ResultResponse>) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = pending_requests_.find(std::get<std::int64_t>(value.id));
                if (it != pending_requests_.end()) {
                    it->second->set_value(value.result);
                    pending_requests_.erase(it);
                } else if (result_handler_) {
                    result_handler_(value);
                }
            } else {
                if (value.id.has_value()) {
                    const auto id = std::get<std::int64_t>(*value.id);
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = pending_requests_.find(id);
                    if (it != pending_requests_.end()) {
                        it->second->set_exception(std::make_exception_ptr(std::runtime_error(value.error.message)));
                        pending_requests_.erase(it);
                    }
                }
                if (error_handler_) {
                    error_handler_(value);
                }
            }
        },
        message);
}

}  // namespace cppmcp::core
