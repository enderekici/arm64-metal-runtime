#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  char valid[] = "/tmp/metal-mkdtemp-contract.XXXXXX";
  char *result = mkdtemp(valid);
  struct stat metadata;
  int mode_ok = result != NULL && stat(valid, &metadata) == 0 &&
                (metadata.st_mode & 0777) == 0700;
  int cleanup_ok = result != NULL && rmdir(valid) == 0;
  char invalid[] = "/tmp/metal-mkdtemp-no-parent-4fa2d136/prefix.XXXXXX";
  errno = 0;
  char *invalid_result = mkdtemp(invalid);
  int invalid_error = errno;
  int invalid_ok = invalid_result == NULL && invalid_error == ENOENT &&
                   access(invalid, F_OK) != 0;
  if (invalid_result != NULL) (void)rmdir(invalid_result);
  printf("valid=%d pointer=%d mode=%d cleanup=%d invalid=%d errno=%d\n",
         result != NULL, result == valid, mode_ok, cleanup_ok, invalid_ok, invalid_error);
  return !(result != NULL && result == valid && mode_ok && cleanup_ok && invalid_ok);
}
