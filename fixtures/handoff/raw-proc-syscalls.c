#include <errno.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <unistd.h>

struct R {
  uint64_t v;
  uint64_t c;
};

static struct R sc(int64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5) {
  register uint64_t x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  register uint64_t x4 __asm__("x4") = a4;
  register uint64_t x5 __asm__("x5") = a5;
  register uint64_t x6 __asm__("x6") = 0;
  register uint64_t x7 __asm__("x7") = 0;
  register uint64_t x16 __asm__("x16") = (uint64_t)n;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %8, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5),
                     "+r"(x6), "+r"(x7), "=r"(nzcv)
                   : "r"(x16)
                   : "cc", "memory");
  return (struct R){x0, (nzcv >> 29) & 1U};
}

#define U(x) ((uint64_t)(uintptr_t)(x))
#define S0(n) sc(n, 0, 0, 0, 0, 0, 0)
#define S1(n, a) sc(n, (uint64_t)(a), 0, 0, 0, 0, 0)
#define S2(n, a, b) sc(n, (uint64_t)(a), (uint64_t)(b), 0, 0, 0, 0)
#define S3(n, a, b, c) sc(n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0, 0)
#define S4(n, a, b, c, d) \
  sc(n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), 0, 0)

static void show(const char *name, struct R r) {
  if (r.c)
    printf("%s: err=%d\n", name, (int)r.v);
  else
    printf("%s: ok\n", name);
}
static void showv(const char *name, struct R r, int64_t expected) {
  if (r.c)
    printf("%s: err=%d\n", name, (int)r.v);
  else
    printf("%s: ok match=%d\n", name, (int64_t)r.v == expected);
}
static void flag(const char *name, int value) { printf("%s: %d\n", name, value); }

/* Kernel struct __sigaction */
struct ksigaction {
  void (*handler)(int);
  void (*tramp)(void *, int, int, siginfo_t *, void *);
  uint32_t mask;
  int flags;
};
struct usigaction {
  void (*handler)(int);
  uint32_t mask;
  int flags;
};

static void tramp(void *handler, int style, int sig, siginfo_t *info, void *uctx) {
  if (style == 1)
    ((void (*)(int))handler)(sig);
  else
    ((void (*)(int, siginfo_t *, void *))handler)(sig, info, uctx);
  register uint64_t x0 __asm__("x0") = (uint64_t)uctx;
  register uint64_t x1 __asm__("x1") = (uint64_t)style;
  register uint64_t x16 __asm__("x16") = 184;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory", "cc");
}

static volatile int hits;
static volatile int last_signal;
static volatile uint32_t mask_in_handler;
static volatile int info_signal;
static volatile int info_code;

static void on_signal(int sig) {
  hits++;
  last_signal = sig;
  uint32_t current = 0;
  sc(48, 1, 0, U(&current), 0, 0, 0);
  mask_in_handler = current;
}
static void on_info(int sig, siginfo_t *info, void *uctx) {
  (void)uctx;
  hits++;
  info_signal = sig;
  info_code = info->si_code;
}

static struct R raw_sigaction(int sig, void (*handler)(int), uint32_t mask, int flags,
                              struct usigaction *old) {
  struct ksigaction n = {handler, tramp, mask, flags};
  return sc(46, (uint64_t)sig, U(&n), U(old), 0, 0, 0);
}

#define BIT(sig) (1U << ((sig)-1))

