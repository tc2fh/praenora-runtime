// The ode-uq-spec target descriptor (ADR-0042): loading and checking the document, and the digest
// that goes into the fingerprint (and the test that keeps it complete).
#include "descriptor_fixtures.hpp"

#include "core/sha256.hpp"
#include "document/resources.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <functional>
#include <set>
#include <sstream>

using namespace descriptor_fixtures;
using praenora::detail::path_to_utf8;

// ---- loading ----

TEST(TargetDescriptor, AMinimalDescriptorLoads) {
    Case c;
    const TargetDescriptor d = c.load();
    EXPECT_EQ(d.id, "d1");
    EXPECT_EQ(d.schema_version, "0.0.0");
    EXPECT_EQ(d.href, "obs.csv");
    EXPECT_FALSE(d.href_remote);
    EXPECT_EQ(d.data_path.filename(), fs::path("obs.csv"));
    EXPECT_EQ(d.data_path.parent_path(), c.dir());
    EXPECT_EQ(d.dialect.delimiter, ',');
    EXPECT_EQ(d.dialect.quote, '"');
    EXPECT_FALSE(d.missing_token.has_value());
    EXPECT_EQ(d.columns.size(), 4u);
    EXPECT_EQ(d.time_unit, "d");
    ASSERT_EQ(d.variables.size(), 1u);
    EXPECT_EQ(d.variables[0].role, "response"); // no role: response
    EXPECT_EQ(d.digest.size(), 64u);
}

TEST(TargetDescriptor, AJsonDescriptorWithByteOrderMarkAndCrlfLoads) {
    ScratchDir dir;
    write_text_file(dir.path() / "d.json",
                    "\xEF\xBB\xBF{\r\n"
                    "  \"schema_version\": \"0.0.0\",\r\n"
                    "  \"id\": \"d1\",\r\n"
                    "  \"name\": \"Test\",\r\n"
                    "  \"data\": { \"id\": \"data1\", \"href\": \"obs.csv\" },\r\n"
                    "  \"columns\": { \"series_id\": \"series_id\", \"variable_id\": \"variable_id\","
                    " \"time\": \"time\", \"value\": \"value\" },\r\n"
                    "  \"variables\": [ { \"id\": \"x\", \"name\": \"X\", \"unit\": \"mmol\","
                    " \"value_type\": \"scalar\" } ]\r\n"
                    "}\r\n");
    const TargetDescriptor d = load_target_descriptor(dir.path() / "d.json");
    EXPECT_EQ(d.id, "d1");
    EXPECT_FALSE(d.time_unit.has_value());
}

// ode-uq-spec descriptors are JSON, and the content decides: YAML is refused whatever the file is
// called (a .json name does not make it JSON), JSON is read whatever its name.
TEST(TargetDescriptor, YamlIsRefusedWhateverTheFileIsCalled) {
    const std::string yaml = "schema_version: \"0.0.0\"\nid: d1\nname: Test\n";
    for (const char* name : {"d.json", "d.yaml", "d.yml", "d.txt", "d"}) {
        SCOPED_TRACE(name);
        ScratchDir dir;
        write_text_file(dir.path() / name, yaml);
        try {
            (void)load_target_descriptor(dir.path() / name);
            FAIL() << "accepted";
        } catch (const Error& e) {
            EXPECT_EQ(e.code(), ErrorCode::UNSUPPORTED) << e.what();
            EXPECT_TRUE(contains(e.what(), "must be JSON")) << e.what();
            EXPECT_EQ(e.location().file, path_to_utf8(dir.path() / name));
        }
    }
}

