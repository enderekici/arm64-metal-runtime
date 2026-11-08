#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t raw_trap(uint64_t number, uint64_t x0_value,
                         uint64_t x1_value, uint64_t x2_value,
                         uint64_t x3_value) {
  register uint64_t x0 __asm__("x0") = x0_value;
  register uint64_t x1 __asm__("x1") = x1_value;
  register uint64_t x2 __asm__("x2") = x2_value;
  register uint64_t x3 __asm__("x3") = x3_value;
  register uint64_t x16 __asm__("x16") = number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x16) : "memory");
  return x0;
}

int main(void) {
  const mach_port_t task_first = (mach_port_t)raw_trap((uint64_t)-28, 0, 0, 0, 0);
  const mach_port_t task_second = (mach_port_t)raw_trap((uint64_t)-28, 0, 0, 0, 0);
  const mach_port_t host_first = (mach_port_t)raw_trap((uint64_t)-29, 0, 0, 0, 0);
  const mach_port_t host_second = (mach_port_t)raw_trap((uint64_t)-29, 0, 0, 0, 0);
  const uint64_t task_deallocate_first = raw_trap((uint64_t)-18, task_first, task_first, 0, 0);
  const uint64_t task_deallocate_second = raw_trap((uint64_t)-18, task_first, task_second, 0, 0);
  const uint64_t host_deallocate_first = raw_trap((uint64_t)-18, task_first, host_first, 0, 0);
  const uint64_t host_deallocate_second = raw_trap((uint64_t)-18, task_first, host_second, 0, 0);
  const uint64_t host_deallocate_again = raw_trap((uint64_t)-18, task_first, host_first, 0, 0);
  const uint64_t invalid_target = raw_trap((uint64_t)-18, 0, host_first, 0, 0);
  const uint64_t invalid_name = raw_trap((uint64_t)-18, task_first, 0x12345678, 0, 0);
  const uint64_t null_sentinel = raw_trap((uint64_t)-18, task_first, MACH_PORT_NULL, 0, 0);
  const uint64_t dead_sentinel = raw_trap((uint64_t)-18, task_first, MACH_PORT_DEAD, 0, 0);

  mach_port_options_t options = {0};
  mach_port_name_t receive_only = MACH_PORT_NULL;
  const uint64_t construct = raw_trap((uint64_t)-24, task_first,
      (uint64_t)(uintptr_t)&options, 0, (uint64_t)(uintptr_t)&receive_only);
  const uint64_t receive_deallocate = construct == KERN_SUCCESS
      ? raw_trap((uint64_t)-18, task_first, receive_only, 0, 0) : KERN_FAILURE;
  const uint64_t receive_destroy = construct == KERN_SUCCESS
      ? raw_trap((uint64_t)-25, task_first, receive_only, 0, 0) : KERN_FAILURE;

  const int task_stable = task_first != MACH_PORT_NULL && task_first == task_second;
  const int host_stable = host_first != MACH_PORT_NULL && host_first == host_second;
  const int task_releases = task_deallocate_first == KERN_SUCCESS &&
      task_deallocate_second == KERN_SUCCESS;
  const int host_releases = host_deallocate_first == KERN_SUCCESS &&
      host_deallocate_second == KERN_SUCCESS;
  const int host_final_invalid = host_deallocate_again == KERN_INVALID_NAME;
  const int target_invalid_dest = invalid_target == MACH_SEND_INVALID_DEST;
  const int unknown_name_invalid = invalid_name == KERN_INVALID_NAME;
  const int sentinels_noop = null_sentinel == KERN_SUCCESS && dead_sentinel == KERN_SUCCESS;
  const int receive_only_rejected = receive_deallocate == KERN_INVALID_RIGHT;
  const int receive_cleanup = construct == KERN_SUCCESS && receive_destroy == KERN_SUCCESS;

  printf("task_stable=%d task_releases=%d host_stable=%d host_releases=%d "
         "host_final_invalid=%d target_invalid_dest=%d unknown_name_invalid=%d "
         "sentinels_noop=%d receive_only_rejected=%d receive_cleanup=%d\n",
      task_stable, task_releases, host_stable, host_releases,
      host_final_invalid, target_invalid_dest, unknown_name_invalid,
      sentinels_noop, receive_only_rejected, receive_cleanup);

  return !(task_stable && task_releases && host_stable && host_releases &&
      host_final_invalid && target_invalid_dest && unknown_name_invalid &&
      sentinels_noop && receive_only_rejected && receive_cleanup);
}
