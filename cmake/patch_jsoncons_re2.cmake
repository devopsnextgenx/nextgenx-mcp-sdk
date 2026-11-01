set(keyword_path "${JSONCONS_SOURCE_DIR}/include/jsoncons_ext/jsonschema/common/keyword_validator.hpp")
file(READ "${keyword_path}" keyword_content)
string(FIND "${keyword_content}" "std::regex regex_" keyword_original)
if(NOT keyword_original EQUAL -1)
    foreach(marker
            "std::regex regex_;"
            "const std::regex& regex"
            "std::regex_search(instance.as_cstring(), regex_)"
            "std::vector<std::pair<std::regex, schema_validator_ptr_type>> pattern_properties_;"
            "std::vector<std::pair<std::regex, schema_validator_ptr_type>>&& pattern_properties"
            "std::regex_search(prop.key(), schema_pp.first)")
        string(FIND "${keyword_content}" "${marker}" marker_index)
        if(marker_index EQUAL -1)
            message(FATAL_ERROR "Unexpected jsoncons keyword_validator.hpp; pinned RE2 patch no longer matches")
        endif()
    endforeach()
    string(REPLACE "#include <vector>"
        "#include <vector>\n#include <re2/re2.h>" keyword_content "${keyword_content}")
    string(REPLACE "std::regex regex_;"
        "std::shared_ptr<const re2::RE2> regex_;" keyword_content "${keyword_content}")
    string(REPLACE "const std::regex& regex"
        "std::shared_ptr<const re2::RE2> regex" keyword_content "${keyword_content}")
    string(REPLACE "std::regex_search(instance.as_cstring(), regex_)"
        "re2::RE2::PartialMatch(instance.as_cstring(), *regex_)" keyword_content "${keyword_content}")
    string(REPLACE "std::vector<std::pair<std::regex, schema_validator_ptr_type>>"
        "std::vector<std::pair<std::shared_ptr<const re2::RE2>, schema_validator_ptr_type>>"
        keyword_content "${keyword_content}")
    string(REPLACE "std::regex_search(prop.key(), schema_pp.first)"
        "re2::RE2::PartialMatch(re2::StringPiece(prop.key().data(), prop.key().size()), *schema_pp.first)"
        keyword_content "${keyword_content}")
    file(WRITE "${keyword_path}" "${keyword_content}")
else()
    string(FIND "${keyword_content}" "re2::RE2::PartialMatch" keyword_patched)
    if(keyword_patched EQUAL -1)
        message(FATAL_ERROR "jsoncons keyword_validator.hpp is neither original nor patched")
    endif()
endif()

set(factory_path "${JSONCONS_SOURCE_DIR}/include/jsoncons_ext/jsonschema/common/keyword_validator_factory.hpp")
file(READ "${factory_path}" factory_content)
string(FIND "${factory_content}" "auto regex = std::regex(pattern_string" factory_original)
if(NOT factory_original EQUAL -1)
    foreach(marker
            "std::vector<std::pair<std::regex, schema_validator_ptr_type>> pattern_properties;"
            "std::regex(prop.key(), std::regex::ECMAScript)"
            "auto regex = std::regex(pattern_string, std::regex::ECMAScript);")
        string(FIND "${factory_content}" "${marker}" marker_index)
        if(marker_index EQUAL -1)
            message(FATAL_ERROR "Unexpected jsoncons keyword_validator_factory.hpp; pinned RE2 patch no longer matches")
        endif()
    endforeach()
    string(REPLACE "#include <jsoncons/nonstd/compiler_support.hpp>"
        "#include <jsoncons/nonstd/compiler_support.hpp>\n#include <re2/re2.h>\n#include <stdexcept>"
        factory_content "${factory_content}")
    string(REPLACE "namespace jsonschema {"
        "namespace jsonschema {\n\ninline std::shared_ptr<const re2::RE2> compile_json_schema_pattern(const std::string& pattern)\n{\n    re2::RE2::Options options;\n    options.set_log_errors(false);\n    auto compiled = std::make_shared<const re2::RE2>(pattern, options);\n    if (!compiled->ok()) throw std::invalid_argument(\"Invalid JSON Schema regular expression: \" + compiled->error());\n    return compiled;\n}"
        factory_content "${factory_content}")
    string(REPLACE "std::vector<std::pair<std::regex, schema_validator_ptr_type>> pattern_properties;"
        "std::vector<std::pair<std::shared_ptr<const re2::RE2>, schema_validator_ptr_type>> pattern_properties;"
        factory_content "${factory_content}")
    string(REPLACE "std::regex(prop.key(), std::regex::ECMAScript)"
        "compile_json_schema_pattern(std::string(prop.key()))" factory_content "${factory_content}")
    string(REPLACE "auto regex = std::regex(pattern_string, std::regex::ECMAScript);"
        "auto regex = compile_json_schema_pattern(pattern_string);" factory_content "${factory_content}")
    file(WRITE "${factory_path}" "${factory_content}")
else()
    string(FIND "${factory_content}" "compile_json_schema_pattern" factory_patched)
    if(factory_patched EQUAL -1)
        message(FATAL_ERROR "jsoncons keyword_validator_factory.hpp is neither original nor patched")
    endif()
endif()
