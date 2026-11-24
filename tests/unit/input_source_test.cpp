// Input sources: observation-csv replay (tick windows, ordering, the observation row rules,
// column remapping, exhaustion, digest) and json-document (a static payload of the port's type).
#include "io/input_source.hpp"

#include "catalog/catalog.hpp"
#include "config/workflow_config.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/yaml_loader.hpp"

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::Catalog;
using praenora::detail::InputBinding;
using praenora::detail::InputSource;
using praenora::detail::make_input_source;
using praenora::detail::MessageTypeDef;
using praenora::detail::parse_semver;
using praenora::detail::TimeBase;
using praenora::test::RestorePermissions;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

namespace {

// The built-in catalog plus the test catalog (test.blob: any JSON object).
const Catalog& catalog() {
    static const Catalog instance =
        Catalog::load({praenora::test::utf8_path(PRAENORA_TEST_CATALOG_DIR)});
    return instance;
}

const MessageTypeDef& message_type(const std::string& id) {
    const MessageTypeDef* type = catalog().find_message_type(id, parse_semver("0.1.0"));
    if (type == nullptr) {
        throw std::runtime_error("no message type " + id);
    }
    return *type;
}

const MessageTypeDef& observation_batch() { return message_type("pmp.observation-batch"); }

TimeBase days() {
    TimeBase time;
    time.unit = "d";
    return time;
}

InputBinding csv_binding(const fs::path& path, std::map<std::string, std::string> columns = {}) {
    InputBinding binding;
    binding.input = "observations";
    binding.source = "observation-csv";
    binding.path = path;
    binding.columns = std::move(columns);
    return binding;
}

// An observation-csv source over `csv`, written to a file in `dir`.
std::unique_ptr<InputSource> csv_source(const ScratchDir& dir, const std::string& csv,
                                        std::map<std::string, std::string> columns = {}) {
    const fs::path file = dir.path() / "observations.csv";
    write_text_file(file, csv);
    return make_input_source(csv_binding(file, std::move(columns)), observation_batch(), days());
}

// The error of building a source over `csv` with `columns`.
Error csv_source_error(const std::string& csv, std::map<std::string, std::string> columns = {}) {
    ScratchDir dir;
    try {
        (void)csv_source(dir, csv, std::move(columns));
    } catch (const Error& e) {
        return e;
    }
    ADD_FAILURE() << "no error for:\n" << csv;
    return Error(ErrorCode::OK, "no error");
}

std::vector<double> times(const json& batch) {
    std::vector<double> result;
    for (const auto& row : batch.at("observations")) {
        result.push_back(row.at("time").get<double>());
    }
    return result;
}

std::vector<double> values(const json& batch) {
    std::vector<double> result;
    for (const auto& row : batch.at("observations")) {
        result.push_back(row.at("value").get<double>());
    }
    return result;
}

const json& row_with_id(const json& batch, const std::string& id) {
    for (const auto& row : batch.at("observations")) {
        if (row.value("observation_id", "") == id) {
            return row;
        }
    }
    throw std::runtime_error("no row " + id);
}

std::unique_ptr<InputSource> fixture_source() {
    return make_input_source(csv_binding(praenora::test::fixture_path("data/observations.csv")),
                             observation_batch(), days());
}

} // namespace

TEST(ObservationCsv, WindowsAreHalfOpen) {
    ScratchDir dir;
    const auto source = csv_source(dir, "series_id,variable_id,time,value\n"
                                        "S1,x,0.999,1\n"
                                        "S1,x,1.0,2\n"
                                        "S1,x,2.0,3\n");
    EXPECT_TRUE(source->replay());
    EXPECT_EQ(times(source->batch(0, 0.0, 1.0)), (std::vector<double>{0.999}));
    EXPECT_EQ(times(source->batch(1, 1.0, 2.0)), (std::vector<double>{1.0})); // t = 1.0: tick 1
    EXPECT_EQ(times(source->batch(2, 2.0, 3.0)), (std::vector<double>{2.0}));
    // A batch holds exactly the rows inside the window it is asked for.
    EXPECT_EQ(times(source->batch(0, 1.5, 2.5)), (std::vector<double>{2.0}));
}

TEST(ObservationCsv, EmptyWindowStillProducesBatch) {
    const auto source = fixture_source();
    const json batch = source->batch(7, 7.0, 8.0);
    EXPECT_EQ(batch, json::parse(R"({"window":{"start":7.0,"end":8.0},"time_unit":"d",
                                     "observations":[]})"));
    EXPECT_TRUE(observation_batch().compiled->validate(batch).empty());
}

