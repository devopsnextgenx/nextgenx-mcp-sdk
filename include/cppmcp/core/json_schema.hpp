#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace cppmcp::core {

struct SchemaValidationLimits {
    std::size_t max_schema_bytes = 1024 * 1024;
    std::size_t max_schema_depth = 64;
    std::size_t max_reference_depth = 64;
    std::size_t max_instance_bytes = 4 * 1024 * 1024;
    std::size_t max_instance_depth = 64;
    std::size_t max_pattern_bytes = 512;
    std::size_t max_pattern_input_bytes = 64 * 1024;
};

struct SchemaValidationError {
    std::string instance_path;
    std::string message;
};

class ICompiledSchema {
public:
    virtual ~ICompiledSchema() = default;
    virtual std::optional<SchemaValidationError> validate(
        const nlohmann::json& instance) const = 0;
};

class ISchemaValidator {
public:
    virtual ~ISchemaValidator() = default;
    virtual std::shared_ptr<const ICompiledSchema> compile(
        const nlohmann::json& schema,
        const SchemaValidationLimits& limits = {}) const = 0;
};

std::shared_ptr<ISchemaValidator> make_json_schema_validator();

}  // namespace cppmcp::core