TEST(TargetDescriptor, JsonIsReadWhateverTheFileIsCalled) {
    for (const char* name : {"d.json", "D.JSON", "d.yaml", "d.txt", "d"}) {
        SCOPED_TRACE(name);
        Case c;
        ScratchDir dir;
        write_text_file(dir.path() / name, c.descriptor().dump(2));
        const TargetDescriptor d = load_target_descriptor(dir.path() / name);
        EXPECT_EQ(d.id, "d1");
        EXPECT_EQ(d.document.origin, path_to_utf8(dir.path() / name));
        // A located error names the real file, not the name the parse was given.
        json broken = c.descriptor();
        broken["id"] = "1bad";
        write_text_file(dir.path() / name, broken.dump(2));
        try {
            (void)load_target_descriptor(dir.path() / name);
            FAIL();
        } catch (const Error& e) {
            EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
            EXPECT_EQ(e.location().file, path_to_utf8(dir.path() / name));
            EXPECT_GT(e.location().line, 0);
        }
    }
}

TEST(TargetDescriptor, AMissingFileIsTargetDataUnavailable) {
    ScratchDir dir;
    try {
        (void)load_target_descriptor(dir.path() / "none.json");
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_UNAVAILABLE);
        EXPECT_EQ(e.location().file, path_to_utf8(dir.path() / "none.json"));
    }
}

TEST(TargetDescriptor, MalformedJsonIsRefusedWithTheParsersWords) {
    ScratchDir dir;
    write_text_file(dir.path() / "d.json", "{ \"id\": ");
    try {
        (void)load_target_descriptor(dir.path() / "d.json");
        FAIL();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::UNSUPPORTED);
        EXPECT_TRUE(contains(e.what(), "must be JSON")) << e.what();
    }
}

