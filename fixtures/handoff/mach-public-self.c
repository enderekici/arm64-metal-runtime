#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_traps.h>
#include <stdio.h>

int main(void) {
  mach_port_t task = mach_task_self();
  mach_port_t host_a = mach_host_self();
  mach_port_t host_b = mach_host_self();
  mach_port_t thread_a = mach_thread_self();
  mach_port_t thread_b = mach_thread_self();
  const int names_ok = task != MACH_PORT_NULL && task != MACH_PORT_DEAD &&
                       host_a != MACH_PORT_NULL && host_a != MACH_PORT_DEAD &&
                       host_a == host_b && thread_a != MACH_PORT_NULL &&
                       thread_a != MACH_PORT_DEAD && thread_a == thread_b;
  const kern_return_t host_release_a = _kernelrpc_mach_port_deallocate_trap(task, host_a);
  const kern_return_t host_release_b = _kernelrpc_mach_port_deallocate_trap(task, host_b);
  const kern_return_t thread_release_a = _kernelrpc_mach_port_deallocate_trap(task, thread_a);
  const kern_return_t thread_release_b = _kernelrpc_mach_port_deallocate_trap(task, thread_b);
  const int releases_ok = host_release_a == KERN_SUCCESS &&
                          host_release_b == KERN_SUCCESS &&
                          thread_release_a == KERN_SUCCESS &&
                          thread_release_b == KERN_SUCCESS;

  printf("task_valid=%d host_valid=%d host_same=%d thread_valid=%d "
         "thread_same=%d releases=%d\n",
         task != MACH_PORT_NULL && task != MACH_PORT_DEAD,
         host_a != MACH_PORT_NULL && host_a != MACH_PORT_DEAD, host_a == host_b,
         thread_a != MACH_PORT_NULL && thread_a != MACH_PORT_DEAD,
         thread_a == thread_b, releases_ok);
  return names_ok && releases_ok ? 0 : 1;
}
