#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/thread_switch.h>

static uint64_t trap(int n, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                     uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
  register uint64_t x0 __asm__("x0") = a;
  register uint64_t x1 __asm__("x1") = b;
  register uint64_t x2 __asm__("x2") = c;
  register uint64_t x3 __asm__("x3") = d;
  register uint64_t x4 __asm__("x4") = e;
  register uint64_t x5 __asm__("x5") = f;
  register uint64_t x6 __asm__("x6") = g;
  register uint64_t x7 __asm__("x7") = h;
  register int64_t x16 __asm__("x16") = -n;
  __asm__ volatile("svc #0x80"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
                     "+r"(x6), "+r"(x7)
                   : "r"(x16)
                   : "cc", "memory");
  return x0;
}
#define T0(n) trap(n, 0, 0, 0, 0, 0, 0, 0, 0)
#define T1(n, a) trap(n, (uint64_t)(a), 0, 0, 0, 0, 0, 0, 0)
#define T2(n, a, b) trap(n, (uint64_t)(a), (uint64_t)(b), 0, 0, 0, 0, 0, 0)
#define T3(n, a, b, c) trap(n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0, 0, 0, 0)
#define T4(n, a, b, c, d) \
  trap(n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), 0, 0, 0, 0)
#define T6(n, a, b, c, d, e, f)                                               \
  trap(n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d),          \
       (uint64_t)(e), (uint64_t)(f), 0, 0)

#define P(...) (printf(__VA_ARGS__), fflush(stdout))
#define RC(x) ((unsigned)(uint32_t)(x))

static mach_port_t task;
static mach_timebase_info_data_t tb;
static uint64_t ms_to_ticks(uint64_t ms) {
  return ms * 1000000ULL * tb.denom / tb.numer;
}
static uint64_t ticks_to_ms(uint64_t t) { return t * tb.numer / tb.denom / 1000000ULL; }

typedef struct {
  mach_msg_header_t h;
  char pad[64];
} rcv_t;

static kern_return_t receive(mach_port_t port, rcv_t* m, mach_msg_timeout_t ms, int timeout) {
  memset(m, 0x5a, sizeof(*m));
  return mach_msg(&m->h, MACH_RCV_MSG | (timeout ? MACH_RCV_TIMEOUT : 0), 0, sizeof(*m),
                  port, ms, MACH_PORT_NULL);
}

static void show_msg(const char* label, kern_return_t kr, const rcv_t* m) {
  P("%s rc=%x", label, RC(kr));
  if (kr == 0) {
    P(" size=%u id=%d bits=%x remote=%d local=%d voucher=%d", m->h.msgh_size, m->h.msgh_id,
      m->h.msgh_bits, m->h.msgh_remote_port != 0, m->h.msgh_local_port != 0,
      m->h.msgh_voucher_port != 0);
  }
  P("\n");
}

