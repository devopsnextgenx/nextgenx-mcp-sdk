#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "cppmcp/transport/http.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) return 0;
    cppmcp::transport::SubscriptionRegistry registry(4, 4, 16384);
    cppmcp::transport::SubscriptionFilter filter;
    if (size != 0) {
        filter.resource_subscriptions.emplace_back(
            reinterpret_cast<const char*>(data), size);
    }
    try {
        const auto id = registry.subscribe("fuzzer", "fuzz-subscription", std::move(filter));
        registry.notify_resource_updated(std::string_view(
            reinterpret_cast<const char*>(data), size));
        nlohmann::json event;
        (void)registry.next(id, event, std::chrono::milliseconds(0));
    } catch (const cppmcp::transport::SubscriptionLimitError&) {
    } catch (const std::invalid_argument&) {
    }
    return 0;
}
