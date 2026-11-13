#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#define printf(...) (printf(__VA_ARGS__), fflush(stdout))

static long sc(long n, long a, long b, long c, long d, long e, long f, long g, long h,
               int* carry) {
  register long x16 __asm__("x16") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  register long x6 __asm__("x6") = g;
  register long x7 __asm__("x7") = h;
  int failed;
  __asm__ volatile("svc #0x80\n\tcset %w1, cs"
                   : "+r"(x0), "=r"(failed)
                   : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6), "r"(x7)
                   : "memory", "cc");
  *carry = failed;
  return x0;
}

static int g_c;
#define SC(n, a, b, c, d, e, f) sc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f), 0, 0, &g_c)
#define SC8(n, a, b, c, d, e, f, g, h) sc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f), (long)(g), (long)(h), &g_c)
#define SHOW(name, r) printf("%s: carry=%d r=%ld\n", (name), g_c, g_c ? (long)(r) : (long)(r))
#define SHOWOK(name, r) printf("%s: carry=%d %s\n", (name), g_c, g_c ? "err" : "ok")
#define SHOWERR(name, r) printf("%s: carry=%d err=%ld\n", (name), g_c, g_c ? (long)(r) : 0L)
#define SHOWVAL(name, r) printf("%s: carry=%d val=%ld\n", (name), g_c, g_c ? 0L : (long)(r))

