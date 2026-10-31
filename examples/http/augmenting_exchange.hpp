#pragma once

// IHttpExchange decorator: when the SDK answers discover/initialize with a JSON body, add the
// capabilities served by the FeatureRegistry (prompts, resources, completions, tasks).
// SSE responses and non-JSON bodies pass through untouched.

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cppmcp/transport/http.hpp"
#include "../common/mcp_features.hpp"

namespace cppmcp::examples {

class AugmentingExchange final : public transport::IHttpExchange {
public:
    AugmentingExchange(std::shared_ptr<transport::IHttpExchange> inner, const FeatureRegistry& registry)
        : inner_(std::move(inner)), registry_(registry) {}

    const transport::HttpRequest& request() const override { return inner_->request(); }

    void respond(int status, transport::Headers headers, std::string body) override {
        if (status == 200 && !body.empty()) {
            auto json = Json::parse(body, nullptr, /*allow_exceptions=*/false);
            if (json.is_object() && json.contains("result") && json["result"].is_object()) {
                registry_.augment_discover(json["result"]);  // no-op unless it has capabilities+serverInfo
                body = json.dump();
            }
        }
        inner_->respond(status, std::move(headers), std::move(body));
    }

    transport::SseStream& begin_sse(int status, transport::Headers headers) override {
        return inner_->begin_sse(status, std::move(headers));
    }
    void on_disconnect(std::function<void()> callback) override { inner_->on_disconnect(std::move(callback)); }

private:
    std::shared_ptr<transport::IHttpExchange> inner_;
    const FeatureRegistry& registry_;
};

}  // namespace cppmcp::examples
