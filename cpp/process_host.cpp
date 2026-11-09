#include "process_host.hpp"
#include "darwin_bridge.hpp"
#include "guest_format.hpp"
#include "guest_state.hpp"
#include "host_service_registry.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <charconv>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <ctime>
#include <clocale>
#include <copyfile.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <bootstrap.h>
#include <sys/stat.h>
#include <sys/attr.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <unistd.h>
#include <cwchar>
#include <cstdlib>
#include <cstdio>

// Read-only libsystem_kernel queries; their private header is absent from the SDK.
extern "C" int csr_check(std::uint32_t mask);
extern "C" int csr_get_active_config(std::uint32_t* config);

namespace arm64metal {
namespace {

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right,
                          const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error(message);
    }
    return left + right;
}

std::uint64_t checked_align(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("Guest alignment must be a power of two");
    }
    return checked_add(value, alignment - 1, "Guest address overflow") & ~(alignment - 1);
}

void store_u64(std::vector<std::uint8_t>& memory, std::uint64_t address,
               std::uint64_t value) {
    if (address > memory.size() || memory.size() - static_cast<std::size_t>(address) < 8) {
        throw std::out_of_range("Guest process bootstrap write exceeds memory");
    }
    for (unsigned byte = 0; byte < 8; ++byte) {
        memory[static_cast<std::size_t>(address) + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

void store_guest_string(std::vector<std::uint8_t>& memory, std::uint64_t& cursor,
                        const std::string& value, std::uint64_t& address) {
    const auto length = checked_add(static_cast<std::uint64_t>(value.size()), 1,
                                    "Guest string size overflow");
    if (cursor > memory.size() || length > memory.size() - cursor) {
        throw std::runtime_error("Guest strings exceed stack space");
    }
    address = cursor;
    std::copy(value.begin(), value.end(), memory.begin() + static_cast<std::ptrdiff_t>(cursor));
    memory[static_cast<std::size_t>(cursor + value.size())] = 0;
    cursor += length;
}

std::string basename(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::uint64_t random_word() {
    std::random_device source;
    const std::uint64_t high = source();
    const std::uint64_t low = source();
    return (high << 32U) ^ low;
}

std::string hex_word(std::uint64_t value) {
    char digits[16];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value, 16);
    if (result.ec != std::errc{}) throw std::runtime_error("Unable to format guest entropy");
    return "0x" + std::string(digits, result.ptr);
}

}  // namespace

ProcessBootstrapResult build_process_bootstrap(ProcessBootstrapInput input) {
    if (input.executablePath.empty()) {
        throw std::invalid_argument("Guest executable path is empty");
    }
    if (input.mappingBackingEnd > input.mappedImage.size()) {
        throw std::invalid_argument("Guest stack starts beyond image backing");
    }
    if (input.stackBytes == 0 || input.heapBytes == 0) {
        throw std::invalid_argument("Guest stack and heap sizes must be nonzero");
    }
    if (input.pageSize == 0 || (input.pageSize & (input.pageSize - 1)) != 0) {
        throw std::invalid_argument("Guest page size must be a power of two");
    }
    if (input.initialAuthenticationEntries == 0) input.initialAuthenticationEntries = 1;

    ProcessBootstrapResult result;
    result.memory = std::move(input.mappedImage);
    result.readOnlyRanges = input.readOnlyRanges;
    result.pageSize = input.pageSize;
    result.mainHeader = input.mainHeader;
    result.activePlatform = input.activePlatform;
    result.objectiveCClassCount = input.objectiveCClassCount;
    result.stack.begin = input.mappingBackingEnd;
    result.stack.end = checked_add(result.stack.begin, input.stackBytes,
                                   "Guest stack address overflow");
    result.heap.begin = result.stack.end;
    result.heap.end = checked_add(result.heap.begin, input.heapBytes,
                                  "Guest heap address overflow");
    if (result.heap.end > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("Guest process memory exceeds host address space");
    }
    result.memory.resize(static_cast<std::size_t>(result.heap.end), 0);

    std::vector<std::string> argv;
    argv.reserve(input.arguments.size() + 1);
    auto launch_path = std::filesystem::path(input.executablePath);
    if (!launch_path.is_absolute()) {
        if (launch_path.native().rfind("./", 0) == 0) {
            launch_path = launch_path.native().substr(2);
        }
        launch_path = std::filesystem::current_path() / launch_path;
    }
    result.resolvedExecutablePath = launch_path.string();
    argv.push_back(input.executablePath);
    argv.insert(argv.end(), std::make_move_iterator(input.arguments.begin()),
                std::make_move_iterator(input.arguments.end()));

    std::uint64_t cursor = result.stack.begin;
    std::vector<std::uint64_t> argument_pointers;
    argument_pointers.reserve(argv.size());
    for (const auto& argument : argv) {
        std::uint64_t address = 0;
        store_guest_string(result.memory, cursor, argument, address);
        argument_pointers.push_back(address);
    }
    store_guest_string(result.memory, cursor, basename(input.executablePath), result.programName);

    std::vector<std::uint64_t> environment_pointers;
    environment_pointers.reserve(input.environment.size());
    for (const auto& entry : input.environment) {
        std::uint64_t address = 0;
        store_guest_string(result.memory, cursor, entry, address);
        environment_pointers.push_back(address);
        const auto equal = entry.find('=');
        if (equal != std::string::npos) {
            const auto name = entry.substr(0, equal);
            result.environmentValueAddresses.try_emplace(name, address + equal + 1);
        }
    }

    std::uint64_t pointer_munge_token = 0;
    arc4random_buf(&pointer_munge_token, sizeof(pointer_munge_token));
    if (pointer_munge_token == 0) pointer_munge_token = 1;
    std::vector<std::string> apple_strings = {
        "executable_path=" + result.resolvedExecutablePath,
        "ptr_munge=" + hex_word(pointer_munge_token),
    };
    std::vector<std::uint64_t> apple_pointers;
    apple_pointers.reserve(apple_strings.size());
    for (const auto& entry : apple_strings) {
        std::uint64_t address = 0;
        store_guest_string(result.memory, cursor, entry, address);
        apple_pointers.push_back(address);
    }

    cursor = checked_align(cursor, 8);
    result.vectorBase = cursor;
    result.environmentAddress = checked_add(
        result.vectorBase, static_cast<std::uint64_t>((argument_pointers.size() + 1) * 8),
        "Guest argv vector overflow");
    result.appleAddress = checked_add(
        result.environmentAddress,
        static_cast<std::uint64_t>((environment_pointers.size() + 1) * 8),
        "Guest environment vector overflow");
    result.stdoutSlot = checked_add(
        result.appleAddress, static_cast<std::uint64_t>((apple_pointers.size() + 1) * 8),
        "Guest apple vector overflow");
    result.localeSlot = checked_add(result.stdoutSlot, 8, "Guest stack overflow");
    result.stderrSlot = checked_add(result.localeSlot, 8, "Guest stack overflow");
    result.optindSlot = checked_add(result.stderrSlot, 8, "Guest stack overflow");
    result.environSlot = checked_add(result.optindSlot, 8, "Guest stack overflow");
    result.optargSlot = checked_add(result.environSlot, 8, "Guest stack overflow");
    result.canarySlot = checked_add(result.optargSlot, 8, "Guest stack overflow");
    result.runeLocaleSlot = checked_add(result.canarySlot, 8, "Guest stack overflow");
    result.errnoSlot = checked_add(result.runeLocaleSlot, input.runeLocaleByteCount,
                                   "Guest stack overflow");
    result.stdinSlot = checked_add(result.errnoSlot, 8, "Guest stack overflow");
    result.vmPageSizeSlot = checked_add(result.stdinSlot, 8, "Guest stack overflow");
    result.vmPageShiftSlot = checked_add(result.vmPageSizeSlot, 8, "Guest stack overflow");
    result.vmKernelPageSizeSlot = checked_add(result.vmPageShiftSlot, 8,
                                              "Guest stack overflow");
    result.taskPortSlot = checked_add(result.vmKernelPageSizeSlot, 8,
                                      "Guest stack overflow");
    result.bootstrapPortSlot = checked_add(result.taskPortSlot, 8,
                                          "Guest stack overflow");
    const auto threadLocalPrefixBegin = checked_add(
        checked_add(result.bootstrapPortSlot, 8, "Guest stack overflow"),
        guestThreadLocalPrefixBytes, "Guest TSD prefix overflow");
    result.mainThreadSpecificData = checked_align(threadLocalPrefixBegin, 16);
    std::fill(result.memory.begin() + static_cast<std::ptrdiff_t>(
                  result.mainThreadSpecificData - guestThreadLocalPrefixBytes),
              result.memory.begin() + static_cast<std::ptrdiff_t>(
                  result.mainThreadSpecificData), 0);
    result.argcAddress = checked_add(result.mainThreadSpecificData, 1024 * 8,
                                     "Guest TSD overflow");
    result.argvAddress = checked_add(result.argcAddress, 8, "Guest stack overflow");
    result.scratchAddress = checked_add(result.argvAddress, 8, "Guest stack overflow");
    constexpr std::uint64_t scratch_bytes = 4096 + 6 * sizeof(std::uint64_t);
    const auto scratch_end = checked_add(result.scratchAddress, scratch_bytes,
                                         "Guest scratch overflow");
    constexpr std::uint64_t reserved_stack = 16 * 1024;
    if (result.stack.end < reserved_stack || scratch_end > result.stack.end - reserved_stack) {
        throw std::runtime_error("Guest process vectors leave insufficient stack space");
    }

    for (std::size_t index = 0; index < argument_pointers.size(); ++index) {
        store_u64(result.memory, result.vectorBase + index * 8, argument_pointers[index]);
    }
    for (std::size_t index = 0; index < environment_pointers.size(); ++index) {
        store_u64(result.memory, result.environmentAddress + index * 8,
                  environment_pointers[index]);
    }
    for (std::size_t index = 0; index < apple_pointers.size(); ++index) {
        store_u64(result.memory, result.appleAddress + index * 8, apple_pointers[index]);
    }
    store_u64(result.memory, result.argcAddress, argv.size());
    store_u64(result.memory, result.argvAddress, result.vectorBase);
    store_u64(result.memory, result.mainThreadSpecificData, 1);
    store_u64(result.memory, result.mainThreadSpecificData + 8, result.errnoSlot);
    store_u64(result.memory, result.mainThreadSpecificData + guestMachThreadSlotOffset,
              guestMachThreadName(guestMainThreadID));
    if (input.mainQueue != 0) {
        store_u64(result.memory, result.mainThreadSpecificData + 0xa0, input.mainQueue);
    }
    store_u64(result.memory, result.stdoutSlot, result.stdoutSlot);
    store_u64(result.memory, result.localeSlot, 1);
    store_u64(result.memory, result.stderrSlot, result.stderrSlot);
    store_u64(result.memory, result.stdinSlot, result.stdinSlot);
    store_u64(result.memory, result.vmPageSizeSlot, input.pageSize);
    std::uint64_t page_shift = 0;
    for (auto size = input.pageSize; size > 1; size >>= 1U) ++page_shift;
    store_u64(result.memory, result.vmPageShiftSlot, page_shift);
    store_u64(result.memory, result.vmKernelPageSizeSlot, input.kernelPageSize);
    store_u64(result.memory, result.taskPortSlot, input.guestTaskPort);
    store_u64(result.memory, result.bootstrapPortSlot, guestMachBootstrapPort);
    store_u64(result.memory, result.optindSlot, 1);
    store_u64(result.memory, result.environSlot, result.environmentAddress);
    store_u64(result.memory, result.canarySlot,
              input.randomCanary == 0 ? random_word() | 1U : input.randomCanary);
    if (input.mainQueueState != 0) {
        store_u64(result.memory, input.mainQueueState, input.mainQueueStateValue);
    }

    if (!input.defaultRuneLocaleBytes.empty()) {
        if (input.defaultRuneLocaleBytes.size() > input.runeLocaleByteCount) {
            throw std::invalid_argument("Default rune locale exceeds its guest slot");
        }
        std::copy(input.defaultRuneLocaleBytes.begin(), input.defaultRuneLocaleBytes.end(),
                  result.memory.begin() + static_cast<std::ptrdiff_t>(result.runeLocaleSlot));
    }

    result.modeledGlobalAddresses = {
        {"___stdoutp", result.stdoutSlot},
        {"___mb_cur_max", result.localeSlot},
        {"___stderrp", result.stderrSlot},
        {"___stdinp", result.stdinSlot},
        {"_optind", result.optindSlot},
        {"_optarg", result.optargSlot},
        {"_environ", result.environSlot},
        {"___stack_chk_guard", result.canarySlot},
        {"_vm_page_size", result.vmPageSizeSlot},
        {"_vm_page_shift", result.vmPageShiftSlot},
        {"_vm_kernel_page_size", result.vmKernelPageSizeSlot},
        {"_mach_task_self_", result.taskPortSlot},
        {"_bootstrap_port", result.bootstrapPortSlot},
    };
    if (!input.defaultRuneLocaleBytes.empty()) {
        result.modeledGlobalAddresses.emplace("__DefaultRuneLocale", result.runeLocaleSlot);
    }

    if (input.initialAuthenticationEntries >
        (std::numeric_limits<std::size_t>::max() - guestTokenStart) / 3) {
        throw std::length_error("Guest authentication table exceeds host address space");
    }
    const auto register_count = guestTokenStart + 3 * input.initialAuthenticationEntries;
    if (register_count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) {
        throw std::length_error("Guest register table exceeds host address space");
    }
    result.registers.assign(register_count, 0);
    result.registers[0] = argv.size();
    result.registers[1] = result.vectorBase;
    result.registers[2] = result.environmentAddress;
    result.registers[3] = result.appleAddress;
    result.registers[30] = std::numeric_limits<std::uint64_t>::max();
    result.registers[31] = result.stack.end;
    result.registers[33] = input.entry;
    result.registers[guestPACGAKey] = input.randomPACGAKey0 == 0 ? random_word()
                                                                 : input.randomPACGAKey0;
    result.registers[guestPACGAKey + 1] = input.randomPACGAKey1 == 0 ? random_word()
                                                                     : input.randomPACGAKey1;
    result.registers[guestTokenCapacity] = input.initialAuthenticationEntries;
    result.registers[guestFaultAddress] = std::numeric_limits<std::uint64_t>::max();
    result.registers[guestCacheFaultAddress] = std::numeric_limits<std::uint64_t>::max();
    return result;
}

GuestMemory::GuestMemory(std::vector<std::uint8_t> bytes,
                         std::vector<GuestMemoryRange> readOnlyRanges,
                         std::vector<GuestMemoryMapping> mappings)
    : bytes_(std::move(bytes)), readOnlyRanges_(std::move(readOnlyRanges)),
      mappings_(std::move(mappings)) {
    size_ = bytes_.size();
    for (const auto& range : readOnlyRanges_) {
        if (range.begin > range.end || range.end > size_) {
            throw std::invalid_argument("Invalid guest read-only range");
        }
    }
    for (const auto& mapping : mappings_) {
        if (mapping.logicalBegin >= mapping.logicalEnd || mapping.backingBegin > size_ ||
            mapping.logicalEnd - mapping.logicalBegin > size_ - mapping.backingBegin) {
            throw std::invalid_argument("Invalid guest memory mapping");
        }
    }
}

GuestMemory::GuestMemory(std::uint8_t* bytes, std::size_t size,
                         std::vector<GuestMemoryRange> readOnlyRanges,
                         std::vector<GuestMemoryMapping> mappings)
    : data_(bytes), size_(size), external_(true), readOnlyRanges_(std::move(readOnlyRanges)),
      mappings_(std::move(mappings)) {
    if (size_ != 0 && data_ == nullptr) {
        throw std::invalid_argument("Nonempty guest memory view requires backing storage");
    }
    for (const auto& range : readOnlyRanges_) {
        if (range.begin > range.end || range.end > size_) {
            throw std::invalid_argument("Invalid guest read-only range");
        }
    }
    for (const auto& mapping : mappings_) {
        if (mapping.logicalBegin >= mapping.logicalEnd || mapping.backingBegin > size_ ||
            mapping.logicalEnd - mapping.logicalBegin > size_ - mapping.backingBegin) {
            throw std::invalid_argument("Invalid guest memory mapping");
        }
    }
}

std::size_t GuestMemory::size() const noexcept { return size_; }

void GuestMemory::rebind(std::uint8_t* bytes, std::size_t size) {
    if (!external_ || bytes == nullptr || size == 0) {
        throw std::invalid_argument("Guest memory rebind requires a nonempty borrowed buffer");
    }
    data_ = bytes;
    size_ = size;
}

void GuestMemory::rebind_with_mapping(std::uint8_t* bytes, std::size_t size,
                                      GuestMemoryMapping mapping) {
    if (!external_ || bytes == nullptr || size == 0) {
        throw std::invalid_argument("Guest memory rebind requires a nonempty borrowed buffer");
    }
    if (mapping.logicalBegin >= mapping.logicalEnd || mapping.backingBegin > size ||
        mapping.logicalEnd - mapping.logicalBegin > size - mapping.backingBegin) {
        throw std::invalid_argument("Invalid guest memory mapping for rebind");
    }
    for (const auto& current : mappings_) {
        if (mapping.logicalBegin < current.logicalEnd &&
            current.logicalBegin < mapping.logicalEnd) {
            throw std::invalid_argument("Replacement guest memory mapping overlaps");
        }
    }
    auto stagedMappings = mappings_;
    stagedMappings.push_back(mapping);
    data_ = bytes;
    size_ = size;
    mappings_.swap(stagedMappings);
}

void GuestMemory::validate_logical_interval(std::uint64_t address,
                                            std::uint64_t length) const {
    if (length > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::out_of_range("Guest memory interval overflows");
    // A contiguous host pointer cannot represent an access spanning distinct
    // logical mappings, even when both happen to share physical backing.
    for (const auto& mapping : mappings_) {
        if (address < mapping.logicalBegin && length > mapping.logicalBegin - address)
            throw std::out_of_range("Guest memory interval crosses a mapping boundary");
    }
}

std::uint8_t* GuestMemory::buffer(std::uint64_t address, std::uint64_t length,
                                 bool writable) {
    validate_logical_interval(address, length);
    const auto find_mapping = [&]() -> const GuestMemoryMapping* {
        for (const auto& mapping : mappings_) {
            if (address >= mapping.logicalBegin && address < mapping.logicalEnd) return &mapping;
        }
        return nullptr;
    };
    auto* mapping = find_mapping();
    if (mapping == nullptr && address > size_ && mappingResolver_ &&
        mappingResolver_(address, length, writable)) {
        mapping = find_mapping();
        validate_logical_interval(address, length);
    }
    const bool mapped = mapping != nullptr;
    std::uint64_t backing = address;
    if (mapping != nullptr) {
        if (!mapping->readable || length > mapping->logicalEnd - address) {
            throw std::out_of_range("Guest memory buffer is out of bounds");
        }
        if (writable && !mapping->writable) {
            throw std::out_of_range("Guest memory write targets a read-only mapping");
        }
        backing = mapping->backingBegin + (address - mapping->logicalBegin);
    }
    if (backing > size_ || length > size_ - static_cast<std::size_t>(backing)) {
        throw std::out_of_range("Guest memory buffer is out of bounds");
    }
    if (!mapped && address > size_) {
        throw std::out_of_range("Guest memory buffer is out of bounds");
    }
    if (writable && length != 0) {
        const GuestMemoryRange requested{backing, backing + length};
        for (const auto& range : readOnlyRanges_) {
            if (requested.begin < range.end && range.begin < requested.end) {
                throw std::out_of_range("Guest memory write targets a read-only range");
            }
        }
    }
    auto* data = external_ ? data_ : bytes_.data();
    return data + static_cast<std::size_t>(backing);
}

const std::uint8_t* GuestMemory::buffer(std::uint64_t address, std::uint64_t length) const {
    validate_logical_interval(address, length);
    const auto find_mapping = [&]() -> const GuestMemoryMapping* {
        for (const auto& mapping : mappings_) {
            if (address >= mapping.logicalBegin && address < mapping.logicalEnd) return &mapping;
        }
        return nullptr;
    };
    auto* mapping = find_mapping();
    if (mapping == nullptr && address > size_ && mappingResolver_ &&
        mappingResolver_(address, length, false)) {
        mapping = find_mapping();
        validate_logical_interval(address, length);
    }
    const bool mapped = mapping != nullptr;
    std::uint64_t backing = address;
    if (mapping != nullptr) {
        if (!mapping->readable || length > mapping->logicalEnd - address) {
            throw std::out_of_range("Guest memory buffer is out of bounds");
        }
        backing = mapping->backingBegin + (address - mapping->logicalBegin);
    }
    if (backing > size_ || length > size_ - static_cast<std::size_t>(backing) ||
        (!mapped && address > size_)) {
        throw std::out_of_range("Guest memory buffer is out of bounds");
    }
    const auto* data = external_ ? data_ : bytes_.data();
    return data + static_cast<std::size_t>(backing);
}

std::uint64_t GuestMemory::contiguous_readable_size(std::uint64_t address) const {
    for (const auto& mapping : mappings_) {
        if (address >= mapping.logicalBegin && address < mapping.logicalEnd) {
            if (!mapping.readable) throw std::out_of_range("Guest memory is not readable");
            return mapping.logicalEnd - address;
        }
    }
    if (address > size_ && mappingResolver_ && mappingResolver_(address, 1, false)) {
        for (const auto& mapping : mappings_) {
            if (address >= mapping.logicalBegin && address < mapping.logicalEnd) {
                if (!mapping.readable) throw std::out_of_range("Guest memory is not readable");
                return mapping.logicalEnd - address;
            }
        }
    }
    if (address > size_) throw std::out_of_range("Guest memory address is out of bounds");
    auto end = static_cast<std::uint64_t>(size_);
    for (const auto& mapping : mappings_)
        if (mapping.logicalBegin > address) end = std::min(end, mapping.logicalBegin);
    return end - address;
}

bool GuestMemory::is_guest_readable(std::uint64_t address,
                                    std::uint64_t length) const noexcept {
    if (length > std::numeric_limits<std::uint64_t>::max() - address) return false;
    for (const auto& mapping : mappings_) {
        if (address >= mapping.logicalBegin && address < mapping.logicalEnd) {
            return mapping.readable && length <= mapping.logicalEnd - address;
        }
    }
    return std::any_of(guestRanges_.begin(), guestRanges_.end(),
        [address, length](const GuestMemoryRange& range) {
            return address >= range.begin && address < range.end &&
                length <= range.end - address;
        });
}

void GuestMemory::add_guest_range(GuestMemoryRange range) {
    if (range.begin >= range.end || range.end > size_) {
        throw std::invalid_argument("Invalid guest mapped range");
    }
    guestRanges_.push_back(range);
}

bool GuestMemory::is_readonly(std::uint64_t address, std::uint64_t length) const {
    try {
        validate_logical_interval(address, length);
    } catch (const std::out_of_range&) {
        return false;
    }
    auto find_mapping = [&]() -> const GuestMemoryMapping* {
        for (const auto& mapping : mappings_) {
            if (address >= mapping.logicalBegin && address < mapping.logicalEnd) return &mapping;
        }
        return nullptr;
    };
    auto* mapping = find_mapping();
    if (mapping == nullptr && address > size_ && mappingResolver_ &&
        mappingResolver_(address, length, false)) {
        mapping = find_mapping();
        try {
            validate_logical_interval(address, length);
        } catch (const std::out_of_range&) {
            return false;
        }
    }
    if (mapping != nullptr) {
        return mapping->readable && length <= mapping->logicalEnd - address && !mapping->writable;
    }
    if (address > size_ || length > size_ - static_cast<std::size_t>(address)) return false;
    const GuestMemoryRange requested{address, address + length};
    return std::any_of(readOnlyRanges_.begin(), readOnlyRanges_.end(),
        [&](const GuestMemoryRange& range) {
            return requested.begin >= range.begin && requested.end <= range.end;
        });
}

bool GuestMemory::is_protection_fault(std::uint64_t address) const {
    for (const auto& mapping : mappings_) {
        if (address >= mapping.logicalBegin && address < mapping.logicalEnd) {
            return !mapping.readable || !mapping.writable;
        }
    }
    return is_readonly(address, 1);
}

void GuestMemory::add_mapping(GuestMemoryMapping mapping) {
    if (mapping.logicalBegin >= mapping.logicalEnd || mapping.backingBegin > size_ ||
        mapping.logicalEnd - mapping.logicalBegin > size_ - mapping.backingBegin) {
        throw std::invalid_argument("Invalid guest memory mapping");
    }
    for (const auto& existing : mappings_) {
        if (mapping.logicalBegin < existing.logicalEnd &&
            existing.logicalBegin < mapping.logicalEnd) {
            throw std::invalid_argument("Guest memory mapping overlaps an existing mapping");
        }
    }
    mappings_.push_back(mapping);
}

void GuestMemory::replace_mappings(
    const std::vector<std::uint64_t>& logicalBegins,
    const std::vector<GuestMemoryMapping>& replacements) {
    auto staged = mappings_;
    for (const auto logicalBegin : logicalBegins) {
        const auto found = std::find_if(staged.begin(), staged.end(),
            [logicalBegin](const GuestMemoryMapping& mapping) {
                return mapping.logicalBegin == logicalBegin;
            });
        if (found == staged.end()) {
            throw std::out_of_range("Guest memory mapping to replace was not found");
        }
        staged.erase(found);
    }
    for (const auto& mapping : replacements) {
        if (mapping.logicalBegin >= mapping.logicalEnd || mapping.backingBegin > size_ ||
            mapping.logicalEnd - mapping.logicalBegin > size_ - mapping.backingBegin) {
            throw std::invalid_argument("Invalid replacement guest memory mapping");
        }
        for (const auto& existing : staged) {
            if (mapping.logicalBegin < existing.logicalEnd &&
                existing.logicalBegin < mapping.logicalEnd) {
                throw std::invalid_argument("Replacement guest memory mapping overlaps");
            }
        }
        staged.push_back(mapping);
    }
    mappings_.swap(staged);
}

void GuestMemory::remove_mapping(std::uint64_t logicalBegin) {
    const auto found = std::find_if(mappings_.begin(), mappings_.end(),
        [&](const GuestMemoryMapping& mapping) {
            return mapping.logicalBegin == logicalBegin;
        });
    if (found != mappings_.end()) mappings_.erase(found);
}

void GuestMemory::set_mapping_permissions(std::uint64_t logicalBegin, bool writable) {
    set_mapping_permissions(logicalBegin, true, writable);
}

void GuestMemory::set_mapping_permissions(std::uint64_t logicalBegin, bool readable,
                                          bool writable) {
    const auto found = std::find_if(mappings_.begin(), mappings_.end(),
        [&](const GuestMemoryMapping& mapping) {
            return mapping.logicalBegin == logicalBegin;
        });
    if (found == mappings_.end()) throw std::out_of_range("Guest memory mapping not found");
    found->readable = readable;
    found->writable = writable;
}

void GuestMemory::set_mapping_resolver(GuestMemoryMappingResolver resolver) {
    mappingResolver_ = std::move(resolver);
}

void GuestMemory::protect_readonly(GuestMemoryRange range) {
    if (range.begin > range.end || range.end > size_) {
        throw std::invalid_argument("Invalid guest read-only range");
    }
    readOnlyRanges_.push_back(range);
}

void GuestMemory::unprotect_readonly(GuestMemoryRange range) {
    const auto found = std::find_if(readOnlyRanges_.begin(), readOnlyRanges_.end(),
        [&](const GuestMemoryRange& existing) {
            return existing.begin == range.begin && existing.end == range.end;
        });
    if (found != readOnlyRanges_.end()) readOnlyRanges_.erase(found);
}

HostServiceState::~HostServiceState() {
    for (const auto& right : guestBootstrapSendRights) {
        (void)mach_port_deallocate(mach_task_self(), right.second);
    }
    for (const auto& file : files) std::fclose(file.second);
    for (const auto& mapping : mappedFiles) {
        if (mapping.second.hostAddress != nullptr) {
            ::munmap(mapping.second.hostAddress, static_cast<std::size_t>(mapping.second.size));
        }
    }
    for (const auto descriptor : openDescriptors) ::close(descriptor);
}

void HostServiceState::ingest_host_signals() {
    auto incoming = guest_signal_take();
    signals.ingest(incoming);
    while (incoming != 0) {
        const auto bit = incoming & (~incoming + 1U);
        incoming &= incoming - 1U;
        const auto signal = static_cast<std::int32_t>(__builtin_ctz(bit) + 1);
        if (GuestSignals::valid(signal) && signals.action(signal).handler != 1) {
            queuedProcessSignals |= bit;
        }
    }
}

HostServiceResult dispatch_guest_host_service(
    std::vector<std::uint64_t>& registers, GuestMemory& memory,
    const ProcessBootstrapResult& bootstrap, HostServiceState& state,
    std::uint64_t service) {
    if (registers.size() < 36) {
        throw std::invalid_argument("Guest host service requires a complete register file");
    }
    const auto string_at = [&](std::uint64_t address) {
        const auto extent = memory.contiguous_readable_size(address);
        if (extent == 0) throw std::out_of_range("Guest string pointer is invalid");
        const auto* bytes = memory.buffer(address, extent);
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(
            bytes, 0, static_cast<std::size_t>(extent)));
        if (end == nullptr) throw std::out_of_range("Guest string is not terminated");
        return std::string(reinterpret_cast<const char*>(bytes),
                            static_cast<std::size_t>(end - bytes));
    };
    const auto load_u64 = [&](std::uint64_t address) {
        std::uint64_t value = 0;
        std::memcpy(&value, memory.buffer(address, sizeof(value)), sizeof(value));
        return value;
    };
    const auto write_all = [](int descriptor, const std::uint8_t* bytes,
                              std::size_t length) {
        std::size_t written = 0;
        while (written < length) {
            const auto result = ::write(descriptor, bytes + written, length - written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) return written == 0 ? -1 : static_cast<std::int64_t>(written);
            written += static_cast<std::size_t>(result);
        }
        return static_cast<std::int64_t>(written);
    };
    const auto failure = static_cast<std::uint64_t>(-1);
    const auto set_errno = [&](int value) {
        if (bootstrap.errnoSlot != 0) {
            const auto guest_error = static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
            std::memcpy(memory.buffer(bootstrap.errnoSlot, sizeof(guest_error), true),
                        &guest_error, sizeof(guest_error));
        }
    };
    const auto standard_descriptor_open = [&](std::int32_t descriptor) {
        if (descriptor == STDIN_FILENO) return !state.closedStreams.count(bootstrap.stdinSlot);
        if (descriptor == STDOUT_FILENO) return !state.closedStreams.count(bootstrap.stdoutSlot);
        if (descriptor == STDERR_FILENO) return !state.closedStreams.count(bootstrap.stderrSlot);
        return false;
    };
    const auto owns_descriptor = [&](std::int32_t descriptor) {
        return state.openDescriptors.count(descriptor) != 0;
    };
    const auto owns_directory_for = [&](std::int32_t descriptor,
                                        const std::string& path) {
        return (!path.empty() && path.front() == '/') || descriptor == AT_FDCWD ||
               owns_descriptor(descriptor);
    };
    const auto ready_for_io = [](std::int32_t descriptor, short events) {
        pollfd item{descriptor, events, 0};
        const auto result = ::poll(&item, 1, 0);
        return result > 0 && (item.revents & events) != 0;
    };
    const auto status_flags = [&](std::int32_t descriptor) {
        if (const auto found = state.standardStatusFlags.find(descriptor);
            found != state.standardStatusFlags.end()) return found->second;
        return ::fcntl(descriptor, F_GETFL);
    };
    const auto normalize_stream = [&](std::uint64_t stream) {
        if (const auto alias = state.standardFileAliases.find(stream);
            alias != state.standardFileAliases.end()) return alias->second;
        constexpr std::uint64_t addressMask = 0x00ffffffffffffffULL;
        const auto untagged = stream & addressMask;
        for (const auto& alias : state.standardFileAliases) {
            if ((alias.first & addressMask) == untagged) return alias.second;
        }
        return stream;
    };
    const auto stream_file = [&](std::uint64_t stream) -> std::FILE* {
        stream = normalize_stream(stream);
        const auto found = state.files.find(stream);
        if (found != state.files.end()) return found->second;
        if (stream == bootstrap.stdinSlot && !state.closedStreams.count(stream)) return stdin;
        if (stream == bootstrap.stdoutSlot && !state.closedStreams.count(stream)) return stdout;
        if (stream == bootstrap.stderrSlot && !state.closedStreams.count(stream)) return stderr;
        return nullptr;
    };
    const auto guest_heap = [&]() -> GuestHeap& {
        if (!state.heap) {
            if (bootstrap.heap.end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::length_error("Guest heap address exceeds the allocator range");
            }
            state.heapEnd = bootstrap.heap.end;
            state.heap.emplace(GuestRange{static_cast<std::int64_t>(bootstrap.heap.begin),
                                          static_cast<std::int64_t>(bootstrap.heap.end)});
        }
        return *state.heap;
    };
    const auto allocate_guest = [&](std::uint64_t size, std::int64_t alignment = 16)
        -> std::optional<std::int64_t> {
        auto& heap = guest_heap();
        auto allocation = heap.allocate(size, alignment);
        if (allocation || !state.growGuestMemory) return allocation;
        const auto extra = std::max<std::uint64_t>(size, static_cast<std::uint64_t>(alignment));
        if (extra > std::numeric_limits<std::size_t>::max() - memory.size()) return std::nullopt;
        const auto minimum = memory.size() + static_cast<std::size_t>(extra);
        if (!state.growGuestMemory(memory, minimum) || memory.size() <= state.heapEnd ||
            memory.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        heap.extend({static_cast<std::int64_t>(state.heapEnd),
                     static_cast<std::int64_t>(memory.size())});
        state.heapEnd = memory.size();
        return heap.allocate(size, alignment);
    };
    const auto is_standard_stream = [&](std::uint64_t address) {
        address = normalize_stream(address);
        return address == bootstrap.stdinSlot || address == bootstrap.stdoutSlot ||
               address == bootstrap.stderrSlot;
    };
    const auto host_signal_supported = [](std::int32_t signal) {
        return signal == SIGHUP || signal == SIGINT || signal == SIGQUIT || signal == SIGPIPE ||
               signal == SIGALRM || signal == SIGTERM || signal == SIGUSR1 || signal == SIGUSR2 ||
               signal == SIGINFO;
    };
    const auto synchronous_signal_supported = [](std::int32_t signal) {
        return signal == SIGSEGV || signal == SIGBUS || signal == SIGILL ||
               signal == SIGTRAP;
    };
    const auto guest_format = [&](std::uint64_t address) {
        const auto text = string_at(address);
        return std::vector<std::uint8_t>(text.begin(), text.end());
    };
    const auto guest_string_reader = [&](std::uint64_t address) {
        const auto text = string_at(address);
        return std::vector<std::uint8_t>(text.begin(), text.end());
    };
    const auto format_with_arguments = [&](std::uint64_t formatAddress,
                                           std::size_t firstRegister,
                                           std::uint64_t stackAddress,
                                           bool useVaList) {
        const auto format = guest_format(formatAddress);
        std::size_t registerIndex = firstRegister;
        auto stackOffset = stackAddress;
        return format_guest_output(format, [&]() {
            if (!useVaList && registerIndex <= 7) return registers[registerIndex++];
            const auto value = load_u64(stackOffset);
            stackOffset += sizeof(std::uint64_t);
            return value;
        }, guest_string_reader);
    };
    switch (service) {
        case 183: {  // bootstrap_look_up (restricted, read-only service discovery)
            constexpr std::uint32_t bootstrapUnknownService = BOOTSTRAP_UNKNOWN_SERVICE;
            std::uint8_t* output = nullptr;
            try {
                output = memory.buffer(registers[2], sizeof(std::uint32_t), true);
            } catch (const std::exception&) {
                registers[0] = 1;  // KERN_INVALID_ADDRESS
                break;
            }
            const std::uint32_t nullPort = MACH_PORT_NULL;
            std::memcpy(output, &nullPort, sizeof(nullPort));
            if (registers[0] != guestMachBootstrapPort) {
                registers[0] = BOOTSTRAP_NOT_PRIVILEGED;
                break;
            }
            std::string name;
            try {
                name = string_at(registers[1]);
            } catch (const std::exception&) {
                registers[0] = 1;  // KERN_INVALID_ADDRESS
                break;
            }
            if (name != "com.apple.windowserver.active") {
                registers[0] = bootstrapUnknownService;
                break;
            }
            if (!state.allocateGuestBootstrapSendRight) {
                registers[0] = BOOTSTRAP_NO_MEMORY;
                break;
            }
            mach_port_t hostPort = MACH_PORT_NULL;
            const auto lookup = ::bootstrap_look_up(::bootstrap_port, name.c_str(), &hostPort);
            if (lookup != BOOTSTRAP_SUCCESS) {
                if (hostPort != MACH_PORT_NULL) {
                    (void)mach_port_deallocate(mach_task_self(), hostPort);
                }
                registers[0] = static_cast<std::uint32_t>(lookup);
                break;
            }
            const auto guestName = state.allocateGuestBootstrapSendRight(
                static_cast<std::uint32_t>(hostPort));
            if (!guestName) {
                registers[0] = BOOTSTRAP_NO_MEMORY;
                break;
            }
            std::memcpy(output, &*guestName, sizeof(*guestName));
            registers[0] = BOOTSTRAP_SUCCESS;
            break;
        }
        case 1: {  // puts
            const auto value = string_at(registers[0]);
            const auto first = write_all(STDOUT_FILENO,
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
            const std::uint8_t newline = '\n';
            const auto second = first < 0 ? -1 : write_all(STDOUT_FILENO, &newline, 1);
            registers[0] = first < 0 || second < 0 ? failure : 0;
            break;
        }
        case 2: {  // write
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            const bool open_standard = standard_descriptor_open(descriptor);
            const bool standard_output = open_standard &&
                (descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO);
            if (!standard_output &&
                (!owns_descriptor(descriptor) || !state.writableDescriptors.count(descriptor))) {
                registers[0] = failure;
                set_errno(EBADF);
                break;
            }
            if (registers[2] > std::numeric_limits<std::size_t>::max()) {
                registers[0] = failure;
                set_errno(EINVAL);
                break;
            }
            const auto count = (status_flags(descriptor) & O_NONBLOCK) != 0
                ? std::min<std::uint64_t>(registers[2], PIPE_BUF) : registers[2];
            const std::uint8_t* source = nullptr;
            if (registers[2] != 0) {
                if (registers[1] < bootstrap.pageSize) {
                    registers[0] = failure;
                    set_errno(EFAULT);
                    break;
                }
                try {
                    source = memory.buffer(registers[1], registers[2]);
                } catch (const std::out_of_range&) {
                    registers[0] = failure;
                    set_errno(EFAULT);
                    break;
                }
            }
            if (registers[2] != 0 && (status_flags(descriptor) & O_NONBLOCK) != 0 &&
                !ready_for_io(descriptor, POLLOUT)) {
                set_errno(EAGAIN);
                registers[0] = failure;
                break;
            }
            const auto result = ::write(descriptor, source, static_cast<std::size_t>(count));
            registers[0] = result < 0 ? failure : static_cast<std::uint64_t>(result);
            if (result < 0) set_errno(errno);
            break;
        }
        case 3:  // exit
            return {true, static_cast<std::int32_t>(registers[0])};
        case 4: {  // getenv
            const auto name = string_at(registers[0]);
            const auto found = bootstrap.environmentValueAddresses.find(name);
            registers[0] = found == bootstrap.environmentValueAddresses.end() ? 0 : found->second;
            break;
        }
        case 5:  // strlen
            registers[0] = string_at(registers[0]).size();
            break;
        case 153: {  // strdup
            const auto value = string_at(registers[0]);
            const auto length = value.size() + 1;
            const auto allocation = allocate_guest(length);
            if (!allocation) {
                set_errno(ENOMEM);
                registers[0] = 0;
                break;
            }
            const auto address = static_cast<std::uint64_t>(*allocation);
            auto* destination = memory.buffer(address, length, true);
            std::memcpy(destination, value.c_str(), length);
            registers[0] = address;
            break;
        }
        case 154: {  // asprintf
            const auto outputSlot = registers[0];
            const auto formatAddress = registers[1];
            (void)memory.buffer(outputSlot, sizeof(std::uint64_t), true);
            const auto output = format_with_arguments(formatAddress, 8, registers[31], false);
            if (output.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                output.size() == std::numeric_limits<std::size_t>::max()) {
                set_errno(EOVERFLOW);
                registers[0] = failure;
                break;
            }
            const auto allocation = allocate_guest(output.size() + 1);
            if (!allocation) {
                set_errno(ENOMEM);
                registers[0] = failure;
                break;
            }
            const auto address = static_cast<std::uint64_t>(*allocation);
            auto* destination = memory.buffer(address, output.size() + 1, true);
            if (!output.empty()) std::memcpy(destination, output.data(), output.size());
            destination[output.size()] = 0;
            std::memcpy(memory.buffer(outputSlot, sizeof(address), true), &address,
                        sizeof(address));
            registers[0] = output.size();
            break;
        }
        case 157: {  // compat_mode
            const auto function = string_at(registers[0]);
            const auto requested = string_at(registers[1]);
            const auto equals_case_insensitively = [](const std::string& left,
                                                       const std::string& right) {
                if (left.size() != right.size()) return false;
                for (std::size_t index = 0; index < left.size(); ++index) {
                    const auto lower_ascii = [](unsigned char value) {
                        return value >= 'A' && value <= 'Z'
                            ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
                    };
                    if (lower_ascii(static_cast<unsigned char>(left[index])) !=
                        lower_ascii(static_cast<unsigned char>(right[index]))) return false;
                }
                return true;
            };
            const auto modeVariable = bootstrap.environmentValueAddresses.find("COMMAND_MODE");
            const auto active = modeVariable == bootstrap.environmentValueAddresses.end()
                ? std::string("Unix2003") : string_at(modeVariable->second);
            const bool activeLegacy = equals_case_insensitively(active, "Legacy");
            const bool activeUnix2003 = !activeLegacy;
            if (equals_case_insensitively(requested, "Legacy")) {
                registers[0] = activeLegacy ? 1 : 0;
            } else if (equals_case_insensitively(requested, "Unix2003")) {
                registers[0] = activeUnix2003 ? 1 : 0;
            } else {
                const auto diagnostic = "invalid mode " + requested +
                    " (while checking for " + function + ")\n";
                (void)write_all(STDERR_FILENO,
                    reinterpret_cast<const std::uint8_t*>(diagnostic.data()), diagnostic.size());
                registers[0] = 0;
            }
            break;
        }
        case 6: {  // putchar
            const auto byte = static_cast<std::uint8_t>(registers[0]);
            const auto result = write_all(STDOUT_FILENO, &byte, 1);
            registers[0] = result < 0 ? failure : byte;
            break;
        }
        case 7: {  // putwchar; the guest starts in the initial C locale
            std::mbstate_t state{};
            char encoded[32]{};
            const auto length = std::wcrtomb(encoded,
                static_cast<wchar_t>(static_cast<std::int32_t>(registers[0])), &state);
            if (length == static_cast<std::size_t>(-1) || length > sizeof(encoded)) {
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            (void)write_all(STDOUT_FILENO,
                reinterpret_cast<const std::uint8_t*>(encoded), length);
            break;
        }
        case 8: {  // mbtowc
            if (registers[0] % alignof(wchar_t) != 0) {
                throw std::runtime_error("Guest mbtowc output is misaligned");
            }
            if (registers[1] == 0 || registers[1] >= memory.size() || registers[2] == 0) {
                throw std::runtime_error("Guest mbtowc arguments unsupported");
            }
            const auto available = memory.size() - static_cast<std::size_t>(registers[1]);
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(registers[2], available));
            const auto* source = reinterpret_cast<const char*>(memory.buffer(registers[1], length));
            auto* destination = memory.buffer(registers[0], sizeof(wchar_t), true);
            wchar_t output = 0;
            const auto result = std::mbtowc(&output, source, length);
            if (result >= 0) std::memcpy(destination, &output, sizeof(output));
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 9:  // fflush: the bridge writes immediately, so no buffering remains
            registers[0] = normalize_stream(registers[0]);
            if (registers[0] != 0 && !is_standard_stream(registers[0])) {
                throw std::runtime_error("Guest fflush stream unsupported");
            }
            registers[0] = 0;
            break;
        case 10:  // getopt
        case 54: {  // getopt_long
            const auto argc = static_cast<std::size_t>(std::min<std::uint64_t>(
                registers[0], std::numeric_limits<std::size_t>::max()));
            const auto argv = registers[1];
            const auto options = string_at(registers[2]);
            registers[0] = std::numeric_limits<std::uint32_t>::max();
            bool missing_argument = false;
            char diagnosed_option = '?';
            if (service == 54 && state.optionOffset == 1 && state.optionIndex < argc) {
                const auto argument_pointer = load_u64(
                    argv + state.optionIndex * sizeof(std::uint64_t));
                const auto argument = string_at(argument_pointer);
                if (argument.size() > 2 && argument[0] == '-' && argument[1] == '-') {
                    const auto equal = argument.find('=');
                    const auto name_end = equal == std::string::npos ? argument.size() : equal;
                    const auto name = argument.substr(2, name_end - 2);
                    const auto table = registers[3];
                    const auto index_pointer = registers[4];
                    std::optional<std::pair<std::size_t, std::uint64_t>> exact;
                    std::vector<std::pair<std::size_t, std::uint64_t>> prefixes;
                    for (std::size_t index = 0; index < 4096; ++index) {
                        if (index > (std::numeric_limits<std::uint64_t>::max() - table) / 32) {
                            throw std::out_of_range("Guest long-option table address overflow");
                        }
                        const auto record = table + index * 32;
                        const auto candidate_address = load_u64(record);
                        if (candidate_address == 0) break;
                        const auto candidate = string_at(candidate_address);
                        if (candidate == name) { exact = std::make_pair(index, record); break; }
                        if (candidate.size() >= name.size() &&
                            candidate.compare(0, name.size(), name) == 0) {
                            prefixes.emplace_back(index, record);
                        }
                    }
                    const auto selected = exact ? exact
                        : (prefixes.size() == 1 ? std::optional<std::pair<std::size_t, std::uint64_t>>(
                              prefixes.front()) : std::nullopt);
                    ++state.optionIndex;
                    registers[0] = '?';
                    const std::uint64_t no_argument = 0;
                    std::memcpy(memory.buffer(bootstrap.optargSlot, 8, true),
                                &no_argument, sizeof(no_argument));
                    if (selected) {
                        const auto record = selected->second;
                        std::int32_t requires = 0;
                        std::uint64_t flag = 0;
                        std::uint32_t value = 0;
                        std::memcpy(&requires, memory.buffer(record + 8, sizeof(requires)), sizeof(requires));
                        std::memcpy(&flag, memory.buffer(record + 16, sizeof(flag)), sizeof(flag));
                        std::memcpy(&value, memory.buffer(record + 24, sizeof(value)), sizeof(value));
                        const bool attached = equal != std::string::npos;
                        if (!(attached && requires == 0) &&
                            !(requires == 1 && !attached && state.optionIndex >= argc)) {
                            if (attached) {
                                const auto value_address = argument_pointer + equal + 1;
                                std::memcpy(memory.buffer(bootstrap.optargSlot, 8, true),
                                            &value_address, sizeof(value_address));
                            } else if (requires == 1) {
                                const auto value_address = load_u64(
                                    argv + state.optionIndex * sizeof(std::uint64_t));
                                std::memcpy(memory.buffer(bootstrap.optargSlot, 8, true),
                                            &value_address, sizeof(value_address));
                                ++state.optionIndex;
                            }
                            if (flag != 0) {
                                std::memcpy(memory.buffer(flag, sizeof(value), true), &value, sizeof(value));
                            }
                            if (index_pointer != 0) {
                                const auto long_index = static_cast<std::int32_t>(selected->first);
                                std::memcpy(memory.buffer(index_pointer, sizeof(long_index), true),
                                            &long_index, sizeof(long_index));
                            }
                            registers[0] = flag == 0 ? value : 0;
                        } else if (requires == 1 && !attached && !options.empty() && options[0] == ':') {
                            registers[0] = ':';
                        }
                    }
                    std::memcpy(memory.buffer(bootstrap.optindSlot, 8, true),
                                &state.optionIndex, sizeof(std::uint64_t));
                    break;
                }
            }
            if (state.optionIndex < argc) {
                const auto argument_slot = argv + state.optionIndex * sizeof(std::uint64_t);
                const auto argument_pointer = load_u64(argument_slot);
                const auto argument = string_at(argument_pointer);
                if (state.optionOffset < argument.size() && argument[0] == '-') {
                    if (argument == "--") {
                        ++state.optionIndex;
                    } else {
                        const auto option = static_cast<unsigned char>(argument[state.optionOffset]);
                        diagnosed_option = static_cast<char>(option);
                        ++state.optionOffset;
                        const auto location = options.find(static_cast<char>(option));
                        if (location != std::string::npos && option != ':') {
                            registers[0] = option;
                            if (location + 1 < options.size() && options[location + 1] == ':') {
                                if (state.optionOffset < argument.size()) {
                                    const auto value = argument_pointer + state.optionOffset;
                                    std::memcpy(memory.buffer(bootstrap.optargSlot, 8, true),
                                                &value, sizeof(value));
                                } else if (state.optionIndex + 1 < argc) {
                                    ++state.optionIndex;
                                    const auto value_slot = argv + state.optionIndex * sizeof(std::uint64_t);
                                    const auto value = load_u64(value_slot);
                                    std::memcpy(memory.buffer(bootstrap.optargSlot, 8, true),
                                                &value, sizeof(value));
                                } else {
                                    registers[0] = !options.empty() && options[0] == ':' ? ':' : '?';
                                    missing_argument = true;
                                }
                                state.optionOffset = argument.size();
                            }
                        } else {
                            registers[0] = '?';
                        }
                        if (state.optionOffset >= argument.size()) {
                            ++state.optionIndex;
                            state.optionOffset = 1;
                        }
                    }
                }
            }
            if (registers[0] == '?' && (options.empty() || options[0] != ':')) {
                std::string program;
                if (argc != 0) {
                    const auto argv0_pointer = load_u64(argv);
                    program = string_at(argv0_pointer);
                }
                std::string diagnostic = program + (missing_argument
                    ? ": option requires an argument -- " : ": illegal option -- ");
                diagnostic.push_back(diagnosed_option);
                diagnostic.push_back('\n');
                (void)write_all(STDERR_FILENO,
                    reinterpret_cast<const std::uint8_t*>(diagnostic.data()), diagnostic.size());
            }
            std::memcpy(memory.buffer(bootstrap.optindSlot, 8, true),
                        &state.optionIndex, sizeof(std::uint64_t));
            if (registers[0] == std::numeric_limits<std::uint32_t>::max()) {
                const std::uint64_t no_argument = 0;
                std::memcpy(memory.buffer(bootstrap.optargSlot, 8, true),
                            &no_argument, sizeof(no_argument));
            }
            break;
        }
        case 14: {  // fwrite to standard output/error or an owned FILE
            const auto stream = normalize_stream(registers[3]);
            if (registers[1] != 0 && registers[2] >
                std::numeric_limits<std::uint64_t>::max() / registers[1]) {
                throw std::runtime_error("Guest fwrite length overflow");
            }
            const auto length = registers[1] * registers[2];
            const auto* source = memory.buffer(registers[0], length);
            if (state.closedStreams.count(stream)) {
                throw std::runtime_error("Guest fwrite stream closed");
            }
            if (stream == bootstrap.stdoutSlot || stream == bootstrap.stderrSlot) {
                const auto descriptor = stream == bootstrap.stdoutSlot
                    ? STDOUT_FILENO : STDERR_FILENO;
                const auto result = write_all(descriptor, source, static_cast<std::size_t>(length));
                if (result < 0) set_errno(errno);
                registers[0] = result < 0 ? 0 : registers[2];
                break;
            }
            const auto found = state.files.find(stream);
            if (found == state.files.end()) throw std::runtime_error("Guest fwrite stream unsupported");
            const auto result = std::fwrite(source, static_cast<std::size_t>(registers[1]),
                                            static_cast<std::size_t>(registers[2]), found->second);
            if (std::ferror(found->second) != 0) set_errno(errno);
            registers[0] = result;
            break;
        }
        case 15:  // dirname
        case 16: {  // basename
            const auto path = string_at(registers[0]);
            std::vector<char> mutable_path(path.begin(), path.end());
            mutable_path.push_back('\0');
            const auto* result = service == 15 ? ::dirname(mutable_path.data())
                                               : ::basename(mutable_path.data());
            if (result == nullptr) {
                registers[0] = 0;
                break;
            }
            const auto length = std::strlen(result) + 1;
            if (bootstrap.scratchAddress == 0 || length > 4096) {
                throw std::runtime_error("Guest path component exceeds guest scratch space");
            }
            std::memcpy(memory.buffer(bootstrap.scratchAddress, length, true), result, length);
            registers[0] = bootstrap.scratchAddress;
            break;
        }
        case 33: {  // printf: the guest bridge reads its variadic save area at SP
            const auto output = format_with_arguments(registers[0], 8, registers[31], false);
            const auto result = write_all(STDOUT_FILENO, output.data(), output.size());
            if (result < 0) set_errno(errno);
            registers[0] = output.size();
            break;
        }
        case 55: {  // fprintf
            const auto stream = normalize_stream(registers[0]);
            if (!is_standard_stream(stream) || state.closedStreams.count(stream)) {
                throw std::runtime_error("Guest fprintf stream unsupported or closed");
            }
            const auto output = format_with_arguments(registers[1], 8, registers[31], false);
            const auto descriptor = stream == bootstrap.stdoutSlot ? STDOUT_FILENO : STDERR_FILENO;
            const auto result = write_all(descriptor, output.data(), output.size());
            if (result < 0) set_errno(errno);
            registers[0] = output.size();
            break;
        }
        case 89: {  // vfprintf, guest va_list points at its saved argument area
            const auto stream = normalize_stream(registers[0]);
            if (!is_standard_stream(stream) || state.closedStreams.count(stream)) {
                throw std::runtime_error("Guest vfprintf stream unsupported or closed");
            }
            const auto output = format_with_arguments(registers[1], 8, registers[2], true);
            const auto descriptor = stream == bootstrap.stdoutSlot ? STDOUT_FILENO : STDERR_FILENO;
            const auto result = write_all(descriptor, output.data(), output.size());
            if (result < 0) set_errno(errno);
            registers[0] = output.size();
            break;
        }
        case 93:  // snprintf
        case 94: {  // __snprintf_chk
            const bool checked = service == 94;
            const auto destination = registers[0];
            const auto capacity = registers[1];
            if (checked && capacity > registers[3]) {
                throw std::runtime_error("Guest snprintf exceeds object size");
            }
            const auto formatAddress = registers[checked ? 4 : 2];
            const auto output = format_with_arguments(formatAddress, 8,
                                                      registers[31], false);
            if (capacity != 0) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                    output.size(), capacity - 1));
                auto* target = memory.buffer(destination, count + 1, true);
                if (count != 0) std::memcpy(target, output.data(), count);
                target[count] = 0;
            }
            registers[0] = output.size();
            break;
        }
        case 97: {  // vwarnx
            std::vector<std::uint8_t> output;
            if (registers[0] != 0) {
                output = format_with_arguments(registers[0], 8, registers[1], true);
            }
            auto message = guest_string_reader(bootstrap.programName);
            if (registers[0] != 0) {
                message.push_back(':');
                message.push_back(' ');
                message.insert(message.end(), output.begin(), output.end());
            }
            message.push_back('\n');
            (void)write_all(STDERR_FILENO, message.data(), message.size());
            registers[0] = 0;
            break;
        }
        case 150:  // err
        case 151:  // warn
        case 152: {  // warnx
            const bool terminating = service == 150;
            const bool include_errno = service != 152;
            const auto format_address = terminating ? registers[1] : registers[0];
            std::vector<std::uint8_t> output;
            if (format_address != 0) {
                output = format_with_arguments(format_address, 8, registers[31], false);
            }
            auto message = guest_string_reader(bootstrap.programName);
            if (format_address != 0) {
                message.push_back(':');
                message.push_back(' ');
                message.insert(message.end(), output.begin(), output.end());
            }
            if (include_errno) {
                std::uint64_t guest_error = 0;
                std::memcpy(&guest_error, memory.buffer(bootstrap.errnoSlot, sizeof(guest_error)),
                            sizeof(guest_error));
                const auto* error_text = std::strerror(static_cast<int>(guest_error));
                if (format_address != 0) {
                    message.push_back(':');
                    message.push_back(' ');
                } else {
                    message.push_back(':');
                    message.push_back(' ');
                }
                message.insert(message.end(), error_text, error_text + std::strlen(error_text));
            }
            message.push_back('\n');
            (void)write_all(STDERR_FILENO, message.data(), message.size());
            if (terminating) return {true, static_cast<std::int32_t>(registers[0])};
            registers[0] = 0;
            break;
        }
        case 26: {  // malloc
            const auto address = allocate_guest(registers[0]);
            if (address) registers[0] = static_cast<std::uint64_t>(*address);
            else { set_errno(ENOMEM); registers[0] = 0; }
            break;
        }
        case 40: {  // calloc
            if (registers[0] != 0 && registers[1] >
                std::numeric_limits<std::uint64_t>::max() / registers[0]) {
                set_errno(ENOMEM);
                registers[0] = 0;
                break;
            }
            const auto size = registers[0] * registers[1];
            const auto address = allocate_guest(size);
            if (!address) {
                set_errno(ENOMEM);
                registers[0] = 0;
                break;
            }
            if (size != 0) std::memset(memory.buffer(static_cast<std::uint64_t>(*address), size, true), 0,
                                       static_cast<std::size_t>(size));
            registers[0] = static_cast<std::uint64_t>(*address);
            break;
        }
        case 56: {  // free
            if (registers[0] > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Invalid guest heap pointer");
            }
            guest_heap().release(static_cast<std::int64_t>(registers[0]));
            break;
        }
        case 57: {  // realloc
            const auto old_address = registers[0];
            const auto requested = registers[1];
            if (old_address > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Invalid guest heap pointer");
            }
            std::int64_t previous = 0;
            if (old_address != 0) {
                const auto prior_size = guest_heap().size_of(static_cast<std::int64_t>(old_address));
                if (!prior_size) throw std::runtime_error("Invalid guest heap pointer");
                previous = *prior_size;
            }
            if (old_address != 0 && requested > 0 &&
                requested <= static_cast<std::uint64_t>(previous)) {
                registers[0] = old_address;
                break;
            }
            const auto replacement = allocate_guest(requested);
            if (!replacement) {
                set_errno(ENOMEM);
                registers[0] = 0;
                break;
            }
            const auto copy_size = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(previous), requested);
            if (copy_size != 0) {
                const auto* source = memory.buffer(old_address, copy_size);
                auto* destination = memory.buffer(static_cast<std::uint64_t>(*replacement), copy_size, true);
                std::memcpy(destination, source, static_cast<std::size_t>(copy_size));
            }
            if (old_address != 0) guest_heap().release(static_cast<std::int64_t>(old_address));
            registers[0] = static_cast<std::uint64_t>(*replacement);
            break;
        }
        case 146: {  // malloc_size
            if (registers[0] > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                registers[0] = 0;
                break;
            }
            const auto size = guest_heap().size_of(static_cast<std::int64_t>(registers[0]));
            registers[0] = size ? static_cast<std::uint64_t>(*size) : 0;
            break;
        }
        case 31: {  // fclose
            const auto stream = normalize_stream(registers[0]);
            const auto found = state.files.find(stream);
            if (found != state.files.end()) {
                const auto result = std::fclose(found->second);
                state.files.erase(found);
                if (const auto lineBuffer = state.fgetlnBuffers.find(stream);
                    lineBuffer != state.fgetlnBuffers.end()) {
                    guest_heap().release(static_cast<std::int64_t>(lineBuffer->second.address));
                    state.fgetlnBuffers.erase(lineBuffer);
                }
                if (result != 0) set_errno(errno);
                registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            } else if (is_standard_stream(stream) &&
                       state.closedStreams.insert(stream).second) {
                if (const auto lineBuffer = state.fgetlnBuffers.find(stream);
                    lineBuffer != state.fgetlnBuffers.end()) {
                    guest_heap().release(static_cast<std::int64_t>(lineBuffer->second.address));
                    state.fgetlnBuffers.erase(lineBuffer);
                }
                registers[0] = 0;
            } else {
                set_errno(EBADF);
                registers[0] = failure;
            }
            break;
        }
        case 62: {  // fopen read-only
            const auto path = string_at(registers[0]);
            const auto mode = string_at(registers[1]);
            if (mode != "r" && mode != "rb") {
                throw std::runtime_error("Guest fopen supports read-only files");
            }
            auto* file = std::fopen(path.c_str(), "rb");
            if (file == nullptr) {
                set_errno(errno);
                registers[0] = 0;
            } else {
                const auto handle = state.nextFile;
                state.nextFile += 16;
                state.files.emplace(handle, file);
                registers[0] = handle;
            }
            break;
        }
        case 158: {  // fgets
            const auto bufferAddress = registers[0];
            const auto requested = static_cast<std::int32_t>(registers[1]);
            const auto stream = normalize_stream(registers[2]);
            auto* file = stream_file(stream);
            if (file == nullptr || state.closedStreams.count(stream)) {
                throw std::runtime_error("Guest fgets stream unsupported or closed");
            }
            if (requested <= 0) {
                registers[0] = 0;
                break;
            }
            auto* destination = reinterpret_cast<char*>(memory.buffer(
                bufferAddress, static_cast<std::uint32_t>(requested), true));
            const auto* result = std::fgets(destination, requested, file);
            if (result == nullptr && std::ferror(file) != 0) set_errno(errno);
            registers[0] = result == nullptr ? 0 : bufferAddress;
            break;
        }
        case 159: {  // getline
            const auto linePointerSlot = registers[0];
            const auto capacitySlot = registers[1];
            const auto stream = normalize_stream(registers[2]);
            (void)memory.buffer(linePointerSlot, sizeof(std::uint64_t), true);
            (void)memory.buffer(capacitySlot, sizeof(std::uint64_t), true);
            std::uint64_t linePointer = 0;
            std::uint64_t capacity = 0;
            std::memcpy(&linePointer, memory.buffer(linePointerSlot, sizeof(linePointer)),
                        sizeof(linePointer));
            std::memcpy(&capacity, memory.buffer(capacitySlot, sizeof(capacity)),
                        sizeof(capacity));
            auto* file = stream_file(stream);
            if (file == nullptr || state.closedStreams.count(stream)) {
                throw std::runtime_error("Guest getline stream unsupported or closed");
            }
            std::vector<std::uint8_t> line;
            for (;;) {
                const auto value = std::fgetc(file);
                if (value == EOF) break;
                line.push_back(static_cast<std::uint8_t>(value));
                if (value == '\n') break;
            }
            if (std::ferror(file) != 0) {
                set_errno(errno);
                registers[0] = failure;
                break;
            }
            if (line.empty() && std::feof(file) != 0) {
                registers[0] = failure;
                break;
            }
            if (line.size() >= std::numeric_limits<std::int64_t>::max()) {
                set_errno(ENOMEM);
                registers[0] = failure;
                break;
            }
            const auto required = static_cast<std::uint64_t>(line.size() + 1);
            if (linePointer == 0 || capacity < required) {
                std::uint64_t oldCapacity = 0;
                if (linePointer != 0) {
                    if (linePointer > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                        throw std::runtime_error("Invalid guest getline buffer pointer");
                    }
                    const auto oldAllocation = guest_heap().size_of(
                        static_cast<std::int64_t>(linePointer));
                    if (!oldAllocation) throw std::runtime_error("Invalid guest getline buffer pointer");
                    oldCapacity = static_cast<std::uint64_t>(*oldAllocation);
                }
                const auto replacement = allocate_guest(required);
                if (!replacement) {
                    set_errno(ENOMEM);
                    registers[0] = failure;
                    break;
                }
                const auto replacementAddress = static_cast<std::uint64_t>(*replacement);
                const auto copyLength = std::min(capacity, oldCapacity);
                if (linePointer != 0 && copyLength != 0) {
                    const auto* source = memory.buffer(linePointer, copyLength);
                    auto* destination = memory.buffer(replacementAddress, copyLength, true);
                    std::memcpy(destination, source, static_cast<std::size_t>(copyLength));
                }
                if (linePointer != 0) guest_heap().release(static_cast<std::int64_t>(linePointer));
                linePointer = replacementAddress;
                capacity = static_cast<std::uint64_t>(*guest_heap().size_of(
                    static_cast<std::int64_t>(replacementAddress)));
                std::memcpy(memory.buffer(linePointerSlot, sizeof(linePointer), true),
                            &linePointer, sizeof(linePointer));
                std::memcpy(memory.buffer(capacitySlot, sizeof(capacity), true),
                            &capacity, sizeof(capacity));
            }
            auto* destination = memory.buffer(linePointer, required, true);
            if (!line.empty()) std::memcpy(destination, line.data(), line.size());
            destination[line.size()] = 0;
            registers[0] = line.size();
            break;
        }
        case 160: {  // fgetln: return a guest-owned reusable buffer, never a host FILE buffer
            const auto stream = normalize_stream(registers[0]);
            const auto lengthAddress = registers[1];
            (void)memory.buffer(lengthAddress, sizeof(std::size_t), true);
            auto* file = stream_file(stream);
            if (file == nullptr || state.closedStreams.count(stream)) {
                throw std::runtime_error("Guest fgetln stream unsupported or closed");
            }
            std::size_t length = 0;
            const auto* hostLine = ::fgetln(file, &length);
            const auto store_length = [&](std::size_t value) {
                std::memcpy(memory.buffer(lengthAddress, sizeof(value), true), &value,
                            sizeof(value));
            };
            if (hostLine == nullptr) {
                store_length(0);
                if (std::ferror(file) != 0) set_errno(errno);
                registers[0] = 0;
                break;
            }
            if (length > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                set_errno(ENOMEM);
                store_length(0);
                registers[0] = 0;
                break;
            }
            auto buffer = state.fgetlnBuffers.find(stream);
            if (buffer == state.fgetlnBuffers.end() || buffer->second.capacity < length) {
                const auto allocation = allocate_guest(length);
                if (!allocation) {
                    set_errno(ENOMEM);
                    store_length(0);
                    registers[0] = 0;
                    break;
                }
                const auto address = static_cast<std::uint64_t>(*allocation);
                const auto capacity = static_cast<std::uint64_t>(*guest_heap().size_of(*allocation));
                if (buffer != state.fgetlnBuffers.end()) {
                    guest_heap().release(static_cast<std::int64_t>(buffer->second.address));
                    buffer->second = HostServiceState::GuestLineBuffer{address, capacity};
                } else {
                    buffer = state.fgetlnBuffers.emplace(
                        stream, HostServiceState::GuestLineBuffer{address, capacity}).first;
                }
            }
            std::memcpy(memory.buffer(buffer->second.address, length, true), hostLine, length);
            store_length(length);
            registers[0] = buffer->second.address;
            break;
        }
        case 63: {  // fread
            const auto stream = normalize_stream(registers[3]);
            auto* file = stream_file(stream);
            if (file == nullptr || (stream != bootstrap.stdinSlot &&
                                    state.closedStreams.count(stream))) {
                throw std::runtime_error("Guest fread stream unsupported");
            }
            if (registers[1] != 0 && registers[2] >
                std::numeric_limits<std::uint64_t>::max() / registers[1]) {
                throw std::runtime_error("Guest fread length overflow");
            }
            const auto length = registers[1] * registers[2];
            auto* destination = memory.buffer(registers[0], length, true);
            if (length == 0) registers[0] = 0;
            else {
                const auto result = std::fread(destination, static_cast<std::size_t>(registers[1]),
                                               static_cast<std::size_t>(registers[2]), file);
                if (std::ferror(file) != 0) set_errno(errno);
                registers[0] = result;
            }
            break;
        }
        case 64: {  // feof
            const auto stream = normalize_stream(registers[0]);
            auto* file = stream_file(stream);
            if (file == nullptr || (stream != bootstrap.stdinSlot &&
                                    state.closedStreams.count(stream))) {
                throw std::runtime_error("Guest feof stream unsupported");
            }
            registers[0] = std::feof(file) == 0 ? 0 : 1;
            break;
        }
        case 90: {  // fputc
            const auto character = static_cast<unsigned char>(registers[0]);
            const auto stream = normalize_stream(registers[1]);
            int result = EOF;
            if (stream == bootstrap.stdoutSlot || stream == bootstrap.stderrSlot) {
                const auto descriptor = stream == bootstrap.stdoutSlot
                    ? STDOUT_FILENO : STDERR_FILENO;
                if (state.closedStreams.count(stream)) {
                    set_errno(EBADF);
                } else {
                    const auto written = write_all(descriptor, &character, sizeof(character));
                    if (written == 1) result = character;
                    else set_errno(errno);
                }
            } else {
                auto* file = stream_file(stream);
                if (file == nullptr) {
                    throw std::runtime_error("Guest fputc stream unsupported or closed");
                }
                result = std::fputc(character, file);
                if (result == EOF && std::ferror(file) != 0) set_errno(errno);
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 92: {  // fcopyfile
            const auto source = static_cast<std::int32_t>(registers[0]);
            const auto destination = static_cast<std::int32_t>(registers[1]);
            const auto flags = static_cast<std::uint32_t>(registers[3]);
            if (source < 0 || destination < 0) {
                registers[0] = failure;
                set_errno(EINVAL);
                break;
            }
            const bool guest_file_source_open = std::any_of(
                state.files.begin(), state.files.end(), [&](const auto& item) {
                    return item.second != nullptr && !state.closedStreams.count(item.first) &&
                           ::fileno(item.second) == source;
                });
            const bool source_open = owns_descriptor(source) || guest_file_source_open ||
                (source == STDIN_FILENO && standard_descriptor_open(source));
            const bool destination_open =
                ((destination == STDOUT_FILENO || destination == STDERR_FILENO) &&
                 standard_descriptor_open(destination)) ||
                (owns_descriptor(destination) &&
                 state.writableDescriptors.count(destination) != 0);
            if (!source_open || !destination_open) {
                registers[0] = failure;
                set_errno(EBADF);
            } else if (registers[2] != 0 ||
                       (flags & ~static_cast<std::uint32_t>(COPYFILE_ACL | COPYFILE_XATTR)) != 0 ||
                       flags == 0) {
                registers[0] = failure;
                set_errno(ENOTSUP);
            } else {
                const auto result = ::fcopyfile(source, destination, nullptr, flags);
                registers[0] = static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(result));
                if (result < 0) set_errno(errno);
            }
            break;
        }
        case 95:
        case 96: {  // fgetattrlist / fsetattrlist
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            const bool setting = service == 96;
            const bool live_guest_file = std::any_of(
                state.files.begin(), state.files.end(), [&](const auto& item) {
                    return item.second != nullptr && !state.closedStreams.count(item.first) &&
                           ::fileno(item.second) == descriptor;
                });
            const bool descriptor_open = setting
                ? (((descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO) &&
                    standard_descriptor_open(descriptor)) ||
                   (owns_descriptor(descriptor) &&
                    state.writableDescriptors.count(descriptor) != 0))
                : (owns_descriptor(descriptor) || live_guest_file ||
                   (descriptor >= STDIN_FILENO && descriptor <= STDERR_FILENO &&
                    standard_descriptor_open(descriptor)));
            if (!descriptor_open) {
                registers[0] = failure;
                set_errno(EBADF);
                break;
            }
            if (registers[3] > std::numeric_limits<std::size_t>::max() ||
                registers[3] > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                registers[0] = failure;
                set_errno(EINVAL);
                break;
            }
            struct attrlist attributes{};
            try {
                std::memcpy(&attributes, memory.buffer(registers[1], sizeof(attributes)),
                            sizeof(attributes));
            } catch (const std::out_of_range&) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            if (registers[4] != 0 ||
                (setting && (attributes.bitmapcount != ATTR_BIT_MAP_COUNT ||
                             attributes.commonattr != ATTR_CMN_FNDRINFO ||
                             attributes.volattr != 0 || attributes.dirattr != 0 ||
                             attributes.fileattr != 0 || attributes.forkattr != 0))) {
                registers[0] = failure;
                set_errno(ENOTSUP);
                break;
            }
            std::uint8_t* buffer = nullptr;
            try {
                buffer = memory.buffer(registers[2], registers[3], setting);
            } catch (const std::out_of_range&) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            const auto result = setting
                ? ::fsetattrlist(descriptor, &attributes, buffer,
                                 static_cast<int>(registers[3]), 0)
                : ::fgetattrlist(descriptor, &attributes, buffer,
                                 static_cast<int>(registers[3]), 0);
            registers[0] = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(result));
            if (result < 0) set_errno(errno);
            break;
        }
        case 170: {  // getattrlist
            if (registers[0] < bootstrap.pageSize) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            std::string path;
            try {
                path = string_at(registers[0]);
            } catch (const std::out_of_range&) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            if (registers[3] > std::numeric_limits<std::size_t>::max()) {
                registers[0] = failure;
                set_errno(EINVAL);
                break;
            }
            if (registers[1] < bootstrap.pageSize) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            struct attrlist attributes{};
            try {
                std::memcpy(&attributes, memory.buffer(registers[1], sizeof(attributes)),
                            sizeof(attributes));
            } catch (const std::out_of_range&) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            void* output = nullptr;
            if (registers[3] != 0) {
                if (registers[2] < bootstrap.pageSize) {
                    registers[0] = failure;
                    set_errno(EFAULT);
                    break;
                }
                try {
                    output = memory.buffer(registers[2], registers[3], true);
                } catch (const std::out_of_range&) {
                    registers[0] = failure;
                    set_errno(EFAULT);
                    break;
                }
            }
            const auto result = ::getattrlist(
                path.c_str(), &attributes, output,
                static_cast<std::size_t>(registers[3]),
                static_cast<unsigned long>(registers[4]));
            registers[0] = static_cast<std::uint32_t>(result);
            if (result < 0) set_errno(errno);
            break;
        }
        case 98: {  // fgetc
            const auto stream = normalize_stream(registers[0]);
            auto* file = stream_file(stream);
            if (file == nullptr || (stream != bootstrap.stdinSlot &&
                                    state.closedStreams.count(stream))) {
                throw std::runtime_error("Guest fgetc stream unsupported");
            }
            const auto result = std::fgetc(file);
            if (result == EOF && std::ferror(file) != 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 99: {  // ungetc
            const auto stream = normalize_stream(registers[1]);
            auto* file = stream_file(stream);
            if (file == nullptr || (stream != bootstrap.stdinSlot &&
                                    state.closedStreams.count(stream))) {
                throw std::runtime_error("Guest ungetc stream unsupported");
            }
            const auto result = std::ungetc(static_cast<int>(registers[0]), file);
            if (result == EOF) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 11: {  // memcmp
            const auto* left = memory.buffer(registers[0], registers[2]);
            const auto* right = memory.buffer(registers[1], registers[2]);
            const int result = std::memcmp(left, right, static_cast<std::size_t>(registers[2]));
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 12: {  // getcwd
            const auto address = registers[0];
            const auto capacity = registers[1];
            if (capacity > std::numeric_limits<std::size_t>::max()) {
                registers[0] = 0;
                break;
            }
            auto* destination = memory.buffer(address, capacity, true);
            registers[0] = ::getcwd(reinterpret_cast<char*>(destination),
                                    static_cast<std::size_t>(capacity)) == nullptr ? 0 : address;
            if (registers[0] == 0 && bootstrap.errnoSlot != 0) {
                const auto host_error = static_cast<std::uint64_t>(errno);
                std::memcpy(memory.buffer(bootstrap.errnoSlot, sizeof(host_error), true),
                            &host_error, sizeof(host_error));
            }
            break;
        }
        case 27: {  // sysconf
            const auto result = ::sysconf(static_cast<int>(registers[0]));
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 13: {  // stat
            const auto path = string_at(registers[0]);
            struct stat info{};
            const auto result = ::stat(path.c_str(), &info);
            if (result == 0) {
                std::memcpy(memory.buffer(registers[1], sizeof(info), true), &info, sizeof(info));
            } else {
                set_errno(errno);
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 28:  // ___error
            registers[0] = bootstrap.errnoSlot;
            break;
        case 29: {  // strrchr
            const auto value = string_at(registers[0]);
            const auto needle = static_cast<std::uint8_t>(registers[1]);
            std::uint64_t found = 0;
            if (needle == 0) {
                found = registers[0] + value.size();
            } else {
                for (std::size_t index = value.size(); index > 0; --index) {
                    if (static_cast<std::uint8_t>(value[index - 1]) == needle) {
                        found = registers[0] + index - 1;
                        break;
                    }
                }
            }
            registers[0] = found;
            break;
        }
        case 48:  // getpid
            registers[0] = static_cast<std::uint64_t>(::getpid());
            break;
        case 49:  // sysctlbyname
        case 50: {  // sysctl
            const bool named = service == 49;
            const auto output_index = named ? 1U : 2U;
            if (registers[output_index + 2] != 0 || registers[output_index + 3] != 0) {
                throw std::runtime_error("Guest sysctl writes are unsupported");
            }
            const auto outputAddress = registers[output_index];
            const auto lengthAddress = registers[output_index + 1];
            std::string name;
            std::vector<int> mib;
            if (named) {
                name = string_at(registers[0]);
            } else {
                const auto mib_count = registers[1];
                if (mib_count == 0 || mib_count > 64) {
                    throw std::runtime_error("Invalid guest sysctl MIB length");
                }
                const auto* mib_bytes = memory.buffer(registers[0],
                    mib_count * sizeof(std::int32_t));
                mib.resize(static_cast<std::size_t>(mib_count));
                for (std::size_t index = 0; index < mib.size(); ++index) {
                    std::int32_t value = 0;
                    std::memcpy(&value, mib_bytes + index * sizeof(value), sizeof(value));
                    mib[index] = value;
                }
            }
            const bool stack64 = named ? name == "kern.usrstack64"
                : mib.size() == 2 && mib[0] == CTL_KERN && mib[1] == KERN_USRSTACK64;
            const bool stack32 = named ? name == "kern.usrstack"
                : mib.size() == 2 && mib[0] == CTL_KERN && mib[1] == KERN_USRSTACK32;
            const auto stackValueSize = stack64 ? sizeof(std::uint64_t)
                : stack32 ? sizeof(std::uint32_t) : std::size_t{0};
            if (lengthAddress == 0) {
                int result = -1;
                int error = 0;
                if (outputAddress != 0) {
                    error = ENOMEM;
                } else if (stackValueSize != 0) {
                    result = 0;
                } else if (named) {
                    result = ::sysctlbyname(name.c_str(), nullptr, nullptr, nullptr, 0);
                    if (result != 0) error = errno;
                } else {
                    result = ::sysctl(mib.data(), static_cast<u_int>(mib.size()),
                                      nullptr, nullptr, nullptr, 0);
                    if (result != 0) error = errno;
                }
                if (result != 0) set_errno(error);
                registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
                break;
            }
            std::size_t count = 0;
            std::memcpy(&count, memory.buffer(lengthAddress, sizeof(count)), sizeof(count));
            if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("Invalid guest sysctl length");
            }
            void* output = nullptr;
            if (outputAddress != 0) {
                output = memory.buffer(outputAddress, count, true);
            }
            int result = -1;
            int error = 0;
            if (stackValueSize != 0) {
                const auto stackTop = static_cast<std::uint64_t>(bootstrap.stack.end);
                if (outputAddress == 0) {
                    count = stackValueSize;
                    result = 0;
                } else if (count < stackValueSize) {
                    error = ERANGE;
                } else {
                    if (stack64) {
                        std::memcpy(output, &stackTop, sizeof(stackTop));
                    } else {
                        const auto stackTop32 = static_cast<std::uint32_t>(stackTop);
                        std::memcpy(output, &stackTop32, sizeof(stackTop32));
                    }
                    count = stackValueSize;
                    result = 0;
                }
            } else if (named) {
                result = ::sysctlbyname(name.c_str(), output, &count, nullptr, 0);
                if (result != 0) error = errno;
            } else {
                result = ::sysctl(mib.data(), static_cast<u_int>(mib.size()), output,
                                  &count, nullptr, 0);
                if (result != 0) error = errno;
            }
            std::memcpy(memory.buffer(lengthAddress, sizeof(count), true),
                        &count, sizeof(count));
            if (result != 0) set_errno(error);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 101:  // issetugid
            registers[0] = static_cast<std::uint64_t>(::issetugid());
            break;
        case 100: {  // posix_memalign
            const auto outputAddress = registers[0];
            const auto alignment = registers[1];
            if (alignment < 8 || (alignment & (alignment - 1)) != 0 ||
                alignment > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                registers[0] = EINVAL;
                break;
            }
            (void)memory.buffer(outputAddress, sizeof(std::uint64_t), true);
            const auto allocation = allocate_guest(registers[2],
                static_cast<std::int64_t>(alignment));
            if (!allocation) {
                registers[0] = ENOMEM;
                break;
            }
            const auto address = static_cast<std::uint64_t>(*allocation);
            auto* output = memory.buffer(outputAddress, sizeof(address), true);
            std::memcpy(output, &address, sizeof(address));
            registers[0] = 0;
            break;
        }
        case 110:  // __NSGetEnviron
            registers[0] = bootstrap.environSlot;
            break;
        case 118:  // gettimeofday
        case 119: {  // commpage gettimeofday
            const bool commpage = service == 119;
            timeval time{};
            struct timezone zone{};
            timeval* time_pointer = nullptr;
            struct timezone* zone_pointer = nullptr;
            if (commpage || registers[0] != 0) {
                time_pointer = &time;
                if (registers[0] != 0) {
                    (void)memory.buffer(registers[0], sizeof(time), true);
                }
            }
            if (!commpage && registers[1] != 0) {
                (void)memory.buffer(registers[1], sizeof(zone), true);
                zone_pointer = &zone;
            }
            const auto result = ::gettimeofday(time_pointer, zone_pointer);
            if (result == 0 && registers[0] != 0 && time_pointer != nullptr) {
                std::memcpy(memory.buffer(registers[0], sizeof(time), true), &time,
                            sizeof(time));
            }
            if (result == 0 && registers[1] != 0 && zone_pointer != nullptr) {
                std::memcpy(memory.buffer(registers[1], sizeof(zone), true), &zone,
                            sizeof(zone));
            }
            if (result != 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 121:  // dyld_is_memory_immutable
            registers[0] = memory.is_readonly(registers[0], registers[1]) ? 1U : 0U;
            break;
        case 179:  // sys_icache_invalidate, code is refreshed when mappings become executable
            break;
        case 182:  // pthread_jit_write_protect_np: the runner applies per-thread mapping mode
            if (state.setJitWriteProtection &&
                !state.setJitWriteProtection(registers[0] != 0)) {
                throw std::runtime_error("Guest MAP_JIT write-protection state is unavailable");
            }
            state.jitWriteProtected = registers[0] != 0;
            break;
        case 112:  // mach_vm_allocate / vm_allocate
        case 114: {  // mach_vm_map (anonymous guest mapping only)
            constexpr std::uint32_t kernSuccess = 0;
            constexpr std::uint32_t kernNoSpace = 3;
            constexpr std::uint32_t kernInvalidArgument = 4;
            constexpr std::uint32_t kernProtectionFailure = 2;
            const bool map = service == 114;
            const auto rawFlags = static_cast<std::uint32_t>(registers[map ? 4 : 3]);
            const auto flags = rawFlags & 0x00ffffffU;
            if (registers[0] != bootstrap.guestTaskPort) {
                registers[0] = kernInvalidArgument;
                break;
            }
            if (map && (flags & 0x4000U) != 0) {
                constexpr std::uint32_t vmFlagsOverwrite = 0x4000U;
                constexpr std::uint32_t vmFlagsPurgable = 0x2U;
                constexpr std::uint32_t vmFlagsMte = 0x2000U;
                if ((flags & ~(vmFlagsOverwrite | vmFlagsPurgable | vmFlagsMte)) != 0 ||
                    registers[3] >= (1ULL << 62) ||
                    (registers[3] & (registers[3] + 1)) != 0 ||
                    registers[5] != 0 || registers[6] != 0 ||
                    registers[7] != 0 || registers[1] == 0 || registers[31] >
                        std::numeric_limits<std::uint64_t>::max() - 8) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                std::uint32_t currentProtection = 0;
                std::uint32_t maximumProtection = 0;
                std::uint32_t inheritance = 0;
                std::memcpy(&currentProtection,
                            memory.buffer(registers[31], sizeof(currentProtection)),
                            sizeof(currentProtection));
                std::memcpy(&maximumProtection,
                            memory.buffer(registers[31] + sizeof(currentProtection),
                                          sizeof(maximumProtection)),
                            sizeof(maximumProtection));
                std::memcpy(&inheritance,
                            memory.buffer(registers[31] + 8, sizeof(inheritance)),
                            sizeof(inheritance));
                if ((currentProtection & ~7U) != 0 ||
                    (maximumProtection & ~7U) != 0 || inheritance > 2U) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                if ((currentProtection & ~maximumProtection) != 0 ||
                    (currentProtection & 6U) == 6U) {
                    registers[0] = kernProtectionFailure;
                    break;
                }
                auto* addressStorage = memory.buffer(registers[1], sizeof(std::uint64_t), true);
                std::uint64_t address = 0;
                std::memcpy(&address, addressStorage, sizeof(address));
                if ((address & registers[3]) != 0) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                if (registers[2] == 0) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                if (!state.machVmOverwrite) {
                    registers[0] = kernNoSpace;
                    break;
                }
                const auto result = state.machVmOverwrite(
                    address, registers[2], currentProtection, maximumProtection,
                    inheritance, rawFlags >> 24U);
                if (result == kernSuccess) {
                    addressStorage = memory.buffer(registers[1], sizeof(address), true);
                    std::memcpy(addressStorage, &address, sizeof(address));
                }
                registers[0] = result;
                break;
            }
            constexpr std::uint32_t vmFlagsAnywhere = 1U;
            constexpr std::uint32_t vmFlagsPurgable = 2U;
            constexpr std::uint32_t vmFlagsMte = 0x2000U;
            if ((flags & ~(vmFlagsAnywhere | vmFlagsPurgable | vmFlagsMte)) != 0 ||
                (!map && (flags & vmFlagsAnywhere) == 0)) {
                registers[0] = kernInvalidArgument;
                break;
            }
            std::uint64_t alignment = bootstrap.pageSize;
            std::uint32_t currentProtection = 3;
            std::uint32_t maximumProtection = 3;
            std::uint32_t inheritance = 1;
            if (map) {
                const auto mask = registers[3];
                if (registers[5] != 0 || registers[6] != 0 || mask >= (1ULL << 62) ||
                    (mask & (mask + 1)) != 0) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                std::memcpy(&currentProtection,
                            memory.buffer(registers[31], sizeof(currentProtection)),
                            sizeof(currentProtection));
                std::memcpy(&maximumProtection,
                            memory.buffer(registers[31] + sizeof(currentProtection),
                                          sizeof(maximumProtection)),
                            sizeof(maximumProtection));
                std::memcpy(&inheritance,
                            memory.buffer(registers[31] + 8, sizeof(inheritance)),
                            sizeof(inheritance));
                const bool currentWriteExecute = (currentProtection & 2U) != 0 &&
                                                 (currentProtection & 4U) != 0;
                if ((currentProtection & ~7U) != 0 || currentWriteExecute ||
                    (maximumProtection & ~7U) != 0 ||
                    (currentProtection & ~maximumProtection) != 0 || inheritance > 2U) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                alignment = std::max(alignment, mask + 1);
            }
            auto* output = memory.buffer(registers[1], sizeof(std::uint64_t), true);
            if (registers[2] == 0) {
                const std::uint64_t zero = 0;
                std::memcpy(output, &zero, sizeof(zero));
                registers[0] = kernSuccess;
                break;
            }
            const auto page = bootstrap.pageSize;
            if (page == 0 || (page & (page - 1)) != 0 ||
                registers[2] > std::numeric_limits<std::uint64_t>::max() - (page - 1)) {
                registers[0] = kernNoSpace;
                break;
            }
            const auto size = (registers[2] + page - 1) & ~(page - 1);
            if (!map && !state.machVmAllocate) {
                registers[0] = kernNoSpace;
                break;
            }
            std::uint64_t address = 0;
            if (map && (flags & vmFlagsAnywhere) == 0) {
                std::memcpy(&address, output, sizeof(address));
                const auto mask = registers[3];
                std::uint32_t inheritance = 0;
                std::memcpy(&inheritance,
                            memory.buffer(registers[31] + 8, sizeof(inheritance)),
                            sizeof(inheritance));
                if (mask >= (1ULL << 62) || (mask & (mask + 1)) != 0 ||
                    (address & mask) != 0 || registers[5] != 0 || registers[6] != 0 ||
                    registers[7] != 0) {
                    registers[0] = kernInvalidArgument;
                    break;
                }
                if (!state.machVmReserve) {
                    registers[0] = kernNoSpace;
                    break;
                }
                const auto result = state.machVmReserve(
                    address, size, currentProtection, maximumProtection, inheritance,
                    rawFlags >> 24U, false);
                registers[0] = result;
                break;
            }
            if (map && !state.machVmAllocate) {
                registers[0] = kernNoSpace;
                break;
            }
            const auto result = state.machVmAllocate(size, alignment, currentProtection,
                                                     maximumProtection, address, false,
                                                     inheritance, rawFlags >> 24U);
            if (result == kernSuccess) std::memcpy(output, &address, sizeof(address));
            registers[0] = result;
            break;
        }
        case 113: {  // mach_vm_deallocate / vm_deallocate
            constexpr std::uint32_t kernSuccess = 0;
            constexpr std::uint32_t kernInvalidArgument = 4;
            if (registers[0] != bootstrap.guestTaskPort) {
                registers[0] = kernInvalidArgument;
            } else if (registers[2] == 0) {
                registers[0] = kernSuccess;
            } else if (!state.machVmDeallocate) {
                registers[0] = kernInvalidArgument;
            } else {
                registers[0] = state.machVmDeallocate(registers[1], registers[2]);
            }
            break;
        }
        case 200: {  // mach_vm_protect wrapper, matching its raw Mach trap
            constexpr std::uint32_t machSendInvalidDest = 0x10000003U;
            constexpr std::uint32_t kernInvalidArgument = 4;
            if (registers[0] != bootstrap.guestTaskPort) {
                registers[0] = machSendInvalidDest;
            } else if (!state.machVmProtect) {
                registers[0] = kernInvalidArgument;
            } else {
                registers[0] = state.machVmProtect(registers[1], registers[2],
                    registers[3] != 0, static_cast<std::uint32_t>(registers[4]), false);
            }
            break;
        }
        case 197: {  // mach_vm_read_overwrite from authoritative guest memory
            constexpr std::uint32_t machSendInvalidDest = 0x10000003U;
            constexpr std::uint32_t kernInvalidAddress = 1;
            constexpr std::uint32_t kernResourceShortage = 3;
            constexpr std::uint32_t kernInvalidArgument = 4;
            if (registers[0] != bootstrap.guestTaskPort) {
                registers[0] = machSendInvalidDest;
                break;
            }
            if (registers[2] > std::numeric_limits<std::size_t>::max()) {
                registers[0] = kernInvalidArgument;
                break;
            }
            const auto size = static_cast<std::size_t>(registers[2]);
            std::uint8_t* copiedSize = nullptr;
            try {
                copiedSize = memory.buffer(registers[4], sizeof(std::uint64_t), true);
            } catch (const std::out_of_range&) {
                registers[0] = kernInvalidAddress;
                break;
            }
            if (size == 0) {
                const std::uint64_t copied = 0;
                std::memcpy(copiedSize, &copied, sizeof(copied));
                registers[0] = 0;
                break;
            }
            if (registers[1] > std::numeric_limits<std::uint64_t>::max() - registers[2] ||
                registers[3] > std::numeric_limits<std::uint64_t>::max() - registers[2]) {
                registers[0] = kernInvalidAddress;
                break;
            }
            std::vector<std::uint8_t> snapshot;
            try {
                snapshot.resize(size);
            } catch (const std::bad_alloc&) {
                registers[0] = kernResourceShortage;
                break;
            } catch (const std::length_error&) {
                registers[0] = kernInvalidArgument;
                break;
            }
            std::uint64_t readOffset = 0;
            try {
                while (readOffset < registers[2]) {
                    const auto address = registers[1] + readOffset;
                    const auto available = memory.contiguous_readable_size(address);
                    if (available == 0) throw std::out_of_range("Empty guest source range");
                    const auto chunk = std::min(available, registers[2] - readOffset);
                    const auto* source = memory.buffer(address, chunk);
                    std::memcpy(snapshot.data() + static_cast<std::size_t>(readOffset),
                                source, static_cast<std::size_t>(chunk));
                    readOffset += chunk;
                }
            } catch (const std::out_of_range&) {
                registers[0] = kernInvalidAddress;
                break;
            }
            std::uint64_t written = 0;
            while (written < registers[2]) {
                try {
                    const auto address = registers[3] + written;
                    const auto available = memory.contiguous_readable_size(address);
                    if (available == 0) throw std::out_of_range("Empty guest destination range");
                    const auto chunk = std::min(available, registers[2] - written);
                    auto* destination = memory.buffer(address, chunk, true);
                    std::memmove(destination,
                        snapshot.data() + static_cast<std::size_t>(written),
                        static_cast<std::size_t>(chunk));
                    written += chunk;
                } catch (const std::out_of_range&) {
                    registers[0] = kernInvalidAddress;
                    break;
                }
            }
            if (written == registers[2]) {
                const auto copied = static_cast<std::uint64_t>(size);
                std::memcpy(copiedSize, &copied, sizeof(copied));
                registers[0] = 0;
            }
            break;
        }
        case 199: {  // mach_vm_region over logical guest map and reservation intervals
            constexpr std::uint32_t machSendInvalidDest = 0x10000003U;
            constexpr std::uint32_t kernInvalidAddress = 1;
            constexpr std::uint32_t kernInvalidArgument = 4;
            if (registers[0] != bootstrap.guestTaskPort) {
                registers[0] = machSendInvalidDest;
                break;
            }
            if (registers[3] != VM_REGION_BASIC_INFO_64) {
                registers[0] = kernInvalidArgument;
                break;
            }
            std::uint32_t count = 0;
            try {
                std::memcpy(&count, memory.buffer(registers[5], sizeof(count)), sizeof(count));
            } catch (const std::exception&) {
                registers[0] = kernInvalidAddress;
                break;
            }
            if (count < VM_REGION_BASIC_INFO_COUNT_64) {
                registers[0] = kernInvalidArgument;
                break;
            }
            mach_vm_address_t* address = nullptr;
            mach_vm_size_t* size = nullptr;
            vm_region_basic_info_data_64_t* info = nullptr;
            mach_port_t* object = nullptr;
            try {
                address = reinterpret_cast<mach_vm_address_t*>(
                    memory.buffer(registers[1], sizeof(*address), true));
                size = reinterpret_cast<mach_vm_size_t*>(
                    memory.buffer(registers[2], sizeof(*size), true));
                info = reinterpret_cast<vm_region_basic_info_data_64_t*>(
                    memory.buffer(registers[4], sizeof(*info), true));
                count = VM_REGION_BASIC_INFO_COUNT_64;
                object = reinterpret_cast<mach_port_t*>(
                    memory.buffer(registers[6], sizeof(*object), true));
            } catch (const std::exception&) {
                registers[0] = kernInvalidAddress;
                break;
            }
            const auto region = state.machVmRegion
                ? state.machVmRegion(*address) : std::nullopt;
            if (!region) {
                registers[0] = kernInvalidAddress;
                break;
            }
            vm_region_basic_info_data_64_t result{};
            result.protection = static_cast<vm_prot_t>(region->protection);
            result.max_protection = static_cast<vm_prot_t>(region->maximumProtection);
            result.inheritance = static_cast<vm_inherit_t>(region->inheritance);
            result.offset = region->offset;
            result.behavior = VM_BEHAVIOR_DEFAULT;
            result.user_wired_count = 0;
            *address = region->begin;
            *size = region->end - region->begin;
            *info = result;
            *object = MACH_PORT_NULL;
            std::memcpy(memory.buffer(registers[5], sizeof(count), true), &count, sizeof(count));
            registers[0] = 0;
            break;
        }
        case 116: {  // mprotect on anonymous guest VM mappings
            constexpr std::uint32_t kernSuccess = 0;
            constexpr std::uint32_t kernProtectionFailure = 2;
            const auto address = registers[0];
            const auto size = registers[1];
            const auto protection = static_cast<std::uint32_t>(registers[2]);
            const auto page = bootstrap.pageSize;
            if (page == 0 || (page & (page - 1)) != 0 || address % page != 0 ||
                size > std::numeric_limits<std::uint64_t>::max() - (page - 1) ||
                (protection & ~7U) != 0) {
                set_errno(EINVAL);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            if (size == 0) {
                registers[0] = 0;
                break;
            }
            const auto length = (size + page - 1) & ~(page - 1);
            if (!state.machVmProtect) {
                set_errno(ENOMEM);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            const auto result = state.machVmProtect(address, length, false, protection, true);
            if (result == kernSuccess) {
                registers[0] = 0;
            } else {
                set_errno(result == kernProtectionFailure ? EACCES : ENOMEM);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
            }
            break;
        }
        case 120: {  // getentropy
            const auto length = registers[1];
            if (length == 0) { registers[0] = 0; break; }
            if (length > 256) {
                set_errno(EINVAL);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            if (registers[0] == 0) {
                set_errno(EFAULT);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            std::uint8_t* output = nullptr;
            try {
                output = memory.buffer(registers[0], length, true);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            const auto result = guest_entropy(output, length);
            if (result != 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 117:  // _NSVersionOfLinkTimeLibrary
            throw std::runtime_error("Guest link-time library version metadata is unavailable");
        case 129:  // __NSGetMachExecuteHeader
            registers[0] = bootstrap.mainHeader;
            break;
        case 130:  // dyld_get_active_platform
            registers[0] = bootstrap.activePlatform;
            break;
        case 132:  // dyld_shared_cache_some_image_overridden
            registers[0] = 0;
            break;
        case 134:  // dyld Objective-C class count
            registers[0] = bootstrap.objectiveCClassCount;
            break;
        case 139:  // __NSGetArgc
            registers[0] = bootstrap.argcAddress;
            break;
        case 140:  // __NSGetArgv
            registers[0] = bootstrap.argvAddress;
            break;
        case 142: {  // __NSGetExecutablePath
            const auto required = bootstrap.resolvedExecutablePath.size() + 1;
            if (required > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("Guest executable path exceeds the Darwin API limit");
            }
            std::uint32_t capacity = 0;
            std::memcpy(&capacity, memory.buffer(registers[1], sizeof(capacity)), sizeof(capacity));
            if (capacity < required) {
                const auto size = static_cast<std::uint32_t>(required);
                std::memcpy(memory.buffer(registers[1], sizeof(size), true), &size, sizeof(size));
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            auto* output = memory.buffer(registers[0], required, true);
            std::memcpy(output, bootstrap.resolvedExecutablePath.c_str(), required);
            registers[0] = 0;
            break;
        }
        case 17:  // ferror
            registers[0] = normalize_stream(registers[0]);
            if (const auto found = state.files.find(registers[0]); found != state.files.end()) {
                registers[0] = std::ferror(found->second);
            } else if (is_standard_stream(registers[0]) && !state.closedStreams.count(registers[0])) {
                registers[0] = registers[0] == bootstrap.stdinSlot ? std::ferror(stdin) : 0;
            } else {
                throw std::runtime_error("Guest ferror stream unsupported");
            }
            break;
        case 18: {  // setlocale and copy its result into guest scratch storage
            const auto category = static_cast<int>(static_cast<std::int32_t>(registers[0]));
            std::string requested;
            const char* request_pointer = nullptr;
            if (registers[1] != 0) {
                requested = string_at(registers[1]);
                request_pointer = requested.c_str();
            }
            const auto* result = std::setlocale(category, request_pointer);
            if (result == nullptr) {
                registers[0] = 0;
            } else {
                const auto length = std::strlen(result) + 1;
                if (bootstrap.scratchAddress == 0 || length > 4096) {
                    throw std::runtime_error("Host locale string exceeds guest scratch space");
                }
                std::memcpy(memory.buffer(bootstrap.scratchAddress, length, true), result, length);
                registers[0] = bootstrap.scratchAddress;
            }
            const std::uint64_t max_multibyte = MB_CUR_MAX;
            std::memcpy(memory.buffer(bootstrap.localeSlot, sizeof(max_multibyte), true),
                        &max_multibyte, sizeof(max_multibyte));
            break;
        }
        case 20: {  // mbrlen with guest-owned multibyte conversion state
            if (registers[1] > std::numeric_limits<std::size_t>::max()) {
                throw std::out_of_range("Guest mbrlen length exceeds host size_t");
            }
            const auto length = static_cast<std::size_t>(registers[1]);
            const auto* input = reinterpret_cast<const char*>(memory.buffer(registers[0], length));
            auto* state_bytes = memory.buffer(registers[2], sizeof(std::mbstate_t), true);
            std::mbstate_t state{};
            std::memcpy(&state, state_bytes, sizeof(state));
            const auto result = std::mbrlen(input, length, &state);
            std::memcpy(state_bytes, &state, sizeof(state));
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 25: {  // fileno
            registers[0] = normalize_stream(registers[0]);
            if (const auto found = state.files.find(registers[0]); found != state.files.end()) {
                const auto descriptor = ::fileno(found->second);
                if (descriptor < 0) set_errno(errno);
                registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(descriptor));
            } else if (registers[0] == bootstrap.stdinSlot && standard_descriptor_open(STDIN_FILENO)) {
                registers[0] = STDIN_FILENO;
            } else if (registers[0] == bootstrap.stdoutSlot && standard_descriptor_open(STDOUT_FILENO)) {
                registers[0] = STDOUT_FILENO;
            } else if (registers[0] == bootstrap.stderrSlot && standard_descriptor_open(STDERR_FILENO)) {
                registers[0] = STDERR_FILENO;
            } else {
                set_errno(EBADF);
                registers[0] = failure;
            }
            break;
        }
        case 21: {  // open guest-owned files
            const auto path = string_at(registers[0]);
            const auto flags = static_cast<std::int32_t>(registers[1]);
            constexpr std::int32_t allowed = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC |
                O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_DIRECTORY;
            if ((flags & ~allowed) != 0) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            const auto needsMode = (flags & O_CREAT) != 0;
            const auto mode = static_cast<mode_t>(registers[2]);
            const auto descriptor = needsMode
                ? ::open(path.c_str(), flags | O_CLOEXEC, mode)
                : ::open(path.c_str(), flags | O_CLOEXEC);
            if (descriptor >= 0) {
                state.openDescriptors.insert(descriptor);
                if ((flags & O_ACCMODE) != O_RDONLY) {
                    state.writableDescriptors.insert(descriptor);
                }
            } else {
                set_errno(errno);
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(descriptor));
            break;
        }
        case 230: {  // mkdirat
            std::string path;
            if (registers[1] == 0) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            try {
                path = string_at(registers[1]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto directory = static_cast<std::int32_t>(registers[0]);
            if (!owns_directory_for(directory, path)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto result = ::mkdirat(directory, path.c_str(),
                                          static_cast<mode_t>(registers[2]));
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 231: {  // linkat
            std::string oldPath;
            std::string newPath;
            if (registers[1] == 0 || registers[3] == 0) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            try {
                oldPath = string_at(registers[1]);
                newPath = string_at(registers[3]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto oldDirectory = static_cast<std::int32_t>(registers[0]);
            const auto newDirectory = static_cast<std::int32_t>(registers[2]);
            if (!owns_directory_for(oldDirectory, oldPath) ||
                !owns_directory_for(newDirectory, newPath)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto flags = static_cast<int>(registers[4]);
            if ((flags & ~AT_SYMLINK_FOLLOW) != 0) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            const auto result = ::linkat(oldDirectory, oldPath.c_str(), newDirectory,
                                         newPath.c_str(), flags);
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 232: {  // symlinkat
            std::string target;
            std::string path;
            if (registers[0] == 0 || registers[2] == 0) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            try {
                target = string_at(registers[0]);
                path = string_at(registers[2]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto directory = static_cast<std::int32_t>(registers[1]);
            if (!owns_directory_for(directory, path)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto result = ::symlinkat(target.c_str(), directory, path.c_str());
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 233: {  // mkdir
            std::string path;
            try {
                path = string_at(registers[0]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto result = ::mkdir(path.c_str(), static_cast<mode_t>(registers[1]));
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 234:  // link
        case 235: {  // symlink
            std::string first;
            std::string second;
            try {
                first = string_at(registers[0]);
                second = string_at(registers[1]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto result = service == 234
                ? ::link(first.c_str(), second.c_str())
                : ::symlink(first.c_str(), second.c_str());
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 236: {  // openat
            std::string path;
            if (registers[1] == 0) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            try {
                path = string_at(registers[1]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto directory = static_cast<std::int32_t>(registers[0]);
            if (!owns_directory_for(directory, path)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto flags = static_cast<std::int32_t>(registers[2]);
            constexpr std::int32_t allowed = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC |
                O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_DIRECTORY;
            if ((flags & ~allowed) != 0) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            int descriptor = -1;
            if ((flags & O_CREAT) != 0) {
                mode_t mode = 0;
                try {
                    if (registers[31] == 0) throw std::out_of_range("Guest stack pointer is null");
                    mode = static_cast<mode_t>(load_u64(registers[31]));
                } catch (const std::out_of_range&) {
                    set_errno(EFAULT);
                    registers[0] = failure;
                    break;
                }
                descriptor = ::openat(directory, path.c_str(), flags | O_CLOEXEC, mode);
            } else {
                descriptor = ::openat(directory, path.c_str(), flags | O_CLOEXEC);
            }
            if (descriptor >= 0) {
                state.openDescriptors.insert(descriptor);
                if ((flags & O_ACCMODE) != O_RDONLY) {
                    state.writableDescriptors.insert(descriptor);
                }
            } else {
                set_errno(errno);
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(descriptor));
            break;
        }
        case 237: {  // unlinkat
            std::string path;
            if (registers[1] == 0) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            try {
                path = string_at(registers[1]);
            } catch (const std::out_of_range&) {
                set_errno(EFAULT);
                registers[0] = failure;
                break;
            }
            const auto directory = static_cast<std::int32_t>(registers[0]);
            if (!owns_directory_for(directory, path)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto flags = static_cast<int>(registers[2]);
            if (flags != 0 && flags != AT_REMOVEDIR) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            const auto result = ::unlinkat(directory, path.c_str(), flags);
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 22: {  // close
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            std::uint64_t stream = 0;
            if (descriptor == STDIN_FILENO) stream = bootstrap.stdinSlot;
            else if (descriptor == STDOUT_FILENO) stream = bootstrap.stdoutSlot;
            else if (descriptor == STDERR_FILENO) stream = bootstrap.stderrSlot;
            if (stream != 0 && !state.closedStreams.count(stream) &&
                !owns_descriptor(descriptor)) {
                if (state.closedStreams.insert(stream).second) registers[0] = 0;
                else { set_errno(EBADF); registers[0] = failure; }
            } else if (state.openDescriptors.erase(descriptor) != 0) {
                state.socketDescriptors.erase(descriptor);
                state.writableDescriptors.erase(descriptor);
                state.standardDescriptorFlags.erase(descriptor);
                state.standardStatusFlags.erase(descriptor);
                const auto result = ::close(descriptor);
                if (result == 0) {
                    if (const auto socket = state.guestUnixSockets.find(descriptor);
                        socket != state.guestUnixSockets.end()) {
                        if (socket->second.boundPath) {
                            const auto path = state.guestUnixSocketPaths.find(
                                *socket->second.boundPath);
                            if (path != state.guestUnixSocketPaths.end() &&
                                path->second == descriptor) {
                                path->second = guestUnixClosedEndpoint;
                            }
                        }
                        for (auto& peer : state.guestUnixSockets) {
                            if (peer.second.connectedPeer == descriptor) {
                                peer.second.connectedPeer.reset();
                                peer.second.connectedPeerClosed = true;
                            }
                        }
                        state.guestUnixSockets.erase(socket);
                    }
                }
                if (result < 0) set_errno(errno);
                registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            } else {
                set_errno(EBADF);
                registers[0] = failure;
            }
            break;
        }
        case 23: {  // read
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            if (state.socketDescriptors.count(descriptor) != 0) {
                set_errno(ENOTSUP);
                registers[0] = failure;
                break;
            }
            const bool readable_standard = descriptor == STDIN_FILENO &&
                                           standard_descriptor_open(descriptor);
            if (!readable_standard && !owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            if (registers[2] > std::numeric_limits<std::size_t>::max()) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            auto* destination = memory.buffer(registers[1], registers[2], true);
            if (registers[2] != 0 && (status_flags(descriptor) & O_NONBLOCK) != 0 &&
                !ready_for_io(descriptor, POLLIN)) {
                set_errno(EAGAIN);
                registers[0] = failure;
                break;
            }
            const auto result = ::read(descriptor, destination, static_cast<std::size_t>(registers[2]));
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 24: {  // fstat
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            if (!standard_descriptor_open(descriptor) && !owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            struct stat info{};
            const auto result = ::fstat(descriptor, &info);
            if (result == 0) {
                std::memcpy(memory.buffer(registers[1], sizeof(info), true), &info, sizeof(info));
            } else set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 34: {  // fcntl
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            if (!standard_descriptor_open(descriptor) && !owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto operation = static_cast<int>(registers[1]);
            int result = -1;
            if (operation == F_GETFD) {
                const auto found = state.standardDescriptorFlags.find(descriptor);
                result = found == state.standardDescriptorFlags.end()
                    ? ::fcntl(descriptor, operation) : found->second;
            } else if (operation == F_GETFL) {
                const auto native = ::fcntl(descriptor, operation);
                if (native >= 0) {
                    const auto found = state.standardStatusFlags.find(descriptor);
                    result = found == state.standardStatusFlags.end() ? native
                        : ((native & ~O_NONBLOCK) | (found->second & O_NONBLOCK));
                } else result = native;
            } else if (operation == F_GETPATH) {
                std::array<char, MAXPATHLEN> hostPath{};
                try {
                    const auto guestPathAddress = load_u64(registers[31]);
                    result = ::fcntl(descriptor, operation, hostPath.data());
                    if (result >= 0) {
                        const auto* terminator = static_cast<const char*>(
                            std::memchr(hostPath.data(), 0, hostPath.size()));
                        if (terminator == nullptr) {
                            result = -1;
                            errno = ENAMETOOLONG;
                        } else {
                            const auto pathBytes = static_cast<std::size_t>(
                                terminator - hostPath.data()) + 1;
                            if (guestPathAddress == 0) {
                                result = -1;
                                errno = EFAULT;
                            } else {
                                auto* guestPath = memory.buffer(
                                    guestPathAddress, pathBytes, true);
                                std::memcpy(guestPath, hostPath.data(), pathBytes);
                            }
                        }
                    }
                } catch (const std::out_of_range&) {
                    result = -1;
                    errno = EFAULT;
                }
            } else if (operation == F_SETFD || operation == F_SETFL) {
                std::int32_t value = 0;
                std::memcpy(&value, memory.buffer(registers[31], sizeof(value)), sizeof(value));
                const bool standard = standard_descriptor_open(descriptor);
                if (!standard) {
                    result = ::fcntl(descriptor, operation, value);
                } else if (operation == F_SETFD) {
                    state.standardDescriptorFlags[descriptor] = value;
                    result = 0;
                } else {
                    const auto native = ::fcntl(descriptor, F_GETFL);
                    if (native < 0) result = native;
                    else if (((native ^ value) & (O_APPEND | O_ASYNC)) != 0) {
                        throw std::runtime_error(
                            "Guest cannot change append or async mode on an inherited descriptor");
                    } else {
                        state.standardStatusFlags[descriptor] = value;
                        result = 0;
                    }
                }
            } else {
                const char* traceFcntl = std::getenv("ARM64_METAL_TRACE_FCNTL");
                if (traceFcntl != nullptr && std::strcmp(traceFcntl, "1") == 0) {
                    std::uint64_t stackArgument = 0;
                    const char* stackArgumentStatus = "unreadable";
                    try {
                        std::memcpy(&stackArgument,
                            memory.buffer(registers[31], sizeof(stackArgument)),
                            sizeof(stackArgument));
                        stackArgumentStatus = "numeric";
                    } catch (const std::out_of_range&) {
                        // Diagnostics must not change the original failure.
                    }
                    std::fprintf(stderr,
                        "arm64-metal fcntl-trace entry=named pc=%s lr=%s fd=%d "
                        "cmd=%d arg_x2=%s sp=%s stack_arg_%s=%s\n",
                        hex_word(registers[33]).c_str(), hex_word(registers[30]).c_str(),
                        descriptor, operation, hex_word(registers[2]).c_str(),
                        hex_word(registers[31]).c_str(), stackArgumentStatus,
                        hex_word(stackArgument).c_str());
                }
                throw std::runtime_error("Guest fcntl command unsupported");
            }
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 35: {  // isatty
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            if (!standard_descriptor_open(descriptor) && !owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = service == 34 ? failure : 0;
                break;
            }
            const auto result = ::isatty(descriptor);
            if (result == 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 42: {  // pipe
            int descriptors[2] = {-1, -1};
            const auto result = ::pipe(descriptors);
            if (result == 0) {
                state.openDescriptors.insert(descriptors[0]);
                state.openDescriptors.insert(descriptors[1]);
                state.writableDescriptors.insert(descriptors[1]);
                auto* output = memory.buffer(registers[0], sizeof(descriptors), true);
                std::memcpy(output, descriptors, sizeof(descriptors));
            } else set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 86: {  // lseek
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            if (!standard_descriptor_open(descriptor) && !owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto result = ::lseek(descriptor, static_cast<off_t>(
                static_cast<std::int64_t>(registers[1])), static_cast<int>(registers[2]));
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 43:  // sigaction
        case 91: {  // signal
            const auto signal = static_cast<std::int32_t>(registers[0]);
            const bool simple = service == 91;
            const auto requested = registers[1];
            const auto previous = simple ? 0 : registers[2];
            const auto failure = simple ? std::numeric_limits<std::uint64_t>::max()
                                        : std::numeric_limits<std::uint32_t>::max();
            if (!GuestSignals::valid(signal) ||
                ((signal == SIGKILL || signal == SIGSTOP) && (simple || requested != 0))) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            const auto old = state.signals.action(signal);
            std::optional<GuestSignals::Action> action;
            if (simple) {
                action = GuestSignals::Action{requested, 0, SA_RESTART};
            } else if (requested != 0) {
                std::uint64_t handler = 0;
                std::uint64_t packed = 0;
                std::memcpy(&handler, memory.buffer(requested, sizeof(handler)), sizeof(handler));
                std::memcpy(&packed, memory.buffer(requested + 8, sizeof(packed)), sizeof(packed));
                GuestSignals::Action next{handler, static_cast<std::uint32_t>(packed),
                    static_cast<std::int32_t>(static_cast<std::uint32_t>(packed >> 32U))};
                constexpr std::int32_t supported = SA_RESTART | SA_RESETHAND |
                                                   SA_NODEFER | SA_SIGINFO;
                if ((next.flags & ~supported) != 0) {
                    throw std::runtime_error("Guest sigaction flags unsupported");
                }
                action = next;
            }
            if (previous != 0) {
                const std::uint64_t packed = static_cast<std::uint64_t>(old.mask) |
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(old.flags)) << 32U);
                auto* output = memory.buffer(previous, 16, true);
                std::memcpy(output, &old.handler, sizeof(old.handler));
                std::memcpy(output + 8, &packed, sizeof(packed));
            }
            if (action) {
                if (!host_signal_supported(signal) &&
                    !synchronous_signal_supported(signal)) {
                    set_errno(ENOTSUP);
                    registers[0] = failure;
                    break;
                }
                if (host_signal_supported(signal) &&
                    guest_signal_listen(signal, action->handler == 1 ? 1 : 0,
                                        (action->flags & SA_RESTART) != 0 ? 1 : 0) != 0) {
                    set_errno(errno);
                    registers[0] = failure;
                    break;
                }
                state.signals.set_action(*action, signal);
                if (action->handler == 1) {
                    state.queuedProcessSignals &= ~GuestSignals::bit(signal);
                    state.threadPendingSignals &= ~GuestSignals::bit(signal);
                }
            }
            registers[0] = simple ? old.handler : 0;
            break;
        }
        case 44:  // sigprocmask
        case 45: {  // pthread_sigmask
            const auto how = static_cast<std::int32_t>(registers[0]);
            const auto requested = registers[1];
            const auto previous = registers[2];
            const auto old_mask = state.threadSignalMask;
            if (requested != 0) {
                std::uint32_t value = 0;
                std::memcpy(&value, memory.buffer(requested, sizeof(value)), sizeof(value));
                if (!state.signals.set_mask(how, value)) {
                    if (service == 44) set_errno(EINVAL);
                    registers[0] = service == 44 ? std::numeric_limits<std::uint32_t>::max()
                                                 : static_cast<std::uint64_t>(EINVAL);
                    break;
                }
                state.threadSignalMask = state.signals.mask();
            }
            if (previous != 0) {
                std::memcpy(memory.buffer(previous, sizeof(old_mask), true),
                            &old_mask, sizeof(old_mask));
            }
            registers[0] = 0;
            break;
        }
        case 46:  // raise
        case 47: {  // kill only supports the guest process
            const bool is_kill = service == 47;
            const auto signal = static_cast<std::int32_t>(is_kill ? registers[1] : registers[0]);
            if (is_kill && static_cast<std::int32_t>(registers[0]) != ::getpid()) {
                throw std::runtime_error("Guest kill only supports the guest process");
            }
            if (!GuestSignals::valid(signal) && !(is_kill && signal == 0)) {
                set_errno(EINVAL);
                registers[0] = std::numeric_limits<std::uint32_t>::max();
                break;
            }
            if (signal != 0) {
                if (is_kill) {
                    state.signals.raise(signal);
                    if (state.signals.action(signal).handler != 1) {
                        state.queuedProcessSignals |= GuestSignals::bit(signal);
                    }
                }
                else if (state.signals.action(signal).handler != 1) {
                    state.threadPendingSignals |= GuestSignals::bit(signal);
                }
            }
            registers[0] = 0;
            break;
        }
        case 104:  // shm_open with mode on guest stack
        case 105:  // alternate shm_open ABI with mode in x2
        case 106: {  // shm_unlink
            const auto name = string_at(registers[0]);
            const auto flags = static_cast<std::int32_t>(registers[1]);
            int result = -1;
            if (service == 106) {
                result = ::shm_unlink(name.c_str());
            } else {
                std::int32_t mode = 0;
                if ((flags & O_CREAT) != 0) {
                    if (service == 104) {
                        std::memcpy(&mode, memory.buffer(registers[31], sizeof(mode)), sizeof(mode));
                    } else {
                        mode = static_cast<std::int32_t>(registers[2]);
                    }
                }
                result = guest_shm_open(name.c_str(), flags, mode);
                if (result >= 0) {
                    state.openDescriptors.insert(result);
                    if ((flags & O_ACCMODE) != O_RDONLY) state.writableDescriptors.insert(result);
                }
            }
            if (result < 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 107: {  // anonymous guest mappings and read-only file-backed mappings
            const auto page_size = bootstrap.pageSize;
            const auto flags = static_cast<std::uint32_t>(registers[3]);
            const auto jit_flag = static_cast<std::uint32_t>(MAP_JIT);
            const auto no_reserve_flag = static_cast<std::uint32_t>(MAP_NORESERVE);
            const bool jit_mapping = (flags & jit_flag) != 0;
            auto anonymous_flags = flags & ~jit_flag;
            if (jit_mapping) anonymous_flags &= ~no_reserve_flag;
            if (jit_mapping && anonymous_flags != (MAP_PRIVATE | MAP_ANON)) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            if (anonymous_flags == (MAP_PRIVATE | MAP_ANON)) {
                const auto protection = static_cast<std::uint32_t>(registers[2]);
                if (page_size == 0 || (page_size & (page_size - 1)) != 0 ||
                    registers[0] != 0 || registers[1] == 0 ||
                    registers[1] > std::numeric_limits<std::uint64_t>::max() - (page_size - 1) ||
                    (protection & ~7U) != 0 ||
                    (jit_mapping && protection != 0 && protection != 7U) ||
                    static_cast<std::int32_t>(registers[4]) != -1 || registers[5] != 0) {
                    set_errno(EINVAL);
                    registers[0] = failure;
                    break;
                }
                if (jit_mapping && !state.guestMapJitEligible) {
                    set_errno(EINVAL);
                    registers[0] = failure;
                    break;
                }
                if (!jit_mapping && (protection & 2U) != 0 && (protection & 4U) != 0) {
                    set_errno(EACCES);
                    registers[0] = failure;
                    break;
                }
                if (!state.machVmAllocate) {
                    set_errno(ENOMEM);
                    registers[0] = failure;
                    break;
                }
                const auto length = (registers[1] + page_size - 1) & ~(page_size - 1);
                std::uint64_t address = 0;
                const std::uint32_t effective_protection = jit_mapping
                    ? (protection == 0 ? 0U : (state.jitWriteProtected ? 5U : 3U))
                    : protection;
                const auto result = state.machVmAllocate(length, page_size,
                    effective_protection, 7, address, jit_mapping, 1, 0);
                if (result != 0) {
                    set_errno(ENOMEM);
                    registers[0] = failure;
                    break;
                }
                registers[0] = address;
                break;
            }
            const auto descriptor = static_cast<std::int32_t>(registers[4]);
            const auto file_offset = static_cast<std::int64_t>(registers[5]);
            if (!owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            if (registers[0] != 0 || registers[1] == 0 ||
                registers[1] > std::numeric_limits<std::size_t>::max() ||
                registers[2] != PROT_READ || registers[3] != MAP_SHARED || file_offset < 0 ||
                page_size == 0 || static_cast<std::uint64_t>(file_offset) % page_size != 0) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            struct stat info{};
            if (::fstat(descriptor, &info) != 0) {
                set_errno(errno);
                registers[0] = failure;
                break;
            }
            if (file_offset > info.st_size ||
                registers[1] > static_cast<std::uint64_t>(info.st_size - file_offset)) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            void* host_address = ::mmap(nullptr, static_cast<std::size_t>(registers[1]),
                                        PROT_READ, MAP_SHARED, descriptor,
                                        static_cast<off_t>(file_offset));
            if (host_address == MAP_FAILED) {
                set_errno(errno);
                registers[0] = failure;
                break;
            }
            const auto allocation = allocate_guest(registers[1],
                static_cast<std::int64_t>(page_size));
            if (!allocation) {
                ::munmap(host_address, static_cast<std::size_t>(registers[1]));
                set_errno(ENOMEM);
                registers[0] = failure;
                break;
            }
            const auto address = static_cast<std::uint64_t>(*allocation);
            std::memcpy(memory.buffer(address, registers[1], true), host_address,
                        static_cast<std::size_t>(registers[1]));
            memory.protect_readonly({address, address + registers[1]});
            state.mappedFiles.emplace(address,
                HostServiceState::MappedFile{registers[1], *allocation, host_address});
            registers[0] = address;
            break;
        }
        case 108: {  // munmap
            const auto page_size = bootstrap.pageSize;
            if (page_size != 0 && (page_size & (page_size - 1)) == 0 && registers[1] != 0 &&
                registers[1] <= std::numeric_limits<std::uint64_t>::max() - (page_size - 1) &&
                state.machVmDeallocate) {
                const auto length = (registers[1] + page_size - 1) & ~(page_size - 1);
                if (state.machVmDeallocate(registers[0], length) == 0) {
                    registers[0] = 0;
                    break;
                }
            }
            const auto found = state.mappedFiles.find(registers[0]);
            if (found == state.mappedFiles.end() || found->second.size != registers[1]) {
                set_errno(EINVAL);
                registers[0] = failure;
                break;
            }
            const auto result = ::munmap(found->second.hostAddress,
                                         static_cast<std::size_t>(found->second.size));
            if (result != 0) {
                set_errno(errno);
                registers[0] = failure;
                break;
            }
            memory.unprotect_readonly({registers[0], registers[0] + registers[1]});
            guest_heap().release(found->second.allocation);
            state.mappedFiles.erase(found);
            registers[0] = 0;
            break;
        }
        case 109: {  // ftruncate
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            if (!owns_descriptor(descriptor)) {
                set_errno(EBADF);
                registers[0] = failure;
                break;
            }
            const auto result = ::ftruncate(descriptor, static_cast<off_t>(registers[1]));
            if (result != 0) set_errno(errno);
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 30:  // getprogname
            registers[0] = bootstrap.programName;
            break;
        case 32: {  // ___chkstk_darwin: validate the probed stack extent
            if (registers[31] < bootstrap.stack.begin || registers[31] > bootstrap.stack.end ||
                registers[9] > registers[31] - bootstrap.stack.begin) {
                throw std::runtime_error("Guest stack probe failed");
            }
            break;
        }
        case 36:  // getuid
            registers[0] = static_cast<std::uint64_t>(::getuid());
            break;
        case 37:  // geteuid
            registers[0] = static_cast<std::uint64_t>(::geteuid());
            break;
        case 38:  // getgid
            registers[0] = static_cast<std::uint64_t>(::getgid());
            break;
        case 39:  // getegid
            registers[0] = static_cast<std::uint64_t>(::getegid());
            break;
        case 41: {  // getrlimit
            struct rlimit limit {};
            const auto resource = static_cast<int>(static_cast<std::int32_t>(registers[0]));
            const auto host_result = ::getrlimit(resource, &limit);
            const auto host_error = errno;
            int result = host_result;
            if (host_result == 0) {
                try {
                    if (registers[1] == 0) throw std::out_of_range("null guest rlimit pointer");
                    auto* output = memory.buffer(registers[1], sizeof(limit), true);
                    std::memcpy(output, &limit, sizeof(limit));
                } catch (const std::out_of_range&) {
                    set_errno(EFAULT);
                    result = -1;
                }
            } else {
                set_errno(host_error);
            }
            registers[0] = static_cast<std::uint32_t>(result);
            break;
        }
        case 51: {  // strncmp
            const auto count = registers[2];
            const auto* left = memory.buffer(registers[0], count);
            const auto* right = memory.buffer(registers[1], count);
            int result = 0;
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto a = left[index];
                const auto b = right[index];
                if (a != b) {
                    result = static_cast<int>(a) - static_cast<int>(b);
                    break;
                }
                if (a == 0) break;
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 52: {  // nanosleep
            timespec request{};
            std::memcpy(&request, memory.buffer(registers[0], sizeof(request)),
                        sizeof(request));
            timespec remainder{};
            std::uint8_t* remainder_pointer = nullptr;
            if (registers[1] != 0) {
                remainder_pointer = memory.buffer(registers[1], sizeof(remainder), true);
            }
            const auto result = ::nanosleep(&request, &remainder);
            if (result != 0) {
                const auto error = errno;
                if (error == EINTR && remainder_pointer != nullptr) {
                    std::memcpy(remainder_pointer, &remainder, sizeof(remainder));
                }
                set_errno(error);
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 75:  // sched_yield
            registers[0] = 0;
            break;
        case 53: {  // strstr
            const auto haystack = string_at(registers[0]);
            const auto needle = string_at(registers[1]);
            const auto position = haystack.find(needle);
            registers[0] = position == std::string::npos ? 0 : registers[0] + position;
            break;
        }
        case 58: {  // mach_timebase_info
            mach_timebase_info_data_t info{};
            const auto result = mach_timebase_info(&info);
            if (result == KERN_SUCCESS) {
                std::memcpy(memory.buffer(registers[0], sizeof(info), true), &info, sizeof(info));
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 59:  // mach_absolute_time
            registers[0] = mach_absolute_time();
            break;
        case 60:  // clock
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::clock()));
            break;
        case 61: {  // clock_gettime
            timespec time{};
            const auto result = ::clock_gettime(static_cast<clockid_t>(registers[0]), &time);
            if (result == 0) {
                std::memcpy(memory.buffer(registers[1], sizeof(time), true), &time, sizeof(time));
            } else if (bootstrap.errnoSlot != 0) {
                const auto host_error = static_cast<std::uint64_t>(errno);
                std::memcpy(memory.buffer(bootstrap.errnoSlot, sizeof(host_error), true),
                            &host_error, sizeof(host_error));
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 141:  // dyld_process_is_restricted
            registers[0] = 0;
            break;
        case 147:  // dyld Swift optimization API version
            registers[0] = 1;
            break;
        case 148:  // no process-wide prebuilt Swift conformance set
            registers[0] = 0;
            break;
        case 137:  // mach_continuous_time
            registers[0] = mach_continuous_time();
            break;
        case 138: {  // __os_feature_enabled_impl
            const auto domain = string_at(registers[0]);
            const auto feature = string_at(registers[1]);
            const auto enabled = guest_feature_enabled(domain.c_str(), feature.c_str());
            if (enabled < 0) {
                throw std::runtime_error("Guest OS feature query is unavailable");
            }
            registers[0] = enabled == 0 ? 0 : 1;
            break;
        }
        case 19: {  // strcmp (unsigned byte ordering, as in the Swift guest bridge)
            const auto left = string_at(registers[0]);
            const auto right = string_at(registers[1]);
            const auto mismatch = std::mismatch(left.begin(), left.end(), right.begin(), right.end());
            int result = 0;
            if (mismatch.first != left.end() && mismatch.second != right.end()) {
                result = static_cast<unsigned char>(*mismatch.first) -
                         static_cast<unsigned char>(*mismatch.second);
            } else if (mismatch.first == left.end() && mismatch.second != right.end()) {
                result = -static_cast<unsigned char>(*mismatch.second);
            } else if (mismatch.first != left.end() && mismatch.second == right.end()) {
                result = static_cast<unsigned char>(*mismatch.first);
            }
            registers[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
            break;
        }
        case 161: {  // access
            std::string path;
            if (registers[0] < bootstrap.pageSize) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            try {
                path = string_at(registers[0]);
            } catch (const std::out_of_range&) {
                registers[0] = failure;
                set_errno(EFAULT);
                break;
            }
            const auto result = ::access(path.c_str(), static_cast<int>(registers[1]));
            registers[0] = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(result));
            if (result < 0) set_errno(errno);
            break;
        }
        case 175: {  // Darwin SYS_gettid: per-thread override identity, not thread ID
            // Guest threads currently have no assumed identity. XNU returns
            // ESRCH before inspecting either output pointer in that state.
            // Host pthread credentials do not describe the guest thread.
            registers[0] = ESRCH;
            registers[32] |= 8U;
            break;
        }
        case 173: {  // Darwin SYS_csrctl syscall boundary
            const auto operation = static_cast<std::uint32_t>(registers[0]);
            const auto address = registers[1];
            registers[32] |= 8U;
            if (operation > 1) {
                registers[0] = ENOSYS;
                break;
            }
            if (address == 0 || registers[2] != sizeof(std::uint32_t)) {
                registers[0] = EINVAL;
                break;
            }
            if (address < bootstrap.pageSize) {
                registers[0] = EFAULT;
                break;
            }
            std::uint8_t* data = nullptr;
            try {
                data = memory.buffer(address, sizeof(std::uint32_t), operation == 1);
            } catch (const std::out_of_range&) {
                registers[0] = EFAULT;
                break;
            }
            std::uint32_t value = 0;
            if (operation == 0) std::memcpy(&value, data, sizeof(value));
            const int result = operation == 0 ? ::csr_check(value) : ::csr_get_active_config(&value);
            if (result != 0) {
                registers[0] = static_cast<std::uint32_t>(errno);
                break;
            }
            if (operation == 1) std::memcpy(data, &value, sizeof(value));
            registers[0] = 0;
            registers[32] &= ~std::uint64_t{8};
            break;
        }
        case 171: {  // Darwin SYS_lstat64 syscall boundary
            registers[32] |= 8U;
            if (registers[0] < bootstrap.pageSize) {
                registers[0] = EFAULT;
                break;
            }
            std::string path;
            try {
                path = string_at(registers[0]);
            } catch (const std::out_of_range&) {
                registers[0] = EFAULT;
                break;
            }
            struct stat info{};
            if (::lstat(path.c_str(), &info) != 0) {
                registers[0] = static_cast<std::uint32_t>(errno);
                break;
            }
            if (registers[1] == 0 || registers[1] < bootstrap.pageSize) {
                registers[0] = EFAULT;
                break;
            }
            try {
                auto* output = memory.buffer(registers[1], sizeof(info), true);
                std::memcpy(output, &info, sizeof(info));
            } catch (const std::out_of_range&) {
                registers[0] = EFAULT;
                break;
            }
            registers[0] = 0;
            registers[32] &= ~std::uint64_t{8};
            break;
        }
        default:
            throw std::runtime_error("Unsupported guest host service " + std::to_string(service));
    }
    return {};
}

}  // namespace arm64metal
