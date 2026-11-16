/*
 * Freestanding native-vs-guest raw BSD descriptor contract.
 *
 * Supply an absolute, read-only file whose contents begin with "native" and
 * are exactly 18 bytes. Compile without the CRT or libc calls:
 *
 * xcrun clang -arch arm64 -ffreestanding -fno-builtin -fno-stack-protector \
 *   -nostdlib -Wl,-e,_start -lSystem -Wall -Wextra -Werror \
 *   -DPROBE_PATH='"/tmp/fd-contract-input"' \
 *   fixtures/handoff/raw-fd-lifecycle-contract.c -o /tmp/fd-contract
 */
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#ifndef PROBE_PATH
#error "Compile with -DPROBE_PATH='\"/absolute/readonly/path\"'"
#endif

struct raw_result {
  uint64_t value;
  uint64_t carry;
};

static struct raw_result syscall6(long number, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  register uint64_t x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  register uint64_t x4 __asm__("x4") = a4;
  register uint64_t x5 __asm__("x5") = a5;
  register uint64_t x16 __asm__("x16") = (uint64_t)number;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %6, nzcv"
      : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "=r"(nzcv)
      : "r"(x16) : "memory", "cc");
  return (struct raw_result){x0, (nzcv >> 29) & 1U};
}

static struct raw_result syscall3(long number, uint64_t a0, uint64_t a1,
                                  uint64_t a2) {
  return syscall6(number, a0, a1, a2, 0, 0, 0);
}

static int returned(struct raw_result result, uint64_t value) {
  return result.carry == 0 && result.value == value;
}

static int failed_with(struct raw_result result, uint64_t error) {
  return result.carry != 0 && result.value == error;
}

static __attribute__((noreturn)) void finish(int status) {
  (void)syscall3(SYS_exit, (uint64_t)status, 0, 0);
  __builtin_unreachable();
}

static void write_line(const char *line, uint64_t size, int error_status) {
  const struct raw_result result = syscall3(SYS_write, 1,
                                             (uint64_t)(uintptr_t)line, size);
  if (!returned(result, size)) finish(error_status);
}

__attribute__((noreturn)) void start(void) {
  static const char frontier[] = "fd-frontier=before-open\n";
  static const char positive[] =
      "fd-positive=dup-shared-offset,fstat-size,close ok\n";
  static const char negative[] =
      "fd-negative=closed-fd fstat,lseek,dup,close => EBADF ok\n";
  static const char path[] = PROBE_PATH;

  struct raw_result result = syscall3(SYS_write, 1,
      (uint64_t)(uintptr_t)frontier, sizeof(frontier) - 1);
  if (!returned(result, sizeof(frontier) - 1)) finish(80);

  result = syscall3(SYS_open, (uint64_t)(uintptr_t)path, O_RDONLY, 0);
  if (result.carry != 0 || (int64_t)result.value < 0) finish(81);
  const uint64_t fd = result.value;

  result = syscall3(SYS_dup, fd, 0, 0);
  if (result.carry != 0 || (int64_t)result.value < 0) finish(82);
  const uint64_t duplicate = result.value;

  char bytes[7] = {0};
  result = syscall3(SYS_read, duplicate, (uint64_t)(uintptr_t)bytes, 6);
  static const char expected[] = "native";
  if (!returned(result, 6)) finish(83);
  for (unsigned index = 0; index < 6; ++index) {
    if (bytes[index] != expected[index]) finish(84);
  }

  result = syscall3(SYS_lseek, fd, 0, SEEK_CUR);
  if (!returned(result, 6)) finish(85);

  struct stat info;
  result = syscall3(SYS_fstat64, fd, (uint64_t)(uintptr_t)&info, 0);
  if (!returned(result, 0) || info.st_size != 18) finish(86);

  result = syscall3(SYS_close, duplicate, 0, 0);
  if (!returned(result, 0)) finish(87);
  result = syscall3(SYS_close, fd, 0, 0);
  if (!returned(result, 0)) finish(88);
  write_line(positive, sizeof(positive) - 1, 89);

  result = syscall3(SYS_fstat64, fd, (uint64_t)(uintptr_t)&info, 0);
  if (!failed_with(result, 9)) finish(91);
  result = syscall3(SYS_lseek, fd, 0, SEEK_SET);
  if (!failed_with(result, 9)) finish(92);
  result = syscall3(SYS_dup, fd, 0, 0);
  if (!failed_with(result, 9)) finish(93);
  result = syscall3(SYS_close, fd, 0, 0);
  if (!failed_with(result, 9)) finish(94);
  write_line(negative, sizeof(negative) - 1, 90);
  finish(0);
}
