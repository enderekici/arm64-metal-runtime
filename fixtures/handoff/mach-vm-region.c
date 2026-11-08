#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MIB (UINT64_C(1) << 20)
#define BASIC_FLAVOR VM_REGION_BASIC_INFO_64
#define BASIC_COUNT VM_REGION_BASIC_INFO_COUNT_64

static void fill_bytes(void *buffer, size_t size, unsigned char value) {
  volatile unsigned char *bytes = buffer;
  for (size_t index = 0; index < size; ++index) bytes[index] = value;
}

static kern_return_t map_at(mach_vm_address_t *address, mach_vm_size_t size,
                            int flags, vm_prot_t current, vm_prot_t maximum) {
  return mach_vm_map(mach_task_self(), address, size, 0, flags, MACH_PORT_NULL, 0,
                     FALSE, current, maximum, VM_INHERIT_SHARE);
}

static int query(mach_port_t task, mach_vm_address_t address,
                 mach_vm_address_t *start, mach_vm_size_t *size,
                 vm_region_basic_info_data_64_t *info, mach_port_t *object) {
  mach_msg_type_number_t count = BASIC_COUNT;
  *start = address;
  *size = 0;
  *object = UINT32_C(0xdeadbeef);
  fill_bytes(info, sizeof(*info), 0);
  return mach_vm_region(task, start, size, BASIC_FLAVOR,
      (vm_region_info_t)info, &count, object);
}

static int query_matches(mach_vm_address_t address, mach_vm_address_t expected_start,
                         mach_vm_size_t expected_size, vm_prot_t protection,
                         vm_prot_t maximum, vm_inherit_t inheritance,
                         mach_vm_offset_t offset) {
  mach_vm_address_t start = 0;
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_port_t object = MACH_PORT_NULL;
  const kern_return_t result = query(mach_task_self(), address, &start, &size,
                                     &info, &object);
  return result == KERN_SUCCESS && start == expected_start && size == expected_size &&
      info.protection == protection && info.max_protection == maximum &&
      info.inheritance == inheritance && info.offset == offset && !info.shared &&
      info.reserved == 0 && info.behavior == VM_BEHAVIOR_DEFAULT &&
      info.user_wired_count == 0 && object == MACH_PORT_NULL;
}

int main(void) {
  mach_vm_address_t base = 0;
  const kern_return_t reserve = map_at(&base, 12 * MIB, VM_FLAGS_ANYWHERE,
      VM_PROT_NONE, VM_PROT_NONE);
  mach_vm_address_t owner_a = base + 2 * MIB;
  mach_vm_address_t owner_b = base + 6 * MIB;
  const kern_return_t a = map_at(&owner_a, 2 * MIB,
      VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);
  const kern_return_t b = map_at(&owner_b, 2 * MIB,
      VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(VM_MEMORY_MALLOC_SMALL),
      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE);

  const int reserve_query = query_matches(base + MIB, base, 2 * MIB,
      VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_SHARE, 0);
  const int owner_query = query_matches(base + 3 * MIB, base + 2 * MIB, 2 * MIB,
      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
      VM_INHERIT_SHARE, 0);
  const int right_query = query_matches(base + 10 * MIB, base + 8 * MIB, 4 * MIB,
      VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_SHARE, 8 * MIB);
  const kern_return_t deallocate_hole =
      mach_vm_deallocate(mach_task_self(), base + 4 * MIB, 2 * MIB);
  const int hole_next = query_matches(base + 4 * MIB, base + 6 * MIB, 2 * MIB,
      VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
      VM_INHERIT_SHARE, 0);

  mach_vm_address_t start = base;
  mach_vm_size_t size = 77;
  vm_region_basic_info_data_64_t info;
  fill_bytes(&info, sizeof(info), 0x5a);
  mach_msg_type_number_t count = BASIC_COUNT;
  mach_port_t object = 0x1234;
  const kern_return_t invalid_task = mach_vm_region(MACH_PORT_NULL, &start, &size,
      BASIC_FLAVOR, (vm_region_info_t)&info, &count, &object);
  const int invalid_task_atomic = start == base && size == 77 && count == BASIC_COUNT &&
      object == 0x1234 && info.protection == 0x5a5a5a5a;

  start = base;
  size = 77;
  fill_bytes(&info, sizeof(info), 0x5a);
  count = BASIC_COUNT;
  object = 0x1234;
  const kern_return_t invalid_flavor = mach_vm_region(mach_task_self(), &start, &size,
      99, (vm_region_info_t)&info, &count, &object);
  const int invalid_flavor_atomic = start == base && size == 77 && count == BASIC_COUNT &&
      object == 0x1234 && info.protection == 0x5a5a5a5a;

  start = base;
  size = 77;
  fill_bytes(&info, sizeof(info), 0x5a);
  count = BASIC_COUNT - 1;
  object = 0x1234;
  const kern_return_t short_count = mach_vm_region(mach_task_self(), &start, &size,
      BASIC_FLAVOR, (vm_region_info_t)&info, &count, &object);
  const int short_count_atomic = start == base && size == 77 &&
      count == BASIC_COUNT - 1 && object == 0x1234 && info.protection == 0x5a5a5a5a;

  printf("reserve=%d owners=%d/%d deallocate_hole=%d queries=%d/%d/%d/%d\n",
      reserve, a, b, deallocate_hole, reserve_query, owner_query, right_query, hole_next);
  printf("errors task=%d/%d flavor=%d/%d count=%d/%d\n",
      invalid_task, invalid_task_atomic, invalid_flavor, invalid_flavor_atomic,
      short_count, short_count_atomic);
  (void)mach_vm_deallocate(mach_task_self(), base, 12 * MIB);
  return reserve || a || b || deallocate_hole || !reserve_query || !owner_query ||
      !right_query || !hole_next || invalid_task != MACH_SEND_INVALID_DEST ||
      !invalid_task_atomic || invalid_flavor != KERN_INVALID_ARGUMENT ||
      !invalid_flavor_atomic || short_count != KERN_INVALID_ARGUMENT ||
      !short_count_atomic;
}
