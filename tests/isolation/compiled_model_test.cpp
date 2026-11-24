// Shared compiled models, white-box: the module hosts of steps that share them hand entries to
// each other through the store in this process (compiled_model_store()), each under its provider's
// namespace, which the runtime takes from its own load request; a module host of a step that does
// not share, and this process, give their providers no store at all; the runtime answers a module
// host's store requests during the load and between calls too; an entry or a key over its limit is
// refused in the module host, which sends only a notice; and a module host that breaks the store's
// rules (asks while its step does not share, names no provider of its load, sends a key or an entry
// over the limits, does not take the answer to its request) breaks the channel's protocol; and a
// stored model that does not load is compiled instead. With the fixture provider test-store
// (tests/fixtures/providers/test-store), which fx-service reaches through test.echo. The store is
// the process's, so each test uses keys no earlier test (or repetition) of this process used.
#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "isolation/child_process.hpp"
#include "isolation/child_settings.hpp"
#include "isolation/compiled_model_store.hpp"
#include "loader/loaded_module.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "instance_fixtures.hpp"
#include "isolation_support.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "sandbox_support.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_echo.h"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::CallOutcome;
using praenora::detail::ChildProvider;
using praenora::detail::ChildSettings;
using praenora::detail::CompiledModelNamespace;
using praenora::detail::LoadedModule;
using praenora::detail::ModuleInstance;
using praenora::detail::ServiceVersion;
using praenora::test::CapturedLogs;
using praenora::test::ScratchDir;

#ifndef _WIN32

namespace {

// A key that no other test of this process used.
std::string fresh_key(const std::string& name) {
    static std::atomic<unsigned> next{0};
    return name + "-" + std::to_string(::getpid()) + "-" + std::to_string(next.fetch_add(1));
}

// test-store, serving test.echo@0.0, its records going to provider.test-store of `logs`.
ChildProvider test_store(CapturedLogs& logs) {
    ChildProvider provider;
    provider.manifest = std::make_shared<const praenora::detail::ProviderManifest>(
        praenora::detail::load_provider_manifest(
            praenora::test::fixture_provider_manifest("test-store")));
    provider.library_sha256 = praenora::detail::check_provider_library_file(*provider.manifest);
    provider.services = {ServiceVersion{"test.echo", 0, 0}};
    provider.logger = logs.hub().logger("provider.test-store");
    return provider;
}

// test-store's namespace in the store: its name and its library's SHA-256.
CompiledModelNamespace store_namespace(CapturedLogs& logs) {
    const ChildProvider provider = test_store(logs);
    return {provider.manifest->name, provider.library_sha256};
}

// fx-service in a module host of its own, with test-store, sharing compiled models or not, with
// the sandbox profile `sandbox`; its instance created and initialized. Its events and records go
// to `logs`.
class StoreHost {
public:
    StoreHost(CapturedLogs& logs, bool share,
              praenora::detail::SandboxProfile sandbox = praenora::detail::SandboxProfile::none) {
        ChildSettings settings = praenora::test::child_settings(logs);
        settings.providers = {test_store(logs)};
        settings.share_compiled_models = share;
        settings.sandbox = sandbox;
        module_ = LoadedModule::load_in_child(
            praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-service")),
            settings);
        instance_ =
            std::make_unique<ModuleInstance>(module_, praenora::test::instance_options(logs));
        EXPECT_EQ(instance_->create(praenora::test::create_document("fx-service")).outcome,
                  CallOutcome::succeeded);
        EXPECT_EQ(instance_->initialize(praenora::test::initialize_document()).outcome,
                  CallOutcome::succeeded);
    }

    // What test-store answers `command` with, through an invoke.
    json echo(const json& command) {
        const praenora::detail::CallResult invoked =
            instance_->invoke(praenora::test::invocation_document(tick_, &command), tick_);
        ++tick_;
        EXPECT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
        if (invoked.outcome != CallOutcome::succeeded) {
            return nullptr;
        }
        return json::parse(invoked.output).at("outputs").at("out").at("data").at("echo");
    }

