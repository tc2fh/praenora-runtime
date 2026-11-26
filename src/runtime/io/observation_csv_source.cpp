// The observation-csv source: replays a long-form observation CSV one tick window at a time.
#include "io/input_source.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "io/csv.hpp"
#include "io/target_descriptor.hpp"

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

// How a table's columns are read. `columns` maps an observation column to the header it is read
// from; a column that is not in it reads the header of its own name, unless `exhaustive` (a
// target descriptor lists the columns it wants: the others are not read). A cell equal to
// `missing_token` is as an empty cell.
struct ColumnLayout {
    std::map<std::string, std::string> columns;
    bool exhaustive = false;
    std::optional<std::string> missing_token;
};

// Reads the rows of one table: the column layout, then each row by the observation rules.
// The columns whose cells the missing token can stand in for: the value, its limits and the other
// optional measurement columns. Identifiers and the time are never absent (ADR-0042).
bool is_value_column(const std::string& column) {
    return column == "value" || column == "unit" || column == "missing" || column == "censoring" ||
           column == "lower_limit" || column == "upper_limit" || column == "weight";
}

class RowReader {
public:
    RowReader(const CsvTable& table, const ColumnLayout& layout, std::string origin)
        : table_(table), layout_(layout), origin_(std::move(origin)) {
        map_columns();
    }

