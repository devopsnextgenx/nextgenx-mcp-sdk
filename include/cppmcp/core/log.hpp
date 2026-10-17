#pragma once

// Minimal header-only logger for cppmcp.
//
// One record per line:
//   2026-10-17T14:03:22.123Z INFO  pid=3981973 tid=1 stdio.cpp:249 StdioClientTransport::start() - spawned child ...
//
// Never writes to stdout (stdout is the protocol channel for stdio servers).
// Output goes to stderr, or to a file when CPPMCP_LOG_FILE is set. Child
// processes inherit the environment, so client and server can append to the
// same file (each record is written with a single unbuffered write).
//
// Environment:
//   CPPMCP_LOG_LEVEL = trace | debug | info | warn | error | off
//   CPPMCP_LOG_FILE  = /path/to/file.log
//   CPPMCP_DEBUG     = (any value) legacy alias for CPPMCP_LOG_LEVEL=debug
//
// Usage:
//   CPPMCP_LOG_INFO("connected to " << name);
//   CPPMCP_LOG_DEBUG("send frame: " << json_value);   // anything streamable

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace cppmcp::core::log {

enum class Level : int { trace = 0, debug, info, warn, error, off };

inline const char* level_name(Level level) {
    switch (level) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info: return "INFO ";
        case Level::warn: return "WARN ";
        case Level::error: return "ERROR";
        case Level::off: return "OFF  ";
    }
    return "?????";
}

inline Level parse_level(std::string_view text, Level fallback) {
    std::string value(text);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "trace") return Level::trace;
    if (value == "debug") return Level::debug;
    if (value == "info") return Level::info;
    if (value == "warn" || value == "warning") return Level::warn;
    if (value == "error") return Level::error;
    if (value == "off" || value == "none") return Level::off;
    return fallback;
}

namespace detail {

// Longest message body written to the log; longer ones are cut and annotated.
constexpr std::size_t kMaxMessageBytes = 8192;

inline Level level_from_env(Level fallback) {
    if (const char* value = std::getenv("CPPMCP_LOG_LEVEL"); value != nullptr && *value != '\0') {
        return parse_level(value, fallback);
    }
    if (std::getenv("CPPMCP_DEBUG") != nullptr) {
        return Level::debug;
    }
    return fallback;
}

struct State {
    std::mutex mutex;
    std::atomic<int> level{static_cast<int>(Level::warn)};
    std::FILE* sink = stderr;
    bool owns_sink = false;

    State() {
        level.store(static_cast<int>(level_from_env(Level::warn)));
        if (const char* path = std::getenv("CPPMCP_LOG_FILE"); path != nullptr && *path != '\0') {
            open_file(path);
        }
    }

    ~State() {
        std::lock_guard<std::mutex> lock(mutex);
        if (owns_sink && sink != nullptr) {
            std::fclose(sink);
        }
        sink = stderr;
        owns_sink = false;
    }

    bool open_file(const char* path) {
        std::FILE* file = std::fopen(path, "a");
        if (file == nullptr) {
            return false;
        }
        std::setvbuf(file, nullptr, _IONBF, 0);  // one write() per record
        std::lock_guard<std::mutex> lock(mutex);
        if (owns_sink && sink != nullptr) {
            std::fclose(sink);
        }
        sink = file;
        owns_sink = true;
        return true;
    }
};

inline State& state() {
    static State instance;
    return instance;
}

inline int process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

// Small sequential thread numbers (1, 2, 3...) are easier to read than raw ids.
inline int thread_number() {
    static std::atomic<int> next{1};
    thread_local const int id = next.fetch_add(1);
    return id;
}

inline std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                        .count() %
                    1000;
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(ms));
    return buffer;
}

inline std::string_view base_name(std::string_view path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

// "void cppmcp::transport::StdioClientTransport::start()" -> "StdioClientTransport::start"
inline std::string short_function_name(std::string_view pretty) {
    const auto paren = pretty.find('(');
    if (paren == std::string_view::npos) {
        return std::string(pretty);
    }
    std::size_t begin = paren;
    int depth = 0;
    while (begin > 0) {
        const char c = pretty[begin - 1];
        if (c == '>') {
            ++depth;
        } else if (c == '<') {
            --depth;
        } else if (c == ' ' && depth == 0) {
            break;
        }
        --begin;
    }
    std::string_view name = pretty.substr(begin, paren - begin);
    const auto last = name.rfind("::");
    if (last != std::string_view::npos && last >= 2) {
        const auto previous = name.rfind("::", last - 1);
        if (previous != std::string_view::npos) {
            name = name.substr(previous + 2);
        }
    }
    return std::string(name);
}

}  // namespace detail

