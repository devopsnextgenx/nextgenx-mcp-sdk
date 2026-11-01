#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cppmcp::core {

class UriTemplate {
public:
    using Variables = std::map<std::string, nlohmann::json>;

    static UriTemplate parse(std::string value);

    [[nodiscard]] std::string expand(const nlohmann::json& variables) const;
    [[nodiscard]] std::optional<Variables> match(std::string_view uri) const;
    [[nodiscard]] const std::string& source() const noexcept { return source_; }

private:
    struct VariableSpec {
        std::string name;
        bool explode = false;
        std::optional<std::size_t> prefix;
    };

    struct Expression {
        char op = '\0';
        std::vector<VariableSpec> variables;
    };

    struct Part {
        std::string literal;
        std::optional<Expression> expression;
    };

    explicit UriTemplate(std::string source, std::vector<Part> parts)
        : source_(std::move(source)), parts_(std::move(parts)) {}

    std::string source_;
    std::vector<Part> parts_;
};

}  // namespace cppmcp::core