TEST(ObservationCsv, SortedDeterministically) {
    ScratchDir dir;
    // Ties on time are broken by series_id, variable_id, observation_id (absent sorts first) and
    // finally the row's position in the file.
    const auto source = csv_source(dir, "series_id,variable_id,time,value,observation_id\n"
                                        "B,v,1.0,1,\n"
                                        "A,w,1.0,2,\n"
                                        "A,v,1.0,3,z\n"
                                        "A,v,1.0,4,\n"
                                        "A,v,1.0,5,\n"
                                        "A,v,0.5,6,\n");
    EXPECT_EQ(values(source->batch(0, 0.0, 2.0)), (std::vector<double>{6, 4, 5, 3, 2, 1}));

    // The fixture's rows are out of order in the file.
    const auto fixture = fixture_source();
    const json all = fixture->batch(0, 0.0, 3.0);
    std::vector<std::string> ids;
    for (const auto& row : all.at("observations")) {
        ids.push_back(row.at("observation_id").get<std::string>());
    }
    EXPECT_EQ(ids,
              (std::vector<std::string>{"o001", "o002", "o003", "o004", "o007", "o006", "o005"}));
    // Every window of the replay is a valid observation batch.
    for (int tick = 0; tick < 4; ++tick) {
        const json batch = fixture->batch(static_cast<std::uint64_t>(tick), tick, tick + 1.0);
        EXPECT_TRUE(observation_batch().compiled->validate(batch).empty()) << batch.dump();
    }
}

TEST(ObservationCsv, MissingAndCensoredRowsKeepNullValues) {
    const auto source = fixture_source();
    const json batch = source->batch(0, 0.0, 3.0);
    EXPECT_EQ(row_with_id(batch, "o004"),
              json::parse(R"({"observation_id":"o004","series_id":"S01","variable_id":"population",
                              "time":1.0,"value":null,"unit":"count","missing":true,
                              "censoring":"none","weight":1.0,"group_id":"control",
                              "batch_id":"B1"})"));
    EXPECT_EQ(row_with_id(batch, "o005"),
              json::parse(R"({"observation_id":"o005","series_id":"S02","variable_id":"population",
                              "time":2.75,"value":null,"unit":"count","missing":false,
                              "censoring":"left","upper_limit":2.0,"weight":1.0,
                              "group_id":"group, B","batch_id":"B2"})"));
    // "FALSE" reads as false; an empty weight is the default 1.
    EXPECT_EQ(row_with_id(batch, "o003").at("missing"), json(false));
    EXPECT_EQ(row_with_id(batch, "o003").at("weight"), json(0.5));
    EXPECT_EQ(row_with_id(batch, "o006").at("weight"), json(1.0));
    EXPECT_EQ(row_with_id(batch, "o006").at("batch_id"), json("batch\ntwo"));

    // missing=true makes the value null even when a number is written.
    ScratchDir dir;
    const auto written = csv_source(dir, "series_id,variable_id,time,value,missing\n"
                                         "S1,x,0.5,4.5,true\n");
    EXPECT_EQ(written->batch(0, 0.0, 1.0).at("observations").at(0).at("value"), json(nullptr));
}

