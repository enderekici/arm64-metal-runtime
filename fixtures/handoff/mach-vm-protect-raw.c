#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>

static kern_return_t raw_protect(mach_port_t task, mach_vm_address_t address,
                                  mach_vm_size_t size, boolean_t set_maximum,
                                  vm_prot_t protection) {
  register uint64_t x0 __asm__("x0") = task;
  register uint64_t x1 __asm__("x1") = address;
  register uint64_t x2 __asm__("x2") = size;
  register uint64_t x3 __asm__("x3") = (uint32_t)set_maximum;
  register uint64_t x4 __asm__("x4") = (uint32_t)protection;
  register uint64_t x16 __asm__("x16") = (uint64_t)-14;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x16) : "memory", "cc");
  return (kern_return_t)x0;
}

static kern_return_t query(mach_port_t task, mach_vm_address_t address,
                           vm_prot_t *current, vm_prot_t *maximum) {
  mach_vm_address_t region_address = address;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  kern_return_t result = mach_vm_region(task, &region_address, &region_size,
      VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
  if (object != MACH_PORT_NULL) mach_port_deallocate(task, object);
  if (result || address < region_address || address >= region_address + region_size)
    return result ? result : KERN_INVALID_ADDRESS;
  *current = info.protection;
  *maximum = info.max_protection;
  return KERN_SUCCESS;
}

int main(void) {
  mach_port_t task = mach_task_self();
  mach_vm_size_t page = vm_page_size;
  mach_vm_address_t base = 0;
  kern_return_t allocate = mach_vm_allocate(task, &base, 3 * page, VM_FLAGS_ANYWHERE);
  if (allocate) { printf("allocate=%d\n", allocate); return 1; }
  kern_return_t wrapper = mach_vm_protect(task, base, page, FALSE, VM_PROT_READ);
  kern_return_t split = raw_protect(task, base + page, page, FALSE, VM_PROT_READ);
  vm_prot_t c[3] = {0}, m[3] = {0};
  kern_return_t q[3];
  for (int i = 0; i < 3; ++i) q[i] = query(task, base + i * page, &c[i], &m[i]);
  kern_return_t lower = raw_protect(task, base + page, page, TRUE, VM_PROT_READ);
  kern_return_t raise = raw_protect(task, base + page, page, TRUE,
                                     VM_PROT_READ | VM_PROT_WRITE);
  kern_return_t over = raw_protect(task, base + page, page, FALSE, VM_PROT_WRITE);
  vm_prot_t cm = 0, mm = 0;
  kern_return_t qm = query(task, base + page, &cm, &mm);
  printf("page=%llu wrapper=%d split=%d q=%d:%d/%d,%d:%d/%d,%d:%d/%d lower=%d raise=%d over=%d final=%d:%d/%d\n",
         (unsigned long long)page, wrapper, split,
         q[0], c[0], m[0], q[1], c[1], m[1], q[2], c[2], m[2],
         lower, raise, over, qm, cm, mm);
  kern_return_t release = mach_vm_deallocate(task, base, 3 * page);
  printf("release=%d\n", release);
  return wrapper || split || q[0] || q[1] || q[2] || lower ||
         raise != KERN_PROTECTION_FAILURE || over != KERN_PROTECTION_FAILURE ||
         qm || cm != VM_PROT_READ || mm != VM_PROT_READ || release;
}
