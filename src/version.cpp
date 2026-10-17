#include "cppmcp/core/version.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace cppmcp::core {
namespace {

std::string trim_version_string(std::string value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    return std::string(begin, end);
}

bool contains_version(const std::vector<std::string>& versions,
                      const std::string& target) {
    const std::string normalized_target = trim_version_string(target);
    if (normalized_target.empty()) {
        return false;
    }

    return std::any_of(versions.begin(), versions.end(), [&](const std::string& version) {
        return trim_version_string(version) == normalized_target;
    });
}

}  // namespace

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
            return contains_version(client_versions, version.value) &&
                   contains_version(server_versions, version.value);
        });
    if (common == supported.end()) {
        return std::nullopt;
    }
    return *common;
}

VersionNegotiation negotiate_version(std::string requested,
                                     const std::vector<std::string>& server_versions) {
    VersionNegotiation result;
    result.requested = trim_version_string(std::move(requested));
    result.supported = server_versions;

    const auto version = std::find_if(
        known_protocol_versions().begin(), known_protocol_versions().end(),
        [&](const ProtocolVersion& candidate) {
            return trim_version_string(candidate.value) == result.requested &&
                   contains_version(server_versions, candidate.value);
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
        versions.push_back(trim_version_string(version));
    }
    return protocol_error(ErrorCode::unsupported_protocol_version,
                          "Unsupported protocol version",
                          Json{{"requested", trim_version_string(std::move(requested))},
                               {"supported", std::move(versions)}});
}

}  // namespace cppmcp::core
