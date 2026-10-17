#include "cppmcp/transport/stdio.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cppmcp/core/log.hpp"

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

#ifndef _WIN32
// Writes the whole buffer, retrying on EINTR and handling partial writes.
bool write_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const auto n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

// Returns true if the child has exited (or no longer exists) within `timeout`.
// When it was reaped, its wait status is stored in `status_out`.
bool wait_for_exit(pid_t pid, std::chrono::milliseconds timeout, int* status_out = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (status_out != nullptr) {
                *status_out = status;
            }
            return true;
        }
        if (r < 0 && errno != EINTR) {
            return true;  // ECHILD etc.: nothing left to wait for
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string describe_status(int status) {
    if (WIFEXITED(status)) {
        std::string text = "exit code " + std::to_string(WEXITSTATUS(status));
        if (WEXITSTATUS(status) == 127) {
            text += " (exec or chdir failed: check the command path and cwd)";
        }
        return text;
    }
    if (WIFSIGNALED(status)) {
        return "killed by signal " + std::to_string(WTERMSIG(status));
    }
    return "unknown status " + std::to_string(status);
}
#endif

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
    CPPMCP_LOG_INFO("server transport started; max_line_length=" << max_line_length_
                    << " stdout_guard=" << (stdout_guard_ ? "on" : "off"));
    reader_thread_ = std::thread(&StdioServerTransport::read_loop, this);
}

