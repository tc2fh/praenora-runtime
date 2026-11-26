// Sources bound through an ode-uq-spec target descriptor (ADR-0042): the data file, the dialect,
// the column mapping, the checks of the rows against the variables, the duplicate policy and the
// roles that decide which rows reach the model.
#include "descriptor_fixtures.hpp"

#include "core/sha256.hpp"

#include <gtest/gtest.h>

#include <cctype>

using namespace descriptor_fixtures;
using praenora::detail::path_to_utf8;

TEST(DescriptorSource, ReadsTheSameRowsAsTheBindingWithoutADescriptor) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,1\n"
           "S1,x,1.5,2\n");
    const auto with = c.source();
    InputBinding plain;
    plain.input = "observations";
    plain.source = "observation-csv";
    plain.path = c.dir() / "obs.csv";
    const auto without = make_input_source(plain, observation_batch(), days());
    EXPECT_EQ(with->batch(0, 0.0, 3.0), without->batch(0, 0.0, 3.0));
    EXPECT_EQ(with->digest(), without->digest()); // the CSV's, as before
    EXPECT_TRUE(warnings(*with).empty());
    ASSERT_TRUE(with->descriptor().has_value());
    EXPECT_EQ(with->descriptor()->id, "d1");
    EXPECT_EQ(with->descriptor()->schema_version, "0.0.0");
    EXPECT_EQ(with->descriptor()->data_file, c.dir() / "obs.csv");
    EXPECT_EQ(with->descriptor()->digest, c.load().digest);
    EXPECT_FALSE(without->descriptor().has_value());
    ASSERT_TRUE(with->counts().has_value());
    EXPECT_EQ(with->counts()->rows_total, 2u);
    EXPECT_EQ(with->counts()->rows_not_delivered_by_role, 0u);
    EXPECT_FALSE(without->counts()->rows_not_delivered_by_role.has_value());
}

TEST(DescriptorSource, TheHrefIsRelativeToTheDescriptorNotToTheWorkingDirectory) {
    Case c;
    fs::create_directories(c.dir() / "data");
    write_text_file(c.dir() / "data" / "o b s.csv",
                    "series_id,variable_id,time,value\nS1,x,0.5,7\n");
    c.descriptor()["data"]["href"] = "../../data/o b s.csv";
    InputBinding binding = c.binding();
    binding.descriptor = c.dir() / "meta" / "more data" / "d.json";
    write_text_file(binding.descriptor, c.descriptor().dump());
    const auto source = make_input_source(binding, observation_batch(), days());
    EXPECT_EQ(source->batch(0, 0.0, 1.0).at("observations").at(0).at("value"), 7.0);
}

TEST(DescriptorSource, ANonAsciiHrefIsRead) {
    Case c;
    write_text_file(c.dir() / praenora::test::utf8_path("d\xC3\xA4ta.csv"),
                    "series_id,variable_id,time,value\nS1,x,0.5,3\n");
    c.descriptor()["data"]["href"] = "d\xC3\xA4ta.csv";
    const auto source = c.source();
    EXPECT_EQ(source->batch(0, 0.0, 1.0).at("observations").size(), 1u);
}

TEST(DescriptorSource, ThePathOfTheConfigurationMustNameTheFileOfTheHref) {
    Case c;
    InputBinding binding = c.binding();
    c.write_descriptor();
    fs::create_directories(c.dir() / "sub");
    binding.path = c.dir() / "sub" / ".." / "obs.csv"; // the same file, written another way round
    EXPECT_NO_THROW((void)make_input_source(binding, observation_batch(), days()));
    write_text_file(c.dir() / "other.csv", kSimpleCsv);
    binding.path = c.dir() / "other.csv";
    try {
        (void)make_input_source(binding, observation_batch(), days());
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::INPUT_BINDING_INVALID);
        EXPECT_TRUE(contains(e.what(), "input 'observations'")) << e.what();
        EXPECT_TRUE(contains(e.what(), "name one file")) << e.what();
        EXPECT_EQ(e.location().pointer, "/data/href");
        EXPECT_EQ(e.location().file, path_to_utf8(c.file()));
    }
}

