#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <string.h>
static long rawsc5(long n, long a, long b, long c, long d, long e, int *cs) {
  register long x0 __asm__("x0") = a; register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c; register long x3 __asm__("x3") = d; register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = 0; register long x16 __asm__("x16") = n; long carry;
  __asm__ volatile("svc #0x80\n cset %1, cs" : "+r"(x0), "=r"(carry) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16) : "memory", "cc");
  *cs = (int)carry; return x0;
}
static long rawsc(long n, long a, long b, long c, long d, int *cs) { return rawsc5(n, a, b, c, d, 0, cs); }
int main(void) {
  int cs; long r;
  r = rawsc(12, (long)"/nonexistent-x", 0, 0, 0, &cs); printf("chdir missing r=%ld carry=%d\n", r, cs);
  r = rawsc(12, (long)"/tmp", 0, 0, 0, &cs); printf("chdir ok r=%ld carry=%d\n", r, cs);
  r = rawsc(463, -2, (long)"/tmp/raw-fixture.bin", O_CREAT | O_WRONLY | O_TRUNC, 0600, &cs);
  printf("openat carry=%d ok=%d\n", cs, r >= 0);
  int fd = (int)r;
  { long d = rawsc5(216, (long)"/tmp/raw-fixture2.bin", O_CREAT | O_WRONLY | O_TRUNC, 4, 0, 0600, &cs); printf("open_dprotected carry=%d ok=%d\n", cs, d >= 0); if (d >= 0) { struct stat ds; fstat((int)d, &ds); printf("dprot mode %o\n", ds.st_mode & 0777); } }
  { long args[6]; char val[16] = "v1"; char got[16] = {0}; char names[128] = {0};
    r = rawsc5(237, fd, (long)"user.fixture", (long)val, 2, 0, &cs); printf("fsetxattr r=%ld carry=%d\n", r, cs);
    r = rawsc5(234, (long)"/tmp/raw-fixture.bin", (long)"user.fixture", (long)got, 16, 0, &cs); printf("getxattr r=%ld carry=%d val=%s\n", r, cs, got);
    r = rawsc5(235, fd, (long)"user.fixture", 0, 0, 0, &cs); printf("fgetxattr size r=%ld carry=%d\n", r, cs);
    r = rawsc(240, (long)"/tmp/raw-fixture.bin", (long)names, 128, 0, &cs); printf("listxattr r=%ld carry=%d first=%s\n", r, cs, names);
    r = rawsc(239, fd, (long)"user.fixture", 0, 0, &cs); printf("fremovexattr r=%ld carry=%d\n", r, cs);
    r = rawsc(238, (long)"/tmp/raw-fixture.bin", (long)"user.fixture", 0, 0, &cs); printf("removexattr again r=%ld carry=%d\n", r, cs);
    r = rawsc5(234, (long)"/tmp/raw-fixture.bin", (long)"user.fixture", (long)got, 16, 0, &cs); printf("getxattr gone r=%ld carry=%d\n", r, cs);
    (void)args; }
  { errno = 0; int pc = fcntl(fd, F_GETPROTECTIONCLASS); printf("protclass r=%d errno=%d\n", pc, pc < 0 ? errno : 0); errno = 0; pc = fcntl(fd, F_SETPROTECTIONCLASS, 3); printf("setprotclass r=%d errno=%d\n", pc, pc < 0 ? errno : 0); pc = fcntl(fd, F_NOCACHE, 1); printf("nocache r=%d\n", pc); errno = 0; pc = fcntl(fd, F_SINGLE_WRITER, 1); printf("single_writer r=%d errno=%d\n", pc, pc < 0 ? errno : 0); }
  r = rawsc(35, fd, 0, 0, 0, &cs); printf("fchflags r=%ld carry=%d\n", r, cs);
  r = rawsc(35, 999, 0, 0, 0, &cs); printf("fchflags badfd r=%ld carry=%d\n", r, cs);
  r = rawsc5(283, fd, 0xfffffffe, 0xfffffffe, 0640, 0, &cs); printf("fchmod_ext r=%ld carry=%d\n", r, cs);
  { struct stat fs; fstat(fd, &fs); printf("mode after %o\n", fs.st_mode & 0777); }
  r = rawsc5(283, 999, 0xfffffffe, 0xfffffffe, 0640, 0, &cs); printf("fchmod_ext badfd r=%ld carry=%d\n", r, cs);
  { char got[8] = {0}; r = rawsc5(154, fd, (long)"hello", 5, 2, 0, &cs); printf("pwrite r=%ld carry=%d\n", r, cs);
    long rd = rawsc(463, -2, (long)"/tmp/raw-fixture.bin", O_RDONLY, 0, &cs);
    r = rawsc5(153, rd, (long)got, 5, 2, 0, &cs); printf("pread r=%ld carry=%d got=%s\n", r, cs, got);
    r = rawsc5(154, rd, (long)"x", 1, 0, 0, &cs); printf("pwrite rdonly r=%ld carry=%d\n", r, cs);
    r = rawsc5(153, 999, (long)got, 5, 2, 0, &cs); printf("pread badfd r=%ld carry=%d\n", r, cs); }
  { struct stat cs2; r = rawsc(124, fd, 0640, 0, 0, &cs); fstat(fd, &cs2); printf("fchmod r=%ld carry=%d mode=%o\n", r, cs, cs2.st_mode & 0777);
    r = rawsc(123, fd, (long)getuid(), (long)getgid(), 0, &cs); printf("fchown self r=%ld carry=%d\n", r, cs);
    r = rawsc(123, fd, 0, 0, 0, &cs); printf("fchown root r=%ld carry=%d\n", r, cs);
    r = rawsc(15, (long)"/tmp/raw-fixture.bin", 0600, 0, 0, &cs); printf("chmod r=%ld carry=%d\n", r, cs);
    r = rawsc(16, (long)"/tmp/raw-fixture.bin", (long)getuid(), (long)getgid(), 0, &cs); printf("chown r=%ld carry=%d\n", r, cs);
    r = rawsc(364, (long)"/nonexistent-x", 0, 0, 0, &cs); printf("lchown missing r=%ld carry=%d\n", r, cs);
    r = rawsc(124, 999, 0600, 0, 0, &cs); printf("fchmod badfd r=%ld carry=%d\n", r, cs); }
  { r = rawsc(128, (long)"/tmp/raw-fixture.bin", (long)"/tmp/raw-fixture3.bin", 0, 0, &cs); printf("rename r=%ld carry=%d\n", r, cs);
    r = rawsc(128, (long)"/nonexistent-x", (long)"/tmp/raw-fixture4.bin", 0, 0, &cs); printf("rename missing r=%ld carry=%d\n", r, cs);
    r = rawsc(10, (long)"/tmp/raw-fixture3.bin", 0, 0, 0, &cs); printf("unlink r=%ld carry=%d\n", r, cs);
    r = rawsc(10, (long)"/tmp/raw-fixture3.bin", 0, 0, 0, &cs); printf("unlink again r=%ld carry=%d\n", r, cs);
    r = rawsc(137, (long)"/tmp", 0, 0, 0, &cs); printf("rmdir nonempty r=%ld carry=%d\n", r, cs); }
  r = rawsc(13, fd, 0, 0, 0, &cs); printf("fchdir file r=%ld carry=%d\n", r, cs);

  struct stat st; memset(&st, 0, sizeof st); size_t xsz = 1024; char xbuf[1024];
  r = rawsc(342, (long)"/etc/hosts", (long)&st, (long)xbuf, (long)&xsz, &cs);
  printf("lstat_ext r=%ld carry=%d size=%lld mode=%o xsz_noacl=%d\n", r, cs, (long long)st.st_size, st.st_mode, xsz == (size_t)-1);
  r = rawsc(342, (long)"/nonexistent-x", (long)&st, 0, 0, &cs); printf("lstat_ext missing r=%ld carry=%d\n", r, cs);
  struct statfs sf; memset(&sf, 0, sizeof sf);
  r = rawsc(345, (long)"/tmp", (long)&sf, 0, 0, &cs); printf("statfs r=%ld carry=%d bsize=%u type=%u mnt=%s\n", r, cs, sf.f_bsize, sf.f_type, sf.f_mntonname);
  r = rawsc(345, (long)"/nonexistent-x", (long)&sf, 0, 0, &cs); printf("statfs missing r=%ld carry=%d\n", r, cs);
  uint32_t out[4];
  __asm__ volatile("mvni v0.4s, #1\n str q0, [%0]" :: "r"(out) : "v0", "memory");
  printf("mvni %08x %08x\n", out[0], out[3]);
  __asm__ volatile("mvni v0.4s, #2, msl #8\n str q0, [%0]" :: "r"(out) : "v0", "memory");
  printf("mvni msl %08x\n", out[1]);
  __asm__ volatile("mvni v0.8h, #3, lsl #8\n str q0, [%0]" :: "r"(out) : "v0", "memory");
  printf("mvni16 %08x\n", out[2]);
  __asm__ volatile("movi v0.4s, #4, msl #16\n str q0, [%0]" :: "r"(out) : "v0", "memory");
  printf("movi msl %08x\n", out[0]);
  return 0;
}
