// Freestanding (no libc imports) probe of voucher Mach traps and the kernel-object RPCs a JVM
// startup issues. Raw svc only, so it runs natively and as a guest without the shared cache.
#include <mach/mach.h>
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

static u64 sys(long n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) {
  register u64 x0 __asm__("x0") = a;
  register u64 x1 __asm__("x1") = b;
  register u64 x2 __asm__("x2") = c;
  register u64 x3 __asm__("x3") = d;
  register u64 x4 __asm__("x4") = e;
  register u64 x5 __asm__("x5") = f;
  register u64 x6 __asm__("x6") = g;
  register u64 x7 __asm__("x7") = h;
  register long x16 __asm__("x16") = n;
  __asm__ volatile("svc #0x80"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                   : "r"(x16)
                   : "cc", "memory");
  return x0;
}
#define TRAP(n, a, b, c, d) sys(-(n), (u64)(a), (u64)(b), (u64)(c), (u64)(d), 0, 0, 0, 0)

// BSD syscall returning x0 and the carry flag (set on error).
static u64 bsd(long n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 *carry) {
  register u64 x0 __asm__("x0") = a;
  register u64 x1 __asm__("x1") = b;
  register u64 x2 __asm__("x2") = c;
  register u64 x3 __asm__("x3") = d;
  register u64 x4 __asm__("x4") = e;
  register u64 x5 __asm__("x5") = f;
  register long x16 __asm__("x16") = n;
  u64 flags;
  __asm__ volatile("svc #0x80\n\tmrs %0, nzcv"
                   : "=r"(flags), "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5)
                   : "r"(x16)
                   : "cc", "memory");
  *carry = (flags >> 29) & 1;
  return x0;
}

static char outbuf[4096];
static unsigned outlen;
static void flush(void) {
  if (outlen) sys(4, 1, (u64)outbuf, outlen, 0, 0, 0, 0, 0);
  outlen = 0;
}
static void pc(char c) {
  if (outlen == sizeof(outbuf)) flush();
  outbuf[outlen++] = c;
}
static void ps(const char *s) {
  while (*s) pc(*s++);
}
static void pd(long v) {
  char t[24];
  int n = 0;
  if (v < 0) { pc('-'); v = -v; }
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) pc(t[--n]);
}
static void ph(u64 v) {
  ps("0x");
  int started = 0;
  for (int s = 60; s >= 0; s -= 4) {
    int d = (int)((v >> s) & 15);
    if (d || started || s == 0) { pc((char)(d < 10 ? '0' + d : 'a' + d - 10)); started = 1; }
  }
}
static void pk(const char *key, long v) { ps(" "); ps(key); ps("="); pd(v); }
static void pkh(const char *key, u64 v) { ps(" "); ps(key); ps("="); ph(v); }
static void nl(void) { pc('\n'); }

static void zero(void *p, unsigned n) { for (unsigned i = 0; i < n; i++) ((volatile char *)p)[i] = 0; }
static void copy(void *d, const void *s, unsigned n) { for (unsigned i = 0; i < n; i++) ((char *)d)[i] = ((const char *)s)[i]; }

#define OPT_SEND 1ull
#define OPT_RCV 2ull
#define OPT_TIMEOUT 0x100ull
#define OPT_KOBJ 0x200000000ull
#define BITS_CALL 0x1513u
#define KERN_NS 0x10000003u

static u32 task, host, thread, reply;
static u32 rep[512];  // last reply
static u64 last_r;

