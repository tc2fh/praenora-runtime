// Writes to a module host that stops reading its channel, white-box: no write of the runtime waits
// for good. A call whose document is larger than the channel's buffers ends at its deadline and
// grace (MODULE_UNRESPONSIVE, its module host terminated), and promptly when a stop comes while it
// is written; ending a module host while the runtime writes it an answer of the shared compiled
// models ends within the wait it was given; and a run stopped while the runtime writes a call its
// module host does not take ends stopped, its teardown not calling the module host it ended. The
// fake module host (praenora_fake_module_host) loads the module as a module host does, then reads
// nothing more ("deaf-after-load"), or first asks the store for an entry larger than the channel's
// buffers ("deaf-store-after-load"), or first serves create and initialize ("deaf-at-invoke").
#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "isolation/child_settings.hpp"
#include "isolation/compiled_model_store.hpp"
#include "loader/loaded_module.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "instance_fixtures.hpp"
#include "isolation_support.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::detail::CallOutcome;
using praenora::detail::ChildSettings;
using praenora::detail::LoadedModule;
using praenora::detail::ModuleInstance;
using praenora::test::CapturedLogs;
using praenora::test::ScratchDir;
using Clock = std::chrono::steady_clock;

#ifndef _WIN32

namespace {

// For a scope, the runtime starts the fake module host in `mode` instead of its own.
class FakeModuleHost {
public:
    explicit FakeModuleHost(const char* mode)
        : link_(make_link()), hook_(directory_.path()),
          mode_("PRAENORA_TEST_FAKE_MODULE_HOST", std::string(mode)) {}

private:
    bool make_link() {
        fs::create_symlink(praenora::test::utf8_path(PRAENORA_TEST_FAKE_MODULE_HOST),
                           directory_.path() / "praenora-module-host");
        return true;
    }

    ScratchDir directory_;
    bool link_;
    praenora::test::ModuleHostDirectory hook_;
    praenora::test::ScopedEnvironment mode_;
};

bool has_event(const CapturedLogs& logs, const std::string& event) {
    for (const json& record : logs.events()) {
        if (record.at("attributes").value("event", std::string()) == event) {
            return true;
        }
    }
    return false;
}

// The padding, in bytes, of a call's document that this host's channel cannot take whole (1 MiB on
// a stock Linux or macOS host, where the buffers are some hundreds of KiB), or 0 when no document
// that a call may carry is that large: a call's frame is limited to 4 MiB, and a host tuned for
// throughput (Rivanna's net.core.wmem_default is 16 MiB) takes such a document in its buffers, so
// the write never blocks. The tests that need a write that stalls skip then.
std::size_t stalling_pad() {
    const std::size_t pad = praenora::test::beyond_the_channel_buffers(std::size_t{1} << 20);
    return pad <= (std::size_t{3} << 20) ? pad : 0;
}

std::string no_stalling_write() {
    return "the channel's buffers (" + std::to_string(praenora::test::channel_buffer_bytes()) +
           " bytes) take any document a call may carry, so a write to a module host that "
           "does not read never blocks here";
}

// fx-echo's create document with a parameter of stalling_pad() bytes: more than the channel's
// buffers take.
std::string large_create_document() {
    return praenora::test::create_document("fx-echo",
                                           {{"pad", std::string(stalling_pad(), 'p')}});
}

} // namespace

// The module host takes no byte of the call: its deadline passes (the cancellation cannot reach
// the module, whose call never arrived), and at the end of the grace the module host is killed and
// the call ends unresponsive, as one that never returns does.
TEST(Isolation, ACallTheModuleHostDoesNotTakeEndsAtItsDeadlineAndGrace) {
    const FakeModuleHost fake("deaf-after-load");
    CapturedLogs logs;
    const auto module = LoadedModule::load_in_child(
        praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-echo")),
        praenora::test::child_settings(logs));
    const std::int64_t pid = praenora::test::module_host_pid(*module);
    praenora::detail::InstanceOptions options = praenora::test::instance_options(logs);
    options.lifecycle_timeout = std::chrono::milliseconds(300);
    options.cancel_grace = std::chrono::milliseconds(300);
    ModuleInstance instance(module, std::move(options));
    const auto started = Clock::now();
    const praenora::detail::CallResult created = instance.create(large_create_document());
    const auto took = Clock::now() - started;
    EXPECT_EQ(created.outcome, CallOutcome::unresponsive) << created.error_json;
    EXPECT_EQ(json::parse(created.details_json), json({{"terminated", true}}));
    EXPECT_GE(took, std::chrono::milliseconds(550));
    EXPECT_LT(took, std::chrono::seconds(5));
    EXPECT_EQ(instance.state(), praenora::StepState::unresponsive);
    EXPECT_TRUE(praenora::test::gone_within(pid, std::chrono::seconds(5))) << pid;
    EXPECT_TRUE(has_event(logs, "module_host.terminated"));
}

