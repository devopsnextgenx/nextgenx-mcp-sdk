#include "cppmcp/core/connection.hpp"

#include "cppmcp/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cppmcp::core {
namespace {

const char* reason_name(CloseReason reason) {
    switch (reason) {
        case CloseReason::normal: return "normal";
        case CloseReason::eof: return "eof";
        case CloseReason::parse_error: return "parse_error";
        case CloseReason::timeout: return "timeout";
        case CloseReason::error: return "error";
        case CloseReason::child_exit: return "child_exit";
        case CloseReason::shutdown: return "shutdown";
    }
    return "unknown";
}

}  // namespace

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
    // A closed/dead transport (e.g. the child process exited) can never answer
    // outstanding requests, so fail them now instead of waiting for timeouts.
    transport_->on_close([this](CloseReason reason) {
        CPPMCP_LOG_INFO("transport closed (reason=" << reason_name(reason) << ")");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            fail_all_pending_locked("transport closed");
        }
        timer_cv_.notify_all();
    });
}

Connection::~Connection() {
    // The transport can outlive this object and still hold callbacks that
    // capture `this`. Close it (joins its reader thread) and drop the
    // callbacks before anything is destroyed.
    try {
        transport_->close();
    } catch (...) {
    }
    transport_->on_message({});
    transport_->on_parse_error({});
    transport_->on_close({});

    // Stop the timer thread so nothing touches `this` while we tear down.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        fail_all_pending_locked("connection destroyed");
    }
    timer_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
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

void Connection::close() {
    // Close the transport first (this joins its reader thread, which may be
    // inside handle_message), and only then fail whatever is still pending.
    transport_->close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        fail_all_pending_locked("connection closed");
    }
    timer_cv_.notify_all();
}

std::int64_t Connection::next_request_id() { return next_id_.fetch_add(1, std::memory_order_relaxed); }

void Connection::fail_all_pending_locked(const std::string& reason) {
    if (!pending_requests_.empty()) {
        CPPMCP_LOG_WARN("failing " << pending_requests_.size() << " pending request(s): " << reason);
    }
    for (auto& entry : pending_requests_) {
        entry.second->set_exception(std::make_exception_ptr(std::runtime_error(reason)));
    }
    pending_requests_.clear();
    deadlines_.clear();
}

void Connection::timer_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!closed_) {
        if (deadlines_.empty()) {
            timer_cv_.wait(lock);
            continue;
        }
        const auto earliest = std::min_element(
            deadlines_.begin(), deadlines_.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        const auto deadline = earliest->second;
        if (deadline <= std::chrono::steady_clock::now()) {
            const auto id = earliest->first;
            deadlines_.erase(earliest);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                CPPMCP_LOG_WARN("request id=" << id << " timed out");
                it->second->set_exception(
                    std::make_exception_ptr(std::runtime_error("request timeout")));
                pending_requests_.erase(it);
            }
            continue;
        }
        // Woken early by a new request, a completed request, or shutdown.
        timer_cv_.wait_until(lock, deadline);
    }
}

std::future<Json> Connection::send_request(std::string method, Json params,
                                          std::chrono::milliseconds timeout) {
    const auto id = next_request_id();
    auto promise = std::make_shared<std::promise<Json>>();
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            promise->set_exception(
                std::make_exception_ptr(std::runtime_error("connection closed")));
            return future;
        }
        pending_requests_[id] = promise;
        if (timeout.count() > 0) {
            deadlines_[id] = std::chrono::steady_clock::now() + timeout;
            if (!timer_thread_.joinable()) {
                timer_thread_ = std::thread(&Connection::timer_loop, this);
            }
        }
    }
    timer_cv_.notify_all();

    CPPMCP_LOG_DEBUG("request id=" << id << " method=" << method << " timeout=" << timeout.count() << "ms");
    try {
        transport_->send(Request{id, std::move(method), std::move(params)});
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_requests_.erase(id);
        deadlines_.erase(id);
        throw;
    }
    return future;
}

void Connection::send_notification(std::string method, Json params) {
    transport_->send(Notification{std::move(method), std::move(params)});
}

void Connection::cancel(const RequestId& request_id) {
    transport_->cancel_request(request_id);
    if (const auto* numeric_id = std::get_if<std::int64_t>(&request_id)) {
        std::shared_ptr<std::promise<Json>> promise;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_requests_.find(*numeric_id);
            if (it != pending_requests_.end()) {
                promise = std::move(it->second);
                pending_requests_.erase(it);
                deadlines_.erase(*numeric_id);
            }
        }
        if (promise) {
            promise->set_exception(std::make_exception_ptr(std::runtime_error("request cancelled")));
        }
        timer_cv_.notify_all();
    }
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
                std::shared_ptr<std::promise<Json>> promise;
                if (const auto* id = std::get_if<std::int64_t>(&value.id)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = pending_requests_.find(*id);
                    if (it != pending_requests_.end()) {
                        promise = std::move(it->second);
                        pending_requests_.erase(it);
                        deadlines_.erase(*id);
                    }
                }
                // Complete the promise and run user callbacks outside the lock.
                if (promise) {
                    promise->set_value(value.result);
                } else {
                    CPPMCP_LOG_DEBUG("result for unknown or already-completed request");
                    if (result_handler_) {
                        result_handler_(value);
                    }
                }
            } else {
                std::shared_ptr<std::promise<Json>> promise;
                if (value.id.has_value()) {
                    if (const auto* id = std::get_if<std::int64_t>(&*value.id)) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto it = pending_requests_.find(*id);
                        if (it != pending_requests_.end()) {
                            promise = std::move(it->second);
                            pending_requests_.erase(it);
                            deadlines_.erase(*id);
                        }
                    }
                }
                if (promise) {
                    promise->set_exception(
                        std::make_exception_ptr(std::runtime_error(value.error.message)));
                }
                CPPMCP_LOG_WARN("error response: code=" << value.error.code << " message=\""
                                << value.error.message << "\" matched_request=" << (promise ? "yes" : "no"));
                if (error_handler_) {
                    error_handler_(value);
                }
            }
        },
        message);
}

}  // namespace cppmcp::core