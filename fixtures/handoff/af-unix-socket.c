#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if SYS_socket != 97 || SYS_close != 6 || SYS_close_nocancel != 399
#error "Expected the installed Darwin socket and close syscall numbers"
#endif

struct raw_result {
  uint64_t value;
  uint64_t carry;
};

static struct raw_result raw_call3(int number, uint64_t arg0,
                                   uint64_t arg1, uint64_t arg2) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = arg1;
  register uint64_t x2 __asm__("x2") = arg2;
  register uint64_t x16 __asm__("x16") = (uint32_t)number;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %3, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

static int failed(struct raw_result result, int error) {
  return result.carry != 0 && result.value == (uint64_t)error;
}

int main(void) {
  const int types[] = {SOCK_STREAM, SOCK_DGRAM};
  for (unsigned index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
    errno = EDOM;
    const struct raw_result created = raw_call3(SYS_socket, AF_UNIX,
        (uint32_t)types[index], 0);
    if (created.carry != 0 || (int64_t)created.value < 0 || errno != EDOM) return 2;
    const int close_syscall = index == 0 ? SYS_close : SYS_close_nocancel;
    const struct raw_result closed = raw_call3(close_syscall, created.value, 0, 0);
    if (closed.carry != 0 || closed.value != 0 || errno != EDOM) return 3;
    const struct raw_result closed_again = raw_call3(close_syscall, created.value, 0, 0);
    if (!failed(closed_again, EBADF) || errno != EDOM) return 4;
  }

  errno = EBUSY;
  const struct raw_result bad_type = raw_call3(SYS_socket, AF_UNIX, SOCK_SEQPACKET, 0);
  if (errno != EBUSY) return 5;
  errno = ERANGE;
  const struct raw_result bad_protocol = raw_call3(SYS_socket, AF_UNIX, SOCK_STREAM, 1);
  if (errno != ERANGE) return 6;
  errno = EDOM;
  const struct raw_result bad_domain = raw_call3(SYS_socket, 0x7fffffff, SOCK_STREAM, 0);
  if (errno != EDOM) return 7;
  if (!failed(bad_type, EPROTONOSUPPORT) ||
      !failed(bad_protocol, EPROTONOSUPPORT) ||
      !failed(bad_domain, EAFNOSUPPORT)) return 8;

  static const char output[] = "socket=stream,dgram close6/399=ok errors=43,43,47\n";
  return write(STDOUT_FILENO, output, sizeof(output) - 1) ==
      (long)(sizeof(output) - 1) ? 0 : 9;
}
