// CSV tables (RFC 4180, with a header row), as target data is written.
#ifndef PRAENORA_RUNTIME_IO_CSV_HPP
#define PRAENORA_RUNTIME_IO_CSV_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

struct CsvTable {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows; // each has header.size() fields
    std::vector<std::int64_t> row_lines;        // the 1-based line each row starts on
};

// The two characters a CSV dialect may change (the descriptor's `dialect` of ode-uq-spec): the
// field delimiter and the quote character. Each is one byte, neither is a line break, and they
// differ (the descriptor reader checks that).
struct CsvDialect {
    char delimiter = ',';
    char quote = '"';
};

// Parses UTF-8 CSV text: fields separated by `dialect.delimiter`, records by line breaks, the
// first record being the header.
// - A field may be quoted with `dialect.quote`; a quoted field may hold delimiters, line breaks
//   and quotes, the latter written twice. A quote may only open a field, and only a separator or
//   a line break may follow the closing quote.
// - A leading UTF-8 byte order mark is ignored, and CRLF reads like LF everywhere, inside quoted
//   fields too; a lone CR is ordinary text.
// - Empty lines are skipped. The last record needs no line break.
// - Every record has as many fields as the header, whose names are distinct.
// Throws praenora::Error(TARGET_DATA_INVALID) for anything else, located at `origin` and the line
// of the problem: a table without a header, a ragged record, misplaced quotes, a quoted field
// that never ends (located where it starts), a duplicate column name, or text that is not UTF-8.
CsvTable parse_csv(std::string_view text, std::string_view origin, CsvDialect dialect = {});

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_IO_CSV_HPP
