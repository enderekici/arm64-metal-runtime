#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace arm64metal::mach_models {

// Kernel objects that receive modelled MIG requests.
enum class Object { Task, Host, Thread, Clock, Voucher };

// Hooks into the process runner's port namespace and guest memory. The model
// never owns a port name: it asks for one and the runner records the right.
struct Environment {
    std::uint32_t hostName = 0;
    std::uint32_t taskName = 0;
    // The name holds a live send right in the guest namespace.
    std::function<bool(std::uint32_t)> holdsSendRight;
    // A fresh name with no urefs yet; 0 when the namespace is exhausted.
    std::function<std::uint32_t()> allocateName;
    // Adds one send uref to a name.
    std::function<void(std::uint32_t)> acquire;
    // Port name for a guest thread without taking a reference; 0 if none.
    std::function<std::uint32_t(std::uint64_t)> threadName;
    std::function<std::vector<std::uint64_t>()> threadIds;
    // x0..x28, fp, lr, sp, pc, cpsr of a guest thread.
    std::function<bool(std::uint64_t, std::uint64_t*)> threadState;
    std::function<std::uint64_t(std::uint64_t)> threadHandle;
    // All names in the namespace with their mach_port_type bits.
    std::function<std::vector<std::pair<std::uint32_t, std::uint32_t>>()> portTypes;
    // Copies bytes into a fresh guest region and returns its address.
    std::function<bool(const void*, std::size_t, std::uint64_t&)> allocateGuest;
    // mach_port_destroy semantics; returns a kern_return_t.
    std::function<std::int32_t(std::uint32_t)> destroyPort;
    // Guest memory access for traps; null when the range is not accessible.
    std::function<const std::uint8_t*(std::uint64_t, std::size_t)> readGuest;
    std::function<std::uint8_t*(std::uint64_t, std::size_t)> writeGuest;
};

struct Request {
    Object object = Object::Task;
    std::uint32_t remote = 0;
    std::uint64_t threadId = 0;
    std::uint64_t currentThreadId = 0;
    std::int32_t id = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct Reply {
    // Send-side failure returned by mach_msg2 itself; no reply is queued.
    std::uint32_t sendResult = 0;
    // Simple reply: NDR record, return code and extra payload.
    std::int32_t code = 0;
    std::vector<std::uint8_t> extra;
    // Complex reply: everything after the header, descriptors included.
    bool complex = false;
    std::vector<std::uint8_t> body;
    // Names that gain one send uref when the reply is received.
    std::vector<std::uint32_t> rights;
};

enum class TrapOutcome { Unhandled, Handled, Sigsys };

class GuestMachModels {
public:
    explicit GuestMachModels(Environment environment) : env_(std::move(environment)) {}

    [[nodiscard]] bool is_voucher_port(std::uint32_t name) const;

    // Returns true when the request was modelled and `reply` is filled in.
    [[nodiscard]] bool handle(const Request& request, Reply& reply);

    // Mach traps -69..-75. Arguments are read from and the result written to
    // regs[0..7].
    [[nodiscard]] TrapOutcome trap(std::int64_t number, std::uint64_t* regs);

    // proc_info(PROC_INFO_CALL_SETCONTROL, ...); returns an errno.
    [[nodiscard]] std::uint32_t proc_info_setcontrol(
        std::int32_t pid, std::int32_t ownPid, std::uint32_t flavor,
        std::uint64_t buffer, std::uint32_t size,
        std::uint64_t threadId);

private:
    struct Voucher {
        std::map<std::uint32_t, std::vector<std::uint8_t>> userData;
        std::uint32_t name = 0;
    };
    struct ThreadPolicy {
        std::int32_t importance = 0;
        bool timeConstraintSet = false;
        std::int32_t timeConstraint[4] = {0, 120000, 240000, 1};
        std::int32_t latency = 0;
        std::int32_t throughput = 0;
    };

    std::int32_t create_voucher(const std::uint8_t* data, std::size_t size,
                                std::size_t& index);
    std::uint32_t voucher_name(std::size_t index);
    std::size_t voucher_for(std::uint32_t name) const;
    std::vector<std::uint8_t> recipe(std::size_t index, std::uint32_t key) const;

    bool handle_thread(const Request& request, Reply& reply);
    bool handle_task(const Request& request, Reply& reply);
    bool handle_host(const Request& request, Reply& reply);
    bool handle_voucher(const Request& request, Reply& reply);
    void thread_voucher(const Request& request, Reply& reply, bool set);
    void exception_ports(const Request& request, Reply& reply, bool task);
    void port_reply(Reply& reply, std::uint32_t name) const;

    Environment env_;
    // Voucher index 0 is the null voucher.
    std::vector<Voucher> vouchers_ = std::vector<Voucher>(1);
    std::map<std::uint64_t, std::size_t> threadVoucher_;
    std::map<std::uint64_t, ThreadPolicy> threadPolicy_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> threadNames_;
};

}  // namespace arm64metal::mach_models
