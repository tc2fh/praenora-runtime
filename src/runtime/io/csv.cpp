#include "io/csv.hpp"

#include "document/json_util.hpp"

#include <praenora/runtime/error.hpp>

#include <set>
#include <utility>

namespace praenora::detail {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

class CsvParser {
public:
    CsvParser(std::string_view text, std::string_view origin, CsvDialect dialect)
        : text_(text), origin_(origin), dialect_(dialect) {}

    CsvTable run() {
        std::size_t i = 0;
        while (i < text_.size()) {
            const char c = text_[i];
            if (in_quotes_) {
                i = quoted_char(i);
            } else if (c == dialect_.delimiter) {
                end_field();
                ++i;
            } else if (c == '\n' || (c == '\r' && i + 1 < text_.size() && text_[i + 1] == '\n')) {
                end_record();
                i += c == '\r' ? 2 : 1;
                ++line_;
            } else if (c == dialect_.quote) {
                if (field_started_) {
                    fail(line_, after_quote_
                                    ? "a field continues after its closing quote"
                                    : "a quote inside an unquoted field; quote the whole field "
                                      "and write the quote twice");
                }
                start_record();
                field_started_ = true;
                in_quotes_ = true;
                quote_line_ = line_;
                ++i;
            } else {
                if (after_quote_) {
                    fail(line_, "a field continues after its closing quote");
                }
                start_record();
                field_started_ = true;
                field_ += c;
                ++i;
            }
        }
        if (in_quotes_) {
            fail(quote_line_, "a quoted field starting on line " + std::to_string(quote_line_) +
                                  " is never closed");
        }
        end_record();
        return finish();
    }

private:
    [[noreturn]] void fail(std::int64_t line, const std::string& message) const {
        SourceLocation location;
        location.file = std::string(origin_);
        location.line = line;
        throw Error(ErrorCode::TARGET_DATA_INVALID,
                    "CSV line " + std::to_string(line) + ": " + message, std::move(location));
    }

    // One character inside a quoted field, at `i`; returns the position after it.
    std::size_t quoted_char(std::size_t i) {
        const char c = text_[i];
        if (c == dialect_.quote) {
            if (i + 1 < text_.size() && text_[i + 1] == dialect_.quote) {
                field_ += dialect_.quote;
                return i + 2;
            }
            in_quotes_ = false;
            after_quote_ = true;
            return i + 1;
        }
        if (c == '\r' && i + 1 < text_.size() && text_[i + 1] == '\n') {
            field_ += '\n';
            ++line_;
            return i + 2;
        }
        if (c == '\n') {
            ++line_;
        }
        field_ += c;
        return i + 1;
    }

    void start_record() {
        if (!record_started_) {
            record_started_ = true;
            record_line_ = line_;
        }
    }

    void end_field() {
        start_record();
        if (!is_valid_utf8(field_)) {
            fail(record_line_, "the text is not valid UTF-8");
        }
        fields_.push_back(std::move(field_));
        field_.clear();
        field_started_ = false;
        after_quote_ = false;
    }

    void end_record() {
        if (!record_started_) {
            return; // an empty line
        }
        end_field();
        records_.push_back(std::move(fields_));
        lines_.push_back(record_line_);
        fields_.clear();
        record_started_ = false;
    }

    CsvTable finish() {
        CsvTable table;
        if (records_.empty()) {
            fail(1, "the table has no header row");
        }
        table.header = std::move(records_.front());
        std::set<std::string> names;
        for (const auto& name : table.header) {
            if (!names.insert(name).second) {
                fail(lines_.front(), "the column name '" + name + "' appears twice in the header");
            }
        }
        for (std::size_t r = 1; r < records_.size(); ++r) {
            if (records_[r].size() != table.header.size()) {
                fail(lines_[r], "the record has " + std::to_string(records_[r].size()) +
                                    " fields; the header has " +
                                    std::to_string(table.header.size()));
            }
            table.rows.push_back(std::move(records_[r]));
            table.row_lines.push_back(lines_[r]);
        }
        return table;
    }

    const std::string_view text_;
    const std::string_view origin_;
    const CsvDialect dialect_;
    std::vector<std::vector<std::string>> records_;
    std::vector<std::int64_t> lines_;
    std::vector<std::string> fields_;
    std::string field_;
    bool record_started_ = false, field_started_ = false, in_quotes_ = false, after_quote_ = false;
    std::int64_t line_ = 1, record_line_ = 1, quote_line_ = 1;
};

} // namespace

CsvTable parse_csv(std::string_view text, std::string_view origin, CsvDialect dialect) {
    if (text.compare(0, kBom.size(), kBom) == 0) {
        text.remove_prefix(kBom.size());
    }
    return CsvParser(text, origin, dialect).run();
}

} // namespace praenora::detail
