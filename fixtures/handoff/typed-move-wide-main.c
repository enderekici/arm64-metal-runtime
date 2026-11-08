#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t movn_x(void);
extern uint64_t movn_w(void);
extern uint64_t movk_x(void);
extern uint64_t movk_w(void);
extern uint64_t movk_discard_xzr(void);
extern uint64_t movk_discard_wzr(void);

int main(void) {
  printf("movn=%" PRIx64 ",%" PRIx64 " movk=%" PRIx64 ",%" PRIx64
         " discard=%" PRIx64 ",%" PRIx64 "\n",
         movn_x(), movn_w(), movk_x(), movk_w(),
         movk_discard_xzr(), movk_discard_wzr());
  return 0;
}
