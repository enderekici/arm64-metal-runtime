#include <mach/mach.h>
#include <mach/mach_traps.h>
#include <servers/bootstrap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

static void out(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  write(1, buf, n);
}
#define printf out

#define OPT_SEND 1ull
#define OPT_RCV 2ull
#define OPT_TIMEOUT 0x100ull
#define OPT_VECTOR 0x100000000ull
#define OPT_MQ 0x400000000ull
#define OPT_KOBJ 0x200000000ull
#define BITS_CALL 0x1513u
#define BITS_ONEWAY 0x13u

static uint64_t msg2(void *msg, uint64_t options, uint64_t bits_size, uint64_t remote_local,
                     uint64_t voucher_id, uint64_t desc_rcvname, uint64_t rcv_size,
                     uint64_t timeout) {
  register uint64_t x0 __asm__("x0") = (uint64_t)msg;
  register uint64_t x1 __asm__("x1") = options;
  register uint64_t x2 __asm__("x2") = bits_size;
  register uint64_t x3 __asm__("x3") = remote_local;
  register uint64_t x4 __asm__("x4") = voucher_id;
  register uint64_t x5 __asm__("x5") = desc_rcvname;
  register uint64_t x6 __asm__("x6") = rcv_size;
  register uint64_t x7 __asm__("x7") = timeout;
  register uint64_t x16 __asm__("x16") = (uint64_t)(int64_t)-47;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5),
                   "r"(x6), "r"(x7), "r"(x16) : "memory", "cc");
  return x0 & 0xffffffffu;
}

struct Reply { mach_msg_header_t h; int32_t body[16]; };

static mach_port_t make_reply(void) {
  mach_port_t p = MACH_PORT_NULL;
  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p);
  return p;
}

static uint64_t call_opt(uint64_t kind, mach_port_t dest, mach_port_t reply, int32_t id, struct Reply *out) {
  struct Reply m;
  memset(&m, 0, sizeof(m));
  m.h.msgh_bits = BITS_CALL;
  m.h.msgh_size = sizeof(mach_msg_header_t);
  m.h.msgh_remote_port = dest;
  m.h.msgh_local_port = reply;
  m.h.msgh_id = id;
  uint64_t r = msg2(&m, OPT_SEND | OPT_RCV | kind | OPT_TIMEOUT, (uint64_t)sizeof(mach_msg_header_t) << 32 | BITS_CALL,
                    (uint64_t)reply << 32 | dest, (uint64_t)(uint32_t)id << 32, (uint64_t)reply << 32, sizeof(m), 500);
  *out = m;
  return r;
}

static uint64_t call(mach_port_t dest, mach_port_t reply, int32_t id, struct Reply *out) {
  return call_opt(OPT_KOBJ, dest, reply, id, out);
}

static void show(const char *label, uint64_t r, struct Reply *m) {
  if (r == 0)
    printf("%s: r=0x%llx reply id=%d size=%u retcode=%d\n", label, (unsigned long long)r,
           m->h.msgh_id, m->h.msgh_size, m->body[2]);
  else
    printf("%s: r=0x%llx\n", label, (unsigned long long)r);
}

static void sweep(const char *label, mach_port_t dest, mach_port_t reply, int32_t lo, int32_t hi) {
  int32_t start = -1, last = -1;
  struct Reply m;
  for (int32_t id = lo; id <= hi; id++) {
    uint64_t r = call(dest, reply, id, &m);
    int bad = r == 0 && m.body[2] == -303;
    if (!bad) {
      if (start < 0) start = id;
      last = id;
    } else if (start >= 0) {
      printf("%s: non-bad-id %d..%d\n", label, start, last);
      start = -1;
    }
  }
  if (start >= 0) printf("%s: non-bad-id %d..%d\n", label, start, last);
}