// Sends a kernel-object RPC, body is NDR + arguments in words.
static u64 rpcx(u32 dest, i32 id, const u32 *body, unsigned nwords, unsigned ndesc) {
  u32 m[128];
  reply = (u32)TRAP(26, 0, 0, 0, 0);
  u32 bits = ndesc ? (BITS_CALL | 0x80000000u) : BITS_CALL;
  zero(m, sizeof(m));
  m[0] = bits;
  m[1] = 24 + 4 * nwords;
  m[2] = dest;
  m[3] = reply;
  m[5] = (u32)id;
  for (unsigned i = 0; i < nwords; i++) m[6 + i] = body[i];
  zero(rep, sizeof(rep));
  u64 r = sys(-47, (u64)m, OPT_SEND | OPT_RCV | OPT_KOBJ | OPT_TIMEOUT, (u64)m[1] << 32 | bits,
              (u64)reply << 32 | dest, (u64)(u32)id << 32, (u64)reply << 32 | ndesc, sizeof(m), 500);
  (void)r;
  // The receive buffer is the send buffer; copy it out.
  copy(rep, m, sizeof(m));
  last_r = r & 0xffffffffu;
  TRAP(19, task, reply, 1, (u64)(i32)-1);
  return last_r;
}
static u64 rpc(u32 dest, i32 id, const u32 *body, unsigned nwords) {
  return rpcx(dest, id, body, nwords, 0);
}
// Plain request with the standard NDR record prefix.
static void voucher_traps(void) {
  // host_create_mach_voucher_trap(host, recipes, size, *voucher): -70
  u32 name = 0xdead, name2 = 0xdead;
  struct { u32 key, cmd, prev, size; char c[4]; } rec = {7, 211, 0, 3, {'a', 'b', 'c', 0}};
  u64 r = TRAP(70, host, &rec, 19, &name);
  ps("trap70 userdata"); pkh("r", r & 0xffffffffu); pk("nonnull", name != 0 && name != 0xdead); nl();
  r = TRAP(70, host, &rec, 19, &name2);
  ps("trap70 userdata again"); pkh("r", r & 0xffffffffu); pk("same", name == name2); nl();
  u32 empty = 0xdead;
  r = TRAP(70, host, 0, 0, &empty);
  ps("trap70 empty"); pkh("r", r & 0xffffffffu); pk("null", empty == 0); nl();
  u32 bad = 0xdead;
  r = TRAP(70, task, &rec, 19, &bad);
  ps("trap70 task port as host"); pkh("r", r & 0xffffffffu); pk("untouched", bad == 0xdead); nl();
  r = TRAP(70, 0x1234, &rec, 19, &bad);
  ps("trap70 bad host name"); pkh("r", r & 0xffffffffu); nl();
  r = TRAP(70, host, &rec, 19, 0);
  ps("trap70 null out"); pkh("r", r & 0xffffffffu); nl();
  r = TRAP(70, host, &rec, 5, &bad);
  ps("trap70 malformed"); pkh("r", r & 0xffffffffu); nl();
  // mach_voucher_extract_attr_recipe_trap(voucher, key, recipe, *size): -72
  unsigned char out[128];
  u32 sz = sizeof(out);
  r = TRAP(72, name, 7, out, &sz);
  ps("trap72 userdata"); pkh("r", r & 0xffffffffu); pk("size", sz);
  for (u32 i = 0; i < sz && i < 19; i++) { ps(i ? "" : " "); pc("0123456789abcdef"[out[i] >> 4]); pc("0123456789abcdef"[out[i] & 15]); }
  nl();
  sz = 4;
  r = TRAP(72, name, 7, out, &sz);
  ps("trap72 small buffer"); pkh("r", r & 0xffffffffu); pk("size", sz); nl();
  sz = sizeof(out);
  r = TRAP(72, name, 4, out, &sz);
  ps("trap72 absent key"); pkh("r", r & 0xffffffffu); pk("size", sz); nl();
  sz = sizeof(out);
  r = TRAP(72, task, 7, out, &sz);
  ps("trap72 task port"); pkh("r", r & 0xffffffffu); nl();
  r = TRAP(72, 0x1234, 7, out, &sz);
  ps("trap72 bad name"); pkh("r", r & 0xffffffffu); nl();
  r = TRAP(72, name, 7, out, 0);
  ps("trap72 null size"); pkh("r", r & 0xffffffffu); nl();
  // Port semantics of the voucher name.
  u32 type = 0xdead;
  r = TRAP(76, task, name, &type, 0);
  ps("voucher mach_port_type"); pkh("r", r & 0xffffffffu); pkh("type", type); nl();
}

