// Helpers for the tests of the target descriptor: a scratch directory holding a descriptor and the
// CSV it names, sources built through it, and the usual checks. Header-only, for white-box tests.
#ifndef PRAENORA_TESTS_SUPPORT_DESCRIPTOR_FIXTURES_HPP
#define PRAENORA_TESTS_SUPPORT_DESCRIPTOR_FIXTURES_HPP

#include "catalog/catalog.hpp"
#include "config/workflow_config.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "io/input_source.hpp"
#include "io/target_descriptor.hpp"

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace descriptor_fixtures {

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Severity;
using praenora::detail::Catalog;
using praenora::detail::InputBinding;
using praenora::detail::InputSource;
using praenora::detail::load_target_descriptor;
using praenora::detail::make_input_source;
using praenora::detail::MessageTypeDef;
using praenora::detail::TargetDescriptor;
using praenora::detail::TimeBase;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

inline const Catalog& catalog() {
    static const Catalog instance =
        Catalog::load({praenora::test::utf8_path(PRAENORA_TEST_CATALOG_DIR)});
    return instance;
}

inline const MessageTypeDef& observation_batch() {
    const MessageTypeDef* type =
        catalog().find_message_type("pmp.observation-batch", praenora::detail::parse_semver("0.1.0"));
    if (type == nullptr) {
        throw std::runtime_error("no message type pmp.observation-batch");
    }
    return *type;
}

inline TimeBase days(double start = 0.0) {
    TimeBase time;
    time.unit = "d";
    time.start = start;
    return time;
}

// A descriptor for obs.csv (next to it), one variable x in mmol, time unit d.
inline json base_descriptor() {
    return json{{"schema_version", "0.0.0"},
                {"id", "d1"},
                {"name", "Test data"},
                {"data", {{"id", "data1"}, {"href", "obs.csv"}}},
                {"columns",
                 {{"series_id", "series_id"},
                  {"variable_id", "variable_id"},
                  {"time", "time"},
                  {"value", "value"}}},
                {"time_unit", "d"},
                {"variables",
                 json::array({{{"id", "x"}, {"name", "X"}, {"unit", "mmol"}, {"value_type", "scalar"}}})}};
}

inline json variable(const std::string& id, const std::string& role = "", const std::string& unit = "mmol") {
    json entry = {{"id", id}, {"name", id}, {"unit", unit}, {"value_type", "scalar"}};
    if (!role.empty()) {
        entry["role"] = role;
    }
    return entry;
}

inline const char* const kSimpleCsv = "series_id,variable_id,time,value\n"
                         "S1,x,0.0,1\n"
                         "S1,x,1.0,2\n";

// A scratch directory with the descriptor (descriptor.json) and the data (obs.csv) in it.
class Case {
public:
    explicit Case(std::string csv = kSimpleCsv, json descriptor = base_descriptor())
        : descriptor_(std::move(descriptor)) {
        write_text_file(dir_.path() / "obs.csv", csv);
        write_descriptor();
    }

    void write_descriptor() { write_text_file(file(), descriptor_.dump(2)); }
    json& descriptor() { return descriptor_; }
    const fs::path& dir() const { return dir_.path(); }
    fs::path file() const { return dir_.path() / "descriptor.json"; }

    InputBinding binding() const {
        InputBinding binding;
        binding.input = "observations";
        binding.source = "observation-csv";
        binding.descriptor = file();
        return binding;
    }

    std::unique_ptr<InputSource> source(const TimeBase& time = days()) {
        write_descriptor();
        return make_input_source(binding(), observation_batch(), time);
    }

    Error error(const TimeBase& time = days()) {
        try {
            (void)source(time);
        } catch (const Error& e) {
            return e;
        }
        ADD_FAILURE() << "no error for the descriptor:\n" << descriptor_.dump(2);
        return Error(ErrorCode::OK, "no error");
    }

    // The error of loading the descriptor alone.
    Error load_error() {
        write_descriptor();
        try {
            (void)load_target_descriptor(file());
        } catch (const Error& e) {
            return e;
        }
        ADD_FAILURE() << "no error for the descriptor:\n" << descriptor_.dump(2);
        return Error(ErrorCode::OK, "no error");
    }

    TargetDescriptor load() {
        write_descriptor();
        return load_target_descriptor(file());
    }

private:
    ScratchDir dir_;
    json descriptor_;
};

inline std::vector<std::string> warnings(const InputSource& source) {
    std::vector<std::string> messages;
    for (const auto& note : source.notes()) {
        EXPECT_EQ(note.severity, Severity::warning);
        EXPECT_EQ(note.code, ErrorCode::TARGET_DATA_INVALID);
        messages.push_back(note.message);
    }
    return messages;
}

inline bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The only warning of `source` that contains `part`, or ADD_FAILURE.
inline const praenora::Diagnostic* warning_with(const InputSource& source, const std::string& part) {
    const praenora::Diagnostic* found = nullptr;
    for (const auto& note : source.notes()) {
        if (contains(note.message, part)) {
            EXPECT_EQ(found, nullptr) << "two warnings with '" << part << "'";
            found = &note;
        }
    }
    EXPECT_NE(found, nullptr) << "no warning with '" << part << "'";
    return found;
}

inline std::vector<std::string> variable_ids(const json& batch) {
    std::vector<std::string> ids;
    for (const auto& row : batch.at("observations")) {
        ids.push_back(row.at("variable_id").get<std::string>());
    }
    return ids;
}


} // namespace descriptor_fixtures

#endif // PRAENORA_TESTS_SUPPORT_DESCRIPTOR_FIXTURES_HPP
