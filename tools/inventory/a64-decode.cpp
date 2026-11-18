// Static process-lowering probe. No generated Metal or guest code runs here.
#include "process_decoder.hpp"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main() {
    std::uint32_t word;
    while (std::cin >> std::hex >> word) {
        try {
            const auto instruction = arm64metal::decode_process(word, 0x100000000, {}, {}, true);
            if (instruction.operation) {
                std::cout << "typed-ir " << std::dec
                          << static_cast<unsigned>(instruction.operation->opcode) << '\n';
            } else {
                std::cout << "translated-ir\n";
            }
        } catch (const std::exception& error) {
            std::cout << "rejected " << error.what() << '\n';
        }
    }
    return std::cin.eof() ? 0 : 1;
}