// Prints a reply with port names reduced to a non-null flag.
static void dump(const char *label) {
  ps(label);
  pkh("r", last_r);
  if (last_r != 0) { nl(); return; }
  pkh("bits", rep[0]);
  pk("size", rep[1]);
  pk("id", (i32)rep[5]);
  unsigned i = 6;
  if (rep[0] >> 31) {
    unsigned n = rep[6];
    pk("desc", n);
    i = 7;
    unsigned same = 0;
    for (unsigned d = 0; d < n; d++, i += 3) {
      if ((rep[i + 2] >> 24) == 0) {
        if (d > 0 && (rep[i] != 0) == (rep[i - 3] != 0) && rep[i + 2] == rep[i - 1]) {
          same++;
          if (d + 1 == n) { ps(" x"); pd(same + 1); }
          continue;
        }
        ps(" p"); pd(rep[i] != 0); ps(":"); ph(rep[i + 2]);
      } else {
        ps(" ool:"); ph(rep[i + 2]); ps(","); pd(rep[i + 3]);
        // The OOL descriptor is 16 bytes.
        i++;
      }
    }
  }
  ps(" w:");
  for (; i < (rep[1] / 4) && i < 140; i++) { ps(" "); ph(rep[i]); }
  nl();
}
static void vouchers(void) {
  u32 name = 0xdead;
  struct { u32 key, cmd, prev, size; char c[4]; } rec = {7, 211, 0, 3, {'a', 'b', 'c', 0}};
  TRAP(70, host, &rec, 19, &name);
  u32 a[16];
  // thread_get_mach_voucher(which)
  a[0] = 0; a[1] = 1; a[2] = 0;
  rpc(thread, 3625, a, 3); dump("thread_get_mach_voucher none");
  // thread_set_mach_voucher: body = msgh_body count, port descriptor
  a[0] = 1; a[1] = name; a[2] = 0; a[3] = (0u << 24) | (19u << 16);
  rpcx(thread, 3626, a, 4, 1); dump("thread_set_mach_voucher v");
  a[0] = 0; a[1] = 1; a[2] = 0;
  rpc(thread, 3625, a, 3); dump("thread_get_mach_voucher after set");
  pk("same_name", rep[7] == name); nl();
  a[0] = 1; a[1] = 0; a[2] = 0; a[3] = (0u << 24) | (19u << 16);
  rpcx(thread, 3626, a, 4, 1); dump("thread_set_mach_voucher null");
  a[0] = 1; a[1] = host; a[2] = 0; a[3] = (0u << 24) | (19u << 16);
  rpcx(thread, 3626, a, 4, 1); dump("thread_set_mach_voucher host port");
  a[0] = 1; a[1] = 0x1234; a[2] = 0; a[3] = (0u << 24) | (19u << 16);
  rpcx(thread, 3626, a, 4, 1); dump("thread_set_mach_voucher bad name");
  a[0] = 2; a[1] = name; a[2] = 0; a[3] = (0u << 24) | (19u << 16); a[4] = 0; a[5] = 0; a[6] = (0u << 24) | (19u << 16);
  rpcx(thread, 3627, a, 7, 2); dump("thread_swap_mach_voucher v,null");
  a[0] = 2; a[1] = 0; a[2] = 0; a[3] = (0u << 24) | (19u << 16); a[4] = 0; a[5] = 0; a[6] = (0u << 24) | (19u << 16);
  rpcx(thread, 3627, a, 7, 2); dump("thread_swap_mach_voucher null,null");
  a[0] = 0; a[1] = 1; a[2] = 7; a[3] = 128;
  rpc(name, 5400, a, 4); dump("voucher extract_attr_content");
  a[2] = 7; a[3] = 128;
  rpc(name, 5401, a, 4); dump("voucher extract_attr_recipe");
  a[2] = 4; a[3] = 128;
  rpc(name, 5401, a, 4); dump("voucher extract_attr_recipe absent");
  a[2] = 128;
  rpc(name, 5402, a, 3); dump("voucher extract_all_attr_recipes");
  a[2] = 7; a[3] = 1; a[4] = 4; a[5] = 0x01020304; a[6] = 128;
  rpc(name, 5403, a, 7); dump("voucher attr_command");
  a[2] = 7; a[3] = 1;
  rpc(name, 5404, a, 4); dump("voucher 5404");
  rpc(name, 5405, a, 4); dump("voucher 5405");
  a[2] = 7; a[3] = 128;
  rpc(host, 5401, a, 4); dump("voucher id sent to host");
}
// Prints header fields and the first n payload words after the return code slot.
static void shape(const char *label, unsigned n) {
  ps(label);
  pkh("r", last_r);
  if (last_r != 0) { nl(); return; }
  pkh("bits", rep[0]);
  pk("size", rep[1]);
  pk("id", (i32)rep[5]);
  ps(" w:");
  for (unsigned k = 0; k < n && 8 + k < rep[1] / 4; k++) { ps(" "); ph(rep[8 + k]); }
}
static u64 u64at(unsigned index) { return (u64)rep[index] | (u64)rep[index + 1] << 32; }