static void timers(void) {
  rcv_t m;
  mach_port_t t = (mach_port_t)T0(91);
  mach_port_type_t type = 0;
  kern_return_t kr = mach_port_type(task, t, &type);
  P("timer_create nonzero=%d type_rc=%x type=%x\n", t != 0, RC(kr), type);
  P("timer_destroy_bad=%x zero=%x\n", RC(T1(92, 0x7777)), RC(T1(92, 0)));
  P("timer_arm_bad=%x cancel_bad=%x leeway_bad=%x\n", RC(T2(93, 0x7777, 1)),
    RC(T2(94, 0x7777, 0)), RC(T4(95, 0x7777, 0, 1, 0)));
  uint64_t t0 = mach_absolute_time();
  P("arm=%x\n", RC(T2(93, t, t0 + ms_to_ticks(30))));
  kr = receive(t, &m, 2000, 1);
  uint64_t dt = ticks_to_ms(mach_absolute_time() - t0);
  show_msg("arm_receive", kr, &m);
  P("arm_waited_ok=%d\n", dt >= 29 && dt < 1500);
  P("quiet_after_fire=%x\n", RC(receive(t, &m, 30, 1)));

  t0 = mach_absolute_time();
  P("rearm1=%x rearm2=%x\n", RC(T2(93, t, t0 + ms_to_ticks(900))),
    RC(T2(93, t, t0 + ms_to_ticks(20))));
  kr = receive(t, &m, 700, 1);
  show_msg("rearm_receive", kr, &m);
  P("rearm_quiet=%x\n", RC(receive(t, &m, 60, 1)));

  uint64_t when = mach_absolute_time() + ms_to_ticks(5000), prev = 0xdead;
  T2(93, t, when);
  P("cancel_armed=%x prev_near_deadline=%d\n", RC(T2(94, t, &prev)),
    prev >= when && prev - when < ms_to_ticks(100));
  prev = 0xdead;
  P("cancel_idle=%x prev=%llu\n", RC(T2(94, t, &prev)), (unsigned long long)prev);
  P("cancel_null=%x\n", RC(T2(94, t, 0)));
  P("cancelled_quiet=%x\n", RC(receive(t, &m, 40, 1)));

  P("arm_past=%x\n", RC(T2(93, t, 1)));
  kr = receive(t, &m, 1000, 1);
  show_msg("past_receive", kr, &m);

  for (uint64_t flags = 0; flags < 4; flags++) {
    t0 = mach_absolute_time();
    kern_return_t arm = (kern_return_t)T4(95, t, flags, t0 + ms_to_ticks(10), 0);
    kr = receive(t, &m, 300, 1);
    P("leeway_flags=%llu rc=%x receive=%x\n", (unsigned long long)flags, RC(arm), RC(kr));
  }
  P("leeway_hiflag=%x\n", RC(T4(95, t, 0x100, mach_absolute_time() + ms_to_ticks(10), 0)));
  (void)receive(t, &m, 100, 1);

  t0 = mach_absolute_time();
  T2(93, t, t0 + ms_to_ticks(30));
  kr = receive(t, &m, 0, 0);
  dt = ticks_to_ms(mach_absolute_time() - t0);
  show_msg("blocking_receive", kr, &m);
  P("blocking_waited_ok=%d\n", dt >= 29 && dt < 1500);

  T2(93, t, mach_absolute_time() + ms_to_ticks(5));
  int polled = 0;
  uint64_t poll_end = mach_absolute_time() + ms_to_ticks(3000);
  while (!polled && mach_absolute_time() < poll_end) polled = receive(t, &m, 0, 1) == 0;
  P("poll_fires=%d\n", polled);

  mach_port_t plain = 0;
  mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &plain);
  P("plain_arm=%x plain_cancel=%x plain_destroy=%x\n", RC(T2(93, plain, 1)),
    RC(T2(94, plain, 0)), RC(T1(92, plain)));

  T2(93, t, mach_absolute_time() + ms_to_ticks(3000));
  P("destroy=%x\n", RC(T1(92, t)));
  P("type_after=%x\n", RC(mach_port_type(task, t, &type)));
  P("destroy_again=%x arm_after=%x\n", RC(T1(92, t)), RC(T2(93, t, 1)));
}

typedef struct {
  mach_port_t sem;
  atomic_int woke;
  atomic_int result;
} waiter_t;

static void* waiter(void* p) {
  waiter_t* w = p;
  kern_return_t kr = (kern_return_t)T1(36, w->sem);
  atomic_store(&w->result, (int)kr);
  atomic_fetch_add(&w->woke, 1);
  return 0;
}

static void spin_ms(int ms) {
  uint64_t end = mach_absolute_time() + ms_to_ticks(ms);
  while (mach_absolute_time() < end) T1(59, 0);
}

static void semaphores(void) {
  semaphore_t a, b;
  semaphore_create(task, &a, SYNC_POLICY_FIFO, 0);
  semaphore_create(task, &b, SYNC_POLICY_FIFO, 0);
  P("signal_all_bad=%x thread_bad=%x wait_signal_bad=%x timed_signal_bad=%x\n",
    RC(T1(34, 0x7777)), RC(T2(35, 0x7777, 0)), RC(T2(37, 0x7777, b)),
    RC(T4(39, 0x7777, b, 0, 0)));
  P("signal_all_idle=%x\n", RC(T1(34, a)));
  P("idle_wait_after=%x\n", RC(T3(38, a, 0, 0)));

  waiter_t w = {a, 0, -1};
  pthread_t th[2];
  pthread_create(&th[0], 0, waiter, &w);
  pthread_create(&th[1], 0, waiter, &w);
  spin_ms(60);
  P("waiters_blocked=%d\n", atomic_load(&w.woke) == 0);
  P("signal_all=%x\n", RC(T1(34, a)));
  pthread_join(th[0], 0);
  pthread_join(th[1], 0);
  P("signal_all_woke=%d result=%x\n", atomic_load(&w.woke), RC(atomic_load(&w.result)));
  P("count_not_banked=%x\n", RC(T3(38, a, 0, 0)));

  P("thread_null=%x\n", RC(T2(35, a, 0)));
  P("thread_null_banked=%x\n", RC(T3(38, a, 0, 0)));
  P("thread_notwaiting=%x\n", RC(T2(35, a, mach_thread_self())));
  P("thread_notwaiting_banked=%x\n", RC(T3(38, a, 0, 0)));

  semaphore_signal(b);
  P("wait_signal=%x\n", RC(T2(37, b, a)));
  P("wait_signal_signalled=%x b_drained=%x\n", RC(T3(38, a, 0, 0)), RC(T3(38, b, 0, 0)));
  P("wait_signal_badsignal=%x\n", RC(T2(37, b, 0x7777)));
  semaphore_signal(b);
  P("wait_signal_badsignal_b=%x\n", RC(T3(38, b, 0, 0)));
  uint64_t t0 = mach_absolute_time();
  P("timedwait_signal_timeout=%x\n", RC(T4(39, a, b, 0, 25000000)));
  P("timedwait_waited_ok=%d\n", ticks_to_ms(mach_absolute_time() - t0) >= 24);
  P("timedwait_signalled_b=%x\n", RC(T3(38, b, 0, 0)));
  P("timedwait_bad_nsec=%x\n", RC(T4(39, a, b, 0, 2000000000U)));
  semaphore_signal(a);
  P("timedwait_signal_ok=%x\n", RC(T4(39, a, b, 1, 0)));
  semaphore_signal(a);
  semaphore_signal(a);
  w.woke = 0;
  semaphore_destroy(task, b);
  semaphore_destroy(task, a);
}

