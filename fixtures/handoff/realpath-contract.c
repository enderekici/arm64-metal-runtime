#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 3) return 2;

#ifdef REALPATH_GUEST_INVALID_OUTPUT
  errno = 0;
  if (realpath(argv[1], (char *)1) != NULL || errno != EFAULT) return 9;
  printf("invalid_output_errno=%d\n", errno);
  return 0;
#else
  char caller[PATH_MAX];
  char *allocated = realpath(argv[1], NULL);
  if (allocated == NULL) return 3;
  if (realpath(argv[1], caller) != caller || strcmp(allocated, caller) != 0) {
    free(allocated);
    return 4;
  }
  printf("allocated=%s\ncaller=%s\n", allocated, caller);
  free(allocated);

  errno = 0;
  if (realpath(argv[2], NULL) != NULL || errno != ENOENT) return 5;
  printf("missing_errno=%d\n", errno);
  errno = 0;
  if (realpath(argv[2], caller) != NULL || errno != ENOENT) return 6;
  printf("missing_caller_errno=%d\n", errno);

  errno = 0;
  if (realpath(NULL, caller) != NULL || errno != EINVAL) return 7;
  printf("null_input_errno=%d\n", errno);
  return 0;
#endif
}
