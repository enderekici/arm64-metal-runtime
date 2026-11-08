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

struct raw_result {
  uint64_t value;
  uint64_t nzcv;
};

static struct raw_result raw_open_nocancel(const char *path, int flags, int mode) {
  register uint64_t x0 __asm__("x0") = (uint64_t)path;
  register uint64_t x1 __asm__("x1") = (uint32_t)flags;
  register uint64_t x2 __asm__("x2") = (uint32_t)mode;
  register uint64_t x16 __asm__("x16") = SYS_open_nocancel;
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
  const struct raw_result opened = raw_open_nocancel(argv[1], O_RDONLY, 0);
  const int success_errno = errno;
  if (((opened.nzcv >> 29) & 1U) != 0 || (int64_t)opened.value < 0) return 4;
  char contents[64] = {0};
  const ssize_t length = read((int)opened.value, contents, sizeof(contents) - 1);
  const int read_error = errno;
  const int close_result = close((int)opened.value);

  errno = ERANGE;
  const struct raw_result missing = raw_open_nocancel(missing_path, O_RDONLY, 0);
  const int failure_errno = errno;
  const int missing_error = (int)missing.value;
  printf("open=%s carry=%llu errno=%d data=%s missing=%d carry=%llu errno=%d\n",
         length > 0 && strcmp(contents, "open-nocancel-fixture\n") == 0 ? "ok" : "bad",
         (unsigned long long)((opened.nzcv >> 29) & 1U), success_errno,
         contents, missing_error, (unsigned long long)((missing.nzcv >> 29) & 1U),
         failure_errno);
  return success_errno != EDOM || read_error != EDOM || close_result != 0 ||
         failure_errno != ERANGE || ((missing.nzcv >> 29) & 1U) == 0 ||
         missing_error != ENOENT || length != sizeof("open-nocancel-fixture\n") - 1 ||
         strcmp(contents, "open-nocancel-fixture\n") != 0;
}
