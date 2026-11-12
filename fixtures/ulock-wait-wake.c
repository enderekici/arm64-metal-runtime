#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define NO_ERRNO 0x01000000u
#define WAKE_ALL 0x100u

static long sc(long n, long a, long b, long c, long d, long e, int* carry) {
  register long x16 __asm__("x16") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  int failed;
  __asm__ volatile("svc #0x80\n\tcset %w1, cs"
                   : "+r"(x0), "=r"(failed)
                   : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                   : "memory", "cc");
  *carry = failed;
  return x0;
}

static volatile uint32_t word;
static volatile uint32_t napword;
static void nap(void) {
  int c;
  (void)sc(515, 1, (long)&napword, 0, 50000, 0, &c);
}
static volatile uint64_t word64;

static void* waiter(void* arg) {
  (void)arg;
  int c;
  long r = sc(515, 1, (long)&word, 0, 0, 0, &c);
  printf("A woke: carry=%d r=%s word=%u\n", c, r >= 0 ? "ok" : "err", word);
  return 0;
}

static void* waiter2(void* arg) {
  (void)arg;
  int c;
  long r = sc(544, 1 | NO_ERRNO, (long)&word, 0, 0, 0, &c);
  printf("wait2 woke: carry=%d r=%s\n", c, r >= 0 ? "ok" : "err");
  return 0;
}

int main(void) {
  int c;
  long r;
  /* mismatch returns at once, success */
  word = 7;
  r = sc(515, 1, (long)&word, 8, 0, 0, &c);
  printf("mismatch: carry=%d r=%ld\n", c, r);
  /* timed wait times out */
  r = sc(515, 1, (long)&word, 7, 20000, 0, &c);
  printf("timeout: carry=%d errno=%ld\n", c, r);
  r = sc(515, 1 | NO_ERRNO, (long)&word, 7, 20000, 0, &c);
  printf("timeout noerrno: carry=%d r=%ld\n", c, r);
  r = sc(544, 1 | NO_ERRNO, (long)&word, 7, 20000000, 0, &c);
  printf("wait2 timeout noerrno: carry=%d r=%ld\n", c, r);
  /* wake with no waiter */
  r = sc(516, 1, (long)&word, 0, 0, 0, &c);
  printf("wake none: carry=%d errno=%ld\n", c, r);
  r = sc(516, 1 | NO_ERRNO, (long)&word, 0, 0, 0, &c);
  printf("wake none noerrno: carry=%d r=%ld\n", c, r);
  /* 64-bit compare */
  word64 = 0x100000000ull;
  r = sc(515, 5, (long)&word64, 0, 0, 0, &c);
  printf("64-bit mismatch: carry=%d r=%ld\n", c, r);
  /* thread A waits, main stores and wakes */
  word = 0;
  pthread_t t;
  pthread_create(&t, 0, waiter, 0);
  nap();
  int n = 0;
  do {
    word = 1;
    r = sc(516, 1, (long)&word, 0, 0, 0, &c);
    n++;
  } while (c && n < 2000000);
  pthread_join(t, 0);
  printf("wake delivered\n");
  /* wake all with two waiters on wait2 */
  word = 0;
  pthread_t t1, t2;
  pthread_create(&t1, 0, waiter2, 0);
  pthread_create(&t2, 0, waiter2, 0);
  nap();
  word = 1;
  for (int i = 0; i < 3; i++) {
    r = sc(516, 1 | WAKE_ALL | NO_ERRNO, (long)&word, 0, 0, 0, &c);
    (void)r;
  }
  pthread_join(t1, 0);
  pthread_join(t2, 0);
  printf("done\n");
  return 0;
}
