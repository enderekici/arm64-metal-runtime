#include <stdio.h>
#include <string.h>

extern void *__memcpy_chk(void *destination, const void *source,
                          size_t count, size_t destination_size);

static const char source[] = "fortified-copy";
static char destination[8];

int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1) {
    (void)__memcpy_chk(destination, source, 9, sizeof(destination));
    return 90;
  }
  void *result = __memcpy_chk(destination, source, 4, sizeof(destination));
  if (result != destination || destination[0] != 'f' || destination[1] != 'o' ||
      destination[2] != 'r' || destination[3] != 't') {
    return 1;
  }
  puts("copy=fort result=1");
  return 0;
}