    ModuleInstance& instance() { return *instance_; }
    std::int64_t pid() const { return praenora::test::module_host_pid(*module_); }

private:
    std::shared_ptr<LoadedModule> module_;
    std::unique_ptr<ModuleInstance> instance_;
    std::uint64_t tick_ = 0;
};

// The events of `logs` named `event` (an attribute).
std::vector<json> events_named(const CapturedLogs& logs, const std::string& event) {
    std::vector<json> found;
    for (const json& record : logs.events()) {
        if (record.at("attributes").value("event", std::string()) == event) {
            found.push_back(record);
        }
    }
    return found;
}

std::size_t store_events(const CapturedLogs& logs) {
    std::size_t count = 0;
    for (const json& record : logs.events()) {
        if (record.at("attributes").value("event", std::string()).rfind("compiled_models.", 0) ==
            0) {
            ++count;
        }
    }
    return count;
}

// Waits up to 10 s until `logs` hold a record whose body is `body`.
bool recorded_within(const CapturedLogs& logs, const std::string& body) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        for (const json& record : logs.events()) {
            if (record.at("body") == body) {
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

// One module host saves an entry, another of a sharing step finds it: the store keeps it under
// test-store's namespace (its name and library SHA-256), and the runtime logs each request at
// debug on the requesting step's logger (compiled_models.saved, .hit, .miss), with the provider,
// the key and the entry's size.
TEST(Isolation, ModuleHostsOfSharingStepsShareCompiledModels) {
    const std::string key = fresh_key("shared");
    const std::string missing = fresh_key("missing");
    CapturedLogs first_logs, second_logs;
    StoreHost first(first_logs, true);
    EXPECT_EQ(first.echo({{"save", {{"key", key}, {"data", "a compiled model"}}}}),
              json({{"saved", key}}));
    StoreHost second(second_logs, true);
    EXPECT_EQ(second.echo({{"load", key}}), json({{"found", true}, {"data", "a compiled model"}}));
    EXPECT_EQ(second.echo({{"load", missing}}), json({{"found", false}, {"data", nullptr}}));

    const auto entry =
        praenora::detail::compiled_model_store().find(store_namespace(first_logs), key);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(*entry, "a compiled model");
    // Another provider's namespace has nothing under the key.
    EXPECT_EQ(
        praenora::detail::compiled_model_store().find(
            CompiledModelNamespace{"test-echo", store_namespace(first_logs).library_sha256}, key),
        nullptr);

    const std::vector<json> saved = events_named(first_logs, "compiled_models.saved");
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved[0].at("severity_text"), "DEBUG");
    EXPECT_EQ(saved[0].at("attributes").at("logger"), "module.s");
    EXPECT_EQ(saved[0].at("attributes").at("provider"), "test-store");
    EXPECT_EQ(saved[0].at("attributes").at("key"), key);
    EXPECT_EQ(saved[0].at("attributes").at("size"), 16);
    const std::vector<json> hits = events_named(second_logs, "compiled_models.hit");
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].at("severity_text"), "DEBUG");
    EXPECT_EQ(hits[0].at("attributes").at("key"), key);
    EXPECT_EQ(hits[0].at("attributes").at("size"), 16);
    const std::vector<json> misses = events_named(second_logs, "compiled_models.miss");
    ASSERT_EQ(misses.size(), 1u);
    EXPECT_EQ(misses[0].at("attributes").at("key"), missing);
    EXPECT_EQ(store_events(first_logs), 1u);
    EXPECT_EQ(store_events(second_logs), 2u);
}

// Module hosts under the restricted sandbox profile share compiled models as others do: each
// loads and starts test-store after the profile is applied, and their store requests and answers
// travel on their channels, which the profile leaves alone: one saves an entry, the other finds it.
TEST(Isolation, RestrictedModuleHostsShareCompiledModels) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    const std::string key = fresh_key("restricted");
    CapturedLogs first_logs, second_logs;
    StoreHost first(first_logs, true, praenora::detail::SandboxProfile::restricted);
    EXPECT_EQ(first.echo({{"save", {{"key", key}, {"data", "a compiled model"}}}}),
              json({{"saved", key}}));
    StoreHost second(second_logs, true, praenora::detail::SandboxProfile::restricted);
    EXPECT_EQ(second.echo({{"load", key}}), json({{"found", true}, {"data", "a compiled model"}}));
    EXPECT_EQ(events_named(first_logs, "compiled_models.saved").size(), 1u);
    EXPECT_EQ(events_named(second_logs, "compiled_models.hit").size(), 1u);
}