TEST(ObservationCsv, RowRulesAreTargetDataInvalidWithLine) {
    const std::string header =
        "series_id,variable_id,time,value,missing,censoring,lower_limit,upper_limit,weight\n";
    struct Case {
        const char* row;
        const char* mentions;
    };
    for (const Case& c : {
             Case{",x,1,2,,,,,", "series_id"},
             Case{"S,,1,2,,,,,", "variable_id"},
             Case{"S,x,,2,,,,,", "time"},
             Case{"S,x,1,two,,,,,", "two"},
             Case{"S,x,1,2,yes,,,,", "missing"},
             Case{"S,x,1,2,,upper,,,", "censoring"},
             Case{"S,x,1,2,,,,,-1", "weight"},
             Case{"S,x,1,,,,,,", "value"},          // empty value, neither missing nor censored
             Case{"S,x,1,,,left,,,", "value"},      // left-censored needs upper_limit
             Case{"S,x,1,,,right,,3,", "value"},    // right-censored needs lower_limit
             Case{"S,x,1,,,interval,1,,", "value"}, // interval needs both limits
             Case{"S,x,1,2,,,low,,", "lower_limit"},
             Case{"S,x,1e999,2,,,,,", "time"}, // beyond double
             Case{"S,x,nan,2,,,,,", "time"},
         }) {
        SCOPED_TRACE(c.row);
        const Error error = csv_source_error(header + "S,x,0,1,,,,,\n" + c.row + "\n");
        EXPECT_EQ(error.code(), ErrorCode::TARGET_DATA_INVALID) << error.what();
        EXPECT_EQ(error.location().line, 3) << error.what();
        EXPECT_NE(error.location().file.find("observations.csv"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find(c.mentions), std::string::npos) << error.what();
    }
    // Censored rows with their limits may leave the value empty.
    ScratchDir dir;
    const auto source = csv_source(dir, header + "S,x,1,,,right,3,,\n"
                                                 "S,x,1,,,interval,1,2,\n");
    EXPECT_EQ(source->batch(0, 0.0, 2.0).at("observations").size(), 2u);
}

TEST(ObservationCsv, NonNumericTimeIsTargetDataInvalid) {
    const Error error = csv_source_error("series_id,variable_id,time,value\n"
                                         "S1,x,0.5,1\n"
                                         "S1,x,soon,2\n");
    EXPECT_EQ(error.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(error.location().line, 3);
    EXPECT_NE(std::string(error.what()).find("time"), std::string::npos) << error.what();
    EXPECT_NE(std::string(error.what()).find("soon"), std::string::npos) << error.what();
    // Numbers are written the C way, whatever the reader's locale: "0,5" is not a number.
    EXPECT_EQ(
        csv_source_error("series_id,variable_id,time,value\nS1,x,\"0,5\",1\n").location().line, 2);
}

TEST(ObservationCsv, MissingRequiredColumnIsTargetDataInvalid) {
    const Error error = csv_source_error("series_id,variable_id,time\nS1,x,0.5\n");
    EXPECT_EQ(error.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(error.location().line, 1);
    EXPECT_NE(std::string(error.what()).find("'value'"), std::string::npos) << error.what();
    // A remapped column must exist under its new name.
    const Error remapped =
        csv_source_error("series_id,variable_id,time,value\nS1,x,0.5,1\n", {{"value", "y"}});
    EXPECT_EQ(remapped.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_NE(std::string(remapped.what()).find("'y'"), std::string::npos) << remapped.what();
}

TEST(ObservationCsv, ColumnRemapping) {
    ScratchDir dir;
    // `value` is read from the header y; the CSV's own "value" column is then not read.
    const auto source = csv_source(dir, "sid,var,t,y,value,obs\nS1,x,0.5,1.25,ignored,a\n",
                                   {{"series_id", "sid"},
                                    {"variable_id", "var"},
                                    {"time", "t"},
                                    {"value", "y"},
                                    {"observation_id", "obs"}});
    EXPECT_EQ(source->batch(0, 0.0, 1.0).at("observations"),
              json::parse(R"([{"series_id":"S1","variable_id":"x","time":0.5,"value":1.25,
                               "observation_id":"a","missing":false,"censoring":"none",
                               "weight":1.0}])"));
    // Two observation columns cannot read one header.
    const Error clash =
        csv_source_error("series_id,variable_id,time,value\nS1,x,0.5,1\n", {{"time", "value"}});
    EXPECT_EQ(clash.code(), ErrorCode::INPUT_BINDING_INVALID) << clash.what();
}

TEST(ObservationCsv, ExhaustionAfterLastRow) {
    ScratchDir dir;
    const auto source = csv_source(dir, "series_id,variable_id,time,value\n"
                                        "S1,x,0.5,1\n"
                                        "S1,x,2.0,2\n");
    EXPECT_FALSE(source->exhausted_after(1.0));
    EXPECT_FALSE(source->exhausted_after(2.0)); // the row at 2.0 belongs to the next window
    EXPECT_TRUE(source->exhausted_after(2.5));
    ScratchDir empty_dir;
    const auto empty = csv_source(empty_dir, "series_id,variable_id,time,value\n");
    EXPECT_TRUE(empty->exhausted_after(0.0));
    EXPECT_EQ(empty->batch(0, 0.0, 1.0).at("observations"), json::array());
}

TEST(ObservationCsv, DigestIsFileSha256) {
    const fs::path file = praenora::test::fixture_path("data/observations.csv");
    EXPECT_EQ(fixture_source()->digest(), praenora::detail::sha256_file_hex(file));
    const fs::path windows = praenora::test::fixture_path("data/bom-crlf/observations.csv");
    const auto windows_source =
        make_input_source(csv_binding(windows), observation_batch(), days());
    EXPECT_EQ(windows_source->digest(), praenora::detail::sha256_file_hex(windows));
    EXPECT_NE(windows_source->digest(), fixture_source()->digest());
    // Same rows, same batches.
    EXPECT_EQ(windows_source->batch(1, 1.0, 2.0), fixture_source()->batch(1, 1.0, 2.0));
}

TEST(ObservationCsv, OversizedFileIsRefusedBeforeItIsRead) {
    // The limit is 256 MiB, the limit of a module's output; the test lowers it through the
    // source's internal parameter.
    EXPECT_EQ(praenora::detail::kMaxObservationCsvBytes, std::uintmax_t{268435456});
    ScratchDir dir;
    const std::string csv = "series_id,variable_id,time,value\nS1,x,0.5,1\n";
    const fs::path file = dir.path() / "observations.csv";
    write_text_file(file, csv);
    EXPECT_NO_THROW((void)praenora::detail::make_observation_csv_source(
        csv_binding(file), observation_batch(), days(), csv.size())); // at the limit
    // Larger than the limit: refused by its size, whatever it holds (here a quoted field that
    // never closes, which reading it would report). The file is also made unreadable, so a read
    // before the size check would report "cannot read" instead of "too large".
    const fs::path big = dir.path() / "big.csv";
    write_text_file(big, csv + "\"never closed\n");
    const RestorePermissions restore{big, fs::perms::owner_read | fs::perms::owner_write};
    fs::permissions(big, fs::perms::none);
    if (std::ifstream(big, std::ios::binary).is_open()) {
        GTEST_SKIP() << "the process can read a file without permissions";
    }
    try {
        (void)praenora::detail::make_observation_csv_source(csv_binding(big), observation_batch(),
                                                            days(), csv.size());
        ADD_FAILURE() << "the file was read";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID) << e.what();
        EXPECT_EQ(e.location().file, praenora::detail::path_to_utf8(big));
        EXPECT_EQ(e.location().line, 0) << e.what();
        const std::string message = e.what();
        EXPECT_NE(message.find("too large"), std::string::npos) << message;
        EXPECT_NE(message.find(std::to_string(fs::file_size(big)) + " bytes"), std::string::npos)
            << message;
        EXPECT_NE(message.find("the limit is " + std::to_string(csv.size()) + " bytes"),
                  std::string::npos)
            << message;
        EXPECT_NE(message.find("input 'observations'"), std::string::npos) << message;
    }
}

TEST(InputSource, FileThatCannotBeReadIsTargetDataUnavailable) {
    // A file that does not exist, and a directory, cannot be read: TARGET_DATA_UNAVAILABLE located
    // at the file, for either kind of source. Validation reports the first two at the
    // configuration's path before it builds a source; the source's own check remains.
    ScratchDir dir;
    const fs::path directory = dir.path() / "dir";
    fs::create_directory(directory);
    for (const char* source : {"observation-csv", "json-document"}) {
        for (const fs::path& path : {dir.path() / "none.csv", directory}) {
            SCOPED_TRACE(std::string(source) + ": " + praenora::detail::path_to_utf8(path));
            InputBinding binding = csv_binding(path);
            binding.source = source;
            try {
                (void)make_input_source(binding, message_type("test.blob"), days());
                ADD_FAILURE() << "the source was built";
            } catch (const Error& e) {
                EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_UNAVAILABLE) << e.what();
                EXPECT_EQ(e.location().file, praenora::detail::path_to_utf8(path));
                EXPECT_NE(std::string(e.what()).find("input 'observations': cannot read the"),
                          std::string::npos)
                    << e.what();
            }
        }
    }
}

TEST(ObservationCsv, PortTypeMustAcceptObservationBatches) {
    // test.blob accepts any object, so it can carry observation batches too.
    const fs::path file = praenora::test::fixture_path("data/observations.csv");
    EXPECT_NO_THROW((void)make_input_source(csv_binding(file), message_type("test.blob"), days()));
    try {
        (void)make_input_source(csv_binding(file), message_type("pmp.prediction-set"), days());
        ADD_FAILURE() << "an observation-csv source was bound to a prediction set";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::INPUT_BINDING_INVALID) << e.what();
        EXPECT_NE(std::string(e.what()).find("pmp.prediction-set"), std::string::npos) << e.what();
    }
}

