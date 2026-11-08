#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#if SYS_open_nocancel != 398
#error "Expected the installed Darwin open_nocancel syscall number"
#endif
#if SYS_read_nocancel != 396 || SYS_write_nocancel != 397 || SYS_close_nocancel != 399
#error "Expected the installed Darwin nocancel file syscall numbers"
#endif

struct raw_result {
  uint64_t value;
  uint64_t nzcv;
};

static struct raw_result raw_file_call(uint64_t number, uint64_t arg0,
                                       uint64_t arg1, uint64_t arg2) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = arg1;
  register uint64_t x2 __asm__("x2") = arg2;
  register uint64_t x16 __asm__("x16") = number;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %3, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, nzcv};
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char missing_path[4096];
  const int path_length = snprintf(missing_path, sizeof(missing_path), "%s.missing", argv[1]);
  if (path_length < 0 || (size_t)path_length >= sizeof(missing_path)) return 3;

  errno = EDOM;
  const struct raw_result opened = raw_file_call(SYS_open_nocancel,
      (uint64_t)argv[1], (uint32_t)O_RDONLY, 0);
  const int success_errno = errno;
  char contents[64] = {0};
  errno = EDOM;
  const struct raw_result read_result = raw_file_call(SYS_read_nocancel,
      opened.value, (uint64_t)contents, sizeof(contents) - 1);
  const int read_error = errno;
  const size_t length = (size_t)read_result.value;
  errno = ERANGE;
  const struct raw_result eof = raw_file_call(SYS_read_nocancel,
      opened.value, (uint64_t)contents + length, sizeof(contents) - 1 - length);
  const int eof_errno = errno;
  errno = E2BIG;
  const struct raw_result closed = raw_file_call(SYS_close_nocancel, opened.value, 0, 0);
  const int close_errno = errno;
  errno = EBUSY;
  const struct raw_result bad_read = raw_file_call(SYS_read_nocancel,
      opened.value, (uint64_t)contents, 1);
  const int bad_read_errno = errno;
  errno = ECHILD;
  const struct raw_result bad_close = raw_file_call(SYS_close_nocancel,
      opened.value, 0, 0);
  const int bad_close_errno = errno;

  errno = ERANGE;
  const struct raw_result missing = raw_file_call(SYS_open_nocancel,
      (uint64_t)missing_path, (uint32_t)O_RDONLY, 0);
  const int failure_errno = errno;
  const int missing_error = (int)missing.value;
  static const char write_marker[] = "nocancel-write=ok\n";
  errno = EDOM;
  const struct raw_result written = raw_file_call(SYS_write_nocancel,
      STDOUT_FILENO, (uint64_t)write_marker, sizeof(write_marker) - 1);
  const int write_errno = errno;
  printf("open=%s carry=%llu errno=%d data=%s missing=%d carry=%llu errno=%d\n",
         length > 0 && strcmp(contents, "open-nocancel-fixture\n") == 0 &&
             ((read_result.nzcv >> 29) & 1U) == 0 ? "ok" : "bad",
         (unsigned long long)((opened.nzcv >> 29) & 1U), success_errno,
         contents, missing_error, (unsigned long long)((missing.nzcv >> 29) & 1U),
         failure_errno);
  return ((opened.nzcv >> 29) & 1U) != 0 || (int64_t)opened.value < 0 ||
         success_errno != EDOM || ((read_result.nzcv >> 29) & 1U) != 0 ||
         read_error != EDOM || ((eof.nzcv >> 29) & 1U) != 0 || eof.value != 0 ||
         eof_errno != ERANGE || ((closed.nzcv >> 29) & 1U) != 0 || closed.value != 0 ||
         close_errno != E2BIG || ((bad_read.nzcv >> 29) & 1U) == 0 ||
         bad_read.value != EBADF || bad_read_errno != EBUSY ||
         ((bad_close.nzcv >> 29) & 1U) == 0 || bad_close.value != EBADF ||
         bad_close_errno != ECHILD ||
         failure_errno != ERANGE || ((missing.nzcv >> 29) & 1U) == 0 ||
         missing_error != ENOENT || write_errno != EDOM ||
         ((written.nzcv >> 29) & 1U) != 0 ||
         written.value != sizeof(write_marker) - 1 ||
         length != sizeof("open-nocancel-fixture\n") - 1 ||
         strcmp(contents, "open-nocancel-fixture\n") != 0;
}
