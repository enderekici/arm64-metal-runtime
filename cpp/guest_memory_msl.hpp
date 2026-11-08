#pragma once

#include <string_view>

namespace arm64metal {

inline constexpr std::string_view guestMemoryMslSource = R"GUEST_MEMORY_MSL(
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
ulong guest_offset(ulong address, ulong count, ulong size,
                   device const ulong *writable, bool write,
                   device ulong *state, device const ulong *resident,
                   device const ulong *config) {
    const ulong minimumImageAddress = config[0];
    const ulong sharedGuestBase = config[1];
    const ulong linearImageEnd = config[2];
    const ulong mappingBackingEnd = config[3];
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
            for (ulong i = 0; i < resident[0]; ++i) {
                device const ulong *entry = resident + 1 + 4 * i;
                if (address >= entry[0] && address < entry[1]) {
                    if ((entry[3] & 2ul) != 0 || (write && (entry[3] & 1ul) == 0) ||
                        count > entry[1] - address) return ~0ul;
                    return entry[2] + address - entry[0];
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
    for (ulong i = 0; i < resident[0]; ++i) {
        device const ulong *entry = resident + 1 + 4 * i;
        if (address >= entry[0] && address < entry[1]) {
            if ((entry[3] & 2ul) != 0 || (write && (entry[3] & 1ul) == 0)) return ~0ul;
            if (count <= entry[1] - address) return entry[2] + address - entry[0];
        }
    }
    if (state[875] == ~0ul) {
        state[875] = address;
        state[877] = count;
    }
    return ~0ul;
}
)GUEST_MEMORY_MSL";

}  // namespace arm64metal
