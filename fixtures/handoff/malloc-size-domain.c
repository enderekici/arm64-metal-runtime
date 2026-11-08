#include <malloc/malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  unsigned char *allocation = malloc(16);
  volatile unsigned char stack = 0;
  if (!allocation) return 1;

  const size_t live = malloc_size(allocation);
  const size_t null_pointer = malloc_size(NULL);
  const size_t interior = malloc_size(allocation + 1);
  const size_t stack_pointer = malloc_size((void *)&stack);
  const size_t unknown_low = malloc_size((void *)(uintptr_t)0x1234);
  const size_t unknown_sparse =
      malloc_size((void *)(uintptr_t)UINT64_C(0x75d0c00000));
  free(allocation);

  if (live < 16 || null_pointer != 0 || interior != 0 || stack_pointer != 0 ||
      unknown_low != 0 || unknown_sparse != 0) {
    fprintf(stderr,
            "malloc_size live=%zu null=%zu interior=%zu stack=%zu "
            "unknown_low=%zu unknown_sparse=%zu\n",
            live, null_pointer, interior, stack_pointer, unknown_low,
            unknown_sparse);
    return 2;
  }

  puts("malloc_size-domain live=ok null=0 interior=0 stack=0 "
       "unknown_low=0 unknown_sparse=0");
  return 0;
}