static void thread_rpcs(void) {
  u32 a[16];
  a[0] = 0; a[1] = 1; a[2] = ARM_THREAD_STATE64; a[3] = ARM_THREAD_STATE64_COUNT;
  rpc(thread, 3603, a, 4); shape("thread_get_state 64", 2); nl();
  a[2] = ARM_THREAD_STATE64 + 100;
  rpc(thread, 3603, a, 4); shape("thread_get_state bad flavor", 1); nl();
  a[2] = ARM_THREAD_STATE64; a[3] = 4;
  rpc(thread, 3603, a, 4); shape("thread_get_state small count", 1); nl();
  a[2] = THREAD_BASIC_INFO; a[3] = THREAD_BASIC_INFO_COUNT;
  rpc(thread, 3612, a, 4); shape("thread_info basic", 2);
  pk("policy", rep[15]); pk("run_state", rep[16]); pk("flags", rep[17]);
  pk("suspend", rep[18]); pk("sleep", rep[19]); nl();
  a[2] = THREAD_IDENTIFIER_INFO; a[3] = THREAD_IDENTIFIER_INFO_COUNT;
  rpc(thread, 3612, a, 4); shape("thread_info identifier", 2);
  pk("id_nonzero", u64at(10) != 0); pk("handle_nonzero", u64at(12) != 0);
  pkh("qaddr_minus_handle", u64at(14) - u64at(12)); nl();
  a[2] = THREAD_EXTENDED_INFO; a[3] = THREAD_EXTENDED_INFO_COUNT;
  rpc(thread, 3612, a, 4); shape("thread_info extended", 2);
  pk("policy", rep[15]); pk("run_state", rep[16]); pk("flags", rep[17]); pk("sleep", rep[18]);
  pk("curpri", rep[19]); pk("pri", rep[20]); pk("maxpri", rep[21]); pk("name0", rep[22]); nl();
  a[2] = 99; a[3] = 10;
  rpc(thread, 3612, a, 4); shape("thread_info bad flavor", 1); nl();
  a[2] = THREAD_BASIC_INFO; a[3] = 2;
  rpc(thread, 3612, a, 4); shape("thread_info small count", 1); nl();
  a[2] = 6; a[3] = 10;
  rpc(thread, 3612, a, 4); shape("thread_info flavor 6", 1); nl();
  static const struct { u32 flavor, count; const char *name; } gets[] = {
      {THREAD_EXTENDED_POLICY, THREAD_EXTENDED_POLICY_COUNT, "extended"},
      {THREAD_TIME_CONSTRAINT_POLICY, THREAD_TIME_CONSTRAINT_POLICY_COUNT, "time_constraint"},
      {THREAD_PRECEDENCE_POLICY, THREAD_PRECEDENCE_POLICY_COUNT, "precedence"},
      {THREAD_AFFINITY_POLICY, THREAD_AFFINITY_POLICY_COUNT, "affinity"},
      {THREAD_LATENCY_QOS_POLICY, THREAD_LATENCY_QOS_POLICY_COUNT, "latency_qos"},
      {THREAD_THROUGHPUT_QOS_POLICY, THREAD_THROUGHPUT_QOS_POLICY_COUNT, "throughput_qos"},
      {0, 1, "flavor 0"}, {5, 1, "flavor 5"}, {6, 1, "flavor 6"}, {12, 1, "flavor 12"},
  };
  for (unsigned g = 0; g < sizeof(gets) / sizeof(gets[0]); g++) {
    a[2] = gets[g].flavor; a[3] = gets[g].count; a[4] = 0;
    rpc(thread, 3618, a, 5);
    ps("thread_policy_get "); ps(gets[g].name);
    shape("", 2 + gets[g].count + 1); nl();
  }
  a[2] = 6; a[3] = 16; a[4] = 0;
  rpc(thread, 3618, a, 5); shape("thread_policy_get state big count", 1); nl();
  a[2] = THREAD_PRECEDENCE_POLICY; a[3] = 1; a[4] = 5;
  rpc(thread, 3617, a, 5); shape("thread_policy_set precedence 5", 1); nl();
  a[2] = THREAD_PRECEDENCE_POLICY; a[3] = THREAD_PRECEDENCE_POLICY_COUNT; a[4] = 0;
  rpc(thread, 3618, a, 5); shape("thread_policy_get precedence after set", 3); nl();
  a[2] = THREAD_PRECEDENCE_POLICY; a[3] = 0; a[4] = 0;
  rpc(thread, 3617, a, 4); shape("thread_policy_set precedence count 0", 1); nl();
  a[2] = THREAD_EXTENDED_POLICY; a[3] = 1; a[4] = 1;
  rpc(thread, 3617, a, 5); shape("thread_policy_set extended", 1); nl();
  a[2] = THREAD_LATENCY_QOS_POLICY; a[3] = 1; a[4] = 0;
  rpc(thread, 3617, a, 5); shape("thread_policy_set latency_qos", 1); nl();
  a[2] = THREAD_AFFINITY_POLICY; a[3] = 1; a[4] = 0;
  rpc(thread, 3617, a, 5); shape("thread_policy_set affinity", 1); nl();
  a[2] = THREAD_TIME_CONSTRAINT_POLICY; a[3] = 4; a[4] = 0; a[5] = 0; a[6] = 0; a[7] = 0;
  rpc(thread, 3617, a, 8); shape("thread_policy_set time_constraint zeros", 1); nl();
  a[2] = THREAD_TIME_CONSTRAINT_POLICY; a[3] = 4; a[4] = 0; a[5] = 60000; a[6] = 120000; a[7] = 1;
  rpc(thread, 3617, a, 8); shape("thread_policy_set time_constraint valid", 1); nl();
  a[2] = THREAD_TIME_CONSTRAINT_POLICY; a[3] = THREAD_TIME_CONSTRAINT_POLICY_COUNT; a[4] = 0;
  rpc(thread, 3618, a, 5); shape("thread_policy_get time_constraint after set", 6); nl();
  a[2] = 99; a[3] = 1; a[4] = 0;
  rpc(thread, 3617, a, 5); shape("thread_policy_set bad flavor", 1); nl();
  a[2] = THREAD_PRECEDENCE_POLICY; a[3] = 2; a[4] = 0;
  rpc(thread, 3617, a, 5); shape("thread_policy_set size mismatch", 1); nl();
  a[0] = 0; a[1] = 1;
  for (u32 which = 0; which <= 5; which++) {
    a[2] = which;
    rpc(thread, 3610, a, 3); dump("thread_get_special_port");
  }
  a[2] = EXC_MASK_ALL;
  rpc(thread, 3614, a, 3); dump("thread_get_exception_ports all");
  a[2] = 0;
  rpc(thread, 3614, a, 3); dump("thread_get_exception_ports 0");
  a[2] = 0xffffffffu;
  rpc(thread, 3614, a, 3); dump("thread_get_exception_ports bad mask");
}

