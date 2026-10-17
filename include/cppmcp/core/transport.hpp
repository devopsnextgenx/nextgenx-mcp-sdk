#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cppmcp/core/json_rpc.hpp"

namespace cppmcp::core {

enum class CloseReason {
    normal,
    eof,
    parse_error,
    timeout,
    error,
    child_exit,
    shutdown,
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void start() = 0;
    virtual void send(const Message&) = 0;
    virtual void close() = 0;

    void on_message(std::function<void(Message)> callback) { on_message_ = std::move(callback); }
    void on_parse_error(std::function<void(RpcError)> callback) { on_parse_error_ = std::move(callback); }
    void on_close(std::function<void(CloseReason)> callback) { on_close_ = std::move(callback); }
    void on_error(std::function<void(std::string)> callback) { on_error_ = std::move(callback); }

protected:
    std::function<void(Message)> on_message_;
    std::function<void(RpcError)> on_parse_error_;
    std::function<void(CloseReason)> on_close_;
    std::function<void(std::string)> on_error_;
};

class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual void post(std::function<void()>) = 0;
};

class ThreadPoolExecutor final : public IExecutor {
public:
    explicit ThreadPoolExecutor(std::size_t workers = 2) {
        if (workers == 0) {
            workers = 1;
        }
        workers_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
                        if (shutdown_ && queue_.empty()) {
                            return;
                        }
                        task = std::move(queue_.front());
                        queue_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPoolExecutor() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void post(std::function<void()> task) override {
        if (!task) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_) {
                return;
            }
            queue_.emplace(std::move(task));
        }
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool shutdown_ = false;
};

class MemoryTransport final : public ITransport {
public:
    void start() override {}
    void send(const Message& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (peer_ == nullptr) {
            return;
        }
        peer_->deliver(message);
    }

    void close() override {
        closed_ = true;
        if (on_close_) {
            on_close_(CloseReason::normal);
        }
    }

    void connect(std::shared_ptr<MemoryTransport> peer) { peer_ = std::move(peer); }
    void deliver(const Message& message) {
        if (on_message_) {
            on_message_(message);
        }
    }

    bool closed() const { return closed_; }

private:
    std::shared_ptr<MemoryTransport> peer_;
    std::mutex mutex_;
    bool closed_ = false;
};

inline std::pair<std::shared_ptr<MemoryTransport>, std::shared_ptr<MemoryTransport>>
make_memory_transport_pair() {
    auto a = std::make_shared<MemoryTransport>();
    auto b = std::make_shared<MemoryTransport>();
    a->connect(b);
    b->connect(a);
    return {a, b};
}

}  // namespace cppmcp::core
