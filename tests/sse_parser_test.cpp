#include "test.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "cppmcp/transport/http.hpp"

namespace {

using cppmcp::transport::SseEvent;
using cppmcp::transport::SseParser;

bool same(const std::vector<SseEvent>& left, const std::vector<SseEvent>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].event != right[i].event || left[i].data != right[i].data ||
            left[i].last_event_id != right[i].last_event_id ||
            left[i].retry != right[i].retry) {
            return false;
        }
    }
    return true;
}

}  // namespace

void test_sse_parser() {
    struct Case {
        std::string input;
        std::vector<SseEvent> expected;
    };
    const std::vector<Case> cases{
        {"data: hello\n\n", {{"message", "hello", "", std::nullopt}}},
        {"event: update\rdata: one\rdata: two\r\r",
         {{"update", "one\ntwo", "", std::nullopt}}},
        {": keepalive\n\n", {}},
        {"data:\n\n", {{"message", "", "", std::nullopt}}},
        {"data: leading: colon\n\n",
         {{"message", "leading: colon", "", std::nullopt}}},
        {"data: one\nunknown: ignored\ndata: two\n\n",
         {{"message", "one\ntwo", "", std::nullopt}}},
        {"id: 1\ndata: first\n\ndata: second\n\n",
         {{"message", "first", "1", std::nullopt},
          {"message", "second", "1", std::nullopt}}},
        {"id: 1\nid:\ndata: blank id\n\n",
         {{"message", "blank id", "", std::nullopt}}},
        {"retry: 125\nretry: 1x\ndata: ready\n\n",
         {{"message", "ready", "", std::chrono::milliseconds(125)}}},
        {"event:\ndata: default name\n\n",
         {{"message", "default name", "", std::nullopt}}},
    };

    for (const auto& test : cases) {
        SseParser parser;
        const auto actual = parser.feed(test.input);
        CPPMCP_CHECK(same(actual, test.expected));
        SseParser byte_parser;
        std::vector<SseEvent> bytewise;
        for (const char byte : test.input) {
            const auto part = byte_parser.feed(std::string_view(&byte, 1));
            bytewise.insert(bytewise.end(), part.begin(), part.end());
        }
        CPPMCP_CHECK(same(actual, bytewise));
    }

    SseParser nul_id_parser;
    std::string nul_id_stream = "id: invalid";
    nul_id_stream.push_back('\0');
    nul_id_stream += "id\ndata: one\n\ndata: two\n\n";
    const auto nul_id_events = nul_id_parser.feed(nul_id_stream);
    CPPMCP_CHECK(nul_id_events.size() == 2);
    if (nul_id_events.size() == 2) {
        CPPMCP_CHECK(nul_id_events[0].last_event_id.empty());
        CPPMCP_CHECK(nul_id_events[1].last_event_id.empty());
    }

    const std::string bom_stream = "\xef\xbb\xbf" "data: bom\n\n";
    SseParser whole_parser;
    const auto whole = whole_parser.feed(bom_stream);
    SseParser byte_parser;
    std::vector<SseEvent> bytewise;
    for (const char byte : bom_stream) {
        const auto part = byte_parser.feed(std::string_view(&byte, 1));
        bytewise.insert(bytewise.end(), part.begin(), part.end());
    }
    CPPMCP_CHECK(whole.size() == 1);
    CPPMCP_CHECK(same(whole, bytewise));

    SseParser truncated_parser;
    CPPMCP_CHECK(truncated_parser.feed("data: unfinished").empty());

    SseParser limited_parser(5);
    bool size_error = false;
    try {
        (void)limited_parser.feed("data: too large\n\n");
    } catch (const std::length_error&) {
        size_error = true;
    }
    CPPMCP_CHECK(size_error);
}
