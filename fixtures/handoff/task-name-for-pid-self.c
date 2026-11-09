#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  const mach_port_t self = mach_task_self();
  mach_port_t own_name = MACH_PORT_DEAD;
  mach_port_t missing_name = MACH_PORT_DEAD;
  const kern_return_t own_result = task_name_for_pid(self, getpid(), &own_name);
  mach_port_type_t own_type = 0;
  const kern_return_t type_result = own_result == KERN_SUCCESS
      ? mach_port_type(self, own_name, &own_type) : own_result;
  const kern_return_t release = own_result == KERN_SUCCESS
      ? mach_port_deallocate(self, own_name) : own_result;
  const kern_return_t missing_result = task_name_for_pid(self, -1, &missing_name);
  const kern_return_t invalid_output_result = task_name_for_pid(
      self, -1, (mach_port_t *)(uintptr_t)1);
  const int okay = own_result == KERN_SUCCESS && own_name != MACH_PORT_NULL &&
      own_name != self && type_result == KERN_SUCCESS &&
      own_type == MACH_PORT_TYPE_SEND && release == KERN_SUCCESS &&
      missing_result == KERN_FAILURE && missing_name == MACH_PORT_NULL &&
      invalid_output_result == KERN_FAILURE;
  printf("own=%d own_nonnull=%u own_distinct=%u type=%d send=%u release=%d "
         "missing=%d missing_null=%u invalid_output=%d result=%s\n",
      own_result, own_name != MACH_PORT_NULL, own_name != self, type_result,
      own_type == MACH_PORT_TYPE_SEND, release, missing_result,
      missing_name == MACH_PORT_NULL, invalid_output_result,
      okay ? "PASS" : "FAIL");
  return okay ? 0 : 1;
}
