#include <stdio.h>

extern int chained_addend_target[];
extern int *chained_addend_pointer;

int main(void) {
  printf("marker=%d offset=%td\n", *chained_addend_pointer,
         chained_addend_pointer - chained_addend_target);
  return 0;
}
