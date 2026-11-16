#include "raw_proc_syscalls.hpp"
#include "guest_survey.hpp"

#include <cstdio>

#include "darwin_bridge.hpp"
#include "image_loader.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <bsm/audit.h>

namespace arm64metal {

namespace {

constexpr std::uint64_t kCarry = 8U;
constexpr std::uint32_t kUnblockable = (1U << (SIGKILL - 1)) | (1U << (SIGSTOP - 1));

struct HostResult {
    std::uint64_t x0 = 0;
    std::uint64_t x1 = 0;
    bool failed = false;
};

// Issues the same BSD syscall on the host with host-resident buffers.
HostResult host_svc(std::uint64_t number, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                    std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0,
                    std::uint64_t a5 = 0) {
    register std::uint64_t x0 __asm__("x0") = a0;
    register std::uint64_t x1 __asm__("x1") = a1;
    register std::uint64_t x2 __asm__("x2") = a2;
    register std::uint64_t x3 __asm__("x3") = a3;
    register std::uint64_t x4 __asm__("x4") = a4;
    register std::uint64_t x5 __asm__("x5") = a5;
    register std::uint64_t x16 __asm__("x16") = number;
    std::uint64_t nzcv = 0;
    __asm__ volatile("svc #0x80\n\tmrs %[nzcv], nzcv"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
                       [nzcv] "=r"(nzcv)
                     : "r"(x16)
                     : "cc", "memory");
    return HostResult{x0, x1, ((nzcv >> 29U) & 1U) != 0};
}

std::uint64_t hp(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t signal_bit(std::int32_t signal) { return 1U << (signal - 1); }

class Dispatcher {
public:
    Dispatcher(RawProcContext& context, std::uint64_t syscall)
        : c_(context), r_(context.registers), syscall_(syscall) {}

    RawProcOutcome run();

private:
    RawProcContext& c_;
    std::uint64_t* r_;
    std::uint64_t syscall_;
    bool exit_ = false;

    std::uint64_t arg(std::size_t index) const { return r_[index]; }
    std::int32_t iarg(std::size_t index) const {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(r_[index]));
    }

    void ok(std::uint64_t value = 0) {
        r_[0] = value;
        r_[32] &= ~kCarry;
        r_[34] = 0;
    }
    void fail(int error) {
        if (error == ENOTSUP || error == ENOSYS) {
            survey("errno_stub", error == ENOSYS ? "ENOSYS" : "ENOTSUP");
        }
        const auto value = static_cast<std::uint32_t>(error);
        try {
            std::memcpy(c_.memory.buffer(c_.bootstrap.errnoSlot, sizeof(value), true), &value,
                        sizeof(value));
        } catch (const std::out_of_range&) {
        }
        r_[0] = static_cast<std::uint64_t>(error);
        r_[32] |= kCarry;
        r_[34] = 0;
    }
    void from_host(const HostResult& result) {
        if (result.failed) fail(static_cast<int>(result.x0));
        else ok(result.x0);
    }

    bool addressable(std::uint64_t address, std::uint64_t length) const {
        return address >= ExecutableImage::minimum_image_address &&
               address <= std::numeric_limits<std::uint64_t>::max() - length;
    }
    const std::uint8_t* read(std::uint64_t address, std::uint64_t length) {
        if (!addressable(address, length)) return nullptr;
        try {
            return c_.memory.buffer(address, length, false);
        } catch (const std::out_of_range&) {
            return nullptr;
        }
    }
    std::uint8_t* write(std::uint64_t address, std::uint64_t length) {
        if (!addressable(address, length)) return nullptr;
        try {
            return c_.memory.buffer(address, length, true);
        } catch (const std::out_of_range&) {
            return nullptr;
        }
    }
    bool put(std::uint64_t address, const void* data, std::uint64_t length) {
        auto* destination = write(address, length);
        if (destination == nullptr) return false;
        std::memcpy(destination, data, static_cast<std::size_t>(length));
        return true;
    }
    template <typename T>
    bool get(std::uint64_t address, T& value) {
        const auto* source = read(address, sizeof(T));
        if (source == nullptr) return false;
        std::memcpy(&value, source, sizeof(T));
        return true;
    }

    bool other_pid_exists(std::int32_t pid) const {
        return ::kill(pid, 0) == 0 || errno != ESRCH;
    }

    void deny() { fail(EPERM); }
    void survey(std::string_view kind, std::string_view sub) {
        guest_survey::note(kind, static_cast<std::int64_t>(syscall_), sub, c_.registers[33],
                           c_.registers[30], c_.scheduler.current_id());
    }
    void unsupported() { fail(ENOTSUP); }

    void identity();
    void groups_and_audit();
    void limits_time_info();
    void priority();
    void signals();
    void kill_call();
    void sigaction_call();
    void sigprocmask_call();
    void sigaltstack_call();
    void sigsuspend_call();
    void sigwait_call();
    void pthread_kill_call();
    void lifecycle();
    void unentitled();
    void mark_signal_pending(std::int32_t signal, bool processWide);
    void wait_for_signal(bool (*ready)(Dispatcher&, void*), void* data);
};

void Dispatcher::mark_signal_pending(std::int32_t signal, bool processWide) {
    auto& state = c_.state;
    if (processWide) {
        state.signals.raise(signal);
        if (state.signals.action(signal).handler != 1) {
            state.queuedProcessSignals |= signal_bit(signal);
        }
    } else if (state.signals.action(signal).handler != 1) {
        state.threadPendingSignals |= signal_bit(signal);
    }
}

void Dispatcher::kill_call() {
    const auto pid = iarg(0);
    const auto signal = iarg(1);
    if (signal < 0 || (signal != 0 && !GuestSignals::valid(signal))) {
        fail(EINVAL);
        return;
    }
    const auto self = static_cast<std::int32_t>(::getpid());
    const auto group = static_cast<std::int32_t>(::getpgrp());
    const bool toSelf = pid == self || pid == 0 || (pid < -1 && -pid == group);
    if (pid == -1) {
        fail(ESRCH);
        return;
    }
    if (toSelf) {
        if (signal != 0) mark_signal_pending(signal, true);
        ok(0);
        return;
    }
    if (pid < 0) {
        fail(ESRCH);
        return;
    }
    if (signal == 0) {
        if (::kill(pid, 0) == 0) ok(0);
        else fail(errno);
        return;
    }
    fail(other_pid_exists(pid) ? EPERM : ESRCH);
}

void Dispatcher::sigaction_call() {
    const auto signal = iarg(0);
    const auto requested = arg(1);
    const auto previous = arg(2);
    if (!GuestSignals::valid(signal) || signal == SIGKILL || signal == SIGSTOP) {
        fail(EINVAL);
        return;
    }
    const auto old = c_.state.signals.action(signal);
    std::optional<GuestSignals::Action> action;
    if (requested != 0) {
        const auto* bytes = read(requested, 24);
        if (bytes == nullptr) {
            fail(EFAULT);
            return;
        }
        std::uint64_t handler = 0;
        std::uint32_t mask = 0;
        std::int32_t flags = 0;
        std::memcpy(&handler, bytes, sizeof(handler));
        std::memcpy(&mask, bytes + 16, sizeof(mask));
        std::memcpy(&flags, bytes + 20, sizeof(flags));
        constexpr std::int32_t accepted = SA_RESTART | SA_RESETHAND | SA_NODEFER | SA_SIGINFO |
                                          SA_NOCLDSTOP | SA_NOCLDWAIT | 0x0200;
        if ((flags & ~accepted) != 0 && guest_survey::enabled()) {
            char sub[16];
            std::snprintf(sub, sizeof(sub), "0x%x", static_cast<unsigned>(flags & ~accepted));
            survey("flag", sub);
            fail(EINVAL);
            return;
        }
        if ((flags & ~accepted) != 0) {
            throw std::runtime_error("Guest raw sigaction flags unsupported");
        }
        action = GuestSignals::Action{handler, mask, flags};
    }
    if (previous != 0) {
        std::uint8_t out[16] = {};
        std::memcpy(out, &old.handler, sizeof(old.handler));
        std::memcpy(out + 8, &old.mask, sizeof(old.mask));
        std::memcpy(out + 12, &old.flags, sizeof(old.flags));
        if (!put(previous, out, sizeof(out))) {
            fail(EFAULT);
            return;
        }
    }
    if (action) {
        const bool hostSignal = signal == SIGHUP || signal == SIGINT || signal == SIGQUIT ||
                                signal == SIGPIPE || signal == SIGALRM || signal == SIGTERM ||
                                signal == SIGUSR1 || signal == SIGUSR2 || signal == SIGINFO;
        const bool synchronous = signal == SIGSEGV || signal == SIGBUS || signal == SIGILL ||
                                 signal == SIGTRAP;
        const bool software = signal == SIGABRT || signal == SIGFPE || signal == SIGSYS ||
                              signal == SIGCHLD || signal == SIGCONT || signal == SIGTSTP ||
                              signal == SIGTTIN || signal == SIGTTOU || signal == SIGURG ||
                              signal == SIGWINCH || signal == SIGEMT || signal == SIGIO ||
                              signal == SIGPROF || signal == SIGVTALRM || signal == SIGXCPU ||
                              signal == SIGXFSZ;
        if (!hostSignal && !synchronous && !software) {
            fail(ENOTSUP);
            return;
        }
        if (hostSignal &&
            guest_signal_listen(signal, action->handler == 1 ? 1 : 0,
                                (action->flags & SA_RESTART) != 0 ? 1 : 0) != 0) {
            fail(errno);
            return;
        }
        c_.state.signals.set_action(*action, signal);
        if (action->handler == 1) {
            c_.state.queuedProcessSignals &= ~signal_bit(signal);
            c_.state.threadPendingSignals &= ~signal_bit(signal);
        }
    }
    ok(0);
}

void Dispatcher::sigprocmask_call() {
    const auto how = iarg(0);
    const auto set = arg(1);
    const auto oset = arg(2);
    auto& state = c_.state;
    const auto old = state.threadSignalMask;
    std::uint32_t value = 0;
    if (set != 0 && !get(set, value)) {
        fail(EFAULT);
        return;
    }
    if (set != 0 && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
        fail(EINVAL);
        return;
    }
    if (oset != 0 && !put(oset, &old, sizeof(old))) {
        fail(EFAULT);
        return;
    }
    if (set != 0) {
        state.signals.set_mask_value(old);
        (void)state.signals.set_mask(how, value);
        state.threadSignalMask = state.signals.mask();
    }
    ok(0);
}

void Dispatcher::sigaltstack_call() {
    const auto requested = arg(0);
    const auto previous = arg(1);
    constexpr std::int32_t onStack = 1;
    constexpr std::int32_t disabled = 4;
    constexpr std::uint64_t minimumSize = 32768;
    auto& entry = c_.proc.alternateStacks[c_.scheduler.current_id()];
    const bool active = entry.enabled && r_[31] >= entry.base && r_[31] < entry.base + entry.size;
    std::uint8_t request[24] = {};
    if (requested != 0) {
        const auto* bytes = read(requested, sizeof(request));
        if (bytes == nullptr) {
            fail(EFAULT);
            return;
        }
        std::memcpy(request, bytes, sizeof(request));
    }
    if (previous != 0) {
        std::uint8_t out[24] = {};
        const std::uint64_t base = entry.enabled ? entry.base : 0;
        const std::uint64_t size = entry.enabled ? entry.size : 0;
        const std::int32_t flags = entry.enabled ? (active ? onStack : 0) : disabled;
        std::memcpy(out, &base, sizeof(base));
        std::memcpy(out + 8, &size, sizeof(size));
        std::memcpy(out + 16, &flags, sizeof(flags));
        if (!put(previous, out, sizeof(out))) {
            fail(EFAULT);
            return;
        }
    }
    if (requested != 0) {
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        std::int32_t flags = 0;
        std::memcpy(&base, request, sizeof(base));
        std::memcpy(&size, request + 8, sizeof(size));
        std::memcpy(&flags, request + 16, sizeof(flags));
        if (active) {
            fail(EPERM);
            return;
        }
        if ((flags & ~disabled) != 0) {
            fail(EINVAL);
            return;
        }
        if ((flags & disabled) != 0) {
            entry = RawProcAlternateStack{};
        } else {
            if (size < minimumSize) {
                fail(ENOMEM);
                return;
            }
            entry = RawProcAlternateStack{base, size, true};
        }
    }
    ok(0);
}

void Dispatcher::wait_for_signal(bool (*ready)(Dispatcher&, void*), void* data) {
    while (true) {
        c_.state.ingest_host_signals();
        if (ready(*this, data)) return;
        if (c_.scheduler.thread_count() > 1) {
            throw std::runtime_error(
                "Guest signal wait would block while other guest threads exist");
        }
        const timespec pause{0, 1'000'000};
        ::nanosleep(&pause, nullptr);
    }
}

void Dispatcher::sigsuspend_call() {
    auto& state = c_.state;
    const auto old = state.threadSignalMask;
    state.signals.set_mask_value(static_cast<std::uint32_t>(arg(0)));
    state.threadSignalMask = state.signals.mask();
    if (state.signals.has_unblocked_pending(state.threadPendingSignals)) {
        c_.proc.sigsuspendRestore[c_.scheduler.current_id()] = old;
    } else if (c_.scheduler.thread_count() > 1 && c_.yieldCurrentThread) {
        const auto thread = c_.scheduler.current_id();
        c_.proc.sigsuspendRestore[thread] = old;
        c_.scheduler.current().wait = GuestSignalSuspendWait{};
        fail(EINTR);
        c_.yieldCurrentThread();
        return;
    } else {
        wait_for_signal(
            [](Dispatcher& self, void*) {
                return self.c_.state.signals.has_unblocked_pending(
                    self.c_.state.threadPendingSignals);
            },
            nullptr);
        c_.proc.sigsuspendRestore[c_.scheduler.current_id()] = old;
    }
    fail(EINTR);
}

void Dispatcher::sigwait_call() {
    const auto setAddress = arg(0);
    const auto outputAddress = arg(1);
    if (setAddress == 0) {
        fail(EINVAL);
        return;
    }
    std::uint32_t set = 0;
    if (!get(setAddress, set)) {
        fail(EFAULT);
        return;
    }
    set &= ~kUnblockable;
    if (set == 0) {
        fail(EINVAL);
        return;
    }
    auto& state = c_.state;
    struct Wanted {
        std::uint32_t set;
    } wanted{set};
    wait_for_signal(
        [](Dispatcher& self, void* data) {
            const auto want = static_cast<Wanted*>(data)->set;
            return ((self.c_.state.signals.pending() | self.c_.state.threadPendingSignals) &
                    want) != 0;
        },
        &wanted);
    const auto live = state.threadSignalMask;
    state.signals.set_mask_value(~set);
    const auto taken = state.signals.next(state.threadPendingSignals);
    state.signals.set_mask_value(live);
    state.threadSignalMask = state.signals.mask();
    if (!taken) {
        fail(EINVAL);
        return;
    }
    state.queuedProcessSignals &= ~signal_bit(taken->first);
    if (outputAddress != 0) {
        const auto number = static_cast<std::int32_t>(taken->first);
        if (!put(outputAddress, &number, sizeof(number))) {
            fail(EFAULT);
            return;
        }
    }
    ok(0);
}

void Dispatcher::pthread_kill_call() {
    const auto port = static_cast<std::uint32_t>(arg(0));
    const auto signal = iarg(1);
    const auto thread = c_.threadForPort ? c_.threadForPort(port) : std::nullopt;
    if (!thread || c_.scheduler.find(*thread) == nullptr) {
        fail(ESRCH);
        return;
    }
    if (signal < 0 || (signal != 0 && !GuestSignals::valid(signal))) {
        fail(EINVAL);
        return;
    }
    if (signal != 0) {
        if (*thread == c_.scheduler.current_id()) {
            mark_signal_pending(signal, false);
        } else if (c_.state.signals.action(signal).handler != 1) {
            c_.scheduler.find(*thread)->pendingSignals |= signal_bit(signal);
        }
    }
    ok(0);
}

void Dispatcher::signals() {
    switch (syscall_) {
        case 37: kill_call(); return;
        case 46: sigaction_call(); return;
        case 48:
        case 329: sigprocmask_call(); return;
        case 52: {
            c_.state.ingest_host_signals();
            const auto pending = (c_.state.signals.pending() | c_.state.threadPendingSignals) &
                                 c_.state.threadSignalMask;
            if (!put(arg(0), &pending, sizeof(pending))) fail(EFAULT);
            else ok(0);
            return;
        }
        case 53: sigaltstack_call(); return;
        case 111:
        case 410: sigsuspend_call(); return;
        case 330:
        case 422: sigwait_call(); return;
        case 328: pthread_kill_call(); return;
        case 331:
            c_.state.signals.set_mask_value(~std::uint32_t{0});
            c_.state.threadSignalMask = c_.state.signals.mask();
            ok(0);
            return;
        case 332: {
            const auto thread = c_.threadForPort ? c_.threadForPort(
                static_cast<std::uint32_t>(arg(0))) : std::nullopt;
            if (!thread || c_.scheduler.find(*thread) == nullptr) {
                fail(ESRCH);
                return;
            }
            c_.proc.cancelPending.insert(*thread);
            ok(0);
            return;
        }
        case 333: {
            const auto action = iarg(0);
            if (action == 1 || action == 2) {
                ok(0);
            } else if (action == 0 &&
                       c_.proc.cancelPending.erase(c_.scheduler.current_id()) != 0) {
                ok(0);
            } else {
                fail(EINVAL);
            }
            return;
        }
        case 184: {
            // Handlers return through the runner's sentinel, never through sigreturn.
            if (read(arg(0), 8) == nullptr) fail(EFAULT);
            else fail(EINVAL);
            return;
        }
        default: return;
    }
}

void Dispatcher::identity() {
    switch (syscall_) {
        case 20: ok(static_cast<std::uint64_t>(::getpid())); return;
        case 39: ok(static_cast<std::uint64_t>(::getppid())); return;
        case 24: ok(::getuid()); return;
        case 25: ok(::geteuid()); return;
        case 47: ok(::getgid()); return;
        case 43: ok(::getegid()); return;
        case 81: ok(static_cast<std::uint64_t>(::getpgrp())); return;
        case 151: {
            const auto result = ::getpgid(iarg(0));
            if (result < 0) fail(errno);
            else ok(static_cast<std::uint64_t>(result));
            return;
        }
        case 310: {
            const auto result = ::getsid(iarg(0));
            if (result < 0) fail(errno);
            else ok(static_cast<std::uint64_t>(result));
            return;
        }
        case 82: {  // setpgid
            const auto pid = iarg(0);
            const auto group = iarg(1);
            const auto self = static_cast<std::int32_t>(::getpid());
            if (pid != 0 && pid != self) {
                fail(ESRCH);
                return;
            }
            if (::setpgid(0, group) == 0) ok(0);
            else fail(errno);
            return;
        }
        case 147: {  // setsid
            const auto result = ::setsid();
            if (result < 0) fail(errno);
            else ok(static_cast<std::uint64_t>(result));
            return;
        }
        case 89: ok(static_cast<std::uint64_t>(::getdtablesize())); return;
        case 327: ok(static_cast<std::uint64_t>(::issetugid())); return;
        case 215: {
            const auto current = ::umask(0);
            ::umask(current);
            ok(current);
            return;
        }
        case 152: from_host(host_svc(152, arg(0))); return;
        case 23:  // setuid
        case 183: {  // seteuid
            const auto target = static_cast<std::uint32_t>(arg(0));
            if (target == ::geteuid() && (syscall_ == 183 || target == ::getuid())) ok(0);
            else fail(EPERM);
            return;
        }
        case 181:  // setgid
        case 182: {  // setegid
            const auto target = static_cast<std::uint32_t>(arg(0));
            if (target == ::getegid() && (syscall_ == 182 || target == ::getgid())) ok(0);
            else fail(EPERM);
            return;
        }
        case 126: {  // setreuid
            const auto real = static_cast<std::uint32_t>(arg(0));
            const auto effective = static_cast<std::uint32_t>(arg(1));
            const bool same = (real == 0xffffffffU || real == ::getuid()) &&
                              (effective == 0xffffffffU || effective == ::geteuid());
            if (same) ok(0);
            else fail(EPERM);
            return;
        }
        case 127: {  // setregid
            const auto real = static_cast<std::uint32_t>(arg(0));
            const auto effective = static_cast<std::uint32_t>(arg(1));
            const bool same = (real == 0xffffffffU || real == ::getgid()) &&
                              (effective == 0xffffffffU || effective == ::getegid());
            if (same) ok(0);
            else fail(EPERM);
            return;
        }
        default: return;
    }
}

void Dispatcher::groups_and_audit() {
    switch (syscall_) {
        case 79: {  // getgroups
            const auto count = iarg(0);
            std::array<gid_t, 16> groups{};
            const auto available = ::getgroups(static_cast<int>(groups.size()), groups.data());
            if (available < 0) {
                fail(errno);
                return;
            }
            if (count == 0) {
                ok(static_cast<std::uint64_t>(available));
                return;
            }
            if (count < available) {
                fail(EINVAL);
                return;
            }
            if (count < 0 ||
                !put(arg(1), groups.data(), static_cast<std::uint64_t>(available) * sizeof(gid_t))) {
                fail(EFAULT);
                return;
            }
            ok(static_cast<std::uint64_t>(available));
            return;
        }
        case 80: {  // setgroups
            const auto count = iarg(0);
            fail(count < 0 || count > 16 ? EINVAL : EPERM);
            return;
        }
        case 243: {  // initgroups
            const auto count = iarg(0);
            fail(count < 0 || count > 16 ? EINVAL : EPERM);
            return;
        }
        case 287: case 288: case 289: case 290: unsupported(); return;
        case 49: {  // getlogin
            const auto length = std::min<std::uint64_t>(static_cast<std::uint32_t>(arg(1)), 255);
            std::array<char, 256> name{};
            const auto result = host_svc(49, hp(name.data()), length);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), name.data(), length)) {
                fail(EFAULT);
                return;
            }
            ok(0);
            return;
        }
        case 50: deny(); return;
        case 353: {  // getauid
            std::uint32_t identifier = 0;
            const auto result = host_svc(353, hp(&identifier));
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), &identifier, sizeof(identifier))) {
                fail(EFAULT);
                return;
            }
            ok(0);
            return;
        }
        case 354: {
            std::uint32_t identifier = 0;
            if (!get(arg(0), identifier)) fail(EFAULT);
            else deny();
            return;
        }
        case 357: {  // getaudit_addr
            const auto length = static_cast<std::uint32_t>(arg(1));
            constexpr std::size_t recordSize = sizeof(auditinfo_addr_t);
            std::array<std::uint8_t, 256> record{};
            const auto result = host_svc(357, hp(record.data()), length > 256 ? 256 : length);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), record.data(), std::min<std::size_t>(length, recordSize))) {
                fail(EFAULT);
                return;
            }
            ok(0);
            return;
        }
        case 358: {  // setaudit_addr
            const auto length = static_cast<std::uint32_t>(arg(1));
            if (read(arg(0), 1) == nullptr) fail(EFAULT);
            else if (length != sizeof(auditinfo_addr_t)) fail(EINVAL);
            else deny();
            return;
        }
        case 350: case 359: deny(); return;
        case 351: {  // auditon
            fail(iarg(0) == 32 ? EINVAL : EPERM);
            return;
        }
        case 428: ok(0); return;  // MACH_PORT_NULL: no audit session port is modeled
        case 429: fail(EINVAL); return;  // no audit session port exists in the guest namespace
        case 432: deny(); return;
        default: return;
    }
}

