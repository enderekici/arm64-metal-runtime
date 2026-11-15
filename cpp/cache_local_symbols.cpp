#include "cache_local_symbols.hpp"

#include <cstring>
#include <vector>

namespace arm64metal {
namespace {

std::uint64_t word(const std::vector<std::uint8_t>& data, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    std::memcpy(&value, data.data() + offset, width);
    return value;
}

}  // namespace

std::optional<std::int64_t> cache_symtab_symbol(const SharedCache& cache,
                                                const std::string& path,
                                                const std::string& name) {
    const auto found = cache.images().find(path);
    if (found == cache.images().end()) return std::nullopt;
    const auto address = found->second;
    const auto header = cache.bytes(address, 32);
    if (word(header, 0, 4) != 0xfeedfacf) return std::nullopt;
    const auto count = word(header, 16, 4);
    const auto commandSize = static_cast<std::size_t>(word(header, 20, 4));
    const auto commands = cache.bytes(address + 32, commandSize);
    std::uint64_t linkeditAddress = 0, linkeditFile = 0;
    std::uint64_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    bool haveLinkedit = false, haveSymtab = false;
    std::size_t cursor = 0;
    for (std::uint64_t index = 0; index < count && cursor + 8 <= commandSize; ++index) {
        const auto command = word(commands, cursor, 4);
        const auto size = static_cast<std::size_t>(word(commands, cursor + 4, 4));
        if (size < 8 || size > commandSize - cursor) return std::nullopt;
        if (command == 0x19 && size >= 72 &&
            std::strncmp(reinterpret_cast<const char*>(commands.data() + cursor + 8),
                         "__LINKEDIT", 16) == 0) {
            linkeditAddress = word(commands, cursor + 24, 8);
            linkeditFile = word(commands, cursor + 40, 8);
            haveLinkedit = true;
        } else if (command == 0x2 && size >= 24) {
            symoff = word(commands, cursor + 8, 4);
            nsyms = word(commands, cursor + 12, 4);
            stroff = word(commands, cursor + 16, 4);
            strsize = word(commands, cursor + 20, 4);
            haveSymtab = true;
        }
        cursor += size;
    }
    if (!haveLinkedit || !haveSymtab || nsyms == 0 || strsize == 0) return std::nullopt;
    const auto symbols = cache.bytes(linkeditAddress + (symoff - linkeditFile), nsyms * 16);
    const auto strings = cache.bytes(linkeditAddress + (stroff - linkeditFile), strsize);
    for (std::uint64_t index = 0; index < nsyms; ++index) {
        const auto strx = word(symbols, index * 16, 4);
        const auto type = symbols[index * 16 + 4];
        if ((type & 0xe0) != 0 || (type & 0x0e) != 0x0e || strx >= strsize) continue;
        if (std::strncmp(reinterpret_cast<const char*>(strings.data() + strx), name.c_str(),
                         strsize - strx) != 0) continue;
        const auto value = word(symbols, index * 16 + 8, 8);
        return cache.guestAddress(value);
    }
    return std::nullopt;
}

}  // namespace arm64metal