TEST(JsonDocument, DeliversSamePayloadEachTick) {
    ScratchDir dir;
    const fs::path file = dir.path() / "model-set.json";
    write_text_file(file, "{\"a\": [1, 2.5], \"b\": \"x\"}\n");
    InputBinding binding;
    binding.input = "source";
    binding.source = "json-document";
    binding.path = file;
    const auto source = make_input_source(binding, message_type("test.blob"), days());
    const json expected = json::parse(R"({"a":[1,2.5],"b":"x"})");
    EXPECT_EQ(source->batch(0, 0.0, 1.0), expected);
    EXPECT_EQ(source->batch(9, 9.0, 10.0), expected);
    EXPECT_FALSE(source->replay());
    EXPECT_TRUE(source->exhausted_after(0.0)); // nothing is ever pending
    EXPECT_EQ(source->digest(), praenora::detail::sha256_file_hex(file));

    // YAML documents work too.
    const fs::path yaml = dir.path() / "model-set.yaml";
    write_text_file(yaml, "a: [1, 2.5]\nb: x\n");
    binding.path = yaml;
    EXPECT_EQ(make_input_source(binding, message_type("test.blob"), days())->batch(0, 0, 1),
              expected);
}

TEST(JsonDocument, OversizedDocumentIsRefused) {
    ScratchDir dir;
    const fs::path file = dir.path() / "huge.json";
    write_text_file(file, "");
    fs::resize_file(file, praenora::detail::kMaxDocumentBytes + 1);
    InputBinding binding;
    binding.input = "source";
    binding.source = "json-document";
    binding.path = file;
    try {
        (void)make_input_source(binding, message_type("test.blob"), days());
        ADD_FAILURE() << "the document was accepted";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::PARSE_ERROR) << e.what();
        EXPECT_NE(std::string(e.what()).find("document too large"), std::string::npos) << e.what();
        EXPECT_EQ(e.location().file, praenora::detail::path_to_utf8(file));
    }
}