// The store's callbacks are NULL where the process offers no store: in a module host whose step
// does not share compiled models, and always in the runtime's own process; there test-store can
// neither save nor load, and the store never hears of it.
TEST(Isolation, AModuleHostOfAStepThatDoesNotShareGetsNoStore) {
    const std::string key = fresh_key("unshared");
    {
        CapturedLogs logs;
        StoreHost sharing(logs, true);
        EXPECT_EQ(sharing.echo({{"probe", true}}),
                  json({{"load_compiled", true}, {"save_compiled", true}}));
    }
    CapturedLogs logs;
    StoreHost unshared(logs, false);
    EXPECT_EQ(unshared.echo({{"probe", true}}),
              json({{"load_compiled", false}, {"save_compiled", false}}));
    EXPECT_EQ(unshared.echo({{"save", {{"key", key}, {"data", "x"}}}}),
              json({{"error", "no store"}}));
    EXPECT_EQ(praenora::detail::compiled_model_store().find(store_namespace(logs), key), nullptr);
    EXPECT_EQ(store_events(logs), 0u);

    // The runtime's own process: test-store loaded and started here.
    const ChildProvider here = test_store(logs);
    const praenora::detail::LoadedProvider& provider = praenora::detail::load_provider(
        *here.manifest, here.services, here.library_sha256, nullptr);
    const auto* echo = static_cast<const praenora_test_echo_v0*>(provider.service("test.echo", 0));
    ASSERT_NE(echo, nullptr);
    const std::string probe = R"({"probe": true})";
    std::string answer;
    praenora_sink sink{&answer, [](void* context, const char* data, std::size_t size) {
                           static_cast<std::string*>(context)->append(data, size);
                           return PRAENORA_STATUS_OK;
                       }};
    ASSERT_EQ(echo->echo(echo->context, nullptr, praenora_bytes{probe.data(), probe.size()}, &sink),
              PRAENORA_STATUS_OK);
    EXPECT_EQ(json::parse(answer), json({{"load_compiled", false}, {"save_compiled", false}}));
}

// A provider host as short as a runtime's before the store's callbacks were appended (struct_size
// ends before load_compiled) offers no store, whatever the fields beyond it hold: test-store,
// started with such a host, finds neither callback, and calls neither. It is started in a process
// of its own (praenora_isolation_short_host_probe), since a provider is started once in a process,
// and earlier tests of this one (and each repetition) start test-store through the provider loader.
TEST(Isolation, AProviderHostTooShortForTheStoreOffersNone) {
    const ChildProvider provider = [] {
        CapturedLogs logs;
        return test_store(logs);
    }();
    auto probe = praenora::detail::ChildProcess::start(
        PRAENORA_TEST_SHORT_HOST_PROBE,
        {praenora::detail::path_to_utf8(provider.manifest->library_path)});
    const std::string text =
        praenora::test::read_within(probe.channel, std::chrono::seconds(30), false);
    ::close(probe.channel);
    const auto exit = probe.process->wait(std::chrono::seconds(30));
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exit_code, 0) << text;
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 4u) << text;
    EXPECT_EQ(json::parse(lines[0]), json({{"load_compiled", false}, {"save_compiled", false}}));
    EXPECT_EQ(json::parse(lines[1]), json({{"error", "no store"}}));
    EXPECT_EQ(json::parse(lines[2]), json({{"error", "no store"}}));
    EXPECT_EQ(lines[3], "store not called");
}

// The runtime's reader answers the store at any time: during the load (test-store saves and loads
// in its start(), as PRAENORA_TEST_STORE_AT_START asks), and between calls (from test-store's own
// thread, after the invoke that started it returned, once the test makes the file it waits for).
TEST(Isolation, TheRuntimeAnswersTheStoreDuringTheLoadAndBetweenCalls) {
    const ScratchDir directory;
    const fs::path trigger = directory.path() / "now";
    const std::string at_start = fresh_key("at-start");
    const std::string between = fresh_key("between");
    const praenora::test::ScopedEnvironment start_key("PRAENORA_TEST_STORE_AT_START", at_start);
    const praenora::test::ScopedEnvironment later_file("PRAENORA_TEST_STORE_LATER",
                                                       praenora::detail::path_to_utf8(trigger));
    CapturedLogs logs;
    StoreHost host(logs, true);
    EXPECT_TRUE(recorded_within(logs, "test-store: at start: found at start"));
    EXPECT_EQ(host.echo({{"later", {{"key", between}, {"data", "later"}}}}),
              json({{"later", true}}));
    EXPECT_EQ(events_named(logs, "compiled_models.saved").size(), 1u); // at start only, so far
    praenora::test::write_text_file(trigger, "");
    EXPECT_TRUE(recorded_within(logs, "test-store: between calls: found later"));
    EXPECT_EQ(events_named(logs, "compiled_models.saved").size(), 2u);
    EXPECT_EQ(events_named(logs, "compiled_models.hit").size(), 2u);
    // The module host serves its instance as before.
    EXPECT_EQ(host.instance().finalize().outcome, CallOutcome::succeeded);
}

