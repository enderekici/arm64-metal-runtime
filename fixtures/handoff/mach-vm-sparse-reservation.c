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
static kern_return_t query_protection(mach_vm_address_t address, vm_prot_t *protection) {
    mach_vm_address_t region_address = address;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t result = mach_vm_region(mach_task_self(), &region_address, &region_size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) (void)mach_port_deallocate(mach_task_self(), object);
    if (result != KERN_SUCCESS || address < region_address ||
        address >= region_address + region_size) return KERN_INVALID_ADDRESS;
    *protection = info.protection;
    return KERN_SUCCESS;
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

    mach_vm_address_t owner_a = base + 16 * MIB;
    mach_vm_address_t owner_b = base + 20 * MIB;
    const kern_return_t owner_a_kr = map_region(&owner_a, 2 * MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    const kern_return_t owner_b_kr = map_region(&owner_b, 2 * MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    mach_vm_address_t span = owner_a;
    const kern_return_t span_kr = map_region(&span, 6 * MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    unsigned char span_left = 0xff, span_mid = 0xff, span_right = 0xff;
    const kern_return_t span_left_kr = read_one(owner_a);
    const kern_return_t span_mid_kr = read_one(base + 19 * MIB);
    const kern_return_t span_right_kr = read_one(owner_b);
    mach_vm_size_t copied = 0;
    if (span_kr == KERN_SUCCESS) {
        (void)mach_vm_read_overwrite(mach_task_self(), owner_a, 1,
            (mach_vm_address_t)(uintptr_t)&span_left, &copied);
        (void)mach_vm_read_overwrite(mach_task_self(), base + 19 * MIB, 1,
            (mach_vm_address_t)(uintptr_t)&span_mid, &copied);
        (void)mach_vm_read_overwrite(mach_task_self(), owner_b, 1,
            (mach_vm_address_t)(uintptr_t)&span_right, &copied);
    }
    printf("multi_owner_overwrite=%d exact=%d zero=%d/%d/%d reads=%d/%d/%d\n",
        span_kr, span == owner_a, span_left == 0, span_mid == 0, span_right == 0,
        span_left_kr, span_mid_kr, span_right_kr);

    mach_vm_address_t protect_a = base + 22 * MIB;
    mach_vm_address_t protect_b = base + 23 * MIB;
    const kern_return_t protect_a_kr = map_region(&protect_a, MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    const kern_return_t protect_b_kr = map_region(&protect_b, MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    const kern_return_t cross_protect_kr = mach_vm_protect(mach_task_self(),
        protect_a, 2 * MIB, FALSE, VM_PROT_READ);
    vm_prot_t protect_a_value = 0, protect_b_value = 0;
    const kern_return_t protect_a_query = query_protection(protect_a, &protect_a_value);
    const kern_return_t protect_b_query = query_protection(protect_b, &protect_b_value);
    printf("adjacent_owners=%d/%d protect_cross=%d query=%d/%d current=%u/%u\n",
        protect_a_kr, protect_b_kr, cross_protect_kr,
        protect_a_query, protect_b_query, protect_a_value, protect_b_value);
    (void)mach_vm_protect(mach_task_self(), protect_a, 2 * MIB, FALSE,
                          VM_PROT_READ | VM_PROT_WRITE);

    mach_vm_address_t fixed_reserved = base + 24 * MIB;
    const kern_return_t fixed_reserved_kr = map_region(&fixed_reserved, MIB, 0,
        VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    mach_vm_address_t partial = base + 26 * MIB;
    const kern_return_t partial_map_kr = map_region(&partial, 2 * MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    if (partial_map_kr == KERN_SUCCESS) memset((void *)(uintptr_t)partial, 0x6c, MIB);
    const kern_return_t partial_deallocate_kr = partial_map_kr == KERN_SUCCESS
        ? mach_vm_deallocate(mach_task_self(), partial + MIB, MIB) : partial_map_kr;
    mach_vm_address_t failed_span = partial;
    const kern_return_t failed_span_kr = partial_deallocate_kr == KERN_SUCCESS
        ? map_region(&failed_span, 2 * MIB, 0,
            VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE)
        : partial_deallocate_kr;
    unsigned char retained_marker = 0, released_hole = 0;
    const kern_return_t marker_read = read_one(partial);
    const kern_return_t hole_read = read_one(partial + MIB);
    if (marker_read == KERN_SUCCESS) {
        (void)mach_vm_read_overwrite(mach_task_self(), partial, 1,
            (mach_vm_address_t)(uintptr_t)&retained_marker, &copied);
    }
    if (hole_read == KERN_SUCCESS) {
        (void)mach_vm_read_overwrite(mach_task_self(), partial + MIB, 1,
            (mach_vm_address_t)(uintptr_t)&released_hole, &copied);
    }
    mach_vm_address_t reused_hole = partial + MIB;
    const kern_return_t reused_hole_kr = partial_deallocate_kr == KERN_SUCCESS
        ? map_region(&reused_hole, MIB, 0, VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE)
        : partial_deallocate_kr;
    unsigned char reused_zero = 0xff;
    const kern_return_t reused_read = reused_hole_kr == KERN_SUCCESS
        ? mach_vm_read_overwrite(mach_task_self(), reused_hole, 1,
            (mach_vm_address_t)(uintptr_t)&reused_zero, &copied)
        : reused_hole_kr;
    printf("reserved_fixed=%d partial_deallocate=%d failed_owner_hole=%d marker=%d:%x hole=%d reuse=%d exact=%d zero=%d\n",
        fixed_reserved_kr, partial_deallocate_kr, failed_span_kr,
        marker_read, retained_marker, hole_read, reused_hole_kr,
        reused_hole == partial + MIB, reused_read == KERN_SUCCESS && reused_zero == 0);

    mach_vm_address_t prefix = base + 28 * MIB;
    const kern_return_t prefix_map = map_region(&prefix, 2 * MIB, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
    if (prefix_map == KERN_SUCCESS) memset((void *)(uintptr_t)prefix, 0x7b, 2 * MIB);
    const kern_return_t prefix_deallocate = prefix_map == KERN_SUCCESS
        ? mach_vm_deallocate(mach_task_self(), prefix, MIB) : prefix_map;
    mach_vm_address_t prefix_reuse = prefix;
    const kern_return_t prefix_reuse_kr = prefix_deallocate == KERN_SUCCESS
        ? map_region(&prefix_reuse, MIB, 0, VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE)
        : prefix_deallocate;
    unsigned char prefix_zero = 0xff, suffix_marker = 0;
    const kern_return_t prefix_read = prefix_reuse_kr == KERN_SUCCESS
        ? mach_vm_read_overwrite(mach_task_self(), prefix, 1,
            (mach_vm_address_t)(uintptr_t)&prefix_zero, &copied)
        : prefix_reuse_kr;
    const kern_return_t suffix_read = prefix_map == KERN_SUCCESS
        ? mach_vm_read_overwrite(mach_task_self(), prefix + MIB, 1,
            (mach_vm_address_t)(uintptr_t)&suffix_marker, &copied)
        : prefix_map;
    printf("prefix_map=%d deallocate=%d reuse=%d exact=%d zero=%d suffix=%d:%x\n",
        prefix_map, prefix_deallocate, prefix_reuse_kr, prefix_reuse == prefix,
        prefix_read == KERN_SUCCESS && prefix_zero == 0, suffix_read, suffix_marker);

    kern_return_t release_kr = mach_vm_deallocate(mach_task_self(), base, RESERVE_SIZE);
    mach_vm_address_t reuse = base;
    kern_return_t reuse_kr = release_kr == KERN_SUCCESS
        ? map_region(&reuse, RESERVE_SIZE, 0, VM_FLAGS_FIXED, VM_PROT_NONE, VM_PROT_NONE)
        : release_kr;
    printf("deallocate=%d fixed_reuse=%d reuse_exact=%d\n", release_kr, reuse_kr,
        reuse_kr == KERN_SUCCESS && reuse == base);
    if (reuse_kr == KERN_SUCCESS) (void)mach_vm_deallocate(mach_task_self(), reuse, RESERVE_SIZE);

    mach_vm_address_t protect_commit = 0;
    const kern_return_t protect_commit_map = map_region(&protect_commit, SEGMENT_SIZE, 0,
        VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
        VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE);
    const kern_return_t protect_commit_kr = protect_commit_map == KERN_SUCCESS
        ? mach_vm_protect(mach_task_self(), protect_commit, SEGMENT_SIZE, FALSE,
            VM_PROT_READ | VM_PROT_WRITE)
        : protect_commit_map;
    int protect_commit_write = 0;
    if (protect_commit_kr == KERN_SUCCESS) {
        unsigned char *p = (unsigned char *)(uintptr_t)protect_commit;
        p[0] = 0x36;
        p[4095] = 0x63;
        protect_commit_write = p[0] == 0x36 && p[4095] == 0x63;
    }
    const kern_return_t protect_commit_release = protect_commit_map == KERN_SUCCESS
        ? mach_vm_deallocate(mach_task_self(), protect_commit, SEGMENT_SIZE)
        : protect_commit_map;
    printf("protect_commit_map=%d protect_rw=%d write_read=%d deallocate=%d\n",
        protect_commit_map, protect_commit_kr, protect_commit_write, protect_commit_release);

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
        owner_a_kr == KERN_SUCCESS && owner_b_kr == KERN_SUCCESS &&
        span_kr == KERN_SUCCESS && span == owner_a && span_left_kr == KERN_SUCCESS &&
        span_mid_kr == KERN_SUCCESS && span_right_kr == KERN_SUCCESS &&
        span_left == 0 && span_mid == 0 && span_right == 0 &&
        protect_a_kr == KERN_SUCCESS && protect_b_kr == KERN_SUCCESS &&
        cross_protect_kr == KERN_SUCCESS && protect_a_query == KERN_SUCCESS &&
        protect_b_query == KERN_SUCCESS && protect_a_value == VM_PROT_READ &&
        protect_b_value == VM_PROT_READ && fixed_reserved_kr == KERN_NO_SPACE &&
        partial_deallocate_kr == KERN_SUCCESS && failed_span_kr == KERN_NO_SPACE &&
        marker_read == KERN_SUCCESS && retained_marker == 0x6c &&
        hole_read == KERN_INVALID_ADDRESS && reused_hole_kr == KERN_SUCCESS &&
        reused_hole == partial + MIB && reused_read == KERN_SUCCESS && reused_zero == 0 &&
        prefix_map == KERN_SUCCESS && prefix_deallocate == KERN_SUCCESS &&
        prefix_reuse_kr == KERN_SUCCESS && prefix_reuse == prefix &&
        prefix_read == KERN_SUCCESS && prefix_zero == 0 &&
        suffix_read == KERN_SUCCESS && suffix_marker == 0x7b &&
        release_kr == KERN_SUCCESS && reuse_kr == KERN_SUCCESS &&
        protect_commit_map == KERN_SUCCESS && protect_commit_kr == KERN_SUCCESS &&
        protect_commit_write && protect_commit_release == KERN_SUCCESS &&
        align_kr == KERN_SUCCESS && (aligned & (SEGMENT_SIZE - 1)) == 0 &&
        mte_kr == KERN_SUCCESS && purgable_kr == KERN_SUCCESS ? 0 : 4;
}