void Dispatcher::priority() {
    const auto which = iarg(0);
    const auto who = iarg(1);
    const auto self = static_cast<std::int32_t>(::getpid());
    const bool isGet = syscall_ == 100;
    if (which < 0 || which > 6) {
        fail(EINVAL);
        return;
    }
    if (isGet) {
        from_host(host_svc(100, static_cast<std::uint64_t>(which),
                           static_cast<std::uint64_t>(static_cast<std::uint32_t>(who))));
        return;
    }
    const auto value = arg(2);
    bool applyToSelf = false;
    switch (which) {
        case PRIO_PROCESS: applyToSelf = who == 0 || who == self; break;
        case PRIO_PGRP: applyToSelf = who == 0 || who == ::getpgrp(); break;
        case PRIO_USER: applyToSelf = who == 0 || static_cast<uid_t>(who) == ::getuid(); break;
        default: applyToSelf = who == 0 || who == self; break;
    }
    if (!applyToSelf) {
        if (which == PRIO_PROCESS && other_pid_exists(who)) fail(EPERM);
        else fail(ESRCH);
        return;
    }
    const auto hostWhich = which == PRIO_PGRP || which == PRIO_USER ? PRIO_PROCESS : which;
    from_host(host_svc(96, static_cast<std::uint64_t>(hostWhich), 0, value));
}

