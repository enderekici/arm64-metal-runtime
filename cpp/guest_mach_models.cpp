#include "guest_mach_models.hpp"

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
    switch (request.id) {
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
    return false;
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

void GuestMachModels::exception_ports(const Request&, Reply&, bool) {}

std::uint32_t GuestMachModels::proc_info_setcontrol(std::int32_t, std::int32_t, std::uint32_t,
                                                    std::uint64_t, std::uint32_t) {
    return 0;
}

}  // namespace arm64metal::mach_models
