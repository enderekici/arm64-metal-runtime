#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  const unsigned int narrow = 0x0a;
  const unsigned long long wide = UINT64_C(0x0123456789abcdef);
  const size_t size_value = (size_t)UINT64_C(0x100000005);
  printf("%d %d %d %d %d %d|%02x|%#llx|%zu\n",
         1, 2, 3, 4, 5, 6, narrow, wide, size_value);
  return 0;
}