// An entry over 128 MiB, or a key over 256 bytes, is refused in the module host: no data is sent,
// only a notice, and the runtime logs compiled_models.refused on the step's logger with what was
// over its limit; the call goes on. A load with a key over the limit finds nothing without asking.
// An entry with a key of exactly 256 bytes is kept.
TEST(Isolation, AnEntryOrAKeyOverItsLimitIsRefusedInTheModuleHost) {
    CapturedLogs logs;
    StoreHost host(logs, true);
    const std::string large = fresh_key("large");
    const std::size_t over = praenora::detail::kMaxCompiledModelBytes + 1;
    EXPECT_EQ(host.echo({{"save", {{"key", large}, {"size", over}}}}), json({{"saved", large}}));
    std::string long_key = fresh_key("long");
    long_key.resize(praenora::detail::kMaxCompiledModelKeyBytes + 1, 'k');
    EXPECT_EQ(host.echo({{"save", {{"key", long_key}, {"data", "x"}}}}),
              json({{"saved", long_key}}));
    std::string longest_key = fresh_key("longest");
    longest_key.resize(praenora::detail::kMaxCompiledModelKeyBytes, 'k');
    EXPECT_EQ(host.echo({{"save", {{"key", longest_key}, {"data", "kept"}}}}),
              json({{"saved", longest_key}}));

    const std::vector<json> refused = events_named(logs, "compiled_models.refused");
    ASSERT_EQ(refused.size(), 2u);
    EXPECT_EQ(refused[0].at("severity_text"), "DEBUG");
    EXPECT_EQ(refused[0].at("attributes").at("key"), large);
    EXPECT_EQ(refused[0].at("attributes").at("size"), over);
    EXPECT_NE(refused[0].at("body").get<std::string>().find("134217728"), std::string::npos)
        << refused[0].at("body");
    EXPECT_EQ(refused[1].at("attributes").at("key"),
              long_key.substr(0, praenora::detail::kMaxCompiledModelKeyBytes));
    EXPECT_EQ(refused[1].at("attributes").at("key_size"), long_key.size());
    EXPECT_NE(refused[1].at("body").get<std::string>().find("256"), std::string::npos)
        << refused[1].at("body");

    const CompiledModelNamespace space = store_namespace(logs);
    EXPECT_EQ(praenora::detail::compiled_model_store().find(space, large), nullptr);
    EXPECT_EQ(host.echo({{"load", large}}), json({{"found", false}, {"data", nullptr}}));
    const std::size_t misses = events_named(logs, "compiled_models.miss").size();
    EXPECT_EQ(host.echo({{"load", long_key}}), json({{"found", false}, {"data", nullptr}}));
    EXPECT_EQ(events_named(logs, "compiled_models.miss").size(), misses); // nothing was asked
    EXPECT_EQ(host.echo({{"load", longest_key}}), json({{"found", true}, {"data", "kept"}}));
    EXPECT_EQ(host.instance().finalize().outcome, CallOutcome::succeeded);
}

