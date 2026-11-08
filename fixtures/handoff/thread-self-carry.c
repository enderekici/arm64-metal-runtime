#include <stdint.h>
#include <unistd.h>

struct trap_result {
    uint64_t value;
    uint64_t carry;
};

int main(void) {
    register uint64_t x0 __asm__("x0") = 0;
    register uint64_t x16 __asm__("x16") = 372;
    register uint64_t x17 __asm__("x17");
    __asm__ volatile("cmp xzr, xzr\n\tsvc #0x80\n\tcset x17, cs"
        : "+r"(x0), "+r"(x16), "=r"(x17) : : "memory", "cc");
    const struct trap_result result = {x0, x17};
    if (result.carry != 0 || result.value == 0) return 1;
    static const char message[] = "thread_self_trap clears seeded carry\n";
    return write(STDOUT_FILENO, message, sizeof(message) - 1) ==
        (ssize_t)(sizeof(message) - 1) ? 0 : 2;
}
