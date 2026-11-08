#include <mach/mach.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef kern_return_t (*deferred_reclamation_allocate_fn)(
    task_t, mach_vm_address_t *, uint64_t *, uint32_t, uint32_t);

static void check_result(deferred_reclamation_allocate_fn allocate,
                         uint32_t length, uint32_t max_length,
                         kern_return_t expected) {
    mach_vm_address_t address = UINT64_C(0x1111111111111111);
    uint64_t next_deadline = UINT64_C(0x2222222222222222);
    const kern_return_t result = allocate(mach_task_self(), &address, &next_deadline,
                                          length, max_length);
    if (result != expected || address != UINT64_C(0x1111111111111111) ||
        next_deadline != UINT64_C(0x2222222222222222)) {
        fprintf(stderr, "unexpected deferred-reclamation result: len=%u max=%u "
                "result=0x%x address=0x%llx deadline=0x%llx\n",
                length, max_length, result, (unsigned long long)address,
                (unsigned long long)next_deadline);
        exit(1);
    }
}

int main(void) {
    deferred_reclamation_allocate_fn allocate =
        (deferred_reclamation_allocate_fn)dlsym(
            RTLD_DEFAULT, "mach_vm_deferred_reclamation_buffer_allocate");
    if (allocate == NULL) {
        fputs("mach_vm_deferred_reclamation_buffer_allocate is unavailable\n", stderr);
        return 2;
    }

    check_result(allocate, 4096, 4096, (kern_return_t)0x20004006U);
    check_result(allocate, 0, 4096, KERN_INVALID_ARGUMENT);
    check_result(allocate, 4096, 1, KERN_INVALID_ARGUMENT);
    check_result(allocate, 1, 0x7ffff9, KERN_NO_SPACE);
    puts("mach_vm_deferred_reclamation valid=0x20004006 invalid=4 oversized=3 "
         "outputs=unchanged");
    return 0;
}
