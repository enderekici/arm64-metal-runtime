#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    const mach_vm_size_t page = (mach_vm_size_t)sysconf(_SC_PAGESIZE);
    mach_vm_address_t source = 0;
    mach_vm_address_t alias = 0;
    mach_vm_size_t size = page;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t allocate = mach_vm_allocate(mach_task_self(), &source, page,
                                               VM_FLAGS_ANYWHERE);
    if (allocate == KERN_SUCCESS) *(volatile unsigned *)source = 0x12345678U;
    const kern_return_t badSizeOutput = mach_make_memory_entry_64(
        mach_task_self(), (mach_vm_size_t *)1, source, VM_PROT_READ,
        &entry, MACH_PORT_NULL);
    const kern_return_t badPortOutput = mach_make_memory_entry_64(
        mach_task_self(), &size, source, VM_PROT_READ,
        (mach_port_t *)1, MACH_PORT_NULL);
    const kern_return_t make = allocate == KERN_SUCCESS
        ? mach_make_memory_entry_64(mach_task_self(), &size, source, VM_PROT_READ,
                                    &entry, MACH_PORT_NULL)
        : allocate;
    const kern_return_t badMapOutput = make == KERN_SUCCESS
        ? mach_vm_map(mach_task_self(), (mach_vm_address_t *)1, page, 0,
                      VM_FLAGS_ANYWHERE, entry, 0, FALSE, VM_PROT_READ,
                      VM_PROT_READ, VM_INHERIT_NONE)
        : make;
    const kern_return_t map = make == KERN_SUCCESS
        ? mach_vm_map(mach_task_self(), &alias, page, 0, VM_FLAGS_ANYWHERE,
                      entry, 0, FALSE, VM_PROT_READ, VM_PROT_READ, VM_INHERIT_NONE)
        : make;
    const unsigned value = map == KERN_SUCCESS ? *(volatile unsigned *)alias : 0;
    if (alias != 0) (void)mach_vm_deallocate(mach_task_self(), alias, page);
    if (entry != MACH_PORT_NULL) (void)mach_port_deallocate(mach_task_self(), entry);
    if (allocate == KERN_SUCCESS) (void)mach_vm_deallocate(mach_task_self(), source, page);
    printf("allocate=%d bad_size_output=%d bad_port_output=%d make=%d "
           "bad_map_output=%d map=%d value=%08x\n", allocate, badSizeOutput,
           badPortOutput, make, badMapOutput, map, value);
    return !(allocate == KERN_SUCCESS && badSizeOutput == KERN_INVALID_ADDRESS &&
        badPortOutput == KERN_INVALID_ADDRESS && make == KERN_SUCCESS &&
        badMapOutput == KERN_INVALID_ADDRESS && map == KERN_SUCCESS &&
        value == 0x12345678U);
}