void Dispatcher::limits_time_info() {
    switch (syscall_) {
        case 194: {  // getrlimit
            struct rlimit limit {};
            if (::getrlimit(iarg(0), &limit) != 0) {
                fail(errno);
                return;
            }
            if (!put(arg(1), &limit, sizeof(limit))) fail(EFAULT);
            else ok(0);
            return;
        }
        case 195: {  // setrlimit
            struct rlimit limit {};
            if (!get(arg(1), limit)) {
                fail(EFAULT);
                return;
            }
            if (::setrlimit(iarg(0), &limit) != 0) fail(errno);
            else ok(0);
            return;
        }
        case 117: {  // getrusage
            struct rusage usage {};
            if (::getrusage(iarg(0), &usage) != 0) {
                fail(errno);
                return;
            }
            if (!put(arg(1), &usage, sizeof(usage))) fail(EFAULT);
            else ok(0);
            return;
        }
        case 116: {  // gettimeofday(tv, tz, mach_absolute_time)
            struct timeval tv {};
            struct timezone tz {};
            std::uint64_t machTime = 0;
            const auto result = host_svc(116, arg(0) != 0 ? hp(&tv) : 0,
                                         arg(1) != 0 ? hp(&tz) : 0,
                                         arg(2) != 0 ? hp(&machTime) : 0);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if ((arg(0) != 0 && !put(arg(0), &tv, sizeof(tv))) ||
                (arg(1) != 0 && !put(arg(1), &tz, sizeof(tz))) ||
                (arg(2) != 0 && !put(arg(2), &machTime, sizeof(machTime)))) {
                fail(EFAULT);
                return;
            }
            ok(0);
            return;
        }
        case 122: case 140: deny(); return;
        case 527: {  // ntp_adjtime
            struct timex tx {};
            if (!get(arg(0), tx)) {
                fail(EFAULT);
                return;
            }
            if (tx.modes != 0) {
                deny();
                return;
            }
            const auto result = host_svc(527, hp(&tx));
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), &tx, sizeof(tx))) {
                fail(EFAULT);
                return;
            }
            ok(result.x0);
            return;
        }
        case 528: {  // ntp_gettime
            struct ntptimeval value {};
            const auto result = host_svc(528, hp(&value));
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), &value, sizeof(value))) {
                fail(EFAULT);
                return;
            }
            ok(result.x0);
            return;
        }
        case 500: {  // getentropy
            const auto length = arg(1);
            if (length > 256) {
                fail(EINVAL);
                return;
            }
            std::array<std::uint8_t, 256> bytes{};
            const auto result = host_svc(500, hp(bytes.data()), length);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), bytes.data(), length)) {
                fail(EFAULT);
                return;
            }
            ok(0);
            return;
        }
        case 142: {  // gethostuuid
            std::array<std::uint8_t, 16> uuid{};
            struct timespec timeout {};
            const bool hasTimeout = arg(1) != 0;
            if (hasTimeout && !get(arg(1), timeout)) {
                fail(EFAULT);
                return;
            }
            const auto result = host_svc(142, hp(uuid.data()), hasTimeout ? hp(&timeout) : 0);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (!put(arg(0), uuid.data(), uuid.size())) {
                fail(EFAULT);
                return;
            }
            ok(0);
            return;
        }
        case 83:    // setitimer
        case 86: {  // getitimer
            const auto which = iarg(0);
            if (which < 0 || which > 2) {
                fail(EINVAL);
                return;
            }
            struct itimerval value {};
            struct itimerval previous {};
            if (syscall_ == 86) {
                const auto result = host_svc(86, static_cast<std::uint64_t>(which), hp(&previous));
                if (result.failed) {
                    fail(static_cast<int>(result.x0));
                } else if (!put(arg(1), &previous, sizeof(previous))) {
                    fail(EFAULT);
                } else {
                    ok(0);
                }
                return;
            }
            const bool hasNew = arg(1) != 0;
            if (hasNew && !get(arg(1), value)) {
                fail(EFAULT);
                return;
            }
            const bool arms = hasNew && (value.it_value.tv_sec != 0 || value.it_value.tv_usec != 0 ||
                                         value.it_interval.tv_sec != 0 ||
                                         value.it_interval.tv_usec != 0);
            // SIGVTALRM and SIGPROF cannot be handled by guest code, so only ITIMER_REAL arms.
            if (arms && which != ITIMER_REAL) {
                unsupported();
                return;
            }
            const auto result = host_svc(83, static_cast<std::uint64_t>(which),
                                         hasNew ? hp(&value) : 0,
                                         arg(2) != 0 ? hp(&previous) : 0);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
            } else if (arg(2) != 0 && !put(arg(2), &previous, sizeof(previous))) {
                fail(EFAULT);
            } else {
                ok(0);
            }
            return;
        }
        case 186: {  // thread_selfcounts(type, buffer, size)
            const auto size = arg(2);
            if (size > 4096) {
                fail(EINVAL);
                return;
            }
            std::array<std::uint8_t, 4096> bytes{};
            if (size != 0) {
                const auto* source = read(arg(1), size);
                if (source == nullptr) {
                    fail(EFAULT);
                    return;
                }
                std::memcpy(bytes.data(), source, static_cast<std::size_t>(size));
            }
            const auto result = host_svc(186, arg(0), hp(bytes.data()), size);
            if (result.failed) {
                fail(static_cast<int>(result.x0));
                return;
            }
            if (size != 0 && !put(arg(1), bytes.data(), size)) {
                fail(EFAULT);
                return;
            }
            ok(result.x0);
            return;
        }
        case 482: ok(host_svc(482).x0); return;
        case 453: {  // memorystatus_get_level
            std::int32_t level = 0;
            const auto result = host_svc(453, hp(&level));
            if (result.failed) {
                fail(static_cast<int>(result.x0));
            } else if (!put(arg(0), &level, sizeof(level))) {
                fail(EFAULT);
            } else {
                ok(0);
            }
            return;
        }
        case 534: from_host(host_svc(534)); return;
        case 531: from_host(host_svc(531, arg(0))); return;
        case 296: {  // vm_pressure_monitor
            if (iarg(0) != 0) {
                unsupported();
                return;
            }
            std::uint32_t pages = 0;
            const auto result = host_svc(296, 0, arg(1), hp(&pages));
            if (result.failed) {
                fail(static_cast<int>(result.x0));
            } else if (arg(2) != 0 && !put(arg(2), &pages, sizeof(pages))) {
                fail(EFAULT);
            } else {
                ok(result.x0);
            }
            return;
        }
        case 322: {  // iopolicysys(cmd, {scope, iotype, policy})
            std::int32_t parameters[3] = {};
            if (!get(arg(1), parameters)) {
                fail(EFAULT);
                return;
            }
            const auto result = host_svc(322, arg(0), hp(parameters));
            if (result.failed) {
                fail(static_cast<int>(result.x0));
            } else if (!put(arg(1), parameters, sizeof(parameters))) {
                fail(EFAULT);
            } else {
                ok(result.x0);
            }
            return;
        }
        case 274: {  // sysctlbyname(name, namelen, old, oldlenp, new, newlen)
            const auto nameLength = arg(1);
            if (nameLength == 0 || nameLength > 256) {
                fail(EINVAL);
                return;
            }
            const auto* nameBytes = read(arg(0), nameLength);
            if (nameBytes == nullptr) {
                fail(EFAULT);
                return;
            }
            const std::string name(reinterpret_cast<const char*>(nameBytes),
                                   static_cast<std::size_t>(nameLength));
            if (arg(4) != 0 || arg(5) != 0) {
                deny();
                return;
            }
            std::uint64_t capacity = 0;
            const bool hasLength = arg(3) != 0;
            if (hasLength && !get(arg(3), capacity)) {
                fail(EFAULT);
                return;
            }
            if (name == "kern.usrstack64" || name == "kern.usrstack") {
                const bool wide = name == "kern.usrstack64";
                const std::uint64_t top = static_cast<std::uint64_t>(c_.bootstrap.stack.end);
                const std::uint64_t size = wide ? sizeof(std::uint64_t) : sizeof(std::uint32_t);
                if (arg(2) != 0 && !hasLength) {
                    fail(EFAULT);
                    return;
                }
                if (arg(2) != 0 && capacity >= size) {
                    const auto narrow = static_cast<std::uint32_t>(top);
                    if (!put(arg(2), wide ? static_cast<const void*>(&top)
                                          : static_cast<const void*>(&narrow), size)) {
                        fail(EFAULT);
                        return;
                    }
                }
                if (hasLength && !put(arg(3), &size, sizeof(size))) {
                    fail(EFAULT);
                    return;
                }
                if (arg(2) != 0 && capacity < size) fail(ENOMEM);
                else ok(0);
                return;
            }
            if (arg(2) != 0 && !hasLength) {
                fail(EFAULT);
                return;
            }
            constexpr std::uint64_t cap = 1U << 20;
            if (capacity > cap) {
                fail(ENOMEM);
                return;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(capacity));
            std::size_t length = static_cast<std::size_t>(capacity);
            const auto status = ::sysctlbyname(name.c_str(), arg(2) != 0 ? bytes.data() : nullptr,
                                               hasLength ? &length : nullptr, nullptr, 0);
            const auto error = errno;
            if (status != 0 && error != ENOMEM) {
                fail(error);
                return;
            }
            if (hasLength && !put(arg(3), &length, sizeof(length))) {
                fail(EFAULT);
                return;
            }
            if (status == 0 && arg(2) != 0 && !put(arg(2), bytes.data(),
                    std::min<std::uint64_t>(length, capacity))) {
                fail(EFAULT);
                return;
            }
            if (status != 0) fail(error);
            else ok(0);
            return;
        }
        default: return;
    }
}