static void task_rpcs(void) {
  u32 a[16];
  a[0] = 0; a[1] = 1; a[2] = EXC_MASK_ALL;
  rpc(task, 3414, a, 3); dump("task_get_exception_ports all");
  a[2] = EXC_MASK_BAD_ACCESS;
  rpc(task, 3414, a, 3); dump("task_get_exception_ports bad_access");
  a[2] = EXC_MASK_BREAKPOINT | EXC_MASK_CRASH;
  rpc(task, 3414, a, 3); dump("task_get_exception_ports breakpoint|crash");
  a[2] = 0;
  rpc(task, 3414, a, 3); dump("task_get_exception_ports 0");
  a[2] = 0xffffffffu;
  rpc(task, 3414, a, 3); dump("task_get_exception_ports bad mask");
  static const u32 flavors[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12};
  a[2] = 4; a[3] = 16; a[4] = 0;
  rpc(task, 3421, a, 5); shape("task_policy_get state big count", 1); nl();
  for (unsigned f = 0; f < sizeof(flavors) / sizeof(flavors[0]); f++) {
    a[2] = flavors[f]; a[3] = 4; a[4] = 0;
    rpc(task, 3421, a, 5);
    ps("task_policy_get flavor="); pd(flavors[f]);
    shape("", 2 + 2 + 1); nl();
  }
  for (unsigned f = 0; f < sizeof(flavors) / sizeof(flavors[0]); f++) {
    for (u32 c = 0; c <= 3; c += 1) {
      a[2] = flavors[f]; a[3] = c; a[4] = 0; a[5] = 0; a[6] = 0;
      rpc(task, 3420, a, 4 + c);
      ps("task_policy_set flavor="); pd(flavors[f]); pk("count", c);
      shape("", 1); nl();
    }
  }
  a[2] = 0x1234;
  rpc(task, 3205, a, 3); shape("mach_port_destroy bad name", 1); nl();
  {
    u32 port = 0;
    TRAP(16, task, 1, &port, 0);  // mach_port_allocate(RECEIVE)
    a[2] = port;
    rpc(task, 3205, a, 3); shape("mach_port_destroy receive", 1); nl();
    rpc(task, 3205, a, 3); shape("mach_port_destroy again", 1); nl();
  }
}