struct kq_event {
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

static int g_kq;

static void* trigger_user(void* arg) {
  (void)arg;
  struct timespec ts = {0, 30 * 1000 * 1000};
  nanosleep(&ts, 0);
  struct kevent ch;
  EV_SET(&ch, 7, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
  int c;
  sc(363, g_kq, (long)&ch, 1, 0, 0, 0, 0, 0, &c);
  return 0;
}

static int g_pipe[2];
static void* write_pipe(void* arg) {
  (void)arg;
  struct timespec ts = {0, 30 * 1000 * 1000};
  nanosleep(&ts, 0);
  (void)!write(g_pipe[1], "x", 1);
  return 0;
}

static void test_kqueue(void) {
  struct kevent ch, out[4];
  struct timespec zero = {0, 0}, shortt = {0, 20 * 1000 * 1000};
  long r;
  g_kq = (int)SC(362, 0, 0, 0, 0, 0, 0);
  printf("kqueue: carry=%d fd_ok=%d\n", g_c, g_kq >= 3);
  EV_SET(&ch, 7, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, (void*)0x1234);
  r = SC(363, g_kq, &ch, 1, 0, 0, 0);
  SHOWERR("add user", r);
  r = SC(363, g_kq, 0, 0, out, 4, &zero);
  SHOWVAL("poll empty", r);
  r = SC(363, g_kq, 0, 0, out, 4, &shortt);
  SHOWVAL("wait timeout", r);
  pthread_t t;
  pthread_create(&t, 0, trigger_user, 0);
  r = SC(363, g_kq, 0, 0, out, 4, 0);
  printf("wait user trigger: carry=%d n=%ld filter=%d ident=%lu udata=%lu fflags_trigger=%d\n",
         g_c, r, g_c ? 0 : out[0].filter, g_c ? 0UL : (unsigned long)out[0].ident,
         g_c ? 0UL : (unsigned long)(uintptr_t)out[0].udata,
         g_c ? 0 : (int)((out[0].fflags & NOTE_TRIGGER) != 0));
  pthread_join(t, 0);
  EV_SET(&ch, 9, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS, 15000, 0);
  r = SC(363, g_kq, &ch, 1, out, 4, 0);
  printf("timer oneshot: carry=%d n=%ld filter=%d ident=%lu\n", g_c, r, g_c ? 0 : out[0].filter,
         g_c ? 0UL : (unsigned long)out[0].ident);
  pipe(g_pipe);
  EV_SET(&ch, g_pipe[0], EVFILT_READ, EV_ADD, 0, 0, 0);
  r = SC(363, g_kq, &ch, 1, out, 4, &zero);
  SHOWVAL("read filter idle", r);
  pthread_create(&t, 0, write_pipe, 0);
  r = SC(363, g_kq, 0, 0, out, 4, 0);
  printf("read filter fires: carry=%d n=%ld data=%ld\n", g_c, r, g_c ? 0L : (long)out[0].data);
  pthread_join(t, 0);
  EV_SET(&ch, 9999, EVFILT_READ, EV_ADD, 0, 0, 0);
  r = SC(363, g_kq, &ch, 1, out, 4, &zero);
  printf("bad fd add: carry=%d n=%ld flags_error=%d data=%ld\n", g_c, r,
         g_c ? 0 : (int)((out[0].flags & EV_ERROR) != 0), g_c ? 0L : (long)out[0].data);
  r = SC(363, 9998, 0, 0, out, 4, &zero);
  SHOWERR("bad kq", r);
  r = SC(363, g_kq, 0, -1, out, 4, &zero);
  SHOWERR("negative nchanges", r);
  struct timespec badt = {0, 2000000000L};
  r = SC(363, g_kq, 0, 0, out, 4, &badt);
  SHOWERR("bad timeout", r);

  int kq64 = (int)SC(362, 0, 0, 0, 0, 0, 0);
  struct kevent64_s c64, o64[2];
  EV_SET64(&c64, 11, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0x55, 1, 2);
  r = SC8(369, kq64, &c64, 1, o64, 2, 0, &zero, 0);
  SHOWERR("kevent64 add", r);
  EV_SET64(&c64, 11, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0, 0, 0);
  r = SC8(369, kq64, &c64, 1, o64, 2, 0, &zero, 0);
  printf("kevent64 trigger: carry=%d n=%ld udata=%llu ext0=%llu\n", g_c, r,
         g_c ? 0ULL : (unsigned long long)o64[0].udata, g_c ? 0ULL : (unsigned long long)o64[0].ext[0]);

  r = SC(363, kq64, 0, 0, o64, 2, &zero);
  SHOWERR("kevent on kevent64 queue", r);
  int kqq = (int)SC(362, 0, 0, 0, 0, 0, 0);
  struct kq_event q[2], qo[2];
  memset(q, 0, sizeof(q));
  q[0].ident = 21;
  q[0].filter = EVFILT_USER;
  q[0].flags = EV_ADD | EV_CLEAR;
  q[0].udata = 0x66;
  r = SC8(374, kqq, q, 1, qo, 2, 0, 0, 1 | 2);
  SHOWERR("kevent_qos add", r);
  memset(q, 0, sizeof(q));
  q[0].ident = 21;
  q[0].filter = EVFILT_USER;
  q[0].fflags = NOTE_TRIGGER;
  r = SC8(374, kqq, q, 1, qo, 2, 0, 0, 1 | 2);
  printf("kevent_qos trigger: carry=%d n=%ld udata=%llu\n", g_c, r,
         (g_c || r < 1) ? 0ULL : (unsigned long long)qo[0].udata);
  r = SC8(374, kqq, 0, 0, qo, 2, 0, 0, 1 | 2);
  SHOWVAL("kevent_qos idle", r);
  r = SC8(374, kqq, 0, 0, qo, 2, 0, 0, 0x20);
  SHOWERR("kevent_qos workq flag", r);
  r = SC8(375, 0, 0, 0, 0, 0, 0, 0, 0);
  SHOWERR("kevent_id", r);
  close(kq64);
  close(kqq);
  close(g_kq);
  close(g_pipe[0]);
  close(g_pipe[1]);
}

static void test_ctl_select_poll(void) {
  long r;
  static const long par[][2] = {{3, 0}, {1, 0}, {3, 4}, {1, 4}, {0, 2}, {0, 6}, {0, 0}, {3, 2}, {7, 0}};
  for (unsigned i = 0; i < sizeof(par) / sizeof(par[0]); ++i) {
    r = SC(478, 0x800, par[i][0], par[i][1], 0, 0, 0);
    printf("bsdthread_ctl parallelism case %u: carry=%d val=%ld\n", i, g_c, r);
  }
  r = SC(478, 0x7777, 0, 0, 0, 0, 0);
  SHOWERR("bsdthread_ctl unknown", r);
  int p[2];
  pipe(p);
  struct pollfd pf = {p[0], POLLIN, 0};
  r = SC(230, &pf, 1, 0, 0, 0, 0);
  printf("poll idle: carry=%d n=%ld revents=%d\n", g_c, r, pf.revents);
  r = SC(230, &pf, 1, 20, 0, 0, 0);
  printf("poll timeout: carry=%d n=%ld revents=%d\n", g_c, r, pf.revents);
  (void)!write(p[1], "z", 1);
  r = SC(230, &pf, 1, 20, 0, 0, 0);
  printf("poll ready: carry=%d n=%ld revents=%d\n", g_c, r, pf.revents);
  struct pollfd bad = {9999, POLLIN, 0};
  r = SC(230, &bad, 1, 0, 0, 0, 0);
  printf("poll nval: carry=%d n=%ld revents=%d\n", g_c, r, bad.revents);
  r = SC(230, &pf, 5000, 0, 0, 0, 0);
  SHOWERR("poll too many", r);
  fd_set rd, wr;
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  FD_SET(p[0], &rd);
  FD_SET(p[1], &wr);
  struct timeval tv = {0, 0};
  r = SC(93, p[1] + 1, &rd, &wr, 0, &tv, 0);
  printf("select: carry=%d n=%ld rd=%d wr=%d\n", g_c, r, FD_ISSET(p[0], &rd) != 0,
         FD_ISSET(p[1], &wr) != 0);
  char c;
  (void)!read(p[0], &c, 1);
  FD_ZERO(&rd);
  FD_SET(p[0], &rd);
  struct timeval ts = {0, 20000};
  r = SC(93, p[0] + 1, &rd, 0, 0, &ts, 0);
  printf("select timeout: carry=%d n=%ld rd=%d\n", g_c, r, FD_ISSET(p[0], &rd) != 0);
  FD_ZERO(&rd);
  FD_SET(9999 % 1000, &rd);
  r = SC(93, 1001, &rd, 0, 0, &tv, 0);
  printf("select bad fd 999: carry=%d val=%ld\n", g_c, g_c ? (long)r : r);
  FD_ZERO(&rd);
  FD_SET(7, &rd);
  r = SC(93, 8, &rd, 0, 0, &tv, 0);
  SHOWERR("select closed fd 7", r);
  (void)!write(p[1], "q", 1);
  FD_ZERO(&rd);
  FD_SET(p[0], &rd);
  struct timespec pts = {0, 0};
  r = SC(394, p[0] + 1, &rd, 0, 0, &pts, 0);
  printf("pselect: carry=%d n=%ld rd=%d\n", g_c, r, FD_ISSET(p[0], &rd) != 0);
  r = SC(93, -1, 0, 0, 0, 0, 0);
  SHOWERR("select negative", r);
  close(p[0]);
  close(p[1]);
}

static void test_sockets(void) {
  long r;
  int sv[2] = {-1, -1};
  r = SC(135, AF_UNIX, SOCK_STREAM, 0, sv, 0, 0);
  printf("socketpair: carry=%d ok=%d\n", g_c, !g_c && sv[0] >= 3 && sv[1] >= 3);
  r = SC(133, sv[0], "hello", 5, 0, 0, 0);
  SHOWVAL("sendto stream", r);
  char buf[16] = {0};
  r = SC(29, sv[1], buf, sizeof(buf), 0, 0, 0);
  printf("recvfrom stream: carry=%d n=%ld text=%s\n", g_c, r, buf);
  struct iovec iov[2] = {{"ab", 2}, {"cde", 3}};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  m.msg_iov = iov;
  m.msg_iovlen = 2;
  r = SC(28, sv[0], &m, 0, 0, 0, 0);
  SHOWVAL("sendmsg two iov", r);
  char b1[4] = {0}, b2[8] = {0};
  struct iovec riov[2] = {{b1, 2}, {b2, 8}};
  memset(&m, 0, sizeof(m));
  m.msg_iov = riov;
  m.msg_iovlen = 2;
  r = SC(27, sv[1], &m, 0, 0, 0, 0);
  printf("recvmsg two iov: carry=%d n=%ld first=%s second=%s\n", g_c, r, b1, b2);
  int pp[2];
  pipe(pp);
  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));
  struct msghdr s;
  memset(&s, 0, sizeof(s));
  struct iovec one = {"f", 1};
  s.msg_iov = &one;
  s.msg_iovlen = 1;
  s.msg_control = control;
  s.msg_controllen = sizeof(control);
  struct cmsghdr* cm = CMSG_FIRSTHDR(&s);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &pp[1], sizeof(int));
  r = SC(28, sv[0], &s, 0, 0, 0, 0);
  SHOWVAL("sendmsg rights", r);
  char rc[1];
  char rcontrol[CMSG_SPACE(sizeof(int))];
  struct iovec rone = {rc, 1};
  struct msghdr g;
  memset(&g, 0, sizeof(g));
  g.msg_iov = &rone;
  g.msg_iovlen = 1;
  g.msg_control = rcontrol;
  g.msg_controllen = sizeof(rcontrol);
  r = SC(27, sv[1], &g, 0, 0, 0, 0);
  int passed = -1;
  struct cmsghdr* gm = CMSG_FIRSTHDR(&g);
  if (!g_c && gm && gm->cmsg_type == SCM_RIGHTS) memcpy(&passed, CMSG_DATA(gm), sizeof(int));
  printf("recvmsg rights: carry=%d n=%ld got_fd=%d\n", g_c, r, passed >= 3);
  if (passed >= 3) {
    (void)!write(passed, "P", 1);
    char rb = 0;
    (void)!read(pp[0], &rb, 1);
    printf("passed fd works: %c\n", rb);
  }
  r = SC(28, sv[0], &s, MSG_PEEK | 0x7000, 0, 0, 0);
  SHOWERR("sendmsg bad flags", r);
  int badfd = 9999;
  memcpy(CMSG_DATA(cm), &badfd, sizeof(int));
  r = SC(28, sv[0], &s, 0, 0, 0, 0);
  SHOWERR("sendmsg rights bad fd", r);
  int ty = 0;
  socklen_t tl = sizeof(ty);
  r = SC(118, sv[0], SOL_SOCKET, SO_TYPE, &ty, &tl, 0);
  printf("getsockopt SO_TYPE: carry=%d type=%d len=%u\n", g_c, ty, tl);
  int on = 1;
  r = SC(105, sv[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on), 0);
  SHOWERR("setsockopt NOSIGPIPE", r);
  int got = 0;
  tl = sizeof(got);
  r = SC(118, sv[0], SOL_SOCKET, SO_NOSIGPIPE, &got, &tl, 0);
  printf("getsockopt NOSIGPIPE: carry=%d val=%d\n", g_c, got);
  r = SC(105, 9999, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on), 0);
  SHOWERR("setsockopt bad fd", r);
  r = SC(105, pp[0], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on), 0);
  SHOWERR("setsockopt not socket", r);
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  r = SC(32, sv[0], &ss, &sl, 0, 0, 0);
  printf("getsockname pair: carry=%d family=%d\n", g_c, g_c ? 0 : ((struct sockaddr*)&ss)->sa_family);
  sl = sizeof(ss);
  r = SC(31, sv[0], &ss, &sl, 0, 0, 0);
  printf("getpeername pair: carry=%d family=%d\n", g_c, g_c ? 0 : ((struct sockaddr*)&ss)->sa_family);
  r = SC(106, sv[0], 1, 0, 0, 0, 0);
  SHOWERR("listen pair", r);
  sl = sizeof(ss);
  r = SC(30, sv[0], &ss, &sl, 0, 0, 0);
  SHOWERR("accept pair", r);
  r = SC(134, sv[0], SHUT_WR, 0, 0, 0, 0);
  SHOWERR("shutdown write", r);
  r = SC(29, sv[1], buf, sizeof(buf), MSG_DONTWAIT, 0, 0);
  SHOWVAL("recv after drain (eof or again)", r);
  r = SC(134, 9999, SHUT_WR, 0, 0, 0, 0);
  SHOWERR("shutdown bad fd", r);
  r = SC(134, pp[0], SHUT_WR, 0, 0, 0, 0);
  SHOWERR("shutdown not socket", r);
  int nb[2];
  SC(135, AF_UNIX, SOCK_DGRAM, 0, nb, 0, 0);
  r = SC(29, nb[1], buf, sizeof(buf), MSG_DONTWAIT, 0, 0);
  SHOWERR("recv dgram empty dontwait", r);
  r = SC(135, AF_INET, SOCK_STREAM, 0, nb, 0, 0);
  SHOWERR("socketpair inet", r);
  r = SC(135, 9999, SOCK_STREAM, 0, nb, 0, 0);
  SHOWERR("socketpair bad domain", r);
  r = SC(480, sv[0], 0, 0, 0, 0, 0);
  SHOWERR("recvmsg_x", r);
  r = SC(447, sv[0], 0, 0, 0, 0, 0);
  SHOWERR("connectx", r);
  close(sv[0]);
  close(sv[1]);
  close(pp[0]);
  close(pp[1]);

  int s1 = socket(AF_UNIX, SOCK_DGRAM, 0), s2 = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un a1, a2;
  memset(&a1, 0, sizeof(a1));
  memset(&a2, 0, sizeof(a2));
  a1.sun_family = a2.sun_family = AF_UNIX;
  snprintf(a1.sun_path, sizeof(a1.sun_path), "/tmp/net-raw-a-%d", getpid());
  snprintf(a2.sun_path, sizeof(a2.sun_path), "/tmp/net-raw-b-%d", getpid());
  unlink(a1.sun_path);
  unlink(a2.sun_path);
  bind(s1, (struct sockaddr*)&a1, sizeof(a1));
  bind(s2, (struct sockaddr*)&a2, sizeof(a2));
  struct pollfd dp = {s2, POLLIN, 0};
  r = SC(230, &dp, 1, 0, 0, 0, 0);
  printf("dgram poll idle: carry=%d n=%ld\n", g_c, r);
  struct iovec di[2] = {{"da", 2}, {"ta", 2}};
  struct msghdr dm;
  memset(&dm, 0, sizeof(dm));
  dm.msg_name = &a2;
  dm.msg_namelen = sizeof(a2);
  dm.msg_iov = di;
  dm.msg_iovlen = 2;
  r = SC(28, s1, &dm, 0, 0, 0, 0);
  SHOWVAL("dgram sendmsg", r);
  r = SC(230, &dp, 1, 0, 0, 0, 0);
  printf("dgram poll ready: carry=%d n=%ld revents=%d\n", g_c, r, dp.revents);
  char db[8] = {0};
  struct iovec dr = {db, 8};
  struct sockaddr_un from;
  memset(&from, 0, sizeof(from));
  struct msghdr rm;
  memset(&rm, 0, sizeof(rm));
  rm.msg_name = &from;
  rm.msg_namelen = sizeof(from);
  rm.msg_iov = &dr;
  rm.msg_iovlen = 1;
  r = SC(27, s2, &rm, 0, 0, 0, 0);
  printf("dgram recvmsg: carry=%d n=%ld text=%s from_match=%d\n", g_c, r, db,
         strcmp(from.sun_path, a1.sun_path) == 0);
  struct sockaddr_un own;
  memset(&own, 0, sizeof(own));
  sl = sizeof(own);
  r = SC(32, s1, &own, &sl, 0, 0, 0);
  printf("dgram getsockname: carry=%d family=%d match=%d\n", g_c, own.sun_family,
         strcmp(own.sun_path, a1.sun_path) == 0);
  sl = sizeof(own);
  r = SC(31, s1, &own, &sl, 0, 0, 0);
  SHOWERR("dgram getpeername unconnected", r);
  connect(s1, (struct sockaddr*)&a2, sizeof(a2));
  memset(&own, 0, sizeof(own));
  sl = sizeof(own);
  r = SC(31, s1, &own, &sl, 0, 0, 0);
  printf("dgram getpeername: carry=%d match=%d\n", g_c, strcmp(own.sun_path, a2.sun_path) == 0);
  r = SC(106, s1, 1, 0, 0, 0, 0);
  SHOWERR("listen dgram", r);
  sl = sizeof(own);
  r = SC(30, s1, &own, &sl, 0, 0, 0);
  SHOWERR("accept dgram", r);
  r = SC(133, s1, "again", 5, 0, 0, 0);
  SHOWVAL("sendto connected dgram nocancel path", r);
  r = SC(403, s2, db, sizeof(db), 0, 0, 0);
  SHOWVAL("recvfrom nocancel dgram", r);
  close(s1);
  close(s2);
  unlink(a1.sun_path);
  unlink(a2.sun_path);
}

