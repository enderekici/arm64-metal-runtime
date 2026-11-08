#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern int *chained_weak_pointer;

int main(void) {
  printf("weak_pointer=%" PRIuPTR "\n", (uintptr_t)chained_weak_pointer);
  return (uintptr_t)chained_weak_pointer == 12 ? 0 : 1;
}