static void host_rpcs(void) {
  u32 a[16];
  a[0] = 0; a[1] = 1;
  rpc(host, 213, a, 0); dump("processor_set_default");
  a[2] = HOST_VM_INFO; a[3] = HOST_VM_INFO_COUNT;
  rpc(host, 216, a, 4); shape("host_statistics vm", 2); nl();
  a[2] = HOST_CPU_LOAD_INFO; a[3] = HOST_CPU_LOAD_INFO_COUNT;
  rpc(host, 216, a, 4); shape("host_statistics cpu_load", 2); nl();
  a[2] = 99; a[3] = 4;
  rpc(host, 216, a, 4); shape("host_statistics bad flavor", 1); nl();
  a[2] = HOST_VM_INFO64; a[3] = HOST_VM_INFO64_COUNT;
  rpc(host, 219, a, 4); shape("host_statistics64 vm64", 2); nl();
  a[2] = 99; a[3] = 4;
  rpc(host, 219, a, 4); shape("host_statistics64 bad flavor", 1); nl();
  {
    struct { u32 key, cmd, prev, size; char c[4]; } rec = {7, 211, 0, 3, {'a', 'b', 'c', 0}};
    u32 w[16];
    w[0] = 0; w[1] = 1; w[2] = 19; copy(&w[3], &rec, 20);
    rpc(host, 222, w, 8); dump("host_create_mach_voucher");
    w[2] = 5;
    rpc(host, 222, w, 8); dump("host_create_mach_voucher malformed");
    w[2] = 0;
    rpc(host, 222, w, 3); dump("host_create_mach_voucher empty");
  }
}

static void mach_port_names_rpc(void) {
  u32 a[4] = {0, 1, 0, 0};
  u32 port = 0, send_port = 0;
  TRAP(16, task, 1, &port, 0);
  TRAP(16, task, 1, &send_port, 0);
  u32 insert[5] = {0, 0, 0, 0, 0};
  TRAP(21, task, send_port, send_port, 20);  // insert_right(MAKE_SEND)
  rpc(task, 3200, a, 0);
  ps("mach_port_names"); pkh("r", last_r); pkh("bits", rep[0]); pk("id", (i32)rep[5]);
  pk("desc", rep[6]);
  const u32 *names = (const u32 *)(u64)u64at(7);
  const u32 *types = (const u32 *)(u64)u64at(11);
  u32 count = rep[17];
  pk("sizes_match", rep[10] == 4 * count && rep[14] == 4 * count && rep[18] == count);
  pk("counts_ok", count >= 3);
  u32 want[3] = {task, port, send_port};
  static const char *const labels[3] = {" task_type=", " receive_type=", " send_receive_type="};
  for (unsigned w = 0; w < 3; w++) {
    for (unsigned i = 0; i < count; i++) {
      if (names[i] == want[w]) { ps(labels[w]); ph(types[i]); }
    }
  }
  (void)insert;
  nl();
}

