#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t add_x_shifted(void);
extern uint64_t sub_x_shifted(void);
extern uint64_t add_w_wrap(void);
extern uint64_t sub_w_shifted(void);
extern uint64_t add_sub_preserve_nzcv(void);

int main(void) {
  printf("add=%" PRIx64 ",%" PRIx64 " sub=%" PRIx64 ",%" PRIx64
         " nzcv=%" PRIx64 "\n",
         add_x_shifted(), add_w_wrap(), sub_x_shifted(), sub_w_shifted(),
         add_sub_preserve_nzcv());
  return 0;
}
