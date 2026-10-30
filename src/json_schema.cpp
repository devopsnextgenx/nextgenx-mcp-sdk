#include "cppmcp/core/json_schema.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace cppmcp::core {
namespace {

using Json = nlohmann::json;
using JsonconsJson = jsoncons::json;
namespace jsonschema = jsoncons::jsonschema;

struct RegexGroup {
    bool has_unbounded_quantifier = false;
    bool has_alternation = false;
};

bool is_repetition(std::string_view pattern, std::size_t position) {
    if (position >= pattern.size()) {
        return false;
    }
    if (pattern[position] == '*' || pattern[position] == '+' || pattern[position] == '?') {
        return true;
    }
    if (pattern[position] != '{') {
        return false;
    }
    const auto close = pattern.find('}', position + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    return true;
}

bool has_risky_repetition(std::string_view pattern) {
    std::vector<RegexGroup> groups(1);
    bool in_character_class = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const char ch = pattern[index];
        if (ch == '\\') {
            if (index + 1 < pattern.size() &&
                ((pattern[index + 1] >= '1' && pattern[index + 1] <= '9') ||
                 pattern[index + 1] == 'k' || pattern[index + 1] == 'p' || pattern[index + 1] == 'P')) {
                return true;
            }
            ++index;
            continue;
        }
        if (in_character_class) {
            if (ch == ']') {
                in_character_class = false;
            }
            continue;
        }
        if (ch == '[') {
            in_character_class = true;
        } else if (ch == '(') {
            if (index + 1 < pattern.size() && pattern[index + 1] == '?') {
                return true;
            }
            groups.push_back({});
        } else if (ch == '|') {
            groups.back().has_alternation = true;
        } else if (ch == ')') {
            if (groups.size() == 1) {
                continue;
            }
            const auto group = groups.back();
            groups.pop_back();
            const bool repeated = index + 1 < pattern.size() &&
                                  is_repetition(pattern, index + 1);
            if (repeated && (group.has_unbounded_quantifier || group.has_alternation)) {
                return true;
            }
            if (repeated) {
                if (groups.back().has_unbounded_quantifier) {
                    return true;
                }
                groups.back().has_unbounded_quantifier = true;
            } else if (group.has_unbounded_quantifier) {
                groups.back().has_unbounded_quantifier = true;
            }
            groups.back().has_alternation =
                groups.back().has_alternation || group.has_alternation;
        } else if (is_repetition(pattern, index)) {
            if (ch == '{') {
                auto cursor = index + 1;
                while (cursor < pattern.size() && pattern[cursor] != '}') {
                    if (pattern[cursor] == ',') {
                        ++cursor;
                        continue;
                    }
                    unsigned count = 0;
                    const auto parsed = std::from_chars(pattern.data() + cursor,
                                                         pattern.data() + pattern.size(), count);
                    if (parsed.ec != std::errc{} || count > 256 || parsed.ptr == pattern.data() + cursor) {
                        return true;
                    }
                    cursor = static_cast<std::size_t>(parsed.ptr - pattern.data());
                }
            }
            if (groups.back().has_unbounded_quantifier) {
                return true;
            }
            groups.back().has_unbounded_quantifier = true;
        }
    }
    return false;
}

template <typename Visitor>
void visit_subschemas(const Json& schema, Visitor visitor) {
    for (const auto* keyword : {"additionalProperties", "unevaluatedProperties", "propertyNames",
                                "contains", "additionalItems", "unevaluatedItems", "not", "if",
                                "then", "else", "contentSchema"}) {
        const auto child = schema.find(keyword);
        if (child != schema.end()) visitor(*child);
    }
    for (const auto* keyword : {"properties", "patternProperties", "$defs", "definitions",
                                "dependentSchemas", "dependencies"}) {
        const auto children = schema.find(keyword);
        if (children != schema.end() && children->is_object()) {
            for (const auto& child : *children) {
                if (!child.is_array()) visitor(child);
            }
        }
    }
    for (const auto* keyword : {"allOf", "anyOf", "oneOf", "prefixItems", "items"}) {
        const auto children = schema.find(keyword);
        if (children != schema.end()) {
            if (children->is_array()) {
                for (const auto& child : *children) visitor(child);
            } else {
                visitor(*children);
            }
        }
    }
}

void inspect_schema_depth(const Json& value, std::size_t depth, std::size_t limit) {
    if (depth > limit) {
        throw std::invalid_argument("schema nesting exceeds the configured depth limit");
    }
    if (value.is_structured()) {
        for (const auto& child : value) inspect_schema_depth(child, depth + 1, limit);
    }
}

std::string decode_fragment(std::string_view fragment) {
    std::string result;
    for (std::size_t index = 0; index < fragment.size(); ++index) {
        if (fragment[index] == '%') {
            if (index + 2 >= fragment.size()) throw std::invalid_argument("Invalid $ref fragment");
            unsigned byte = 0;
            const auto parsed = std::from_chars(fragment.data() + index + 1,
                                                fragment.data() + index + 3, byte, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != fragment.data() + index + 3) {
                throw std::invalid_argument("Invalid $ref fragment");
            }
            result.push_back(static_cast<char>(byte));
            index += 2;
        } else {
            result.push_back(fragment[index]);
        }
    }
    return result;
}

void inspect_patterns(const Json& node, const SchemaValidationLimits& limits,
                      bool& has_patterns) {
    if (node.is_object()) {
        const auto pattern = node.find("pattern");
        if (pattern != node.end()) {
            if (!pattern->is_string()) {
                throw std::invalid_argument("schema pattern must be a string");
            }
            const auto value = pattern->get_ref<const std::string&>();
            if (value.size() > limits.max_pattern_bytes || has_risky_repetition(value)) {
                throw std::invalid_argument("schema pattern exceeds the safe regex subset");
            }
            has_patterns = true;
        }
        const auto patterns = node.find("patternProperties");
        if (patterns != node.end()) {
            if (!patterns->is_object()) {
                throw std::invalid_argument("schema patternProperties must be an object");
            }
            for (auto item = patterns->begin(); item != patterns->end(); ++item) {
                if (item.key().size() > limits.max_pattern_bytes ||
                    has_risky_repetition(item.key())) {
                    throw std::invalid_argument(
                        "schema patternProperties exceeds the safe regex subset");
                }
                has_patterns = true;
            }
        }
        visit_subschemas(node, [&](const Json& child) { inspect_patterns(child, limits, has_patterns); });
    }
}

void collect_anchors(const Json& node, std::unordered_map<std::string, const Json*>& anchors) {
    if (!node.is_object()) return;
    for (const auto* keyword : {"$anchor", "$dynamicAnchor"}) {
        const auto anchor = node.find(keyword);
        if (anchor != node.end()) {
            if (!anchor->is_string()) throw std::invalid_argument("Schema anchors must be strings");
            const auto [existing, inserted] = anchors.emplace(anchor->get<std::string>(), &node);
            if (!inserted && existing->second != &node) {
                throw std::invalid_argument("duplicate schema anchors are disabled");
            }
        }
    }
    visit_subschemas(node, [&](const Json& child) { collect_anchors(child, anchors); });
}

void inspect_references(const Json& root, const Json& node, std::size_t schema_depth,
                        std::size_t reference_depth, const SchemaValidationLimits& limits,
                        std::unordered_set<std::string>& active_references,
                        const std::unordered_map<std::string, const Json*>& anchors) {
    if (schema_depth > limits.max_schema_depth) {
        throw std::invalid_argument("schema nesting exceeds the configured depth limit");
    }
    if (node.is_object()) {
        for (const auto* keyword : {"$ref", "$dynamicRef", "$recursiveRef"}) {
            const auto ref = node.find(keyword);
            if (ref == node.end()) continue;
            if (!ref->is_string()) {
                throw std::invalid_argument("schema $ref must be a string");
            }
            const auto reference = ref->get<std::string>();
            if (reference.empty() || reference.front() != '#') {
                throw std::invalid_argument("remote schema $ref is disabled");
            }
            if (reference_depth >= limits.max_reference_depth) {
                throw std::invalid_argument("schema $ref depth exceeds the configured limit");
            }
            const auto fragment = decode_fragment(std::string_view(reference).substr(1));
            const Json* target = nullptr;
            if (fragment.empty()) {
                target = &root;
            } else if (fragment.front() == '/') {
                try {
                    target = &root.at(Json::json_pointer(fragment));
                } catch (const std::exception&) {
                    throw std::invalid_argument("schema contains an unresolved local $ref");
                }
            } else {
                const auto anchor = anchors.find(fragment);
                if (anchor != anchors.end()) target = anchor->second;
            }
            if (target != nullptr) {
                if (!active_references.insert(reference).second) {
                    throw std::invalid_argument("cyclic schema $ref is disabled");
                }
                inspect_references(root, *target, 0, reference_depth + 1, limits,
                                   active_references, anchors);
                active_references.erase(reference);
            }
        }
        visit_subschemas(node, [&](const Json& child) {
            inspect_references(root, child, schema_depth + 1, reference_depth, limits,
                               active_references, anchors);
        });
    }
}

bool add_size(std::size_t amount, std::size_t limit, std::size_t& total) {
    if (amount > limit || total > limit - amount) {
        return false;
    }
    total += amount;
    return true;
}

bool inspect_instance(const Json& value, std::size_t depth, std::size_t& total,
                      const SchemaValidationLimits& limits, bool check_pattern_strings) {
    if (depth > limits.max_instance_depth || !add_size(1, limits.max_instance_bytes, total)) {
        return false;
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return (!check_pattern_strings || text.size() <= limits.max_pattern_input_bytes) &&
               add_size(text.size(), limits.max_instance_bytes, total);
    }
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            if ((check_pattern_strings && item.key().size() > limits.max_pattern_input_bytes) ||
                !add_size(item.key().size(), limits.max_instance_bytes, total) ||
                !inspect_instance(item.value(), depth + 1, total, limits,
                                  check_pattern_strings)) {
                return false;
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (!inspect_instance(item, depth + 1, total, limits, check_pattern_strings)) {
                return false;
            }
        }
    }
    return true;
}

