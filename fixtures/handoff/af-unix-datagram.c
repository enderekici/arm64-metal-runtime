#include <stdint.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#if SYS_socket != 97 || SYS_connect != 98 || SYS_close != 6 || \
    SYS_bind != 104 || SYS_recvfrom != 29 || SYS_sendto != 133 || \
    SYS_write_nocancel != 397
#error "Expected the installed Darwin AF_UNIX datagram syscall numbers"
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

static int failed(struct raw_result result, int error) {
  return result.carry != 0 && result.value == (uint64_t)error;
}

static int make_address(struct sockaddr_un* address, const char* path) {
  volatile unsigned char* bytes = (volatile unsigned char*)address;
  for (unsigned index = 0; index < sizeof(*address); ++index) bytes[index] = 0;
  address->sun_len = 0;
  address->sun_family = AF_UNIX;
  int terminated = 0;
  for (unsigned index = 0; index < sizeof(address->sun_path); ++index) {
    const char value = path[index];
    if (value == '\0') {
      terminated = 1;
      break;
    }
    address->sun_path[index] = value;
  }
  return terminated;
}

int main(int argc, char** argv) {
  if (argc != 3 || argv[1] == 0 || argv[2] == 0) return 2;
  struct sockaddr_un address;
  struct sockaddr_un client_address;
  if (!make_address(&address, argv[1]) || !make_address(&client_address, argv[2]))
    return 2;

  const struct raw_result server = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (server.carry != 0 || (int64_t)server.value < 0) return 3;
  const struct raw_result invalid_pointer = raw_call3(SYS_connect, server.value, 1,
      sizeof(address));
  if (!failed(invalid_pointer, 14)) return 4;
  const struct raw_result invalid_descriptor = raw_call3(SYS_connect, UINT64_MAX, 0,
      sizeof(address));
  if (!failed(invalid_descriptor, 9)) return 5;
  const struct raw_result bound = raw_call3(SYS_bind, server.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (bound.carry != 0 || bound.value != 0) return 6;

  const struct raw_result duplicate = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (duplicate.carry != 0 || (int64_t)duplicate.value < 0) return 7;
  const struct raw_result duplicate_bind = raw_call3(SYS_bind, duplicate.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(duplicate_bind, 48)) return 8;
  const struct raw_result duplicate_closed = raw_call3(SYS_close, duplicate.value, 0, 0);
  if (duplicate_closed.carry != 0 || duplicate_closed.value != 0) return 9;

  const struct raw_result client = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (client.carry != 0 || (int64_t)client.value < 0) return 10;
  const struct raw_result client_bound = raw_call3(SYS_bind, client.value,
      (uint64_t)(uintptr_t)&client_address, sizeof(client_address));
  if (client_bound.carry != 0 || client_bound.value != 0) return 10;
  static const char message[] = "owned guest datagram";
  const struct raw_result missing_peer = raw_call6(SYS_sendto, client.value,
      (uint64_t)(uintptr_t)message, sizeof(message) - 1, 0, 0, 0);
  if (!failed(missing_peer, 39)) return 11;
  const struct raw_result connected = raw_call3(SYS_connect, client.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (connected.carry != 0 || connected.value != 0) return 12;

  const struct raw_result sent = raw_call6(SYS_sendto, client.value,
      (uint64_t)(uintptr_t)message, sizeof(message) - 1, 0, 0, 0);
  if (sent.carry != 0 || sent.value != sizeof(message) - 1) return 13;

  char received[sizeof(message)];
  struct sockaddr_un source;
  volatile unsigned char* source_bytes = (volatile unsigned char*)&source;
  for (unsigned index = 0; index < sizeof(source); ++index) source_bytes[index] = 0;
  uint32_t source_length = sizeof(source);
  const struct raw_result received_result = raw_call6(SYS_recvfrom, server.value,
      (uint64_t)(uintptr_t)received, sizeof(received), 0,
      (uint64_t)(uintptr_t)&source, (uint64_t)(uintptr_t)&source_length);
  if (received_result.carry != 0 || received_result.value != sizeof(message) - 1)
    return 14;
  if (source_length != sizeof(source) || source.sun_len != sizeof(source) ||
      source.sun_family != AF_UNIX) return 14;
  for (unsigned index = 0; index < sizeof(client_address.sun_path); ++index) {
    if (source.sun_path[index] != client_address.sun_path[index]) return 14;
  }
  for (unsigned index = 0; index < sizeof(message) - 1; ++index) {
    if (received[index] != message[index]) return 15;
  }

  const struct raw_result second_sent = raw_call6(SYS_sendto, client.value,
      (uint64_t)(uintptr_t)message, sizeof(message) - 1, 0, 0, 0);
  if (second_sent.carry != 0 || second_sent.value != sizeof(message) - 1) return 15;
  for (unsigned index = 0; index < sizeof(source); ++index) source_bytes[index] = 0;
  source_length = 1;
  const struct raw_result truncated_source = raw_call6(SYS_recvfrom, server.value,
      (uint64_t)(uintptr_t)received, sizeof(received), 0,
      (uint64_t)(uintptr_t)&source, (uint64_t)(uintptr_t)&source_length);
  if (truncated_source.carry != 0 || truncated_source.value != sizeof(message) - 1 ||
      source_length != sizeof(source) || source.sun_len != sizeof(source) ||
      source.sun_family != 0) return 15;

  const struct raw_result server_closed = raw_call3(SYS_close, server.value, 0, 0);
  if (server_closed.carry != 0) return 16;
  const struct raw_result closed_peer = raw_call6(SYS_sendto, client.value,
      (uint64_t)(uintptr_t)message, sizeof(message) - 1, 0, 0, 0);
  if (!failed(closed_peer, 54)) return 17;
  const struct raw_result rebinder = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (rebinder.carry != 0 || (int64_t)rebinder.value < 0) return 18;
  const struct raw_result stale_bind = raw_call3(SYS_bind, rebinder.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(stale_bind, 48)) return 19;
  const struct raw_result rebinder_closed = raw_call3(SYS_close, rebinder.value, 0, 0);
  if (rebinder_closed.carry != 0) return 20;
  const struct raw_result late_client = raw_call3(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
  if (late_client.carry != 0 || (int64_t)late_client.value < 0) return 21;
  const struct raw_result closed_path = raw_call3(SYS_connect, late_client.value,
      (uint64_t)(uintptr_t)&address, sizeof(address));
  if (!failed(closed_path, 61)) return 22;
  const struct raw_result late_closed = raw_call3(SYS_close, late_client.value, 0, 0);
  if (late_closed.carry != 0) return 23;
  const struct raw_result client_closed = raw_call3(SYS_close, client.value, 0, 0);
  if (client_closed.carry != 0) return 24;
  static const char output[] = "af_unix_dgram=bind,connect,sendto,recvfrom,close ok\n";
  const struct raw_result written = raw_call3(SYS_write_nocancel, 1,
      (uint64_t)(uintptr_t)output, sizeof(output) - 1);
  return written.carry == 0 && written.value == sizeof(output) - 1 ? 0 : 25;
}
