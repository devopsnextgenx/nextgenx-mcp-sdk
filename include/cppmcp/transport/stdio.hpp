#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cppmcp/core/json_rpc.hpp"
#include "cppmcp/core/transport.hpp"

namespace cppmcp::transport {

struct StdioProcessSpec {
    std::string command;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::string cwd;
};

class StdioServerTransport final : public cppmcp::core::ITransport {
public:
    StdioServerTransport();
    explicit StdioServerTransport(std::size_t max_line_length);
    ~StdioServerTransport() override;

    void start() override;
    void send(const cppmcp::core::Message& message) override;
    void close() override;

    void set_stdout_guard(bool enabled);
    void set_max_line_length(std::size_t max_line_length);

private:
    void read_loop();
    void handle_line(std::string line);

    std::mutex output_mutex_;
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    std::size_t max_line_length_ = 16 * 1024 * 1024;
    bool stdout_guard_ = false;
    int saved_stdout_ = -1;
};

class StdioClientTransport final : public cppmcp::core::ITransport {
public:
    using ErrorCallback = std::function<void(std::string)>;

    explicit StdioClientTransport(StdioProcessSpec spec,
                                  std::chrono::milliseconds shutdown_grace = std::chrono::milliseconds{2000},
                                  ErrorCallback error_callback = {});
    ~StdioClientTransport() override;

    void start() override;
    void send(const cppmcp::core::Message& message) override;
    void close() override;

    bool running() const;

private:
    void read_loop();
    void set_error_callback(ErrorCallback callback);

    StdioProcessSpec spec_;
    std::chrono::milliseconds shutdown_grace_;
    ErrorCallback error_callback_;
    std::mutex output_mutex_;
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    std::string child_stderr_buffer_;

#ifdef _WIN32
    void* process_handle_ = nullptr;
    void* stdout_read_ = nullptr;
    void* stdin_write_ = nullptr;
    void* stderr_read_ = nullptr;
#else
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    pid_t child_pid_ = -1;
#endif
};

}  // namespace cppmcp::transport
