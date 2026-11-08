#include <mach/mach.h>
#include <mach/mach_port.h>
#include <stdio.h>

int main(void) {
  const mach_port_t task = mach_task_self();
  mach_port_urefs_t refs = 0;
  const kern_return_t result = mach_port_get_refs(
      task, task, MACH_PORT_RIGHT_SEND, &refs);
  const int valid = result == KERN_SUCCESS && refs != 0;
  printf("task_send_refs_result=%d valid=%d\n", result, valid);
  return valid ? 0 : 1;
}