void validate_dialect(const Json& schema) {
    const auto dialect = schema.find("$schema");
    if (dialect == schema.end()) {
        return;
    }
    if (!dialect->is_string()) {
        throw std::invalid_argument("schema $schema must be a string");
    }
    const auto value = dialect->get<std::string>();
    const std::array<std::string, 5> supported = {
        jsonschema::schema_version::draft4(),
        jsonschema::schema_version::draft6(),
        jsonschema::schema_version::draft7(),
        jsonschema::schema_version::draft201909(),
        jsonschema::schema_version::draft202012(),
    };
    if (std::find(supported.begin(), supported.end(), value) == supported.end()) {
        throw std::invalid_argument("unsupported JSON Schema dialect: " + value);
    }
}

class CompiledJsonSchema final : public ICompiledSchema {
public:
    CompiledJsonSchema(jsonschema::json_schema<JsonconsJson> schema,
                       SchemaValidationLimits limits, bool has_patterns)
        : schema_(std::move(schema)), limits_(limits), has_patterns_(has_patterns) {}

    std::optional<SchemaValidationError> validate(const Json& instance) const override {
        std::size_t total = 0;
        if (!inspect_instance(instance, 0, total, limits_, has_patterns_)) {
            return SchemaValidationError{
                {}, "instance exceeds the configured size, depth, or regex-input limit"};
        }
        const auto instance_text = instance.dump();
        if (instance_text.size() > limits_.max_instance_bytes) {
            return SchemaValidationError{{}, "instance exceeds the configured size limit"};
        }
        const auto jsoncons_instance = JsonconsJson::parse(instance_text);
        if (schema_.is_valid(jsoncons_instance)) {
            return std::nullopt;
        }
        jsoncons::json_decoder<jsoncons::ojson> decoder;
        schema_.validate(jsoncons_instance, decoder);
        const auto diagnostics = decoder.get_result();
        if (diagnostics.is_array() && !diagnostics.empty()) {
            const auto& diagnostic = diagnostics[0];
            return SchemaValidationError{
                diagnostic.at("instanceLocation").as<std::string>(),
                diagnostic.at("error").as<std::string>()};
        }
        return SchemaValidationError{{}, "instance does not conform to the schema"};
    }

private:
    jsonschema::json_schema<JsonconsJson> schema_;
    SchemaValidationLimits limits_;
    bool has_patterns_;
};

