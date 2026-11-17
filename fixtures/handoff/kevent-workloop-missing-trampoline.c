#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct event_qos {
  uint64_t ident;
  int16_t filter;
  uint16_t flags;
  int32_t qos;
  uint64_t udata;
  uint32_t fflags;
  uint32_t xflags;
  int64_t data;
  uint64_t ext[4];
};
_Static_assert(sizeof(struct event_qos) == 72, "kevent_qos ABI");

static struct trap_result {
  uint64_t value, carry;
} invoke_kevent_id(uint64_t id, struct event_qos *change) {
  register uint64_t x0 __asm__("x0") = id;
  register uint64_t x1 __asm__("x1") = (uintptr_t)change;
  register uint64_t x2 __asm__("x2") = 1;
  register uint64_t x3 __asm__("x3") = 0;
  register uint64_t x4 __asm__("x4") = 0;
  register uint64_t x5 __asm__("x5") = 0;
  register uint64_t x6 __asm__("x6") = 0;
  register uint64_t x7 __asm__("x7") = 0x403;
  register uint64_t x16 __asm__("x16") = 375;
  register uint64_t x17 __asm__("x17");
  __asm__ volatile("svc #0x80\n\tcset x17, cs"
      : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
        "+r"(x6), "+r"(x7), "+r"(x16), "=r"(x17) : : "memory", "cc");
  return (struct trap_result){x0, x17};
}

int main(void) {
  volatile uint64_t owner = 1;
  const uint64_t id = 0x4242;
  struct event_qos change;
  memset(&change, 0, sizeof(change));
  change.ident = id;
  change.filter = -17;
  change.flags = 0x5;
  change.qos = (int32_t)0x800008ffU;
  change.udata = id;
  change.fflags = 0x111;
  change.ext[1] = (uintptr_t)&owner;
  change.ext[2] = 1;
  change.ext[3] = 1;
  const struct trap_result result = invoke_kevent_id(id, &change);
  if (result.carry == 0 || result.value != 45) return 1;
  puts("matching workloop attach rejected without guest worker trampoline (ENOTSUP)");
  return 0;
}
