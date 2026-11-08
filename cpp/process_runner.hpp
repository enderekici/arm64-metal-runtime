#pragma once

// Whole-process orchestration for translated ARM64 macOS executables. Guest
// instructions are decoded into Metal and dispatched there; this runner never
// interprets or executes guest instructions on the CPU.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace arm64metal::process_runner {

struct LaunchOptions {
    bool sharedCache = false;
    bool demandCache = false;
    bool cacheBootstrap = false;
    bool cachedInitializers = false;
    bool interpreter = false;
    // Number of guest instructions allowed per Metal dispatch (1..8192).
    // Repeated bounded dispatches continue the process until exit or failure.
    std::uint32_t dispatchBlocks = 8192;
};

// Launch `executable` with arguments following argv[0]. The host environment
// is captured by default. Returns the guest process exit status; malformed
// images, unsupported imports, and unimplemented cache faults throw.
[[nodiscard]] std::int32_t launch_executable(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments = {},
    const LaunchOptions& options = {});

}  // namespace arm64metal::process_runner
