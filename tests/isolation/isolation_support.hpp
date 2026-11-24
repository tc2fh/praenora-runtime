// What the isolation tests share: loading fixture modules in a module host, whether a process is
// gone, the number of threads of this process, and a scoped test hook for where the module host
// executable is looked for (OwnSignals, for module hosts that crash on purpose, is in
// scoped_environment.hpp). Header-only.
#ifndef PRAENORA_TESTS_ISOLATION_ISOLATION_SUPPORT_HPP
#define PRAENORA_TESTS_ISOLATION_ISOLATION_SUPPORT_HPP

#include "discovery/manifest.hpp"
#include "isolation/channel.hpp"
#include "isolation/child_module.hpp"
#include "loader/loaded_module.hpp"

#include "instance_fixtures.hpp"
#include "module_fixtures.hpp"
#include "scoped_environment.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace praenora::test {

// Settings of a module host whose events go to `logs`.
inline detail::ChildSettings child_settings(CapturedLogs& logs) {
    detail::ChildSettings settings;
    settings.logger = logs.hub().logger("module.s");
    return settings;
}

inline std::shared_ptr<detail::LoadedModule> load_in_child(std::string_view name,
                                                           CapturedLogs& logs) {
    return detail::LoadedModule::load_in_child(detail::load_manifest(fixture_manifest(name)),
                                               child_settings(logs));
}

// The module host's process id.
inline std::int64_t module_host_pid(const detail::LoadedModule& module) {
    return module.code().process_id().value_or(0);
}

// The module host's side in the runtime, for its counters.
inline const detail::ChildModule& child_of(const detail::LoadedModule& module) {
    return dynamic_cast<const detail::ChildModule&>(module.code());
}

#ifndef _WIN32
// Whether no process `pid` exists (a reaped child, or one that ended and was reaped elsewhere).
inline bool process_gone(std::int64_t pid) {
    return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
}

inline bool process_running(std::int64_t pid) {
    return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0;
}

// What a helper program writes to `descriptor` (its channel): everything until the end of the
// stream, or with `one_line` until the end of the first line (the newline left out); but no longer
// than `limit`, so that a helper that hangs does not hold the test until CTest's timeout.
inline std::string read_within(int descriptor, std::chrono::milliseconds limit, bool one_line) {
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return text;
        }
        pollfd readable{descriptor, POLLIN, 0};
        const int ready = ::poll(&readable, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return text;
        }
        char buffer[4096];
        const ssize_t got = ::recv(descriptor, buffer, one_line ? 1 : sizeof buffer, 0);
        if (got <= 0 || (one_line && buffer[0] == '\n')) {
            return text;
        }
        text.append(buffer, static_cast<std::size_t>(got));
    }
}

// How many bytes the runtime's channel to a module host may take before a write blocks, as this
// host sets the buffers of a socket pair made like the runtime's own, counted generously: twice
// the send buffer of one end (Linux keeps the double of the value it reports) and the receive
// buffer of the other, for a write from the first to the second. A stock Linux or macOS host takes
// some hundreds of KiB; a host tuned for throughput (net.core.wmem_default of megabytes, such as
// the cluster Rivanna's 16 MiB) takes tens of MiB, more than a test's fixed payload.
inline std::size_t channel_buffer_bytes() {
    const auto ends = detail::open_channel_pair();
    const auto option_of = [](int end, int option) {
        int value = 0;
        socklen_t size = sizeof value;
        return ::getsockopt(end, SOL_SOCKET, option, &value, &size) == 0 && value > 0
                   ? static_cast<std::size_t>(value)
                   : std::size_t{0};
    };
    const std::size_t total =
        2 * option_of(ends.first, SO_SNDBUF) + option_of(ends.second, SO_RCVBUF);
    ::close(ends.first);
    ::close(ends.second);
    return total;
}

// A size for a payload that the channel's buffers cannot take whole: `at_least`, or more when this
// host's buffers need more (twice what channel_buffer_bytes() counts, and a little over), so a
// write of it blocks until the peer reads.
inline std::size_t beyond_the_channel_buffers(std::size_t at_least) {
    const std::size_t buffers = channel_buffer_bytes();
    return at_least > 2 * buffers ? at_least : 2 * buffers + (std::size_t{1} << 16);
}

// Waits up to `limit` until process_gone(pid).
inline bool gone_within(std::int64_t pid, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!process_gone(pid)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}
#endif

// The number of threads of this process, or nullopt where it cannot be told.
inline std::optional<std::size_t> thread_count() {
#if defined(__APPLE__)
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  sizeof(thread_act_t) * count);
    return static_cast<std::size_t>(count);
#elif defined(__linux__)
    std::error_code ec;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        static_cast<void>(entry);
        ++count;
    }
    if (ec) {
        return std::nullopt;
    }
    return count;
#else
    return std::nullopt;
#endif
}

// Waits up to 5 s until this process has `count` threads again (threads that end are not gone
// from the count at once).
inline bool threads_back_to(std::optional<std::size_t> count) {
    if (!count) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto now = thread_count();
        if (!now || *now <= *count) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// For a scope, the runtime looks for its module host in `directory`.
class ModuleHostDirectory {
public:
    explicit ModuleHostDirectory(const std::filesystem::path& directory) {
        detail::set_module_host_directory_for_testing(directory);
    }
    ~ModuleHostDirectory() { detail::set_module_host_directory_for_testing(std::nullopt); }
    ModuleHostDirectory(const ModuleHostDirectory&) = delete;
    ModuleHostDirectory& operator=(const ModuleHostDirectory&) = delete;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_ISOLATION_ISOLATION_SUPPORT_HPP