void Dispatcher::lifecycle() {
    switch (syscall_) {
        case 1:
            c_.exitStatus = iarg(0);
            exit_ = true;
            return;
        case 2: case 66: fail(EAGAIN); return;
        case 7: case 400: fail(ECHILD); return;
        case 173: case 416: {
            constexpr std::int32_t exited = 0x4;
            constexpr std::int32_t stopped = 0x8;
            constexpr std::int32_t continued = 0x10;
            const auto options = iarg(3);
            if ((options & (exited | stopped | continued)) == 0 || arg(0) > 2) fail(EINVAL);
            else fail(ECHILD);
            return;
        }
        case 59: case 380: case 244: deny(); return;
        case 26: {  // ptrace
            constexpr std::int32_t denyAttach = 31;
            if (iarg(0) == denyAttach) ok(0);
            else deny();
            return;
        }
        case 38: unsupported(); return;
        case 433: case 434: case 435: {
            const auto pid = iarg(0);
            if (pid != static_cast<std::int32_t>(::getpid()) && !other_pid_exists(pid)) fail(ESRCH);
            else deny();
            return;
        }
        case 436: {
            const auto level = iarg(1);
            const auto pid = iarg(0);
            if (level < 1 || level > 3) fail(EINVAL);
            else if (pid != static_cast<std::int32_t>(::getpid()) && !other_pid_exists(pid))
                fail(ESRCH);
            else deny();
            return;
        }
        case 520: {  // terminate_with_payload(pid, ...)
            const auto pid = iarg(0);
            if (pid == 0) {
                fail(EINVAL);
            } else if (pid == static_cast<std::int32_t>(::getpid())) {
                c_.exitStatus = 128 + SIGKILL;
                exit_ = true;
            } else {
                fail(other_pid_exists(pid) ? EPERM : ESRCH);
            }
            return;
        }
        case 521:  // abort_with_payload raises SIGABRT on the calling thread
            mark_signal_pending(SIGABRT, false);
            ok(0);
            return;
        case 529: ok(0); return;
        default: return;
    }
}

