#include "cppmcp/core/version.hpp"
#include "test.hpp"

void test_versions() {
    using namespace cppmcp::core;

    const auto common = highest_common_version(
        {"2025-11-25", "2026-07-28"},
        {"2025-11-25", "2026-07-28"});
    CPPMCP_CHECK(common.has_value());
    CPPMCP_CHECK(common->value == "2026-07-28");
    CPPMCP_CHECK(common->era == ProtocolEra::modern);

#if CPPMCP_ENABLE_LEGACY
    const auto legacy = highest_common_version(
        {"2024-11-05", "2025-06-18"},
        {"2025-11-25", "2025-06-18"});
    CPPMCP_CHECK(legacy.has_value());
    CPPMCP_CHECK(legacy->value == "2025-06-18");
    CPPMCP_CHECK(legacy->era == ProtocolEra::legacy);
#else
    CPPMCP_CHECK(!highest_common_version(
                      {"2024-11-05", "2025-06-18"},
                      {"2025-11-25", "2025-06-18"})
                      .has_value());
#endif

    CPPMCP_CHECK(!highest_common_version({"2030-01-01"}, {"2026-07-28"}).has_value());

    const auto mismatch = unsupported_protocol_version_error(
        "1900-01-01", {"2026-07-28", "2025-11-25"});
    CPPMCP_CHECK(mismatch.code == -32022);
    CPPMCP_CHECK(mismatch.data->at("requested") == "1900-01-01");
    CPPMCP_CHECK(mismatch.data->at("supported").size() == 2);

    const auto negotiated = negotiate_version(" 2026-07-28 ",
                                             {"2025-11-25", "2026-07-28"});
    CPPMCP_CHECK(negotiated.selected.has_value());
    CPPMCP_CHECK(negotiated.selected->value == "2026-07-28");
    CPPMCP_CHECK(negotiated.requested == "2026-07-28");

    const auto duplicate_common = highest_common_version(
        {"2026-07-28", "2026-07-28", "2025-11-25"},
        {" 2025-11-25 ", "2026-07-28"});
    CPPMCP_CHECK(duplicate_common.has_value());
    CPPMCP_CHECK(duplicate_common->value == "2026-07-28");
}