static void procinfo(void) {
  u64 carry;
  u64 pid = bsd(20, 0, 0, 0, 0, 0, 0, &carry);
  char name[8] = {'v', 't', 'p', 0};
  for (u32 flavor = 0; flavor <= 7; flavor++) {
    u64 r = bsd(336, 5, pid, flavor, 0, (u64)name, 4, &carry);
    ps("proc_info setcontrol"); pk("flavor", flavor); pk("r", (long)r); pk("carry", (long)carry); nl();
  }
  u64 r = bsd(336, 5, pid, 2, 0, 0, 0, &carry);
  ps("proc_info threadname null buffer"); pk("r", (long)r); pk("carry", (long)carry); nl();
  r = bsd(336, 5, pid + 1, 2, 0, (u64)name, 4, &carry);
  ps("proc_info threadname other pid"); pk("r", (long)r); pk("carry", (long)carry); nl();
  r = bsd(336, 5, pid, 2, 0, (u64)name, 100, &carry);
  ps("proc_info threadname long"); pk("r", (long)r); pk("carry", (long)carry); nl();
  r = bsd(336, 5, pid, 2, 0, (u64)name, 0, &carry);
  ps("proc_info threadname empty"); pk("r", (long)r); pk("carry", (long)carry); nl();
  {
    static const u32 sizes[] = {1, 62, 63, 64, 65};
    char big[80];
    for (unsigned i = 0; i < sizeof(big); i++) big[i] = 'n';
    big[79] = 0;
    for (unsigned i = 0; i < 5; i++) {
      r = bsd(336, 5, pid, 2, 0, (u64)big, sizes[i], &carry);
      ps("proc_info threadname"); pk("size", sizes[i]); pk("r", (long)r); pk("carry", (long)carry); nl();
    }
    r = bsd(336, 5, pid, 2, 0, 0, 4, &carry);
    ps("proc_info threadname null buffer size 4"); pk("r", (long)r); pk("carry", (long)carry); nl();
    r = bsd(336, 5, pid, 2, 0, 0x10, 4, &carry);
    ps("proc_info threadname bad buffer"); pk("r", (long)r); pk("carry", (long)carry); nl();
    for (u32 flavor = 1; flavor <= 4; flavor++) {
      r = bsd(336, 5, pid + 1, flavor, 0, (u64)name, 4, &carry);
      ps("proc_info setcontrol other pid"); pk("flavor", flavor); pk("r", (long)r); pk("carry", (long)carry); nl();
      r = bsd(336, 5, 0, flavor, 0, (u64)name, 4, &carry);
      ps("proc_info setcontrol pid 0"); pk("flavor", flavor); pk("r", (long)r); pk("carry", (long)carry); nl();
    }
  }
  r = bsd(336, 6, pid, 1, 0, (u64)name, 4, &carry);
  ps("proc_info call 6"); pk("r", (long)r); pk("carry", (long)carry); nl();
}

// Mach traps with no kernel implementation raise SIGSYS; verified through the exit status of a
// child process only natively, so here only the in-range numbers that return are checked.
int main(int argc, char **argv) {
  task = (u32)TRAP(28, 0, 0, 0, 0);
  host = (u32)TRAP(29, 0, 0, 0, 0);
  thread = (u32)TRAP(27, 0, 0, 0, 0);
  reply = (u32)TRAP(26, 0, 0, 0, 0);
  const char *section = argc > 1 ? argv[1] : "traps";
  if (section[0] == 't' && section[1] == 'r') { voucher_traps(); vouchers(); }
  else if (section[0] == 't' && section[1] == 'h') thread_rpcs();
  else if (section[0] == 't') task_rpcs();
  else if (section[0] == 'h') host_rpcs();
  else if (section[0] == 'n') mach_port_names_rpc();
  else if (section[0] == 'p') procinfo();
  else if (section[0] == 's') {  // sigsys <trap>: the trap must raise SIGSYS, killing the process
    long n = 0;
    for (const char *c = argv[2]; *c; c++) n = n * 10 + (*c - '0');
    flush();
    TRAP(n, 0, 0, 0, 0);
    ps("returned\n");
  }
  flush();
  return 0;
}