TEST(TargetDescriptor, EveryRequiredKeyIsRequired) {
    for (const char* key : {"schema_version", "id", "name", "data", "columns", "variables"}) {
        SCOPED_TRACE(key);
        Case c;
        c.descriptor().erase(key);
        const Error e = c.load_error();
        // A missing schema_version is a schema violation too: only a different one is a version
        // problem.
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION) << e.what();
        EXPECT_EQ(e.location().file, path_to_utf8(c.file()));
    }
    for (const char* key : {"series_id", "variable_id", "time", "value"}) {
        SCOPED_TRACE(key);
        Case c;
        c.descriptor()["columns"].erase(key);
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
    for (const char* key : {"id", "href"}) {
        SCOPED_TRACE(key);
        Case c;
        c.descriptor()["data"].erase(key);
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
    for (const char* key : {"id", "name", "unit", "value_type"}) {
        SCOPED_TRACE(key);
        Case c;
        c.descriptor()["variables"][0].erase(key);
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
}

TEST(TargetDescriptor, SchemaViolationsAreLocatedAtTheProblem) {
    Case c;
    c.descriptor()["variables"][0]["role"] = "outcome";
    const Error e = c.load_error();
    EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
    EXPECT_EQ(e.location().pointer, "/variables/0/role");
    EXPECT_GT(e.location().line, 0);
    EXPECT_TRUE(contains(e.what(), "/variables/0/role")) << e.what();
}

TEST(TargetDescriptor, MoreThanOneProblemIsCounted) {
    Case c;
    c.descriptor()["id"] = "1bad";
    c.descriptor()["variables"][0]["role"] = "outcome";
    const Error e = c.load_error();
    EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
    EXPECT_TRUE(contains(e.what(), "(and 1 more)")) << e.what();
}

TEST(TargetDescriptor, AnIdMustMatchThePattern) {
    for (const char* id : {"1abc", "a b", "", "_a"}) {
        SCOPED_TRACE(id);
        Case c;
        c.descriptor()["id"] = id;
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
    Case ok;
    ok.descriptor()["id"] = "A.b-c_1";
    (void)ok.load();
}

TEST(TargetDescriptor, EveryRoleOfTheSchemaIsAccepted) {
    for (const char* role : {"response", "covariate", "input", "validation"}) {
        SCOPED_TRACE(role);
        Case c;
        c.descriptor()["variables"][0]["role"] = role;
        EXPECT_EQ(c.load().variables[0].role, role);
    }
}

TEST(TargetDescriptor, UnknownKeysAreRefusedExceptXKeysAtTheTopAndInData) {
    {
        Case c;
        c.descriptor()["unknown"] = 1;
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
    }
    {
        Case c;
        c.descriptor()["x-circle-outcomes"] = {{"variable_ids", json::array({"x"})}};
        c.descriptor()["data"]["x-note"] = "kept";
        (void)c.load();
    }
    for (const char* place : {"dialect", "columns"}) {
        SCOPED_TRACE(place);
        Case c;
        c.descriptor()[place]["x-note"] = "refused";
        if (std::string(place) == "dialect") {
            c.descriptor()["dialect"]["delimiter"] = ",";
        }
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
    {
        Case c;
        c.descriptor()["variables"][0]["x-note"] = "refused";
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
}

TEST(TargetDescriptor, AnotherSchemaVersionIsUnsupported) {
    Case c;
    c.descriptor()["schema_version"] = "0.1.0";
    const Error e = c.load_error();
    EXPECT_EQ(e.code(), ErrorCode::FORMAT_VERSION_UNSUPPORTED);
    EXPECT_TRUE(contains(e.what(), "0.1.0") && contains(e.what(), "0.0.0")) << e.what();
    EXPECT_EQ(e.location().pointer, "/schema_version");
}

TEST(TargetDescriptor, TheDialectIsOneAsciiCharacterEach) {
    {
        Case c;
        c.descriptor()["dialect"] = {{"delimiter", ";"}, {"quotechar", "'"}, {"missing_token", "NA"}};
        const TargetDescriptor d = c.load();
        EXPECT_EQ(d.dialect.delimiter, ';');
        EXPECT_EQ(d.dialect.quote, '\'');
        EXPECT_EQ(d.missing_token, "NA");
    }
    {
        Case c; // one character, but two bytes
        c.descriptor()["dialect"] = {{"delimiter", "\xC2\xA7"}};
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::UNSUPPORTED) << e.what();
        EXPECT_EQ(e.location().pointer, "/dialect/delimiter");
    }
    {
        Case c;
        c.descriptor()["dialect"] = {{"delimiter", "\n"}};
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
        EXPECT_EQ(e.location().pointer, "/dialect/delimiter");
    }
    {
        Case c;
        c.descriptor()["dialect"] = {{"quotechar", "\r"}};
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
        EXPECT_EQ(e.location().pointer, "/dialect/quotechar");
    }
    {
        Case c; // only the delimiter is set, to the default quote: the key that is set is named
        c.descriptor()["dialect"] = {{"delimiter", "\""}};
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
        EXPECT_EQ(e.location().pointer, "/dialect/delimiter");
    }
    {
        Case c; // only the quotechar is set, to the default delimiter
        c.descriptor()["dialect"] = {{"quotechar", ","}};
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
        EXPECT_EQ(e.location().pointer, "/dialect/quotechar");
    }
    {
        Case c; // both set to one character
        c.descriptor()["dialect"] = {{"delimiter", ";"}, {"quotechar", ";"}};
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::SCHEMA_VIOLATION);
        EXPECT_EQ(e.location().pointer, "/dialect/quotechar");
    }
    {
        Case c; // a delimiter of two characters is a schema violation
        c.descriptor()["dialect"] = {{"delimiter", ";;"}};
        EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
    }
}

TEST(TargetDescriptor, DuplicateVariableIdsAreRefused) {
    Case c;
    c.descriptor()["variables"].push_back(variable("y"));
    c.descriptor()["variables"].push_back(variable("x"));
    const Error e = c.load_error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(e.location().pointer, "/variables/2/id");
    EXPECT_TRUE(contains(e.what(), "'x' appears twice")) << e.what();
}

TEST(TargetDescriptor, ALowerBoundAboveTheUpperBoundIsRefused) {
    Case c;
    c.descriptor()["variables"][0]["lower_bound"] = 5;
    c.descriptor()["variables"][0]["upper_bound"] = 4.5;
    const Error e = c.load_error();
    EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(e.location().pointer, "/variables/0");
    // Equal bounds are a point, which is fine.
    Case ok;
    ok.descriptor()["variables"][0]["lower_bound"] = 5;
    ok.descriptor()["variables"][0]["upper_bound"] = 5;
    (void)ok.load();
}

TEST(TargetDescriptor, AggregateDuplicatePoliciesAreUnsupported) {
    for (const char* policy : {"aggregate_mean", "aggregate_median"}) {
        SCOPED_TRACE(policy);
        Case c;
        c.descriptor()["duplicate_policy"] = policy;
        const Error e = c.load_error();
        EXPECT_EQ(e.code(), ErrorCode::UNSUPPORTED);
        EXPECT_EQ(e.location().pointer, "/duplicate_policy");
    }
    for (const char* policy : {"error", "require_observation_id"}) {
        SCOPED_TRACE(policy);
        Case c;
        c.descriptor()["duplicate_policy"] = policy;
        EXPECT_EQ(c.load().duplicate_policy, policy);
    }
    Case unknown;
    unknown.descriptor()["duplicate_policy"] = "keep_last";
    EXPECT_EQ(unknown.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
}

TEST(TargetDescriptor, HrefsWithASchemeAreRemote) {
    for (const char* href : {"https://example.org/a.csv", "s3://bucket/a.csv", "doi:10.1/x",
                             "file:///a.csv"}) {
        SCOPED_TRACE(href);
        Case c;
        c.descriptor()["data"]["href"] = href;
        const TargetDescriptor d = c.load();
        EXPECT_TRUE(d.href_remote);
        EXPECT_TRUE(d.data_path.empty());
    }
    // One letter is a Windows drive, as the spec's validator reads it; the rest are local paths.
    for (const char* href : {"C:/data/a.csv", "../data/a.csv", "sub dir/a b.csv", "a:b"}) {
        SCOPED_TRACE(href);
        Case c;
        c.descriptor()["data"]["href"] = href;
        EXPECT_FALSE(c.load().href_remote);
    }
}

TEST(TargetDescriptor, ARelativeHrefResolvesAgainstTheDescriptorsDirectory) {
    Case c;
    c.descriptor()["data"]["href"] = "../data/obs.csv";
    const TargetDescriptor d = c.load();
    EXPECT_EQ(d.data_path.lexically_normal(),
              (c.dir().parent_path() / "data" / "obs.csv").lexically_normal());
}

TEST(TargetDescriptor, TheShaIsReadInLowerCase) {
    Case c;
    c.descriptor()["data"]["sha256"] = std::string(64, 'A');
    EXPECT_EQ(c.load().sha256, std::string(64, 'a'));
    c.descriptor()["data"]["sha256"] = "abc";
    EXPECT_EQ(c.load_error().code(), ErrorCode::SCHEMA_VIOLATION);
}

// ---- the digest ----

namespace {

std::string digest_of(Case& c) { return c.load().digest; }

} // namespace

TEST(TargetDescriptorDigest, ProseAndPathsDoNotChangeIt) {
    Case c;
    const std::string before = digest_of(c);
    c.descriptor()["id"] = "other-id";
    c.descriptor()["name"] = "Another name";
    c.descriptor()["description"] = "Some words";
    c.descriptor()["data"]["id"] = "other-data";
    c.descriptor()["data"]["href"] = "../elsewhere/obs.csv";
    c.descriptor()["data"]["media_type"] = "text/csv";
    c.descriptor()["data"]["role"] = "observations";
    c.descriptor()["data"]["sha256"] = std::string(64, '0');
    c.descriptor()["data"]["x-note"] = "words";
    c.descriptor()["x-circle-outcomes"] = {{"variable_ids", json::array({"x"})}};
    c.descriptor()["variables"][0]["name"] = "Renamed";
    EXPECT_EQ(digest_of(c), before);
}

TEST(TargetDescriptorDigest, WhatDecidesTheDataDoes) {
    const auto changes = [](const std::function<void(json&)>& edit) {
        Case base;
        Case edited;
        edit(edited.descriptor());
        return digest_of(base) != digest_of(edited);
    };
    EXPECT_TRUE(changes([](json& d) { d["columns"]["value"] = "val"; }));
    EXPECT_TRUE(changes([](json& d) { d["columns"]["unit"] = "unit"; }));
    EXPECT_TRUE(changes([](json& d) { d["dialect"] = {{"delimiter", ";"}}; }));
    EXPECT_TRUE(changes([](json& d) { d["dialect"] = {{"quotechar", "'"}}; }));
    EXPECT_TRUE(changes([](json& d) { d["dialect"] = {{"missing_token", "NA"}}; }));
    EXPECT_TRUE(changes([](json& d) { d["time_unit"] = "h"; }));
    EXPECT_TRUE(changes([](json& d) { d.erase("time_unit"); }));
    EXPECT_TRUE(changes([](json& d) { d["duplicate_policy"] = "error"; }));
    EXPECT_TRUE(changes([](json& d) { d["variables"][0]["unit"] = "g"; }));
    EXPECT_TRUE(changes([](json& d) { d["variables"][0]["role"] = "covariate"; }));
    EXPECT_TRUE(changes([](json& d) { d["variables"][0]["lower_bound"] = 0; }));
    EXPECT_TRUE(changes([](json& d) { d["variables"][0]["upper_bound"] = 9; }));
    EXPECT_TRUE(changes([](json& d) { d["variables"][0]["id"] = "z"; }));
    EXPECT_TRUE(changes([](json& d) { d["variables"].push_back(variable("y")); }));
}

TEST(TargetDescriptorDigest, ADefaultWrittenOutEqualsOneLeftOut) {
    Case plain;
    Case explicit_defaults;
    explicit_defaults.descriptor()["dialect"] = {
        {"delimiter", ","}, {"quotechar", "\""}, {"header", true}, {"encoding", "UTF-8"}};
    explicit_defaults.descriptor()["variables"][0]["role"] = "response";
    EXPECT_EQ(digest_of(plain), digest_of(explicit_defaults));
}

TEST(TargetDescriptorDigest, TheOrderOfTheVariablesDoesNotMatter) {
    Case a;
    a.descriptor()["variables"].push_back(variable("y", "covariate"));
    Case b;
    b.descriptor()["variables"] = json::array({variable("y", "covariate"), variable("x")});
    b.descriptor()["variables"][1]["name"] = "X";
    EXPECT_EQ(digest_of(a), digest_of(b));
}

TEST(TargetDescriptorDigest, IsTheSha256OfTheCanonicalSubset) {
    Case c;
    // The subset by hand: defaults filled in, only the hashed members, variables sorted.
    const json subset = {
        {"columns",
         {{"series_id", "series_id"},
          {"variable_id", "variable_id"},
          {"time", "time"},
          {"value", "value"}}},
        {"dialect", {{"delimiter", ","}, {"quotechar", "\""}, {"header", true}, {"encoding", "UTF-8"}}},
        {"time_unit", "d"},
        {"variables",
         json::array({{{"id", "x"}, {"unit", "mmol"}, {"value_type", "scalar"}, {"role", "response"}}})}};
    EXPECT_EQ(digest_of(c), praenora::detail::sha256_hex(subset.dump()));
}

namespace {

// The leaf properties of `schema` as the lists name them: "a.b", "list[].c", "x-*" for the
// patternProperties "^x-".
void collect_leaves(const json& schema, const std::string& prefix, std::set<std::string>& out) {
    const auto join = [&](const std::string& name) { return prefix.empty() ? name : prefix + "." + name; };
    if (schema.contains("patternProperties")) {
        for (const auto& item : schema.at("patternProperties").items()) {
            EXPECT_EQ(item.key(), "^x-");
            out.insert(join("x-*"));
        }
    }
    if (!schema.contains("properties")) {
        return;
    }
    for (const auto& [name, child] : schema.at("properties").items()) {
        if (child.contains("properties") || child.contains("patternProperties")) {
            collect_leaves(child, join(name), out);
        } else if (child.value("type", "") == "array" && child.contains("items") &&
                   child.at("items").contains("properties")) {
            collect_leaves(child.at("items"), join(name) + "[]", out);
        } else {
            out.insert(join(name));
        }
    }
}

} // namespace

TEST(TargetDescriptorDigest, EveryPropertyOfTheSchemaIsHashedOrExcluded) {
    std::set<std::string> leaves;
    collect_leaves(praenora::detail::embedded_target_descriptor_schema(), "", leaves);
    // The schema has more than a handful of leaves; the walk did not stop early.
    EXPECT_GT(leaves.size(), 30u);
    std::set<std::string> hashed(praenora::detail::descriptor_hashed_paths().begin(),
                                 praenora::detail::descriptor_hashed_paths().end());
    std::set<std::string> excluded(praenora::detail::descriptor_excluded_paths().begin(),
                                   praenora::detail::descriptor_excluded_paths().end());
    EXPECT_EQ(hashed.size(), praenora::detail::descriptor_hashed_paths().size()); // no repeats
    EXPECT_EQ(excluded.size(), praenora::detail::descriptor_excluded_paths().size());
    for (const std::string& path : leaves) {
        SCOPED_TRACE(path);
        EXPECT_EQ(hashed.count(path) + excluded.count(path), 1u)
            << "classify this property of the descriptor schema: add it to the hashed or the "
               "excluded paths of target_descriptor.cpp";
    }
    for (const std::string& path : hashed) {
        EXPECT_EQ(leaves.count(path), 1u) << path << " is hashed but not in the schema";
    }
    for (const std::string& path : excluded) {
        EXPECT_EQ(leaves.count(path), 1u) << path << " is excluded but not in the schema";
    }
}

TEST(TargetDescriptorDigest, EveryHashedPathChangesTheDigest) {
    // The hashed list and the code that builds the subset must agree: each hashed property, set
    // to another value, changes the digest. (Columns need a header that the test never reads.)
    const std::vector<std::pair<std::string, json>> edits = {
        {"columns.observation_id", "oid"},   {"columns.unit", "u"},
        {"columns.missing", "m"},            {"columns.censoring", "c"},
        {"columns.lower_limit", "ll"},       {"columns.upper_limit", "ul"},
        {"columns.weight", "w"},             {"columns.group_id", "g"},
        {"columns.batch_id", "b"},           {"columns.series_id", "s2"},
        {"columns.variable_id", "v2"},       {"columns.time", "t2"},
        {"columns.value", "val2"},           {"dialect.delimiter", ";"},
        {"dialect.quotechar", "'"},          {"dialect.missing_token", "NA"},
        {"time_unit", "h"},                  {"duplicate_policy", "error"},
        {"variables[].id", "z"},             {"variables[].unit", "g"},
        {"variables[].role", "input"},       {"variables[].lower_bound", 1},
        {"variables[].upper_bound", 2},
    };
    std::set<std::string> covered;
    for (const auto& [path, value] : edits) {
        SCOPED_TRACE(path);
        covered.insert(path);
        Case base;
        Case edited;
        json& d = edited.descriptor();
        const std::string variables = "variables[].";
        if (path.compare(0, variables.size(), variables) == 0) {
            d["variables"][0][path.substr(variables.size())] = value;
        } else if (const auto dot = path.find('.'); dot != std::string::npos) {
            d[path.substr(0, dot)][path.substr(dot + 1)] = value;
        } else {
            d[path] = value;
        }
        EXPECT_NE(digest_of(base), digest_of(edited));
    }
    // The members that the schema fixes to one value cannot change; the others are all above.
    for (const std::string& path : praenora::detail::descriptor_hashed_paths()) {
        if (path == "dialect.header" || path == "dialect.encoding" ||
            path == "variables[].value_type") {
            continue;
        }
        EXPECT_EQ(covered.count(path), 1u) << path << " has no edit in this test";
    }
}

TEST(TargetDescriptorSchema, IsTheOneThatOdeUqSpecPublishes) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* dir = std::getenv("PRAENORA_ODE_UQ_SPEC_DIR");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (dir == nullptr) {
        GTEST_SKIP() << "set PRAENORA_ODE_UQ_SPEC_DIR to a checkout of ode-uq-spec (branch start)";
    }
    std::ifstream in(fs::path(dir) / "schemas" / "target.schema.json", std::ios::binary);
    ASSERT_TRUE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    // Checkouts with CRLF line endings are the same file.
    const json upstream = json::parse(text.str());
    EXPECT_EQ(praenora::detail::embedded_target_descriptor_schema(), upstream);
}


TEST(TargetDescriptorDigest, ANumberIsOneValueHoweverItIsWritten) {
    // 0, 0.0, -0 and 1e0 forms of the same bound give one digest.
    const auto digest_with = [](const std::string& lower, const std::string& upper) {
        Case c;
        c.descriptor()["variables"][0]["lower_bound"] = 7777;
        c.descriptor()["variables"][0]["upper_bound"] = 8888;
        std::string text = c.descriptor().dump(2);
        const auto replace = [&](const std::string& from, const std::string& to) {
            const auto at = text.find(from);
            EXPECT_NE(at, std::string::npos) << from;
            text.replace(at, from.size(), to);
        };
        replace("7777", lower);
        replace("8888", upper);
        write_text_file(c.file(), text);
        return load_target_descriptor(c.file()).digest;
    };
    const std::string zero_one = digest_with("0", "1");
    EXPECT_EQ(digest_with("0.0", "1.0"), zero_one);
    EXPECT_EQ(digest_with("-0", "1e0"), zero_one);
    EXPECT_EQ(digest_with("-0.0", "10e-1"), zero_one);
    EXPECT_EQ(digest_with("0e5", "0.1E1"), zero_one);
    EXPECT_NE(digest_with("0", "2"), zero_one);
}

// The embedded copy is the file recorded in spec/ode-uq/README.md: this test runs everywhere, the
// comparison with a checkout of the spec only where one is named. The hash is that of the git
// blob (LF line endings).
TEST(TargetDescriptorSchema, IsTheFileThatTheReadmeRecords) {
    const auto text = praenora::detail::find_embedded_resource("ode-uq/target.0.0.schema.json");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(praenora::detail::sha256_hex(*text),
              "0c364a8e754cdd961b740c61d1229fbb79f545f9b063b34ce21436f09589f910")
        << "the embedded schema changed: update the hash here and in spec/ode-uq/README.md, and "
           "classify every new property in target_descriptor.cpp";
}

namespace {

void collect_value_sets(const json& schema, const std::string& path,
                        std::map<std::string, std::string>& out) {
    if (schema.is_object()) {
        for (const char* keyword : {"enum", "const"}) {
            if (schema.contains(keyword)) {
                out[path + "#" + keyword] = schema.at(keyword).dump();
            }
        }
        for (const auto& [key, child] : schema.items()) {
            if (key == "enum" || key == "const") {
                continue;
            }
            collect_value_sets(child, path.empty() ? key : path + "." + key, out);
        }
    } else if (schema.is_array()) {
        for (std::size_t i = 0; i < schema.size(); ++i) {
            collect_value_sets(schema[i], path + "[" + std::to_string(i) + "]", out);
        }
    }
}

} // namespace

// The values the runtime acts on are enumerations and constants of the schema: a role, a duplicate
// policy, the version. A new value in an updated copy of the schema has to be looked at (does the
// runtime know what to do with it, does the digest cover it?) before this snapshot is changed.
TEST(TargetDescriptorSchema, EnumerationsAndConstantsAreTheOnesTheRuntimeKnows) {
    std::map<std::string, std::string> found;
    collect_value_sets(praenora::detail::embedded_target_descriptor_schema(), "", found);
    const std::map<std::string, std::string> known = {
        {"properties.dialect.properties.encoding#const", "\"UTF-8\""},
        {"properties.dialect.properties.header#const", "true"},
        {"properties.duplicate_policy#enum",
         "[\"require_observation_id\",\"aggregate_mean\",\"aggregate_median\",\"error\"]"},
        {"properties.schema_version#const", "\"0.0.0\""},
        {"properties.variables.items.properties.role#enum",
         "[\"response\",\"covariate\",\"input\",\"validation\"]"},
        {"properties.variables.items.properties.value_type#const", "\"scalar\""},
    };
    EXPECT_EQ(found, known);
}
