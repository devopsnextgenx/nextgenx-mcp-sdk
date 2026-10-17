#include "cppmcp/core/version.hpp"

#include <algorithm>
#include <utility>

namespace cppmcp::core {

const std::vector<ProtocolVersion>& known_protocol_versions() {
#if CPPMCP_ENABLE_LEGACY
    static const std::vector<ProtocolVersion> versions{
        {"2026-07-28", ProtocolEra::modern},
        {"2025-11-25", ProtocolEra::legacy},
        {"2025-06-18", ProtocolEra::legacy},
        {"2025-03-26", ProtocolEra::legacy},
        {"2024-11-05", ProtocolEra::legacy},
    };
#else
    static const std::vector<ProtocolVersion> versions{
        {"2026-07-28", ProtocolEra::modern},
    };
#endif
    return versions;
}

std::optional<ProtocolVersion> highest_common_version(
    const std::vector<std::string>& client_versions,
    const std::vector<std::string>& server_versions) {
    const auto& supported = known_protocol_versions();
    const auto common = std::find_if(
        supported.begin(), supported.end(), [&](const ProtocolVersion& version) {
            return std::find(client_versions.begin(), client_versions.end(), version.value) !=
                       client_versions.end() &&
                   std::find(server_versions.begin(), server_versions.end(), version.value) !=
                       server_versions.end();
        });
    if (common == supported.end()) {
        return std::nullopt;
    }
    return *common;
}

VersionNegotiation negotiate_version(std::string requested,
                                     const std::vector<std::string>& server_versions) {
    VersionNegotiation result;
    result.requested = std::move(requested);
    result.supported = server_versions;
    const auto version = std::find_if(
        known_protocol_versions().begin(), known_protocol_versions().end(),
        [&](const ProtocolVersion& candidate) {
            return candidate.value == result.requested &&
                   std::find(server_versions.begin(), server_versions.end(), candidate.value) !=
                       server_versions.end();
        });
    if (version != known_protocol_versions().end()) {
        result.selected = *version;
    }
    return result;
}

RpcError unsupported_protocol_version_error(std::string requested,
                                            const std::vector<std::string>& supported) {
    Json versions = Json::array();
    for (const auto& version : supported) {
        versions.push_back(version);
    }
    return protocol_error(ErrorCode::unsupported_protocol_version,
                          "Unsupported protocol version",
                          Json{{"requested", std::move(requested)},
                               {"supported", std::move(versions)}});
}

}  // namespace cppmcp::core
