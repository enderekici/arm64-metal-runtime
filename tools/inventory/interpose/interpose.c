// DYLD_INSERT_LIBRARIES tracer. Records, into a MAP_SHARED file (survives _exit/crash):
//  - sampled (first 256 per function) argument records of every libsystem_kernel function (stubs.S)
//  - text events for sysctl / sysctlbyname / bootstrap / xpc / notify / dlopen / mach_msg ids
// Cannot see: raw `svc #0x80` issued outside libsystem_kernel wrappers, calls made inside libsystem_kernel
// to its own exports (e.g. MIG stubs calling mach_msg), anything dyld performs before this dylib's constructor.
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RING_OFF 65536UL
#define RING_ENTRIES (1UL << 19)
#define STR_OFF (RING_OFF + RING_ENTRIES * 64UL)
#define STR_SIZE (4UL << 20)
#define FILE_SIZE (STR_OFF + STR_SIZE)

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define LR() ((unsigned long)ptrauth_strip(__builtin_return_address(0), ptrauth_key_return_address))
#else
#define LR() ((unsigned long)__builtin_return_address(0))
#endif
// Stubs write into `early` (BSS) until the constructor migrates the data into the shared file, so the
// libSystem-initializer window is counted too (calls the dyld cache binds through interposable stubs only).
static uint8_t early[FILE_SIZE] __attribute__((aligned(16384)));
volatile uint8_t *g_state = early;  // referenced by stubs.S
static int g_migrated;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
  uint8_t *s = (uint8_t *)g_state;
  if (!s) return;
  char buf[512];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  __builtin_va_end(ap);
  if (n <= 0) return;
  if (n >= (int)sizeof buf) n = sizeof buf - 1;
  uint64_t off = __atomic_fetch_add((uint64_t *)(s + 8), (uint64_t)n, __ATOMIC_RELAXED);
  if (off + n > STR_SIZE) return;
  memcpy(s + STR_OFF + off, buf, n);
}

static void dump_maps(void) {
  uint8_t *s = (uint8_t *)g_state;
  if (!s) return;
  for (uint32_t i = 0; i < _dyld_image_count(); i++)
    emit("image\t%s\t0x%lx\t0x%lx\n", _dyld_get_image_name(i), (unsigned long)_dyld_get_image_header(i),
         (unsigned long)_dyld_get_image_vmaddr_slide(i));
}

__attribute__((constructor)) static void trace_init(void) {
  const char *base = getenv("TRACE_OUT");
  if (!base) return;
  char path[1024];
  snprintf(path, sizeof path, "%s.%d", base, (int)getpid());
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  if (ftruncate(fd, FILE_SIZE) != 0) return;
  void *m = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return;
  char *args = getenv("TRACE_TAG");
  uint8_t *dst = (uint8_t *)m, *src = early;
  uint64_t nring = *(uint64_t *)src, slen = *(uint64_t *)(src + 8);
  if (nring > RING_ENTRIES) nring = RING_ENTRIES;
  memcpy(dst, src, 4096 + 4 * 4096);  // header + counters (<= 4096 functions)
  memcpy(dst + RING_OFF, src + RING_OFF, nring * 64);
  memcpy(dst + STR_OFF, src + STR_OFF, slen);
  g_state = m;
  g_migrated = 1;
  emit("self\ttask=0x%x\thost=0x%x\n", (unsigned)mach_task_self(), (unsigned)mach_host_self());
  emit("start\t%d\t%s\n", (int)getpid(), args ? args : "");
  atexit(dump_maps);
}