void Dispatcher::unentitled() {
    switch (syscall_) {
        case 51: case 55: case 85: case 155:
            // acct, reboot, swapon and nfssvc: privileged
            if (syscall_ == 85) unsupported();
            else deny();
            return;
        case 178: case 180: ok(0); return;
        case 185: case 285: case 293: case 439: case 440: case 445: case 452: case 454:
        case 458: case 477: case 491: case 492: case 493: case 519: case 532: case 533:
        case 546: case 552: case 556: case 557:
            deny();
            return;
        case 311: {
            const auto pid = iarg(0);
            if (pid != static_cast<std::int32_t>(::getpid()) && !other_pid_exists(pid))
                fail(ESRCH);
            else deny();
            return;
        }
        case 323: fail(EINVAL); return;
        case 446: {
            const auto pid = iarg(0);
            if (pid != 0 && pid != static_cast<std::int32_t>(::getpid()) && !other_pid_exists(pid))
                fail(ESRCH);
            else fail(EACCES);
            return;
        }
        case 547: case 548: unsupported(); return;
        case 456: fail(iarg(0) == 1 || iarg(0) == 2 ? EPERM : ENOTSUP); return;
        case 457: fail(EINVAL); return;
        case 535: fail(EACCES); return;
        case 494: {
            static constexpr int table[] = {ENOSYS, EPERM, EPERM, EPERM, ESRCH,
                                            EFAULT, EFAULT, EFAULT, EINVAL, EFAULT};
            const auto operation = arg(0);
            fail(operation < std::size(table) ? table[operation] : ENOSYS);
            return;
        }
        case 501: case 499: unsupported(); return;
        case 502: case 523: fail(EBADF); return;
        case 522: fail(EACCES); return;
        case 460: case 490: case 525: case 503: case 504: case 505: case 506: case 507:
        case 508: case 509: case 510: case 511: case 512: case 513: case 514:
            fail(EINVAL);
            return;
        case 373: {
            const auto command = iarg(0);
            if (command < 0 || command > 2) fail(EINVAL);
            else fail(arg(1) == 0 ? EFAULT : ENOTSUP);
            return;
        }
        case 451: fail(iarg(0) == 1 ? ENOTSUP : EINVAL); return;
        case 459: unsupported(); return;
        case 545: fail(EINVAL); return;
        case 478: fail(EINVAL); return;
        case 538:
        case 539: {
            const auto task = static_cast<std::uint32_t>(arg(0));
            const auto pid = iarg(1);
            if (task != c_.bootstrap.guestTaskPort) {
                fail(EINVAL);
            } else if (pid == static_cast<std::int32_t>(::getpid())) {
                if (arg(2) != 0) {
                    const auto port = c_.acquireTaskPort ? c_.acquireTaskPort() : 0U;
                    if (!put(arg(2), &port, sizeof(port))) {
                        fail(EFAULT);
                        return;
                    }
                }
                ok(0);
            } else {
                fail(other_pid_exists(pid) ? EPERM : ESRCH);
            }
            return;
        }
        default: return;
    }
}

