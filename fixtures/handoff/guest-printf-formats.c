#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  const unsigned int narrow = 0x0a;
  const unsigned long long wide = UINT64_C(0x0123456789abcdef);
  const size_t size_value = (size_t)UINT64_C(0x100000005);
  const unsigned int zero = 0;
  const int negative = -42;
  printf("%d %d %d %d %d %d|%02x|%#llx|%zu|%#x|%05d\n",
         1, 2, 3, 4, 5, 6, narrow, wide, size_value, zero, negative);
  return 0;
}
