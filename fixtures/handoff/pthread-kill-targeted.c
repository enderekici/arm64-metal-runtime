#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if SYS_write_nocancel != 397
#error "Expected the installed Darwin write_nocancel syscall number"
#endif

static pthread_t workers[2];
static volatile sig_atomic_t ready[2];
static volatile sig_atomic_t releaseWorker[2];
static volatile sig_atomic_t received[2];
static volatile sig_atomic_t unexpectedRecipient;

struct raw_result {
  uint64_t value;
  uint64_t carry;
};

static struct raw_result raw_write(int descriptor, const void* data,
                                   uint64_t length) {
  register uint64_t x0 __asm__("x0") = (uint64_t)descriptor;
  register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)data;
  register uint64_t x2 __asm__("x2") = length;
  register uint64_t x16 __asm__("x16") = SYS_write_nocancel;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %3, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

static void signal_handler(int signal) {
  const pthread_t current = pthread_self();
  if (current == workers[0]) received[0] = signal;
  else if (current == workers[1]) received[1] = signal;
  else unexpectedRecipient = signal;
}

static void* worker(void* argument) {
  const int index = (int)(intptr_t)argument;
  if (index == 0) {
    const sigset_t blocked = (sigset_t)1U << (SIGUSR1 - 1);
    if (pthread_sigmask(SIG_BLOCK, &blocked, 0) != 0) return (void*)1;
  }
  ready[index] = 1;
  while (releaseWorker[index] == 0) sched_yield();
  if (index == 0) {
    const sigset_t unblocked = (sigset_t)1U << (SIGUSR1 - 1);
    if (pthread_sigmask(SIG_UNBLOCK, &unblocked, 0) != 0) return (void*)2;
  }
  while (received[index] == 0) sched_yield();
  return 0;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, 0) != 0 ||
      sigaction(SIGUSR2, &action, 0) != 0) return 1;

  if (pthread_create(&workers[0], 0, worker, (void*)(intptr_t)0) != 0 ||
      pthread_create(&workers[1], 0, worker, (void*)(intptr_t)1) != 0) return 2;
  for (unsigned attempt = 0; attempt < 10000 && (!ready[0] || !ready[1]); ++attempt) {
    sched_yield();
  }
  if (!ready[0] || !ready[1]) return 3;

  const pthread_t invalid = (pthread_t)(uintptr_t)0;
  if (pthread_kill(invalid, 0) != ESRCH) return 4;
  if (pthread_kill(workers[0], NSIG) != EINVAL) return 5;
  if (pthread_kill(workers[0], 0) != 0) return 6;
  if (pthread_kill(workers[0], SIGUSR1) != 0) return 7;
  if (pthread_kill(workers[1], SIGUSR2) != 0) return 8;

  for (unsigned attempt = 0; attempt < 10000 && received[1] == 0; ++attempt) {
    sched_yield();
  }
  if (received[0] != 0 || received[1] != SIGUSR2 || unexpectedRecipient != 0) return 9;

  releaseWorker[0] = 1;
  releaseWorker[1] = 1;
  if (pthread_join(workers[0], 0) != 0 || pthread_join(workers[1], 0) != 0) return 10;
  if (received[0] != SIGUSR1 || received[1] != SIGUSR2 ||
      unexpectedRecipient != 0) return 11;

  static const char output[] = "pthread_kill=targeted mask=pending ESRCH EINVAL\n";
  const struct raw_result written = raw_write(1, output, sizeof(output) - 1);
  return written.carry == 0 && written.value == sizeof(output) - 1 ? 0 : 12;
}