namespace {

// A provider of the simulation service whose manifest is `manifest`, with validation's digest.
ChildProvider simulation_provider(const fs::path& manifest) {
    ChildProvider provider;
    provider.manifest = std::make_shared<const praenora::detail::ProviderManifest>(
        praenora::detail::load_provider_manifest(manifest));
    provider.library_sha256 = praenora::detail::check_provider_library_file(*provider.manifest);
    provider.services = {ServiceVersion{"praenora.simulation", 0, 0}};
    return provider;
}

// What simulating-module with `provider` and `parameters` gave in a module host that shares
// compiled models: its first invoke's output, the messages its instance logged (records without
// an event) and the store's saves.
struct SimulationRun {
    std::string output;
    std::vector<std::string> messages;
    std::vector<json> saved; // compiled_models.saved
};

SimulationRun simulate_sharing(const ChildProvider& provider, const json& parameters) {
    CapturedLogs logs;
    ChildSettings settings = praenora::test::child_settings(logs);
    settings.providers = {provider};
    settings.share_compiled_models = true;
    const auto module = LoadedModule::load_in_child(
        praenora::detail::load_manifest(praenora::test::fixture_manifest("simulating-module")),
        settings);
    ModuleInstance instance(module, praenora::test::instance_options(logs));
    SimulationRun run;
    EXPECT_EQ(
        instance.create(praenora::test::create_document("simulating-module", parameters)).outcome,
        CallOutcome::succeeded);
    EXPECT_EQ(instance.initialize(praenora::test::initialize_document()).outcome,
              CallOutcome::succeeded);
    const praenora::detail::CallResult invoked =
        instance.invoke(praenora::test::invocation_document(0), 0);
    EXPECT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
    run.output = invoked.output;
    for (const json& record : logs.events()) {
        if (record.at("attributes").value("logger", std::string()) == "module.s" &&
            !record.at("attributes").contains("event")) {
            run.messages.push_back(record.at("body").get<std::string>());
        }
    }
    run.saved = events_named(logs, "compiled_models.saved");
    return run;
}

// simulating-module's parameters for a test-decay model, with their defaults (its manifest's), and
// a kernel.
json test_decay_parameters(const std::string& model) {
    return {{"model", model},
            {"format", "test-decay"},
            {"plan",
             {{"parameters", {"k_x"}},
              {"initial_conditions", {"x"}},
              {"time_domain", {{"start", 0}, {"end", 2}}},
              {"output",
               {{"save_at", "explicit_times"}, {"times", {0, 1, 2}}, {"quantities", {"total"}}}}}},
            {"values", {0.6931471805599453, 8}},
            {"kernel", true}};
}

#ifdef PRAENORA_TEST_WITH_ROADRUNNER
std::string file_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
#endif

bool holds(const std::vector<std::string>& messages, const std::string& part) {
    for (const std::string& message : messages) {
        if (message.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// A stored model that does not load is compiled instead, with a warning, and the outputs are those
// of a compile: test-simulation finds garbage under its model's key, then the entry of another
// model text. The compile keeps the model in the store again, and the next module host loads it.
TEST(Isolation, AStoredModelThatDoesNotLoadIsCompiledInstead) {
    // A text no earlier test of this process gave: JSON allows any number of spaces after it.
    static std::atomic<unsigned> next{0};
    const std::string text =
        R"({"states": {"x": 8, "y": 3}, "parameters": {"k_x": 0.6931471805599453, "k_y": 0}})" +
        std::string(1 + next.fetch_add(1), ' ');
    const ChildProvider provider =
        simulation_provider(praenora::test::fixture_provider_manifest("test-simulation"));
    const json parameters = test_decay_parameters(text);
    const SimulationRun compiled = simulate_sharing(provider, parameters);
    EXPECT_TRUE(holds(compiled.messages, "test-simulation: compiled the model"));
    ASSERT_EQ(compiled.saved.size(), 1u);
    const std::string key = compiled.saved[0].at("attributes").at("key").get<std::string>();
    const CompiledModelNamespace space{"test-simulation", provider.library_sha256};
    ASSERT_NE(praenora::detail::compiled_model_store().find(space, key), nullptr);

    const std::vector<std::pair<std::string, std::string>> corrupt = {
        {"garbage", "\x01garbage"},
        {"another text", json{{"text", "{}"}, {"model", "{}"}}.dump()},
    };
    for (const auto& [name, entry] : corrupt) {
        SCOPED_TRACE(name);
        praenora::detail::compiled_model_store().save(space, key, entry);
        const SimulationRun run = simulate_sharing(provider, parameters);
        EXPECT_EQ(run.output, compiled.output);
        EXPECT_TRUE(holds(run.messages, "the model in the shared store does not load"));
        EXPECT_TRUE(holds(run.messages, "test-simulation: compiled the model"));
        EXPECT_FALSE(holds(run.messages, "loaded the model from the shared store"));
    }
    const SimulationRun loaded = simulate_sharing(provider, parameters);
    EXPECT_EQ(loaded.output, compiled.output);
    EXPECT_TRUE(holds(loaded.messages, "test-simulation: loaded the model from the shared store"));
    EXPECT_FALSE(holds(loaded.messages, "compiled the model"));
}

#ifdef PRAENORA_TEST_WITH_ROADRUNNER
// roadrunner keeps a model's saved state in the store, in an envelope it checks before
// libRoadRunner loads the state. A state that does not load is compiled instead, with a warning
// through the calling instance's host, and the outputs are those of a compile: garbage under the
// model's key, the state the first module host saved with its last byte changed (the envelope's
// SHA-256 no longer fits it), and that state cut short by one byte. The compile keeps the state
// again, and the next module host loads it without compiling.
TEST(Isolation, ARoadrunnerStateThatDoesNotLoadIsCompiledInstead) {
    static std::atomic<unsigned> next{0};
    // A model text no earlier test of this process gave: roadrunner keys models by their text.
    std::string model = file_text(praenora::test::fixture_path("models/sbml/decay.xml"));
    model.insert(model.find('\n') + 1,
                 "<!-- " + fresh_key("run") + "-" + std::to_string(next.fetch_add(1)) + " -->\n");
    const json parameters = {
        {"model", model},
        {"format", "sbml"},
        {"plan",
         {{"parameters", {"k_x"}},
          {"initial_conditions", {"x"}},
          {"time_domain", {{"start", 0}, {"end", 2}}},
          {"output", {{"save_at", "explicit_times"}, {"times", {0, 0.5, 1, 2}}}},
          {"solver",
           {{"method", "cvode"},
            {"stiffness", "nonstiff"},
            {"relative_tolerance", 1e-12},
            {"absolute_tolerance", 1e-12}}}}},
        {"values", {0.5, 10}},
        {"kernel", true}};
    const ChildProvider provider =
        simulation_provider(praenora::test::utf8_path(PRAENORA_TEST_ROADRUNNER_MANIFEST));
    const SimulationRun compiled = simulate_sharing(provider, parameters);
    EXPECT_TRUE(holds(compiled.messages, "roadrunner: compiled the model"));
    ASSERT_EQ(compiled.saved.size(), 1u);
    const std::string key = compiled.saved[0].at("attributes").at("key").get<std::string>();
    const CompiledModelNamespace space{"roadrunner", provider.library_sha256};
    const auto saved = praenora::detail::compiled_model_store().find(space, key);
    ASSERT_NE(saved, nullptr);
    std::string changed = *saved;
    changed.back() = static_cast<char>(changed.back() ^ 0x5A);
    const std::vector<std::pair<std::string, std::string>> corrupt = {
        {"garbage", std::string(4096, '\x7F')},
        {"a byte of the state changed", changed},
        {"cut short", saved->substr(0, saved->size() - 1)},
    };
    for (const auto& [name, entry] : corrupt) {
        SCOPED_TRACE(name);
        praenora::detail::compiled_model_store().save(space, key, entry);
        const SimulationRun run = simulate_sharing(provider, parameters);
        EXPECT_EQ(run.output, compiled.output);
        EXPECT_TRUE(holds(run.messages, "roadrunner: the compiled model in the shared store does "
                                        "not load"));
        EXPECT_TRUE(holds(run.messages, "roadrunner: compiled the model"));
        EXPECT_FALSE(holds(run.messages, "loaded the compiled model from the shared store"));
    }
    const SimulationRun loaded = simulate_sharing(provider, parameters);
    EXPECT_EQ(loaded.output, compiled.output);
    EXPECT_TRUE(
        holds(loaded.messages, "roadrunner: loaded the compiled model from the shared store"));
    EXPECT_FALSE(holds(loaded.messages, "compiled the model"));
}
#endif

// A module host that breaks the store's rules breaks the channel's protocol, a crash with reason
// "protocol" (here during the load, where the fake module host sends it): a store request from the
// module host of a step that does not share, one that names no provider of its load, a save whose
// key is longer than 256 bytes, a save whose length is over its frame's limit (refused before any
// of it is allocated), a save within its frame's limit whose entry is over 128 MiB (a module host
// refuses it, as AnEntryOrAKeyOverItsLimitIsRefusedInTheModuleHost shows, and never sends it),
// and a notice that refuses an entry within the limits. A store frame from the module host of a
// step that does not share is refused at its header, before any of its payload is read: a save of
// 128 MiB that the fake module host announces, and never sends, is refused at once, before the fake
// module host ends (3 s).
TEST(Isolation, AModuleHostThatBreaksTheStoresRulesBreaksTheProtocol) {
    const ScratchDir directory;
    fs::create_symlink(praenora::test::utf8_path(PRAENORA_TEST_FAKE_MODULE_HOST),
                       directory.path() / "praenora-module-host");
    const praenora::test::ModuleHostDirectory hook(directory.path());
    struct Case {
        const char* mode;
        bool share;
        const char* problem;
        bool at_once = false; // refused within 2 s, where the fake module host waits for 3 s
    };
    const std::vector<Case> cases = {
        {"store-unshared", false,
         "a store_load frame came from the module host of a step that does not share compiled "
         "models"},
        {"store-save-unshared", false,
         "a store_save frame came from the module host of a step that does not share compiled "
         "models",
         true},
        {"store-provider", true, "names no provider"},
        {"store-key", true, "key"},
        {"store-huge", true, "over its limit"},
        {"store-refused-within", true, "within the limits"},
        {"store-entry-over", true, "entry of 134217729 bytes is over the limit of 134217728"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.mode);
        const auto started = std::chrono::steady_clock::now();
        const praenora::test::ScopedEnvironment fake("PRAENORA_TEST_FAKE_MODULE_HOST", c.mode);
        CapturedLogs logs;
        ChildSettings settings = praenora::test::child_settings(logs);
        settings.providers = {test_store(logs)};
        settings.share_compiled_models = c.share;
        try {
            static_cast<void>(LoadedModule::load_in_child(
                praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-service")),
                settings));
            ADD_FAILURE() << "the load did not fail";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
            EXPECT_EQ(json::parse(error.details_json()),
                      json({{"phase", "load"}, {"reason", "protocol"}}));
        }
        if (c.at_once) {
            EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
        }
        const std::vector<json> crashed = events_named(logs, "module_host.crashed");
        ASSERT_EQ(crashed.size(), 1u);
        EXPECT_NE(crashed[0].at("body").get<std::string>().find(c.problem), std::string::npos)
            << crashed[0].at("body");
        EXPECT_EQ(store_events(logs), 0u);
    }
}

// A module host that does not take the answer to its store request (it stopped reading its channel)
// cannot hold the runtime's reader, and with it the channel's writer, for good: after
// ChildSettings::store_reply_wait the answer counts as a broken protocol, and the module host is
// killed. The fake module host asks for an entry larger than the socket's buffers ("deaf", which
// this test keeps under test-store's namespace) and reads nothing more.
TEST(Isolation, AModuleHostThatDoesNotTakeTheStoresAnswerIsEnded) {
    const ScratchDir directory;
    fs::create_symlink(praenora::test::utf8_path(PRAENORA_TEST_FAKE_MODULE_HOST),
                       directory.path() / "praenora-module-host");
    const praenora::test::ModuleHostDirectory hook(directory.path());
    const praenora::test::ScopedEnvironment fake("PRAENORA_TEST_FAKE_MODULE_HOST", "store-deaf");
    CapturedLogs logs;
    praenora::detail::compiled_model_store().save(store_namespace(logs), "deaf",
                                                  std::string(praenora::test::beyond_the_channel_buffers(
                                                                  std::size_t{8} << 20),
                                                              'd'));
    ChildSettings settings = praenora::test::child_settings(logs);
    settings.providers = {test_store(logs)};
    settings.share_compiled_models = true;
    settings.store_reply_wait = std::chrono::milliseconds(500);
    const auto started = std::chrono::steady_clock::now();
    try {
        static_cast<void>(LoadedModule::load_in_child(
            praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-service")),
            settings));
        ADD_FAILURE() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
        EXPECT_EQ(json::parse(error.details_json()),
                  json({{"phase", "load"}, {"reason", "protocol"}}));
    }
    // Well before the fake module host would read again (60 s).
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20));
    const std::vector<json> crashed = events_named(logs, "module_host.crashed");
    ASSERT_EQ(crashed.size(), 1u);
    EXPECT_NE(crashed[0].at("body").get<std::string>().find("did not take"), std::string::npos)
        << crashed[0].at("body");
}

#endif // _WIN32
