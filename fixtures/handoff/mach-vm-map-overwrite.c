#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static kern_return_t overwrite(mach_vm_address_t *address, mach_vm_size_t size,
                               int flags, vm_prot_t current, vm_prot_t maximum) {
  return mach_vm_map(mach_task_self(), address, size, 0, flags, MEMORY_OBJECT_NULL, 0,
                     FALSE, current, maximum, VM_INHERIT_DEFAULT);
}

int main(void) {
  const mach_vm_size_t page = (mach_vm_size_t)sysconf(_SC_PAGESIZE);
  const mach_vm_size_t size = page * 4;
  const int flags = VM_MAKE_TAG(VM_MEMORY_MALLOC_TINY) | VM_FLAGS_OVERWRITE;
  if (page == 0 || (page & (page - 1)) != 0) return 1;

  mach_vm_address_t base = 0;
  if (mach_vm_allocate(mach_task_self(), &base, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
    return 2;
  }
  *(volatile uint32_t *)(uintptr_t)base = 0x12345678;
  mach_vm_address_t full = base;
  const kern_return_t fullResult = overwrite(&full, size, flags,
                                              VM_PROT_READ | VM_PROT_WRITE,
                                              VM_PROT_ALL);
  if (fullResult != KERN_SUCCESS || full != base ||
      *(volatile uint32_t *)(uintptr_t)full != 0) {
    return 3;
  }

  *(volatile uint32_t *)(uintptr_t)(base + page) = 0xabcdef01;
  *(volatile uint32_t *)(uintptr_t)(base + 2 * page) = 0x10203040;
  mach_vm_address_t partial = base + page;
  const kern_return_t partialResult = overwrite(&partial, page, flags,
                                                  VM_PROT_READ | VM_PROT_WRITE,
                                                  VM_PROT_ALL);
  if (partialResult != KERN_SUCCESS || partial != base + page ||
      *(volatile uint32_t *)(uintptr_t)partial != 0 ||
      *(volatile uint32_t *)(uintptr_t)(base + 2 * page) != 0x10203040) {
    return 4;
  }

  mach_vm_address_t hole = base + 8 * page;
  const mach_vm_address_t expectedHole = hole;
  const kern_return_t holeResult = overwrite(&hole, page, flags,
                                               VM_PROT_READ | VM_PROT_WRITE,
                                               VM_PROT_ALL);
  if (holeResult != KERN_SUCCESS || hole != expectedHole ||
      *(volatile uint32_t *)(uintptr_t)hole != 0) {
    return 5;
  }

  mach_vm_address_t unmapped = 0;
  const kern_return_t unmappedResult = overwrite(&unmapped, page, flags,
                                                   VM_PROT_READ | VM_PROT_WRITE,
                                                   VM_PROT_ALL);
  if (unmappedResult != KERN_INVALID_ADDRESS || unmapped != 0) return 6;

  mach_vm_address_t badFlags = base;
  const kern_return_t flagsResult = overwrite(&badFlags, page, flags | 0x200,
                                                VM_PROT_READ | VM_PROT_WRITE,
                                                VM_PROT_ALL);
  if (flagsResult != KERN_INVALID_ARGUMENT || badFlags != base) return 7;

  if (mach_vm_deallocate(mach_task_self(), hole, page) != KERN_SUCCESS) return 8;
  if (mach_vm_deallocate(mach_task_self(), base, size) != KERN_SUCCESS) return 9;
  puts("mach_vm_map overwrite full=ok partial=ok hole=ok null=1 bad_flags=4");
  return 0;
}
