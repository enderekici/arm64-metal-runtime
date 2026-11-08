#include <stdint.h>

// Ordinary C compiled by Clang. The translator receives only the resulting Mach-O.
uint32_t update_words(uint32_t *words, uint64_t count, uint32_t seed) {
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t value = words[i] ^ seed;
        if (value & 1u) {
            value = value * 3u + 1u;
        } else {
            value >>= 1;
        }
        words[i] = value;
        seed += value;
    }
    return seed;
}

uint32_t xor_words(uint32_t *words, uint64_t count, uint32_t seed) {
    for (uint64_t i = 0; i < count; ++i) {
        words[i] ^= seed;
    }
    return seed;
}

uint32_t unsupported(uint32_t *words, uint64_t count, uint32_t seed) {
    __builtin_trap();
}

uint32_t read_second_word(uint32_t *words, uint64_t count, uint32_t seed) {
    return words[1];
}
