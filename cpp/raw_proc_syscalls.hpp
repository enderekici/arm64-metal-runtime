#pragma once

#include "guest_scheduler.hpp"
#include "guest_signal_runtime.hpp"
#include "process_host.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace arm64metal {

struct RawProcAlternateStack {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    bool enabled = false;
};

// Process-lifetime state behind the raw process, signal and identity syscalls.
struct RawProcState {
    std::map<std::uint64_t, RawProcAlternateStack> alternateStacks;
    std::set<std::uint64_t> cancelPending;
    std::set<std::uint64_t> cancelDisabled;
    // Mask to reinstate once sigsuspend's signal handler returns.
    std::map<std::uint64_t, std::uint32_t> sigsuspendRestore;
};

struct RawProcContext {
    RawProcContext(std::uint64_t* registerFile, GuestMemory& guestMemory,
                   HostServiceState& hostState, GuestScheduler& guestScheduler,
                   GuestSignalRuntime& runtime, RawProcState& procState,
                   const ProcessBootstrapResult& bootstrapResult)
        : registers(registerFile), memory(guestMemory), state(hostState),
          scheduler(guestScheduler), signalRuntime(runtime), proc(procState),
          bootstrap(bootstrapResult) {}

    std::uint64_t* registers = nullptr;
    GuestMemory& memory;
    HostServiceState& state;
    GuestScheduler& scheduler;
    GuestSignalRuntime& signalRuntime;
    RawProcState& proc;
    const ProcessBootstrapResult& bootstrap;
    std::function<std::optional<std::uint64_t>(std::uint32_t port)> threadForPort;
    std::function<std::uint32_t()> acquireTaskPort;
    std::function<void()> yieldCurrentThread;
    std::int32_t exitStatus = 0;
};

enum class RawProcOutcome { NotHandled, Handled, Exit };

// Raw BSD syscalls for process identity, limits, time, signals and the
// unentitled-process error surface. Returns NotHandled for other numbers.
[[nodiscard]] RawProcOutcome dispatch_raw_proc_syscall(RawProcContext& context,
                                                       std::uint64_t syscall);

}  // namespace arm64metal