// Re-reads CPPMCP_LOG_LEVEL / CPPMCP_DEBUG; `default_level` applies when neither is set.
inline void configure(Level default_level) {
    detail::state().level.store(static_cast<int>(detail::level_from_env(default_level)));
}

inline void set_level(Level level) { detail::state().level.store(static_cast<int>(level)); }

inline Level level() {
    return static_cast<Level>(detail::state().level.load(std::memory_order_relaxed));
}

inline bool enabled(Level candidate) {
    return static_cast<int>(candidate) >= static_cast<int>(level());
}

// Appends to `path` instead of writing to stderr. Returns false if it cannot be opened.
inline bool set_file(const std::string& path) { return detail::state().open_file(path.c_str()); }

// Writes one record. `function` may be __PRETTY_FUNCTION__ / __func__; it is shortened here.
inline void write(Level level_value, const char* file, int line, const char* function,
                  std::string_view message) {
    if (level_value == Level::off || !enabled(level_value)) {
        return;
    }

    // Keep every record on one line, and bound its size.
    std::string text;
    text.reserve(std::min(message.size(), detail::kMaxMessageBytes) + 48);
    std::size_t consumed = 0;
    for (const char c : message) {
        if (text.size() >= detail::kMaxMessageBytes) {
            break;
        }
        if (c == '\n') {
            text += "\\n";
        } else if (c == '\r') {
            text += "\\r";
        } else {
            text.push_back(c);
        }
        ++consumed;
    }
    if (consumed < message.size()) {
        text += "...[truncated ";
        text += std::to_string(message.size() - consumed);
        text += " bytes]";
    }

    std::string out;
    out.reserve(text.size() + 128);
    out += detail::timestamp_now();
    out += ' ';
    out += level_name(level_value);
    out += " pid=";
    out += std::to_string(detail::process_id());
    out += " tid=";
    out += std::to_string(detail::thread_number());
    out += ' ';
    out += detail::base_name(file != nullptr ? file : "?");
    out += ':';
    out += std::to_string(line);
    out += ' ';
    out += detail::short_function_name(function != nullptr ? function : "?");
    out += "() - ";
    out += text;
    out += '\n';

    auto& state = detail::state();
    std::lock_guard<std::mutex> lock(state.mutex);
    std::fwrite(out.data(), 1, out.size(), state.sink);
    std::fflush(state.sink);
}

}  // namespace cppmcp::core::log

#if defined(__GNUC__) || defined(__clang__)
#define CPPMCP_FUNC __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
#define CPPMCP_FUNC __FUNCSIG__
#else
#define CPPMCP_FUNC __func__
#endif

// `expr_` is a stream expression: CPPMCP_LOG_INFO("a=" << a << " b=" << b)
// The expression is only evaluated when the level is enabled.
#define CPPMCP_LOG(level_, expr_)                                                        \
    do {                                                                                 \
        if (::cppmcp::core::log::enabled(level_)) {                                      \
            std::ostringstream cppmcp_log_stream_;                                       \
            cppmcp_log_stream_ << expr_;                                                 \
            ::cppmcp::core::log::write(level_, __FILE__, __LINE__, CPPMCP_FUNC,         \
                                       cppmcp_log_stream_.str());                        \
        }                                                                                \
    } while (false)

#define CPPMCP_LOG_TRACE(expr_) CPPMCP_LOG(::cppmcp::core::log::Level::trace, expr_)
#define CPPMCP_LOG_DEBUG(expr_) CPPMCP_LOG(::cppmcp::core::log::Level::debug, expr_)
#define CPPMCP_LOG_INFO(expr_) CPPMCP_LOG(::cppmcp::core::log::Level::info, expr_)
#define CPPMCP_LOG_WARN(expr_) CPPMCP_LOG(::cppmcp::core::log::Level::warn, expr_)
#define CPPMCP_LOG_ERROR(expr_) CPPMCP_LOG(::cppmcp::core::log::Level::error, expr_)