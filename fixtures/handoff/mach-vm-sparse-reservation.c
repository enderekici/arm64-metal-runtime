#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_inherit.h>
#include <mach/vm_statistics.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GIB (UINT64_C(1) << 30)
#define MIB (UINT64_C(1) << 20)
#define RESERVE_SIZE (UINT64_C(24) * GIB)
#define RESERVE_MASK ((UINT64_C(32) * MIB) - 1)
#define SEGMENT_SIZE (UINT64_C(4) * MIB)

static kern_return_t map_region(mach_vm_address_t *address, mach_vm_size_t size,
    mach_vm_offset_t mask, int flags, vm_prot_t current, vm_prot_t maximum) {
    return mach_vm_map(mach_task_self(), address, size, mask, flags,
        MACH_PORT_NULL, 0, FALSE, current, maximum, VM_INHERIT_SHARE);
}
static kern_return_t read_one(mach_vm_address_t address) {
    char byte = 0;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, 1,
        (mach_vm_address_t)(uintptr_t)&byte, &copied);
}
int main(void) {
    kern_return_t kr;
    mach_vm_address_t base = 0;
    kr = map_region(&base, RESERVE_SIZE, RESERVE_MASK, VM_FLAGS_ANYWHERE,
        VM_PROT_NONE, VM_PROT_NONE);
    if (kr != KERN_SUCCESS) {
        printf("reserve_anywhere_24g=%d\n", kr);
        return 1;
    }
    int align32m = (base & RESERVE_MASK) == 0;
    kr = mach_vm_deallocate(mach_task_self(), base, RESERVE_SIZE);
    if (kr != KERN_SUCCESS) { printf("seed_deallocate=%d\n", kr); return 2; }
    mach_vm_address_t fixed = base;
    kr = map_region(&fixed, RESERVE_SIZE, 0, VM_FLAGS_FIXED,
        VM_PROT_NONE, VM_PROT_NONE);
    int fixed_exact = kr == KERN_SUCCESS && fixed == base;
    printf("reserve_align32m=%d fixed_24g=%d fixed_exact=%d\n", align32m, kr, fixed_exact);
    if (!fixed_exact) return 3;

    mach_vm_address_t overlap = base;
    kern_return_t overlap_kr = map_region(&overlap, 4096, 0, VM_FLAGS_FIXED,
        VM_PROT_NONE, VM_PROT_NONE);
    kern_return_t protect_kr = mach_vm_protect(mach_task_self(), base + SEGMENT_SIZE,
        4096, FALSE, VM_PROT_READ | VM_PROT_WRITE);

    mach_vm_address_t middle = base + 2 * SEGMENT_SIZE;
    int overwrite_flags = VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE |
        VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL);
    kern_return_t overwrite_kr = map_region(&middle, SEGMENT_SIZE, 0,
        overwrite_flags, VM_PROT_READ | VM_PROT_WRITE,
        VM_PROT_READ | VM_PROT_WRITE);
    int zero_fill = 0, write_read = 0;
    kern_return_t left_kr = read_one(middle - 4096);
    kern_return_t right_kr = read_one(middle + SEGMENT_SIZE);
    mach_vm_address_t left_overlap = middle - SEGMENT_SIZE;
    mach_vm_address_t right_overlap = middle + SEGMENT_SIZE;
    kern_return_t left_overlap_kr = map_region(&left_overlap, 4096, 0,
        VM_FLAGS_FIXED, VM_PROT_NONE, VM_PROT_NONE);
    kern_return_t right_overlap_kr = map_region(&right_overlap, 4096, 0,
        VM_FLAGS_FIXED, VM_PROT_NONE, VM_PROT_NONE);
    if (overwrite_kr == KERN_SUCCESS && middle == base + 2 * SEGMENT_SIZE) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)middle;
        zero_fill = 1;
        for (size_t i = 0; i < 4096; ++i) if (p[i] != 0) { zero_fill = 0; break; }
        memset((void *)(uintptr_t)middle, 0x5a, 4096);
        write_read = p[0] == 0x5a && p[4095] == 0x5a;
    }
    mach_vm_address_t mte_overwrite = middle + SEGMENT_SIZE;
    kern_return_t mte_overwrite_kr = map_region(&mte_overwrite, SEGMENT_SIZE, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_FLAGS_MTE |
            VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    mach_vm_address_t repeated = middle;
    kern_return_t repeated_kr = map_region(&repeated, SEGMENT_SIZE, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    int repeated_zero = 0;
    if (repeated_kr == KERN_SUCCESS) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)repeated;
        repeated_zero = p[0] == 0 && p[4095] == 0;
    }
    printf("fixed_overlap=%d protect_none_to_rw=%d overwrite_tag2=%d zero_fill=%d write_read=%d left_reserved_read=%d right_reserved_read=%d left_fixed_overlap=%d right_fixed_overlap=%d overwrite_tag2_mte=%d repeated_overwrite=%d repeated_zero=%d\n",
        overlap_kr, protect_kr, overwrite_kr, zero_fill, write_read, left_kr, right_kr,
        left_overlap_kr, right_overlap_kr, mte_overwrite_kr, repeated_kr, repeated_zero);

    kern_return_t release_kr = mach_vm_deallocate(mach_task_self(), base, RESERVE_SIZE);
    mach_vm_address_t reuse = base;
    kern_return_t reuse_kr = release_kr == KERN_SUCCESS
        ? map_region(&reuse, RESERVE_SIZE, 0, VM_FLAGS_FIXED, VM_PROT_NONE, VM_PROT_NONE)
        : release_kr;
    printf("deallocate=%d fixed_reuse=%d reuse_exact=%d\n", release_kr, reuse_kr,
        reuse_kr == KERN_SUCCESS && reuse == base);
    if (reuse_kr == KERN_SUCCESS) (void)mach_vm_deallocate(mach_task_self(), reuse, RESERVE_SIZE);

    mach_vm_address_t aligned = 0;
    kern_return_t align_kr = map_region(&aligned, SEGMENT_SIZE, SEGMENT_SIZE - 1,
        VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_EXECUTE | VM_PROT_WRITE);
    printf("anywhere_4m=%d aligned4m=%d\n", align_kr,
        align_kr == KERN_SUCCESS && (aligned & (SEGMENT_SIZE - 1)) == 0);
    if (align_kr == KERN_SUCCESS) (void)mach_vm_deallocate(mach_task_self(), aligned, SEGMENT_SIZE);

    mach_vm_address_t mte = 0;
    kern_return_t mte_kr = map_region(&mte, SEGMENT_SIZE, SEGMENT_SIZE - 1,
        VM_FLAGS_ANYWHERE | VM_FLAGS_MTE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_EXECUTE | VM_PROT_WRITE);
    printf("anywhere_mte=%d\n", mte_kr);
    if (mte_kr == KERN_SUCCESS) (void)mach_vm_deallocate(mach_task_self(), mte, SEGMENT_SIZE);
    mach_vm_address_t purgable = 0;
    kern_return_t purgable_kr = map_region(&purgable, SEGMENT_SIZE, SEGMENT_SIZE - 1,
        VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    printf("anywhere_purgable=%d\n", purgable_kr);
    if (purgable_kr == KERN_SUCCESS) (void)mach_vm_deallocate(mach_task_self(), purgable, SEGMENT_SIZE);

    return overlap_kr == KERN_NO_SPACE && protect_kr == KERN_PROTECTION_FAILURE &&
        overwrite_kr == KERN_SUCCESS && zero_fill && write_read &&
        left_kr == KERN_INVALID_ADDRESS && right_kr == KERN_INVALID_ADDRESS &&
        left_overlap_kr == KERN_NO_SPACE && right_overlap_kr == KERN_NO_SPACE &&
        mte_overwrite_kr == KERN_SUCCESS && repeated_kr == KERN_SUCCESS && repeated_zero &&
        release_kr == KERN_SUCCESS && reuse_kr == KERN_SUCCESS &&
        align_kr == KERN_SUCCESS && (aligned & (SEGMENT_SIZE - 1)) == 0 &&
        mte_kr == KERN_SUCCESS && purgable_kr == KERN_SUCCESS ? 0 : 4;
}