TEST(DescriptorSource, ARemoteHrefNeedsAPathInTheConfiguration) {
    Case c;
    c.descriptor()["data"]["href"] = "https://example.org/obs.csv";
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::INPUT_BINDING_INVALID);
    EXPECT_TRUE(contains(e.what(), "remote")) << e.what();
    EXPECT_EQ(e.location().pointer, "/data/href");
    // With a path, the path wins and the href is not looked at.
    InputBinding binding = c.binding();
    binding.path = c.dir() / "obs.csv";
    const auto source = make_input_source(binding, observation_batch(), days());
    EXPECT_EQ(source->counts()->rows_total, 2u);
}

TEST(DescriptorSource, AnAbsoluteHrefIsReadAsWritten) {
    Case c;
    ScratchDir elsewhere;
    write_text_file(elsewhere.path() / "abs.csv", "series_id,variable_id,time,value\nS1,x,0.5,9\n");
    c.descriptor()["data"]["href"] = path_to_utf8(elsewhere.path() / "abs.csv");
    const auto source = c.source();
    EXPECT_EQ(source->batch(0, 0.0, 1.0).at("observations").at(0).at("value"), 9.0);
}

TEST(DescriptorSource, AMissingDataFileIsTargetDataUnavailable) {
    Case c;
    fs::remove(c.dir() / "obs.csv");
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_UNAVAILABLE);
    EXPECT_EQ(e.location().file, path_to_utf8(c.dir() / "obs.csv"));
}

TEST(DescriptorSource, ADescriptorErrorNamesTheInput) {
    Case c;
    c.descriptor()["id"] = "9";
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
    EXPECT_TRUE(contains(e.what(), "input 'observations'")) << e.what();
    EXPECT_EQ(e.location().file, path_to_utf8(c.file()));
}

TEST(DescriptorSource, TheDialectIsUsedForTheCsv) {
    Case c("series_id;variable_id;time;value\n"
           "S1;x;0.5;1\n"
           "S1;x;1.5;'2;3'\n", base_descriptor());
    c.descriptor()["dialect"] = {{"delimiter", ";"}, {"quotechar", "'"}};
    // The quoted cell holds a semicolon and is no number: the second row is refused at its line.
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_TRUE(contains(e.what(), "CSV line 3")) << e.what();

    Case ok("series_id;variable_id;time;value\n"
            "S1;x;0.5;1\n"
            "'S;1';x;1.5;2\n",
            base_descriptor());
    ok.descriptor()["dialect"] = {{"delimiter", ";"}, {"quotechar", "'"}};
    const json batch = ok.source()->batch(0, 0.0, 3.0);
    ASSERT_EQ(batch.at("observations").size(), 2u);
    EXPECT_EQ(batch.at("observations").at(1).at("series_id"), "S;1");
}

TEST(DescriptorSource, TheDefaultDialectIsTheUsualOne) {
    Case c("series_id;variable_id;time;value\nS1;x;0.5;1\n");
    // A semicolon file without a dialect is a one-column table: the columns are not found.
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
}

TEST(DescriptorSource, TheMissingTokenIsAnAbsentCell) {
    Case c("series_id,variable_id,time,value,missing\n"
           "S1,x,0.5,NA,true\n"
           "S1,x,1.5,4,NA\n"
           "S1,x,2.5,5,\n");
    c.descriptor()["dialect"] = {{"missing_token", "NA"}};
    c.descriptor()["columns"]["missing"] = "missing";
    const json batch = c.source()->batch(0, 0.0, 4.0);
    ASSERT_EQ(batch.at("observations").size(), 3u);
    EXPECT_TRUE(batch.at("observations").at(0).at("value").is_null()); // NA with missing=true
    EXPECT_EQ(batch.at("observations").at(0).at("missing"), true);
    EXPECT_EQ(batch.at("observations").at(1).at("value"), 4.0);
    EXPECT_EQ(batch.at("observations").at(1).at("missing"), false); // NA: absent, as empty
}

TEST(DescriptorSource, ANumberCellEqualToTheMissingTokenIsEmptyNotANumber) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,NA\n");
    c.descriptor()["dialect"] = {{"missing_token", "NA"}};
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_TRUE(contains(e.what(), "value is empty")) << e.what();
}

TEST(DescriptorSource, TheColumnsOfTheDescriptorMapTheHeaders) {
    Case c("sid,var,t,val,u\n"
           "S1,x,0.5,1,mmol\n");
    c.descriptor()["columns"] = {{"series_id", "sid"}, {"variable_id", "var"},
                                 {"time", "t"},        {"value", "val"},
                                 {"unit", "u"}};
    const json batch = c.source()->batch(0, 0.0, 1.0);
    ASSERT_EQ(batch.at("observations").size(), 1u);
    EXPECT_EQ(batch.at("observations").at(0).at("series_id"), "S1");
    EXPECT_EQ(batch.at("observations").at(0).at("unit"), "mmol");
}