RawProcOutcome Dispatcher::run() {
    switch (syscall_) {
        case 20: case 39: case 24: case 25: case 47: case 43: case 81: case 151: case 310:
        case 82: case 147: case 89: case 327: case 215: case 152: case 23: case 183:
        case 181: case 182: case 126: case 127:
            identity();
            break;
        case 79: case 80: case 243: case 287: case 288: case 289: case 290: case 49: case 50:
        case 353: case 354: case 357: case 358: case 350: case 359: case 351: case 428:
        case 429: case 432:
            groups_and_audit();
            break;
        case 96: case 100:
            priority();
            break;
        case 194: case 195: case 117: case 116: case 122: case 140: case 527: case 528:
        case 500: case 142: case 83: case 86: case 186: case 482: case 453: case 534:
        case 531: case 296: case 322: case 274:
            limits_time_info();
            break;
        case 37: case 46: case 48: case 329: case 52: case 53: case 111: case 410: case 330:
        case 422: case 328: case 331: case 332: case 333: case 184:
            signals();
            break;
        case 1: case 2: case 66: case 7: case 400: case 173: case 416: case 59: case 380:
        case 244: case 26: case 38: case 433: case 434: case 435: case 436: case 520:
        case 521: case 529:
            lifecycle();
            break;
        case 51: case 55: case 85: case 155: case 178: case 180: case 185:
        case 285: case 293: case 439: case 440: case 445: case 452: case 454: case 458:
        case 477: case 491: case 492: case 493: case 519: case 532: case 533: case 546:
        case 552: case 556: case 557: case 311: case 323: case 446: case 547: case 548:
        case 456: case 457: case 535: case 494: case 501: case 499: case 502: case 523:
        case 522: case 460: case 490: case 525: case 503: case 504: case 505: case 506:
        case 507: case 508: case 509: case 510: case 511: case 512: case 513: case 514:
        case 373: case 451: case 459: case 545: case 478: case 538: case 539:
            unentitled();
            break;
        default:
            return RawProcOutcome::NotHandled;
    }
    return exit_ ? RawProcOutcome::Exit : RawProcOutcome::Handled;
}

}  // namespace

RawProcOutcome dispatch_raw_proc_syscall(RawProcContext& context, std::uint64_t syscall) {
    if (syscall >= 1000) return RawProcOutcome::NotHandled;
    return Dispatcher(context, syscall).run();
}

}  // namespace arm64metal
