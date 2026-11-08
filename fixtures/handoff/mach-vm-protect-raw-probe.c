#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static kern_return_t raw_protect(mach_port_t task, mach_vm_address_t address,
                                  mach_vm_size_t size, boolean_t set_maximum,
                                  vm_prot_t protection) {
  register uint64_t x0 __asm__("x0") = task;
  register uint64_t x1 __asm__("x1") = address;
  register uint64_t x2 __asm__("x2") = size;
  register uint64_t x3 __asm__("x3") = (uint32_t)set_maximum;
  register uint64_t x4 __asm__("x4") = (uint32_t)protection;
  register uint64_t x16 __asm__("x16") = (uint64_t)-14;
  __asm__ volatile("svc #0x80"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x16)
                   : "memory", "cc");
  return (kern_return_t)x0;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_vm_size_t page = vm_page_size;
  mach_vm_address_t base = 0;
  const kern_return_t allocation = mach_vm_allocate(task, &base, 3 * page,
                                                     VM_FLAGS_ANYWHERE);
  if (allocation != KERN_SUCCESS) {
    printf("allocate=%d\n", allocation);
    return 1;
  }
  volatile uint8_t *memory = (volatile uint8_t *)(uintptr_t)base;
  if (mprotect((void *)(uintptr_t)base, 3 * page, PROT_READ | PROT_WRITE) != 0) {
    printf("initial-protect=%d\n", errno);
    return 1;
  }
  memory[0] = 0x31;
  memory[page] = 0x52;
  memory[2 * page] = 0x73;

  const kern_return_t current_read = raw_protect(task, base + page, page,
                                                  FALSE, VM_PROT_READ);
  const int restore_write = mprotect((void *)(uintptr_t)(base + page), page,
                                     PROT_READ | PROT_WRITE);
  memory[page] = 0x53;
  const int set_read = mprotect((void *)(uintptr_t)(base + page), page, PROT_READ);
  const kern_return_t lower_maximum = raw_protect(task, base + page, page,
                                                   TRUE, VM_PROT_READ);
  const kern_return_t raise_maximum = raw_protect(task, base + page, page,
                                                   TRUE, VM_PROT_READ | VM_PROT_WRITE);
  const kern_return_t write_over_maximum = raw_protect(task, base + page, page,
                                                        FALSE, VM_PROT_WRITE);

  errno = 0;
  const int writable_after_lowering = mprotect((void *)(uintptr_t)(base + page),
                                                page, PROT_READ | PROT_WRITE);
  const int writable_errno = errno;
  errno = 0;
  const int readable_after_lowering = mprotect((void *)(uintptr_t)(base + page),
                                                page, PROT_READ);
  const int readable_errno = errno;
  const uint8_t middle = memory[page];

  const int left_rw = mprotect((void *)(uintptr_t)base, page,
                               PROT_READ | PROT_WRITE);
  const int right_rw = mprotect((void *)(uintptr_t)(base + 2 * page), page,
                                PROT_READ | PROT_WRITE);
  memory[0] = 0x32;
  memory[2 * page] = 0x74;
  const uint8_t left = memory[0];
  const uint8_t right = memory[2 * page];

  const kern_return_t deallocate_middle = mach_vm_deallocate(task, base + page, page);
  errno = 0;
  const int access_hole = mprotect((void *)(uintptr_t)(base + page), page, PROT_READ);
  const int hole_errno = errno;
  const kern_return_t release_left = mach_vm_deallocate(task, base, page);
  const kern_return_t release_right = mach_vm_deallocate(task, base + 2 * page, page);

  printf("page=%llu current=%d restore=%d/read=%d lower=%d raise=%d write=%d "
         "mprotect-rw=%d/%d read=%d/%d middle=%u edges=%d,%d:%u,%u "
         "dealloc=%d hole=%d/%d release=%d,%d\n",
         (unsigned long long)page, current_read, restore_write, set_read,
         lower_maximum, raise_maximum, write_over_maximum,
         writable_after_lowering, writable_errno,
         readable_after_lowering, readable_errno, middle,
         left_rw, right_rw, left, right, deallocate_middle,
         access_hole, hole_errno, release_left, release_right);
  return current_read || restore_write || set_read || lower_maximum ||
         raise_maximum != KERN_PROTECTION_FAILURE ||
         write_over_maximum != KERN_PROTECTION_FAILURE ||
         writable_after_lowering != -1 || writable_errno != EACCES ||
         readable_after_lowering || readable_errno || middle != 0x53 ||
         left_rw || right_rw || left != 0x32 || right != 0x74 ||
         deallocate_middle || access_hole != -1 || hole_errno != ENOMEM ||
         release_left || release_right;
}
