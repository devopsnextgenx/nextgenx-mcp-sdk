#include "cppmcp/transport/http.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace cppmcp::transport {

struct SubscriptionRegistry::Impl {
    struct LegacyQueue {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<nlohmann::json> queue;
        std::size_t queued_bytes = 0;
        bool closed = false;
        SubscriptionReadStatus close_status = SubscriptionReadStatus::disconnected;
    };

    struct LegacySession {
        bool tools_list_changed = false;
        std::deque<nlohmann::json> pending;
        std::size_t pending_bytes = 0;
        std::shared_ptr<LegacyQueue> stream;
        std::uint64_t stream_id = 0;
    };

    struct Subscriber {
        std::string peer;
        nlohmann::json subscription_id;
        SubscriptionFilter filter;
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<nlohmann::json> queue;
        std::size_t queued_bytes = 0;
        bool closed = false;
        SubscriptionReadStatus close_status = SubscriptionReadStatus::disconnected;
    };

    Impl(std::size_t global_max, std::size_t peer_max, std::size_t queue_max)
        : global_limit(global_max), per_peer_limit(peer_max), queue_max_bytes(queue_max) {
        if (global_limit == 0 || per_peer_limit == 0 || queue_max_bytes == 0) {
            throw std::invalid_argument("subscription limits must be greater than zero");
        }
    }

    std::size_t global_limit;
    std::size_t per_peer_limit;
    std::size_t queue_max_bytes;
    mutable std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Subscriber>> subscribers;
    std::map<std::string, std::size_t> peers;
    std::map<std::string, LegacySession> legacy_sessions;
    std::uint64_t next_id = 1;

    std::vector<std::shared_ptr<Subscriber>> matching(
        const std::function<bool(const Subscriber&)>& predicate) const {
        std::vector<std::shared_ptr<Subscriber>> result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            result.reserve(subscribers.size());
            for (const auto& [_, subscriber] : subscribers) {
                if (predicate(*subscriber)) {
                    result.push_back(subscriber);
                }
            }
        }
        return result;
    }

    void publish(std::string method, const nlohmann::json& params,
                 const std::function<bool(const Subscriber&)>& predicate) {
        const auto targets = matching(predicate);
        for (const auto& subscriber : targets) {
            nlohmann::json message{
                {"jsonrpc", "2.0"},
                {"method", method},
                {"params", params},
            };
            message["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] =
                subscriber->subscription_id;
            const auto bytes = message.dump().size();
            std::lock_guard<std::mutex> lock(subscriber->mutex);
            if (subscriber->closed) {
                continue;
            }
            if (bytes > queue_max_bytes - std::min(queue_max_bytes, subscriber->queued_bytes)) {
                subscriber->closed = true;
                subscriber->close_status = SubscriptionReadStatus::overflow;
                subscriber->queue.clear();
                subscriber->queued_bytes = 0;
            } else {
                subscriber->queue.push_back(std::move(message));
                subscriber->queued_bytes += bytes;
            }
            subscriber->condition.notify_one();
        }
    }

    void enqueue_legacy(LegacySession& session, const nlohmann::json& message) {
        const auto bytes = message.dump().size();
        if (bytes > queue_max_bytes) {
            return;
        }
        while (session.pending_bytes + bytes > queue_max_bytes && !session.pending.empty()) {
            session.pending_bytes -= session.pending.front().dump().size();
            session.pending.pop_front();
        }
        session.pending.push_back(message);
        session.pending_bytes += bytes;
    }

    void notify_legacy_tools() {
        const nlohmann::json message{
            {"jsonrpc", "2.0"},
            {"method", "notifications/tools/list_changed"},
            {"params", nlohmann::json::object()},
        };
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [_, session] : legacy_sessions) {
            if (!session.tools_list_changed) {
                continue;
            }
            if (!session.stream) {
                enqueue_legacy(session, message);
                continue;
            }
            auto stream = session.stream;
            bool overflow = false;
            {
                std::lock_guard<std::mutex> stream_lock(stream->mutex);
                const auto bytes = message.dump().size();
                if (stream->closed) {
                    overflow = true;
                } else if (bytes > queue_max_bytes -
                                       std::min(queue_max_bytes, stream->queued_bytes)) {
                    stream->queue.clear();
                    stream->queued_bytes = 0;
                    stream->closed = true;
                    stream->close_status = SubscriptionReadStatus::overflow;
                    overflow = true;
                } else {
                    stream->queue.push_back(message);
                    stream->queued_bytes += bytes;
                }
            }
            stream->condition.notify_one();
            if (overflow) {
                session.stream.reset();
                enqueue_legacy(session, message);
            }
        }
    }
};