static void test_memory(void) {
  long r;
  void* p = (void*)SC(197, 0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  printf("mmap raw: carry=%d ok=%d\n", g_c, !g_c && p != 0);
  if (!g_c) {
    memset(p, 0x5a, 65536);
    printf("mmap usable: %d\n", ((unsigned char*)p)[65535]);
  }
  r = SC(75, p, 65536, 0, 0, 0, 0);
  SHOWERR("madvise normal", r);
  r = SC(75, p, 65536, 5, 0, 0, 0);
  SHOWERR("madvise free", r);
  r = SC(75, p, 65536, 99, 0, 0, 0);
  SHOWERR("madvise bad advice", r);
  r = SC(75, (char*)p + 1, 4096, 0, 0, 0, 0);
  SHOWERR("madvise unaligned", r);
  r = SC(65, 0x10000, 4096, 0x10, 0, 0, 0);
  SHOWERR("msync unmapped", r);
  r = SC(74, 0x10000, 4096, PROT_READ, 0, 0, 0);
  SHOWERR("mprotect unmapped", r);
  r = SC(73, 0x10000, 4096, 0, 0, 0, 0);
  SHOWERR("munmap unmapped", r);
  r = SC(204, 0x10001, 4096, 0, 0, 0, 0);
  SHOWERR("munlock unaligned", r);
  r = SC(75, 0x10000, 4096, 0, 0, 0, 0);
  SHOWERR("madvise unmapped", r);
  r = SC(65, p, 65536, 0x10, 0, 0, 0);
  SHOWERR("msync sync", r);
  r = SC(65, p, 65536, 0x1 | 0x10, 0, 0, 0);
  SHOWERR("msync async+sync", r);
  r = SC(65, p, 65536, 0x1000, 0, 0, 0);
  SHOWERR("msync bad flags", r);
  unsigned char vec[16];
  memset(vec, 0, sizeof(vec));
  r = SC(78, p, 65536, vec, 0, 0, 0);
  printf("mincore: carry=%d first=%d\n", g_c, vec[0] & 1);
  vec[0] = 9;
  r = SC(78, 0x10000, 4096, vec, 0, 0, 0);
  printf("mincore unmapped: carry=%d vec=%d\n", g_c, vec[0]);
  r = SC(203, p, 16384, 0, 0, 0, 0);
  SHOWERR("mlock", r);
  r = SC(204, p, 16384, 0, 0, 0, 0);
  SHOWERR("munlock", r);
  r = SC(203, 0x10000, 4096, 0, 0, 0, 0);
  SHOWERR("mlock unmapped", r);
  r = SC(250, p, 65536, 1, 0, 0, 0);
  SHOWERR("minherit copy", r);
  r = SC(250, p, 65536, 9, 0, 0, 0);
  SHOWERR("minherit bad", r);
  r = SC(74, p, 65536, PROT_READ, 0, 0, 0);
  SHOWERR("mprotect read", r);
  r = SC(73, p, 65536, 0, 0, 0, 0);
  SHOWERR("munmap", r);
  r = SC(73, (char*)p + 1, 4096, 0, 0, 0, 0);
  SHOWERR("munmap unaligned", r);
  char name[64];
  snprintf(name, sizeof(name), "/net-raw-shm-%d", getpid());
  r = SC(266, name, O_CREAT | O_RDWR, 0600, 0, 0, 0);
  printf("shm_open: carry=%d ok=%d\n", g_c, !g_c && r >= 3);
  if (!g_c) {
    ftruncate((int)r, 4096);
    close((int)r);
  }
  r = SC(267, name, 0, 0, 0, 0, 0);
  SHOWERR("shm_unlink", r);
  r = SC(267, name, 0, 0, 0, 0, 0);
  SHOWERR("shm_unlink again", r);
}

union semun_u {
  int val;
  struct semid_ds* buf;
  unsigned short* array;
};

static void test_ipc(void) {
  long r;
  long id = SC(255, IPC_PRIVATE, 2, IPC_CREAT | 0600, 0, 0, 0);
  printf("semget: carry=%d ok=%d\n", g_c, !g_c && id >= 0);
  if (!g_c) {
    r = SC(254, id, 0, 8, 3, 0, 0);
    SHOWERR("semctl SETVAL", r);
    r = SC(254, id, 0, 5, 0, 0, 0);
    SHOWVAL("semctl GETVAL", r);
    struct sembuf op = {0, -1, 0};
    r = SC(256, id, &op, 1, 0, 0, 0);
    SHOWERR("semop dec", r);
    r = SC(254, id, 0, 5, 0, 0, 0);
    SHOWVAL("semctl GETVAL after", r);
    unsigned short all[2] = {0, 0};
    r = SC(254, id, 0, 6, all, 0, 0);
    printf("semctl GETALL: carry=%d v0=%d v1=%d\n", g_c, all[0], all[1]);
    struct semid_ds ds;
    memset(&ds, 0, sizeof(ds));
    r = SC(254, id, 0, 2, &ds, 0, 0);
    printf("semctl STAT: carry=%d nsems=%d\n", g_c, ds.sem_nsems);
    struct sembuf nowait = {1, -1, IPC_NOWAIT};
    r = SC(256, id, &nowait, 1, 0, 0, 0);
    SHOWERR("semop would block nowait", r);
    r = SC(256, id, &op, 0, 0, 0, 0);
    SHOWERR("semop zero ops", r);
    r = SC(254, id, 0, 0, 0, 0, 0);
    SHOWERR("semctl RMID", r);
    r = SC(254, id, 0, 5, 0, 0, 0);
    SHOWERR("semctl after rmid", r);
  }
  long q = SC(259, IPC_PRIVATE, IPC_CREAT | 0600, 0, 0, 0, 0);
  printf("msgget: carry=%d ok=%d\n", g_c, !g_c && q >= 0);
  if (!g_c) {
    struct {
      long type;
      char text[16];
    } m = {7, "queued"}, got;
    memset(&got, 0, sizeof(got));
    r = SC(260, q, &m, 7, 0, 0, 0);
    SHOWERR("msgsnd", r);
    r = SC(261, q, &got, sizeof(got.text), 0, 0, 0);
    printf("msgrcv: carry=%d n=%ld type=%ld text=%s\n", g_c, r, got.type, got.text);
    r = SC(261, q, &got, sizeof(got.text), 0, IPC_NOWAIT, 0);
    SHOWERR("msgrcv empty nowait", r);
    struct msqid_ds mds;
    memset(&mds, 0, sizeof(mds));
    r = SC(258, q, 2, &mds, 0, 0, 0);
    printf("msgctl STAT: carry=%d qnum=%d\n", g_c, (int)mds.msg_qnum);
    r = SC(258, q, 0, 0, 0, 0, 0);
    SHOWERR("msgctl RMID", r);
  }
  char name[64];
  snprintf(name, sizeof(name), "/net-raw-sem-%d", getpid());
  r = SC(270, name, 0, 0, 0, 0, 0);
  long sem = SC(268, name, O_CREAT | O_EXCL, 0600, 1, 0, 0);
  printf("sem_open: carry=%d ok=%d\n", g_c, !g_c);
  if (!g_c) {
    r = SC(271, sem, 0, 0, 0, 0, 0);
    SHOWERR("sem_wait", r);
    r = SC(272, sem, 0, 0, 0, 0, 0);
    SHOWERR("sem_trywait empty", r);
    r = SC(273, sem, 0, 0, 0, 0, 0);
    SHOWERR("sem_post", r);
    r = SC(420, sem, 0, 0, 0, 0, 0);
    SHOWERR("sem_wait_nocancel", r);
    r = SC(269, sem, 0, 0, 0, 0, 0);
    SHOWERR("sem_close", r);
  }
  r = SC(270, name, 0, 0, 0, 0, 0);
  SHOWERR("sem_unlink", r);
  r = SC(270, name, 0, 0, 0, 0, 0);
  SHOWERR("sem_unlink again", r);
}

static void test_fcntl_ioctl(void) {
  long r;
  int p[2];
  pipe(p);
  r = SC(92, p[0], F_GETFD, 0, 0, 0, 0);
  SHOWVAL("fcntl GETFD", r);
  r = SC(92, p[0], F_SETFD, FD_CLOEXEC, 0, 0, 0);
  SHOWERR("fcntl SETFD", r);
  r = SC(92, p[0], F_GETFD, 0, 0, 0, 0);
  SHOWVAL("fcntl GETFD cloexec", r);
  r = SC(92, p[0], F_SETFL, O_NONBLOCK, 0, 0, 0);
  SHOWERR("fcntl SETFL nonblock", r);
  r = SC(92, p[0], F_GETFL, 0, 0, 0, 0);
  printf("fcntl GETFL: carry=%d nonblock=%d\n", g_c, (int)((r & O_NONBLOCK) != 0));
  long d = SC(92, p[0], F_DUPFD, 20, 0, 0, 0);
  printf("fcntl DUPFD: carry=%d ge20=%d\n", g_c, d >= 20);
  if (!g_c) {
    char c;
    (void)!write(p[1], "d", 1);
    printf("dup readable: %d\n", (int)read((int)d, &c, 1));
    close((int)d);
  }
  d = SC(92, p[0], F_DUPFD_CLOEXEC, 0, 0, 0, 0);
  printf("fcntl DUPFD_CLOEXEC: carry=%d ok=%d\n", g_c, d >= 3);
  if (!g_c) {
    r = SC(92, (int)d, F_GETFD, 0, 0, 0, 0);
    SHOWVAL("dupfd_cloexec flag", r);
    close((int)d);
  }
  r = SC(92, 9999, F_GETFD, 0, 0, 0, 0);
  SHOWERR("fcntl bad fd", r);
  r = SC(406, p[0], F_GETFD, 0, 0, 0, 0);
  SHOWVAL("fcntl_nocancel GETFD", r);
  r = SC(92, p[0], 9999, 0, 0, 0, 0);
  SHOWERR("fcntl bad cmd", r);
  int fd = open("/tmp/net-raw-fcntl", O_RDWR | O_CREAT | O_TRUNC, 0600);
  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  r = SC(92, fd, F_SETLK, &fl, 0, 0, 0);
  SHOWERR("fcntl SETLK", r);
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  r = SC(92, fd, F_GETLK, &fl, 0, 0, 0);
  printf("fcntl GETLK own lock: carry=%d type_unlck=%d\n", g_c, fl.l_type == F_UNLCK);
  char path[1024];
  memset(path, 0, sizeof(path));
  r = SC(92, fd, F_GETPATH, path, 0, 0, 0);
  printf("fcntl GETPATH: carry=%d tail=%s\n", g_c, strrchr(path, '/'));
  fstore_t fs = {F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, 4096, 0};
  r = SC(92, fd, F_PREALLOCATE, &fs, 0, 0, 0);
  printf("fcntl PREALLOCATE: carry=%d\n", g_c);
  r = SC(92, fd, F_NOCACHE, 1, 0, 0, 0);
  SHOWERR("fcntl NOCACHE", r);
  r = SC(92, fd, F_FULLFSYNC, 0, 0, 0, 0);
  printf("fcntl FULLFSYNC: carry=%d\n", g_c);
  close(fd);
  unlink("/tmp/net-raw-fcntl");
  int avail = -1;
  (void)!write(p[1], "abc", 3);
  r = SC(54, p[0], 0x4004667f, &avail, 0, 0, 0);
  printf("ioctl FIONREAD: carry=%d avail=%d\n", g_c, avail);
  r = SC(54, p[0], 0x4004667f, 0, 0, 0, 0);
  SHOWERR("ioctl FIONREAD efault", r);
  int one = 1;
  r = SC(54, p[0], 0x8004667e, &one, 0, 0, 0);
  SHOWERR("ioctl FIONBIO", r);
  r = SC(54, 9999, 0x4004667f, &avail, 0, 0, 0);
  SHOWERR("ioctl bad fd", r);
  r = SC(54, p[0], 0x40087468, &avail, 0, 0, 0);
  SHOWERR("ioctl TIOCGWINSZ on pipe", r);
  close(p[0]);
  close(p[1]);
}

static void test_misc(void) {
  static const struct { int n; const char* name; } list[] = {
    {436, "pid_shutdown_sockets"}, {450, "socket_delegate"}, {460, "necp_match_policy"},
    {490, "netagent_trigger"}, {501, "necp_open"}, {502, "necp_client_action"},
    {503, "nexus_open"}, {504, "nexus_register"}, {505, "nexus_deregister"},
    {506, "nexus_create"}, {507, "nexus_destroy"}, {508, "nexus_get_opt"},
    {509, "nexus_set_opt"}, {510, "channel_open"}, {511, "channel_get_info"},
    {512, "channel_sync"}, {513, "channel_get_opt"}, {514, "channel_set_opt"},
    {522, "necp_session_open"}, {523, "necp_session_action"}, {525, "net_qos_guideline"},
    {296, "vm_pressure_monitor"}, {439, "kas_info"}, {440, "memorystatus_control"},
    {251, "semsys"}, {252, "msgsys"}, {253, "shmsys"},
    {262, "shmat"}, {263, "shmctl"}, {264, "shmdt"}, {265, "shmget"},
    {443, "guarded_kqueue_np"}, {530, "kqueue_workloop_ctl"}, {449, "peeloff"},
    {448, "disconnectx"},
  };
  for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); ++i) {
    long r = SC(list[i].n, 0, 0, 0, 0, 0, 0);
    printf("misc %s: carry=%d err=%ld\n", list[i].name, g_c, g_c ? r : 0L);
  }
}

int main(int argc, char** argv) {
  const char* which = argc > 1 ? argv[1] : "all";
  int all = strcmp(which, "all") == 0;
  if (all || !strcmp(which, "kqueue")) test_kqueue();
  if (all || !strcmp(which, "poll")) test_ctl_select_poll();
  if (all || !strcmp(which, "sockets")) test_sockets();
  if (all || !strcmp(which, "memory")) test_memory();
  if (all || !strcmp(which, "ipc")) test_ipc();
  if (all || !strcmp(which, "fcntl")) test_fcntl_ioctl();
  if (all || !strcmp(which, "misc")) test_misc();
  printf("done\n");
  return 0;
}
