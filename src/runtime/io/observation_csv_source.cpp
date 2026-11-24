// The observation-csv source: replays a long-form observation CSV one tick window at a time.
#include "io/input_source.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "io/csv.hpp"

#include <praenora/runtime/error.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

using nlohmann::json;

bool is_required(const std::string& column) {
    return column == "series_id" || column == "variable_id" || column == "time" ||
           column == "value";
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

struct Row {
    double time = 0;
    std::string series, variable, observation_id;
    std::int64_t line = 0;
    json data;
};

// Reads the rows of one table: the column layout, then each row by the observation rules.
class RowReader {
public:
    RowReader(const CsvTable& table, const InputBinding& binding, std::string origin)
        : table_(table), binding_(binding), origin_(std::move(origin)) {
        map_columns();
    }

    std::vector<Row> rows() const {
        std::vector<Row> result;
        result.reserve(table_.rows.size());
        for (std::size_t r = 0; r < table_.rows.size(); ++r) {
            result.push_back(read_row(table_.rows[r], table_.row_lines[r]));
        }
        return result;
    }

private:
    [[noreturn]] void bad(std::int64_t line, const std::string& message) const {
        SourceLocation location;
        location.file = origin_;
        location.line = line;
        throw Error(ErrorCode::TARGET_DATA_INVALID,
                    "CSV line " + std::to_string(line) + ": " + message, std::move(location));
    }

    void map_columns() {
        std::map<std::string, std::size_t> by_header;
        for (std::size_t i = 0; i < table_.header.size(); ++i) {
            by_header.emplace(table_.header[i], i);
        }
        const auto& known = observation_columns();
        for (const auto& entry : binding_.columns) {
            if (std::find(known.begin(), known.end(), entry.first) == known.end()) {
                throw Error(ErrorCode::INPUT_BINDING_INVALID,
                            "'" + entry.first + "' is not an observation column");
            }
        }
        std::map<std::string, std::string> reader_of; // header -> the column reading it
        for (const auto& column : known) {
            const auto remapped = binding_.columns.find(column);
            const std::string& header =
                remapped == binding_.columns.end() ? column : remapped->second;
            const auto found = by_header.find(header);
            if (found == by_header.end()) {
                if (remapped != binding_.columns.end()) {
                    bad(1, "the column '" + column + "' is to be read from the header '" + header +
                               "', which the table does not have");
                }
                if (is_required(column)) {
                    bad(1, "the required column '" + column +
                               "' is missing (required: series_id, variable_id, time, value)");
                }
                continue;
            }
            // A configuration's mapping never gets here with such a clash: loading it refuses
            // one (CONFIG_INVALID), and validation then builds no source for the input.
            const auto [other, inserted] = reader_of.emplace(header, column);
            if (!inserted) {
                throw Error(ErrorCode::INPUT_BINDING_INVALID,
                            "the columns '" + other->second + "' and '" + column +
                                "' would both be read from the header '" + header + "'");
            }
            index_.emplace(column, found->second);
        }
    }

    // The cell of `column` in `fields`; empty when the table has no such column.
    const std::string& cell(const std::vector<std::string>& fields,
                            const std::string& column) const {
        static const std::string kEmpty;
        const auto it = index_.find(column);
        return it == index_.end() ? kEmpty : fields[it->second];
    }

    std::optional<double> number(const std::vector<std::string>& fields, const std::string& column,
                                 std::int64_t line) const {
        const std::string& text = cell(fields, column);
        if (text.empty()) {
            return std::nullopt;
        }
        const auto value = parse_decimal(text);
        if (!value) {
            bad(line,
                column + " '" + text + "' is not a decimal number within the range of a double");
        }
        return value;
    }

    Row read_row(const std::vector<std::string>& fields, std::int64_t line) const {
        Row row;
        row.line = line;
        row.series = cell(fields, "series_id");
        row.variable = cell(fields, "variable_id");
        row.observation_id = cell(fields, "observation_id");
        if (row.series.empty()) {
            bad(line, "series_id is empty");
        }
        if (row.variable.empty()) {
            bad(line, "variable_id is empty");
        }
        const auto time = number(fields, "time", line);
        if (!time) {
            bad(line, "time is empty");
        }
        row.time = *time;

        const std::string missing_text = lower(cell(fields, "missing"));
        if (!missing_text.empty() && missing_text != "true" && missing_text != "false") {
            bad(line,
                "missing must be true or false (or empty), not '" + cell(fields, "missing") + "'");
        }
        const bool missing = missing_text == "true";

        std::string censoring = cell(fields, "censoring");
        if (censoring.empty()) {
            censoring = "none";
        } else if (censoring != "none" && censoring != "left" && censoring != "right" &&
                   censoring != "interval") {
            bad(line, "censoring must be none, left, right or interval (or empty), not '" +
                          censoring + "'");
        }
        const auto lower_limit = number(fields, "lower_limit", line);
        const auto upper_limit = number(fields, "upper_limit", line);
        const auto weight = number(fields, "weight", line).value_or(1.0);
        if (weight < 0) {
            bad(line, "weight " + cell(fields, "weight") + " is negative; a weight is >= 0");
        }
        auto value = number(fields, "value", line);
        if (!value) {
            const bool limited = (censoring == "left" && upper_limit) ||
                                 (censoring == "right" && lower_limit) ||
                                 (censoring == "interval" && lower_limit && upper_limit);
            if (!missing && !limited) {
                bad(line, "value is empty, but the row is neither missing (missing=true) nor "
                          "censored with its limit (left: upper_limit, right: lower_limit, "
                          "interval: both)");
            }
        }
        if (missing) {
            value.reset();
        }

        json& data = row.data;
        data = json::object();
        data["series_id"] = row.series;
        data["variable_id"] = row.variable;
        data["time"] = row.time;
        data["value"] = value ? json(*value) : json(nullptr);
        data["missing"] = missing;
        data["censoring"] = censoring;
        data["weight"] = weight;
        if (lower_limit) {
            data["lower_limit"] = *lower_limit;
        }
        if (upper_limit) {
            data["upper_limit"] = *upper_limit;
        }
        for (const char* optional : {"observation_id", "unit", "group_id", "batch_id"}) {
            const std::string& text = cell(fields, optional);
            if (!text.empty()) {
                data[optional] = text;
            }
        }
        return row;
    }

    const CsvTable& table_;
    const InputBinding& binding_;
    const std::string origin_;
    std::map<std::string, std::size_t> index_; // observation column -> field index
};

// Refuses an observation CSV of `size` bytes when that is more than `max_bytes`.
void check_csv_size(std::uintmax_t size, std::uintmax_t max_bytes, const std::string& origin) {
    if (size <= max_bytes) {
        return;
    }
    std::string limit = std::to_string(max_bytes) + " bytes";
    if (max_bytes % (std::uintmax_t{1} << 20) == 0) {
        limit += " (" + std::to_string(max_bytes >> 20) + " MiB)";
    }
    SourceLocation location;
    location.file = origin;
    throw Error(ErrorCode::TARGET_DATA_INVALID,
                "the observation CSV is too large: " + std::to_string(size) +
                    " bytes; the limit is " + limit,
                std::move(location));
}

class ObservationCsvSource final : public InputSource {
public:
    ObservationCsvSource(const InputBinding& binding, const MessageTypeDef& port_type,
                         const TimeBase& time, std::uintmax_t max_bytes)
        : time_unit_(time.unit) {
        const std::string origin = path_to_utf8(binding.path);
        // The size first, so a file beyond the limit is never read. A size that cannot be taken
        // is left to the read, which reports why.
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(binding.path, ec);
        if (!ec) {
            check_csv_size(size, max_bytes, origin);
        }
        std::string bytes;
        try {
            bytes = read_file_bytes(binding.path);
        } catch (const Error& e) {
            SourceLocation location;
            location.file = origin;
            throw Error(ErrorCode::TARGET_DATA_UNAVAILABLE,
                        std::string("cannot read the observation CSV: ") + e.what(),
                        std::move(location));
        }
        check_csv_size(bytes.size(), max_bytes, origin); // the file may have grown meanwhile
        digest_ = sha256_hex(bytes);
        const CsvTable table = parse_csv(bytes, origin);
        rows_ = RowReader(table, binding, origin).rows();
        count_rows(binding.input, origin, time.start); // in file order, before the sort
        std::stable_sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
            if (a.time != b.time) {
                return a.time < b.time;
            }
            if (a.series != b.series) {
                return a.series < b.series;
            }
            if (a.variable != b.variable) {
                return a.variable < b.variable;
            }
            return a.observation_id < b.observation_id;
        });
        check_port_type(port_type, time, origin);
    }

    json batch(std::uint64_t /*tick*/, double window_start, double window_end) override {
        json observations = json::array();
        const auto first =
            std::lower_bound(rows_.begin(), rows_.end(), window_start,
                             [](const Row& row, double start) { return row.time < start; });
        for (auto it = first; it != rows_.end() && it->time < window_end; ++it) {
            observations.push_back(it->data);
        }
        return make_batch(window_start, window_end, std::move(observations));
    }

    bool replay() const override { return true; }

    bool exhausted_after(double window_end) const override {
        return rows_.empty() || window_end > rows_.back().time;
    }

    const std::string& digest() const override { return digest_; }

    const std::vector<Diagnostic>& notes() const override { return notes_; }

    const std::optional<SourceCounts>& counts() const override { return counts_; }

