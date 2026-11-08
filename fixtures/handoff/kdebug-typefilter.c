#include <errno.h>
#include <stdint.h>
#include <stdio.h>

static unsigned invoke(void **address, void *size, unsigned *carry) {
  register uintptr_t x0 __asm__("x0") = (uintptr_t)address;
  register uintptr_t x1 __asm__("x1") = (uintptr_t)size;
  register uintptr_t x16 __asm__("x16") = 177;
  unsigned c;
  __asm__ volatile("svc #0x80\ncset %w0, cs" : "=&r"(c), "+r"(x0)
                   : "r"(x1), "r"(x16) : "memory", "cc");
  *carry = c;
  return (unsigned)x0;
}
int main(void) {
  unsigned carry = 0, result;
  size_t size = 0x2222;
  void *mapping = NULL;
  errno = 123;
  result = invoke(&mapping, &size, &carry);
  if (result || carry || !mapping) return 1;
  const unsigned char *bytes = mapping;
  unsigned nonzero = 0;
  for (unsigned i = 0; i < 8192; ++i) nonzero += bytes[i] != 0;
  printf("mapped=%d size_unchanged=%d nonzero=%u errno=%d\n",
         mapping != NULL, size == 0x2222, nonzero, errno);
  result = invoke(&mapping, NULL, &carry);
  printf("null=%u carry=%u\n", result, carry);
  result = invoke((void **)(uintptr_t)1, &size, &carry);
  printf("bad=%u carry=%u\n", result, carry);
  return 0;
}