#define W(name, ...) __attribute__((used)) static uint64_t my_##name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
#define INTERPOSE(name) \
  __attribute__((used)) static struct { const void *r; const void *o; } ip_##name \
      __attribute__((section("__DATA,__interpose"))) = {(const void *)my_##name, (const void *)name}

extern uint64_t sysctlbyname(), sysctl(), sysctlnametomib(), bootstrap_look_up(), bootstrap_look_up2(),
    bootstrap_look_up3(), bootstrap_check_in(), bootstrap_check_in2(), bootstrap_check_in3(), bootstrap_register(),
    bootstrap_register2(), xpc_connection_create_mach_service(), notify_register_check(),
    notify_register_dispatch(), notify_register_mach_port(), notify_register_file_descriptor(), notify_post(),
    mach_msg2_internal();
// the original entry points are called through the interpose-resolved symbol (dyld rebinds only other images)
typedef uint64_t (*fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define ORIG(name) ((fn8)name)

W(sysctlbyname) { emit("sysctlbyname\t%s\tlr=0x%lx\n", a0 ? (const char *)a0 : "(null)", LR()); return ORIG(sysctlbyname)(a0, a1, a2, a3, a4, a5, a6, a7); }
INTERPOSE(sysctlbyname);
W(sysctlnametomib) { emit("sysctlnametomib\t%s\tlr=0x%lx\n", a0 ? (const char *)a0 : "(null)", LR()); return ORIG(sysctlnametomib)(a0, a1, a2, a3, a4, a5, a6, a7); }
INTERPOSE(sysctlnametomib);
W(sysctl) {
  char b[160] = "";
  int *mib = (int *)a0;
  unsigned n = (unsigned)a1;
  for (unsigned i = 0; mib && i < n && i < 8; i++) {
    char t[16];
    snprintf(t, sizeof t, "%s%d", i ? "." : "", mib[i]);
    strlcat(b, t, sizeof b);
  }
  emit("sysctl\t%s\tnew=%d\tlr=0x%lx\n", b, a4 != 0, LR());
  return ORIG(sysctl)(a0, a1, a2, a3, a4, a5, a6, a7);
}
INTERPOSE(sysctl);
#define STR1(fn, idx) W(fn) { emit(#fn "\t%s\tlr=0x%lx\n", a##idx ? (const char *)a##idx : "(null)", LR()); return ORIG(fn)(a0, a1, a2, a3, a4, a5, a6, a7); } INTERPOSE(fn)
#define LOOKUP(fn) W(fn) { uint64_t r = ORIG(fn)(a0, a1, a2, a3, a4, a5, a6, a7); \
  emit(#fn "\t%s\tport=0x%x\tret=%d\tlr=0x%lx\n", a1 ? (const char *)a1 : "(null)", a2 ? *(unsigned *)a2 : 0, (int)r, LR()); return r; } INTERPOSE(fn)
LOOKUP(bootstrap_look_up);
LOOKUP(bootstrap_look_up2);
LOOKUP(bootstrap_look_up3);
STR1(bootstrap_check_in, 1);
STR1(bootstrap_check_in2, 1);
STR1(bootstrap_check_in3, 1);
STR1(bootstrap_register, 1);
STR1(bootstrap_register2, 1);
STR1(xpc_connection_create_mach_service, 0);
STR1(notify_register_check, 0);
STR1(notify_register_dispatch, 0);
STR1(notify_register_mach_port, 0);
STR1(notify_register_file_descriptor, 0);
STR1(notify_post, 0);
STR1(dlopen, 0);

W(mach_msg_overwrite) {
  unsigned long lr = LR();
  mach_msg_header_t *h = (mach_msg_header_t *)a0;
  unsigned opt = (unsigned)a1;
  int sendid = (opt & MACH_SEND_MSG) && h ? h->msgh_id : -1;
  unsigned remote = (opt & MACH_SEND_MSG) && h ? h->msgh_remote_port : 0;
  uint64_t r = ORIG(mach_msg_overwrite)(a0, a1, a2, a3, a4, a5, a6, a7);
  int rcvid = (opt & MACH_RCV_MSG) && h && (uint32_t)r == 0 ? ((mach_msg_header_t *)(a6 ? a6 : a0))->msgh_id : -1;
  emit("mach_msg\tsend=%d\trcv=%d\topt=0x%x\tret=0x%x\tremote=0x%x\tlr=0x%lx\n", sendid, rcvid, opt, (unsigned)r, remote, lr);
  return r;
}
INTERPOSE(mach_msg_overwrite);
W(mach_msg) {
  unsigned long lr = LR();
  mach_msg_header_t *h = (mach_msg_header_t *)a0;
  unsigned opt = (unsigned)a1;
  int sendid = (opt & MACH_SEND_MSG) && h ? h->msgh_id : -1;
  unsigned remote = (opt & MACH_SEND_MSG) && h ? h->msgh_remote_port : 0;
  uint64_t r = ORIG(mach_msg)(a0, a1, a2, a3, a4, a5, a6, a7);
  int rcvid = (opt & MACH_RCV_MSG) && h && (uint32_t)r == 0 ? h->msgh_id : -1;
  emit("mach_msg\tsend=%d\trcv=%d\topt=0x%x\tret=0x%x\tremote=0x%x\tlr=0x%lx\n", sendid, rcvid, opt, (unsigned)r, remote, lr);
  return r;
}
INTERPOSE(mach_msg);
W(mach_msg2_internal) {
  emit("mach_msg2\tid=%d\tsize=%u\topt=0x%llx\tremote=0x%x\tlr=0x%lx\n", (int)(a4 >> 32), (unsigned)(a2 >> 32),
       (unsigned long long)a1, (unsigned)a3, LR());
  return ORIG(mach_msg2_internal)(a0, a1, a2, a3, a4, a5, a6, a7);
}
INTERPOSE(mach_msg2_internal);