private:
    // Counts the rows and those whose time is before the run's start, which no window ever holds
    // (batch() delivers from the first window's start on), and notes the latter as one warning
    // that gives their number and the first of them in the file.
    void count_rows(const std::string& input, const std::string& origin, double start) {
        SourceCounts counts;
        counts.rows_total = rows_.size();
        const Row* first = nullptr;
        for (const Row& row : rows_) {
            if (row.time < start) {
                ++counts.rows_before_start;
                if (first == nullptr) {
                    first = &row;
                }
            }
        }
        counts_ = counts;
        if (first == nullptr) {
            return;
        }
        Diagnostic note;
        note.severity = Severity::warning;
        note.code = ErrorCode::TARGET_DATA_INVALID;
        note.location.file = origin;
        note.location.line = first->line;
        note.message = "input '" + input + "': " + std::to_string(counts.rows_before_start) +
                       " of " + std::to_string(counts.rows_total) +
                       " rows have a time before run.time.start (" + json(start).dump() + " " +
                       time_unit_ + ") and are never delivered; the first is at CSV line " +
                       std::to_string(first->line) + " (time " + json(first->time).dump() + ")";
        notes_.push_back(std::move(note));
    }

    json make_batch(double start, double end, json observations) const {
        json message = json::object();
        message["window"] = {{"start", start}, {"end", end}};
        message["time_unit"] = time_unit_;
        message["observations"] = std::move(observations);
        return message;
    }

    // Every row, in one batch, must satisfy the input's message type: a problem inside a row is
    // the data's fault, any other one the binding's.
    void check_port_type(const MessageTypeDef& port_type, const TimeBase& time,
                         const std::string& origin) const {
        json all = json::array();
        for (const auto& row : rows_) {
            all.push_back(row.data);
        }
        const auto issues = port_type.compiled->validate(
            make_batch(time.start, time.start + time.period, std::move(all)));
        if (issues.empty()) {
            return;
        }
        const auto& issue = issues.front();
        const std::string type = port_type.id + "@" + to_string(port_type.version);
        const std::string prefix = "/observations/";
        if (issue.pointer.compare(0, prefix.size(), prefix) == 0) {
            // "/observations/<index>[/<field>...]"
            const std::size_t slash = issue.pointer.find('/', prefix.size());
            const std::size_t index = std::stoul(issue.pointer.substr(prefix.size()));
            const std::string field =
                slash == std::string::npos ? std::string() : issue.pointer.substr(slash) + ": ";
            const Row& row = rows_.at(index);
            SourceLocation location;
            location.file = origin;
            location.line = row.line;
            throw Error(ErrorCode::TARGET_DATA_INVALID,
                        "CSV line " + std::to_string(row.line) +
                            ": the observation does not satisfy message type " + type + ": " +
                            field + issue.message,
                        std::move(location));
        }
        throw Error(ErrorCode::INPUT_BINDING_INVALID,
                    "an observation-csv source delivers observation batches, which the input's "
                    "message type " +
                        type + " does not accept: " +
                        (issue.pointer.empty() ? std::string("(root)") : issue.pointer) + ": " +
                        issue.message);
    }

    std::vector<Row> rows_;
    std::string time_unit_, digest_;
    std::vector<Diagnostic> notes_;
    std::optional<SourceCounts> counts_;
};

} // namespace

std::unique_ptr<InputSource> make_observation_csv_source(const InputBinding& binding,
                                                         const MessageTypeDef& port_type,
                                                         const TimeBase& time,
                                                         std::uintmax_t max_bytes) {
    try {
        return std::make_unique<ObservationCsvSource>(binding, port_type, time, max_bytes);
    } catch (const Error& e) {
        throw Error(e.code(), "input '" + binding.input + "': " + e.what(), e.location(),
                    e.details_json());
    }
}

} // namespace praenora::detail
