#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if SYS_socket != 97 || SYS_close != 6 || SYS_bind != 104 || \
    SYS_recvfrom != 29 || SYS_sendto != 133 || SYS_write_nocancel != 397
#error "Expected the installed Darwin AF_UNIX datagram syscall numbers"
#endif

enum { kCloseNoCancel = 399, kBadFileDescriptor = 9, kConnectionRefused = 61 };

struct raw_result {
  uint64_t value;
  uint64_t carry;
};

struct receive_state {
  int descriptor;
  struct raw_result result;
  char payload[32];
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

static struct raw_result raw_call6(int number, uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                   uint64_t arg5) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = arg1;
  register uint64_t x2 __asm__("x2") = arg2;
  register uint64_t x3 __asm__("x3") = arg3;
  register uint64_t x4 __asm__("x4") = arg4;
  register uint64_t x5 __asm__("x5") = arg5;
  register uint64_t x16 __asm__("x16") = (uint32_t)number;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %6, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
                     "+r"(x4), "+r"(x5), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

static int make_address(struct sockaddr_un* address, const char* path) {
  volatile unsigned char* bytes = (volatile unsigned char*)address;
  for (unsigned index = 0; index < sizeof(*address); ++index) bytes[index] = 0;
  address->sun_len = 0;
  address->sun_family = AF_UNIX;
  for (unsigned index = 0; index < sizeof(address->sun_path); ++index) {
    const char value = path[index];
    if (value == '\0') return 1;
    address->sun_path[index] = value;
  }
  return 0;
}

static void* receive_worker(void* argument) {
  struct receive_state* state = (struct receive_state*)argument;
  state->result = raw_call6(SYS_recvfrom, state->descriptor,
      (uint64_t)(uintptr_t)state->payload, sizeof(state->payload), 0, 0, 0);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3 || argv[1] == 0 || argv[2] == 0) return 2;
  const int closeNumber = argv[1][0] == '6' && argv[1][1] == '\0'
      ? SYS_close
      : argv[1][0] == 'n' && argv[1][1] == '\0' ? kCloseNoCancel : 0;
  if (closeNumber == 0) return 2;

  struct sockaddr_un address;
  if (!make_address(&address, argv[2])) return 2;
  const struct raw_result server = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (server.carry != 0 || (int64_t)server.value < 0) return 3;
  const struct raw_result bound = raw_call3(SYS_bind, server.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (bound.carry != 0 || bound.value != 0) return 4;

  struct receive_state state;
  state.descriptor = (int)server.value;
  state.result = (struct raw_result){0, 0};
  pthread_t thread;
  if (pthread_create(&thread, 0, receive_worker, &state) != 0) return 5;
  const struct timespec pause = {0, 10 * 1000 * 1000};
  if (nanosleep(&pause, 0) != 0) return 6;

  const struct raw_result closed = raw_call3(closeNumber, server.value, 0, 0);
  if (closed.carry != 0 || closed.value != 0) return 7;
  if (pthread_join(thread, 0) != 0) return 8;
  if (state.result.carry == 0 || state.result.value != kBadFileDescriptor) return 9;

  const struct raw_result client = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (client.carry != 0 || (int64_t)client.value < 0) return 10;
  const struct raw_result sent = raw_call6(SYS_sendto, client.value,
      (uint64_t)(uintptr_t)"x", 1, 0, (uint64_t)(uintptr_t)&address,
      sizeof(address));
  if (sent.carry == 0 || sent.value != kConnectionRefused) return 11;
  const struct raw_result clientClosed = raw_call3(SYS_close, client.value, 0, 0);
  if (clientClosed.carry != 0) return 12;

  const char* output = closeNumber == SYS_close
      ? "close6=EBADF path=ECONNREFUSED\n"
      : "close399=EBADF path=ECONNREFUSED\n";
  const size_t length = closeNumber == SYS_close
      ? sizeof("close6=EBADF path=ECONNREFUSED\n") - 1
      : sizeof("close399=EBADF path=ECONNREFUSED\n") - 1;
  const struct raw_result written = raw_call3(SYS_write_nocancel, 1,
      (uint64_t)(uintptr_t)output, length);
  return written.carry == 0 && written.value == length ? 0 : 13;
}
