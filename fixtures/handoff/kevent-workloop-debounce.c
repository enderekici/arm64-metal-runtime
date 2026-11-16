#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EV_ADD 0x0001
#define EV_ENABLE 0x0004
#define KEVENT_FLAG_IMMEDIATE 0x001
#define KEVENT_FLAG_ERROR_EVENTS 0x002
#define KEVENT_FLAG_WORKLOOP 0x400
#define EVFILT_WORKLOOP (-17)
#define NOTE_WL_THREAD_REQUEST 0x001
#define NOTE_WL_UPDATE_QOS 0x010
#define NOTE_WL_IGNORE_ESTALE 0x100
#define QOS_THREAD_REQUEST 0x080008ff

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

static void invoke_kevent_id(uint64_t id, struct event_qos *change, uint32_t flags,
                             long *result, int *error) {
  register long x0 __asm__("x0") = (long)id;
  register long x1 __asm__("x1") = (long)(uintptr_t)change;
  register long x2 __asm__("x2") = 1;
  register long x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0;
  register long x5 __asm__("x5") = 0;
  register long x6 __asm__("x6") = 0;
  register long x7 __asm__("x7") = (long)flags;
  register long x16 __asm__("x16") = 375;
  long carry;
  __asm__ volatile("svc #0x80\n\tcset %w[carry], cs"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
                     "+r"(x5), "+r"(x6), "+r"(x7), [carry] "=r"(carry)
                   : "r"(x16) : "cc", "memory");
  *result = x0;
  *error = (int)carry;
}

int main(void) {
  volatile uint64_t workloopIdentity = 0x1234;
  volatile uint64_t ownerState = 0;
  const uint64_t id = (uint64_t)(uintptr_t)&workloopIdentity;
  struct event_qos change;
  memset(&change, 0, sizeof(change));
  change.ident = id;
  change.filter = EVFILT_WORKLOOP;
  change.flags = EV_ADD | EV_ENABLE;
  change.qos = QOS_THREAD_REQUEST;
  change.udata = id;
  change.fflags = NOTE_WL_THREAD_REQUEST | NOTE_WL_UPDATE_QOS | NOTE_WL_IGNORE_ESTALE;
  change.ext[1] = (uint64_t)(uintptr_t)&ownerState;
  change.ext[2] = UINT64_MAX;
  change.ext[3] = 1;  // Deliberately stale against ownerState with a full mask.
  const uint32_t flags = KEVENT_FLAG_IMMEDIATE | KEVENT_FLAG_ERROR_EVENTS |
                         KEVENT_FLAG_WORKLOOP;
  long result = 0;
  int error = 0;
  invoke_kevent_id(id, &change, flags, &result, &error);
  printf("rc=%ld error=%d flags=0x%x data=%lld\n", result, error, change.flags,
         (long long)change.data);
  return error || result != 0;
}
