// CSV parsing (RFC 4180 with a header row): quoting, embedded line breaks, byte order marks and
// CRLF line endings, and the errors of malformed tables.
#include "io/csv.hpp"

#include "core/paths.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::CsvTable;
using praenora::detail::parse_csv;
using Rows = std::vector<std::vector<std::string>>;

namespace {

// Parses `text` expecting TARGET_DATA_INVALID and returns the error.
Error csv_error(const std::string& text) {
    try {
        (void)parse_csv(text, "data.csv");
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::TARGET_DATA_INVALID) << e.what();
        EXPECT_EQ(e.location().file, "data.csv");
        return e;
    }
    ADD_FAILURE() << "no error for:\n" << text;
    return Error(ErrorCode::OK, "no error");
}

} // namespace

TEST(Csv, QuotedFieldsAndEscapes) {
    const CsvTable table = parse_csv("a,b,c,d\n"
                                     "\"x, y\",\"say \"\"hi\"\"\",plain,\n"
                                     "\"\",,\"\"\"\",last\n",
                                     "data.csv");
    EXPECT_EQ(table.header, (std::vector<std::string>{"a", "b", "c", "d"}));
    EXPECT_EQ(table.rows, (Rows{{"x, y", "say \"hi\"", "plain", ""}, {"", "", "\"", "last"}}));
    EXPECT_EQ(table.row_lines, (std::vector<std::int64_t>{2, 3}));
    // The last record needs no line break; a header-only table has no rows.
    EXPECT_EQ(parse_csv("a,b\n1,2", "data.csv").rows, (Rows{{"1", "2"}}));
    EXPECT_TRUE(parse_csv("a,b\n", "data.csv").rows.empty());
}

TEST(Csv, EmbeddedNewlineInQuotes) {
    const CsvTable table = parse_csv("id,note\n"
                                     "1,\"line one\n"
                                     "line two\"\n"
                                     "2,after\n",
                                     "data.csv");
    EXPECT_EQ(table.rows, (Rows{{"1", "line one\nline two"}, {"2", "after"}}));
    EXPECT_EQ(table.row_lines, (std::vector<std::int64_t>{2, 4}));
    // A line break inside quotes is CRLF in a file edited on Windows; it reads as LF.
    EXPECT_EQ(parse_csv("id,note\r\n1,\"a\r\nb\"\r\n", "data.csv").rows, (Rows{{"1", "a\nb"}}));
}

TEST(Csv, BomAndCrlfParseIdentically) {
    const auto windows = praenora::test::fixture_path("data/bom-crlf/observations.csv");
    const auto unix_lf = praenora::test::fixture_path("data/observations.csv");
    const std::string windows_bytes = praenora::detail::read_file_bytes(windows);
    const std::string lf_bytes = praenora::detail::read_file_bytes(unix_lf);
    // Guard against the fixtures being normalized by a checkout.
    ASSERT_EQ(windows_bytes.rfind("\xEF\xBB\xBF", 0), 0u) << "the bom-crlf fixture lost its BOM";
    ASSERT_NE(windows_bytes.find("\r\n"), std::string::npos) << "the bom-crlf fixture lost CRLF";
    ASSERT_EQ(lf_bytes.find('\r'), std::string::npos);
    ASSERT_NE(lf_bytes.rfind("\xEF\xBB\xBF", 0), 0u);

    const CsvTable a = parse_csv(windows_bytes, "windows.csv");
    const CsvTable b = parse_csv(lf_bytes, "lf.csv");
    EXPECT_EQ(a.header, b.header);
    EXPECT_EQ(a.rows, b.rows);
    EXPECT_EQ(a.row_lines, b.row_lines);
    ASSERT_EQ(a.header.size(), 13u);
    EXPECT_EQ(a.header.front(), "observation_id"); // not "\xEF\xBB\xBFobservation_id"
    ASSERT_EQ(a.rows.size(), 7u);
    EXPECT_EQ(a.rows.back().back(), "batch\ntwo");
    EXPECT_EQ(a.row_lines.back(), 8);
}

TEST(Csv, RaggedRowIsTargetDataInvalidWithLine) {
    const Error short_row = csv_error("a,b,c\n1,2,3\n4,5\n6,7,8\n");
    EXPECT_EQ(short_row.location().line, 3);
    EXPECT_NE(std::string(short_row.what()).find("2 fields"), std::string::npos)
        << short_row.what();
    const Error long_row = csv_error("a,b\n1,2\n\"x\ny\",2,3\n");
    EXPECT_EQ(long_row.location().line, 3);
}

TEST(Csv, MalformedQuotingIsTargetDataInvalidWithLine) {
    EXPECT_EQ(csv_error("a,b\n1,\"open\n2,3\n").location().line, 2); // never closed
    EXPECT_EQ(csv_error("a,b\n1,x\"y\n").location().line, 2);        // quote inside a field
    EXPECT_EQ(csv_error("a,b\n1,\"x\"y\n").location().line, 2);      // text after the quote
}

TEST(Csv, HeaderRules) {
    EXPECT_EQ(csv_error("").location().line, 1);                  // no header row
    EXPECT_EQ(csv_error("\n\n").location().line, 1);              // blank lines only
    EXPECT_EQ(csv_error("a,b,a\n1,2,3\n").location().line, 1);    // duplicate column name
    EXPECT_EQ(csv_error("a,b\n1,\"\xFF\"\n").location().line, 2); // not UTF-8
}

TEST(Csv, BlankLinesAreSkipped) {
    const CsvTable table = parse_csv("a,b\n\n1,2\n\r\n3,4\n\n", "data.csv");
    EXPECT_EQ(table.rows, (Rows{{"1", "2"}, {"3", "4"}}));
    EXPECT_EQ(table.row_lines, (std::vector<std::int64_t>{3, 5}));
}

TEST(Csv, ADialectChangesTheDelimiterAndTheQuote) {
    const praenora::detail::CsvDialect dialect{';', '\''};
    const CsvTable table = parse_csv("a;b;c\n"
                                     "'x; y';'say ''hi''';plain\n"
                                     "1,5;\"q\";\n",
                                     "data.csv", dialect);
    EXPECT_EQ(table.header, (std::vector<std::string>{"a", "b", "c"}));
    // A comma and a double quote are ordinary text now.
    EXPECT_EQ(table.rows, (Rows{{"x; y", "say 'hi'", "plain"}, {"1,5", "\"q\"", ""}}));
}

TEST(Csv, ADialectQuoteKeepsLineBreaksInsideQuotedFields) {
    const praenora::detail::CsvDialect dialect{'\t', '|'};
    const CsvTable table = parse_csv("id\tnote\n1\t|two\nlines|\n", "data.csv", dialect);
    EXPECT_EQ(table.rows, (Rows{{"1", "two\nlines"}}));
}

TEST(Csv, TheDefaultDialectIsTheCommaAndTheDoubleQuote) {
    EXPECT_EQ(parse_csv("a,b\n\"1,2\",3\n", "data.csv").rows,
              parse_csv("a,b\n\"1,2\",3\n", "data.csv", {}).rows);
    // With another delimiter a comma does not split, and a double quote opens nothing.
    const CsvTable table = parse_csv("a;b\n1,2;\"3\n", "data.csv", {';', '\''});
    EXPECT_EQ(table.rows, (Rows{{"1,2", "\"3"}}));
}
