#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppmcp/core/json_rpc.hpp"

namespace cppmcp::core {

enum class ProtocolEra {
    modern,
    legacy,
};

struct ProtocolVersion {
    std::string value;
    ProtocolEra era;
};

struct VersionNegotiation {
    std::optional<ProtocolVersion> selected;
    std::vector<std::string> supported;
    std::string requested;
};

[[nodiscard]] const std::vector<ProtocolVersion>& known_protocol_versions();
[[nodiscard]] std::optional<ProtocolVersion> highest_common_version(
    const std::vector<std::string>& client_versions,
    const std::vector<std::string>& server_versions);
[[nodiscard]] VersionNegotiation negotiate_version(
    std::string requested, const std::vector<std::string>& server_versions);
[[nodiscard]] RpcError unsupported_protocol_version_error(
    std::string requested, const std::vector<std::string>& supported);

}  // namespace cppmcp::core
