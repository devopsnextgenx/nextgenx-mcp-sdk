#pragma once

#include <map>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cppmcp::auth {

using ScopeSet = std::set<std::string>;

struct RequestView {
    std::string method;
    std::string name;
    std::string uri;
    nlohmann::json arguments = nlohmann::json::object();
    nlohmann::json params = nlohmann::json::object();
};

using DynamicScopeRequirements = std::function<ScopeSet(const RequestView&)>;

[[nodiscard]] bool valid_scope_token(std::string_view scope) noexcept;
[[nodiscard]] ScopeSet parse_scope_set(std::string_view value);
void validate_scope_set(const ScopeSet& scopes);

class ScopeHierarchy {
public:
    void add_implication(std::string broader, std::string narrower);
    [[nodiscard]] bool satisfies(const ScopeSet& granted,
                                 std::string_view required) const;
    [[nodiscard]] ScopeSet missing(const ScopeSet& granted,
                                   const ScopeSet& required) const;

private:
    std::map<std::string, ScopeSet> implications_;
};

}  // namespace cppmcp::auth
