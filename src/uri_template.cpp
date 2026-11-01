#include "cppmcp/core/uri_template.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace cppmcp::core {
namespace {

int hex_value(char ch);

bool valid_varname(std::string_view name) {
    if (name.empty()) return false;
    bool has_character = false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto ch = static_cast<unsigned char>(name[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') {
            has_character = true;
        } else if (ch == '%') {
            if (i + 2 >= name.size() || hex_value(name[i + 1]) < 0 ||
                hex_value(name[i + 2]) < 0) {
                return false;
            }
            i += 2;
            has_character = true;
        } else if (ch == '.' && has_character && i + 1 < name.size() &&
                   name[i + 1] != '.') {
            has_character = false;
        } else {
            return false;
        }
    }
    return has_character;
}

bool is_unreserved(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

bool is_reserved(unsigned char ch) {
    constexpr std::string_view reserved = ":/?#[]@!$&'()*+,;=";
    return reserved.find(static_cast<char>(ch)) != std::string_view::npos;
}

std::string encode(std::string_view value, bool allow_reserved) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto ch = static_cast<unsigned char>(value[i]);
        if (allow_reserved && ch == '%' && i + 2 < value.size() &&
            hex_value(value[i + 1]) >= 0 && hex_value(value[i + 2]) >= 0) {
            result.append(value.substr(i, 3));
            i += 2;
            continue;
        }
        if (is_unreserved(ch) || (allow_reserved && is_reserved(ch))) {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 0x0f]);
        }
    }
    return result;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<std::string> decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            result.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) return std::nullopt;
        const auto high = hex_value(value[i + 1]);
        const auto low = hex_value(value[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return result;
}

std::string json_scalar(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number()) return value.dump();
    return {};
}

