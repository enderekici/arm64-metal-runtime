#pragma once

#include <string_view>

namespace arm64metal {

inline constexpr std::string_view guestMemoryMslSource = R"GUEST_MEMORY_MSL(
// Darwin permits misaligned RCpc2 and LSE min/max accesses within one
// 16-byte granule; an access that crosses the granule raises an alignment fault.
bool rcpc2_alignment_fault(ulong address, uint bytes, bool write, ulong pc,
                           device ulong *state) {
    if ((address & 15ul) + bytes <= 16ul) return false;
    state[874] = address;
    state[877] = bytes;
    state[33] = pc;
    state[34] = write ? 20ul : 19ul;
    return true;
}
ulong load_guest_word(device const uchar *memory, uint count) {
    if (count == 8 && (ulong(memory) & 7ul) == 0)
        return *((device const ulong *)memory);
    if (count == 4 && (ulong(memory) & 3ul) == 0)
        return *((device const uint *)memory);
    ulong word = 0;
    for (uint byte = 0; byte < count; ++byte)
        word |= ulong(memory[byte]) << (8 * byte);
    return word;
}
void store_guest_word(device uchar *memory, ulong word, uint count) {
    if (count == 8 && (ulong(memory) & 7ul) == 0) {
        *((device ulong *)memory) = word;
        return;
    }
    if (count == 4 && (ulong(memory) & 3ul) == 0) {
        *((device uint *)memory) = uint(word);
        return;
    }
    for (uint byte = 0; byte < count; ++byte)
        memory[byte] = uchar(word >> (8 * byte));
}
bool guest_write(ulong address, ulong count, device const ulong *writable) {
    if (address >= writable[0]) return true;
    for (ulong index = 0; index < writable[1]; ++index) {
        device const ulong *range = writable + 2 + 2 * index;
        if (address >= range[0] && address < range[1] &&
            count <= range[1] - address) return true;
    }
    return false;
}
// The writable-range header stays stable for guest_write(). Denied page
// intervals are appended after those writable ranges as a count followed by
// (begin, end, protection) triples using Darwin VM_PROT_READ/WRITE bits.
bool guest_page_access_allowed(ulong address, ulong count,
                               device const ulong *writable, bool write) {
    if (count == 0) return true;
    if (count > ~0ul - address) return false;
    const ulong requested = write ? 2ul : 1ul;
    const ulong rangeCount = writable[1];
    const ulong permissionHeader = 2ul + 2ul * rangeCount;
    const ulong permissionCount = writable[permissionHeader];
    const ulong end = address + count;
    ulong low = 0;
    ulong high = permissionCount;
    while (low < high) {
        const ulong middle = low + (high - low) / 2ul;
        device const ulong *entry = writable + permissionHeader + 1ul + 3ul * middle;
        if (entry[1] <= address) low = middle + 1ul;
        else high = middle;
    }
    for (ulong index = low; index < permissionCount; ++index) {
        device const ulong *entry = writable + permissionHeader + 1ul + 3ul * index;
        if (entry[0] >= end) break;
        if ((entry[2] & requested) == 0) return false;
    }
    return true;
}
ulong guest_resident_offset(ulong address, ulong count,
                            device const ulong *resident, bool write) {
    if (count > ~0ul - address) return ~0ul;
    if (count == 0) {
        for (ulong index = 0; index < resident[0]; ++index) {
            device const ulong *entry = resident + 1ul + 4ul * index;
            if (address < entry[0] || address >= entry[1]) continue;
            if ((entry[3] & 2ul) != 0 || (write && (entry[3] & 1ul) == 0)) return ~0ul;
            return entry[2] + address - entry[0];
        }
        return ~0ul;
    }
    ulong cursor = address;
    ulong remaining = count;
    ulong offset = ~0ul;
    ulong expectedBacking = ~0ul;
    while (remaining != 0) {
        device const ulong *entry = nullptr;
        for (ulong index = 0; index < resident[0]; ++index) {
            device const ulong *candidate = resident + 1ul + 4ul * index;
            if (cursor >= candidate[0] && cursor < candidate[1]) {
                entry = candidate;
                break;
            }
        }
        if (entry == nullptr || (entry[3] & 2ul) != 0 ||
            (write && (entry[3] & 1ul) == 0)) return ~0ul;
        const ulong backing = entry[2] + cursor - entry[0];
        if (offset == ~0ul) offset = backing;
        else if (backing != expectedBacking) return ~0ul;
        const ulong chunk = min(remaining, entry[1] - cursor);
        expectedBacking = backing + chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return offset;
}
ulong guest_offset(ulong address, ulong count, ulong size,
                   device const ulong *writable, bool write,
                   device ulong *state, device const ulong *resident,
                   device const ulong *config) {
    // Darwin arm64 user data accesses ignore the top byte (TBI); objc now
    // tags heap method lists with bit 63 and stores through them.
    address &= 0x00fffffffffffffful;
    const ulong minimumImageAddress = config[0];
    const ulong sharedGuestBase = config[1];
    const ulong linearImageEnd = config[2];
    const ulong mappingBackingEnd = config[3];
    if (!guest_page_access_allowed(address, count, writable, write)) return ~0ul;
    if (address < sharedGuestBase) {
        if (address >= linearImageEnd && count != 0) {
            for (ulong i = 0; i < resident[0]; ++i) {
                device const ulong *entry = resident + 1 + 4 * i;
                if ((entry[3] & 2ul) != 0 && address < entry[1] &&
                    (address >= entry[0] || entry[0] - address < count))
                    return ~0ul;
            }
        }
        if (address >= linearImageEnd) {
            bool startsMapped = false;
            for (ulong i = 0; i < resident[0]; ++i) {
                device const ulong *entry = resident + 1 + 4 * i;
                if (address >= entry[0] && address < entry[1]) {
                    startsMapped = true;
                    break;
                }
            }
            if (startsMapped) return guest_resident_offset(address, count, resident, write);
            if (count != 0) {
                for (ulong i = 0; i < resident[0]; ++i) {
                    device const ulong *entry = resident + 1 + 4 * i;
                    if (address < entry[0] && entry[0] - address < count) return ~0ul;
                }
            }
        }
        const ulong limit = address < linearImageEnd ? linearImageEnd : size;
        if ((address < minimumImageAddress && count != 0) ||
            (address >= linearImageEnd && address < mappingBackingEnd) ||
            address > limit || count > limit - address ||
            (write && !guest_write(address, count, writable))) return ~0ul;
        return address;
    }
    const ulong mapped = guest_resident_offset(address, count, resident, write);
    if (mapped != ~0ul) return mapped;
    if (state[875] == ~0ul) {
        state[875] = address;
        state[877] = count;
    }
    return ~0ul;
}
)GUEST_MEMORY_MSL";

}  // namespace arm64metal
