#include "cppmcp/transport/stdio.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
extern char** environ;
#endif

namespace cppmcp::transport {
namespace {

std::string trim_newlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

}  // namespace

StdioServerTransport::StdioServerTransport() = default;

StdioServerTransport::StdioServerTransport(std::size_t max_line_length)
    : max_line_length_(max_line_length) {}

StdioServerTransport::~StdioServerTransport() {
    close();
}

void StdioServerTransport::set_stdout_guard(bool enabled) {
    stdout_guard_ = enabled;
#ifndef _WIN32
    if (enabled && saved_stdout_ == -1) {
        saved_stdout_ = dup(STDOUT_FILENO);
        if (saved_stdout_ >= 0) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
        }
    }
#endif
}

void StdioServerTransport::set_max_line_length(std::size_t max_line_length) {
    max_line_length_ = max_line_length;
}

void StdioServerTransport::start() {
    if (running_) {
        return;
    }
    running_ = true;
    if (stdout_guard_) {
        set_stdout_guard(true);
    }
    reader_thread_ = std::thread(&StdioServerTransport::read_loop, this);
}

void StdioServerTransport::handle_line(std::string line) {
    line = trim_newlines(std::move(line));
    if (line.empty()) {
        return;
    }
    if (line.size() > max_line_length_) {
        if (on_parse_error_) {
            on_parse_error_(cppmcp::core::protocol_error(cppmcp::core::ErrorCode::invalid_request,
                                                       "JSON-RPC line exceeds the configured maximum length."));
        }
        return;
    }
    const auto parsed = cppmcp::core::parse_message(line);
    if (const auto* error = std::get_if<cppmcp::core::RpcError>(&parsed)) {
        if (on_parse_error_) {
            on_parse_error_(*error);
        }
        return;
    }
    if (on_message_) {
        on_message_(std::get<cppmcp::core::Message>(parsed));
    }
}

void StdioServerTransport::read_loop() {
#ifndef _WIN32
    std::string current;
    while (running_) {
        char buffer[4096];
        const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            const char ch = buffer[i];
            if (ch == '\r' || ch == '\n') {
                if (!current.empty()) {
                    handle_line(current);
                    current.clear();
                }
                continue;
            }
            if (current.size() >= max_line_length_) {
                if (on_parse_error_) {
                    on_parse_error_(cppmcp::core::protocol_error(cppmcp::core::ErrorCode::invalid_request,
                                                               "JSON-RPC line exceeds the configured maximum length."));
                }
                current.clear();
                continue;
            }
            current.push_back(ch);
        }
    }
#endif
}

void StdioServerTransport::send(const cppmcp::core::Message& message) {
    const auto serialized = cppmcp::core::serialize_message(message) + '\n';
    std::lock_guard<std::mutex> lock(output_mutex_);
#ifndef _WIN32
    const auto bytes = serialized.size();
    const auto written = ::write(STDOUT_FILENO, serialized.data(), bytes);
    if (written < 0) {
        if (on_error_) {
            on_error_("failed to write JSON-RPC frame to stdout");
        }
    }
#endif
}

void StdioServerTransport::close() {
    running_ = false;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
#ifndef _WIN32
    if (saved_stdout_ >= 0) {
        dup2(saved_stdout_, STDOUT_FILENO);
    ::close(saved_stdout_);
        saved_stdout_ = -1;
    }
#endif
    if (on_close_) {
        on_close_(cppmcp::core::CloseReason::normal);
    }
}

StdioClientTransport::StdioClientTransport(StdioProcessSpec spec,
                                           std::chrono::milliseconds shutdown_grace,
                                           ErrorCallback error_callback)
    : spec_(std::move(spec)),
      shutdown_grace_(shutdown_grace),
      error_callback_(std::move(error_callback)) {}

StdioClientTransport::~StdioClientTransport() {
    close();
}

bool StdioClientTransport::running() const { return running_.load(); }

void StdioClientTransport::set_error_callback(ErrorCallback callback) {
    error_callback_ = std::move(callback);
}

void StdioClientTransport::start() {
#ifndef _WIN32
    if (running_) {
        return;
    }

    if (spec_.command.empty()) {
        throw std::runtime_error("stdio command is empty");
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        throw std::runtime_error("unable to create stdio pipes");
    }

    std::vector<std::string> argv_strings;
    argv_strings.push_back(spec_.command);
    for (const auto& arg : spec_.args) {
        argv_strings.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(argv_strings.size() + 1);
    for (auto& arg : argv_strings) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_strings = spec_.env;
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& entry : env_strings) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid == -1) {
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        throw std::runtime_error("unable to spawn subprocess");
    }

    if (pid == 0) {
        if (!spec_.cwd.empty() && chdir(spec_.cwd.c_str()) != 0) {
            _exit(127);
        }
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        if (spec_.env.empty()) {
            execv(spec_.command.c_str(), argv.data());
        } else {
            execve(spec_.command.c_str(), argv.data(), envp.data());
        }
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    stderr_fd_ = stderr_pipe[0];
    child_pid_ = pid;
    running_ = true;
    reader_thread_ = std::thread(&StdioClientTransport::read_loop, this);
#else
    (void)spec_;
    (void)shutdown_grace_;
    (void)error_callback_;
#endif
}

void StdioClientTransport::read_loop() {
#ifndef _WIN32
    std::string current;
    while (running_) {
        char buffer[4096];
        const auto count = read(stdout_fd_, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            const char ch = buffer[i];
            if (ch == '\n' || ch == '\r') {
                if (!current.empty()) {
                    auto parsed = cppmcp::core::parse_message(current);
                    if (const auto* error = std::get_if<cppmcp::core::RpcError>(&parsed)) {
                        if (on_parse_error_) {
                            on_parse_error_(*error);
                        }
                    } else {
                        if (on_message_) {
                            on_message_(std::get<cppmcp::core::Message>(parsed));
                        }
                    }
                    current.clear();
                }
                continue;
            }
            current.push_back(ch);
        }
    }
#endif
}

void StdioClientTransport::send(const cppmcp::core::Message& message) {
#ifndef _WIN32
    if (stdin_fd_ < 0) {
        return;
    }
    const auto payload = cppmcp::core::serialize_message(message) + '\n';
    std::lock_guard<std::mutex> lock(output_mutex_);
    ::write(stdin_fd_, payload.data(), payload.size());
#endif
}

void StdioClientTransport::close() {
#ifndef _WIN32
    running_ = false;
    if (stdin_fd_ >= 0) {
        ::close(stdin_fd_);
        stdin_fd_ = -1;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        std::this_thread::sleep_for(shutdown_grace_);
        kill(child_pid_, SIGKILL);
        waitpid(child_pid_, nullptr, 0);
        child_pid_ = -1;
    }
    if (stdout_fd_ >= 0) {
        ::close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (stderr_fd_ >= 0) {
        ::close(stderr_fd_);
        stderr_fd_ = -1;
    }
    if (on_close_) {
        on_close_(cppmcp::core::CloseReason::normal);
    }
#else
    if (on_close_) {
        on_close_(cppmcp::core::CloseReason::normal);
    }
#endif
}

}  // namespace cppmcp::transport