class JsonSchemaValidator final : public ISchemaValidator {
public:
    std::shared_ptr<const ICompiledSchema> compile(
        const Json& schema, const SchemaValidationLimits& limits) const override {
        if (!schema.is_object() && !schema.is_boolean()) {
            throw std::invalid_argument("JSON Schema must be an object or boolean");
        }
        inspect_schema_depth(schema, 0, limits.max_schema_depth);
        const auto schema_text = schema.dump();
        if (schema_text.size() > limits.max_schema_bytes) {
            throw std::invalid_argument("schema exceeds the configured size limit");
        }
        validate_dialect(schema);
        std::unordered_set<std::string> active_references;
        std::unordered_map<std::string, const Json*> anchors;
        collect_anchors(schema, anchors);
        inspect_references(schema, schema, 0, 0, limits, active_references, anchors);
        bool has_patterns = false;
        inspect_patterns(schema, limits, has_patterns);

        auto jsoncons_schema = JsonconsJson::parse(schema_text);
        auto options = jsonschema::evaluation_options{}.default_version(
            jsonschema::schema_version::draft202012());
        auto compiled = jsonschema::make_json_schema(std::move(jsoncons_schema), options);
        return std::make_shared<CompiledJsonSchema>(std::move(compiled), limits, has_patterns);
    }
};

}  // namespace