// A stop requested while the runtime still writes a call the module host does not take reaches it
// at once, long before the call's deadline: the module host, which has not seen the call, is
// killed, and the call ends cancelled (a run ends stopped).
TEST(Isolation, AStopReachesACallTheModuleHostDoesNotTake) {
    if (stalling_pad() == 0) {
        GTEST_SKIP() << no_stalling_write();
    }
    const FakeModuleHost fake("deaf-after-load");
    CapturedLogs logs;
    const auto module = LoadedModule::load_in_child(
        praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-echo")),
        praenora::test::child_settings(logs));
    const std::int64_t pid = praenora::test::module_host_pid(*module);
    praenora::detail::InstanceOptions options = praenora::test::instance_options(logs);
    options.lifecycle_timeout = std::chrono::seconds(20);
    options.cancel_grace = std::chrono::seconds(5);
    ModuleInstance instance(module, std::move(options));
    std::thread stopper([&instance] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        instance.request_cancel();
    });
    const auto started = Clock::now();
    const praenora::detail::CallResult created = instance.create(large_create_document());
    const auto took = Clock::now() - started;
    stopper.join();
    EXPECT_EQ(created.outcome, CallOutcome::cancelled) << created.error_json;
    EXPECT_EQ(created.status, PRAENORA_STATUS_CANCELLED);
    EXPECT_LT(took, std::chrono::seconds(3));
    EXPECT_TRUE(praenora::test::gone_within(pid, std::chrono::seconds(5))) << pid;
    EXPECT_TRUE(has_event(logs, "module_host.terminated"));
}