std::size_t utf8_prefix_bytes(std::string_view text, std::size_t characters) {
    std::size_t offset = 0;
    while (offset < text.size() && characters > 0) {
        const auto ch = static_cast<unsigned char>(text[offset]);
        offset += ch < 0x80 ? 1 : (ch < 0xe0 ? 2 : (ch < 0xf0 ? 3 : 4));
        --characters;
    }
    return std::min(offset, text.size());
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    for (;;) {
        const auto end = value.find(delimiter, start);
        result.push_back(value.substr(start, end == std::string_view::npos
                                                ? std::string_view::npos
                                                : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

struct Operator {
    std::string_view prefix;
    std::string_view separator;
    bool named = false;
    bool allow_reserved = false;
};

Operator operator_info(char op) {
    switch (op) {
        case '+': return {"", ",", false, true};
        case '#': return {"#", ",", false, true};
        case '.': return {".", ".", false, false};
        case '/': return {"/", "/", false, false};
        case ';': return {";", ";", true, false};
        case '?': return {"?", "&", true, false};
        case '&': return {"&", "&", true, false};
        default: return {"", ",", false, false};
    }
}

template <typename Spec>
std::string named_value(std::string_view name, const nlohmann::json& value,
                        const Spec& spec, bool explode, const Operator& op) {
    if (value.is_array()) {
        std::string result;
        bool first = true;
        for (const auto& item : value) {
            if (!first) result += explode ? op.separator : ",";
            if (explode) {
                if (op.named) result += encode(name, false) + (item.is_null() ? "" : "=");
            }
            result += encode(json_scalar(item), op.allow_reserved);
            first = false;
        }
        if (op.named && !explode && !value.empty()) {
            return encode(name, true) + "=" + result;
        }
        return result;
    }
    if (value.is_object()) {
        std::string result;
        bool first = true;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!first) result += explode ? op.separator : ",";
            if (explode) {
                result += encode(it.key(), false) + "=" +
                          encode(json_scalar(it.value()), op.allow_reserved);
            } else {
                result += encode(it.key(), op.allow_reserved) + ',' +
                          encode(json_scalar(it.value()), op.allow_reserved);
            }
            first = false;
        }
        if (op.named && !explode && !value.empty()) {
            return encode(name, true) + "=" + result;
        }
        return result;
    }
    auto text = json_scalar(value);
    if (spec.prefix) text.resize(utf8_prefix_bytes(text, *spec.prefix));
    const auto encoded = encode(text, op.allow_reserved);
    if (!op.named) return encoded;
    return encode(name, true) + (text.empty() && op.separator == ";" ? "" : "=") + encoded;
}

}  // namespace

UriTemplate UriTemplate::parse(std::string value) {
    std::vector<Part> parts;
    std::size_t cursor = 0;
    bool previous_expression = false;
    while (cursor < value.size()) {
        const auto open = value.find('{', cursor);
        const auto close_before_open = value.find('}', cursor);
        if (close_before_open != std::string::npos &&
            (open == std::string::npos || close_before_open < open)) {
            throw std::invalid_argument("unmatched '}' in URI template");
        }
        if (open == std::string::npos) {
            parts.push_back({value.substr(cursor), std::nullopt});
            cursor = value.size();
            break;
        }
        const auto literal = value.substr(cursor, open - cursor);
        if (!literal.empty()) parts.push_back({literal, std::nullopt});
        const auto close = value.find('}', open + 1);
        if (close == std::string::npos || value.find('{', open + 1) < close) {
            throw std::invalid_argument("malformed URI-template expression");
        }
        auto body = std::string_view(value).substr(open + 1, close - open - 1);
        if (body.empty()) throw std::invalid_argument("empty URI-template expression");
        Expression expression;
        if (std::string_view("+#./;?&").find(body.front()) != std::string_view::npos) {
            expression.op = body.front();
            body.remove_prefix(1);
        }
        if (body.empty()) throw std::invalid_argument("URI-template expression has no variables");
        for (auto token : split(body, ',')) {
            if (token.empty()) throw std::invalid_argument("empty variable in URI-template expression");
            VariableSpec spec;
            const auto modifier = token.find_first_of("*:");
            spec.name = std::string(token.substr(0, modifier));
            if (!valid_varname(spec.name)) throw std::invalid_argument("invalid URI-template variable name");
            if (modifier != std::string_view::npos) {
                const auto suffix = token.substr(modifier);
                if (suffix == "*") {
                    spec.explode = true;
                } else if (suffix.size() > 1 && suffix.front() == ':') {
                    std::size_t prefix = 0;
                    const auto parsed = std::from_chars(suffix.data() + 1,
                                                        suffix.data() + suffix.size(), prefix);
                    if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
                        prefix == 0 || prefix > 9999 || suffix[1] == '0') {
                        throw std::invalid_argument(
                            "URI-template prefix modifier must be between 1 and 9999");
                    }
                    spec.prefix = prefix;
                } else {
                    throw std::invalid_argument("invalid URI-template variable modifier");
                }
            }
            expression.variables.push_back(std::move(spec));
        }
        if (previous_expression && literal.empty() && !parts.empty() &&
            parts.back().expression && parts.back().expression->op == '\0' &&
            expression.op == '\0') {
            throw std::invalid_argument(
                "adjacent simple URI-template expressions are ambiguous without a literal separator");
        }
        parts.push_back({{}, std::move(expression)});
        cursor = close + 1;
        previous_expression = true;
    }
    if (value.empty()) parts.push_back({{}, std::nullopt});
    return UriTemplate(std::move(value), std::move(parts));
}

std::string UriTemplate::expand(const nlohmann::json& variables) const {
    if (!variables.is_object()) throw std::invalid_argument("URI-template variables must be an object");
    std::string output;
    for (const auto& part : parts_) {
        if (!part.expression) {
            output += encode(part.literal, true);
            continue;
        }
        const auto& expression = *part.expression;
        const auto op = operator_info(expression.op);
        std::vector<std::string> expanded;
        for (const auto& spec : expression.variables) {
            const auto found = variables.find(spec.name);
            if (found == variables.end() || found->is_null()) continue;
            if (spec.prefix && (found->is_array() || found->is_object())) {
                throw std::invalid_argument(
                    "URI-template prefix modifiers apply only to scalar variables");
            }
            auto text = named_value(spec.name, *found, spec, spec.explode, op);
            if (text.empty() && op.named &&
                ((found->is_string() && found->get_ref<const std::string&>().empty()) ||
                 (found->is_array() && !found->empty()))) {
                if (expression.op == ';' && found->is_string()) {
                    text = encode(spec.name, false);
                } else {
                    text = encode(spec.name, false) + "=";
                }
            }
            if (!text.empty()) expanded.push_back(std::move(text));
        }
        if (!expanded.empty()) {
            output += op.prefix;
            for (std::size_t i = 0; i < expanded.size(); ++i) {
                if (i != 0) output += op.separator;
                output += expanded[i];
            }
        }
    }
    return output;
}