int main(int argc, char **argv) {
  struct Reply m;
  mach_port_t reply = make_reply();
  int c = argc > 1 ? atoi(argv[1]) : -1;
#define CASE(n) if (c == (n) || c < 0)
  CASE(1) show("task unknown id", call(mach_task_self(), reply, 99999, &m), &m);
  CASE(2) show("host unknown id", call(mach_host_self(), reply, 99999, &m), &m);
  CASE(3) show("thread unknown id", call(mach_thread_self(), reply, 99999, &m), &m);
  CASE(4) show("invalid dest", call(0x12345, reply, 99999, &m), &m);
  CASE(5) show("null dest", call(MACH_PORT_NULL, reply, 99999, &m), &m);
  CASE(6) show("dead dest", call(MACH_PORT_DEAD, reply, 99999, &m), &m);
  CASE(7) show("reply is task port", call(mach_task_self(), mach_task_self(), 99999, &m), &m);
  CASE(8) {
    mach_port_t bs = MACH_PORT_NULL;
    task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bs);
    show("bootstrap unknown id", call_opt(OPT_MQ, bs, reply, 99999, &m), &m);
  }
  CASE(9) {
    memset(&m, 0, sizeof(m));
    uint64_t r = msg2(&m, OPT_RCV | OPT_TIMEOUT, 0, 0, 0, (uint64_t)mach_task_self() << 32, sizeof(m), 50);
    printf("receive on send right: r=0x%llx\n", (unsigned long long)r);
    r = msg2(&m, OPT_RCV | OPT_TIMEOUT, 0, 0, 0, (uint64_t)0x12345 << 32, sizeof(m), 50);
    printf("receive on invalid name: r=0x%llx\n", (unsigned long long)r);
  }
  CASE(10) {
    mach_port_t port = make_reply();
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    struct { mach_msg_header_t h; int32_t b[2]; } s = {0};
    s.h.msgh_bits = BITS_ONEWAY; s.h.msgh_size = sizeof(s); s.h.msgh_remote_port = port; s.h.msgh_id = 4242;
    s.b[0] = 0x11111111; s.b[1] = 0x22222222;
    uint64_t r = msg2(&s, OPT_SEND | OPT_MQ, (uint64_t)sizeof(s) << 32 | BITS_ONEWAY, port, (uint64_t)4242 << 32, 0, 0, 0);
    printf("legacy send to receive right: r=0x%llx\n", (unsigned long long)r);
    struct Reply in; memset(&in, 0, sizeof(in));
    r = mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(in), port, 100, MACH_PORT_NULL);
    printf("  recv: r=0x%x id=%d size=%u b=%08x %08x\n", (unsigned)r, in.h.msgh_id, in.h.msgh_size,
           in.body[0], in.body[1]);
    struct { int32_t b[2]; } body = {{0x33333333, 0x44444444}};
    struct { uint64_t data, rcv; uint32_t ssz, rsz; } vec = {(uint64_t)&body, 0, sizeof(body), 0};
    r = msg2(&vec, OPT_SEND | OPT_MQ | OPT_VECTOR, (uint64_t)sizeof(s) << 32 | BITS_ONEWAY, port, (uint64_t)4243 << 32, 0, 0, 0);
    printf("vector send to receive right: r=0x%llx\n", (unsigned long long)r);
    memset(&in, 0, sizeof(in));
    r = mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(in), port, 100, MACH_PORT_NULL);
    printf("  recv: r=0x%x id=%d size=%u b=%08x %08x\n", (unsigned)r, in.h.msgh_id, in.h.msgh_size,
           in.body[0], in.body[1]);
    s.h.msgh_size = 20;
    r = msg2(&s, OPT_SEND | OPT_MQ, (uint64_t)20 << 32 | BITS_ONEWAY, port, (uint64_t)4244 << 32, 0, 0, 0);
    printf("send too small: r=0x%llx\n", (unsigned long long)r);
  }
  if (c == 99 && argc > 4) {
    mach_port_t dest = argv[2][0] == 't' ? mach_task_self()
                     : argv[2][0] == 'h' ? mach_host_self() : mach_thread_self();
    if (argv[2][0] == 'c') host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &dest);
    sweep(argv[2], dest, reply, atoi(argv[3]), atoi(argv[4]));
  }
  return 0;
}
