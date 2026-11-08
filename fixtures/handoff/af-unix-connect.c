#include <stdint.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#if SYS_socket != 97 || SYS_connect != 98 || SYS_close != 6 || \
    SYS_write_nocancel != 397
#error "Expected the installed Darwin socket syscalls"
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

int main(int argc, char** argv) {
  if (argc != 2 || argv[1] == 0) return 2;
  struct sockaddr_un address;
  volatile unsigned char* address_bytes = (volatile unsigned char*)&address;
  for (unsigned index = 0; index < sizeof(address); ++index) address_bytes[index] = 0;
  address.sun_len = 0;
  address.sun_family = AF_UNIX;
  int terminated = 0;
  for (unsigned index = 0; index < sizeof(address.sun_path); ++index) {
    const char value = argv[1][index];
    if (value == '\0') {
      terminated = 1;
      break;
    }
    address.sun_path[index] = value;
  }
  if (!terminated) return 2;

  const struct raw_result created = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (created.carry != 0 || (int64_t)created.value < 0) return 3;
  const struct raw_result absent = raw_call3(SYS_connect, created.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(absent, 2)) return 4;

  const struct raw_result bad_descriptor = raw_call3(SYS_connect, UINT64_MAX, 0,
      sizeof(address));
  if (!failed(bad_descriptor, 9)) return 5;
  const struct raw_result null_address = raw_call3(SYS_connect, created.value, 0,
      sizeof(address));
  if (!failed(null_address, 22)) return 6;
  const struct raw_result bad_address = raw_call3(SYS_connect, created.value, 1,
      sizeof(address));
  if (!failed(bad_address, 14)) return 7;

  const struct raw_result closed = raw_call3(SYS_close, created.value, 0, 0);
  if (closed.carry != 0 || closed.value != 0) return 8;
  static const char output[] = "connect=guest-namespace-absent errors=2,9,22,14\n";
  const struct raw_result written = raw_call3(SYS_write_nocancel, 1,
      (uint64_t)(uintptr_t)output, sizeof(output) - 1);
  return written.carry == 0 && written.value == sizeof(output) - 1 ? 0 : 9;
}
