#include <stdint.h>
#include <string.h>
#include <zlib.h>

#ifndef PAIR_LOAD
#define PAIR_LOAD 1
#endif

int main(void) {
    const char *version = zlibVersion();
    uint64_t instruction_pair[2] = {0, 0};
    const size_t width = PAIR_LOAD ? sizeof(instruction_pair) : sizeof(instruction_pair[0]);
    memcpy(instruction_pair, (const void *)zlibVersion, width);
    return version == 0 ? 1 : (int)(instruction_pair[0] & 0xffU);
}
