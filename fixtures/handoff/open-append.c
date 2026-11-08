#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if SYS_open_nocancel != 398
#error "Expected the installed Darwin open_nocancel syscall number"
#endif

struct raw_result {
  uint64_t value;
  uint64_t nzcv;
};

static struct raw_result raw_open_nocancel(const char *path, int flags, mode_t mode) {
  register uint64_t x0 __asm__("x0") = (uint64_t)path;
  register uint64_t x1 __asm__("x1") = (uint32_t)flags;
  register uint64_t x2 __asm__("x2") = mode;
  register uint64_t x16 __asm__("x16") = SYS_open_nocancel;
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %3, nzcv"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "=r"(nzcv)
                   : "r"(x16) : "cc", "memory");
  return (struct raw_result){x0, nzcv};
}

static int check_append(int raw, const char *directory) {
  char path[1024];
  const int path_length = snprintf(path, sizeof(path), "%s/%s", directory,
                                   raw ? "raw-append" : "named-append");
  if (path_length < 0 || (size_t)path_length >= sizeof(path)) return 1;

  int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (descriptor < 0 || write(descriptor, "seed", 4) != 4 || close(descriptor) != 0) {
    return 2;
  }

  int append_descriptor = -1;
  if (raw) {
    const struct raw_result opened = raw_open_nocancel(
        path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (((opened.nzcv >> 29) & 1U) != 0 || (int64_t)opened.value < 0) return 3;
    append_descriptor = (int)opened.value;
  } else {
    append_descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (append_descriptor < 0) return 4;
  }

  const int seek_ok = lseek(append_descriptor, 0, SEEK_SET) == 0;
  const int first_write = write(append_descriptor, "A", 1) == 1;
  const int second_seek = lseek(append_descriptor, 0, SEEK_SET) == 0;
  const int second_write = write(append_descriptor, "B", 1) == 1;
  const off_t final_offset = lseek(append_descriptor, 0, SEEK_CUR);
  if (close(append_descriptor) != 0 || !seek_ok || !first_write || !second_seek ||
      !second_write || final_offset != 6) return 5;

  descriptor = open(path, O_RDONLY);
  if (descriptor < 0) return 6;
  char contents[8] = {0};
  const ssize_t count = read(descriptor, contents, sizeof(contents));
  const int close_ok = close(descriptor) == 0;
  if (!close_ok || count != 6 || memcmp(contents, "seedAB", 6) != 0) return 7;
  return 0;
}

int main(void) {
  char directory[] = "/tmp/arm64-open-append.XXXXXX";
  if (mkdtemp(directory) == NULL) return 1;
  const int named = check_append(0, directory);
  const int raw = check_append(1, directory);
  char named_path[1024];
  char raw_path[1024];
  (void)snprintf(named_path, sizeof(named_path), "%s/named-append", directory);
  (void)snprintf(raw_path, sizeof(raw_path), "%s/raw-append", directory);
  const int cleanup = unlink(named_path) == 0 && unlink(raw_path) == 0 &&
                      rmdir(directory) == 0;
  if (named != 0 || raw != 0 || !cleanup) return 10 + named + raw;
  puts("named=seedAB raw=seedAB offset=6 cleanup=ok");
  return 0;
}
