#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <stdint.h>
#include <stdio.h>

#define EXPECT_SEND ((mach_port_type_t)0x00010000U)
#define EXPECT_RECEIVE ((mach_port_type_t)0x00020000U)
#define TYPE_SENTINEL ((mach_port_type_t)0xdecafbadU)
#define REFS_SENTINEL ((mach_port_urefs_t)0xdeadbeefU)

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_port_context_t context = UINT64_C(0x12345678abcdef01);
  mach_port_options_t options = {0};
  options.flags = MPO_CONTEXT_AS_GUARD | MPO_INSERT_SEND_RIGHT | MPO_STRICT;
  mach_port_name_t name = MACH_PORT_NULL;
  const kern_return_t create =
      _kernelrpc_mach_port_construct_trap(task, &options, context, &name);

  mach_port_type_t type = TYPE_SENTINEL;
  mach_port_urefs_t refs = REFS_SENTINEL;
  const kern_return_t type_result = create == KERN_SUCCESS
      ? _kernelrpc_mach_port_type_trap(task, name, &type) : create;
  const kern_return_t refs_result = create == KERN_SUCCESS
      ? mach_port_get_refs(task, name, MACH_PORT_RIGHT_SEND, &refs) : create;
  const int created_rights = type_result == KERN_SUCCESS &&
      type == (EXPECT_SEND | EXPECT_RECEIVE) && refs_result == KERN_SUCCESS && refs == 1;

  const kern_return_t destroy = create == KERN_SUCCESS
      ? _kernelrpc_mach_port_destruct_trap(task, name, -1, context) : create;
  type = TYPE_SENTINEL;
  const kern_return_t after_destroy = create == KERN_SUCCESS
      ? _kernelrpc_mach_port_type_trap(task, name, &type) : create;
  const int destroyed = destroy == KERN_SUCCESS &&
      after_destroy == KERN_INVALID_NAME && type == TYPE_SENTINEL;

  mach_port_options_t invalid_options = {0};
  invalid_options.flags = UINT32_C(0x80000000);
  mach_port_name_t invalid_name = MACH_PORT_DEAD;
  const kern_return_t invalid = _kernelrpc_mach_port_construct_trap(
      task, &invalid_options, 0, &invalid_name);
  const int invalid_untouched = invalid == KERN_INVALID_ARGUMENT &&
      invalid_name == MACH_PORT_DEAD;

  printf("create=%d combined=%u refs=%u destruct=%d destroyed=%u invalid=%d "
         "invalid_untouched=%u result=%s\n",
      create, created_rights, refs, destroy, destroyed, invalid, invalid_untouched,
      created_rights && destroyed && invalid_untouched ? "PASS" : "FAIL");
  return created_rights && destroyed && invalid_untouched ? 0 : 1;
}