TEST(JsonDocument, TypeMismatchIsInputBindingInvalid) {
    ScratchDir dir;
    const fs::path file = dir.path() / "batch.json";
    write_text_file(file, "{\n  \"window\": {\"start\": 0},\n  \"time_unit\": \"d\",\n"
                          "  \"observations\": []\n}\n");
    InputBinding binding;
    binding.input = "observations";
    binding.source = "json-document";
    binding.path = file;
    try {
        (void)make_input_source(binding, observation_batch(), days());
        ADD_FAILURE() << "the document was accepted";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::INPUT_BINDING_INVALID) << e.what();
        EXPECT_EQ(e.location().file, praenora::detail::path_to_utf8(file));
        EXPECT_EQ(e.location().pointer, "/window");
        EXPECT_EQ(e.location().line, 2);
        EXPECT_NE(std::string(e.what()).find("pmp.observation-batch@0.1.0"), std::string::npos)
            << e.what();
    }
    // A document that is not JSON is a parse error located in it.
    write_text_file(file, "{\"a\": }");
    try {
        (void)make_input_source(binding, message_type("test.blob"), days());
        ADD_FAILURE() << "the document was accepted";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::PARSE_ERROR) << e.what();
    }
}

// ---- What a source notices while it is built: notes() and counts() ----

namespace {

// An observation-csv source over `csv` for a run that starts at `start`.
std::unique_ptr<InputSource> csv_source_from(const ScratchDir& dir, const std::string& csv,
                                             double start) {
    const fs::path file = dir.path() / "observations.csv";
    write_text_file(file, csv);
    TimeBase time = days();
    time.start = start;
    return make_input_source(csv_binding(file), observation_batch(), time);
}

} // namespace

