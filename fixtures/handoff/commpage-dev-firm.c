#include <stdint.h>
#include <stdio.h>

int main(void) {
  const volatile uint32_t *dev_firm =
      (const volatile uint32_t *)(uintptr_t)0xfffff4084ULL;
  printf("dev_firm=%u\n", *dev_firm);
  return 0;
}
