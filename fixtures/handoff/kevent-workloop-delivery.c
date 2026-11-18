#include <errno.h>
#include <ptrauth.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define EV_ADD 0x0001
#define EV_DELETE 0x0002
#define EV_ENABLE 0x0004
#define EV_CLEAR 0x0020
#define EV_ONESHOT 0x0010
#define EVFILT_TIMER (-7)
#define WQOPS_THREAD_KEVENT_RETURN 0x040
#define NOTE_WL_THREAD_REQUEST 0x001
#define WQOPS_THREAD_WORKLOOP_RETURN 0x100
#define WQ_FLAG_THREAD_PRIO_QOS 0x00004000U
#define WQ_FLAG_THREAD_NEWSPI 0x00040000U
#define WQ_FLAG_THREAD_KEVENT 0x00080000U
#define WQ_FLAG_THREAD_TSD_BASE_SET 0x00200000U
#define WQ_FLAG_THREAD_WORKLOOP 0x00400000U
#define WQ_FLAG_THREAD_PRIO_MASK 0x00000fffU

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

struct registration_data {
  uint64_t version, dispatch_queue_offset, main_qos;
  uint32_t tsd_offset, return_to_kernel_offset, mach_thread_self_offset;
  uint64_t stack_addr_hint;
  uint32_t mutex_default_policy, joinable_offset_bits, wq_quantum_expiry_offset;
} __attribute__((packed));
struct dispatch_config { uint32_t version, flags; uint64_t serial_offset, label_offset; };
struct trap_result { uint64_t value, carry; };

static _Atomic unsigned delivery_bits;
static _Atomic unsigned deliveries;
static const uint64_t workloop_ids[] = {0x4242, 0x4343};
static volatile uint64_t debounce_owner = 1;

static struct trap_result darwin_syscall(uint64_t number, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
  register uint64_t x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  register uint64_t x4 __asm__("x4") = a4;
  register uint64_t x5 __asm__("x5") = a5;
  register uint64_t x6 __asm__("x6") = a6;
  register uint64_t x7 __asm__("x7") = a7;
  register uint64_t x16 __asm__("x16") = number;
  register uint64_t x17 __asm__("x17");
  __asm__ volatile("svc #0x80\n\tcset x17, cs"
      : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
        "+r"(x6), "+r"(x7), "+r"(x16), "=r"(x17) : : "memory", "cc");
  return (struct trap_result){x0, x17};
}

__attribute__((noinline)) static void worker_start(void *self, uint32_t port,
    void *stack_low, void *list, uint32_t flags, int count) {
  if (!(flags & WQ_FLAG_THREAD_WORKLOOP)) {
    const struct event_qos *timer = list;
    if (timer && count == 1 && timer->filter == EVFILT_TIMER && timer->ident == 17 &&
        timer->data >= 1) atomic_fetch_add_explicit(&deliveries, 1, memory_order_release);
    (void)darwin_syscall(368, WQOPS_THREAD_KEVENT_RETURN, 0, 0, 0, 0, 0, 0, 0);
    __builtin_trap();
  }
  unsigned bits = self != NULL && port != 0 && (uintptr_t)stack_low < (uintptr_t)self;
  if (list != NULL && count == 1) {
    const struct event_qos *event = (const struct event_qos *)list;
    const uint32_t qos_class = ((uint32_t)event->qos & 0x00003f00U) >> 8U;
    const uint32_t qos_index = (uint32_t)__builtin_ffs((int)qos_class);
    const uint32_t expected_flags = WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_KEVENT |
        WQ_FLAG_THREAD_TSD_BASE_SET | WQ_FLAG_THREAD_WORKLOOP |
        WQ_FLAG_THREAD_PRIO_QOS | (qos_index & WQ_FLAG_THREAD_PRIO_MASK);
    if ((event->ident == workloop_ids[0] || event->ident == workloop_ids[1]) &&
        event->filter == -17 && event->flags == 0x25 &&
        (uint32_t)event->qos == 0x800008ffU && event->udata == event->ident &&
        event->fflags == 0x111 && event->ext[1] == (uintptr_t)&debounce_owner &&
        event->ext[2] == 1 && event->ext[3] == 1 && flags == expected_flags) bits |= 2U;
  }
  atomic_fetch_or_explicit(&delivery_bits, bits, memory_order_release);
  atomic_fetch_add_explicit(&deliveries, 1, memory_order_release);
  if (list == NULL || count != 1) __builtin_trap();
  struct event_qos return_change = *(const struct event_qos *)list;
  return_change.flags = EV_ADD | EV_DELETE | EV_ENABLE;
  return_change.qos = 0;
  return_change.fflags = NOTE_WL_THREAD_REQUEST;
  return_change.xflags = 0;
  return_change.data = 0;
  return_change.ext[3] = debounce_owner;
  (void)darwin_syscall(368, WQOPS_THREAD_WORKLOOP_RETURN,
      (uintptr_t)&return_change, 1, 0, 0, 0, 0, 0);
  __builtin_trap();
}