typedef struct {
  semaphore_t sem;
  mach_port_t thread_port;
  atomic_int ready;
  atomic_int woke;
} directed_t;

static void* directed_waiter(void* p) {
  directed_t* d = p;
  d->thread_port = mach_thread_self();
  atomic_store(&d->ready, 1);
  T1(36, d->sem);
  atomic_store(&d->woke, 1);
  return 0;
}

static void directed(void) {
  directed_t d = {0, 0, 0, 0};
  semaphore_create(task, &d.sem, SYNC_POLICY_FIFO, 0);
  pthread_t th;
  pthread_create(&th, 0, directed_waiter, &d);
  while (!atomic_load(&d.ready)) T1(59, 0);
  spin_ms(60);
  P("directed_wake=%x\n", RC(T2(35, d.sem, d.thread_port)));
  pthread_join(th, 0);
  P("directed_woke=%d banked=%x\n", atomic_load(&d.woke), RC(T3(38, d.sem, 0, 0)));
  semaphore_destroy(task, d.sem);
}

static void misc(void) {
  uint64_t t0 = mach_absolute_time();
  P("wait_until=%x\n", RC(T1(90, t0 + ms_to_ticks(30))));
  P("wait_until_waited_ok=%d\n", ticks_to_ms(mach_absolute_time() - t0) >= 29);
  t0 = mach_absolute_time();
  P("wait_until_past=%x quick=%d\n", RC(T1(90, 1)),
    ticks_to_ms(mach_absolute_time() - t0) < 1000);
  mach_timebase_info_data_t info = {0, 0};
  P("timebase=%x match=%d\n", RC(T1(89, &info)), info.numer == tb.numer && info.denom == tb.denom);
  P("timebase_efault=%x\n", RC(T1(89, 8)));
  P("swtch_pri=%d swtch=%d\n", T1(59, 0) <= 1, T0(60) <= 1);
  for (int opt = 0; opt < 6; opt++) {
    t0 = mach_absolute_time();
    kern_return_t kr = (kern_return_t)T3(61, 0, opt, 1500);
    uint64_t dt = ticks_to_ms(mach_absolute_time() - t0);
    P("thread_switch opt=%d rc=%x slept=%d fast=%d\n", opt, RC(kr), dt >= 1490, dt < 1000);
  }
  mach_timespec_t wt = {9, 9};
  t0 = mach_absolute_time();
  P("clock_sleep_rel=%x\n", RC(T6(62, 0, 1, 0, 25000000, &wt, 0)));
  P("clock_sleep_waited_ok=%d wakeup_written=%d\n", ticks_to_ms(mach_absolute_time() - t0) >= 24,
    wt.tv_sec != 9 && wt.tv_nsec != 9);
  P("clock_sleep_abs_past=%x bad_type=%x bad_name=%x neg=%x big=%x null_out=%x\n",
    RC(T6(62, 0, 0, 0, 0, &wt, 0)), RC(T6(62, 0, 5, 0, 0, &wt, 0)),
    RC(T6(62, 0x7777, 1, 0, 0, &wt, 0)), RC(T6(62, 0, 1, 0, (uint64_t)(int64_t)-5, &wt, 0)),
    RC(T6(62, 0, 1, 0, 2000000000U, &wt, 0)), RC(T6(62, 0, 1, 0, 1, 0, 0)));
  uint64_t id = 0, id2 = 0;
  P("activity=%x nonzero=%d\n", RC(T3(43, task, 1, &id)), id != 0);
  T3(43, task, 5, &id2);
  T3(43, task, 1, &id);
  P("activity_block=%d bad_count=%x\n", (int)(id - id2), RC(T3(43, task, 0, &id)));
  mach_port_t sr1 = (mach_port_t)T0(50), sr2 = (mach_port_t)T0(50);
  P("special_reply_distinct=%d nonzero=%d\n", sr1 != sr2, sr1 != 0 && sr2 != 0);
  P("exclaves=%x iokit=%llx\n", RC(T0(88)), (unsigned long long)T0(100));
  mach_port_t tfp = 0;
  P("task_for_pid_self=%x same_task=%d other=%x\n", RC(T3(45, task, getpid(), &tfp)),
    tfp == task, RC(T3(45, task, 1, &tfp)));
}