TEST(DescriptorSource, AListedColumnThatTheCsvLacksIsAnError) {
    Case c;
    c.descriptor()["columns"]["unit"] = "unit";
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_TRUE(contains(e.what(), "'unit' is to be read from the header 'unit'")) << e.what();
}

TEST(DescriptorSource, ColumnsThatTheDescriptorDoesNotListAreIgnoredWithOneWarning) {
    Case c("series_id,variable_id,time,value,unit,weight,group_id\n"
           "S1,x,0.5,1,mmol,0,G\n"
           "S1,x,1.5,2,mmol,0,G\n");
    const auto source = c.source();
    // The weight is not read: its 0 does not become a weight of 0 (1 is the default), nor is the
    // unit or the group.
    const json row = source->batch(0, 0.0, 3.0).at("observations").at(0);
    EXPECT_EQ(row.at("weight"), 1.0);
    EXPECT_FALSE(row.contains("unit"));
    EXPECT_FALSE(row.contains("group_id"));
    ASSERT_EQ(source->notes().size(), 1u);
    const auto& note = source->notes().front();
    EXPECT_TRUE(contains(note.message, "the CSV has the columns 'unit', 'weight', 'group_id'"))
        << note.message;
    EXPECT_TRUE(contains(note.message, "which the descriptor does not list in columns"));
    EXPECT_TRUE(contains(note.message, "input 'observations'"));
    EXPECT_EQ(note.location.file, path_to_utf8(c.dir() / "obs.csv"));
    EXPECT_EQ(note.location.line, 1);
    EXPECT_EQ(note.severity, Severity::warning);
}

TEST(DescriptorSource, AColumnsMappingOfTheDescriptorSkipsTheHeaderOfTheSameNameWithoutAWarning) {
    // value is read from 'val'; a column 'value' of the table is not the observation column
    // 'value' then, and is not an unlisted optional one either.
    Case c("series_id,variable_id,time,val,value\n"
           "S1,x,0.5,1,99\n");
    c.descriptor()["columns"]["value"] = "val";
    const auto source = c.source();
    EXPECT_EQ(source->batch(0, 0.0, 1.0).at("observations").at(0).at("value"), 1.0);
    EXPECT_TRUE(source->notes().empty());
}

TEST(DescriptorSource, TheShaOfTheDescriptorMustBeTheCsvs) {
    const std::string digest = praenora::detail::sha256_hex(std::string(kSimpleCsv));
    {
        Case c;
        c.descriptor()["data"]["sha256"] = digest;
        EXPECT_NO_THROW((void)c.source());
    }
    {
        Case c;
        std::string upper = digest;
        for (char& ch : upper) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        c.descriptor()["data"]["sha256"] = upper;
        EXPECT_NO_THROW((void)c.source());
    }
    {
        Case c;
        c.descriptor()["data"]["sha256"] = std::string(64, '0');
        const Error e = c.error();
        EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
        EXPECT_EQ(e.location().pointer, "/data/sha256");
        EXPECT_TRUE(contains(e.what(), digest)) << e.what();
    }
}

TEST(DescriptorSource, ARowOfAnUnknownVariableIsRefused) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,1\n"
           "S1,nope,1.5,2\n");
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(e.location().line, 3);
    EXPECT_EQ(e.location().file, path_to_utf8(c.dir() / "obs.csv"));
    EXPECT_TRUE(contains(e.what(), "variable_id 'nope' is not one of the descriptor's variables"))
        << e.what();
}

TEST(DescriptorSource, TheUnitOfARowMustBeTheVariablesUnit) {
    Case c("series_id,variable_id,time,value,unit\n"
           "S1,x,0.5,1,mmol\n"
           "S1,x,1.5,2,g\n");
    c.descriptor()["columns"]["unit"] = "unit";
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(e.location().line, 3);
    EXPECT_TRUE(contains(e.what(), "unit 'g' differs from the unit 'mmol'")) << e.what();
}

