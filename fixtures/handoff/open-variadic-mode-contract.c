#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int open_with_poisoned_x2(const char *path, int flags, mode_t mode) {
  __asm__ volatile("mov x2, #0777" ::: "x2");
  return open(path, flags, mode);
}

int main(void) {
  char directory[] = "/tmp/metal-open-mode.XXXXXX";
  if (mkdtemp(directory) == NULL) return 1;
  char path[1024];
  if (snprintf(path, sizeof(path), "%s/new-file", directory) >= (int)sizeof(path)) return 2;
  int descriptor = open_with_poisoned_x2(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  int saved_error = errno;
  struct stat metadata;
  int mode_ok = descriptor >= 0 && fstat(descriptor, &metadata) == 0 &&
                (metadata.st_mode & 0777) == 0600;
  if (descriptor >= 0) (void)close(descriptor);
  int cleanup_ok = unlink(path) == 0 && rmdir(directory) == 0;
  printf("created=%d mode=%d cleanup=%d errno=%d\n", descriptor >= 0, mode_ok,
         cleanup_ok, saved_error);
  return !(descriptor >= 0 && mode_ok && cleanup_ok);
}
