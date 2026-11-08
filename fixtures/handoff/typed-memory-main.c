#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern void typed_memory(uint64_t *values, uint32_t word, uint64_t doubleword);

int main(void) {
  uint64_t values[5] = {0};
  typed_memory(values, UINT32_C(0xaabbccdd), UINT64_C(0x1122334455667788));
  printf("%016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
         " %016" PRIx64 "\n",
         values[0], values[1], values[2], values[3], values[4]);
  return 0;
}
