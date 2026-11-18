#include <arm_sme.h>
#include <arm_sve.h>

#include <stdint.h>
#include <stdio.h>

#define STORE_SLICE(T, S)                                                   \
  do {                                                                       \
    svint32_t row = svread_hor_za32_s32_m(svdup_s32(0), pg32, T, S);         \
    svst1_s32(pg32, output + (T) * 256 + (S) * 16, row);                      \
  } while (0)
#define STORE_SLICES(T)                                                      \
  STORE_SLICE(T, 0); STORE_SLICE(T, 1); STORE_SLICE(T, 2); STORE_SLICE(T, 3); \
  STORE_SLICE(T, 4); STORE_SLICE(T, 5); STORE_SLICE(T, 6); STORE_SLICE(T, 7); \
  STORE_SLICE(T, 8); STORE_SLICE(T, 9); STORE_SLICE(T, 10); STORE_SLICE(T, 11); \
  STORE_SLICE(T, 12); STORE_SLICE(T, 13); STORE_SLICE(T, 14); STORE_SLICE(T, 15)

__arm_new("za") static void smopa_oracle(const int8_t *left,
                                           const int8_t *right,
                                           int32_t *output) __arm_streaming {
  svbool_t pleft = svwhilelt_b8((uint64_t)0, (uint64_t)53);
  svbool_t pright = svwhilelt_b8((uint64_t)0, (uint64_t)49);
  svbool_t pg32 = svptrue_b32();
  svint8_t a = svld1_s8(svptrue_b8(), left);
  svint8_t b = svld1_s8(svptrue_b8(), right);
  svzero_za();
  svmopa_za32_s8_m(0, pleft, pright, a, b);
  svmopa_za32_s8_m(1, pleft, pright, a, b);
  svmopa_za32_s8_m(2, pleft, pright, a, b);
  svmopa_za32_s8_m(3, pleft, pright, a, b);
  STORE_SLICES(0); STORE_SLICES(1); STORE_SLICES(2); STORE_SLICES(3);
}

static int expected(const int8_t *left, const int8_t *right,
                    unsigned slice, unsigned column) {
  int32_t sum = 0;
  for (unsigned k = 0; k < 4; ++k) {
    unsigned left_lane = slice * 4 + k;
    unsigned right_lane = column * 4 + k;
    if (left_lane < 53 && right_lane < 49) {
      sum += (int32_t)left[left_lane] * (int32_t)right[right_lane];
    }
  }
  return sum;
}

int main(void) {
  if (!__arm_has_sme()) return 77;
  int8_t left[64], right[64];
  int32_t output[4 * 16 * 16] = {0};
  for (unsigned i = 0; i < 64; ++i) {
    left[i] = (int8_t)((int)((i * 37u + 11u) % 127u) - 63);
    right[i] = (int8_t)((int)((i * 19u + 7u) % 127u) - 63);
  }
  smopa_oracle(left, right, output);
  for (unsigned tile = 0; tile < 4; ++tile) {
    for (unsigned slice = 0; slice < 16; ++slice) {
      for (unsigned column = 0; column < 16; ++column) {
        int actual = output[tile * 256 + slice * 16 + column];
        int want = expected(left, right, slice, column);
        if (actual != want) {
          fprintf(stderr, "SMOPA tile=%u slice=%u column=%u got=%d want=%d\n",
                  tile, slice, column, actual, want);
          return 1;
        }
      }
    }
  }
  puts("SMOPA signed byte masks, four ZA tiles, and all 16 slices match scalar oracle");
  return 0;
}