TEST(DescriptorSource, AnEmptyUnitCellOrOneEqualToTheMissingTokenIsSkipped) {
    Case c("series_id,variable_id,time,value,unit\n"
           "S1,x,0.5,1,mmol\n"
           "S1,x,1.5,2,\n"
           "S1,x,2.5,3,NA\n");
    c.descriptor()["columns"]["unit"] = "unit";
    c.descriptor()["dialect"] = {{"missing_token", "NA"}};
    const json batch = c.source()->batch(0, 0.0, 4.0);
    EXPECT_EQ(batch.at("observations").size(), 3u);
}

TEST(DescriptorSource, ValuesOutsideTheBoundsAreKeptAndNotedOnce) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,5\n"
           "S1,x,1.5,-2\n"
           "S1,x,2.5,11\n"
           "S1,x,3.5,10\n"
           "S1,x,4.5,0\n");
    c.descriptor()["variables"][0]["lower_bound"] = 0;
    c.descriptor()["variables"][0]["upper_bound"] = 10;
    const auto source = c.source();
    ASSERT_EQ(source->notes().size(), 1u) << source->notes().size();
    const auto& note = source->notes().front();
    EXPECT_EQ(note.severity, Severity::warning);
    EXPECT_EQ(note.code, ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(note.location.line, 3); // the first, in the file
    EXPECT_TRUE(contains(note.message, "2 of 5 rows have a value outside the bounds")) << note.message;
    EXPECT_TRUE(contains(note.message, "CSV line 3 (variable 'x', value -2.0)")) << note.message;
    EXPECT_EQ(source->batch(0, 0.0, 10.0).at("observations").size(), 5u); // all are kept
}

TEST(DescriptorSource, MissingAndEmptyValuesAreNotOutOfBounds) {
    Case c("series_id,variable_id,time,value,missing\n"
           "S1,x,0.5,,true\n"
           "S1,x,1.5,50,true\n");
    c.descriptor()["variables"][0]["upper_bound"] = 10;
    c.descriptor()["columns"]["missing"] = "missing";
    EXPECT_TRUE(c.source()->notes().empty());
}

TEST(DescriptorSource, TheTimeUnitMustBeTheRunsUnit) {
    Case c;
    c.descriptor()["time_unit"] = "h";
    const Error e = c.error(days());
    EXPECT_EQ(e.code(), ErrorCode::INPUT_BINDING_INVALID);
    EXPECT_EQ(e.location().pointer, "/time_unit");
    EXPECT_TRUE(contains(e.what(), "time_unit 'h' differs from the run's time unit 'd'")) << e.what();
}

TEST(DescriptorSource, ADescriptorWithoutATimeUnitGivesAWarning) {
    Case c;
    c.descriptor().erase("time_unit");
    const auto source = c.source();
    ASSERT_EQ(source->notes().size(), 1u);
    EXPECT_TRUE(contains(source->notes().front().message,
                         "the descriptor has no time_unit; the run's unit 'd' is used"))
        << source->notes().front().message;
    EXPECT_EQ(source->notes().front().location.file, path_to_utf8(c.file()));
}

TEST(DescriptorSource, WithoutADuplicatePolicyDuplicatesPassAsBefore) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,1\n"
           "S1,x,0.5,2\n");
    EXPECT_EQ(c.source()->batch(0, 0.0, 1.0).at("observations").size(), 2u);
}

TEST(DescriptorSource, TheErrorPolicyRefusesTheSecondRowAtTheSameTime) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,1\n"
           "S2,x,0.5,2\n"
           "S1,x,0.5,3\n");
    c.descriptor()["duplicate_policy"] = "error";
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(e.location().line, 4); // the second S1 row; S2 is another series
    EXPECT_TRUE(contains(e.what(), "series 'S1' has a second row for variable 'x' at time 0.5"))
        << e.what();
    // Distinct times, series and variables are no duplicates.
    Case ok("series_id,variable_id,time,value\n"
            "S1,x,0.5,1\n"
            "S2,x,0.5,2\n"
            "S1,x,1.5,3\n");
    ok.descriptor()["duplicate_policy"] = "error";
    EXPECT_NO_THROW((void)ok.source());
}

