#include "shared_cache.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const bool scalarLoad = argc == 2 && std::string_view(argv[1]) == "scalar";
    const bool indexedLoad = argc == 2 && std::string_view(argv[1]) == "indexed";
    const bool byteLoad = argc == 2 && std::string_view(argv[1]) == "byte";
    const bool halfLoad = argc == 2 && std::string_view(argv[1]) == "half";
    const bool signedByteLoad = argc == 2 && std::string_view(argv[1]) == "signed-byte";
    const bool signedHalfLoad = argc == 2 && std::string_view(argv[1]) == "signed-half";
    const bool signedWordLoad = argc == 2 && std::string_view(argv[1]) == "signed-word";
    const bool vectorPostLoad = argc == 2 && std::string_view(argv[1]) == "vector-post";
    const bool vectorPreLoad = argc == 2 && std::string_view(argv[1]) == "vector-pre";
    if (argc > 2 || (argc == 2 && !scalarLoad && !indexedLoad &&
                     !byteLoad && !halfLoad && !signedByteLoad &&
                     !signedHalfLoad && !signedWordLoad && !vectorPostLoad &&
                     !vectorPreLoad)) return 2;
    const bool useX1 = indexedLoad || byteLoad || halfLoad || signedByteLoad ||
                       signedHalfLoad || signedWordLoad || vectorPostLoad ||
                       vectorPreLoad;
    const std::string_view loadInstruction = signedByteLoad
        ? "    ldrsb w0, [x1]\n"
        : (signedHalfLoad ? "    ldrsh w0, [x1]\n"
        : (signedWordLoad ? "    ldrsw x0, [x1]\n"
        : (vectorPostLoad ? "    ldr q0, [x1], #16\n"
        : (vectorPreLoad ? "    ldr q0, [x1, #16]!\n"
        : (byteLoad ? "    ldrb w0, [x1]\n"
        : (halfLoad ? "    ldrh w0, [x1]\n"
        : (indexedLoad ? "    ldr x0, [x1, #0]!\n"
        : (scalarLoad ? "    ldr x0, [x0]\n"
                       : "    ldp x0, x1, [x0]\n"))))))));
    arm64metal::SharedCache cache;
    const auto symbol = cache.resolveExport("/usr/lib/libz.1.dylib", "_zlibVersion");
    if (!symbol || symbol->guest_address <= 0) return 3;

    const auto address = static_cast<std::uint64_t>(symbol->guest_address);
    std::cout << ".text\n"
                 ".globl _main\n"
                 ".p2align 2\n"
                 "_main:\n"
              << "    movz x" << (useX1 ? 1 : 0) << ", #0x"
              << std::hex << (address & 0xffffU) << "\n";
    for (unsigned shift = 16; shift < 64; shift += 16) {
        const auto part = (address >> shift) & 0xffffU;
        if (part == 0) continue;
        std::cout << "    movk x" << (useX1 ? 1 : 0) << ", #0x"
                  << std::hex << part
                  << ", lsl #" << std::dec << shift << "\n";
    }
    if (vectorPostLoad || vectorPreLoad) {
        std::cout << "    sub sp, sp, #16\n"
                     "    add x2, x1, #16\n";
    }
    std::cout << loadInstruction
              ;
    if (vectorPostLoad || vectorPreLoad) {
        std::cout << "    cmp x1, x2\n"
                     "    b.ne .Lwriteback_fail\n"
                     "    str q0, [sp]\n"
                     "    ldrb w0, [sp]\n"
                     "    add sp, sp, #16\n"
                     "    ret\n"
                     ".Lwriteback_fail:\n"
                     "    mov w0, #99\n"
                     "    add sp, sp, #16\n"
                     "    ret\n";
    } else {
        std::cout << "    ret\n";
    }
    std::cout
              << ".section __DATA,__data\n"
              << ".p2align 3\n"
              << ".quad _zlibVersion\n";
    return std::cout ? 0 : 4;
}
