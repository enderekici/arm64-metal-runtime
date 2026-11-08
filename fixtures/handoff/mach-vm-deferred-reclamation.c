#include <mach/mach.h>
#include <stdint.h>
#include <unistd.h>

extern kern_return_t mach_vm_deferred_reclamation_buffer_allocate(
    task_t, mach_vm_address_t *, uint64_t *, uint32_t, uint32_t);

static void write_hex(const char *label, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char output[96];
    size_t position = 0;
    while (*label != '\0') output[position++] = *label++;
    output[position++] = ' ';
    output[position++] = 'v';
    output[position++] = 'a';
    output[position++] = 'l';
    output[position++] = 'u';
    output[position++] = 'e';
    output[position++] = '=';
    output[position++] = '0';
    output[position++] = 'x';
    int first = 1;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned nibble = (unsigned)((value >> shift) & 0xfU);
        if (nibble != 0 || !first || shift == 0) {
            output[position++] = digits[nibble];
            first = 0;
        }
    }
    output[position++] = '\n';
    (void)write(STDERR_FILENO, output, position);
}

static int check_result(uint32_t length, uint32_t max_length,
                        kern_return_t expected) {
    mach_vm_address_t address = UINT64_C(0x1111111111111111);
    uint64_t next_deadline = UINT64_C(0x2222222222222222);
    const kern_return_t result = mach_vm_deferred_reclamation_buffer_allocate(
        mach_task_self(), &address, &next_deadline, length, max_length);
    if (result != expected || address != UINT64_C(0x1111111111111111) ||
        next_deadline != UINT64_C(0x2222222222222222)) {
        write_hex("deferred-reclamation-return", (uint32_t)result);
        write_hex("deferred-reclamation-address", address);
        write_hex("deferred-reclamation-deadline", next_deadline);
        write_hex("deferred-reclamation-length", length);
        write_hex("deferred-reclamation-max-length", max_length);
        return 0;
    }
    return 1;
}

int main(void) {
    if (!check_result(4096, 4096, (kern_return_t)0x20004006U) ||
        !check_result(0, 4096, KERN_INVALID_ARGUMENT) ||
        !check_result(4096, 1, KERN_INVALID_ARGUMENT) ||
        !check_result(1, 0x7ffff9, KERN_NO_SPACE)) return 1;
    static const char result[] =
        "mach_vm_deferred_reclamation valid=0x20004006 invalid=4 oversized=3 "
        "outputs=unchanged\n";
    return write(STDOUT_FILENO, result, sizeof(result) - 1) ==
        (ssize_t)(sizeof(result) - 1) ? 0 : 1;
}
