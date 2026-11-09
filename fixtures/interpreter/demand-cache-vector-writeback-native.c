#include <arm_neon.h>
#include <stdint.h>
#include <zlib.h>

int main(void) {
  const uint8_t *base = (const uint8_t *)(uintptr_t)zlibVersion;
  const uint8_t *cursor = base;
  uint8x16_t value;
#if VECTOR_PRE
  __asm__ volatile("ldr %q0, [%1, #16]!"
                   : "=w"(value), "+r"(cursor)
                   :
                   : "memory");
#else
  __asm__ volatile("ldr %q0, [%1], #16"
                   : "=w"(value), "+r"(cursor)
                   :
                   : "memory");
#endif
  if (cursor != base + 16) return 99;
  return ((const uint8_t *)&value)[0];
}
