#pragma once

// Whole-process orchestration for translated ARM64 macOS executables. Guest
// instructions are decoded into Metal and dispatched there; this runner never
// interprets or executes guest instructions on the CPU.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace arm64metal::process_runner {

struct LaunchOptions {
    bool sharedCache = false;
    bool demandCache = false;
    bool cacheBootstrap = false;
    bool cachedInitializers = false;
    bool interpreter = false;
    // A selected AOT library is strict: load errors do not fall back to MSL.
    std::filesystem::path interpreterLibrary;
    // Used only when no AOT library was selected (development mode).
    std::filesystem::path interpreterSource;
    std::filesystem::path runtimeExecutablePath;
    std::optional<std::string> guestArgv0;
    std::optional<std::vector<std::string>> guestEnvironment;
    std::vector<std::int32_t> inheritedGuestDescriptors;
    std::vector<std::int32_t> inheritedWritableGuestDescriptors;
    // Number of guest instructions allowed per Metal dispatch (1..8192).
    // Repeated bounded dispatches continue the process until exit or failure.
    std::uint32_t dispatchBlocks = 8192;
};

// Launch `executable` with arguments following argv[0]. The host environment
// is captured by default. Returns the guest process exit status; malformed
// images, unsupported imports, and unimplemented cache faults throw. If
// `terminatingSignal` is non-null, it receives the default guest termination
// signal, or zero for a normal exit; signal propagation remains a CLI choice.
[[nodiscard]] std::int32_t launch_executable(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments = {},
    const LaunchOptions& options = {},
    std::int32_t* terminatingSignal = nullptr);

}  // namespace arm64metal::process_runner
