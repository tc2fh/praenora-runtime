// praenora schema: the document schemas embedded in the runtime (list) and one schema's text
// (show).
#include "commands/commands.hpp"

#include "output.hpp"

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

constexpr std::string_view kUsage =
    "usage: praenora schema list\n"
    "       praenora schema show <kind> [--format-version V]\n"
    "\n"
    "list  the embedded JSON Schemas, as <kind>@<major>.<minor>\n"
    "show  the schema of documents of one kind, for format version V (major.minor or a whole\n"
    "      version; the highest one by default)\n";

std::string usage_text() { return std::string(kUsage) + "\n" + global_options_usage(); }

int list(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (!arguments.positionals().empty()) {
                throw UsageError("schema list takes no arguments");
            }
            const Runtime runtime(runtime_options(globals));
            const std::vector<std::string> schemas = runtime.list_schemas();
            std::string text;
            for (const std::string& schema : schemas) {
                text += schema + "\n";
            }
            return output.result(schemas, text);
        });
}

int show(const std::vector<std::string>& args) {
    static const std::vector<Flag> flags = {{"--format-version", true}};
    return run_command(
        args, usage_text(), flags,
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (arguments.positionals().size() != 1) {
                throw UsageError("schema show takes one document kind");
            }
            const Runtime runtime(runtime_options(globals));
            std::string text = runtime.schema(arguments.positionals().front(),
                                              arguments.value("--format-version"));
            const json schema = json::parse(text);
            if (text.empty() || text.back() != '\n') {
                text += "\n";
            }
            return output.result(schema, text);
        });
}

} // namespace

int command_schema(const std::vector<std::string>& args) {
    return run_subcommand(args, "schema", usage_text(), {{"list", &list}, {"show", &show}});
}

} // namespace praenora::cli
