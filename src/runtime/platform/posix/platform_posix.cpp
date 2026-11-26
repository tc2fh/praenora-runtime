// platform/platform.hpp on POSIX systems (Linux, macOS).
#include "platform/platform.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace praenora::detail {

namespace {

// An object inside the runtime's binary, whose address dladdr() maps back to that binary.
const char kRuntimeMarker = 0;

// The largest buffer home_directory() offers getpwuid_r() for one password database entry.
constexpr std::size_t kMaxPasswordEntryBytes = std::size_t(1) << 20;

struct StackTask {
    const std::function<void()>* task = nullptr;
    std::exception_ptr error;
};

void* run_stack_task(void* argument) {
    auto* task = static_cast<StackTask*>(argument);
    try {
        (*task->task)();
    } catch (...) {
        task->error = std::current_exception();
    }
    return nullptr;
}

} // namespace

void run_on_thread_with_stack(std::size_t stack_bytes, const std::function<void()>& task) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        page = 4096;
    }
    std::size_t size = std::max<std::size_t>(stack_bytes, static_cast<std::size_t>(PTHREAD_STACK_MIN));
    size = (size + static_cast<std::size_t>(page) - 1) / static_cast<std::size_t>(page) *
           static_cast<std::size_t>(page);
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    const bool initialized = result == 0;
    if (result == 0) {
        result = pthread_attr_setstacksize(&attributes, size);
    }
    StackTask state;
    state.task = &task;
    pthread_t thread;
    if (result == 0) {
        result = pthread_create(&thread, &attributes, run_stack_task, &state);
    }
    if (initialized) {
        pthread_attr_destroy(&attributes);
    }
    if (result != 0) {
        throw ThreadStartError("cannot start a thread with a stack of " + std::to_string(size) +
                               " bytes: " + std::strerror(result));
    }
    pthread_join(thread, nullptr);
    if (state.error) {
        std::rethrow_exception(state.error);
    }
}

std::optional<std::string> environment_variable(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<std::filesystem::path> home_directory() {
    if (const auto home = environment_variable("HOME"); home && !home->empty()) {
        return std::filesystem::path(*home);
    }
    // The user's entry in the password database. The buffer grows until the entry fits.
    const long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggested > 0 ? static_cast<std::size_t>(suggested) : 1024);
    passwd entry{};
    passwd* found = nullptr;
    int result = 0;
    while ((result = getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found)) ==
               ERANGE &&
           buffer.size() < kMaxPasswordEntryBytes) {
        buffer.resize(buffer.size() * 2);
    }
    if (result != 0 || found == nullptr || found->pw_dir == nullptr || found->pw_dir[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(found->pw_dir);
}

std::optional<std::filesystem::path> runtime_binary_path() {
    Dl_info info{};
    if (dladdr(&kRuntimeMarker, &info) == 0 || info.dli_fname == nullptr ||
        info.dli_fname[0] == '\0') {
        return std::nullopt;
    }
    std::error_code ec;
    const auto path = std::filesystem::absolute(std::filesystem::path(info.dli_fname), ec);
    if (ec) {
        return std::nullopt;
    }
    return path.lexically_normal();
}

void set_current_thread_name(const std::string& name) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    // macOS takes up to 63 bytes, Linux up to 15, each plus the terminating NUL.
#if defined(__APPLE__)
    constexpr std::size_t kMaxBytes = 63;
#else
    constexpr std::size_t kMaxBytes = 15;
#endif
    char buffer[kMaxBytes + 1];
    const std::size_t size = std::min(name.size(), kMaxBytes);
    std::memcpy(buffer, name.data(), size);
    buffer[size] = '\0';
#if defined(__APPLE__)
    // macOS can name only the calling thread.
    static_cast<void>(pthread_setname_np(buffer));
#else
    static_cast<void>(pthread_setname_np(pthread_self(), buffer));
#endif
#else
    static_cast<void>(name);
#endif
}

std::int64_t current_process_id() noexcept { return static_cast<std::int64_t>(getpid()); }

} // namespace praenora::detail
