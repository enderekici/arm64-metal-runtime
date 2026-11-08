#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t adds_x_overflow_flags(uint64_t value);
extern uint64_t subs_x_underflow_flags(uint64_t value);
extern uint64_t cmp_x_equal_flags(uint64_t value);
extern uint64_t cmn_x_carry_flags(uint64_t value);
extern uint64_t adds_w_carry_flags(uint32_t value);
extern uint64_t subs_w_underflow_flags(uint32_t value);
extern uint64_t cmp_sp_zero_flags(void);
extern uint64_t adds_w_zero_extend(uint32_t value);
extern void sp_update(uint64_t *values);
extern uint64_t wsp_zero_extend(void);

int main(void) {
  const uint64_t adds_x = adds_x_overflow_flags(UINT64_C(0x7fffffffffffffff));
  const uint64_t subs_x = subs_x_underflow_flags(0);
  const uint64_t cmp_x = cmp_x_equal_flags(0x1000);
  const uint64_t cmn_x = cmn_x_carry_flags(UINT64_MAX - UINT64_C(0xfff));
  const uint64_t adds_w = adds_w_carry_flags(UINT32_MAX);
  const uint64_t subs_w = subs_w_underflow_flags(0);
  const uint64_t cmp_sp = cmp_sp_zero_flags();
  const uint64_t word_result = adds_w_zero_extend(UINT32_MAX);
  uint64_t sp_values[2] = {};
  sp_update(sp_values);
  const uint64_t wsp = wsp_zero_extend();
  printf("adds=%" PRIx64 ",%" PRIx64 " subs=%" PRIx64 ",%" PRIx64
         " cmp=%" PRIx64 " cmn=%" PRIx64 " cmpsp=%" PRIx64
         " wz=%" PRIx64 " sp=%" PRIx64 ",%" PRIx64 ",%" PRIx64 "\n",
         adds_x, adds_w, subs_x, subs_w, cmp_x, cmn_x, cmp_sp, word_result,
         sp_values[0], sp_values[1], wsp);
  return adds_x == UINT64_C(0x90000000) &&
                 adds_w == UINT64_C(0x60000000) &&
                 subs_x == UINT64_C(0x80000000) &&
                 subs_w == UINT64_C(0x80000000) &&
                 cmp_x == UINT64_C(0x60000000) &&
                 cmn_x == UINT64_C(0x60000000) &&
                 cmp_sp == UINT64_C(0x20000000) && word_result == 0 &&
                 sp_values[0] == UINT64_C(0x2000) &&
                 sp_values[1] == UINT64_MAX - UINT64_C(4095) &&
                 wsp == UINT64_C(0x80000001)
             ? 0
             : 1;
}
