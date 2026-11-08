#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_port.h>
#include <stdio.h>

static int refs_for(mach_port_t task, mach_port_t name, mach_port_urefs_t* refs) {
  return mach_port_get_refs(task, name, MACH_PORT_RIGHT_SEND, refs);
}

int main(void) {
  mach_port_t task = mach_task_self();
  mach_port_t host_baseline = mach_host_self();
  mach_port_urefs_t host_before = 0;
  if (refs_for(task, host_baseline, &host_before) != KERN_SUCCESS ||
      mach_port_deallocate(task, host_baseline) != KERN_SUCCESS) {
    return 1;
  }
  mach_port_t host_a = mach_host_self();
  mach_port_t host_b = mach_host_self();
  mach_port_urefs_t host_after = 0;
  const int host_refs_result = refs_for(task, host_a, &host_after);
  const int host_ok = host_a == host_b && host_refs_result == KERN_SUCCESS &&
                      host_after == host_before + 1;
  (void)mach_port_deallocate(task, host_a);
  (void)mach_port_deallocate(task, host_b);

  mach_port_t thread_baseline = mach_thread_self();
  mach_port_urefs_t thread_before = 0;
  if (refs_for(task, thread_baseline, &thread_before) != KERN_SUCCESS ||
      mach_port_deallocate(task, thread_baseline) != KERN_SUCCESS) {
    return 2;
  }
  mach_port_t thread_a = mach_thread_self();
  mach_port_t thread_b = mach_thread_self();
  mach_port_urefs_t thread_after = 0;
  const int thread_refs_result = refs_for(task, thread_a, &thread_after);
  const int thread_ok = thread_a == thread_b && thread_refs_result == KERN_SUCCESS &&
                        thread_after == thread_before + 1;
  (void)mach_port_deallocate(task, thread_a);
  (void)mach_port_deallocate(task, thread_b);

  printf("host_same=%d host_delta=%d thread_same=%d thread_delta=%d\n",
         host_a == host_b, (int)host_after - (int)host_before,
         thread_a == thread_b, (int)thread_after - (int)thread_before);
  return host_ok && thread_ok ? 0 : 3;
}
