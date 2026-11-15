#include "guest_mach_models.hpp"

#include <mach/exception_types.h>

#include <cstring>

namespace arm64metal::mach_models {
namespace {

constexpr std::int32_t kernSuccess = 0;
constexpr std::int32_t kernNoSpace = 3;
constexpr std::int32_t kernInvalidArgument = 4;
constexpr std::int32_t kernFailure = 5;
constexpr std::int32_t kernInvalidCapability = 20;
constexpr std::int32_t kernNotSupported = 46;
constexpr std::uint32_t machSendInvalidDest = 0x10000003U;
constexpr std::uint32_t machSendInvalidRight = 0x1000000aU;
constexpr std::uint32_t kernMemoryError = 10U;
constexpr std::uint32_t machMsgTypeMoveSend = 0x11U;
constexpr std::uint32_t voucherKeyUserData = 7;
constexpr std::uint32_t voucherCommandUserDataStore = 211;
constexpr std::size_t maxRecipeBytes = 5120;
constexpr std::size_t recipeHeader = 16;
constexpr std::int32_t kernProtectionFailure = 2;
constexpr std::int32_t kernDenied = 53;
constexpr std::int32_t migBadArguments = -304;
constexpr std::uint32_t armThreadState64 = 6;
constexpr std::uint32_t armThreadState64Count = 68;
constexpr std::uint32_t threadBasicInfo = 3;
constexpr std::uint32_t threadIdentifierInfo = 4;
constexpr std::uint32_t threadExtendedInfo = 5;
constexpr std::uint32_t threadBasicInfoCount = 10;
constexpr std::uint32_t threadIdentifierInfoCount = 6;
constexpr std::uint32_t threadExtendedInfoCount = 28;
constexpr std::uint32_t exceptionMaskValid = 0x3ffe;

std::uint32_t word(const Request& request, std::size_t offset) {
    std::uint32_t value = 0;
    if (offset + sizeof(value) <= request.size) {
        std::memcpy(&value, request.data + offset, sizeof(value));
    }
    return value;
}

void append32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

void append_bytes_padded(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
    while (out.size() % 4 != 0) out.push_back(0);
}

}  // namespace

bool GuestMachModels::is_voucher_port(std::uint32_t name) const {
    return voucher_for(name) != 0;
}

std::size_t GuestMachModels::voucher_for(std::uint32_t name) const {
    if (name == 0) return 0;
    for (std::size_t index = 1; index < vouchers_.size(); ++index) {
        if (vouchers_[index].name == name && env_.holdsSendRight &&
            env_.holdsSendRight(name)) {
            return index;
        }
    }
    return 0;
}

// Parses a raw recipe array the way the voucher attribute managers do for a
// user-created voucher. Only the user-data manager stores content; the others
// accept the no-op commands and reject the rest. Index 0 is the null voucher.
std::int32_t GuestMachModels::create_voucher(const std::uint8_t* data, std::size_t size,
                                             std::size_t& index) {
    index = 0;
    if (size > maxRecipeBytes) return kernInvalidArgument;
    Voucher candidate;
    std::size_t offset = 0;
    while (offset < size) {
        if (size - offset < recipeHeader) return kernInvalidArgument;
        std::uint32_t key = 0, command = 0, previous = 0, content = 0;
        std::memcpy(&key, data + offset, 4);
        std::memcpy(&command, data + offset + 4, 4);
        std::memcpy(&previous, data + offset + 8, 4);
        std::memcpy(&content, data + offset + 12, 4);
        offset += recipeHeader;
        if (content > size - offset) return kernInvalidArgument;
        const std::uint8_t* bytes = data + offset;
        offset += content;
        (void)previous;
        if (key == voucherKeyUserData && command == voucherCommandUserDataStore) {
            candidate.userData[key].assign(bytes, bytes + content);
            continue;
        }
        if (command == 3) return kernInvalidCapability;
        const bool copy = command == 1 && content == 0;
        const bool remove = command == 2 && content == 0 && key >= 1 && key <= 8;
        const bool importance = key == 2 && command == 10 && content == 0;
        if (!copy && !remove && !importance) return kernInvalidArgument;
    }
    if (candidate.userData.empty()) return kernSuccess;
    for (std::size_t existing = 1; existing < vouchers_.size(); ++existing) {
        if (vouchers_[existing].userData == candidate.userData) {
            index = existing;
            return kernSuccess;
        }
    }
    vouchers_.push_back(std::move(candidate));
    index = vouchers_.size() - 1;
    return kernSuccess;
}

// The voucher port name, allocating a new name when the guest has dropped
// every reference to the previous one. The caller takes the send uref.
std::uint32_t GuestMachModels::voucher_name(std::size_t index) {
    if (index == 0) return 0;
    auto& voucher = vouchers_[index];
    if (voucher.name != 0 && env_.holdsSendRight(voucher.name)) return voucher.name;
    voucher.name = env_.allocateName();
    return voucher.name;
}

std::vector<std::uint8_t> GuestMachModels::recipe(std::size_t index, std::uint32_t key) const {
    std::vector<std::uint8_t> out;
    if (index == 0) return out;
    const auto found = vouchers_[index].userData.find(key);
    if (found == vouchers_[index].userData.end()) return out;
    append32(out, key);
    append32(out, voucherCommandUserDataStore);
    append32(out, 0);
    append32(out, static_cast<std::uint32_t>(found->second.size()));
    out.insert(out.end(), found->second.begin(), found->second.end());
    return out;
}

void GuestMachModels::port_reply(Reply& reply, std::uint32_t name) const {
    reply.complex = true;
    append32(reply.body, 1);
    append32(reply.body, name);
    append32(reply.body, 0);
    append32(reply.body, machMsgTypeMoveSend << 16);
    if (name != 0) reply.rights.push_back(name);
}

TrapOutcome GuestMachModels::trap(std::int64_t number, std::uint64_t* regs) {
    switch (number) {
    case 69:
    case 71:
    case 73:
    case 74:
    case 75:
        return TrapOutcome::Sigsys;
    case 70: {  // host_create_mach_voucher_trap(host, recipes, size, voucher*)
        const auto host = static_cast<std::uint32_t>(regs[0]);
        const auto size = regs[2];
        if (host != env_.hostName || !env_.holdsSendRight(host)) {
            regs[0] = machSendInvalidDest;
            return TrapOutcome::Handled;
        }
        std::vector<std::uint8_t> recipes;
        if (size > maxRecipeBytes) {
            regs[0] = static_cast<std::uint32_t>(kernInvalidArgument);
            return TrapOutcome::Handled;
        }
        if (size != 0) {
            const auto* source = env_.readGuest(regs[1], static_cast<std::size_t>(size));
            if (source == nullptr) {
                regs[0] = kernMemoryError;
                return TrapOutcome::Handled;
            }
            recipes.assign(source, source + size);
        }
        std::size_t index = 0;
        const auto status = create_voucher(recipes.data(), recipes.size(), index);
        if (status != kernSuccess) {
            regs[0] = static_cast<std::uint32_t>(status);
            return TrapOutcome::Handled;
        }
        auto* output = env_.writeGuest(regs[3], sizeof(std::uint32_t));
        if (output == nullptr) {
            regs[0] = kernMemoryError;
            return TrapOutcome::Handled;
        }
        const auto name = voucher_name(index);
        if (name != 0) env_.acquire(name);
        std::memcpy(output, &name, sizeof(name));
        regs[0] = kernSuccess;
        return TrapOutcome::Handled;
    }
    case 72: {  // mach_voucher_extract_attr_recipe_trap(voucher, key, recipe, size*)
        const auto index = voucher_for(static_cast<std::uint32_t>(regs[0]));
        if (index == 0) {
            regs[0] = machSendInvalidDest;
            return TrapOutcome::Handled;
        }
        const auto* sizeSource = env_.readGuest(regs[3], sizeof(std::uint32_t));
        if (sizeSource == nullptr) {
            regs[0] = kernMemoryError;
            return TrapOutcome::Handled;
        }
        std::uint32_t capacity = 0;
        std::memcpy(&capacity, sizeSource, sizeof(capacity));
        const auto bytes = recipe(index, static_cast<std::uint32_t>(regs[1]));
        if (bytes.size() > capacity) {
            regs[0] = static_cast<std::uint32_t>(kernNoSpace);
            return TrapOutcome::Handled;
        }
        if (!bytes.empty()) {
            auto* buffer = env_.writeGuest(regs[2], bytes.size());
            if (buffer == nullptr) {
                regs[0] = kernMemoryError;
                return TrapOutcome::Handled;
            }
            std::memcpy(buffer, bytes.data(), bytes.size());
        }
        auto* sizeOutput = env_.writeGuest(regs[3], sizeof(std::uint32_t));
        if (sizeOutput == nullptr) {
            regs[0] = kernMemoryError;
            return TrapOutcome::Handled;
        }
        const auto produced = static_cast<std::uint32_t>(bytes.size());
        std::memcpy(sizeOutput, &produced, sizeof(produced));
        regs[0] = kernSuccess;
        return TrapOutcome::Handled;
    }
    default:
        return TrapOutcome::Unhandled;
    }
}

bool GuestMachModels::handle(const Request& request, Reply& reply) {
    switch (request.object) {
    case Object::Thread: return handle_thread(request, reply);
    case Object::Task: return handle_task(request, reply);
    case Object::Host: return handle_host(request, reply);
    case Object::Voucher: return handle_voucher(request, reply);
    case Object::Clock: return false;
    }
    return false;
}

// thread_get_mach_voucher 3625, thread_set_mach_voucher 3626 and
// thread_swap_mach_voucher 3627 (which the kernel does not support).
void GuestMachModels::thread_voucher(const Request& request, Reply& reply, bool set) {
    if (!set) {
        const auto held = threadVoucher_.find(request.threadId);
        const auto index = held == threadVoucher_.end() ? std::size_t{0} : held->second;
        port_reply(reply, voucher_name(index));
        return;
    }
    // Header (24), body count (4), port descriptor {name, pad, disposition/type}.
    const auto name = word(request, 28);
    if (name == 0) {
        threadVoucher_.erase(request.threadId);
        return;
    }
    if (!env_.holdsSendRight(name)) {
        reply.sendResult = machSendInvalidRight;
        return;
    }
    const auto index = voucher_for(name);
    if (index == 0) {
        threadVoucher_.erase(request.threadId);
    } else {
        threadVoucher_[request.threadId] = index;
    }
}

bool GuestMachModels::handle_voucher(const Request& request, Reply& reply) {
    const auto index = voucher_for(request.remote);
    if (index == 0) return false;
    const auto key = word(request, 32);
    switch (request.id) {
    case 5400: {  // mach_voucher_extract_attr_content(key, capacity)
        const auto found = vouchers_[index].userData.find(key);
        const auto capacity = word(request, 36);
        const std::vector<std::uint8_t> empty;
        const auto& content = found == vouchers_[index].userData.end() ? empty : found->second;
        if (content.size() > capacity) {
            reply.code = kernNoSpace;
            return true;
        }
        append32(reply.extra, static_cast<std::uint32_t>(content.size()));
        append_bytes_padded(reply.extra, content);
        return true;
    }
    case 5401:    // mach_voucher_extract_attr_recipe(key, capacity)
    case 5402: {  // mach_voucher_extract_all_attr_recipes(capacity)
        const auto capacity = request.id == 5401 ? word(request, 36) : word(request, 32);
        std::vector<std::uint8_t> bytes;
        if (request.id == 5401) {
            bytes = recipe(index, key);
        } else {
            bytes = recipe(index, voucherKeyUserData);
        }
        if (bytes.size() > capacity) {
            reply.code = kernNoSpace;
            return true;
        }
        append32(reply.extra, static_cast<std::uint32_t>(bytes.size()));
        append_bytes_padded(reply.extra, bytes);
        return true;
    }
    case 5403:  // mach_voucher_attr_command
        reply.code = kernFailure;
        return true;
    default:
        return false;
    }
}

bool GuestMachModels::handle_thread(const Request& request, Reply& reply) {
    const auto flavor = word(request, 32);
    const auto count = word(request, 36);
    auto& policy = threadPolicy_[request.threadId];
    switch (request.id) {
    case 3603: {  // thread_get_state(flavor, count)
        if (request.size != 40) return false;
        std::uint64_t state[34] = {};
        if (flavor != armThreadState64 || count < armThreadState64Count) {
            reply.code = kernInvalidArgument;
            return true;
        }
        if (!env_.threadState || !env_.threadState(request.threadId, state)) return false;
        append32(reply.extra, armThreadState64Count);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(state);
        // x0-x28, fp, lr, sp, pc as 64-bit words, then cpsr and flags.
        reply.extra.insert(reply.extra.end(), bytes, bytes + 33 * sizeof(std::uint64_t));
        append32(reply.extra, static_cast<std::uint32_t>(state[33]));
        append32(reply.extra, 0);
        return true;
    }
    case 3612: {  // thread_info(flavor, count)
        if (request.size != 40) return false;
        std::vector<std::uint32_t> info;
        std::uint32_t needed = 0;
        if (flavor == threadBasicInfo) {
            needed = threadBasicInfoCount;
            // user_time, system_time, cpu_usage, policy TIMESHARE, state RUNNING,
            // flags, suspend_count, sleep_time.
            info = {0, 0, 0, 0, 0, 1, 1, 0, 0, 0};
        } else if (flavor == threadIdentifierInfo) {
            needed = threadIdentifierInfoCount;
            const std::uint64_t handle = env_.threadHandle ? env_.threadHandle(request.threadId) : 0;
            const std::uint64_t values[3] = {request.threadId, handle, handle + 0xa0};
            for (const auto value : values) {
                info.push_back(static_cast<std::uint32_t>(value));
                info.push_back(static_cast<std::uint32_t>(value >> 32));
            }
        } else if (flavor == threadExtendedInfo) {
            needed = threadExtendedInfoCount;
            // user/system time, cpu_usage, policy, run_state, flags, sleep, curpri,
            // priority, maxpriority, then the 64-byte thread name.
            info = {0, 0, 0, 0, 0, 1, 1, 0, 0, 31, 31, 63};
            std::vector<std::uint8_t> name(64, 0);
            const auto held = threadNames_.find(request.threadId);
            if (held != threadNames_.end()) {
                std::memcpy(name.data(), held->second.data(), held->second.size());
            }
            for (std::size_t offset = 0; offset < name.size(); offset += 4) {
                std::uint32_t chunk = 0;
                std::memcpy(&chunk, name.data() + offset, 4);
                info.push_back(chunk);
            }
        } else if (flavor == 6) {
            reply.code = kernNotSupported;
            return true;
        } else if (flavor == 99 || flavor == 0) {
            reply.code = kernInvalidArgument;
            return true;
        } else {
            return false;
        }
        if (count < needed) {
            reply.code = kernInvalidArgument;
            return true;
        }
        append32(reply.extra, needed);
        for (const auto value : info) append32(reply.extra, value);
        return true;
    }
    case 3617: {  // thread_policy_set(flavor, count, info[count])
        if (request.size < 40 || count > 16) return false;
        if (request.size != 40 + 4 * static_cast<std::size_t>(count)) {
            reply.code = migBadArguments;
            return true;
        }
        const auto info = [&](std::size_t index) {
            return static_cast<std::int32_t>(word(request, 40 + 4 * index));
        };
        switch (flavor) {
        case 1:  // THREAD_EXTENDED_POLICY
            break;
        case 3:  // THREAD_PRECEDENCE_POLICY
            if (count < 1) reply.code = kernInvalidArgument; else policy.importance = info(0);
            break;
        case 2:  // THREAD_TIME_CONSTRAINT_POLICY
            if (count != 4 || info(1) == 0 || info(2) < info(1)) {
                reply.code = kernInvalidArgument;
            } else {
                policy.timeConstraintSet = true;
                for (std::size_t i = 0; i < 4; ++i) policy.timeConstraint[i] = info(i);
            }
            break;
        case 7:  // THREAD_LATENCY_QOS_POLICY
            if (count < 1) reply.code = kernInvalidArgument; else policy.latency = info(0);
            break;
        case 8:  // THREAD_THROUGHPUT_QOS_POLICY
            if (count < 1) reply.code = kernInvalidArgument; else policy.throughput = info(0);
            break;
        case 4:  // THREAD_AFFINITY_POLICY
            reply.code = kernNotSupported;
            break;
        default:
            reply.code = kernInvalidArgument;
            break;
        }
        return true;
    }
    case 3618: {  // thread_policy_get(flavor, count, get_default)
        if (request.size != 44) return false;
        const bool wantDefault = word(request, 40) != 0;
        std::vector<std::int32_t> info;
        bool isDefault = wantDefault;
        switch (flavor) {
        case 1:
            info = {1};
            break;
        case 2:
            if (policy.timeConstraintSet && !wantDefault) {
                info.assign(policy.timeConstraint, policy.timeConstraint + 4);
            } else {
                info = {0, 120000, 240000, 1};
                isDefault = true;
            }
            break;
        case 3:
            info = {wantDefault ? 0 : policy.importance};
            break;
        case 7:
            info = {wantDefault ? 0 : policy.latency};
            break;
        case 8:
            info = {wantDefault ? 0 : policy.throughput};
            break;
        case 4:
            reply.code = kernNotSupported;
            return true;
        case 6:  // THREAD_POLICY_STATE is restricted; the count is checked first
            reply.code = count < 16 ? kernInvalidArgument : kernProtectionFailure;
            return true;
        case 0:
        case 5:
        case 12:
            reply.code = kernInvalidArgument;
            return true;
        default:
            return false;
        }
        if (count < info.size()) {
            reply.code = kernInvalidArgument;
            return true;
        }
        append32(reply.extra, static_cast<std::uint32_t>(info.size()));
        for (const auto value : info) append32(reply.extra, static_cast<std::uint32_t>(value));
        append32(reply.extra, isDefault ? 1 : 0);
        return true;
    }
    case 3610: {  // thread_get_special_port(which)
        if (request.size != 36) return false;
        const auto which = word(request, 32);
        if (which == 1) {
            reply.code = kernDenied;
        } else if (which == 2 || which == 3) {
            if (!env_.threadName) return false;
            port_reply(reply, env_.threadName(request.threadId));
        } else {
            reply.code = kernInvalidArgument;
        }
        return true;
    }
    case 3614:  // thread_get_exception_ports(mask)
        exception_ports(request, reply, false);
        return true;
    case 3625:
    case 3626:
        thread_voucher(request, reply, request.id == 3626);
        return true;
    case 3627:
        reply.code = kernNotSupported;
        return true;
    default:
        return false;
    }
}

bool GuestMachModels::handle_task(const Request& request, Reply& reply) {
    if (request.id >= 5400 && request.id <= 5403) {
        reply.code = kernInvalidArgument;
        return true;
    }
    const auto flavor = word(request, 32);
    const auto count = word(request, 36);
    switch (request.id) {
    case 3414:  // task_get_exception_ports(mask)
        exception_ports(request, reply, true);
        return true;
    case 3420:  // task_policy_set(flavor, count, info[count])
        if (request.size < 40 || count > 16) return false;
        if (request.size != 40 + 4 * static_cast<std::size_t>(count)) {
            reply.code = migBadArguments;
        } else if (flavor >= 8 && flavor <= 11 && count >= 2) {
            reply.code = kernSuccess;
        } else {
            reply.code = kernInvalidArgument;
        }
        return true;
    case 3421:  // task_policy_get(flavor, count, get_default)
        if (request.size != 44) return false;
        if (flavor == 4) {  // TASK_POLICY_STATE is restricted
            reply.code = count < 16 ? kernInvalidArgument : kernProtectionFailure;
        } else if (flavor == 1 || flavor == 8 || flavor == 9) {
            if (count < 1) {
                reply.code = kernInvalidArgument;
                return true;
            }
            append32(reply.extra, count);
            for (std::uint32_t i = 0; i < count; ++i) append32(reply.extra, 0);
            append32(reply.extra, 0);
        } else {
            reply.code = kernInvalidArgument;
        }
        return true;
    default:
        return false;
    }
}

bool GuestMachModels::handle_host(const Request& request, Reply& reply) {
    if (request.id >= 5400 && request.id <= 5403) {
        reply.code = kernInvalidArgument;
        return true;
    }
    if (request.id == 222) {  // host_create_mach_voucher(recipes)
        const auto count = word(request, 32);
        if (request.size < 36 || count > maxRecipeBytes ||
            request.size != 36 + ((static_cast<std::size_t>(count) + 3) & ~std::size_t{3})) {
            reply.code = -304;  // MIG_BAD_ARGUMENTS
            return true;
        }
        std::size_t index = 0;
        const auto status = create_voucher(request.data + 36, count, index);
        if (status != kernSuccess) {
            reply.code = status;
            return true;
        }
        port_reply(reply, voucher_name(index));
        return true;
    }
    return false;
}

// thread_get_exception_ports / task_get_exception_ports: no handler ports are
// registered; the task keeps the kernel's default behaviour and flavor per
// exception type and merges types that share them.
void GuestMachModels::exception_ports(const Request& request, Reply& reply, bool task) {
    const auto mask = word(request, 32);
    if (request.size != 36 || (mask & ~exceptionMaskValid) != 0) {
        reply.code = kernInvalidArgument;
        return;
    }
    struct Entry {
        std::uint32_t mask;
        std::uint32_t behavior;
        std::uint32_t flavor;
    };
    std::vector<Entry> entries;
    if (task) {
        for (std::uint32_t type = 1; type <= 13; ++type) {
            if ((mask & (1U << type)) == 0) continue;
            const std::uint32_t behavior = type <= 9 ? 0U : type <= 12 ? 1U : 0xa0000004U;
            const std::uint32_t flavor = type <= 9 ? 0U : type <= 12 ? 5U : 1U;
            bool merged = false;
            for (auto& entry : entries) {
                if (entry.behavior == behavior && entry.flavor == flavor) {
                    entry.mask |= 1U << type;
                    merged = true;
                    break;
                }
            }
            if (!merged) entries.push_back({1U << type, behavior, flavor});
        }
    }
    reply.complex = true;
    append32(reply.body, 32);
    for (int i = 0; i < 32; ++i) {
        append32(reply.body, 0);
        append32(reply.body, 0);
        append32(reply.body, machMsgTypeMoveSend << 16);
    }
    append32(reply.body, 0);
    append32(reply.body, 1);
    append32(reply.body, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) append32(reply.body, entry.mask);
    for (const auto& entry : entries) append32(reply.body, entry.behavior);
    for (const auto& entry : entries) append32(reply.body, entry.flavor);
}

// PROC_INFO_CALL_SETCONTROL against the calling process.
std::uint32_t GuestMachModels::proc_info_setcontrol(
    std::int32_t pid, std::int32_t ownPid, std::uint32_t flavor, std::uint64_t buffer,
    std::uint32_t size, std::uint64_t threadId) {
    constexpr std::uint32_t errnoPerm = 1;
    constexpr std::uint32_t errnoFault = 14;
    constexpr std::uint32_t errnoInvalid = 22;
    constexpr std::uint32_t errnoNameTooLong = 63;
    constexpr std::uint32_t errnoNotSupported = 45;
    constexpr std::uint32_t maxThreadNameSize = 64;
    if (pid != ownPid) return errnoInvalid;
    switch (flavor) {
    case 1:  // PROC_SELFSET_PCONTROL
    case 4:  // PROC_SELFSET_DELAYIDLESLEEP
        return 0;
    case 3:  // PROC_SELFSET_VMRSRCOWNER needs an entitlement
        return errnoPerm;
    case 2: {  // PROC_SELFSET_THREADNAME
        if (size >= maxThreadNameSize) return errnoNameTooLong;
        std::vector<std::uint8_t> name;
        if (size != 0) {
            const auto* bytes = env_.readGuest(buffer, size);
            if (bytes == nullptr) return errnoFault;
            for (std::uint32_t i = 0; i < size && bytes[i] != 0; ++i) name.push_back(bytes[i]);
        }
        threadNames_[threadId] = std::move(name);
        return 0;
    }
    default:
        return errnoNotSupported;
    }
}

}  // namespace arm64metal::mach_models