TEST(SourceNotes, RowsBeforeTheStartAreCountedAndNotedOnce) {
    ScratchDir dir;
    // Lines 2 to 5; the rows at lines 3 and 4 are before the start, the one at the start is not.
    const auto source = csv_source_from(dir,
                                        "series_id,variable_id,time,value\n"
                                        "S1,x,1.0,1\n"
                                        "S1,x,-1.0,2\n"
                                        "S1,x,-2.0,3\n"
                                        "S1,x,0.0,4\n",
                                        0.0);
    ASSERT_TRUE(source->counts().has_value());
    EXPECT_EQ(source->counts()->rows_total, 4u);
    EXPECT_EQ(source->counts()->rows_before_start, 2u);
    ASSERT_EQ(source->notes().size(), 1u);
    const praenora::Diagnostic& note = source->notes().front();
    EXPECT_EQ(note.severity, praenora::Severity::warning);
    EXPECT_EQ(note.code, ErrorCode::TARGET_DATA_INVALID);
    // The first of them in the file, not in time order: the row at line 3.
    EXPECT_EQ(note.location.line, 3);
    EXPECT_EQ(note.location.file, praenora::detail::path_to_utf8(dir.path() / "observations.csv"));
    EXPECT_NE(note.message.find("input 'observations'"), std::string::npos) << note.message;
    EXPECT_NE(note.message.find("2 of 4 rows"), std::string::npos) << note.message;
    EXPECT_NE(note.message.find("run.time.start (0.0 d)"), std::string::npos) << note.message;
    EXPECT_NE(note.message.find("CSV line 3 (time -1.0)"), std::string::npos) << note.message;
    // They are never delivered, as before: no window holds them.
    EXPECT_EQ(times(source->batch(0, 0.0, 2.0)), (std::vector<double>{0.0, 1.0}));
}

TEST(SourceNotes, NoRowBeforeTheStartGivesNoNote) {
    ScratchDir dir;
    const auto source = csv_source_from(dir,
                                        "series_id,variable_id,time,value\n"
                                        "S1,x,0.0,1\n"
                                        "S1,x,3.5,2\n",
                                        0.0);
    EXPECT_TRUE(source->notes().empty());
    ASSERT_TRUE(source->counts().has_value());
    EXPECT_EQ(source->counts()->rows_total, 2u);
    EXPECT_EQ(source->counts()->rows_before_start, 0u);
}

TEST(SourceNotes, TheStartOfTheRunDecidesWhichRowsAreEarly) {
    ScratchDir dir;
    const std::string csv = "series_id,variable_id,time,value\n"
                            "S1,x,1.0,1\n"
                            "S1,x,5.0,2\n"
                            "S1,x,9.0,3\n";
    const auto late = csv_source_from(dir, csv, 5.0);
    EXPECT_EQ(late->counts()->rows_before_start, 1u); // a row at the start is not early
    EXPECT_EQ(late->notes().size(), 1u);
    ScratchDir other;
    const auto early = csv_source_from(other, csv, -10.0);
    EXPECT_EQ(early->counts()->rows_before_start, 0u);
    EXPECT_TRUE(early->notes().empty());
}

TEST(SourceNotes, ManyEarlyRowsGiveOneNote) {
    ScratchDir dir;
    std::string csv = "series_id,variable_id,time,value\n";
    for (int i = 0; i < 2000; ++i) {
        csv += "S1,x," + std::to_string(-1 - i) + ",1\n";
    }
    csv += "S1,x,1.0,1\n";
    const auto source = csv_source_from(dir, csv, 0.0);
    EXPECT_EQ(source->counts()->rows_total, 2001u);
    EXPECT_EQ(source->counts()->rows_before_start, 2000u);
    ASSERT_EQ(source->notes().size(), 1u); // one diagnostic for the kind, with its count
    EXPECT_NE(source->notes().front().message.find("2000 of 2001 rows"), std::string::npos);
    EXPECT_EQ(source->notes().front().location.line, 2);
}

TEST(SourceNotes, AnEmptyTableHasCountsOfZeroAndNoNote) {
    ScratchDir dir;
    const auto source = csv_source_from(dir, "series_id,variable_id,time,value\n", 0.0);
    ASSERT_TRUE(source->counts().has_value());
    EXPECT_EQ(source->counts()->rows_total, 0u);
    EXPECT_EQ(source->counts()->rows_before_start, 0u);
    EXPECT_TRUE(source->notes().empty());
}

TEST(SourceNotes, AJsonDocumentHasNeitherCountsNorNotes) {
    ScratchDir dir;
    const fs::path file = dir.path() / "doc.json";
    write_text_file(file, "{\"a\": 1}\n");
    InputBinding binding;
    binding.input = "source";
    binding.source = "json-document";
    binding.path = file;
    const auto source = make_input_source(binding, message_type("test.blob"), days());
    EXPECT_FALSE(source->counts().has_value());
    EXPECT_TRUE(source->notes().empty());
}

TEST(SourceNotes, ARefusedRowStaysAnErrorWhateverElseTheFileHolds) {
    ScratchDir dir;
    try {
        (void)csv_source_from(dir,
                              "series_id,variable_id,time,value\n"
                              "S1,x,-5.0,1\n"
                              "S1,x,abc,2\n",
                              0.0);
        ADD_FAILURE() << "no error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    }
}