static int timer_while_runnable(void) {
  struct event_qos timer = {.ident = 17, .filter = EVFILT_TIMER, .flags = EV_ADD | EV_ONESHOT,
                            .qos = (int32_t)0x800008ffU, .data = 5};
  struct trap_result result = darwin_syscall(374, -1, (uintptr_t)&timer, 1, 0, 0, 0, 0, 0x21);
  if (result.carry || result.value) return 14;
  // A running thread must receive the asynchronous upcall without yielding or entering the kernel.
  for (unsigned retry = 0; retry < 1000000; ++retry) {
    if (atomic_load_explicit(&deliveries, memory_order_acquire)) {
      const char marker[] = "workq-timer delivered\n";
      return write(1, marker, sizeof(marker) - 1) == sizeof(marker) - 1 ? 0 : 16;
    }
  }
  return 15;
}

int main(int argc, char **argv) {
  const int timer_mode = argc == 2 && strcmp(argv[1], "--poll-timer") == 0;
  if (argc != 1 && !timer_mode) return 64;
  struct registration_data registration_data = {
    sizeof(registration_data), 160, 0, 224, 40, 24, 0, 0, 392, 960,
  };
  uintptr_t entry = (uintptr_t)worker_start;
  __asm__ volatile("mov x10, xzr\n\tpacia %0, x10" : "+r"(entry) : : "x10");
  const uint64_t page = 16384;
  const uint64_t pthread_size = (0x18e0U + page - 1U) & ~(page - 1U);
  struct trap_result result = darwin_syscall(366, entry, entry, pthread_size,
      (uintptr_t)&registration_data, sizeof(registration_data), 160, 0, 0);
  if (result.carry) return 10;
  struct dispatch_config dispatch = {.version = 2};
  result = darwin_syscall(368, 0x400, (uintptr_t)&dispatch, sizeof(dispatch), 0, 0, 0, 0, 0);
  if (result.carry) return 12;
  result = darwin_syscall(367, 0, 0, 0, 0, 0, 0, 0, 0);
  if (result.carry) return 13;
  if (timer_mode) return timer_while_runnable();

  struct event_qos change;
  memset(&change, 0, sizeof(change));
  change.ident = workloop_ids[0];
  change.filter = -17;
  change.flags = 0x5;
  change.qos = (int32_t)0x800008ffU;
  change.udata = workloop_ids[0];
  change.fflags = 0x111;
  change.ext[1] = (uintptr_t)&debounce_owner;
  change.ext[2] = 1;
  change.ext[3] = 1;
  debounce_owner = 2;
  result = darwin_syscall(375, workloop_ids[0], (uintptr_t)&change, 1,
      (uintptr_t)&change, 0, 0, 0, 0x403);
  if (result.carry || result.value != 0 ||
      atomic_load_explicit(&deliveries, memory_order_acquire) != 0) return 14;
  debounce_owner = 1;
  for (unsigned index = 0; index < 2; ++index) {
    if (index != 0) {
      change.ident = workloop_ids[index];
      change.udata = workloop_ids[index];
    }
    result = darwin_syscall(375, workloop_ids[index], (uintptr_t)&change, 1,
        (uintptr_t)&change, 0, 0, 0, 0x403);
    if (result.carry || result.value != 0) return 15;
  }
  for (unsigned retry = 0; retry < 100000; ++retry) {
    if (atomic_load_explicit(&deliveries, memory_order_acquire) >= 2) break;
    sched_yield();
  }
  if (atomic_load_explicit(&deliveries, memory_order_acquire) != 2) return 16;
  change.flags = 0x5;
  change.fflags = 0x111;
  for (unsigned index = 0; index < 2; ++index) {
    change.ident = workloop_ids[index];
    change.udata = workloop_ids[index];
    result = darwin_syscall(375, workloop_ids[index], (uintptr_t)&change, 1,
        (uintptr_t)&change, 0, 0, 0, 0x403);
    if (result.carry || result.value != 0) return 17;
    const unsigned expected_delivery = index + 3;
    for (unsigned retry = 0; retry < 100000; ++retry) {
      if (atomic_load_explicit(&deliveries, memory_order_acquire) >= expected_delivery) break;
      sched_yield();
    }
    if (atomic_load_explicit(&deliveries, memory_order_acquire) != expected_delivery) return 18;
  }
  for (unsigned index = 0; index < 2; ++index) {
    change.ident = workloop_ids[index];
    change.udata = workloop_ids[index];
    change.flags = 0x2;
    change.fflags = 0x121;
    result = darwin_syscall(375, workloop_ids[index], (uintptr_t)&change, 1,
      (uintptr_t)&change, 0, 0, 0, 0x403);
    if (result.carry || result.value != 0) return 19;
  }
  if (atomic_load_explicit(&delivery_bits, memory_order_acquire) != 3U) return 20;
  static const char ok[] = "workloop delivery and owner return ok\n";
  return write(STDOUT_FILENO, ok, sizeof(ok) - 1) == (ssize_t)(sizeof(ok) - 1) ? 0 : 16;
}
