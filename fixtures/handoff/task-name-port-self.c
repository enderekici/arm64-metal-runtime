#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/task.h>
#include <mach/task_special_ports.h>
#include <stdint.h>
#include <stdio.h>

#define TYPE_SENTINEL ((mach_port_type_t)0xdecafbadU)
#define REFS_SENTINEL ((mach_port_urefs_t)0xdeadbeefU)

int main(void) {
  const mach_port_t self = mach_task_self();
  mach_port_t name_port = MACH_PORT_NULL;
  mach_port_t repeated_name_port = MACH_PORT_NULL;
  const kern_return_t get = task_get_special_port(self, TASK_NAME_PORT, &name_port);
  const kern_return_t get_again = get == KERN_SUCCESS
      ? task_get_special_port(self, TASK_NAME_PORT, &repeated_name_port) : get;

  mach_port_type_t type = TYPE_SENTINEL;
  mach_port_urefs_t refs = REFS_SENTINEL;
  const kern_return_t type_result = get == KERN_SUCCESS
      ? _kernelrpc_mach_port_type_trap(self, name_port, &type) : get;
  const int send_type = type_result == KERN_SUCCESS && type == MACH_PORT_TYPE_SEND;
  const int repeated_same_name = repeated_name_port == name_port;
  const kern_return_t refs_twice_result = get_again == KERN_SUCCESS
      ? mach_port_get_refs(self, name_port, MACH_PORT_RIGHT_SEND, &refs) : get_again;
  const int refs_twice = refs_twice_result == KERN_SUCCESS && refs == 2;
  const kern_return_t release_one = get_again == KERN_SUCCESS
      ? _kernelrpc_mach_port_deallocate_trap(self, name_port) : get_again;
  refs = REFS_SENTINEL;
  const kern_return_t refs_once_result = release_one == KERN_SUCCESS
      ? mach_port_get_refs(self, name_port, MACH_PORT_RIGHT_SEND, &refs) : release_one;
  const int refs_once = refs_once_result == KERN_SUCCESS && refs == 1;
  const kern_return_t release_two = get_again == KERN_SUCCESS
      ? _kernelrpc_mach_port_deallocate_trap(self, repeated_name_port) : get_again;
  type = TYPE_SENTINEL;
  const kern_return_t after = get_again == KERN_SUCCESS
      ? _kernelrpc_mach_port_type_trap(self, name_port, &type) : get_again;
  const int okay = get == KERN_SUCCESS && get_again == KERN_SUCCESS &&
      name_port != MACH_PORT_NULL && name_port != self && repeated_same_name &&
      type_result == KERN_SUCCESS && send_type && refs_twice && refs_once &&
      release_one == KERN_SUCCESS && release_two == KERN_SUCCESS &&
      after == KERN_INVALID_NAME && type == TYPE_SENTINEL;
  printf("get=%d repeat=%d same=%u nonnull=%u distinct=%u type=%d send=%u "
         "refs2=%u refs1=%u release=%d/%d after=%d output_untouched=%u result=%s\n",
      get, get_again, repeated_same_name, name_port != MACH_PORT_NULL,
      name_port != self, type_result, send_type, refs_twice, refs_once,
      release_one, release_two, after, type == TYPE_SENTINEL,
      okay ? "PASS" : "FAIL");
  return okay ? 0 : 1;
}
