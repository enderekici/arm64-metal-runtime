// ARM64_METAL_SURVEY positive control, raw traps only (runs without the shared
// cache). An unknown msgh_id (0x7777) sent twice to the host port with a reply
// port, then two real BSD syscalls the runtime does not model, each issued
// twice (aio_cancel on fd -1, aio_error on NULL).
// Native: aio_cancel=EBADF, aio_error=EINVAL, host replies MIG_BAD_ID (-303).
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t raw_syscall(uint64_t number, uint64_t arg0, int *carry) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = 0;
  register uint64_t x2 __asm__("x2") = 0;
  register uint64_t x3 __asm__("x3") = 0;
  register uint64_t x16 __asm__("x16") = number;
  uint64_t flags = 0;
  __asm__ volatile("svc #0x80\n\tmrs %1, nzcv"
                   : "+r"(x0), "=r"(flags)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x16)
                   : "memory", "cc");
  *carry = (int)((flags >> 29) & 1U);
  return x0;
}

static uint64_t raw_trap0(int64_t number) {
  register uint64_t x0 __asm__("x0") = 0;
  register uint64_t x16 __asm__("x16") = (uint64_t)number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory", "cc");
  return x0;
}

static uint64_t raw_msg2(uint64_t address, uint64_t options, uint64_t bits_and_size,
                         uint64_t ports, uint64_t voucher_and_id,
                         uint64_t descriptors_and_receive, uint64_t size_and_priority,
                         uint64_t timeout) {
  register uint64_t x0 __asm__("x0") = address;
  register uint64_t x1 __asm__("x1") = options;
  register uint64_t x2 __asm__("x2") = bits_and_size;
  register uint64_t x3 __asm__("x3") = ports;
  register uint64_t x4 __asm__("x4") = voucher_and_id;
  register uint64_t x5 __asm__("x5") = descriptors_and_receive;
  register uint64_t x6 __asm__("x6") = size_and_priority;
  register uint64_t x7 __asm__("x7") = timeout;
  register uint64_t x16 __asm__("x16") = (uint64_t)-47;
  __asm__ volatile("svc #0x80"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6),
                     "r"(x7), "r"(x16)
                   : "memory", "cc");
  return x0;
}

int main(void) {
  const mach_port_t host = (mach_port_t)raw_trap0(-29);
  const mach_port_t reply = (mach_port_t)raw_trap0(-26);
  for (unsigned index = 0; index < 2; ++index) {
    union {
      mach_msg_header_t header;
      mig_reply_error_t error;
      unsigned char bytes[64];
    } wire;
    memset(&wire, 0, sizeof(wire));
    wire.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    wire.header.msgh_size = sizeof(mach_msg_header_t);
    wire.header.msgh_remote_port = host;
    wire.header.msgh_local_port = reply;
    wire.header.msgh_id = 0x7777;
    const uint64_t result = raw_msg2(
        (uint64_t)(uintptr_t)&wire, UINT64_C(0x200000003),
        ((uint64_t)sizeof(mach_msg_header_t) << 32) | wire.header.msgh_bits,
        ((uint64_t)reply << 32) | host, (uint64_t)0x7777 << 32, (uint64_t)reply << 32,
        sizeof(wire), 0);
    printf("msgh_id 0x7777 -> result=0x%llx reply_id=%d retcode=%d\n",
           (unsigned long long)result, wire.header.msgh_id, wire.error.RetCode);
  }
  const uint64_t numbers[] = {316, 316, 317, 317};
  const uint64_t args[] = {(uint64_t)-1, (uint64_t)-1, 0, 0};
  for (unsigned index = 0; index < 4; ++index) {
    int carry = 0;
    const uint64_t result = raw_syscall(numbers[index], args[index], &carry);
    printf("syscall %llu -> x0=%llu carry=%d\n", (unsigned long long)numbers[index],
           (unsigned long long)result, carry);
  }
  printf("done\n");
  fflush(stdout);
  return 0;
}