void StdioServerTransport::handle_line(std::string line) {
    line = trim_newlines(std::move(line));
    if (line.empty()) {
        return;
    }
    if (line.size() > max_line_length_) {
        CPPMCP_LOG_WARN("dropping oversized frame (" << line.size() << " bytes, limit "
                        << max_line_length_ << ")");
        if (on_parse_error_) {
            on_parse_error_(cppmcp::core::protocol_error(cppmcp::core::ErrorCode::invalid_request,
                                                       "JSON-RPC line exceeds the configured maximum length."));
        }
        return;
    }
    CPPMCP_LOG_DEBUG("recv frame (" << line.size() << " bytes): " << line);
    const auto parsed = cppmcp::core::parse_message(line);
    if (const auto* error = std::get_if<cppmcp::core::RpcError>(&parsed)) {
        CPPMCP_LOG_WARN("rejecting frame: code=" << error->code << " message=\"" << error->message
                        << "\" frame: " << line);
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
        if (count < 0 && errno == EINTR) {
            continue;
        }
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
                CPPMCP_LOG_WARN("frame exceeds " << max_line_length_ << " bytes; discarding buffer");
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
    // stdin reached EOF (or errored): tell the owner exactly once. Do NOT call
    // close() from this callback (it would join this very thread); just signal
    // the main thread and let it call close().
    const bool was_running = running_.exchange(false);
    CPPMCP_LOG_INFO("stdin reached EOF; was_running=" << (was_running ? "yes" : "no")
                    << "; on_close handler " << (on_close_ ? "set" : "NOT SET (nothing is waiting on it)"));
    if (was_running) {
        if (on_close_) {
            on_close_(cppmcp::core::CloseReason::eof);
        }
    }
#endif
}

void StdioServerTransport::send(const cppmcp::core::Message& message) {
    std::string frame = cppmcp::core::serialize_message(message);
    CPPMCP_LOG_DEBUG("send frame (" << frame.size() << " bytes): " << frame);
    frame.push_back('\n');
    std::lock_guard<std::mutex> lock(output_mutex_);
#ifndef _WIN32
    // When the stdout guard is active, STDOUT_FILENO has been redirected to
    // stderr, so protocol frames must go to the saved original stdout.
    const int fd = (stdout_guard_ && saved_stdout_ >= 0) ? saved_stdout_ : STDOUT_FILENO;
    if (!write_all(fd, frame.data(), frame.size())) {
        CPPMCP_LOG_ERROR("failed to write frame to stdout: " << std::strerror(errno));
        if (on_error_) {
            on_error_("failed to write JSON-RPC frame to stdout");
        }
    }
#endif
}

void StdioServerTransport::close() {
    // If the read loop already reported EOF it cleared running_ and fired
    // on_close_, so don't fire it a second time.
    const bool was_running = running_.exchange(false);
    CPPMCP_LOG_DEBUG("close() called; was_running=" << (was_running ? "yes" : "no"));
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
    if (was_running && on_close_) {
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

    // The child's stderr is NOT piped: it inherits ours. A pipe nobody reads
    // would eventually block a chatty server, and inheriting lets the child's
    // log output show up wherever ours does.
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        throw std::runtime_error(std::string("unable to create stdio pipes: ") + std::strerror(errno));
    }
    if (pipe(stdout_pipe) != 0) {
        const int err = errno;
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        throw std::runtime_error(std::string("unable to create stdio pipes: ") + std::strerror(err));
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

    // Note: with an empty spec_.env the child inherits our environment (including
    // CPPMCP_LOG_LEVEL / CPPMCP_LOG_FILE). A non-empty env replaces it entirely.
    std::vector<std::string> env_strings = spec_.env;
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& entry : env_strings) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid == -1) {
        const int err = errno;
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        CPPMCP_LOG_ERROR("fork failed: " << std::strerror(err));
        throw std::runtime_error(std::string("unable to spawn subprocess: ") +
                                 std::strerror(err));
    }

    if (pid == 0) {
        if (!spec_.cwd.empty() && chdir(spec_.cwd.c_str()) != 0) {
            _exit(127);
        }
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);
        if (spec_.env.empty()) {
            execv(spec_.command.c_str(), argv.data());
        } else {
            execve(spec_.command.c_str(), argv.data(), envp.data());
        }
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    stderr_fd_ = -1;
    child_pid_ = pid;
    running_ = true;
    CPPMCP_LOG_INFO("spawned child pid=" << static_cast<int>(pid) << " command=" << spec_.command
                    << " args=" << spec_.args.size()
                    << " cwd=" << (spec_.cwd.empty() ? std::string("(inherited)") : spec_.cwd));
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
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            const char ch = buffer[i];
            if (ch == '\n' || ch == '\r') {
                if (!current.empty()) {
                    CPPMCP_LOG_DEBUG("recv frame (" << current.size() << " bytes): " << current);
                    auto parsed = cppmcp::core::parse_message(current);
                    if (const auto* error = std::get_if<cppmcp::core::RpcError>(&parsed)) {
                        CPPMCP_LOG_WARN("rejecting frame from child: code=" << error->code
                                        << " message=\"" << error->message << "\" frame: " << current);
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
    CPPMCP_LOG_DEBUG("child stdout reached EOF (running=" << (running_.load() ? "yes" : "no") << ")");
#endif
}

void StdioClientTransport::send(const cppmcp::core::Message& message) {
#ifndef _WIN32
    if (stdin_fd_ < 0) {
        CPPMCP_LOG_WARN("send ignored: child stdin is closed");
        return;
    }
    std::string frame = cppmcp::core::serialize_message(message);
    CPPMCP_LOG_DEBUG("send frame (" << frame.size() << " bytes): " << frame);
    frame.push_back('\n');
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (!write_all(stdin_fd_, frame.data(), frame.size())) {
        CPPMCP_LOG_ERROR("failed to write frame to child stdin: " << std::strerror(errno));
        if (error_callback_) {
            error_callback_("failed to write JSON-RPC frame to child stdin");
        }
    }
#endif
}

void StdioClientTransport::close() {
#ifndef _WIN32
    running_ = false;

    // 1. Signal EOF to the child.
    if (stdin_fd_ >= 0) {
        ::close(stdin_fd_);
        stdin_fd_ = -1;
    }

    // 2. Stop the child BEFORE joining the reader. The reader is blocked in
    //    read() and only wakes on EOF, which needs the child to exit.
    if (child_pid_ > 0) {
        const auto began = std::chrono::steady_clock::now();
        auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - began)
                .count();
        };
        int status = 0;
        CPPMCP_LOG_INFO("closing: stdin closed; waiting up to " << shutdown_grace_.count()
                        << " ms for child pid=" << static_cast<int>(child_pid_) << " to exit on EOF");
        if (wait_for_exit(child_pid_, shutdown_grace_, &status)) {
            CPPMCP_LOG_INFO("child exited on its own after " << elapsed_ms() << " ms: "
                            << describe_status(status));
        } else {
            CPPMCP_LOG_WARN("child did not exit on EOF after " << elapsed_ms() << " ms; sending SIGTERM");
            kill(child_pid_, SIGTERM);
            if (wait_for_exit(child_pid_, std::chrono::milliseconds(500), &status)) {
                CPPMCP_LOG_WARN("child exited after SIGTERM at " << elapsed_ms() << " ms: "
                                << describe_status(status));
            } else {
                CPPMCP_LOG_WARN("child ignored SIGTERM; sending SIGKILL at " << elapsed_ms() << " ms");
                kill(child_pid_, SIGKILL);
                while (waitpid(child_pid_, &status, 0) < 0 && errno == EINTR) {
                }
                CPPMCP_LOG_WARN("child reaped after SIGKILL: " << describe_status(status));
            }
        }
        child_pid_ = -1;
    }

    // 3. Now the reader gets EOF and can be joined.
    if (reader_thread_.joinable()) {
        reader_thread_.join();
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