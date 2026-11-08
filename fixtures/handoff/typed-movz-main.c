#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t movz_x_lsl0(void);
extern uint64_t movz_x_lsl16(void);
extern uint64_t movz_x_lsl32(void);
extern uint64_t movz_x_lsl48(void);
extern uint64_t movz_w_lsl0(void);
extern uint64_t movz_w_lsl16(void);
extern uint64_t movz_w_zero_extends(void);
extern uint64_t movz_discard_xzr(void);
extern uint64_t movz_discard_wzr(void);
extern uint64_t movk_outside_slice(void);

int main(void) {
  printf("x=%" PRIx64 ",%" PRIx64 ",%" PRIx64 ",%" PRIx64
         " w=%" PRIx64 ",%" PRIx64 ",zeroext=%" PRIx64 " discard=%" PRIx64 ",%" PRIx64
         " outside=%" PRIx64 "\n",
         movz_x_lsl0(), movz_x_lsl16(), movz_x_lsl32(), movz_x_lsl48(),
         movz_w_lsl0(), movz_w_lsl16(), movz_w_zero_extends(), movz_discard_xzr(), movz_discard_wzr(),
         movk_outside_slice());
  return 0;
}
