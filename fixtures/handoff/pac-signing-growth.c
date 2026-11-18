#include <stdint.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argv;
    static unsigned char target;
    const uintptr_t address = (uintptr_t)&target;
    uintptr_t first = 0, last = 0;
    for (uintptr_t modifier = 1; modifier <= 4097; ++modifier) {
        uintptr_t value = address;
        uintptr_t flags;
        __asm__ volatile("cmp xzr, xzr\n\tpacia %0, %2\n\tmrs %1, nzcv"
                         : "+r"(value), "=r"(flags) : "r"(modifier) : "cc");
        if (flags != 0x60000000U) return 3;
        if (modifier == 1) first = value;
        last = value;
    }
    uintptr_t first_modifier = argc == 1 ? 1 : 2;
    uintptr_t last_modifier = 4097;
    __asm__ volatile("autia %0, %1" : "+r"(first) : "r"(first_modifier));
    __asm__ volatile("autia %0, %1" : "+r"(last) : "r"(last_modifier));
    if (first != address || last != address) return 1;
    static const char message[] = "4097 distinct PAC modifiers verified\n";
    return write(1, message, sizeof(message) - 1) == sizeof(message) - 1 ? 0 : 2;
}
