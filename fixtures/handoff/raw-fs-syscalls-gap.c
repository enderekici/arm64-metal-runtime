#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static long g_x1;
static int g_cs;
static long g_err;

static long raw(long n, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  register long x6 __asm__("x6") = 0;
  register long x7 __asm__("x7") = 0;
  register long x16 __asm__("x16") = n;
  long carry;
  __asm__ volatile("svc #0x80\n cset %1, cs"
                   : "+r"(x0), "=r"(carry), "+r"(x1)
                   : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6), "r"(x7), "r"(x16)
                   : "memory", "cc");
  g_cs = (int)carry;
  g_x1 = x1;
  g_err = x0;
  return carry ? -1 : x0;
}

static void show(const char *label, long r) {
  (void)r;
  if (g_cs) printf("%s: err=%ld\n", label, g_err);
  else printf("%s: ok\n", label);
}

static void showv(const char *label, long r) {
  if (g_cs) printf("%s: err=%ld\n", label, g_err);
  else printf("%s: ok val=%ld\n", label, r);
}

#define W "/tmp/sc-fs-work"
#define P(name) W "/" name

static void cleanup(void) {
  const char *files[] = {"f", "f2", "f3", "fifo", "mknod", "hard", "sym", "clone", "cloned",
                         "ext", "mkfifo_ext", "copy", "dirat/inner", "dirat/fifoat", "dirat/nodat",
                         "dirat/symat", "dirat/linkat", "dirat/renamed", "dirat/ren2", "dirat/f",
                         "dirat/clone", "rev"};
  char path[256];
  for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
    snprintf(path, sizeof path, "%s/%s", W, files[i]);
    raw(SYS_unlink, (long)path, 0, 0, 0, 0, 0);
  }
  const char *dirs[] = {"dirat/mkdirat", "dirat/ext_dir", "dirat/sub", "dirat", "mkdir", "mkdir_ext"};
  for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
    snprintf(path, sizeof path, "%s/%s", W, dirs[i]);
    raw(SYS_rmdir, (long)path, 0, 0, 0, 0, 0);
  }
  raw(SYS_rmdir, (long)W, 0, 0, 0, 0, 0);
}


