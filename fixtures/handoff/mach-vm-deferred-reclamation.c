#include <mach/mach.h>
#include <stdint.h>
#include <unistd.h>

extern kern_return_t mach_vm_deferred_reclamation_buffer_allocate(
    task_t, mach_vm_address_t *, uint64_t *, uint32_t, uint32_t);

static int check_result(uint32_t length, uint32_t max_length,
                        kern_return_t expected) {
    mach_vm_address_t address = UINT64_C(0x1111111111111111);
    uint64_t next_deadline = UINT64_C(0x2222222222222222);
    const kern_return_t result = mach_vm_deferred_reclamation_buffer_allocate(
        mach_task_self(), &address, &next_deadline, length, max_length);
    if (result != expected || address != UINT64_C(0x1111111111111111) ||
        next_deadline != UINT64_C(0x2222222222222222)) {
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
