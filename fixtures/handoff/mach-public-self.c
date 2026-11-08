#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdio.h>

static kern_return_t raw_port_deallocate(mach_port_t task, mach_port_t name) {
  register uint64_t x0 __asm__("x0") = task;
  register uint64_t x1 __asm__("x1") = name;
  register uint64_t x16 __asm__("x16") = (uint64_t)-18;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory", "cc");
  return (kern_return_t)x0;
}

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
  const kern_return_t host_release_a = raw_port_deallocate(task, host_a);
  const kern_return_t host_release_b = raw_port_deallocate(task, host_b);
  const kern_return_t thread_release_a = raw_port_deallocate(task, thread_a);
  const kern_return_t thread_release_b = raw_port_deallocate(task, thread_b);
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
