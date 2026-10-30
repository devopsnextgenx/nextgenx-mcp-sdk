#include "cppmcp/core/json_schema.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool policy_skip(std::string_view reason) {
    return reason.find("remote schema $ref is disabled") != std::string_view::npos ||
           reason.find("cyclic schema $ref is disabled") != std::string_view::npos ||
           reason.find("duplicate schema anchors are disabled") != std::string_view::npos ||
           reason.find("unsupported JSON Schema dialect") != std::string_view::npos ||
           reason.find("safe regex subset") != std::string_view::npos ||
           reason.find("configured") != std::string_view::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    if (!input) return 2;
    const auto groups = nlohmann::json::parse(input);
    auto validator = cppmcp::core::make_json_schema_validator();
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const auto& group : groups) {
        std::shared_ptr<const cppmcp::core::ICompiledSchema> compiled;
        try {
            compiled = validator->compile(group.at("schema"));
        } catch (const std::exception& error) {
            if (policy_skip(error.what())) {
                skipped += group.at("tests").size();
                std::cout << "SKIP " << group.at("description") << ": " << error.what() << '\n';
            } else {
                failed += group.at("tests").size();
                std::cerr << "COMPILE_FAILURE " << group.at("description") << ": " << error.what() << '\n';
            }
            continue;
        }
        for (const auto& test : group.at("tests")) {
            try {
                const bool valid = !compiled->validate(test.at("data"));
                if (valid == test.at("valid").get<bool>()) {
                    ++passed;
                } else {
                    ++failed;
                    std::cerr << "FAIL " << group.at("description") << " / "
                              << test.at("description") << '\n';
                }
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "VALIDATION_FAILURE " << test.at("description") << ": " << error.what() << '\n';
            }
        }
    }
    std::cout << "SCHEMA_SUITE " << std::filesystem::path(argv[1]).filename().string()
              << " passed=" << passed << " failed=" << failed << " skipped=" << skipped << '\n';
    return failed != 0 ? 1 : passed == 0 ? 77 : 0;
}