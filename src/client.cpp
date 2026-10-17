#include "cppmcp/client.hpp"

#include <stdexcept>

namespace cppmcp {

Client::Client(ClientInfo info, NegotiationPolicy policy)
    : info_(std::move(info)), policy_(policy) {}

void Client::connect(std::shared_ptr<cppmcp::core::ITransport> transport) {
    transport_ = std::move(transport);
    connection_ = std::make_shared<cppmcp::core::Connection>(transport_);
    connection_->start();
}

void Client::close() {
    if (connection_) {
        connection_->close();
    }
}

nlohmann::json Client::discover() {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("server/discover", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", {{}}}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

nlohmann::json Client::list_tools() {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("tools/list", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", {{}}}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

nlohmann::json Client::call_tool(const std::string& name, const nlohmann::json& args) {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("tools/call", {{"name", name}, {"arguments", args}, {"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", {{}}}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

nlohmann::json Client::ping() {
    if (!connection_) {
        throw std::runtime_error("client is not connected");
    }
    auto future = connection_->send_request("ping", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", {{}}}, {"io.modelcontextprotocol/clientInfo", {{"name", info_.name}, {"title", info_.title}, {"version", info_.version}}}}}});
    return future.get();
}

}  // namespace cppmcp
