#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace cppmcp::net {

struct NetworkPolicy {
    bool require_https = true;
    bool allow_http_loopback = false;
    bool allow_ipv4_mapped = false;
    bool block_private_ranges = true;
    bool follow_redirects = false;
    std::size_t max_response_bytes = 256 * 1024;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds read_timeout{10000};
    std::vector<std::string> allowed_hosts;
};

class BlockedAddress : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

HttpResponse fetch_http(const std::string& url, const NetworkPolicy& policy,
                        std::string method = "GET",
                        std::map<std::string, std::string> headers = {},
                        std::string body = {});

}  // namespace cppmcp::net
