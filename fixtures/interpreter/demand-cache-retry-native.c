#include <stdint.h>
#include <string.h>
#include <zlib.h>

int main(void) {
    const char *version = zlibVersion();
    uint64_t instruction_word = 0;
    memcpy(&instruction_word, (const void *)zlibVersion, sizeof(instruction_word));
    return version == 0 ? 1 : (int)(instruction_word & 0xffU);
}