static void vm(void) {
  mach_vm_address_t a = 0;
  P("vm_allocate=%x aligned=%d\n", RC(T4(10, task, &a, 0x4000, 1)), (a & 0x3fff) == 0);
  *(volatile int*)a = 5;
  P("vm_allocate_readback=%d\n", *(volatile int*)a);
  P("vm_deallocate=%x again=%x bad_target=%x\n", RC(T3(12, task, a, 0x4000)),
    RC(T3(12, task, a, 0x4000)), RC(T3(12, 0x7777, a, 0x4000)));
  mach_vm_address_t b = 0x1234;
  P("vm_allocate_bad_target=%x zero=%x zero_addr=%llx\n", RC(T4(10, 0x7777, &b, 0x4000, 1)),
    RC(T4(10, task, &b, 0, 1)), (unsigned long long)b);
  a = 0;
  P("vm_map=%x\n", RC(T6(15, task, &a, 0x8000, 0, 1, 3)));
  *(volatile int*)a = 7;
  P("vm_map_readback=%d\n", *(volatile int*)a);
  P("vm_map_dealloc=%x\n", RC(T3(12, task, a, 0x8000)));
  P("vm_map_bad_target=%x\n", RC(T6(15, 0x7777, &a, 0x8000, 0, 1, 3)));
}

static void ports(void) {
  mach_port_t p = 0, q = 0, set = 0;
  mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &p);
  mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &q);
  mach_port_allocate(task, MACH_PORT_RIGHT_PORT_SET, &set);
  P("extract_notin=%x\n", RC(T3(23, task, p, set)));
  P("insert=%x\n", RC(T3(22, task, p, set)));
  P("insert_again=%x\n", RC(T3(22, task, p, set)));
  P("extract=%x\n", RC(T3(23, task, p, set)));
  P("extract_again=%x\n", RC(T3(23, task, p, set)));
  P("move_into=%x\n", RC(T3(20, task, p, set)));
  P("move_out=%x\n", RC(T3(20, task, p, 0)));
  P("extract_after_move_out=%x\n", RC(T3(23, task, p, set)));
  P("move_member_to_nonset=%x bad_target=%x\n", RC(T3(20, task, p, q)),
    RC(T3(20, 0x7777, p, set)));
  uint64_t guard = 0x1234;
  P("guard=%x guard_again=%x unguard=%x\n", RC(T3(41, task, p, guard)),
    RC(T3(41, task, p, guard)), RC(T3(42, task, p, guard)));
  P("guard_set=%x guard_bad_target=%x\n", RC(T3(41, task, set, guard)),
    RC(T3(41, 0x7777, p, guard)));
  mach_port_limits_t lim = {0};
  mach_msg_type_number_t cnt = MACH_PORT_LIMITS_INFO_COUNT;
  P("attr_limits=%x ql=%u cnt=%u\n", RC(T6(40, task, p, MACH_PORT_LIMITS_INFO, &lim, &cnt, 0)),
    lim.mpl_qlimit, cnt);
  P("purgable_plain=%x\n", RC(T4(11, task, 0x1000, 1, &cnt)));
}

int main(int argc, char** argv) {
  task = mach_task_self();
  mach_timebase_info(&tb);
  const char* only = argc > 1 ? argv[1] : "all";
  int all = strcmp(only, "all") == 0;
  if (all || !strcmp(only, "timers")) timers();
  if (all || !strcmp(only, "sem")) {
    semaphores();
    directed();
  }
  if (all || !strcmp(only, "misc")) misc();
  if (all || !strcmp(only, "vm")) vm();
  if (all || !strcmp(only, "ports")) ports();
  P("done\n");
  return 0;
}
