#include <stdio.h>
#include <string.h>

extern void *__memset_chk(void *destination, int value, size_t count,
                          size_t destination_size);

static char destination[8];

int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1) {
    (void)__memset_chk(destination, 'x', 9, sizeof(destination));
    return 90;
  }
  void *result = __memset_chk(destination, 'f', 4, sizeof(destination));
  if (result != destination || destination[0] != 'f' || destination[1] != 'f' ||
      destination[2] != 'f' || destination[3] != 'f' || destination[4] != 0) {
    return 1;
  }
  puts("fill=ffff result=1");
  return 0;
}
