#include <mach/mach.h>
#include <mach/semaphore.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static long sc(long n, long a, long b, long c, long d, long e, long f, int* carry) {
  register long x16 __asm__("x16") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  int failed;
  __asm__ volatile("svc #0x80\n\tcset %w1, cs"
                   : "+r"(x0), "=r"(failed)
                   : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory", "cc");
  *carry = failed;
  return x0;
}

static semaphore_t sem;

static void* signaler(void* arg) {
  (void)arg;
  struct timespec ts = {0, 30 * 1000 * 1000};
  nanosleep(&ts, 0);
  semaphore_signal(sem);
  return 0;
}

int main(void) {
  int c;
  long r;
  semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
  semaphore_signal(sem);
  r = sc(423, sem, 0, 0, 0, 0, 0, &c);
  printf("signaled: carry=%d r=%ld\n", c, r);
  r = sc(423, sem, 0, 1, 1, 0, 20 * 1000 * 1000, &c);
  printf("timeout relative: carry=%d r=%ld\n", c, r);
  r = sc(334, sem, 0, 1, 1, 0, 0, &c);
  printf("timeout zero: carry=%d r=%ld\n", c, r);
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  r = sc(423, sem, 0, 1, 0, now.tv_sec + 1, 0, &c);
  printf("timeout absolute: carry=%d r=%ld\n", c, r);
  r = sc(423, 0x7777, 0, 0, 0, 0, 0, &c);
  printf("bad sem: carry=%d r=%ld\n", c, r);
  pthread_t t;
  pthread_create(&t, 0, signaler, 0);
  r = sc(423, sem, 0, 1, 1, 5, 0, &c);
  printf("woken by thread: carry=%d r=%ld\n", c, r);
  pthread_join(t, 0);
  semaphore_signal(sem);
  r = sc(334, sem, 0, 0, 0, 0, 0, &c);
  printf("cancelable signaled: carry=%d r=%ld\n", c, r);
  printf("done\n");
  return 0;
}