TEST(DescriptorSource, RequireObservationIdAllowsDuplicatesThatHaveTheirOwnId) {
    const std::string header = "series_id,variable_id,time,value,observation_id\n";
    {
        Case c(header + "S1,x,0.5,1,a\nS1,x,0.5,2,b\n");
        c.descriptor()["duplicate_policy"] = "require_observation_id";
        c.descriptor()["columns"]["observation_id"] = "observation_id";
        EXPECT_NO_THROW((void)c.source());
    }
    for (const char* rows : {"S1,x,0.5,1,a\nS1,x,0.5,2,a\n",   // the same id twice
                             "S1,x,0.5,1,a\nS1,x,0.5,2,\n",    // one without an id
                             "S1,x,0.5,1,\nS1,x,0.5,2,\n"}) {  // neither has one
        SCOPED_TRACE(rows);
        Case c(header + rows);
        c.descriptor()["duplicate_policy"] = "require_observation_id";
        c.descriptor()["columns"]["observation_id"] = "observation_id";
        const Error e = c.error();
        EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
        EXPECT_EQ(e.location().line, 3);
        EXPECT_TRUE(contains(e.what(), "without a distinct observation_id")) << e.what();
    }
}

TEST(DescriptorSource, OnlyResponseRowsAreDelivered) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,1\n"
           "S1,age,0.5,60\n"
           "S1,x,1.5,2\n"
           "S1,dose,1.5,3\n"
           "S1,truth,2.5,4\n"
           "S1,free,2.5,5\n"
           "S1,age,9.5,61\n");
    auto& variables = c.descriptor()["variables"];
    variables.push_back(variable("age", "covariate"));
    variables.push_back(variable("dose", "input"));
    variables.push_back(variable("truth", "validation"));
    variables.push_back(variable("free")); // no role: response
    const auto source = c.source();
    const json batch = source->batch(0, 0.0, 10.0);
    EXPECT_EQ(variable_ids(batch), (std::vector<std::string>{"x", "x", "free"}));
    ASSERT_TRUE(source->counts().has_value());
    EXPECT_EQ(source->counts()->rows_total, 7u);
    EXPECT_EQ(source->counts()->rows_not_delivered_by_role, 4u);
    EXPECT_EQ(source->counts()->rows_before_start, 0u);
    // One warning for the whole kind, with the count and the first row.
    ASSERT_EQ(source->notes().size(), 1u);
    const auto& note = source->notes().front();
    EXPECT_EQ(note.severity, Severity::warning);
    EXPECT_EQ(note.code, ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(note.location.line, 3);
    EXPECT_TRUE(contains(note.message, "4 of 7 rows belong to variables whose role is not response"))
        << note.message;
    EXPECT_TRUE(contains(note.message, "CSV line 3 (variable 'age', role covariate)")) << note.message;
    // They count as nothing else: the exhaustion looks at the delivered rows only.
    EXPECT_FALSE(source->exhausted_after(2.5));
    EXPECT_TRUE(source->exhausted_after(2.6)); // the covariate row at 9.5 is no data to wait for
}

TEST(DescriptorSource, ARowOfAVariableOfAnotherRoleBeforeTheStartIsNotCountedTwice) {
    Case c("series_id,variable_id,time,value\n"
           "S1,x,-1.0,1\n"
           "S1,age,-1.0,60\n"
           "S1,x,1.0,2\n");
    c.descriptor()["variables"].push_back(variable("age", "covariate"));
    const auto source = c.source(days(0.0));
    EXPECT_EQ(source->counts()->rows_total, 3u);
    EXPECT_EQ(source->counts()->rows_before_start, 1u);
    EXPECT_EQ(source->counts()->rows_not_delivered_by_role, 1u);
    EXPECT_EQ(source->notes().size(), 2u); // the roles, and the early row of x
    (void)warning_with(*source, "1 of 3 rows have a time before run.time.start");
    (void)warning_with(*source, "1 of 3 rows belong to variables whose role is not response");
}

TEST(DescriptorSource, ManyRowsOfOtherRolesGiveOneWarning) {
    std::string csv = "series_id,variable_id,time,value\n";
    for (int i = 0; i < 3000; ++i) {
        csv += "S1,age," + std::to_string(i) + ",60\n";
    }
    Case c(csv);
    c.descriptor()["variables"].push_back(variable("age", "covariate"));
    const auto source = c.source();
    EXPECT_EQ(source->notes().size(), 1u);
    EXPECT_EQ(source->counts()->rows_not_delivered_by_role, 3000u);
    EXPECT_TRUE(source->batch(0, 0.0, 5000.0).at("observations").empty());
}