    // The optional observation columns that the table has under their own names and that the
    // layout does not read (exhaustive layouts only), in column order.
    const std::vector<std::string>& ignored_columns() const { return ignored_; }

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
        for (const auto& entry : layout_.columns) {
            if (std::find(known.begin(), known.end(), entry.first) == known.end()) {
                throw Error(ErrorCode::INPUT_BINDING_INVALID,
                            "'" + entry.first + "' is not an observation column");
            }
        }
        std::map<std::string, std::string> reader_of; // header -> the column reading it
        for (const auto& column : known) {
            const auto remapped = layout_.columns.find(column);
            if (layout_.exhaustive && remapped == layout_.columns.end()) {
                if (by_header.count(column) != 0) {
                    ignored_.push_back(column);
                }
                continue;
            }
            const std::string& header =
                remapped == layout_.columns.end() ? column : remapped->second;
            const auto found = by_header.find(header);
            if (found == by_header.end()) {
                if (remapped != layout_.columns.end()) {
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
        if (it == index_.end()) {
            return kEmpty;
        }
        const std::string& text = fields[it->second];
        return layout_.missing_token && text == *layout_.missing_token && is_value_column(column)
                   ? kEmpty
                   : text;
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
    const ColumnLayout& layout_;
    const std::string origin_;
    std::map<std::string, std::size_t> index_; // observation column -> field index
    std::vector<std::string> ignored_;
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
        std::optional<TargetDescriptor> descriptor;
        std::filesystem::path path = binding.path;
        if (!binding.descriptor.empty()) {
            descriptor = load_target_descriptor(binding.descriptor);
            path = data_file(*descriptor, binding.path);
            check_time_unit(*descriptor, time);
        }
        const std::string origin = path_to_utf8(path);
        // The size first, so a file beyond the limit is never read. A size that cannot be taken
        // is left to the read, which reports why.
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (!ec) {
            check_csv_size(size, max_bytes, origin);
        }
        std::string bytes;
        try {
            bytes = read_file_bytes(path);
        } catch (const Error& e) {
            SourceLocation location;
            location.file = origin;
            throw Error(ErrorCode::TARGET_DATA_UNAVAILABLE,
                        std::string("cannot read the observation CSV: ") + e.what(),
                        std::move(location));
        }
        check_csv_size(bytes.size(), max_bytes, origin); // the file may have grown meanwhile
        digest_ = sha256_hex(bytes);
        ColumnLayout layout;
        CsvDialect dialect;
        if (descriptor) {
            check_sha256(*descriptor, digest_);
            layout.columns = descriptor->columns;
            layout.exhaustive = true;
            layout.missing_token = descriptor->missing_token;
            dialect = descriptor->dialect;
            descriptor_ = SourceDescriptor{descriptor->id, descriptor->digest,
                                           descriptor->schema_version, path};
        } else {
            layout.columns = binding.columns;
        }
        const CsvTable table = parse_csv(bytes, origin, dialect);
        const RowReader reader(table, layout, origin);
        rows_ = reader.rows();
        const std::size_t rows_total = rows_.size();
        std::optional<std::uint64_t> not_delivered;
        if (descriptor) {
            note_ignored_columns(binding.input, origin, reader.ignored_columns());
            note_missing_time_unit(binding.input, *descriptor);
            not_delivered = apply_descriptor(binding.input, origin, *descriptor);
        }
        count_rows(binding.input, origin, time.start, rows_total, not_delivered); // file order
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

    const std::optional<SourceDescriptor>& descriptor() const override { return descriptor_; }

private:
    [[noreturn]] static void descriptor_error(const TargetDescriptor& descriptor, ErrorCode code,
                                              const std::string& pointer,
                                              const std::string& message) {
        throw Error(code, "the target descriptor '" + descriptor.document.origin + "': " + message,
                    locate(descriptor.document, pointer));
    }

    // The CSV that the descriptor and the configuration name. A remote href cannot be read, so
    // the configuration must give the path then, and it wins; otherwise a path given beside a
    // descriptor must be the file the href names.
    static std::filesystem::path data_file(const TargetDescriptor& descriptor,
                                           const std::filesystem::path& configured) {
        if (descriptor.href_remote) {
            if (configured.empty()) {
                descriptor_error(descriptor, ErrorCode::INPUT_BINDING_INVALID, "/data/href",
                                 "data.href '" + descriptor.href +
                                     "' is remote and cannot be read; give the file in the "
                                     "configuration's path");
            }
            return configured;
        }
        if (configured.empty()) {
            return descriptor.data_path;
        }
        std::error_code ec;
        const bool same =
            configured.lexically_normal() == descriptor.data_path.lexically_normal() ||
            std::filesystem::equivalent(configured, descriptor.data_path, ec);
        if (!same) {
            descriptor_error(descriptor, ErrorCode::INPUT_BINDING_INVALID, "/data/href",
                             "data.href '" + descriptor.href + "' names '" +
                                 path_to_utf8(descriptor.data_path) +
                                 "', but the configuration's path is '" +
                                 path_to_utf8(configured) + "'; name one file");
        }
        return configured;
    }

    static void check_time_unit(const TargetDescriptor& descriptor, const TimeBase& time) {
        if (descriptor.time_unit && *descriptor.time_unit != time.unit) {
            descriptor_error(descriptor, ErrorCode::INPUT_BINDING_INVALID, "/time_unit",
                             "time_unit '" + *descriptor.time_unit +
                                 "' differs from the run's time unit '" + time.unit + "'");
        }
    }

    static void check_sha256(const TargetDescriptor& descriptor, const std::string& digest) {
        if (descriptor.sha256 && *descriptor.sha256 != digest) {
            descriptor_error(descriptor, ErrorCode::TARGET_DATA_INVALID, "/data/sha256",
                             "data.sha256 " + *descriptor.sha256 +
                                 " is not the digest of the CSV, " + digest);
        }
    }

    void warn(const std::string& file, std::int64_t line, std::string message,
              std::string pointer = {}) {
        Diagnostic note;
        note.severity = Severity::warning;
        note.code = ErrorCode::TARGET_DATA_INVALID;
        note.location.file = file;
        note.location.line = line;
        note.location.pointer = std::move(pointer);
        note.message = std::move(message);
        notes_.push_back(std::move(note));
    }

    // The columns the descriptor does not list but the CSV has under the name of an optional
    // observation column: not read, so the data is what the descriptor declares.
    void note_ignored_columns(const std::string& input, const std::string& origin,
                              const std::vector<std::string>& ignored) {
        if (ignored.empty()) {
            return;
        }
        std::string list;
        for (const std::string& column : ignored) {
            list += (list.empty() ? "'" : ", '") + column + "'";
        }
        warn(origin, 1,
             "input '" + input + "': the CSV has the column" + (ignored.size() > 1 ? "s " : " ") +
                 list + ", which the descriptor does not list in columns; " +
                 (ignored.size() > 1 ? "they are" : "it is") + " ignored");
    }

    void note_missing_time_unit(const std::string& input, const TargetDescriptor& descriptor) {
        if (!descriptor.time_unit) {
            warn(descriptor.document.origin, 0,
                 "input '" + input + "': the descriptor has no time_unit; the run's unit '" +
                     time_unit_ + "' is used",
                 "/time_unit");
        }
    }

    [[noreturn]] static void row_error(const std::string& origin, std::int64_t line,
                                       const std::string& message) {
        SourceLocation location;
        location.file = origin;
        location.line = line;
        throw Error(ErrorCode::TARGET_DATA_INVALID,
                    "CSV line " + std::to_string(line) + ": " + message, std::move(location));
    }

    // What a descriptor says about the rows: every row names a variable of the descriptor, in its
    // unit when the row has one; values outside a variable's bounds are noted (one warning, with
    // the count and the first); duplicate_policy is enforced; and only rows of variables whose
    // role is response (or none) stay: the others are counted, noted, and never reach the model.
    // Returns the number of rows removed by role.
    std::uint64_t apply_descriptor(const std::string& input, const std::string& origin,
                                   const TargetDescriptor& descriptor) {
        std::map<std::string, const TargetVariable*> variables;
        for (const TargetVariable& variable : descriptor.variables) {
            variables.emplace(variable.id, &variable);
        }
        std::uint64_t out_of_bounds = 0, by_role = 0;
        const Row *first_out = nullptr, *first_role = nullptr;
        std::string out_detail, role_detail;
        std::vector<char> delivered; // built when the first row is removed by role
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const Row& row = rows_[i];
            const auto found = variables.find(row.variable);
            if (found == variables.end()) {
                row_error(origin, row.line,
                          "variable_id '" + row.variable +
                              "' is not one of the descriptor's variables");
            }
            const TargetVariable& variable = *found->second;
            if (const auto unit = row.data.find("unit");
                unit != row.data.end() && unit->get_ref<const std::string&>() != variable.unit) {
                row_error(origin, row.line,
                          "unit '" + unit->get<std::string>() + "' differs from the unit '" +
                              variable.unit + "' of the variable '" + variable.id + "'");
            }
            const json& value = row.data.at("value");
            if (value.is_number() &&
                ((variable.lower_bound && value.get<double>() < *variable.lower_bound) ||
                 (variable.upper_bound && value.get<double>() > *variable.upper_bound))) {
                if (out_of_bounds++ == 0) {
                    first_out = &row;
                    out_detail = "variable '" + variable.id + "', value " + value.dump();
                }
            }
            if (variable.role != "response") {
                if (delivered.empty()) {
                    delivered.assign(rows_.size(), 1);
                }
                delivered[i] = 0;
                if (by_role++ == 0) {
                    first_role = &row;
                    role_detail = "variable '" + variable.id + "', role " + variable.role;
                }
            }
        }
        if (descriptor.duplicate_policy) {
            check_duplicates(origin, *descriptor.duplicate_policy,
                             descriptor.columns.count("observation_id") != 0);
        }
        if (first_out != nullptr) {
            warn(origin, first_out->line,
                 "input '" + input + "': " + std::to_string(out_of_bounds) + " of " +
                     std::to_string(rows_.size()) +
                     " rows have a value outside the bounds of their variable; they are kept; the "
                     "first is at CSV line " +
                     std::to_string(first_out->line) + " (" + out_detail + ")");
        }
        if (first_role != nullptr) {
            warn(origin, first_role->line,
                 "input '" + input + "': " + std::to_string(by_role) + " of " +
                     std::to_string(rows_.size()) +
                     " rows belong to variables whose role is not response and are never "
                     "delivered; the first is at CSV line " +
                     std::to_string(first_role->line) + " (" + role_detail + ")");
        }
        if (by_role == 0) {
            return 0; // nothing to remove: the rows stay where they are
        }
        std::vector<Row> kept;
        kept.reserve(rows_.size() - by_role);
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (is_delivered(delivered, i)) {
                kept.push_back(std::move(rows_[i]));
            }
        }
        rows_ = std::move(kept);
        return by_role;
    }

    // Whether row `i` stays: `delivered` is built only when a row is removed (empty: none was).
    static bool is_delivered(const std::vector<char>& delivered, std::size_t i) {
        return delivered.empty() || delivered[i] != 0;
    }

    // duplicate_policy: two rows of one series and variable at one time are an error ("error"),
    // or are allowed when each carries its own observation_id ("require_observation_id"). The
    // first offence in the file is reported.
    void check_duplicates(const std::string& origin, const std::string& policy,
                          bool observation_id_mapped) const {
        std::vector<std::size_t> order(rows_.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const Row &x = rows_[a], &y = rows_[b];
            if (x.series != y.series) {
                return x.series < y.series;
            }
            if (x.variable != y.variable) {
                return x.variable < y.variable;
            }
            if (x.time != y.time) {
                return x.time < y.time;
            }
            return x.observation_id < y.observation_id;
        });
        const Row* offender = nullptr;
        for (std::size_t i = 1; i < order.size(); ++i) {
            const Row &previous = rows_[order[i - 1]], &row = rows_[order[i]];
            if (previous.series != row.series || previous.variable != row.variable ||
                previous.time != row.time) {
                continue;
            }
            const bool distinct = policy == "require_observation_id" &&
                                  !previous.observation_id.empty() &&
                                  previous.observation_id != row.observation_id;
            const Row& later = previous.line > row.line ? previous : row;
            if (!distinct && (offender == nullptr || later.line < offender->line)) {
                offender = &later;
            }
        }
        if (offender != nullptr) {
            row_error(origin, offender->line,
                      "series '" + offender->series + "' has a second row for variable '" +
                          offender->variable + "' at time " + json(offender->time).dump() +
                          (policy == "require_observation_id"
                               ? std::string(" without a distinct observation_id (duplicate_policy "
                                 "require_observation_id)") +
                                 (observation_id_mapped ? ""
                                                         : "; the descriptor does not map the observation_id column, so no row has one")
                               : " (duplicate_policy error)"));
        }
    }

    // Counts the rows and those whose time is before the run's start, which no window ever holds
    // (batch() delivers from the first window's start on), and notes the latter as one warning
    // that gives their number and the first of them in the file. `rows_total` counts the rows of
    // the file, among them the `not_delivered` ones that a descriptor's roles removed from rows_.
    void count_rows(const std::string& input, const std::string& origin, double start,
                    std::size_t rows_total, std::optional<std::uint64_t> not_delivered) {
        SourceCounts counts;
        counts.rows_total = rows_total;
        counts.rows_not_delivered_by_role = not_delivered;
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
    std::optional<SourceDescriptor> descriptor_;
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
