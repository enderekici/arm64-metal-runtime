#include <stdint.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if SYS_socket != 97 || SYS_connect != 98 || SYS_close != 6 || \
    SYS_write_nocancel != 397
#error "Expected the installed Darwin AF_UNIX socket syscall numbers"
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

int main(void) {
  const struct raw_result socket = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (socket.carry != 0 || (int64_t)socket.value < 0) return 2;
  const struct raw_result connect = raw_call3(SYS_connect, socket.value, 1, 106);
  if (connect.carry == 0 || connect.value != 14) return 3;
  const struct raw_result close = raw_call3(SYS_close, socket.value, 0, 0);
  if (close.carry != 0 || close.value != 0) return 4;
  static const char output[] = "connect_bad_pointer=EFAULT14\n";
  const struct raw_result written = raw_call3(SYS_write_nocancel, 1,
      (uint64_t)(uintptr_t)output, sizeof(output) - 1);
  return written.carry == 0 && written.value == sizeof(output) - 1 ? 0 : 5;
}
