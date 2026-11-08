#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INVALID_ADDRESS UINT64_C(0x1000)

int main(void) {
  mach_port_t task = mach_task_self();
  const mach_vm_size_t page = vm_page_size;
  mach_vm_address_t source = 0;
  mach_vm_address_t destination = 0;
  kern_return_t allocate = mach_vm_allocate(task, &source, 2 * page, VM_FLAGS_ANYWHERE);
  if (allocate != KERN_SUCCESS) {
    printf("allocate=%d\n", allocate);
    return 1;
  }
  kern_return_t allocate_destination = mach_vm_allocate(
      task, &destination, 2 * page, VM_FLAGS_ANYWHERE);
  if (allocate_destination != KERN_SUCCESS) {
    printf("allocate_destination=%d\n", allocate_destination);
    return 1;
  }
  kern_return_t source_guard = mach_vm_protect(task, source + page, page, FALSE,
                                                VM_PROT_NONE);
  kern_return_t destination_guard = mach_vm_protect(
      task, destination + page, page, FALSE, VM_PROT_NONE);
  if (source_guard != KERN_SUCCESS || destination_guard != KERN_SUCCESS) return 1;

  unsigned char *bytes = (unsigned char *)(uintptr_t)source;
  memcpy(bytes, "abcdefgh", 8);
  kern_return_t protect = mach_vm_protect(task, source, page, FALSE,
                                           VM_PROT_READ | VM_PROT_WRITE);
  mach_vm_size_t copied = UINT64_C(0x5555);
  kern_return_t alias = mach_vm_read_overwrite(task, source, 6,
      source + 2, &copied);
  int alias_ok = memcmp(bytes, "ababcdef", 8) == 0 && copied == 6;

  memcpy(bytes, "ABCDEFGH", 8);
  unsigned char output[8];
  memset(output, 0x5a, sizeof(output));
  copied = UINT64_C(0x7777);
  kern_return_t source_range = mach_vm_read_overwrite(task,
      source + page - 4, 8, (mach_vm_address_t)(uintptr_t)output, &copied);
  int source_atomic = copied == UINT64_C(0x7777) &&
      output[0] == 0x5a && output[7] == 0x5a;

  unsigned char *destinationBytes = (unsigned char *)(uintptr_t)destination;
  memset(destinationBytes + page - 4, 0xcc, 4);
  copied = UINT64_C(0x8888);
  kern_return_t destination_range = mach_vm_read_overwrite(task, source, 8,
      destination + page - 4, &copied);
  int destination_prefix = memcmp(destinationBytes + page - 4, "ABCD", 4) == 0;
  int count_on_error = copied == UINT64_C(0x8888);

  copied = UINT64_C(0x9999);
  kern_return_t invalid_task = mach_vm_read_overwrite(MACH_PORT_NULL, source, 8,
      (mach_vm_address_t)(uintptr_t)output, &copied);
  int task_atomic = copied == UINT64_C(0x9999);

  copied = UINT64_C(0xaaaa);
  kern_return_t invalid_source = mach_vm_read_overwrite(task,
      source + page, 8, (mach_vm_address_t)(uintptr_t)output, &copied);
  int invalid_source_atomic = copied == UINT64_C(0xaaaa);

  printf("protect=%d alias=%d alias_ok=%d source_range=%d source_atomic=%d "
         "destination_range=%d destination_prefix=%d count_on_error=%d "
         "invalid_task=%d task_atomic=%d invalid_source=%d invalid_source_atomic=%d\n",
      protect, alias, alias_ok, source_range, source_atomic, destination_range,
      destination_prefix, count_on_error, invalid_task, task_atomic, invalid_source,
      invalid_source_atomic);

  kern_return_t release = mach_vm_deallocate(task, source, 2 * page);
  kern_return_t release_destination = mach_vm_deallocate(task, destination, 2 * page);
  printf("release=%d/%d\n", release, release_destination);
  return protect == KERN_SUCCESS && alias == KERN_SUCCESS && alias_ok &&
      source_range == KERN_INVALID_ADDRESS && source_atomic &&
      destination_range == KERN_INVALID_ADDRESS && destination_prefix && count_on_error &&
      invalid_task == MACH_SEND_INVALID_DEST && task_atomic &&
      invalid_source == KERN_INVALID_ADDRESS && invalid_source_atomic &&
      release == KERN_SUCCESS && release_destination == KERN_SUCCESS ? 0 : 2;
}