SubscriptionRegistry::SubscriptionRegistry(std::size_t global_limit,
                                           std::size_t per_peer_limit,
                                           std::size_t queue_max_bytes)
    : impl_(std::make_unique<Impl>(global_limit, per_peer_limit, queue_max_bytes)) {}

SubscriptionRegistry::~SubscriptionRegistry() {
    close_all();
}

std::uint64_t SubscriptionRegistry::subscribe(std::string peer,
                                             nlohmann::json subscription_id,
                                             SubscriptionFilter filter) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->subscribers.size() >= impl_->global_limit) {
        throw SubscriptionLimitError("global subscription limit reached");
    }
    auto& peer_count = impl_->peers[peer];
    if (peer_count >= impl_->per_peer_limit) {
        throw SubscriptionLimitError("per-peer subscription limit reached");
    }
    const auto id = impl_->next_id++;
    auto subscriber = std::make_shared<Impl::Subscriber>();
    subscriber->peer = std::move(peer);
    subscriber->subscription_id = std::move(subscription_id);
    subscriber->filter = std::move(filter);
    ++peer_count;
    impl_->subscribers.emplace(id, std::move(subscriber));
    return id;
}

SubscriptionReadStatus SubscriptionRegistry::next(std::uint64_t subscriber_id,
                                                  nlohmann::json& event,
                                                  std::chrono::milliseconds timeout) {
    std::shared_ptr<Impl::Subscriber> subscriber;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->subscribers.find(subscriber_id);
        if (found == impl_->subscribers.end()) {
            return SubscriptionReadStatus::disconnected;
        }
        subscriber = found->second;
    }
    std::unique_lock<std::mutex> lock(subscriber->mutex);
    subscriber->condition.wait_for(lock, timeout, [&] {
        return subscriber->closed || !subscriber->queue.empty();
    });
    if (!subscriber->queue.empty()) {
        event = std::move(subscriber->queue.front());
        subscriber->queued_bytes -= event.dump().size();
        subscriber->queue.pop_front();
        return SubscriptionReadStatus::event;
    }
    if (subscriber->closed) {
        return subscriber->close_status;
    }
    return SubscriptionReadStatus::timeout;
}

void SubscriptionRegistry::unsubscribe(std::uint64_t subscriber_id) {
    std::shared_ptr<Impl::Subscriber> subscriber;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->subscribers.find(subscriber_id);
        if (found == impl_->subscribers.end()) {
            return;
        }
        subscriber = found->second;
        const auto peer = impl_->peers.find(subscriber->peer);
        if (peer != impl_->peers.end()) {
            if (--peer->second == 0) {
                impl_->peers.erase(peer);
            }
        }
        impl_->subscribers.erase(found);
    }
    {
        std::lock_guard<std::mutex> lock(subscriber->mutex);
        subscriber->closed = true;
        subscriber->close_status = SubscriptionReadStatus::disconnected;
        subscriber->queue.clear();
        subscriber->queued_bytes = 0;
    }
    subscriber->condition.notify_all();
}

void SubscriptionRegistry::notify_tools_list_changed() {
    impl_->publish("notifications/tools/list_changed", nlohmann::json::object(),
                   [](const auto& subscriber) {
                       return subscriber.filter.tools_list_changed;
                   });
    impl_->notify_legacy_tools();
}

void SubscriptionRegistry::notify_prompts_list_changed() {
    impl_->publish("notifications/prompts/list_changed", nlohmann::json::object(),
                   [](const auto& subscriber) {
                       return subscriber.filter.prompts_list_changed;
                   });
}

void SubscriptionRegistry::notify_resources_list_changed() {
    impl_->publish("notifications/resources/list_changed", nlohmann::json::object(),
                   [](const auto& subscriber) {
                       return subscriber.filter.resources_list_changed;
                   });
}

void SubscriptionRegistry::notify_resource_updated(std::string_view uri) {
    nlohmann::json params{{"uri", uri}};
    impl_->publish("notifications/resources/updated", params,
                   [uri](const auto& subscriber) {
                       return std::find(subscriber.filter.resource_subscriptions.begin(),
                                        subscriber.filter.resource_subscriptions.end(), uri) !=
                              subscriber.filter.resource_subscriptions.end();
                   });
}

