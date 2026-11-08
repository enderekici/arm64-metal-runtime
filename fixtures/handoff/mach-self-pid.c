#include <mach/mach_traps.h>
#include <mach/mach_init.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static const volatile int read_only_output = -7;

struct ThreadPortProbe {
  uint64_t first;
  uint64_t second;
};

static uint64_t trap_without_arguments(int64_t number, uint64_t stale_x0) {
  register uint64_t x0 __asm("x0") = stale_x0;
  register uint64_t x16 __asm("x16") = (uint64_t)number;
  __asm volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory", "cc");
  return x0;
}

static uint64_t trap_pid_for_task(uint32_t task, int *pid) {
  register uint64_t x0 __asm("x0") = task;
  register uint64_t x1 __asm("x1") = (uint64_t)pid;
  register uint64_t x16 __asm("x16") = (uint64_t)-46;
  __asm volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory", "cc");
  return x0;
}

static uint64_t trap_deallocate(uint32_t task, uint32_t name) {
  register uint64_t x0 __asm("x0") = task;
  register uint64_t x1 __asm("x1") = name;
  register uint64_t x16 __asm("x16") = (uint64_t)-18;
  __asm volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory", "cc");
  return x0;
}

static void *probe_worker_thread_port(void *context) {
  struct ThreadPortProbe *result = (struct ThreadPortProbe *)context;
  result->first = trap_without_arguments(-27, 5);
  result->second = trap_without_arguments(-27, UINT64_C(0xabcdef));
  return NULL;
}

int main(void) {
  const uint64_t thread = trap_without_arguments(-27, 5);
  const uint64_t thread_again = trap_without_arguments(-27, UINT64_C(0xabcdef));
  const uint64_t task = trap_without_arguments(-28, 5);
  const uint64_t task_again = trap_without_arguments(-28, UINT64_C(0xabcdef));
  const uint64_t host = trap_without_arguments(-29, 5);
  const uint64_t host_again = trap_without_arguments(-29, UINT64_C(0xabcdef));
  struct ThreadPortProbe worker = {0, 0};
  pthread_t worker_thread;
  const int worker_create = pthread_create(&worker_thread, NULL,
                                            probe_worker_thread_port, &worker);
  const int worker_join = worker_create == 0 ? pthread_join(worker_thread, NULL) : -1;
  const uint64_t task_cleanup_main_first = trap_deallocate((uint32_t)task, (uint32_t)thread);
  const uint64_t task_cleanup_main_second = trap_deallocate((uint32_t)task,
                                                            (uint32_t)thread_again);
  const uint64_t task_cleanup_worker_first = trap_deallocate((uint32_t)task,
                                                             (uint32_t)worker.first);
  const uint64_t task_cleanup_worker_second = trap_deallocate((uint32_t)task,
                                                              (uint32_t)worker.second);
  int pid = -1;
  const uint64_t pid_result = trap_pid_for_task((uint32_t)task, &pid);
  const int pid_matches = pid == getpid();
  const uint64_t invalid_task = trap_pid_for_task(5, &pid);
  const uint64_t null_output = trap_pid_for_task((uint32_t)task, NULL);
  const uint64_t low_output = trap_pid_for_task((uint32_t)task, (int *)1);
  const uint64_t high_output = trap_pid_for_task(
      (uint32_t)task, (int *)(uintptr_t)UINT64_MAX);
  const uint64_t protected_output = trap_pid_for_task(
      (uint32_t)task, (int *)&read_only_output);
  const int matches_wrappers = task == task_self_trap() && host == mach_host_self();

  const int thread_cleanup = task_cleanup_main_first == 0 &&
      task_cleanup_main_second == 0 && task_cleanup_worker_first == 0 &&
      task_cleanup_worker_second == 0;
  const int worker_valid = worker_create == 0 && worker_join == 0 &&
      worker.first != 0 && worker.first == worker.second;
  printf("thread_reused=%d thread_nonnull=%d worker_valid=%d thread_distinct=%d "
         "thread_cleanup=%d "
         "task_reused=%d host_reused=%d nonnull=%d distinct=%d wrappers_match=%d "
         "pid_result=%llu pid_matches=%d invalid_task=%llu invalid_outputs=%d\n",
         thread == thread_again, thread != 0, worker_valid,
         thread != task && thread != host && thread != worker.first, thread_cleanup,
         task == task_again, host == host_again, task != 0 && host != 0,
         task != host, matches_wrappers, pid_result, pid_matches,
         invalid_task, null_output == 0 && low_output == 0 && high_output == 0 &&
                           protected_output == 0 && read_only_output == -7);
  return !(thread == thread_again && thread != 0 && worker_valid &&
           thread != task && thread != host && thread != worker.first && thread_cleanup &&
           task == task_again && host == host_again && task != 0 && host != 0 &&
           task != host && matches_wrappers && pid_result == 0 && pid_matches &&
           invalid_task == 5 && null_output == 0 && low_output == 0 &&
           high_output == 0 && protected_output == 0 && read_only_output == -7);
}
