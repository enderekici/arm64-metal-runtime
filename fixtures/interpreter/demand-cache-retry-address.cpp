#include "shared_cache.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const bool scalarLoad = argc == 2 && std::string_view(argv[1]) == "scalar";
    if (argc > 2 || (argc == 2 && !scalarLoad)) return 2;
    arm64metal::SharedCache cache;
    const auto symbol = cache.resolveExport("/usr/lib/libz.1.dylib", "_zlibVersion");
    if (!symbol || symbol->guest_address <= 0) return 3;

    const auto address = static_cast<std::uint64_t>(symbol->guest_address);
    std::cout << ".text\n"
                 ".globl _main\n"
                 ".p2align 2\n"
                 "_main:\n"
              << "    movz x0, #0x" << std::hex << (address & 0xffffU) << "\n";
    for (unsigned shift = 16; shift < 64; shift += 16) {
        const auto part = (address >> shift) & 0xffffU;
        if (part == 0) continue;
        std::cout << "    movk x0, #0x" << std::hex << part
                  << ", lsl #" << std::dec << shift << "\n";
    }
    std::cout << (scalarLoad ? "    ldr x0, [x0]\n" : "    ldp x0, x1, [x0]\n")
              << "    ret\n"
              << ".section __DATA,__data\n"
              << ".p2align 3\n"
              << ".quad _zlibVersion\n";
    return std::cout ? 0 : 4;
}
