#include <mach/mach.h>
#include <mach/mach_port.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int mismatch_child(void) {
  mach_port_options_t options = {0};
  options.flags = MPO_CONTEXT_AS_GUARD | MPO_INSERT_SEND_RIGHT | MPO_STRICT;
  mach_port_name_t name = MACH_PORT_NULL;
  const kern_return_t create = _kernelrpc_mach_port_construct_trap(
      mach_task_self(), &options, UINT64_C(0x12345678abcdef01), &name);
  if (create != KERN_SUCCESS) return 2;
  const kern_return_t destroy = _kernelrpc_mach_port_destruct_trap(
      mach_task_self(), name, -1, UINT64_C(0x12345678abcdef02));
  return destroy == KERN_SUCCESS ? 3 : 4;
}

int main(void) {
  const pid_t child = fork();
  if (child == 0) _exit(mismatch_child());
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child) return 1;
  const int strict_termination = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
  printf("mismatch_signaled=%u signal=%d result=%s\n", WIFSIGNALED(status),
      WIFSIGNALED(status) ? WTERMSIG(status) : 0,
      strict_termination ? "PASS" : "FAIL");
  return strict_termination ? 0 : 1;
}
