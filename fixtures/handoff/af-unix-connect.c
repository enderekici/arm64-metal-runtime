#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#if SYS_socket != 97 || SYS_connect != 98 || SYS_close != 6
#error "Expected the installed Darwin socket, connect, and close syscall numbers"
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
  char directory[] = "/tmp/arm64-metal-connect-XXXXXX";
  if (mkdtemp(directory) == NULL) return 2;
  char missing[sizeof(((struct sockaddr_un *)0)->sun_path)];
  if (snprintf(missing, sizeof(missing), "%s/absent", directory) >=
      (int)sizeof(missing)) return 3;

  struct sockaddr_un address = {0};
  address.sun_len = 0;
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, missing, strlen(missing) + 1);
  const struct raw_result created = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (created.carry != 0 || (int64_t)created.value < 0) return 4;

  errno = EDOM;
  const struct raw_result absent = raw_call3(SYS_connect, created.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(absent, ENOENT) || errno != EDOM) return 5;

  address.sun_len = 1;
  errno = EDOM;
  const struct raw_result absent_with_length = raw_call3(SYS_connect, created.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(absent_with_length, ENOENT) || errno != EDOM) return 6;

  errno = EDOM;
  const struct raw_result bad_descriptor = raw_call3(SYS_connect, UINT64_MAX, 0,
      sizeof(address));
  if (!failed(bad_descriptor, EBADF) || errno != EDOM) return 7;

  const int ordinary_file = open("/dev/null", O_RDONLY);
  if (ordinary_file < 0) return 8;
  errno = EDOM;
  const struct raw_result not_socket = raw_call3(SYS_connect, ordinary_file,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(not_socket, ENOTSOCK) || errno != EDOM) return 9;
  if (close(ordinary_file) != 0) return 10;

  errno = EDOM;
  const struct raw_result null_address = raw_call3(SYS_connect, created.value, 0,
      sizeof(address));
  if (!failed(null_address, EINVAL) || errno != EDOM) return 11;

  errno = EDOM;
  const struct raw_result bad_address = raw_call3(SYS_connect, created.value, 1,
      sizeof(address));
  if (!failed(bad_address, EFAULT) || errno != EDOM) return 12;

  const struct raw_result closed = raw_call3(SYS_close, created.value, 0, 0);
  if (closed.carry != 0 || closed.value != 0) return 13;
  if (rmdir(directory) != 0) return 14;

  static const char output[] =
      "connect=sun_len0/1 absent errors=2,9,38,22,14\n";
  return write(STDOUT_FILENO, output, sizeof(output) - 1) ==
      (long)(sizeof(output) - 1) ? 0 : 15;
}
