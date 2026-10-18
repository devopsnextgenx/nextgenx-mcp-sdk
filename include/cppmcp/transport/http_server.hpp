#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "cppmcp/core/transport.hpp"

namespace cppmcp {
class Server;
}

namespace cppmcp::transport {

class HttpServerHandle {
public:
    ~HttpServerHandle();

    HttpServerHandle(HttpServerHandle&&) noexcept;
    HttpServerHandle& operator=(HttpServerHandle&&) noexcept;
    HttpServerHandle(const HttpServerHandle&) = delete;
    HttpServerHandle& operator=(const HttpServerHandle&) = delete;

    void stop();
    void wait();
    unsigned short port() const noexcept;

private:
    struct Impl;
    explicit HttpServerHandle(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class ::cppmcp::Server;
};

class AsioExecutor final : public cppmcp::core::IExecutor {
public:
    explicit AsioExecutor(std::size_t workers = 2);
    ~AsioExecutor() override;

    AsioExecutor(const AsioExecutor&) = delete;
    AsioExecutor& operator=(const AsioExecutor&) = delete;

    void post(std::function<void()> task) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cppmcp::transport
