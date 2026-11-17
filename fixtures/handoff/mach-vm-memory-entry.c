#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <unistd.h>

static int map_readonly_entry(mach_port_t entry, mach_vm_size_t page,
                              mach_vm_address_t *address) {
    *address = 0;
    return mach_vm_map(mach_task_self(), address, page, 0, VM_FLAGS_ANYWHERE,
                       entry, 0, FALSE, VM_PROT_READ, VM_PROT_READ,
                       VM_INHERIT_NONE);
}

int main(void) {
    const mach_vm_size_t page = (mach_vm_size_t)sysconf(_SC_PAGESIZE);
    mach_vm_address_t source = 0;
    mach_vm_address_t alias = 0;
    mach_port_t entry = MACH_PORT_NULL;
    mach_vm_size_t entry_size = page;
    int create = mach_vm_allocate(mach_task_self(), &source, page, VM_FLAGS_ANYWHERE);
    if (create != KERN_SUCCESS) return 1;
    *(volatile unsigned *)source = 0x12345678U;
    mach_vm_size_t invalid_size = page;
    mach_port_t invalid_entry = MACH_PORT_NULL;
    const kern_return_t invalid_source = mach_make_memory_entry_64(
        mach_task_self(), &invalid_size, source + page, VM_PROT_READ,
        &invalid_entry, MACH_PORT_NULL);
    const kern_return_t make = mach_make_memory_entry_64(
        mach_task_self(), &entry_size, source, VM_PROT_READ, &entry, MACH_PORT_NULL);
    const kern_return_t map = make == KERN_SUCCESS
        ? map_readonly_entry(entry, page, &alias) : make;
    const unsigned before = map == KERN_SUCCESS ? *(volatile unsigned *)alias : 0;
    kern_return_t denied = KERN_FAILURE;
    if (make == KERN_SUCCESS) {
        mach_vm_address_t writable = 0;
        denied = mach_vm_map(mach_task_self(), &writable, page, 0, VM_FLAGS_ANYWHERE,
                             entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                             VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (writable != 0) (void)mach_vm_deallocate(mach_task_self(), writable, page);
    }
    if (map == KERN_SUCCESS) *(volatile unsigned *)source = 0x87654321U;
    const unsigned shared = map == KERN_SUCCESS ? *(volatile unsigned *)alias : 0;
    const kern_return_t source_release = mach_vm_deallocate(mach_task_self(), source, page);
    const unsigned after_source_release = map == KERN_SUCCESS ? *(volatile unsigned *)alias : 0;
    const kern_return_t port_release = entry == MACH_PORT_NULL
        ? KERN_INVALID_NAME : mach_port_deallocate(mach_task_self(), entry);
    const unsigned after_port_release = map == KERN_SUCCESS ? *(volatile unsigned *)alias : 0;
    mach_vm_address_t stale_alias = 0;
    const kern_return_t stale_map = entry == MACH_PORT_NULL ? KERN_INVALID_NAME
        : map_readonly_entry(entry, page, &stale_alias);
    if (stale_alias != 0) (void)mach_vm_deallocate(mach_task_self(), stale_alias, page);
    const kern_return_t alias_release = alias == 0 ? KERN_INVALID_ADDRESS
        : mach_vm_deallocate(mach_task_self(), alias, page);

    mach_vm_address_t source2 = 0;
    mach_vm_address_t alias2 = 0;
    mach_port_t entry2 = MACH_PORT_NULL;
    entry_size = page;
    const kern_return_t create2 = mach_vm_allocate(
        mach_task_self(), &source2, page, VM_FLAGS_ANYWHERE);
    if (create2 == KERN_SUCCESS) *(volatile unsigned *)source2 = 0x0badc0deU;
    const kern_return_t make2 = create2 == KERN_SUCCESS
        ? mach_make_memory_entry_64(mach_task_self(), &entry_size, source2,
                                    VM_PROT_READ, &entry2, MACH_PORT_NULL)
        : create2;
    const kern_return_t map2 = make2 == KERN_SUCCESS
        ? map_readonly_entry(entry2, page, &alias2) : make2;
    const kern_return_t alias2_release = map2 == KERN_SUCCESS
        ? mach_vm_deallocate(mach_task_self(), alias2, page) : map2;
    const kern_return_t source2_release = create2 == KERN_SUCCESS
        ? mach_vm_deallocate(mach_task_self(), source2, page) : create2;
    mach_vm_address_t alias3 = 0;
    const kern_return_t remap = make2 == KERN_SUCCESS
        ? map_readonly_entry(entry2, page, &alias3) : make2;
    const unsigned remapped = remap == KERN_SUCCESS ? *(volatile unsigned *)alias3 : 0;
    const kern_return_t port2_release = entry2 == MACH_PORT_NULL
        ? KERN_INVALID_NAME : mach_port_deallocate(mach_task_self(), entry2);
    const unsigned after_port2_release = remap == KERN_SUCCESS
        ? *(volatile unsigned *)alias3 : 0;
    const kern_return_t alias3_release = remap == KERN_SUCCESS
        ? mach_vm_deallocate(mach_task_self(), alias3, page) : remap;

    printf("invalid_source=%d/%d entry=%d/%d valid=%d size=%llu map=%d before=%08x denied=%d shared=%08x "
           "release_source=%d after_source=%08x release_port=%d after_port=%08x "
           "stale_map=%d release_alias=%d remap=%d/%d release_source2=%d remapped=%08x "
           "release_port2=%d after_port2=%08x release_alias3=%d\n",
           invalid_source, invalid_entry != MACH_PORT_NULL, create, make,
           entry != MACH_PORT_NULL, (unsigned long long)entry_size, map, before, denied,
           shared, source_release,
           after_source_release, port_release, after_port_release, stale_map, alias_release,
           create2, make2, source2_release, remapped, port2_release,
           after_port2_release, alias3_release);
    return !(invalid_source == KERN_INVALID_ADDRESS && invalid_entry == MACH_PORT_NULL &&
        create == KERN_SUCCESS && make == KERN_SUCCESS && entry_size == page &&
        map == KERN_SUCCESS && before == 0x12345678U && denied == KERN_INVALID_RIGHT &&
        shared == 0x87654321U && source_release == KERN_SUCCESS &&
        after_source_release == shared && port_release == KERN_SUCCESS &&
        after_port_release == shared && stale_map == MACH_SEND_INVALID_RIGHT &&
        alias_release == KERN_SUCCESS &&
        create2 == KERN_SUCCESS && make2 == KERN_SUCCESS && map2 == KERN_SUCCESS &&
        alias2_release == KERN_SUCCESS && source2_release == KERN_SUCCESS &&
        remap == KERN_SUCCESS && remapped == 0x0badc0deU &&
        port2_release == KERN_SUCCESS && after_port2_release == remapped &&
        alias3_release == KERN_SUCCESS);
}
