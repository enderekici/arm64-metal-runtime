#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_vm_size_t page = vm_page_size;
  mach_vm_address_t first = 0;
  mach_vm_address_t middle = 0;
  mach_vm_address_t last = 0;
  kern_return_t a = mach_vm_allocate(task, &first, 3 * page, VM_FLAGS_ANYWHERE);
  kern_return_t b = mach_vm_allocate(task, &middle, 2 * page, VM_FLAGS_ANYWHERE);
  kern_return_t c = mach_vm_allocate(task, &last, 2 * page, VM_FLAGS_ANYWHERE);
  if (a != KERN_SUCCESS || b != KERN_SUCCESS || c != KERN_SUCCESS) {
    printf("alloc=%d,%d,%d\n", a, b, c);
    return 1;
  }
  if (middle != first + 3 * page || last != middle + 2 * page) {
    mach_vm_deallocate(task, first, 3 * page);
    mach_vm_deallocate(task, middle, 2 * page);
    mach_vm_deallocate(task, last, 2 * page);
    printf("layout=noncontiguous\n");
    return 77;
  }
  const int first_protect = mprotect((void *)(uintptr_t)first, 3 * page, PROT_READ | PROT_WRITE);
  const int middle_protect = mprotect((void *)(uintptr_t)middle, 2 * page, PROT_READ | PROT_WRITE);
  const int last_protect = mprotect((void *)(uintptr_t)last, 2 * page, PROT_READ | PROT_WRITE);
  if (first_protect || middle_protect || last_protect) {
    perror("mprotect setup");
    return 1;
  }

  *(volatile uint8_t *)(uintptr_t)first = 0x31;
  *(volatile uint8_t *)(uintptr_t)(last + page) = 0x73;
  const kern_return_t remove_middle = mach_vm_deallocate(task, middle, 2 * page);
  const kern_return_t spanning = mach_vm_deallocate(
      task, first + page, (last + page) - (first + page));

  const uint8_t first_data = *(volatile uint8_t *)(uintptr_t)first;
  const uint8_t last_data = *(volatile uint8_t *)(uintptr_t)(last + page);
  int protections[7];
  for (size_t index = 0; index < 7; ++index) {
    errno = 0;
    const int result = mprotect((void *)(uintptr_t)(first + index * page),
                                page, PROT_READ);
    protections[index] = result == 0 ? 0 : errno;
  }

  printf("page=%llu deltas=%llu,%llu removed=%d span=%d data=%u,%u protection=",
         (unsigned long long)page, (unsigned long long)(middle - first),
         (unsigned long long)(last - first), remove_middle, spanning,
         first_data, last_data);
  for (size_t index = 0; index < 7; ++index) {
    printf("%s%d", index ? "," : "", protections[index]);
  }
  putchar('\n');

  const kern_return_t release_first = mach_vm_deallocate(task, first, page);
  const kern_return_t release_last = mach_vm_deallocate(task, last + page, page);
  printf("release=%d,%d\n", release_first, release_last);
  return remove_middle != KERN_SUCCESS || spanning != KERN_SUCCESS ||
         first_data != 0x31 || last_data != 0x73 || protections[0] != 0 ||
         protections[6] != 0 || release_first != KERN_SUCCESS ||
         release_last != KERN_SUCCESS;
}