std::shared_ptr<ISchemaValidator> make_json_schema_validator() {
    return std::make_shared<JsonSchemaValidator>();
}

void validate_elicitation_form_schema(const nlohmann::json& schema) {
    if (!schema.is_object() || schema.value("type", std::string{}) != "object" ||
        !schema.contains("properties") || !schema["properties"].is_object()) {
        throw std::invalid_argument("Elicitation requires flat object properties");
    }
    for (auto property = schema["properties"].begin(); property != schema["properties"].end(); ++property) {
        auto name = property.key();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
        });
        const auto& definition = property.value();
        if (name.find("password") != std::string::npos || name.find("secret") != std::string::npos ||
            name.find("token") != std::string::npos || name.find("passphrase") != std::string::npos ||
            name.find("api_key") != std::string::npos || name.find("apikey") != std::string::npos ||
            !definition.is_object() || definition.value("writeOnly", false) ||
            definition.value("format", std::string{}) == "password") {
            throw std::invalid_argument("Sensitive input requires URL elicitation");
        }
        const auto type = definition.value("type", std::string{});
        if (type != "string" && type != "number" && type != "integer" && type != "boolean" &&
            type != "array") {
            throw std::invalid_argument("Elicitation fields must be primitive values");
        }
        if (definition.contains("$ref") || definition.contains("properties") ||
            (type == "array" && (!definition.contains("items") ||
             !definition["items"].is_object() ||
             definition["items"].value("type", std::string{}) != "string" ||
             (!definition["items"].contains("enum") && !definition["items"].contains("anyOf"))))) {
            throw std::invalid_argument("Nested elicitation fields are not supported");
        }
    }
}

}  // namespace cppmcp::core