#include <errno.h>
#include <stdarg.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);
extern int csops_audittoken(pid_t pid, unsigned int ops, void* useraddr, size_t usersize,
                            audit_token_t* token);

static long raw169(long pid, long op, void* buf, long size, int* err) {
  register long x0 __asm__("x0") = pid;
  register long x1 __asm__("x1") = op;
  register long x2 __asm__("x2") = (long)buf;
  register long x3 __asm__("x3") = size;
  register long x16 __asm__("x16") = 169;
  __asm__ volatile("svc #0x80\n\tcset %w1, cs"
                   : "+r"(x0), "=r"(*err) : "r"(x1), "r"(x2), "r"(x3), "r"(x16)
                   : "memory", "cc");
  return x0;
}

static long raw170(long pid, long op, void* buf, long size, audit_token_t* tok, int* err) {
  register long x0 __asm__("x0") = pid;
  register long x1 __asm__("x1") = op;
  register long x2 __asm__("x2") = (long)buf;
  register long x3 __asm__("x3") = size;
  register long x4 __asm__("x4") = (long)tok;
  register long x16 __asm__("x16") = 170;
  __asm__ volatile("svc #0x80\n\tcset %w1, cs"
                   : "+r"(x0), "=r"(*err) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x16)
                   : "memory", "cc");
  return x0;
}

static void say(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n > 0) write(1, line, (size_t)(n < 255 ? n : 255));
}

static void one(int mode, long pid, unsigned op, size_t cap, int use_buf, audit_token_t* tok) {
  static uint8_t buf[4200];
  const size_t span = cap >= 4096 ? 1024 : (cap < 64 ? 64 : cap) + 16;
  memset(buf, 0xAA, span);
  const char* names[] = {"libc", "raw", "libc-at", "raw-at"};
  const char* who = pid == 0 ? "0" : pid == -1 ? "self" : "other";
  if (pid == -1) pid = getpid();
  void* b = use_buf ? buf : NULL;
  long ret; int carry = 0; int en = 0;
  errno = 0;
  if (mode == 0) { ret = csops((pid_t)pid, op, b, cap); en = errno; }
  else if (mode == 1) { ret = raw169(pid, op, b, cap, &carry); }
  else if (mode == 2) { ret = csops_audittoken((pid_t)pid, op, b, cap, tok); en = errno; }
  else { ret = raw170(pid, op, b, cap, tok, &carry); }
  size_t last = 0;
  for (size_t i = 0; i < span; ++i) if (buf[i] != 0xAA) last = i + 1;
  uint32_t sum = 0;
  for (size_t i = 0; i < last; ++i) sum = sum * 31 + buf[i];
  char line[256];
  int n;
  if (mode == 0 || mode == 2) {
    n = snprintf(line, sizeof line, "%s pid=%s op=%u cap=%zu buf=%d ret=%ld errno=%d touched=%zu sum=%08x hex=",
                 names[mode], who, op, cap, use_buf, ret, en, last, sum);
  } else {
    n = snprintf(line, sizeof line, "%s pid=%s op=%u cap=%zu buf=%d ret=%ld carry=%d touched=%zu sum=%08x hex=",
                 names[mode], who, op, cap, use_buf, ret, carry, last, sum);
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < last && i < 48 && n < 240; ++i) {
    line[n++] = digits[buf[i] >> 4];
    line[n++] = digits[buf[i] & 15];
  }
  line[n++] = '\n';
  write(1, line, (size_t)n);
}

static void status_line(const char* label, long pid) {
  uint32_t st = 0xAAAAAAAA;
  int r = csops((pid_t)(pid == -1 ? getpid() : pid), 0, &st, sizeof st);
  say("  %s status r=%d errno=%d value=%08x\n", label, r, r ? errno : 0, st);
}

static void sequence(long pid, const char* name) {
  say("sequence %s\n", name);
  pid_t p = (pid_t)(pid == -1 ? getpid() : pid);
  status_line("initial", pid);
  const unsigned simple[] = {2, 3, 8, 12, 13, 15};
  for (size_t i = 0; i < sizeof simple / sizeof *simple; ++i) {
    errno = 0;
    int r = csops(p, simple[i], NULL, 0);
    say("  op=%u nobuf r=%d errno=%d\n", simple[i], r, r ? errno : 0);
    status_line("after", pid);
  }
  const uint32_t values[] = {0, 0x4, 0x800, 0x1000, 0x2000, 0x10000, 0x100000, 0x200000,
                             0x400000, 0x800000, 0x80000000u, 0xAAAAAAAAu};
  for (size_t i = 0; i < sizeof values / sizeof *values; ++i) {
    uint32_t v = values[i];
    errno = 0;
    int r = csops(p, 9, &v, sizeof v);
    say("  op=9 value=%08x r=%d errno=%d\n", values[i], r, r ? errno : 0);
    status_line("after", pid);
  }
  errno = 0;
  uint32_t v = 0;
  int r = csops(p, 9, &v, 2);
  say("  op=9 cap=2 r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 9, (void*)8, 4);
  say("  op=9 badptr r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 0, (void*)8, 4);
  say("  op=0 badptr r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 10, (void*)8, 4096);
  say("  op=10 badptr r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 11, (void*)8, 4096);
  say("  op=11 badptr r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 6, (void*)8, 4096);
  say("  op=6 badptr r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 5, (void*)8, 20);
  say("  op=5 badptr r=%d errno=%d\n", r, r ? errno : 0);
  r = csops(p, 17, (void*)8, 4);
  say("  op=17 badptr r=%d errno=%d\n", r, r ? errno : 0);
}

int main(int argc, char** argv) {
  audit_token_t tok;
  mach_msg_type_number_t cnt = TASK_AUDIT_TOKEN_COUNT;
  task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&tok, &cnt);
  if (argc == 3 && strcmp(argv[1], "kill") == 0) {
    int mode = atoi(argv[2]);
    say("marking invalid mode=%d\n", mode);
      one(mode, 0, 1, 0, 1, &tok);
    say("survived\n");
    return 3;
  }
  mach_msg_type_number_t tcnt = TASK_AUDIT_TOKEN_COUNT;
  task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&tok, &tcnt);
  const size_t caps[] = {0, 4, 8, 20, 4096};
  for (unsigned op = 0; op <= 24; ++op) {
    for (int mode = 0; mode < 4; ++mode) {
      for (size_t ci = 0; ci < sizeof caps / sizeof *caps; ++ci) {
        if (op == 1) continue;
        one(mode, 0, op, caps[ci], 1, &tok);
      }
    }
    if (op != 1) {
      one(0, 0, op, 0, 0, &tok);
      one(0, -1, op, 4096, 1, &tok);
      one(1, -1, op, 4096, 1, &tok);
      one(3, -1, op, 4, 1, &tok);
    }
    one(0, 999999, op, 4096, 1, &tok);
    one(2, 999999, op, 4096, 1, &tok);
    one(1, 999999, op, 4096, 1, &tok);
    one(3, 999999, op, 4096, 1, &tok);
  }
  sequence(-1, "self");
  say("done\n");
  return 0;
}