// Ending a module host while the runtime writes it an answer of the shared compiled models (8 MiB,
// more where the channel's buffers are larger, which it does not take) ends within the wait given:
// with the answer's own wait longer than it, the module host is killed when the wait passes; with
// it shorter, the answer's timeout already ends the module host, although it was being ended, and a
// warning says why (module_host.terminated, with the problem).
TEST(Isolation, EndingAModuleHostThatDoesNotTakeAStoreAnswerEnds) {
    struct Case {
        const char* name;
        std::chrono::milliseconds reply_wait, end_wait, bound;
        const char* terminated; // words of the module_host.terminated warning
    };
    const std::vector<Case> cases = {
        {"the end's wait passes first", std::chrono::seconds(30), std::chrono::seconds(1),
         std::chrono::seconds(4), "did not take the request to exit"},
        {"the answer's wait passes first", std::chrono::milliseconds(500), std::chrono::seconds(10),
         std::chrono::seconds(4),
         "broke the channel's protocol while it was being ended: the module host did not take "
         "the answer to its store request within 500 ms"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const FakeModuleHost fake("deaf-store-after-load");
        CapturedLogs logs;
        praenora::detail::ChildProvider provider;
        provider.manifest = std::make_shared<const praenora::detail::ProviderManifest>(
            praenora::detail::load_provider_manifest(
                praenora::test::fixture_provider_manifest("test-store")));
        provider.library_sha256 = praenora::detail::check_provider_library_file(*provider.manifest);
        provider.services = {praenora::detail::ServiceVersion{"test.echo", 0, 0}};
        praenora::detail::compiled_model_store().save(
            {provider.manifest->name, provider.library_sha256}, "deaf",
            std::string(praenora::test::beyond_the_channel_buffers(std::size_t{8} << 20), 'd'));
        ChildSettings settings = praenora::test::child_settings(logs);
        settings.providers = {provider};
        settings.share_compiled_models = true;
        settings.store_reply_wait = c.reply_wait;
        const auto module = LoadedModule::load_in_child(
            praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-service")),
            settings);
        const std::int64_t pid = praenora::test::module_host_pid(*module);
        // The runtime is writing the answer once it has logged the hit.
        const auto asked = Clock::now() + std::chrono::seconds(10);
        while (!has_event(logs, "compiled_models.hit") && Clock::now() < asked) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(has_event(logs, "compiled_models.hit"));
        const auto started = Clock::now();
        module->code().end(c.end_wait);
        const auto took = Clock::now() - started;
        EXPECT_LT(took, c.bound);
        EXPECT_TRUE(praenora::test::process_gone(pid)) << pid;
        bool warned = false;
        for (const json& record : logs.events()) {
            if (record.at("attributes").value("event", std::string()) == "module_host.terminated" &&
                record.at("severity_text") == "WARN" &&
                record.at("body").get<std::string>().find(c.terminated) != std::string::npos) {
                warned = true;
            }
        }
        EXPECT_TRUE(warned) << c.terminated;
    }
}

// A run stopped while the runtime writes an invocation (of 1 MiB) that the step's module host does
// not take ends stopped at once: the module host, which never began the call, is killed. Its
// instance went with it, so the teardown neither finalizes nor destroys it, and does not report
// that as a failure: a warning (teardown.skipped) says what was left undone, the step ends
// destroyed, and the run's health is not unhealthy.
TEST(Isolation, AStopDuringACallTheModuleHostDoesNotTakeEndsTheRunWithoutAFailedTeardown) {
    if (stalling_pad() == 0) {
        GTEST_SKIP() << no_stalling_write();
    }
    const FakeModuleHost fake("deaf-at-invoke");
    const ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    praenora::test::write_text_file(dir.path() / "source.json",
                                    json{{"pad", std::string(stalling_pad(), 'p')}}.dump());
    const fs::path config = dir.path() / "workflow.yaml";
    praenora::test::write_text_file(config,
                                    "kind: workflow-config\n"
                                    "format_version: \"0.1.0\"\n"
                                    "workflow: { name: test.single, version: \"0.1.0\" }\n"
                                    "run: { ticks: 1, isolation: { mode: process } }\n"
                                    "inputs:\n"
                                    "  source: { source: json-document, path: source.json }\n"
                                    "steps:\n"
                                    "  a:\n"
                                    "    module: { name: fx-echo, version: \"0.1.0\" }\n");
    praenora::RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    options.logging.level = praenora::LogLevel::debug;
    options.logging.format = praenora::LogFormat::json;
    options.logging.file = praenora::detail::path_to_utf8(log);
    const praenora::Runtime runtime(options);
    praenora::WorkflowOptions overrides;
    overrides.output_dir = praenora::detail::path_to_utf8(dir.path() / "out");
    const auto workflow = runtime.open(praenora::detail::path_to_utf8(config), overrides);
    std::thread stopper([&workflow] {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (workflow->state() != praenora::WorkflowState::running && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // The invocation is being written by now, and its module host takes none of it.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        workflow->request_stop();
    });
    const auto started = Clock::now();
    const praenora::RunSummary summary = workflow->run();
    const auto took = Clock::now() - started;
    stopper.join();
    EXPECT_EQ(summary.final_state, praenora::WorkflowState::stopped)
        << (summary.error ? summary.error->message : std::string());
    EXPECT_LT(took, std::chrono::seconds(5));
    const praenora::StatusSnapshot status = workflow->status();
    EXPECT_NE(status.health, praenora::Health::unhealthy);
    ASSERT_EQ(status.steps.size(), 1u);
    EXPECT_EQ(status.steps[0].state, praenora::StepState::destroyed);
    EXPECT_EQ(status.steps[0].cancelled, 1u);
    EXPECT_FALSE(status.steps[0].last_error.has_value()) << status.steps[0].last_error->message;

    std::vector<json> records;
    {
        std::ifstream in(log);
        std::string line;
        while (std::getline(in, line)) {
            records.push_back(json::parse(line));
        }
    }
    const auto with_event = [&records](const std::string& event) {
        std::vector<json> found;
        for (const json& record : records) {
            if (record.at("attributes").value("event", std::string()) == event) {
                found.push_back(record);
            }
        }
        return found;
    };
    EXPECT_TRUE(with_event("teardown.finalize_failed").empty());
    EXPECT_TRUE(with_event("teardown.destroy_failed").empty());
    const std::vector<json> skipped = with_event("teardown.skipped");
    ASSERT_EQ(skipped.size(), 1u);
    EXPECT_EQ(skipped[0].at("severity_text"), "WARN");
    EXPECT_EQ(skipped[0].at("attributes").at("step"), "a");
    EXPECT_EQ(skipped[0].at("attributes").at("state"), "ready");
    EXPECT_NE(skipped[0].at("body").get<std::string>().find(
                  "is neither finalized nor destroyed: its module host was ended"),
              std::string::npos)
        << skipped[0].at("body");
    const std::vector<json> terminated = with_event("module_host.terminated");
    ASSERT_EQ(terminated.size(), 1u);
    EXPECT_TRUE(praenora::test::gone_within(terminated[0].at("attributes").at("pid"),
                                            std::chrono::seconds(5)));
}

#endif // _WIN32
