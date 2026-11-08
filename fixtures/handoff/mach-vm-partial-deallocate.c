#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static int verify_unaligned_deallocation(mach_vm_size_t page_size) {
  mach_vm_address_t base = 0;
  const mach_vm_size_t allocation_size = page_size * 3;
  const mach_port_t task = mach_task_self();
  if (mach_vm_allocate(task, &base, allocation_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS ||
      mprotect((void *)(uintptr_t)base, allocation_size, PROT_READ | PROT_WRITE) != 0) {
    return 1;
  }

  const kern_return_t unaligned_result =
      mach_vm_deallocate(task, base + 1, page_size);
  errno = 0;
  const int first_page = mprotect((void *)(uintptr_t)base, page_size, PROT_READ);
  const int first_errno = errno;
  errno = 0;
  const int second_page =
      mprotect((void *)(uintptr_t)(base + page_size), page_size, PROT_READ);
  const int second_errno = errno;
  const int third_page =
      mprotect((void *)(uintptr_t)(base + page_size * 2), page_size, PROT_READ);
  const kern_return_t release_third_page =
      mach_vm_deallocate(task, base + page_size * 2, page_size);

  printf("unaligned=%d pages=%d/%d,%d/%d,%d release=%d\n", unaligned_result,
         first_page, first_errno, second_page, second_errno, third_page,
         release_third_page);
  return unaligned_result != KERN_SUCCESS || first_page != -1 || first_errno != ENOMEM ||
         second_page != -1 || second_errno != ENOMEM || third_page != 0 ||
         release_third_page != KERN_SUCCESS;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_vm_size_t page_size = vm_page_size;
  if (verify_unaligned_deallocation(page_size) != 0) return 1;
  mach_vm_address_t base = 0;
  const mach_vm_size_t allocation_size = page_size * 3;
  const kern_return_t allocate_result = mach_vm_allocate(
      task, &base, allocation_size, VM_FLAGS_ANYWHERE);
  if (allocate_result != KERN_SUCCESS) {
    printf("allocate=%d\n", allocate_result);
    return 1;
  }
  if (mprotect((void *)(uintptr_t)base, allocation_size, PROT_READ | PROT_WRITE) != 0) {
    mach_vm_deallocate(task, base, allocation_size);
    printf("protect=%d\n", errno);
    return 1;
  }

  volatile uint8_t *memory = (volatile uint8_t *)(uintptr_t)base;
  memory[0] = 0x31;
  memory[page_size * 2] = 0x73;

  const kern_return_t middle_result =
      mach_vm_deallocate(task, base + page_size, page_size);

  const uint8_t first = memory[0];
  const uint8_t last = memory[page_size * 2];

  errno = 0;
  const int middle_protect_result =
      mprotect((void *)(uintptr_t)(base + page_size), page_size, PROT_READ);
  const int middle_protect_errno = errno;
  const kern_return_t repeated_middle_result =
      mach_vm_deallocate(task, base + page_size, page_size);

  const kern_return_t left_release =
      mach_vm_deallocate(task, base, page_size);
  const kern_return_t right_release =
      mach_vm_deallocate(task, base + page_size * 2, page_size);

  printf("page=%llu middle=%d repeat=%d first=%u last=%u middle_protect=%d/%d left=%d right=%d\n",
         (unsigned long long)page_size, middle_result, repeated_middle_result,
         first, last, middle_protect_result, middle_protect_errno,
         left_release, right_release);

  return middle_result != KERN_SUCCESS || first != 0x31 || last != 0x73 ||
         repeated_middle_result != KERN_SUCCESS ||
         middle_protect_result != -1 || middle_protect_errno != ENOMEM ||
         left_release != KERN_SUCCESS || right_release != KERN_SUCCESS;
}