int main(void) {
  long r;
  char buf[256];
  cleanup();
  mkdir(W, 0755);

  r = raw(SYS_umask, 022, 0, 0, 0, 0, 0);
  r = raw(SYS_getumask, 0, 0, 0, 0, 0, 0);
  showv("getumask", r);
  r = raw(SYS_umask_extended, 027, 0, 0, 0, 0, 0);
  showv("umask_extended prev", r);
  r = raw(SYS_getumask, 0, 0, 0, 0, 0, 0);
  showv("getumask after", r);
  r = raw(SYS_umask, 022, 0, 0, 0, 0, 0);

  long fd = raw(SYS_open, (long)P("f"), O_CREAT | O_WRONLY | O_TRUNC, 0644, 0, 0, 0);
  show("open create", fd);
  r = raw(SYS_write, fd, (long)"hello world\n", 12, 0, 0, 0);
  showv("write", r);
  r = raw(SYS_lseek, fd, 3, SEEK_SET, 0, 0, 0);
  showv("lseek set", r);
  r = raw(SYS_lseek, fd, 0, SEEK_END, 0, 0, 0);
  showv("lseek end", r);
  r = raw(SYS_lseek, 999, 0, SEEK_SET, 0, 0, 0);
  show("lseek badfd", r);
  r = raw(SYS_fsync, fd, 0, 0, 0, 0, 0);
  show("fsync", r);
  r = raw(SYS_fsync_nocancel, fd, 0, 0, 0, 0, 0);
  show("fsync_nocancel", r);
  r = raw(SYS_fdatasync, fd, 0, 0, 0, 0, 0);
  show("fdatasync", r);
  r = raw(SYS_fsync, 999, 0, 0, 0, 0, 0);
  show("fsync badfd", r);
  r = raw(SYS_flock, fd, LOCK_EX, 0, 0, 0, 0);
  show("flock ex", r);
  r = raw(SYS_flock, fd, LOCK_UN, 0, 0, 0, 0);
  show("flock un", r);
  r = raw(SYS_flock, 999, LOCK_EX, 0, 0, 0, 0);
  show("flock badfd", r);
  r = raw(SYS_sync, 0, 0, 0, 0, 0, 0);
  show("sync", r);

  struct stat st;
  memset(&st, 0, sizeof st);
  r = raw(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0);
  printf("fstat64 %s size=%lld mode=%o\n", g_cs ? "err" : "ok", (long long)st.st_size, st.st_mode & 0777);
  memset(&st, 0, sizeof st);
  r = raw(SYS_stat64, (long)P("f"), (long)&st, 0, 0, 0, 0);
  printf("stat64 %s size=%lld nlink=%d\n", g_cs ? "err" : "ok", (long long)st.st_size, st.st_nlink);
  r = raw(SYS_stat64, (long)P("missing"), (long)&st, 0, 0, 0, 0);
  show("stat64 missing", r);
  r = raw(SYS_fstat64, 999, (long)&st, 0, 0, 0, 0);
  show("fstat64 badfd", r);
  {
    uint8_t legacy[256];
    memset(legacy, 0, sizeof legacy);
    r = raw(SYS_stat, (long)P("f"), (long)legacy, 0, 0, 0, 0);
    int64_t size;
    uint16_t mode;
    memcpy(&size, legacy + 72, 8);
    memcpy(&mode, legacy + 8, 2);
    printf("stat legacy %s size=%lld mode=%o\n", g_cs ? "err" : "ok", (long long)size, mode & 0777);
    r = raw(SYS_fstat, fd, (long)legacy, 0, 0, 0, 0);
    memcpy(&size, legacy + 72, 8);
    printf("fstat legacy %s size=%lld\n", g_cs ? "err" : "ok", (long long)size);
    r = raw(SYS_lstat, (long)P("f"), (long)legacy, 0, 0, 0, 0);
    show("lstat legacy", r);
    r = raw(SYS_stat, (long)P("missing"), (long)legacy, 0, 0, 0, 0);
    show("stat legacy missing", r);
  }
  r = raw(SYS_ftruncate, fd, 5, 0, 0, 0, 0);
  show("ftruncate", r);
  r = raw(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0);
  printf("size after ftruncate %lld\n", (long long)st.st_size);
  r = raw(SYS_truncate, (long)P("f"), 8, 0, 0, 0, 0);
  show("truncate", r);
  r = raw(SYS_stat64, (long)P("f"), (long)&st, 0, 0, 0, 0);
  printf("size after truncate %lld\n", (long long)st.st_size);
  r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
  show("close", r);

  fd = raw(SYS_open, (long)P("f"), O_RDWR, 0, 0, 0, 0);
  show("open rdwr", fd);
  memset(buf, 0, sizeof buf);
  r = raw(SYS_read, fd, (long)buf, 5, 0, 0, 0);
  printf("read %ld [%s]\n", r, buf);
  r = raw(SYS_write, fd, (long)"ZZ", 2, 0, 0, 0);
  showv("write", r);
  memset(buf, 0, sizeof buf);
  r = raw(SYS_pread_nocancel, fd, (long)buf, 3, 2, 0, 0);
  printf("pread_nocancel %ld [%s]\n", r, buf);
  r = raw(SYS_pwrite_nocancel, fd, (long)"QQ", 2, 0, 0, 0);
  showv("pwrite_nocancel", r);
  {
    struct iovec iov[2];
    char a[4] = {0}, b[4] = {0};
    iov[0].iov_base = a; iov[0].iov_len = 3;
    iov[1].iov_base = b; iov[1].iov_len = 3;
    r = raw(SYS_preadv, fd, (long)iov, 2, 0, 0, 0);
    printf("preadv %ld [%s][%s]\n", r, a, b);
    r = raw(SYS_preadv_nocancel, fd, (long)iov, 2, 1, 0, 0);
    printf("preadv_nocancel %ld [%s][%s]\n", r, a, b);
    memset(a, 0, 4); memset(b, 0, 4);
    r = raw(SYS_lseek, fd, 0, SEEK_SET, 0, 0, 0);
    r = raw(SYS_readv, fd, (long)iov, 2, 0, 0, 0);
    printf("readv %ld [%s][%s]\n", r, a, b);
    r = raw(SYS_readv_nocancel, fd, (long)iov, 2, 0, 0, 0);
    printf("readv_nocancel %ld [%s][%s]\n", r, a, b);
    iov[0].iov_base = "ab"; iov[0].iov_len = 2;
    iov[1].iov_base = "cd"; iov[1].iov_len = 2;
    r = raw(SYS_lseek, fd, 0, SEEK_END, 0, 0, 0);
    r = raw(SYS_writev, fd, (long)iov, 2, 0, 0, 0);
    showv("writev", r);
    r = raw(SYS_writev_nocancel, fd, (long)iov, 2, 0, 0, 0);
    showv("writev_nocancel", r);
    r = raw(SYS_pwritev, fd, (long)iov, 2, 0, 0, 0);
    showv("pwritev", r);
    r = raw(SYS_pwritev_nocancel, fd, (long)iov, 2, 2, 0, 0);
    showv("pwritev_nocancel", r);
    r = raw(SYS_readv, fd, (long)iov, 2000, 0, 0, 0);
    show("readv too many", r);
    r = raw(SYS_readv, 999, (long)iov, 2, 0, 0, 0);
    show("readv badfd", r);
    r = raw(SYS_writev, 999, (long)iov, 2, 0, 0, 0);
    show("writev badfd", r);
    struct iovec bad = {(void *)8, 4};
    r = raw(SYS_lseek, fd, 0, SEEK_SET, 0, 0, 0);
    r = raw(SYS_readv, fd, (long)&bad, 1, 0, 0, 0);
    show("readv efault", r);
  }
  memset(buf, 0, sizeof buf);
  r = raw(SYS_pread_nocancel, fd, (long)buf, 20, 0, 0, 0);
  printf("final content %ld [%s]\n", r, buf);

  long d1 = raw(SYS_dup, fd, 0, 0, 0, 0, 0);
  show("dup", d1);
  r = raw(SYS_lseek, d1, 1, SEEK_SET, 0, 0, 0);
  r = raw(SYS_lseek, fd, 0, SEEK_CUR, 0, 0, 0);
  showv("shared offset", r);
  r = raw(SYS_write, d1, (long)"!", 1, 0, 0, 0);
  showv("write via dup", r);
  long d2 = raw(SYS_dup2, fd, d1 + 10, 0, 0, 0, 0);
  printf("dup2 %s same-as-target=%d\n", g_cs ? "err" : "ok", d2 == d1 + 10);
  r = raw(SYS_dup2, fd, fd, 0, 0, 0, 0);
  printf("dup2 same %s same=%d\n", g_cs ? "err" : "ok", r == fd);
  r = raw(SYS_dup2, 999, 50, 0, 0, 0, 0);
  show("dup2 badfd", r);
  long d3 = raw(SYS_dup3, fd, d1 + 11, O_CLOEXEC, 0, 0, 0);
  printf("dup3 %s cloexec=%d\n", g_cs ? "err" : "ok", fcntl((int)d3, F_GETFD));
  r = raw(SYS_dup3, fd, fd, 0, 0, 0, 0);
  show("dup3 same", r);
  r = raw(SYS_dup3, fd, d1 + 12, 0x1234, 0, 0, 0);
  show("dup3 badflags", r);
  r = raw(SYS_dup, 999, 0, 0, 0, 0, 0);
  show("dup badfd", r);
  r = raw(SYS_close, d3, 0, 0, 0, 0, 0);
  r = raw(SYS_close, d2, 0, 0, 0, 0, 0);
  r = raw(SYS_close, d1, 0, 0, 0, 0, 0);
  r = raw(SYS_close, fd, 0, 0, 0, 0, 0);

  {
    r = raw(SYS_pipe, 0, 0, 0, 0, 0, 0);
    long rd = r, wr = g_x1;
    show("pipe", r);
    r = raw(SYS_write, wr, (long)"pipe-data", 9, 0, 0, 0);
    showv("pipe write", r);
    memset(buf, 0, sizeof buf);
    r = raw(SYS_read, rd, (long)buf, 64, 0, 0, 0);
    printf("pipe read %ld [%s]\n", r, buf);
    r = raw(SYS_close, rd, 0, 0, 0, 0, 0);
    r = raw(SYS_close, wr, 0, 0, 0, 0, 0);
    r = raw(SYS_pipe2, 0, 0, 0, 0, 0, 0);
    rd = r; wr = g_x1;
    show("pipe2", r);
    r = raw(SYS_write, wr, (long)"p2", 2, 0, 0, 0);
    memset(buf, 0, sizeof buf);
    r = raw(SYS_read, rd, (long)buf, 64, 0, 0, 0);
    printf("pipe2 read %ld [%s]\n", r, buf);
    r = raw(SYS_close, rd, 0, 0, 0, 0, 0);
    r = raw(SYS_close, wr, 0, 0, 0, 0, 0);
  }

  r = raw(SYS_mkdir, (long)P("mkdir"), 0755, 0, 0, 0, 0);
  show("mkdir", r);
  r = raw(SYS_mkdir, (long)P("mkdir"), 0755, 0, 0, 0, 0);
  show("mkdir again", r);
  r = raw(SYS_mkfifo, (long)P("fifo"), 0644, 0, 0, 0, 0);
  show("mkfifo", r);
  r = raw(SYS_stat64, (long)P("fifo"), (long)&st, 0, 0, 0, 0);
  printf("fifo is fifo=%d\n", S_ISFIFO(st.st_mode));
  r = raw(SYS_mknod, (long)P("mknod"), S_IFIFO | 0600, 0, 0, 0, 0);
  show("mknod fifo", r);
  r = raw(SYS_link, (long)P("f"), (long)P("hard"), 0, 0, 0, 0);
  show("link", r);
  r = raw(SYS_stat64, (long)P("f"), (long)&st, 0, 0, 0, 0);
  printf("nlink after link %d\n", st.st_nlink);
  r = raw(SYS_link, (long)P("missing"), (long)P("hard2"), 0, 0, 0, 0);
  show("link missing", r);
  r = raw(SYS_symlink, (long)"f", (long)P("sym"), 0, 0, 0, 0);
  show("symlink", r);
  memset(buf, 0, sizeof buf);
  r = raw(SYS_readlink, (long)P("sym"), (long)buf, sizeof buf, 0, 0, 0);
  printf("readlink %ld [%s]\n", r, buf);
  r = raw(SYS_readlink, (long)P("f"), (long)buf, sizeof buf, 0, 0, 0);
  show("readlink non-link", r);
  r = raw(SYS_lstat, (long)P("sym"), (long)&st, 0, 0, 0, 0);
  {
    uint8_t legacy[256];
    memset(legacy, 0, sizeof legacy);
    r = raw(SYS_lstat, (long)P("sym"), (long)legacy, 0, 0, 0, 0);
    uint16_t mode;
    memcpy(&mode, legacy + 8, 2);
    printf("lstat legacy symlink=%d\n", (mode & S_IFMT) == S_IFLNK);
  }
  {
    struct timeval tv[2] = {{1000000000, 0}, {1000000100, 0}};
    r = raw(SYS_utimes, (long)P("f"), (long)tv, 0, 0, 0, 0);
    show("utimes", r);
    r = raw(SYS_stat64, (long)P("f"), (long)&st, 0, 0, 0, 0);
    printf("mtime=%ld atime=%ld\n", (long)st.st_mtime, (long)st.st_atime);
    struct timeval tv2[2] = {{1100000000, 0}, {1100000200, 0}};
    fd = raw(SYS_open, (long)P("f"), O_RDONLY, 0, 0, 0, 0);
    r = raw(SYS_futimes, fd, (long)tv2, 0, 0, 0, 0);
    show("futimes", r);
    r = raw(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0);
    printf("mtime=%ld atime=%ld\n", (long)st.st_mtime, (long)st.st_atime);
    r = raw(SYS_futimes, 999, (long)tv2, 0, 0, 0, 0);
    show("futimes badfd", r);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
  }
  r = raw(SYS_pathconf, (long)W, _PC_NAME_MAX, 0, 0, 0, 0);
  showv("pathconf name_max", r);
  fd = raw(SYS_open, (long)P("f"), O_RDONLY, 0, 0, 0, 0);
  r = raw(SYS_fpathconf, fd, _PC_LINK_MAX, 0, 0, 0, 0);
  showv("fpathconf link_max", r);
  r = raw(SYS_fpathconf, 999, _PC_LINK_MAX, 0, 0, 0, 0);
  show("fpathconf badfd", r);
  {
    struct attrlist al;
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_OBJTYPE;
    struct { uint32_t length; uint32_t type; } out;
    memset(&out, 0, sizeof out);
    r = raw(SYS_fgetattrlist, fd, (long)&al, (long)&out, sizeof out, 0, 0);
    printf("fgetattrlist %s len=%u type=%u\n", g_cs ? "err" : "ok", out.length, out.type);
    memset(&out, 0, sizeof out);
    r = raw(SYS_getattrlistat, AT_FDCWD, (long)P("mkdir"), (long)&al, (long)&out, sizeof out, 0);
    printf("getattrlistat %s len=%u type=%u\n", g_cs ? "err" : "ok", out.length, out.type);
    struct attrlist set;
    memset(&set, 0, sizeof set);
    set.bitmapcount = ATTR_BIT_MAP_COUNT;
    set.commonattr = ATTR_CMN_MODTIME;
    struct timespec when = {1200000000, 0};
    r = raw(SYS_setattrlist, (long)P("hard"), (long)&set, (long)&when, sizeof when, 0, 0);
    show("setattrlist modtime", r);
    r = raw(SYS_stat64, (long)P("f"), (long)&st, 0, 0, 0, 0);
    printf("mtime via link %ld\n", (long)st.st_mtime);
    when.tv_sec = 1210000000;
    r = raw(SYS_setattrlistat, AT_FDCWD, (long)P("f"), (long)&set, (long)&when, sizeof when, 0);
    show("setattrlistat modtime", r);
    r = raw(SYS_stat64, (long)P("f"), (long)&st, 0, 0, 0, 0);
    printf("mtime %ld\n", (long)st.st_mtime);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    fd = raw(SYS_open, (long)P("f"), O_RDWR, 0, 0, 0, 0);
    when.tv_sec = 1220000000;
    r = raw(SYS_fsetattrlist, fd, (long)&set, (long)&when, sizeof when, 0, 0);
    show("fsetattrlist modtime", r);
    r = raw(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0);
    printf("mtime %ld\n", (long)st.st_mtime);
    r = raw(SYS_fgetattrlist, 999, (long)&al, (long)&out, sizeof out, 0, 0);
    show("fgetattrlist badfd", r);
  }
  r = raw(SYS_close, fd, 0, 0, 0, 0, 0);

  {
    struct statfs sf;
    memset(&sf, 0, sizeof sf);
    r = raw(SYS_getfsstat64, 0, 0, MNT_NOWAIT, 0, 0, 0);
    printf("getfsstat64 count>0=%d\n", r > 0);
    long count = r;
    r = raw(SYS_getfsstat64, (long)buf, 0, MNT_NOWAIT, 0, 0, 0);
    struct statfs list[2];
    memset(list, 0, sizeof list);
    r = raw(SYS_getfsstat64, (long)list, sizeof list, MNT_NOWAIT, 0, 0, 0);
    printf("getfsstat64 buf r>0=%d first-type-nonempty=%d\n", r > 0, list[0].f_fstypename[0] != 0);
    r = raw(SYS_getfsstat, 0, 0, MNT_NOWAIT, 0, 0, 0);
    printf("getfsstat legacy count==64bit=%d\n", r == count);
    uint8_t legacy[1024];
    memset(legacy, 0, sizeof legacy);
    r = raw(SYS_statfs, (long)"/tmp", (long)legacy, 0, 0, 0, 0);
    int64_t bsize;
    memcpy(&bsize, legacy + 8, 8);
    printf("statfs legacy %s bsize>0=%d\n", g_cs ? "err" : "ok", bsize > 0);
    r = raw(SYS_statfs, (long)"/nonexistent-x", (long)legacy, 0, 0, 0, 0);
    show("statfs legacy missing", r);
    fd = raw(SYS_open, (long)P("f"), O_RDONLY, 0, 0, 0, 0);
    memset(legacy, 0, sizeof legacy);
    r = raw(SYS_fstatfs, fd, (long)legacy, 0, 0, 0, 0);
    memcpy(&bsize, legacy + 8, 8);
    printf("fstatfs legacy %s bsize>0=%d\n", g_cs ? "err" : "ok", bsize > 0);
    r = raw(SYS_fstatfs, 999, (long)legacy, 0, 0, 0, 0);
    show("fstatfs legacy badfd", r);
    r = raw(SYS_statfs64, (long)W, (long)&sf, 0, 0, 0, 0);
    struct stat info;
    stat(P("f"), &info);
    char out[1024];
    memset(out, 0, sizeof out);
    r = raw(SYS_fsgetpath, (long)out, sizeof out, (long)&sf.f_fsid, (long)info.st_ino, 0, 0);
    printf("fsgetpath %s tail-ok=%d\n", g_cs ? "err" : "ok",
           !g_cs && strstr(out, "sc-fs-work/f") != 0);
    memset(out, 0, sizeof out);
    r = raw(SYS_fsgetpath_ext, (long)out, sizeof out, (long)&sf.f_fsid, (long)info.st_ino, 0, 0);
    printf("fsgetpath_ext %s tail-ok=%d\n", g_cs ? "err" : "ok",
           !g_cs && strstr(out, "sc-fs-work/f") != 0);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    uint32_t objid[2] = {(uint32_t)info.st_ino, 0};
    r = raw(SYS_openbyid_np, (long)&sf.f_fsid, (long)objid, O_RDONLY, 0, 0, 0);
    printf("openbyid_np %s %ld\n", g_cs ? "err" : "ok", g_cs ? g_err : 0L);
    if (!g_cs) raw(SYS_close, r, 0, 0, 0, 0, 0);
  }

  r = raw(SYS_mkdir, (long)P("dirat"), 0755, 0, 0, 0, 0);
  long dirfd = raw(SYS_open, (long)P("dirat"), O_RDONLY | O_DIRECTORY, 0, 0, 0, 0);
  show("open dir", dirfd);
  r = raw(SYS_mkdirat, dirfd, (long)"mkdirat", 0755, 0, 0, 0);
  show("mkdirat", r);
  r = raw(SYS_mkfifoat, dirfd, (long)"fifoat", 0644, 0, 0, 0);
  show("mkfifoat", r);
  r = raw(SYS_mknodat, dirfd, (long)"nodat", S_IFIFO | 0600, 0, 0, 0);
  show("mknodat", r);
  fd = raw(SYS_openat, dirfd, (long)"inner", O_CREAT | O_WRONLY, 0644, 0, 0);
  r = raw(SYS_write, fd, (long)"inner", 5, 0, 0, 0);
  r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
  r = raw(SYS_symlinkat, (long)"inner", dirfd, (long)"symat", 0, 0, 0);
  show("symlinkat", r);
  memset(buf, 0, sizeof buf);
  r = raw(SYS_readlinkat, dirfd, (long)"symat", (long)buf, sizeof buf, 0, 0);
  printf("readlinkat %ld [%s]\n", r, buf);
  r = raw(SYS_linkat, dirfd, (long)"inner", dirfd, (long)"linkat", 0, 0);
  show("linkat", r);
  r = raw(SYS_renameat, dirfd, (long)"linkat", dirfd, (long)"renamed", 0, 0);
  show("renameat", r);
  r = raw(SYS_renameatx_np, dirfd, (long)"renamed", dirfd, (long)"inner", 4, 0);
  show("renameatx_np excl", r);
  r = raw(SYS_renameatx_np, dirfd, (long)"renamed", dirfd, (long)"ren2", 4, 0);
  show("renameatx_np move", r);
  r = raw(SYS_faccessat, dirfd, (long)"inner", R_OK, 0, 0, 0);
  show("faccessat", r);
  r = raw(SYS_faccessat, dirfd, (long)"nope", R_OK, 0, 0, 0);
  show("faccessat missing", r);
  r = raw(SYS_fchmodat, dirfd, (long)"inner", 0640, 0, 0, 0);
  show("fchmodat", r);
  r = raw(SYS_fchownat, dirfd, (long)"inner", (long)getuid(), (long)getgid(), 0, 0);
  show("fchownat", r);
  r = raw(SYS_fchflagsat, dirfd, (long)"inner", 0, 0, 0, 0);
  show("fchflagsat", r);
  r = raw(SYS_fstatat64, dirfd, (long)"inner", (long)&st, 0, 0, 0);
  printf("fstatat64 %s size=%lld mode=%o\n", g_cs ? "err" : "ok", (long long)st.st_size, st.st_mode & 0777);
  {
    uint8_t legacy[256];
    memset(legacy, 0, sizeof legacy);
    r = raw(SYS_fstatat, dirfd, (long)"inner", (long)legacy, 0, 0, 0);
    int64_t size;
    memcpy(&size, legacy + 72, 8);
    printf("fstatat legacy %s size=%lld\n", g_cs ? "err" : "ok", (long long)size);
  }
  r = raw(SYS_fstatat64, 999, (long)"inner", (long)&st, 0, 0, 0);
  show("fstatat64 badfd", r);
  r = raw(SYS_clonefileat, dirfd, (long)"inner", dirfd, (long)"clone", 0, 0);
  show("clonefileat", r);
  fd = raw(SYS_openat, dirfd, (long)"inner", O_RDONLY, 0, 0, 0);
  r = raw(SYS_fclonefileat, fd, dirfd, (long)"f", 0, 0, 0);
  show("fclonefileat", r);
  r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
  {
    struct attrlist al;
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME;
    static char bulk[8192];
    r = raw(SYS_getattrlistbulk, dirfd, (long)&al, (long)bulk, sizeof bulk, 0, 0);
    printf("getattrlistbulk %s entries=%ld\n", g_cs ? "err" : "ok", g_cs ? 0L : r);
  }
  {
    static char dents[8192];
    char *names[64];
    int n = 0;
    long base = 0;
    r = raw(SYS_lseek, dirfd, 0, SEEK_SET, 0, 0, 0);
    r = raw(SYS_getdirentries64, dirfd, (long)dents, sizeof dents, (long)&base, 0, 0);
    printf("getdirentries64 %s\n", g_cs ? "err" : "ok");
    for (long off = 0; !g_cs && off < r && n < 64;) {
      uint16_t reclen, namlen;
      memcpy(&reclen, dents + off + 16, 2);
      memcpy(&namlen, dents + off + 18, 2);
      names[n++] = dents + off + 21;
      off += reclen;
    }
    for (int i = 1; i < n; i++) {
      char *key = names[i];
      int j = i - 1;
      while (j >= 0 && strcmp(names[j], key) > 0) {
        names[j + 1] = names[j];
        j--;
      }
      names[j + 1] = key;
    }
    for (int i = 0; i < n; i++) printf("  entry %s\n", names[i]);
    r = raw(SYS_lseek, dirfd, 0, SEEK_SET, 0, 0, 0);
    long legacyBase = 0;
    r = raw(SYS_getdirentries, dirfd, (long)dents, sizeof dents, (long)&legacyBase, 0, 0);
    printf("getdirentries legacy %s\n", g_cs ? "err" : "ok");
    if (g_cs) printf("  legacy errno=%ld\n", g_err);
  }
  r = raw(SYS_unlinkat, dirfd, (long)"mkdirat", AT_REMOVEDIR, 0, 0, 0);
  show("unlinkat removedir", r);
  r = raw(SYS_unlinkat, dirfd, (long)"nope", 0, 0, 0, 0);
  show("unlinkat missing", r);
  r = raw(SYS_getdirentries64, 999, (long)buf, sizeof buf, 0, 0, 0);
  show("getdirentries64 badfd", r);
  r = raw(SYS_close, dirfd, 0, 0, 0, 0, 0);

  {
    const long none = (long)getuid();
    const long gnone = (long)getgid();
    r = raw(SYS_mkdir_extended, (long)P("mkdir_ext"), none, gnone, 0755, 0, 0);
    show("mkdir_extended", r);
    r = raw(SYS_mkfifo_extended, (long)P("mkfifo_ext"), none, gnone, 0644, 0, 0);
    show("mkfifo_extended", r);
    fd = raw(SYS_open_extended, (long)P("ext"), O_CREAT | O_WRONLY, none, gnone, 0644, 0);
    show("open_extended", fd);
    r = raw(SYS_write, fd, (long)"ext", 3, 0, 0, 0);
    showv("write ext", r);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    uint8_t legacy[256];
    memset(legacy, 0, sizeof legacy);
    r = raw(SYS_stat_extended, (long)P("ext"), (long)legacy, 0, 0, 0, 0);
    int64_t size;
    memcpy(&size, legacy + 72, 8);
    printf("stat_extended %s size=%lld\n", g_cs ? "err" : "ok", (long long)size);
    r = raw(SYS_lstat_extended, (long)P("ext"), (long)legacy, 0, 0, 0, 0);
    show("lstat_extended", r);
    fd = raw(SYS_open, (long)P("ext"), O_RDONLY, 0, 0, 0, 0);
    r = raw(SYS_fstat_extended, fd, (long)legacy, 0, 0, 0, 0);
    show("fstat_extended", r);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    r = raw(SYS_stat_extended, (long)P("missing"), (long)legacy, 0, 0, 0, 0);
    show("stat_extended missing", r);
    struct { uint32_t offset; int flags; int pad[2]; char name[40]; } entries;
    memset(&entries, 0, sizeof entries);
    entries.offset = 16;
    entries.flags = R_OK;
    strcpy(entries.name, P("ext"));
    int results[4] = {-9, -9, -9, -9};
    r = raw(SYS_access_extended, (long)&entries, 16 + strlen(entries.name) + 1, (long)results,
            (long)getuid(), 0, 0);
    printf("access_extended %s result0=%d\n", g_cs ? "err" : "ok", results[0]);
  }
  {
    r = raw(SYS_copyfile, (long)P("f"), (long)P("copy"), 0644, 0, 0, 0);
    show("copyfile", r);
    fd = raw(SYS_open, (long)P("f2"), O_CREAT | O_WRONLY, 0644, 0, 0, 0);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    fd = raw(SYS_open, (long)P("f3"), O_CREAT | O_WRONLY, 0644, 0, 0, 0);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    r = raw(SYS_exchangedata, (long)P("f2"), (long)P("f3"), 0, 0, 0, 0);
    show("exchangedata", r);
    r = raw(SYS_delete, (long)P("f3"), 0, 0, 0, 0, 0);
    show("delete", r);
    r = raw(SYS_undelete, (long)P("f3"), 0, 0, 0, 0, 0);
    show("undelete", r);
    r = raw(SYS_chroot, (long)"/tmp", 0, 0, 0, 0, 0);
    show("chroot", r);
    fd = raw(SYS_open, (long)P("rev"), O_CREAT | O_WRONLY, 0644, 0, 0, 0);
    r = raw(SYS_close, fd, 0, 0, 0, 0, 0);
    r = raw(SYS_revoke, (long)P("rev"), 0, 0, 0, 0, 0);
    show("revoke regular", r);
    r = raw(SYS_open, (long)P("missing"), O_RDONLY, 0, 0, 0, 0);
    show("open missing", r);
    r = raw(SYS_write, 999, (long)"x", 1, 0, 0, 0);
    show("write badfd", r);
    r = raw(SYS_read, 999, (long)buf, 1, 0, 0, 0);
    show("read badfd", r);
  }
  {
    r = raw(SYS_mkdir, 0, 0755, 0, 0, 0, 0);
    show("mkdir null path", r);
    r = raw(SYS_mkdir, 8, 0755, 0, 0, 0, 0);
    show("mkdir low path", r);
    r = raw(SYS_link, 8, (long)P("x"), 0, 0, 0, 0);
    show("link low path", r);
    r = raw(SYS_readlink, (long)P("sym"), 8, 16, 0, 0, 0);
    show("readlink low buf", r);
    r = raw(SYS_stat64, (long)P("f"), 8, 0, 0, 0, 0);
    show("stat64 low out", r);
    r = raw(SYS_open, (long)P("f"), O_CREAT | O_EXCL | O_WRONLY, 0644, 0, 0, 0);
    show("open excl existing", r);
    r = raw(SYS_utimes, 8, 0, 0, 0, 0, 0);
    show("utimes low path", r);
    r = raw(SYS_getdirentries64, 999, 8, 16, 0, 0, 0);
    show("getdirentries64 badfd", r);
    long keep = raw(SYS_dup, 1, 0, 0, 0, 0, 0);
    show("dup stdout", keep);
    fd = raw(SYS_open, (long)P("f2"), O_CREAT | O_RDWR | O_TRUNC, 0644, 0, 0, 0);
    long moved = raw(SYS_dup2, fd, 1, 0, 0, 0, 0);
    raw(SYS_write, 1, (long)"redirected", 10, 0, 0, 0);
    raw(SYS_dup2, keep, 1, 0, 0, 0, 0);
    raw(SYS_close, keep, 0, 0, 0, 0, 0);
    printf("dup2 onto stdout returned stdout=%d\n", moved == 1);
    memset(buf, 0, sizeof buf);
    r = raw(SYS_pread_nocancel, fd, (long)buf, 64, 0, 0, 0);
    printf("redirected content %ld [%s]\n", r, buf);
    raw(SYS_close, fd, 0, 0, 0, 0, 0);
    r = raw(SYS_write, 1, (long)"", 0, 0, 0, 0);
    show("stdout still open", r);
  }

  cleanup();
  puts("done");
  return 0;
}
