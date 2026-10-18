#include "test.hpp"

#include <chrono>
#include <string>

#include "cppmcp/transport/http.hpp"

void test_listen_registry() {
    using cppmcp::transport::SubscriptionFilter;
    using cppmcp::transport::SubscriptionReadStatus;
    using cppmcp::transport::SubscriptionRegistry;

    SubscriptionRegistry registry(3, 2, 1024);
    SubscriptionFilter tools_filter;
    tools_filter.tools_list_changed = true;
    const auto first = registry.subscribe("peer-a", 11, tools_filter);
    const auto second = registry.subscribe("peer-b", 22, tools_filter);
    SubscriptionFilter prompt_filter;
    prompt_filter.prompts_list_changed = true;
    const auto prompts = registry.subscribe("peer-a", 33, prompt_filter);

    registry.notify_tools_list_changed();
    nlohmann::json first_event;
    nlohmann::json second_event;
    nlohmann::json no_event;
    CPPMCP_CHECK(registry.next(first, first_event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(registry.next(second, second_event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(registry.next(prompts, no_event, std::chrono::milliseconds(0)) ==
                 SubscriptionReadStatus::timeout);
    CPPMCP_CHECK(first_event["method"] == "notifications/tools/list_changed");
    CPPMCP_CHECK(first_event["params"]["_meta"]
                         ["io.modelcontextprotocol/subscriptionId"] == 11);
    CPPMCP_CHECK(second_event["params"]["_meta"]
                          ["io.modelcontextprotocol/subscriptionId"] == 22);

    registry.notify_prompts_list_changed();
    CPPMCP_CHECK(registry.next(prompts, no_event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::event);
    CPPMCP_CHECK(no_event["method"] == "notifications/prompts/list_changed");

    SubscriptionRegistry limited(2, 1, 1024);
    const auto peer_subscription = limited.subscribe("same-peer", 1, tools_filter);
    bool peer_limited = false;
    try {
        (void)limited.subscribe("same-peer", 2, tools_filter);
    } catch (const cppmcp::transport::SubscriptionLimitError&) {
        peer_limited = true;
    }
    CPPMCP_CHECK(peer_limited);
    (void)limited.subscribe("other-peer", 3, tools_filter);
    bool globally_limited = false;
    try {
        (void)limited.subscribe("third-peer", 4, tools_filter);
    } catch (const cppmcp::transport::SubscriptionLimitError&) {
        globally_limited = true;
    }
    CPPMCP_CHECK(globally_limited);
    limited.unsubscribe(peer_subscription);

    SubscriptionRegistry overflowing(2, 2, 8);
    const auto slow = overflowing.subscribe("slow", 5, tools_filter);
    overflowing.notify_tools_list_changed();
    CPPMCP_CHECK(overflowing.next(slow, no_event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::overflow);
    overflowing.unsubscribe(slow);
    CPPMCP_CHECK(overflowing.size() == 0);

    SubscriptionRegistry shutdown(1, 1, 1024);
    const auto active = shutdown.subscribe("shutdown", 6, tools_filter);
    shutdown.close_all();
    CPPMCP_CHECK(shutdown.next(active, no_event, std::chrono::milliseconds(10)) ==
                 SubscriptionReadStatus::shutdown);
}
