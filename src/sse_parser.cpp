#include "cppmcp/transport/http.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace cppmcp::transport {
namespace {

constexpr std::string_view utf8_bom{"\xef\xbb\xbf", 3};

bool contains_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

}  // namespace

SseParser::SseParser(std::size_t max_event_bytes) : max_event_bytes_(max_event_bytes) {
    if (max_event_bytes_ == 0) {
        throw std::invalid_argument("SSE maximum event size must be greater than zero");
    }
}

void SseParser::reset() {
    event_bytes_ = 0;
    bom_prefix_.clear();
    line_.clear();
    data_.clear();
    event_type_.clear();
    last_event_id_.clear();
    retry_.reset();
    bom_complete_ = false;
    skip_lf_ = false;
    has_data_ = false;
}

std::vector<SseEvent> SseParser::feed(std::string_view bytes) {
    std::vector<SseEvent> events;
    for (const char byte : bytes) {
        if (!bom_complete_) {
            bom_prefix_.push_back(byte);
            if (utf8_bom.substr(0, bom_prefix_.size()) == bom_prefix_) {
                if (bom_prefix_.size() == utf8_bom.size()) {
                    bom_prefix_.clear();
                    bom_complete_ = true;
                }
                continue;
            }
            bom_complete_ = true;
            for (const char prefix_byte : bom_prefix_) {
                consume_byte(prefix_byte, events);
            }
            bom_prefix_.clear();
            continue;
        }
        consume_byte(byte, events);
    }
    return events;
}

void SseParser::consume_byte(char byte, std::vector<SseEvent>& events) {
    if (skip_lf_) {
        skip_lf_ = false;
        if (byte == '\n') {
            return;
        }
    }

    if (event_bytes_ == max_event_bytes_) {
        throw std::length_error("SSE event exceeds configured maximum size");
    }
    ++event_bytes_;

    if (byte == '\r' || byte == '\n') {
        consume_line(events);
        skip_lf_ = byte == '\r';
        return;
    }
    line_.push_back(byte);
}

void SseParser::consume_line(std::vector<SseEvent>& events) {
    if (line_.empty()) {
        if (has_data_) {
            if (!data_.empty()) {
                data_.pop_back();
            }
            events.push_back(SseEvent{
                event_type_.empty() ? "message" : std::move(event_type_),
                std::move(data_),
                last_event_id_,
                retry_,
            });
        }
        data_.clear();
        event_type_.clear();
        has_data_ = false;
        event_bytes_ = 0;
        return;
    }

    if (line_.front() == ':') {
        line_.clear();
        return;
    }
    const auto separator = line_.find(':');
    const std::string_view field(line_.data(),
                                 separator == std::string::npos ? line_.size() : separator);
    std::string_view value;
    if (separator != std::string::npos) {
        value = std::string_view(line_).substr(separator + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }

    if (field == "data") {
        data_.append(value);
        data_.push_back('\n');
        has_data_ = true;
    } else if (field == "event") {
        event_type_.assign(value);
    } else if (field == "id") {
        if (!contains_nul(value)) {
            last_event_id_.assign(value);
        }
    } else if (field == "retry" && !value.empty()) {
        std::uint64_t milliseconds = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                            milliseconds);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
            milliseconds <= static_cast<std::uint64_t>(
                                std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
            retry_ = std::chrono::milliseconds(
                static_cast<std::chrono::milliseconds::rep>(milliseconds));
        }
    }
    line_.clear();
}

}  // namespace cppmcp::transport
