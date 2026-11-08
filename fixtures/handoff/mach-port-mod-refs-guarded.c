#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <stdio.h>

int main(void) {
  const mach_port_t task = mach_task_self();
  mach_port_options_t options = {0};
  options.flags = MPO_CONTEXT_AS_GUARD;
  mach_port_t guarded = MACH_PORT_NULL;
  if (_kernelrpc_mach_port_construct_trap(task, &options, 0xabc, &guarded) !=
          KERN_SUCCESS ||
      guarded == MACH_PORT_NULL) {
    return 1;
  }
  puts("guarded_port_ready=1");
  fflush(stdout);
  (void)_kernelrpc_mach_port_mod_refs_trap(
      task, guarded, MACH_PORT_RIGHT_RECEIVE, -1);
  return 99;
}