void SubscriptionRegistry::register_legacy_session(std::string session_id,
                                                    bool tools_list_changed) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::LegacySession session;
    session.tools_list_changed = tools_list_changed;
    impl_->legacy_sessions.insert_or_assign(std::move(session_id), std::move(session));
}

void SubscriptionRegistry::remove_legacy_session(std::string_view session_id) {
    std::shared_ptr<Impl::LegacyQueue> stream;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->legacy_sessions.find(std::string(session_id));
        if (found == impl_->legacy_sessions.end()) {
            return;
        }
        stream = found->second.stream;
        impl_->legacy_sessions.erase(found);
    }
    if (stream) {
        {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->closed = true;
            stream->close_status = SubscriptionReadStatus::disconnected;
        }
        stream->condition.notify_all();
    }
}

LegacyStreamToken SubscriptionRegistry::open_legacy_stream(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->legacy_sessions.find(std::string(session_id));
    if (found == impl_->legacy_sessions.end()) {
        return {};
    }
    auto& session = found->second;
    if (session.stream) {
        {
            std::lock_guard<std::mutex> stream_lock(session.stream->mutex);
            session.stream->closed = true;
            session.stream->close_status = SubscriptionReadStatus::disconnected;
        }
        session.stream->condition.notify_all();
    }
    session.stream = std::make_shared<Impl::LegacyQueue>();
    session.stream_id = impl_->next_id++;
    session.stream->queue = std::move(session.pending);
    session.stream->queued_bytes = session.pending_bytes;
    session.pending.clear();
    session.pending_bytes = 0;
    return {std::string(session_id), session.stream_id};
}

SubscriptionReadStatus SubscriptionRegistry::next_legacy_stream(
    const LegacyStreamToken& token, nlohmann::json& event,
    std::chrono::milliseconds timeout) {
    std::shared_ptr<Impl::LegacyQueue> stream;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto session = impl_->legacy_sessions.find(token.session_id);
        if (session == impl_->legacy_sessions.end() ||
            session->second.stream_id != token.stream_id) {
            return SubscriptionReadStatus::disconnected;
        }
        stream = session->second.stream;
    }
    if (!stream) {
        return SubscriptionReadStatus::disconnected;
    }
    std::unique_lock<std::mutex> lock(stream->mutex);
    stream->condition.wait_for(lock, timeout, [&] {
        return stream->closed || !stream->queue.empty();
    });
    if (!stream->queue.empty()) {
        event = std::move(stream->queue.front());
        stream->queued_bytes -= event.dump().size();
        stream->queue.pop_front();
        return SubscriptionReadStatus::event;
    }
    if (stream->closed) {
        return stream->close_status;
    }
    return SubscriptionReadStatus::timeout;
}

void SubscriptionRegistry::close_legacy_stream(const LegacyStreamToken& token) {
    std::shared_ptr<Impl::LegacyQueue> stream;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto session = impl_->legacy_sessions.find(token.session_id);
        if (session == impl_->legacy_sessions.end() ||
            session->second.stream_id != token.stream_id) {
            return;
        }
        stream = session->second.stream;
        session->second.stream.reset();
    }
    if (stream) {
        {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->closed = true;
            stream->close_status = SubscriptionReadStatus::disconnected;
        }
        stream->condition.notify_all();
    }
}

void SubscriptionRegistry::close_all() {
    std::vector<std::shared_ptr<Impl::Subscriber>> subscribers;
    std::vector<std::shared_ptr<Impl::LegacyQueue>> legacy_streams;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        subscribers.reserve(impl_->subscribers.size());
        for (const auto& [_, subscriber] : impl_->subscribers) {
            subscribers.push_back(subscriber);
        }
        for (const auto& [_, session] : impl_->legacy_sessions) {
            if (session.stream) {
                legacy_streams.push_back(session.stream);
            }
        }
    }
    for (const auto& subscriber : subscribers) {
        {
            std::lock_guard<std::mutex> lock(subscriber->mutex);
            subscriber->closed = true;
            subscriber->close_status = SubscriptionReadStatus::shutdown;
        }
        subscriber->condition.notify_all();
    }
    for (const auto& stream : legacy_streams) {
        {
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->closed = true;
            stream->close_status = SubscriptionReadStatus::shutdown;
        }
        stream->condition.notify_all();
    }
}

std::size_t SubscriptionRegistry::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->subscribers.size();
}

}  // namespace cppmcp::transport