int main(int argc, char **argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "kill-term") == 0) {
      sc(37, (uint64_t)getpid(), SIGTERM, 0, 0, 0, 0);
      puts("survived");
      return 1;
    }
    if (strcmp(argv[1], "kill-kill") == 0) {
      sc(37, (uint64_t)getpid(), SIGKILL, 0, 0, 0, 0);
      puts("survived");
      return 1;
    }
    if (strcmp(argv[1], "abort") == 0) {
      sc(521, 0, 0, 0, 0, 0, 0);
      puts("survived");
      return 1;
    }
    if (strcmp(argv[1], "terminate") == 0) {
      sc(520, (uint64_t)getpid(), 0, 0, 0, 0, 0);
      puts("survived");
      return 1;
    }
    if (strcmp(argv[1], "alarm") == 0) {
      struct itimerval timer = {{0, 0}, {0, 30000}};
      sc(83, ITIMER_REAL, U(&timer), 0, 0, 0, 0);
      sc(111, 0, 0, 0, 0, 0, 0);
      puts("survived");
      return 1;
    }
    return 2;
  }
  /* identity */
  showv("getpid", S0(20), getpid());
  showv("getppid", S0(39), getppid());
  showv("getuid", S0(24), getuid());
  showv("geteuid", S0(25), geteuid());
  showv("getgid", S0(47), getgid());
  showv("getegid", S0(43), getegid());
  showv("getpgrp", S0(81), getpgrp());
  showv("getpgid-self", S1(151, 0), getpgrp());
  showv("getpgid-pid", S1(151, getpid()), getpgrp());
  show("getpgid-bogus", S1(151, 0x7ffffff0));
  showv("getsid-self", S1(310, 0), getsid(0));
  show("getsid-bogus", S1(310, 0x7ffffff0));
  showv("getdtablesize", S0(89), getdtablesize());
  flag("issetugid", (int)S0(327).v == issetugid());
  {
    mode_t previous = umask(022);
    showv("getumask", S0(215), 022);
    umask(previous);
  }
  {
    gid_t host[32];
    int count = getgroups(32, host);
    showv("getgroups-count", S2(79, 0, 0), count);
    gid_t guest[32];
    memset(guest, 0xee, sizeof guest);
    struct R r = S2(79, 32, guest);
    flag("getgroups-fill", !r.c && (int)r.v == count &&
                               memcmp(guest, host, sizeof(gid_t) * (size_t)count) == 0);
    if (count > 1) show("getgroups-short", S2(79, 1, guest));
    show("getgroups-efault", S2(79, 32, 0));
  }
  show("setgroups-empty", S2(80, 0, 0));
  show("setgroups-many", S2(80, 17, 0));
  show("initgroups", S3(243, 0, 0, 0));
  show("setuid-self", S1(23, getuid()));
  show("setuid-root", S1(23, 0));
  show("seteuid-self", S1(183, geteuid()));
  show("seteuid-root", S1(183, 0));
  show("setgid-self", S1(181, getgid()));
  show("setgid-root", S1(181, 0));
  show("setegid-self", S1(182, getegid()));
  show("setegid-root", S1(182, 0));
  show("setreuid-keep", S2(126, -1, -1));
  show("setreuid-self", S2(126, getuid(), geteuid()));
  show("setreuid-root", S2(126, 0, 0));
  show("setregid-keep", S2(127, -1, -1));
  show("setregid-self", S2(127, getgid(), getegid()));
  show("setregid-root", S2(127, 0, 0));
  {
    char name[64];
    memset(name, 'x', sizeof name);
    struct R r = S2(49, name, sizeof name);
    flag("getlogin", !r.c);
    show("getlogin-efault", S2(49, 0, 8));
    show("setlogin", S1(50, name));
  }
  {
    uint32_t auid = 0;
    show("getauid", S1(353, &auid));
    show("getauid-efault", S1(353, 0));
    show("setauid", S1(354, &auid));
    show("setauid-efault", S1(354, 0));
    uint64_t record[16] = {0};
    show("getaudit_addr", S2(357, record, 48));
    show("getaudit_addr-small", S2(357, record, 8));
    show("setaudit_addr", S2(358, record, 48));
    show("setaudit_addr-len", S2(358, record, 8));
    show("setaudit_addr-efault", S2(358, 0, 48));
    show("audit", S2(350, record, 8));
    show("auditctl", S1(359, "/nonexistent"));
    for (int cmd = 0; cmd < 40; ++cmd) {
      struct R r = S3(351, cmd, record, 8);
      printf("auditon[%d]: %s%d\n", cmd, r.c ? "err=" : "ok=", r.c ? (int)r.v : 0);
    }
    show("audit_session_join-bogus", S1(429, 0x1234));
    show("audit_session_port", S2(432, 1, record));
  }
  show("settid", S2(285, 0, 0));
  show("settid_with_pid", S2(311, 1, 0));
  show("identitysvc", S2(293, 0, 0));
  show("setsgroups", S2(287, 0, 0));
  show("getsgroups", S2(288, 0, 0));
  show("setwgroups", S2(289, 0, 0));
  show("getwgroups", S2(290, 0, 0));
  show("setprivexec-0", S1(152, 0));
  show("settimeofday", S2(122, 0, 0));
  show("adjtime", S2(140, 0, 0));
  {
    struct timeval tv = {1, 1};
    show("adjtime-set", S2(140, &tv, 0));
  }

  /* limits and usage */
  {
    struct rlimit host, guest;
    getrlimit(RLIMIT_NOFILE, &host);
    memset(&guest, 0, sizeof guest);
    struct R r = S2(194, RLIMIT_NOFILE, &guest);
    flag("getrlimit-nofile", !r.c && guest.rlim_cur == host.rlim_cur &&
                                 guest.rlim_max == host.rlim_max);
    show("getrlimit-bad", S2(194, 999, &guest));
    show("getrlimit-efault", S2(194, RLIMIT_NOFILE, 0));
    show("setrlimit-same", S2(195, RLIMIT_NOFILE, &host));
    struct rlimit lowered = host;
    lowered.rlim_cur = host.rlim_cur > 256 ? 256 : host.rlim_cur;
    show("setrlimit-lower", S2(195, RLIMIT_NOFILE, &lowered));
    struct rlimit after;
    getrlimit(RLIMIT_NOFILE, &after);
    flag("setrlimit-visible", after.rlim_cur == lowered.rlim_cur);
    show("setrlimit-restore", S2(195, RLIMIT_NOFILE, &host));
    struct rlimit inverted = {10, 5};
    show("setrlimit-inverted", S2(195, RLIMIT_NOFILE, &inverted));
    struct rlimit raised = host;
    raised.rlim_max = host.rlim_max == RLIM_INFINITY ? RLIM_INFINITY : host.rlim_max + 1;
    if (host.rlim_max != RLIM_INFINITY) show("setrlimit-raise-hard", S2(195, RLIMIT_CPU, &raised));
    show("setrlimit-efault", S2(195, RLIMIT_NOFILE, 0));
  }
  {
    struct rusage ru;
    memset(&ru, 0xab, sizeof ru);
    struct R r = S2(117, RUSAGE_SELF, &ru);
    flag("getrusage-self", !r.c && ru.ru_utime.tv_usec < 1000000 && ru.ru_maxrss > 0);
    show("getrusage-children", S2(117, RUSAGE_CHILDREN, &ru));
    show("getrusage-bad", S2(117, 5, &ru));
    show("getrusage-efault", S2(117, RUSAGE_SELF, 0));
  }
  {
    errno = 0;
    int base = getpriority(PRIO_PROCESS, 0);
    struct R r = S2(100, PRIO_PROCESS, 0);
    printf("getpriority-raw: %d base=%d\n", (int)r.v, base);
    flag("getpriority-self", !r.c && (int)r.v == base + 20);
    show("getpriority-bogus", S2(100, PRIO_PROCESS, 0x7ffffff0));
    show("getpriority-badwhich", S2(100, 77, 0));
    show("setpriority-same", S3(96, PRIO_PROCESS, 0, base));
    show("setpriority-bogus", S3(96, PRIO_PROCESS, 0x7ffffff0, 0));
    show("setpriority-badwhich", S3(96, 77, 0, 0));
    show("setpriority-lower-nice", S3(96, PRIO_PROCESS, 0, base > 0 ? base - 1 : -5));
  }
  {
    struct timeval tv = {0, 0}, host;
    struct timezone tz = {99, 99};
    uint64_t mach = 0;
    gettimeofday(&host, 0);
    struct R r = S3(116, &tv, &tz, &mach);
    flag("gettimeofday", !r.c && llabs((long long)tv.tv_sec - (long long)host.tv_sec) <= 1 &&
                             mach != 0);
    flag("gettimeofday-tz", tz.tz_minuteswest == 0 && tz.tz_dsttime == 0);
    show("gettimeofday-tv-only", S3(116, &tv, 0, 0));
    show("gettimeofday-efault", S3(116, 1, 0, 0));
  }
  {
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    show("ntp_adjtime-read", S1(527, &tx));
    tx.modes = 1;
    show("ntp_adjtime-write", S1(527, &tx));
    show("ntp_adjtime-efault", S1(527, 0));
    struct ntptimeval nv;
    memset(&nv, 0, sizeof nv);
    show("ntp_gettime", S1(528, &nv));
    flag("ntp_gettime-sec", nv.time.tv_sec > 1000000000);
    show("ntp_gettime-efault", S1(528, 0));
  }
  {
    uint8_t a[32], b[32];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    show("getentropy", S2(500, a, 32));
    show("getentropy-again", S2(500, b, 32));
    flag("getentropy-differs", memcmp(a, b, 32) != 0);
    show("getentropy-zero", S2(500, a, 0));
    show("getentropy-257", S2(500, a, 257));
    show("getentropy-efault", S2(500, 0, 8));
    uint8_t uuid[16] = {0};
    struct timespec ts = {5, 0};
    struct R r = S2(142, uuid, &ts);
    flag("gethostuuid", !r.c);
    show("gethostuuid-efault", S2(142, 0, &ts));
  }
  {
    struct itimerval zero = {{0, 0}, {0, 0}}, old;
    memset(&old, 0xff, sizeof old);
    show("getitimer-real", S2(86, ITIMER_REAL, &old));
    flag("getitimer-zero", old.it_value.tv_sec == 0 && old.it_interval.tv_sec == 0);
    show("getitimer-bad", S2(86, 9, &old));
    show("setitimer-disarm", S3(83, ITIMER_REAL, &zero, &old));
    show("setitimer-bad", S3(83, 9, &zero, &old));
    show("setitimer-efault", S3(83, ITIMER_REAL, 0, 0));
  }
  {
    uint32_t type = 1;
    uint64_t counts[8] = {0};
    show("thread_selfcounts", S3(186, type, counts, sizeof counts));
    show("thread_selfcounts-bad", S3(186, 99, counts, sizeof counts));
    flag("thread_selfusage", S0(482).v != 0);
    int level = -1;
    show("memorystatus_get_level", S1(453, &level));
    flag("memorystatus_level-range", level >= 0 && level <= 100);
    show("memorystatus_available_memory", S0(534));
    show("vm_pressure_monitor", S3(296, 0, 0, 0));
    int iop[3] = {0, 0, 0};
    iop[0] = 0;
    iop[1] = 0;
    show("iopolicysys-get", S2(322, 1, iop));
    show("iopolicysys-bad", S2(322, 99, iop));
  }

  /* privileged or entitlement-gated calls an ordinary process receives errors for */
  show("process_policy", S3(323, 0, 0, 0));
  show("proc_rlimit_control-other", S3(446, 1, 1, 0));
  show("proc_rlimit_control-bogus", S3(446, 0x7ffffff0, 1, 0));
  show("proc_uuid_policy", S3(452, 1, 0, 0));
  show("system_override", S3(454, 0, 0, 0));
  show("memorystatus_control", S4(440, 1, 0, 0, 0));
  show("kas_info", S3(439, 0, 0, 0));
  show("usrctl", S1(445, 0));
  show("coalition-create", S3(458, 1, 0, 0));
  show("coalition_ledger", S4(532, 0, 0, 0, 0));
  show("coalition_policy_set", S4(556, 0, 0, 0, 0));
  show("coalition_policy_get", S2(557, 0, 0));
  show("proc_trace_log", S3(477, 0, 0, 0));
  show("log_data", S3(533, 0, 0, 0));
  show("record_system_event", S3(552, 0, 0, 0));
  show("tracker_action", S3(546, 0, 0, 0));
  show("stack_snapshot_with_config", S3(491, 0, 0, 0));
  show("microstackshot", S3(492, 0, 0, 0));
  show("grab_pgo_data", S3(493, 0, 0, 0));
  show("register_uexc_handler", S3(519, 0, 0, 0));
  show("panic_with_data", S3(185, 0, 0, 0));
  show("debug_syscall_reject", S1(547, 0));
  show("debug_syscall_reject_config", S2(548, 0, 0));
  show("sfi_ctl-1", S4(456, 1, 0, 0, 0));
  show("sfi_ctl-0", S4(456, 0, 0, 0, 0));
  show("sfi_pidctl", S4(457, 1, 0, 0, 0));
  show("crossarch_trap", S0(38));
  show("objc_bp_assist_cfg_np", S2(535, 0, 0));
  show("persona-0", S3(494, 0, 0, 0));
  for (int op = 1; op <= 9; ++op) {
    char label[32];
    snprintf(label, sizeof label, "persona-op%d", op);
    show(label, sc(494, (uint64_t)op, 0, 0, 0, 0, 0));
  }
  show("necp_open", S1(501, 0));
  show("necp_client_action-badfd", S4(502, 999, 1, 0, 0));
  show("necp_session_open", S1(522, 0));
  show("necp_session_action-badfd", S4(523, 999, 1, 0, 0));
  show("necp_match_policy", S3(460, 0, 0, 0));
  show("netagent_trigger", S2(490, 0, 0));
  show("net_qos_guideline", S2(525, 0, 0));
  show("nexus_open", S2(503, 0, 0));
  show("channel_open", S2(510, 0, 0));
  show("work_interval_ctl", S4(499, 1, 0, 0, 0));
  show("ledger-0", S4(373, 0, 0, 0, 0));
  show("ledger-1", S4(373, 1, 0, 0, 0));
  show("ledger-9", S4(373, 9, 0, 0, 0));
  show("telemetry-0", S4(451, 0, 0, 0, 0));
  show("telemetry-1", S4(451, 1, 0, 0, 0));
  show("proc_info_extended_id", S4(545, 0, 0, 0, 0));
  {
    uint32_t port = 0;
    uint64_t task = mach_task_self();
    show("task_read_for_pid-self", S3(539, task, getpid(), &port));
    flag("task_read_for_pid-port", port != 0);
    port = 0;
    show("task_inspect_for_pid-self", S3(538, task, getpid(), &port));
    flag("task_inspect_for_pid-port", port != 0);
    show("task_read_for_pid-other", S3(539, task, 1, &port));
    show("task_read_for_pid-bogus", S3(539, task, 0x7ffffff0, &port));
    show("task_read_for_pid-badtask", S3(539, 0x7777, getpid(), &port));
    show("task_read_for_pid-efault", S3(539, task, getpid(), 0));
  }
  show("pid_suspend-bogus", S1(433, 0x7ffffff0));
  show("pid_resume-bogus", S1(434, 0x7ffffff0));
  show("pid_shutdown_sockets-bogus", S2(436, 0x7ffffff0, 0));
  show("acct", S1(51, 0));
  show("swapon", S1(85, 0));
  show("nfssvc", S2(155, 0, 0));
  show("bsdthread_ctl", S4(478, 0, 0, 0, 0));
  show("kdebug_trace_string", S3(178, 0, 0, 0));
  show("kdebug_trace", S4(180, 0, 0, 0, 0));
  show("os_fault_with_payload", sc(529, 0, 0, 0, 0, 0, 0));

  /* lifecycle */
  show("wait4-nochild", sc(7, (uint64_t)-1, 0, 0, 0, 0, 0));
  show("wait4_nocancel-nochild", sc(400, (uint64_t)-1, 0, 0, 0, 0, 0));
  show("waitid-nochild", sc(173, 0, 0, 0, 0, 0, 0));
  show("ptrace-deny-attach", S4(26, 31, 0, 0, 0));
  show("ptrace-attach-other", S4(26, 10, 1, 0, 0));

  /* signals */
  {
    struct usigaction old;
    memset(&old, 0xee, sizeof old);
    struct R r = raw_sigaction(SIGUSR1, on_signal, BIT(SIGUSR2), 0, &old);
    show("sigaction-install", r);
    flag("sigaction-old-default", old.handler == SIG_DFL && old.mask == 0 && old.flags == 0);
    memset(&old, 0xee, sizeof old);
    show("sigaction-query", sc(46, SIGUSR1, 0, U(&old), 0, 0, 0));
    flag("sigaction-query-values", old.handler == on_signal && old.mask == BIT(SIGUSR2) &&
                                       old.flags == 0);
    show("sigaction-bad-signal", raw_sigaction(0, on_signal, 0, 0, 0));
    show("sigaction-signal-65", raw_sigaction(65, on_signal, 0, 0, 0));
    show("sigaction-kill", raw_sigaction(SIGKILL, on_signal, 0, 0, 0));
    show("sigaction-stop", raw_sigaction(SIGSTOP, on_signal, 0, 0, 0));
    show("sigaction-query-kill", sc(46, SIGKILL, 0, U(&old), 0, 0, 0));
    show("sigaction-efault-old", sc(46, SIGUSR1, 0, 1, 0, 0, 0));
  }
  {
    hits = 0;
    last_signal = 0;
    show("kill-self-usr1", S2(37, getpid(), SIGUSR1));
    flag("kill-self-handler-ran", hits == 1 && last_signal == SIGUSR1);
    flag("kill-handler-mask", (mask_in_handler & BIT(SIGUSR1)) != 0 &&
                                  (mask_in_handler & BIT(SIGUSR2)) != 0);
    uint32_t after = 0xffffffffU;
    show("sigprocmask-query", S3(48, 1, 0, &after));
    flag("mask-restored", after == 0);
    show("kill-signal-0", S2(37, getpid(), 0));
    show("kill-bad-signal", S2(37, getpid(), 65));
    show("kill-negative-signal", S2(37, getpid(), -1));
    show("kill-bogus-pid", S2(37, 0x7ffffff0, 0));
    show("kill-bogus-pid-signal", S2(37, 0x7ffffff0, SIGUSR1));
    show("kill-pgrp-zero-probe", S2(37, 0, 0));
  }
  {
    uint32_t block = BIT(SIGUSR1), old = 0xffffffffU, pending = 0;
    hits = 0;
    show("sigprocmask-block", S3(48, 1, &block, &old));
    flag("sigprocmask-old-empty", old == 0);
    show("kill-blocked", S2(37, getpid(), SIGUSR1));
    flag("blocked-no-handler", hits == 0);
    show("sigpending", S1(52, &pending));
    flag("sigpending-has-usr1", pending == BIT(SIGUSR1));
    uint32_t none = 0;
    show("sigprocmask-unblock", S3(48, 2, &block, &none));
    flag("unblock-ran-handler", hits == 1);
    pending = 0xffffffffU;
    show("sigpending-after", S1(52, &pending));
    flag("sigpending-empty", pending == 0);
    show("sigprocmask-bad-how", S3(48, 9, &block, &old));
    uint32_t unblockable = BIT(SIGKILL) | BIT(SIGSTOP);
    uint32_t seen = 0;
    show("sigprocmask-block-kill", S3(48, 1, &unblockable, &old));
    show("sigprocmask-read-kill", S3(48, 1, 0, &seen));
    flag("kill-stop-unblockable", seen == 0);
    show("sigprocmask-efault-set", S3(48, 1, 1, &old));
    show("pthread_sigmask-block", S3(329, 1, &block, &old));
    show("pthread_sigmask-read", S3(329, 1, 0, &seen));
    flag("pthread_sigmask-value", seen == block);
    show("pthread_sigmask-bad-how", S3(329, 9, &block, &old));
    show("pthread_sigmask-restore", S3(329, 3, &old, 0));
  }
  {
    siginfo_t dummy;
    (void)dummy;
    struct ksigaction n = {(void (*)(int))(void *)on_info, tramp, 0, SA_SIGINFO};
    show("sigaction-siginfo", sc(46, SIGUSR2, U(&n), 0, 0, 0, 0));
    hits = 0;
    show("kill-siginfo", S2(37, getpid(), SIGUSR2));
    flag("siginfo-ran", hits == 1 && info_signal == SIGUSR2 && info_code == 0x10001);
    printf("siginfo-code: %d\n", info_code);
  }
  {
    struct usigaction old;
    show("sigaction-ignore", raw_sigaction(SIGUSR1, SIG_IGN, 0, 0, 0));
    hits = 0;
    show("kill-ignored", S2(37, getpid(), SIGUSR1));
    flag("ignored-no-handler", hits == 0);
    show("sigaction-ignore-query", sc(46, SIGUSR1, 0, U(&old), 0, 0, 0));
    flag("ignore-handler-is-1", old.handler == SIG_IGN);
    show("sigaction-reset-default", raw_sigaction(SIGUSR1, SIG_DFL, 0, 0, 0));
    show("sigaction-reinstall", raw_sigaction(SIGUSR1, on_signal, 0, SA_RESETHAND, 0));
    hits = 0;
    show("kill-resethand", S2(37, getpid(), SIGUSR1));
    flag("resethand-ran", hits == 1);
    show("sigaction-resethand-query", sc(46, SIGUSR1, 0, U(&old), 0, 0, 0));
    flag("resethand-reset", old.handler == SIG_DFL);
  }
  {
    /* pthread_kill needs a mach thread port; take our own through -27 */
    uint64_t port = S0((int64_t)-27).v;
    raw_sigaction(SIGUSR1, on_signal, 0, 0, 0);
    hits = 0;
    show("pthread_kill-self", S2(328, port, SIGUSR1));
    flag("pthread_kill-ran", hits == 1);
    show("pthread_kill-zero", S2(328, port, 0));
    show("pthread_kill-bad-port", S2(328, 0x12345, SIGUSR1));
    show("pthread_kill-bad-signal", S2(328, port, 65));
    show("markcancel-bad-port", S1(332, 0x12345));
    show("canceled-check-clear", S1(333, 0));
    show("canceled-disable", S1(333, 1));
    show("canceled-enable", S1(333, 2));
    show("canceled-bad-action", S1(333, 9));
    show("markcancel-self", S1(332, port));
    show("canceled-disable-2", S1(333, 1));
    show("canceled-check-disabled", S1(333, 0));
    show("canceled-enable-2", S1(333, 2));
    show("canceled-check-set", S1(333, 0));
    show("canceled-check-consumed", S1(333, 0));
  }
  {
    /* sigaltstack */
    static char stack[65536];
    struct {
      void *sp;
      size_t size;
      int flags;
    } ss = {stack, sizeof stack, 0}, old;
    memset(&old, 0xee, sizeof old);
    show("sigaltstack-query", S2(53, 0, &old));
    flag("sigaltstack-disabled", old.flags == 4);
    show("sigaltstack-install", S2(53, &ss, &old));
    memset(&old, 0xee, sizeof old);
    show("sigaltstack-readback", S2(53, 0, &old));
    flag("sigaltstack-readback-values", old.sp == stack && old.size == sizeof stack &&
                                            old.flags == 0);
    ss.size = 1024;
    show("sigaltstack-small", S2(53, &ss, 0));
    ss.size = sizeof stack;
    ss.flags = 8;
    show("sigaltstack-badflags", S2(53, &ss, 0));
    ss.flags = 4;
    show("sigaltstack-disable", S2(53, &ss, &old));
    memset(&old, 0xee, sizeof old);
    show("sigaltstack-after-disable", S2(53, 0, &old));
    flag("sigaltstack-after-disable-flag", old.flags == 4);
    show("sigaltstack-efault", S2(53, 1, 0));
  }
  {
    /* sigsuspend: signal is pending and blocked, suspend with an empty mask */
    uint32_t block = BIT(SIGUSR1), old = 0, empty = 0, seen = 0xffffffffU;
    S3(48, 1, &block, &old);
    hits = 0;
    S2(37, getpid(), SIGUSR1);
    struct R r = S1(111, empty);
    show("sigsuspend", r);
    flag("sigsuspend-handler-ran", hits == 1);
    S3(48, 1, 0, &seen);
    flag("sigsuspend-mask-restored", seen == block);
    S3(48, 2, &block, 0);
    /* __sigwait consumes a blocked pending signal without running the handler */
    S3(48, 1, &block, 0);
    hits = 0;
    S2(37, getpid(), SIGUSR1);
    int got = 0;
    uint32_t set = BIT(SIGUSR1);
    show("sigwait", S2(330, &set, &got));
    flag("sigwait-got", got == SIGUSR1 && hits == 0);
    uint32_t pending = 0xffffffffU;
    S1(52, &pending);
    flag("sigwait-consumed", pending == 0);
    show("sigwait-efault", S2(330, 0, &got));
    uint32_t empty_set = 0;
    show("sigwait-empty-set-efault-sig", S2(330, &empty_set, 0));
    S3(48, 2, &block, 0);
  }
  {
    /* a host SIGALRM from setitimer must wake sigsuspend and sigwait */
    uint32_t block = BIT(SIGALRM), old = 0, empty = 0, seen = 0xffffffffU;
    struct itimerval timer = {{0, 0}, {0, 40000}}, previous;
    show("sigaction-alrm", raw_sigaction(SIGALRM, on_signal, 0, 0, 0));
    S3(48, 1, &block, &old);
    hits = 0;
    last_signal = 0;
    show("setitimer-arm", S3(83, ITIMER_REAL, &timer, &previous));
    struct R r = S1(111, empty);
    show("sigsuspend-timer", r);
    flag("sigsuspend-timer-handler", hits == 1 && last_signal == SIGALRM);
    S3(48, 1, 0, &seen);
    flag("sigsuspend-timer-mask-restored", seen == block);
    hits = 0;
    S3(83, ITIMER_REAL, &timer, &previous);
    int got = 0;
    uint32_t set = BIT(SIGALRM);
    show("sigwait-timer", S2(330, &set, &got));
    flag("sigwait-timer-got", got == SIGALRM && hits == 0);
    struct itimerval off = {{0, 0}, {0, 0}};
    S3(83, ITIMER_REAL, &off, &previous);
    show("setitimer-readback", S2(86, ITIMER_REAL, &previous));
    flag("setitimer-disarmed", previous.it_value.tv_sec == 0 && previous.it_value.tv_usec == 0);
    show("setitimer-virtual-arm", S3(83, ITIMER_VIRTUAL, &timer, 0));
    S3(48, 2, &block, 0);
  }
  {
    /* abort_with_payload delivers SIGABRT; handled here, then process continues */
    raw_sigaction(SIGABRT, on_signal, 0, 0, 0);
    hits = 0;
    struct R r = sc(521, 0, 0, 0, 0, 0, 0);
    show("abort_with_payload", r);
    flag("abort-handler-ran", hits == 1 && last_signal == SIGABRT);
  }
  fflush(stdout);
  /* terminate_with_payload against another pid is refused */
  show("terminate_with_payload-other", sc(520, 1, 0, 0, 0, 0, 0));
  show("terminate_with_payload-bogus", sc(520, 0x7ffffff0, 0, 0, 0, 0, 0));
  show("terminate_with_payload-pid0", sc(520, 0, 0, 0, 0, 0, 0));
  fflush(stdout);
  /* exit with a status */
  sc(1, 37, 0, 0, 0, 0, 0);
  puts("not reached");
  return 1;
}