std::optional<UriTemplate::Variables> UriTemplate::match(std::string_view uri) const {
    if (source_.size() > 4096 || uri.size() > 16384) return std::nullopt;
    Variables result;
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < parts_.size(); ++index) {
        const auto& part = parts_[index];
        if (!part.expression) {
            if (!uri.substr(cursor).starts_with(part.literal)) return std::nullopt;
            cursor += part.literal.size();
            continue;
        }
        const auto& expression = *part.expression;
        const auto op = operator_info(expression.op);
        std::size_t end = uri.size();
        for (std::size_t next = index + 1; next < parts_.size(); ++next) {
            if (!parts_[next].expression && !parts_[next].literal.empty()) {
                end = uri.find(parts_[next].literal, cursor);
                if (end == std::string_view::npos) return std::nullopt;
                break;
            }
        }
        auto body = uri.substr(cursor, end - cursor);
        const bool has_prefix = !op.prefix.empty() && body.starts_with(op.prefix);
        if (!op.prefix.empty() && !has_prefix) {
            if (expression.op == '?' || expression.op == '&') {
                cursor = end;
                continue;
            }
            return std::nullopt;
        }
        if (has_prefix) body.remove_prefix(op.prefix.size());
        cursor = end;
        if (body.empty()) {
            if ((expression.op == '\0' || expression.op == '+') &&
                expression.variables.size() == 1) {
                result[expression.variables.front().name] = "";
            }
            continue;
        }

        if (op.named) {
            const char sep = expression.op == ';' ? ';' : '&';
            for (const auto item : split(body, sep)) {
                if (item.empty()) continue;
                const auto equals = item.find('=');
                const auto name = decode(item.substr(0, equals));
                const auto value = equals == std::string_view::npos
                                       ? std::optional<std::string>(std::string{})
                                       : decode(item.substr(equals + 1));
                if (!name || !value) return std::nullopt;
                const auto spec = std::find_if(expression.variables.begin(), expression.variables.end(),
                                               [&](const auto& candidate) {
                                                   return candidate.name == *name;
                                               });
                if (spec == expression.variables.end()) return std::nullopt;
                result[*name] = *value;
            }
        } else {
            if (expression.op == '\0' && body.find('/') != std::string_view::npos) {
                return std::nullopt;
            }
            const char separator = expression.op == '/' || expression.op == '.' ? expression.op : ',';
            const auto values = expression.variables.size() == 1 &&
                                        !expression.variables.front().explode
                                    ? std::vector<std::string_view>{body}
                                    : split(body, separator);
            if (expression.variables.size() == 1) {
                const auto& spec = expression.variables.front();
                if (spec.explode) {
                    nlohmann::json items = nlohmann::json::array();
                    for (auto item : values) {
                        auto decoded = decode(item);
                        if (!decoded) return std::nullopt;
                        items.push_back(std::move(*decoded));
                    }
                    result[spec.name] = std::move(items);
                } else {
                    if (values.size() != 1) return std::nullopt;
                    auto decoded = decode(values.front());
                    if (!decoded) return std::nullopt;
                    result[spec.name] = std::move(*decoded);
                }
            } else {
                if (values.size() > expression.variables.size()) return std::nullopt;
                for (std::size_t i = 0; i < values.size(); ++i) {
                    auto decoded = decode(values[i]);
                    if (!decoded) return std::nullopt;
                    result[expression.variables[i].name] = std::move(*decoded);
                }
            }
        }
    }
    if (cursor != uri.size()) return std::nullopt;
    return result;
}

}  // namespace cppmcp::core