TEST(DescriptorSource, TheOutcomeListIsOpaqueToTheRuntime) {
    // The outcome variables carry the role validation; the x- list that names them is not read.
    Case c("series_id,variable_id,time,value\n"
           "S1,x,0.5,1\n"
           "S1,death,0.5,0\n");
    c.descriptor()["variables"].push_back(variable("death", "validation", "1"));
    c.descriptor()["x-circle-outcomes"] = {{"variable_ids", json::array({"death", "not-a-variable"})}};
    const auto source = c.source();
    EXPECT_EQ(variable_ids(source->batch(0, 0.0, 1.0)), (std::vector<std::string>{"x"}));
    EXPECT_EQ(source->counts()->rows_not_delivered_by_role, 1u);
}

// The missing token stands for an absent value (and the other optional measurement cells), never
// for an identifier or a time: a series or a variable called like the token is a series or a
// variable (ode-uq-spec conventions: "an empty value with missing=true is missing").
TEST(DescriptorSource, TheMissingTokenIsNeverAnIdentifier) {
    Case c("series_id,variable_id,time,value,observation_id\n"
           "NA,x,0.5,1,NA\n"
           "S1,NA,1.5,2,\n");
    c.descriptor()["dialect"] = {{"missing_token", "NA"}};
    c.descriptor()["columns"]["observation_id"] = "observation_id";
    c.descriptor()["variables"].push_back(variable("NA"));
    const json batch = c.source()->batch(0, 0.0, 3.0);
    ASSERT_EQ(batch.at("observations").size(), 2u);
    EXPECT_EQ(batch.at("observations").at(0).at("series_id"), "NA");
    EXPECT_EQ(batch.at("observations").at(0).at("observation_id"), "NA");
    EXPECT_EQ(batch.at("observations").at(1).at("variable_id"), "NA");
    // A time that is the token is not a number, as before.
    Case bad("series_id,variable_id,time,value\nS1,x,NA,1\n");
    bad.descriptor()["dialect"] = {{"missing_token", "NA"}};
    const Error e = bad.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_TRUE(contains(e.what(), "time 'NA'")) << e.what();
}

TEST(DescriptorSource, ADescriptorWithoutATimeUnitIsLocatedByPointerAndHasNoLine) {
    Case c;
    c.descriptor().erase("time_unit");
    const auto source = c.source();
    ASSERT_EQ(source->notes().size(), 1u);
    const auto& note = source->notes().front();
    EXPECT_EQ(note.location.file, path_to_utf8(c.file()));
    EXPECT_EQ(note.location.pointer, "/time_unit");
    EXPECT_EQ(note.location.line, 0);
}

// Duplicates are checked over every row, before the roles remove any: the data has to be sound
// whatever reaches the model.
TEST(DescriptorSource, DuplicatesAmongRowsOfOtherRolesAreRefusedToo) {
    Case c("series_id,variable_id,time,value\n"
           "S1,age,0.5,60\n"
           "S1,x,0.5,1\n"
           "S1,age,0.5,61\n");
    c.descriptor()["variables"].push_back(variable("age", "covariate"));
    c.descriptor()["duplicate_policy"] = "error";
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(e.location().line, 4);
    EXPECT_TRUE(contains(e.what(), "variable 'age'")) << e.what();
}

TEST(DescriptorSource, RequireObservationIdSaysWhenTheColumnIsNotMapped) {
    Case c("series_id,variable_id,time,value,observation_id\n"
           "S1,x,0.5,1,a\n"
           "S1,x,0.5,2,b\n");
    c.descriptor()["duplicate_policy"] = "require_observation_id";
    // observation_id is not in the descriptor's columns: no row has one.
    const Error e = c.error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_TRUE(contains(e.what(), "does not map the observation_id column")) << e.what();
    // Mapped, the same rows pass, and the message is not there for other refusals.
    Case mapped("series_id,variable_id,time,value,observation_id\n"
                "S1,x,0.5,1,a\n"
                "S1,x,0.5,2,a\n");
    mapped.descriptor()["duplicate_policy"] = "require_observation_id";
    mapped.descriptor()["columns"]["observation_id"] = "observation_id";
    EXPECT_FALSE(contains(mapped.error().what(), "does not map"));
}

// Only the rows that are removed need a second vector: with none, the rows are untouched (and the
// batch is the one without a descriptor).
TEST(DescriptorSource, WithoutRowsOfOtherRolesNothingIsRemoved) {
    Case c;
    const auto source = c.source();
    EXPECT_EQ(source->counts()->rows_not_delivered_by_role, 0u);
    EXPECT_EQ(source->batch(0, 0.0, 3.0).at("observations").size(), 2u);
}
