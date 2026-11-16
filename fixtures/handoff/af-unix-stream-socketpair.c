#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static long syscall_result(long number, long a0, long a1, long a2, long a3,
                           long a4, long a5, int* carry) {
  register long x16 __asm__("x16") = number;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  int failed;
  __asm__ volatile("svc #0x80\n\tcset %w1, cs"
                   : "+r"(x0), "=r"(failed)
                   : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory", "cc");
  *carry = failed;
  return x0;
}

static int carry;
#define CALL(number, a0, a1, a2, a3, a4, a5) \
  syscall_result((number), (long)(a0), (long)(a1), (long)(a2), (long)(a3), \
                 (long)(a4), (long)(a5), &carry)

int main(void) {
  int pair[2] = {-1, -1};
  long result = CALL(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, pair, 0, 0);
  printf("pair: carry=%d valid=%d\n", carry,
         !carry && pair[0] >= 3 && pair[1] >= 3 && pair[0] != pair[1]);
  if (carry) return 1;

  char buffer[16] = {0};
  result = CALL(SYS_sendto, pair[0], "left", 4, 0, 0, 0);
  printf("send_lr: carry=%d val=%ld\n", carry, carry ? 0 : result);
  result = CALL(SYS_recvfrom, pair[1], buffer, sizeof(buffer), 0, 0, 0);
  printf("recv_lr: carry=%d val=%ld data=%s\n", carry, carry ? 0 : result, buffer);
  result = CALL(SYS_sendto, pair[1], "right", 5, 0, 0, 0);
  printf("send_rl: carry=%d val=%ld\n", carry, carry ? 0 : result);
  memset(buffer, 0, sizeof(buffer));
  result = CALL(SYS_recvfrom, pair[0], buffer, sizeof(buffer), 0, 0, 0);
  printf("recv_rl: carry=%d val=%ld data=%s\n", carry, carry ? 0 : result, buffer);

  result = CALL(SYS_sendto, 999, "x", 1, 0, 0, 0);
  printf("send_bad_fd: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_recvfrom, 999, buffer, sizeof(buffer), 0, 0, 0);
  printf("recv_bad_fd: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_socketpair, AF_INET, SOCK_STREAM, 0, pair, 0, 0);
  printf("bad_family: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_socketpair, 9999, SOCK_STREAM, 0, pair, 0, 0);
  printf("bad_domain: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, 1, 0, 0);
  printf("bad_output: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_socketpair, AF_INET, SOCK_STREAM, 0, 1, 0, 0);
  printf("bad_family_output: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_socketpair, 9999, SOCK_STREAM, 0, 1, 0, 0);
  printf("bad_domain_output: carry=%d err=%ld\n", carry, carry ? result : 0);
  result = CALL(SYS_socketpair, AF_UNIX, 0xdead, 0, 1, 0, 0);
  printf("bad_type_output: carry=%d err=%ld\n", carry, carry ? result : 0);

  result = CALL(SYS_close, pair[1], 0, 0, 0, 0, 0);
  printf("close_peer: carry=%d\n", carry);
  result = CALL(SYS_recvfrom, pair[0], buffer, sizeof(buffer), 0, 0, 0);
  printf("eof: carry=%d val=%ld\n", carry, carry ? 0 : result);
  result = CALL(SYS_close, pair[0], 0, 0, 0, 0, 0);
  printf("close_local: carry=%d\n", carry);
  return 0;
}
