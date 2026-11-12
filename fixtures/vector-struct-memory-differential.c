#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long long u64;
static u64 st = 0x13198a2e03707344ull;
static u64 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static u64 h = 1469598103934665603ull;
static void mix(u64 v) { h ^= v; h *= 1099511628211ull; }
static int dump;
typedef struct { u64 reg[8]; u64 base; unsigned char buf[512]; } State;
typedef void (*Fn)(State *, u64);
#define OP(nm, ins) static void f_##nm(State *s, u64 inc) { \
  u64 *r = s->reg; unsigned char *b = s->buf + 192; u64 post; \
  __asm__ volatile("ldp q4, q5, [%[r]]\n ldp q6, q7, [%[r], #32]\n mov x9, %[b]\n mov x10, %[inc]\n " ins \
                   "\n mov %[post], x9\n stp q4, q5, [%[r]]\n stp q6, q7, [%[r], #32]" \
                   : [post] "=r"(post) : [r] "r"(r), [b] "r"(b), [inc] "r"(inc) \
                   : "x9", "x10", "v4", "v5", "v6", "v7", "memory"); \
  s->base = post - (u64)b; }
OP(ld2_8b, "ld2 {v4.8b, v5.8b}, [x9]")
OP(ld2_8b_pi, "ld2 {v4.8b, v5.8b}, [x9], #16")
OP(ld2_8b_pr, "ld2 {v4.8b, v5.8b}, [x9], x10")
OP(st2_8b, "st2 {v4.8b, v5.8b}, [x9]")
OP(st2_8b_pi, "st2 {v4.8b, v5.8b}, [x9], #16")
OP(st2_8b_pr, "st2 {v4.8b, v5.8b}, [x9], x10")
OP(ld2_16b, "ld2 {v4.16b, v5.16b}, [x9]")
OP(ld2_16b_pi, "ld2 {v4.16b, v5.16b}, [x9], #32")
OP(ld2_16b_pr, "ld2 {v4.16b, v5.16b}, [x9], x10")
OP(st2_16b, "st2 {v4.16b, v5.16b}, [x9]")
OP(st2_16b_pi, "st2 {v4.16b, v5.16b}, [x9], #32")
OP(st2_16b_pr, "st2 {v4.16b, v5.16b}, [x9], x10")
OP(ld2_4h, "ld2 {v4.4h, v5.4h}, [x9]")
OP(ld2_4h_pi, "ld2 {v4.4h, v5.4h}, [x9], #16")
OP(ld2_4h_pr, "ld2 {v4.4h, v5.4h}, [x9], x10")
OP(st2_4h, "st2 {v4.4h, v5.4h}, [x9]")
OP(st2_4h_pi, "st2 {v4.4h, v5.4h}, [x9], #16")
OP(st2_4h_pr, "st2 {v4.4h, v5.4h}, [x9], x10")
OP(ld2_8h, "ld2 {v4.8h, v5.8h}, [x9]")
OP(ld2_8h_pi, "ld2 {v4.8h, v5.8h}, [x9], #32")
OP(ld2_8h_pr, "ld2 {v4.8h, v5.8h}, [x9], x10")
OP(st2_8h, "st2 {v4.8h, v5.8h}, [x9]")
OP(st2_8h_pi, "st2 {v4.8h, v5.8h}, [x9], #32")
OP(st2_8h_pr, "st2 {v4.8h, v5.8h}, [x9], x10")
OP(ld2_2s, "ld2 {v4.2s, v5.2s}, [x9]")
OP(ld2_2s_pi, "ld2 {v4.2s, v5.2s}, [x9], #16")
OP(ld2_2s_pr, "ld2 {v4.2s, v5.2s}, [x9], x10")
OP(st2_2s, "st2 {v4.2s, v5.2s}, [x9]")
OP(st2_2s_pi, "st2 {v4.2s, v5.2s}, [x9], #16")
OP(st2_2s_pr, "st2 {v4.2s, v5.2s}, [x9], x10")
OP(ld2_4s, "ld2 {v4.4s, v5.4s}, [x9]")
OP(ld2_4s_pi, "ld2 {v4.4s, v5.4s}, [x9], #32")
OP(ld2_4s_pr, "ld2 {v4.4s, v5.4s}, [x9], x10")
OP(st2_4s, "st2 {v4.4s, v5.4s}, [x9]")
OP(st2_4s_pi, "st2 {v4.4s, v5.4s}, [x9], #32")
OP(st2_4s_pr, "st2 {v4.4s, v5.4s}, [x9], x10")
OP(ld2_2d, "ld2 {v4.2d, v5.2d}, [x9]")
OP(ld2_2d_pi, "ld2 {v4.2d, v5.2d}, [x9], #32")
OP(ld2_2d_pr, "ld2 {v4.2d, v5.2d}, [x9], x10")
OP(st2_2d, "st2 {v4.2d, v5.2d}, [x9]")
OP(st2_2d_pi, "st2 {v4.2d, v5.2d}, [x9], #32")
OP(st2_2d_pr, "st2 {v4.2d, v5.2d}, [x9], x10")
OP(ld3_8b, "ld3 {v4.8b, v5.8b, v6.8b}, [x9]")
OP(ld3_8b_pi, "ld3 {v4.8b, v5.8b, v6.8b}, [x9], #24")
OP(ld3_8b_pr, "ld3 {v4.8b, v5.8b, v6.8b}, [x9], x10")
OP(st3_8b, "st3 {v4.8b, v5.8b, v6.8b}, [x9]")
OP(st3_8b_pi, "st3 {v4.8b, v5.8b, v6.8b}, [x9], #24")
OP(st3_8b_pr, "st3 {v4.8b, v5.8b, v6.8b}, [x9], x10")
OP(ld3_16b, "ld3 {v4.16b, v5.16b, v6.16b}, [x9]")
OP(ld3_16b_pi, "ld3 {v4.16b, v5.16b, v6.16b}, [x9], #48")
OP(ld3_16b_pr, "ld3 {v4.16b, v5.16b, v6.16b}, [x9], x10")
OP(st3_16b, "st3 {v4.16b, v5.16b, v6.16b}, [x9]")
OP(st3_16b_pi, "st3 {v4.16b, v5.16b, v6.16b}, [x9], #48")
OP(st3_16b_pr, "st3 {v4.16b, v5.16b, v6.16b}, [x9], x10")
OP(ld3_4h, "ld3 {v4.4h, v5.4h, v6.4h}, [x9]")
OP(ld3_4h_pi, "ld3 {v4.4h, v5.4h, v6.4h}, [x9], #24")
OP(ld3_4h_pr, "ld3 {v4.4h, v5.4h, v6.4h}, [x9], x10")
OP(st3_4h, "st3 {v4.4h, v5.4h, v6.4h}, [x9]")
OP(st3_4h_pi, "st3 {v4.4h, v5.4h, v6.4h}, [x9], #24")
OP(st3_4h_pr, "st3 {v4.4h, v5.4h, v6.4h}, [x9], x10")
OP(ld3_8h, "ld3 {v4.8h, v5.8h, v6.8h}, [x9]")
OP(ld3_8h_pi, "ld3 {v4.8h, v5.8h, v6.8h}, [x9], #48")
OP(ld3_8h_pr, "ld3 {v4.8h, v5.8h, v6.8h}, [x9], x10")
OP(st3_8h, "st3 {v4.8h, v5.8h, v6.8h}, [x9]")
OP(st3_8h_pi, "st3 {v4.8h, v5.8h, v6.8h}, [x9], #48")
OP(st3_8h_pr, "st3 {v4.8h, v5.8h, v6.8h}, [x9], x10")
OP(ld3_2s, "ld3 {v4.2s, v5.2s, v6.2s}, [x9]")
OP(ld3_2s_pi, "ld3 {v4.2s, v5.2s, v6.2s}, [x9], #24")
OP(ld3_2s_pr, "ld3 {v4.2s, v5.2s, v6.2s}, [x9], x10")
OP(st3_2s, "st3 {v4.2s, v5.2s, v6.2s}, [x9]")
OP(st3_2s_pi, "st3 {v4.2s, v5.2s, v6.2s}, [x9], #24")
OP(st3_2s_pr, "st3 {v4.2s, v5.2s, v6.2s}, [x9], x10")
OP(ld3_4s, "ld3 {v4.4s, v5.4s, v6.4s}, [x9]")
OP(ld3_4s_pi, "ld3 {v4.4s, v5.4s, v6.4s}, [x9], #48")
OP(ld3_4s_pr, "ld3 {v4.4s, v5.4s, v6.4s}, [x9], x10")
OP(st3_4s, "st3 {v4.4s, v5.4s, v6.4s}, [x9]")
OP(st3_4s_pi, "st3 {v4.4s, v5.4s, v6.4s}, [x9], #48")
OP(st3_4s_pr, "st3 {v4.4s, v5.4s, v6.4s}, [x9], x10")
OP(ld3_2d, "ld3 {v4.2d, v5.2d, v6.2d}, [x9]")
OP(ld3_2d_pi, "ld3 {v4.2d, v5.2d, v6.2d}, [x9], #48")
OP(ld3_2d_pr, "ld3 {v4.2d, v5.2d, v6.2d}, [x9], x10")
OP(st3_2d, "st3 {v4.2d, v5.2d, v6.2d}, [x9]")
OP(st3_2d_pi, "st3 {v4.2d, v5.2d, v6.2d}, [x9], #48")
OP(st3_2d_pr, "st3 {v4.2d, v5.2d, v6.2d}, [x9], x10")
OP(ld4_8b, "ld4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x9]")
OP(ld4_8b_pi, "ld4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x9], #32")
OP(ld4_8b_pr, "ld4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x9], x10")
OP(st4_8b, "st4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x9]")
OP(st4_8b_pi, "st4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x9], #32")
OP(st4_8b_pr, "st4 {v4.8b, v5.8b, v6.8b, v7.8b}, [x9], x10")
OP(ld4_16b, "ld4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9]")
OP(ld4_16b_pi, "ld4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9], #64")
OP(ld4_16b_pr, "ld4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9], x10")
OP(st4_16b, "st4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9]")
OP(st4_16b_pi, "st4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9], #64")
OP(st4_16b_pr, "st4 {v4.16b, v5.16b, v6.16b, v7.16b}, [x9], x10")
OP(ld4_4h, "ld4 {v4.4h, v5.4h, v6.4h, v7.4h}, [x9]")
OP(ld4_4h_pi, "ld4 {v4.4h, v5.4h, v6.4h, v7.4h}, [x9], #32")
OP(ld4_4h_pr, "ld4 {v4.4h, v5.4h, v6.4h, v7.4h}, [x9], x10")
OP(st4_4h, "st4 {v4.4h, v5.4h, v6.4h, v7.4h}, [x9]")
OP(st4_4h_pi, "st4 {v4.4h, v5.4h, v6.4h, v7.4h}, [x9], #32")
OP(st4_4h_pr, "st4 {v4.4h, v5.4h, v6.4h, v7.4h}, [x9], x10")
OP(ld4_8h, "ld4 {v4.8h, v5.8h, v6.8h, v7.8h}, [x9]")
OP(ld4_8h_pi, "ld4 {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], #64")
OP(ld4_8h_pr, "ld4 {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], x10")
OP(st4_8h, "st4 {v4.8h, v5.8h, v6.8h, v7.8h}, [x9]")
OP(st4_8h_pi, "st4 {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], #64")
OP(st4_8h_pr, "st4 {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], x10")
OP(ld4_2s, "ld4 {v4.2s, v5.2s, v6.2s, v7.2s}, [x9]")
OP(ld4_2s_pi, "ld4 {v4.2s, v5.2s, v6.2s, v7.2s}, [x9], #32")
OP(ld4_2s_pr, "ld4 {v4.2s, v5.2s, v6.2s, v7.2s}, [x9], x10")
OP(st4_2s, "st4 {v4.2s, v5.2s, v6.2s, v7.2s}, [x9]")
OP(st4_2s_pi, "st4 {v4.2s, v5.2s, v6.2s, v7.2s}, [x9], #32")
OP(st4_2s_pr, "st4 {v4.2s, v5.2s, v6.2s, v7.2s}, [x9], x10")
OP(ld4_4s, "ld4 {v4.4s, v5.4s, v6.4s, v7.4s}, [x9]")
OP(ld4_4s_pi, "ld4 {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], #64")
OP(ld4_4s_pr, "ld4 {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], x10")
OP(st4_4s, "st4 {v4.4s, v5.4s, v6.4s, v7.4s}, [x9]")
OP(st4_4s_pi, "st4 {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], #64")
OP(st4_4s_pr, "st4 {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], x10")
OP(ld4_2d, "ld4 {v4.2d, v5.2d, v6.2d, v7.2d}, [x9]")
OP(ld4_2d_pi, "ld4 {v4.2d, v5.2d, v6.2d, v7.2d}, [x9], #64")
OP(ld4_2d_pr, "ld4 {v4.2d, v5.2d, v6.2d, v7.2d}, [x9], x10")
OP(st4_2d, "st4 {v4.2d, v5.2d, v6.2d, v7.2d}, [x9]")
OP(st4_2d_pi, "st4 {v4.2d, v5.2d, v6.2d, v7.2d}, [x9], #64")
OP(st4_2d_pr, "st4 {v4.2d, v5.2d, v6.2d, v7.2d}, [x9], x10")
OP(ld1_b0, "ld1 {v4.b}[0], [x9]")
OP(st1_b0, "st1 {v4.b}[0], [x9]")
OP(ld1_b5, "ld1 {v4.b}[5], [x9]")
OP(st1_b5, "st1 {v4.b}[5], [x9]")
OP(ld1_b15, "ld1 {v4.b}[15], [x9]")
OP(st1_b15, "st1 {v4.b}[15], [x9]")
OP(ld1_b15_pi, "ld1 {v4.b}[15], [x9], #1")
OP(ld1_b15_pr, "ld1 {v4.b}[15], [x9], x10")
OP(st1_b15_pi, "st1 {v4.b}[15], [x9], #1")
OP(st1_b15_pr, "st1 {v4.b}[15], [x9], x10")
OP(ld1_h0, "ld1 {v4.h}[0], [x9]")
OP(st1_h0, "st1 {v4.h}[0], [x9]")
OP(ld1_h3, "ld1 {v4.h}[3], [x9]")
OP(st1_h3, "st1 {v4.h}[3], [x9]")
OP(ld1_h7, "ld1 {v4.h}[7], [x9]")
OP(st1_h7, "st1 {v4.h}[7], [x9]")
OP(ld1_h7_pi, "ld1 {v4.h}[7], [x9], #2")
OP(ld1_h7_pr, "ld1 {v4.h}[7], [x9], x10")
OP(st1_h7_pi, "st1 {v4.h}[7], [x9], #2")
OP(st1_h7_pr, "st1 {v4.h}[7], [x9], x10")
OP(ld1_s0, "ld1 {v4.s}[0], [x9]")
OP(st1_s0, "st1 {v4.s}[0], [x9]")
OP(ld1_s2, "ld1 {v4.s}[2], [x9]")
OP(st1_s2, "st1 {v4.s}[2], [x9]")
OP(ld1_s3, "ld1 {v4.s}[3], [x9]")
OP(st1_s3, "st1 {v4.s}[3], [x9]")
OP(ld1_s3_pi, "ld1 {v4.s}[3], [x9], #4")
OP(ld1_s3_pr, "ld1 {v4.s}[3], [x9], x10")
OP(st1_s3_pi, "st1 {v4.s}[3], [x9], #4")
OP(st1_s3_pr, "st1 {v4.s}[3], [x9], x10")
OP(ld1_d0, "ld1 {v4.d}[0], [x9]")
OP(st1_d0, "st1 {v4.d}[0], [x9]")
OP(ld1_d1, "ld1 {v4.d}[1], [x9]")
OP(st1_d1, "st1 {v4.d}[1], [x9]")
OP(ld1_d1_pi, "ld1 {v4.d}[1], [x9], #8")
OP(ld1_d1_pr, "ld1 {v4.d}[1], [x9], x10")
OP(st1_d1_pi, "st1 {v4.d}[1], [x9], #8")
OP(st1_d1_pr, "st1 {v4.d}[1], [x9], x10")
OP(ld2_b0, "ld2 {v4.b, v5.b}[0], [x9]")
OP(st2_b0, "st2 {v4.b, v5.b}[0], [x9]")
OP(ld2_b5, "ld2 {v4.b, v5.b}[5], [x9]")
OP(st2_b5, "st2 {v4.b, v5.b}[5], [x9]")
OP(ld2_b15, "ld2 {v4.b, v5.b}[15], [x9]")
OP(st2_b15, "st2 {v4.b, v5.b}[15], [x9]")
OP(ld2_b15_pi, "ld2 {v4.b, v5.b}[15], [x9], #2")
OP(ld2_b15_pr, "ld2 {v4.b, v5.b}[15], [x9], x10")
OP(st2_b15_pi, "st2 {v4.b, v5.b}[15], [x9], #2")
OP(st2_b15_pr, "st2 {v4.b, v5.b}[15], [x9], x10")
OP(ld2_h0, "ld2 {v4.h, v5.h}[0], [x9]")
OP(st2_h0, "st2 {v4.h, v5.h}[0], [x9]")
OP(ld2_h3, "ld2 {v4.h, v5.h}[3], [x9]")
OP(st2_h3, "st2 {v4.h, v5.h}[3], [x9]")
OP(ld2_h7, "ld2 {v4.h, v5.h}[7], [x9]")
OP(st2_h7, "st2 {v4.h, v5.h}[7], [x9]")
OP(ld2_h7_pi, "ld2 {v4.h, v5.h}[7], [x9], #4")
OP(ld2_h7_pr, "ld2 {v4.h, v5.h}[7], [x9], x10")
OP(st2_h7_pi, "st2 {v4.h, v5.h}[7], [x9], #4")
OP(st2_h7_pr, "st2 {v4.h, v5.h}[7], [x9], x10")
OP(ld2_s0, "ld2 {v4.s, v5.s}[0], [x9]")
OP(st2_s0, "st2 {v4.s, v5.s}[0], [x9]")
OP(ld2_s2, "ld2 {v4.s, v5.s}[2], [x9]")
OP(st2_s2, "st2 {v4.s, v5.s}[2], [x9]")
OP(ld2_s3, "ld2 {v4.s, v5.s}[3], [x9]")
OP(st2_s3, "st2 {v4.s, v5.s}[3], [x9]")
OP(ld2_s3_pi, "ld2 {v4.s, v5.s}[3], [x9], #8")
OP(ld2_s3_pr, "ld2 {v4.s, v5.s}[3], [x9], x10")
OP(st2_s3_pi, "st2 {v4.s, v5.s}[3], [x9], #8")
OP(st2_s3_pr, "st2 {v4.s, v5.s}[3], [x9], x10")
OP(ld2_d0, "ld2 {v4.d, v5.d}[0], [x9]")
OP(st2_d0, "st2 {v4.d, v5.d}[0], [x9]")
OP(ld2_d1, "ld2 {v4.d, v5.d}[1], [x9]")
OP(st2_d1, "st2 {v4.d, v5.d}[1], [x9]")
OP(ld2_d1_pi, "ld2 {v4.d, v5.d}[1], [x9], #16")
OP(ld2_d1_pr, "ld2 {v4.d, v5.d}[1], [x9], x10")
OP(st2_d1_pi, "st2 {v4.d, v5.d}[1], [x9], #16")
OP(st2_d1_pr, "st2 {v4.d, v5.d}[1], [x9], x10")
OP(ld3_b0, "ld3 {v4.b, v5.b, v6.b}[0], [x9]")
OP(st3_b0, "st3 {v4.b, v5.b, v6.b}[0], [x9]")
OP(ld3_b5, "ld3 {v4.b, v5.b, v6.b}[5], [x9]")
OP(st3_b5, "st3 {v4.b, v5.b, v6.b}[5], [x9]")
OP(ld3_b15, "ld3 {v4.b, v5.b, v6.b}[15], [x9]")
OP(st3_b15, "st3 {v4.b, v5.b, v6.b}[15], [x9]")
OP(ld3_b15_pi, "ld3 {v4.b, v5.b, v6.b}[15], [x9], #3")
OP(ld3_b15_pr, "ld3 {v4.b, v5.b, v6.b}[15], [x9], x10")
OP(st3_b15_pi, "st3 {v4.b, v5.b, v6.b}[15], [x9], #3")
OP(st3_b15_pr, "st3 {v4.b, v5.b, v6.b}[15], [x9], x10")
OP(ld3_h0, "ld3 {v4.h, v5.h, v6.h}[0], [x9]")
OP(st3_h0, "st3 {v4.h, v5.h, v6.h}[0], [x9]")
OP(ld3_h3, "ld3 {v4.h, v5.h, v6.h}[3], [x9]")
OP(st3_h3, "st3 {v4.h, v5.h, v6.h}[3], [x9]")
OP(ld3_h7, "ld3 {v4.h, v5.h, v6.h}[7], [x9]")
OP(st3_h7, "st3 {v4.h, v5.h, v6.h}[7], [x9]")
OP(ld3_h7_pi, "ld3 {v4.h, v5.h, v6.h}[7], [x9], #6")
OP(ld3_h7_pr, "ld3 {v4.h, v5.h, v6.h}[7], [x9], x10")
OP(st3_h7_pi, "st3 {v4.h, v5.h, v6.h}[7], [x9], #6")
OP(st3_h7_pr, "st3 {v4.h, v5.h, v6.h}[7], [x9], x10")
OP(ld3_s0, "ld3 {v4.s, v5.s, v6.s}[0], [x9]")
OP(st3_s0, "st3 {v4.s, v5.s, v6.s}[0], [x9]")
OP(ld3_s2, "ld3 {v4.s, v5.s, v6.s}[2], [x9]")
OP(st3_s2, "st3 {v4.s, v5.s, v6.s}[2], [x9]")
OP(ld3_s3, "ld3 {v4.s, v5.s, v6.s}[3], [x9]")
OP(st3_s3, "st3 {v4.s, v5.s, v6.s}[3], [x9]")
OP(ld3_s3_pi, "ld3 {v4.s, v5.s, v6.s}[3], [x9], #12")
OP(ld3_s3_pr, "ld3 {v4.s, v5.s, v6.s}[3], [x9], x10")
OP(st3_s3_pi, "st3 {v4.s, v5.s, v6.s}[3], [x9], #12")
OP(st3_s3_pr, "st3 {v4.s, v5.s, v6.s}[3], [x9], x10")
OP(ld3_d0, "ld3 {v4.d, v5.d, v6.d}[0], [x9]")
OP(st3_d0, "st3 {v4.d, v5.d, v6.d}[0], [x9]")
OP(ld3_d1, "ld3 {v4.d, v5.d, v6.d}[1], [x9]")
OP(st3_d1, "st3 {v4.d, v5.d, v6.d}[1], [x9]")
OP(ld3_d1_pi, "ld3 {v4.d, v5.d, v6.d}[1], [x9], #24")
OP(ld3_d1_pr, "ld3 {v4.d, v5.d, v6.d}[1], [x9], x10")
OP(st3_d1_pi, "st3 {v4.d, v5.d, v6.d}[1], [x9], #24")
OP(st3_d1_pr, "st3 {v4.d, v5.d, v6.d}[1], [x9], x10")
OP(ld4_b0, "ld4 {v4.b, v5.b, v6.b, v7.b}[0], [x9]")
OP(st4_b0, "st4 {v4.b, v5.b, v6.b, v7.b}[0], [x9]")
OP(ld4_b5, "ld4 {v4.b, v5.b, v6.b, v7.b}[5], [x9]")
OP(st4_b5, "st4 {v4.b, v5.b, v6.b, v7.b}[5], [x9]")
OP(ld4_b15, "ld4 {v4.b, v5.b, v6.b, v7.b}[15], [x9]")
OP(st4_b15, "st4 {v4.b, v5.b, v6.b, v7.b}[15], [x9]")
OP(ld4_b15_pi, "ld4 {v4.b, v5.b, v6.b, v7.b}[15], [x9], #4")
OP(ld4_b15_pr, "ld4 {v4.b, v5.b, v6.b, v7.b}[15], [x9], x10")
OP(st4_b15_pi, "st4 {v4.b, v5.b, v6.b, v7.b}[15], [x9], #4")
OP(st4_b15_pr, "st4 {v4.b, v5.b, v6.b, v7.b}[15], [x9], x10")
OP(ld4_h0, "ld4 {v4.h, v5.h, v6.h, v7.h}[0], [x9]")
OP(st4_h0, "st4 {v4.h, v5.h, v6.h, v7.h}[0], [x9]")
OP(ld4_h3, "ld4 {v4.h, v5.h, v6.h, v7.h}[3], [x9]")
OP(st4_h3, "st4 {v4.h, v5.h, v6.h, v7.h}[3], [x9]")
OP(ld4_h7, "ld4 {v4.h, v5.h, v6.h, v7.h}[7], [x9]")
OP(st4_h7, "st4 {v4.h, v5.h, v6.h, v7.h}[7], [x9]")
OP(ld4_h7_pi, "ld4 {v4.h, v5.h, v6.h, v7.h}[7], [x9], #8")
OP(ld4_h7_pr, "ld4 {v4.h, v5.h, v6.h, v7.h}[7], [x9], x10")
OP(st4_h7_pi, "st4 {v4.h, v5.h, v6.h, v7.h}[7], [x9], #8")
OP(st4_h7_pr, "st4 {v4.h, v5.h, v6.h, v7.h}[7], [x9], x10")
OP(ld4_s0, "ld4 {v4.s, v5.s, v6.s, v7.s}[0], [x9]")
OP(st4_s0, "st4 {v4.s, v5.s, v6.s, v7.s}[0], [x9]")
OP(ld4_s2, "ld4 {v4.s, v5.s, v6.s, v7.s}[2], [x9]")
OP(st4_s2, "st4 {v4.s, v5.s, v6.s, v7.s}[2], [x9]")
OP(ld4_s3, "ld4 {v4.s, v5.s, v6.s, v7.s}[3], [x9]")
OP(st4_s3, "st4 {v4.s, v5.s, v6.s, v7.s}[3], [x9]")
OP(ld4_s3_pi, "ld4 {v4.s, v5.s, v6.s, v7.s}[3], [x9], #16")
OP(ld4_s3_pr, "ld4 {v4.s, v5.s, v6.s, v7.s}[3], [x9], x10")
OP(st4_s3_pi, "st4 {v4.s, v5.s, v6.s, v7.s}[3], [x9], #16")
OP(st4_s3_pr, "st4 {v4.s, v5.s, v6.s, v7.s}[3], [x9], x10")
OP(ld4_d0, "ld4 {v4.d, v5.d, v6.d, v7.d}[0], [x9]")
OP(st4_d0, "st4 {v4.d, v5.d, v6.d, v7.d}[0], [x9]")
OP(ld4_d1, "ld4 {v4.d, v5.d, v6.d, v7.d}[1], [x9]")
OP(st4_d1, "st4 {v4.d, v5.d, v6.d, v7.d}[1], [x9]")
OP(ld4_d1_pi, "ld4 {v4.d, v5.d, v6.d, v7.d}[1], [x9], #32")
OP(ld4_d1_pr, "ld4 {v4.d, v5.d, v6.d, v7.d}[1], [x9], x10")
OP(st4_d1_pi, "st4 {v4.d, v5.d, v6.d, v7.d}[1], [x9], #32")
OP(st4_d1_pr, "st4 {v4.d, v5.d, v6.d, v7.d}[1], [x9], x10")
OP(ld1r_8b, "ld1r {v4.8b}, [x9]")
OP(ld1r_8b_pi, "ld1r {v4.8b}, [x9], #1")
OP(ld1r_8b_pr, "ld1r {v4.8b}, [x9], x10")
OP(ld1r_16b, "ld1r {v4.16b}, [x9]")
OP(ld1r_16b_pi, "ld1r {v4.16b}, [x9], #1")
OP(ld1r_16b_pr, "ld1r {v4.16b}, [x9], x10")
OP(ld1r_4h, "ld1r {v4.4h}, [x9]")
OP(ld1r_4h_pi, "ld1r {v4.4h}, [x9], #2")
OP(ld1r_4h_pr, "ld1r {v4.4h}, [x9], x10")
OP(ld1r_8h, "ld1r {v4.8h}, [x9]")
OP(ld1r_8h_pi, "ld1r {v4.8h}, [x9], #2")
OP(ld1r_8h_pr, "ld1r {v4.8h}, [x9], x10")
OP(ld1r_2s, "ld1r {v4.2s}, [x9]")
OP(ld1r_2s_pi, "ld1r {v4.2s}, [x9], #4")
OP(ld1r_2s_pr, "ld1r {v4.2s}, [x9], x10")
OP(ld1r_4s, "ld1r {v4.4s}, [x9]")
OP(ld1r_4s_pi, "ld1r {v4.4s}, [x9], #4")
OP(ld1r_4s_pr, "ld1r {v4.4s}, [x9], x10")
OP(ld1r_1d, "ld1r {v4.1d}, [x9]")
OP(ld1r_1d_pi, "ld1r {v4.1d}, [x9], #8")
OP(ld1r_1d_pr, "ld1r {v4.1d}, [x9], x10")
OP(ld1r_2d, "ld1r {v4.2d}, [x9]")
OP(ld1r_2d_pi, "ld1r {v4.2d}, [x9], #8")
OP(ld1r_2d_pr, "ld1r {v4.2d}, [x9], x10")
OP(ld2r_8b, "ld2r {v4.8b, v5.8b}, [x9]")
OP(ld2r_8b_pi, "ld2r {v4.8b, v5.8b}, [x9], #2")
OP(ld2r_8b_pr, "ld2r {v4.8b, v5.8b}, [x9], x10")
OP(ld2r_16b, "ld2r {v4.16b, v5.16b}, [x9]")
OP(ld2r_16b_pi, "ld2r {v4.16b, v5.16b}, [x9], #2")
OP(ld2r_16b_pr, "ld2r {v4.16b, v5.16b}, [x9], x10")
OP(ld2r_4h, "ld2r {v4.4h, v5.4h}, [x9]")
OP(ld2r_4h_pi, "ld2r {v4.4h, v5.4h}, [x9], #4")
OP(ld2r_4h_pr, "ld2r {v4.4h, v5.4h}, [x9], x10")
OP(ld2r_8h, "ld2r {v4.8h, v5.8h}, [x9]")
OP(ld2r_8h_pi, "ld2r {v4.8h, v5.8h}, [x9], #4")
OP(ld2r_8h_pr, "ld2r {v4.8h, v5.8h}, [x9], x10")
OP(ld2r_2s, "ld2r {v4.2s, v5.2s}, [x9]")
OP(ld2r_2s_pi, "ld2r {v4.2s, v5.2s}, [x9], #8")
OP(ld2r_2s_pr, "ld2r {v4.2s, v5.2s}, [x9], x10")
OP(ld2r_4s, "ld2r {v4.4s, v5.4s}, [x9]")
OP(ld2r_4s_pi, "ld2r {v4.4s, v5.4s}, [x9], #8")
OP(ld2r_4s_pr, "ld2r {v4.4s, v5.4s}, [x9], x10")
OP(ld2r_1d, "ld2r {v4.1d, v5.1d}, [x9]")
OP(ld2r_1d_pi, "ld2r {v4.1d, v5.1d}, [x9], #16")
OP(ld2r_1d_pr, "ld2r {v4.1d, v5.1d}, [x9], x10")
OP(ld2r_2d, "ld2r {v4.2d, v5.2d}, [x9]")
OP(ld2r_2d_pi, "ld2r {v4.2d, v5.2d}, [x9], #16")
OP(ld2r_2d_pr, "ld2r {v4.2d, v5.2d}, [x9], x10")
OP(ld3r_8b, "ld3r {v4.8b, v5.8b, v6.8b}, [x9]")
OP(ld3r_8b_pi, "ld3r {v4.8b, v5.8b, v6.8b}, [x9], #3")
OP(ld3r_8b_pr, "ld3r {v4.8b, v5.8b, v6.8b}, [x9], x10")
OP(ld3r_16b, "ld3r {v4.16b, v5.16b, v6.16b}, [x9]")
OP(ld3r_16b_pi, "ld3r {v4.16b, v5.16b, v6.16b}, [x9], #3")
OP(ld3r_16b_pr, "ld3r {v4.16b, v5.16b, v6.16b}, [x9], x10")
OP(ld3r_4h, "ld3r {v4.4h, v5.4h, v6.4h}, [x9]")
OP(ld3r_4h_pi, "ld3r {v4.4h, v5.4h, v6.4h}, [x9], #6")
OP(ld3r_4h_pr, "ld3r {v4.4h, v5.4h, v6.4h}, [x9], x10")
OP(ld3r_8h, "ld3r {v4.8h, v5.8h, v6.8h}, [x9]")
OP(ld3r_8h_pi, "ld3r {v4.8h, v5.8h, v6.8h}, [x9], #6")
OP(ld3r_8h_pr, "ld3r {v4.8h, v5.8h, v6.8h}, [x9], x10")
OP(ld3r_2s, "ld3r {v4.2s, v5.2s, v6.2s}, [x9]")
OP(ld3r_2s_pi, "ld3r {v4.2s, v5.2s, v6.2s}, [x9], #12")
OP(ld3r_2s_pr, "ld3r {v4.2s, v5.2s, v6.2s}, [x9], x10")
OP(ld3r_4s, "ld3r {v4.4s, v5.4s, v6.4s}, [x9]")
OP(ld3r_4s_pi, "ld3r {v4.4s, v5.4s, v6.4s}, [x9], #12")
OP(ld3r_4s_pr, "ld3r {v4.4s, v5.4s, v6.4s}, [x9], x10")
OP(ld3r_1d, "ld3r {v4.1d, v5.1d, v6.1d}, [x9]")
OP(ld3r_1d_pi, "ld3r {v4.1d, v5.1d, v6.1d}, [x9], #24")
OP(ld3r_1d_pr, "ld3r {v4.1d, v5.1d, v6.1d}, [x9], x10")
OP(ld3r_2d, "ld3r {v4.2d, v5.2d, v6.2d}, [x9]")
OP(ld3r_2d_pi, "ld3r {v4.2d, v5.2d, v6.2d}, [x9], #24")
OP(ld3r_2d_pr, "ld3r {v4.2d, v5.2d, v6.2d}, [x9], x10")
OP(ld4r_8b, "ld4r {v4.8b, v5.8b, v6.8b, v7.8b}, [x9]")
OP(ld4r_8b_pi, "ld4r {v4.8b, v5.8b, v6.8b, v7.8b}, [x9], #4")
OP(ld4r_8b_pr, "ld4r {v4.8b, v5.8b, v6.8b, v7.8b}, [x9], x10")
OP(ld4r_16b, "ld4r {v4.16b, v5.16b, v6.16b, v7.16b}, [x9]")
OP(ld4r_16b_pi, "ld4r {v4.16b, v5.16b, v6.16b, v7.16b}, [x9], #4")
OP(ld4r_16b_pr, "ld4r {v4.16b, v5.16b, v6.16b, v7.16b}, [x9], x10")
OP(ld4r_4h, "ld4r {v4.4h, v5.4h, v6.4h, v7.4h}, [x9]")
OP(ld4r_4h_pi, "ld4r {v4.4h, v5.4h, v6.4h, v7.4h}, [x9], #8")
OP(ld4r_4h_pr, "ld4r {v4.4h, v5.4h, v6.4h, v7.4h}, [x9], x10")
OP(ld4r_8h, "ld4r {v4.8h, v5.8h, v6.8h, v7.8h}, [x9]")
OP(ld4r_8h_pi, "ld4r {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], #8")
OP(ld4r_8h_pr, "ld4r {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], x10")
OP(ld4r_2s, "ld4r {v4.2s, v5.2s, v6.2s, v7.2s}, [x9]")
OP(ld4r_2s_pi, "ld4r {v4.2s, v5.2s, v6.2s, v7.2s}, [x9], #16")
OP(ld4r_2s_pr, "ld4r {v4.2s, v5.2s, v6.2s, v7.2s}, [x9], x10")
OP(ld4r_4s, "ld4r {v4.4s, v5.4s, v6.4s, v7.4s}, [x9]")
OP(ld4r_4s_pi, "ld4r {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], #16")
OP(ld4r_4s_pr, "ld4r {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], x10")
OP(ld4r_1d, "ld4r {v4.1d, v5.1d, v6.1d, v7.1d}, [x9]")
OP(ld4r_1d_pi, "ld4r {v4.1d, v5.1d, v6.1d, v7.1d}, [x9], #32")
OP(ld4r_1d_pr, "ld4r {v4.1d, v5.1d, v6.1d, v7.1d}, [x9], x10")
OP(ld4r_2d, "ld4r {v4.2d, v5.2d, v6.2d, v7.2d}, [x9]")
OP(ld4r_2d_pi, "ld4r {v4.2d, v5.2d, v6.2d, v7.2d}, [x9], #32")
OP(ld4r_2d_pr, "ld4r {v4.2d, v5.2d, v6.2d, v7.2d}, [x9], x10")
OP(ldr_b, "ldr b4, [x9, #3]")
OP(str_b, "str b4, [x9, #5]")
OP(ldur_b, "ldur b4, [x9, #-2]")
OP(stur_b, "stur b4, [x9, #-3]")
OP(ldr_b_post, "ldr b4, [x9], #3")
OP(str_b_post, "str b4, [x9], #-5")
OP(ldr_b_pre, "ldr b4, [x9, #5]!")
OP(str_b_pre, "str b4, [x9, #-7]!")
OP(ldr_h, "ldr h4, [x9, #6]")
OP(str_h, "str h4, [x9, #10]")
OP(ldur_h, "ldur h4, [x9, #-3]")
OP(stur_h, "stur h4, [x9, #-3]")
OP(ldr_h_post, "ldr h4, [x9], #4")
OP(str_h_post, "str h4, [x9], #-5")
OP(ldr_h_pre, "ldr h4, [x9, #6]!")
OP(str_h_pre, "str h4, [x9, #-7]!")
typedef struct { const char *name; Fn fn; } Op;
int main(int argc, char **argv) {
  int n = 20;
  if (argc > 1) { n = 0; for (char *p = argv[1]; *p; p++) n = n * 10 + (*p - '0'); }
  dump = argc > 2;
  static const Op ops[] = {
    {"ld2_8b", f_ld2_8b},
    {"ld2_8b_pi", f_ld2_8b_pi},
    {"ld2_8b_pr", f_ld2_8b_pr},
    {"st2_8b", f_st2_8b},
    {"st2_8b_pi", f_st2_8b_pi},
    {"st2_8b_pr", f_st2_8b_pr},
    {"ld2_16b", f_ld2_16b},
    {"ld2_16b_pi", f_ld2_16b_pi},
    {"ld2_16b_pr", f_ld2_16b_pr},
    {"st2_16b", f_st2_16b},
    {"st2_16b_pi", f_st2_16b_pi},
    {"st2_16b_pr", f_st2_16b_pr},
    {"ld2_4h", f_ld2_4h},
    {"ld2_4h_pi", f_ld2_4h_pi},
    {"ld2_4h_pr", f_ld2_4h_pr},
    {"st2_4h", f_st2_4h},
    {"st2_4h_pi", f_st2_4h_pi},
    {"st2_4h_pr", f_st2_4h_pr},
    {"ld2_8h", f_ld2_8h},
    {"ld2_8h_pi", f_ld2_8h_pi},
    {"ld2_8h_pr", f_ld2_8h_pr},
    {"st2_8h", f_st2_8h},
    {"st2_8h_pi", f_st2_8h_pi},
    {"st2_8h_pr", f_st2_8h_pr},
    {"ld2_2s", f_ld2_2s},
    {"ld2_2s_pi", f_ld2_2s_pi},
    {"ld2_2s_pr", f_ld2_2s_pr},
    {"st2_2s", f_st2_2s},
    {"st2_2s_pi", f_st2_2s_pi},
    {"st2_2s_pr", f_st2_2s_pr},
    {"ld2_4s", f_ld2_4s},
    {"ld2_4s_pi", f_ld2_4s_pi},
    {"ld2_4s_pr", f_ld2_4s_pr},
    {"st2_4s", f_st2_4s},
    {"st2_4s_pi", f_st2_4s_pi},
    {"st2_4s_pr", f_st2_4s_pr},
    {"ld2_2d", f_ld2_2d},
    {"ld2_2d_pi", f_ld2_2d_pi},
    {"ld2_2d_pr", f_ld2_2d_pr},
    {"st2_2d", f_st2_2d},
    {"st2_2d_pi", f_st2_2d_pi},
    {"st2_2d_pr", f_st2_2d_pr},
    {"ld3_8b", f_ld3_8b},
    {"ld3_8b_pi", f_ld3_8b_pi},
    {"ld3_8b_pr", f_ld3_8b_pr},
    {"st3_8b", f_st3_8b},
    {"st3_8b_pi", f_st3_8b_pi},
    {"st3_8b_pr", f_st3_8b_pr},
    {"ld3_16b", f_ld3_16b},
    {"ld3_16b_pi", f_ld3_16b_pi},
    {"ld3_16b_pr", f_ld3_16b_pr},
    {"st3_16b", f_st3_16b},
    {"st3_16b_pi", f_st3_16b_pi},
    {"st3_16b_pr", f_st3_16b_pr},
    {"ld3_4h", f_ld3_4h},
    {"ld3_4h_pi", f_ld3_4h_pi},
    {"ld3_4h_pr", f_ld3_4h_pr},
    {"st3_4h", f_st3_4h},
    {"st3_4h_pi", f_st3_4h_pi},
    {"st3_4h_pr", f_st3_4h_pr},
    {"ld3_8h", f_ld3_8h},
    {"ld3_8h_pi", f_ld3_8h_pi},
    {"ld3_8h_pr", f_ld3_8h_pr},
    {"st3_8h", f_st3_8h},
    {"st3_8h_pi", f_st3_8h_pi},
    {"st3_8h_pr", f_st3_8h_pr},
    {"ld3_2s", f_ld3_2s},
    {"ld3_2s_pi", f_ld3_2s_pi},
    {"ld3_2s_pr", f_ld3_2s_pr},
    {"st3_2s", f_st3_2s},
    {"st3_2s_pi", f_st3_2s_pi},
    {"st3_2s_pr", f_st3_2s_pr},
    {"ld3_4s", f_ld3_4s},
    {"ld3_4s_pi", f_ld3_4s_pi},
    {"ld3_4s_pr", f_ld3_4s_pr},
    {"st3_4s", f_st3_4s},
    {"st3_4s_pi", f_st3_4s_pi},
    {"st3_4s_pr", f_st3_4s_pr},
    {"ld3_2d", f_ld3_2d},
    {"ld3_2d_pi", f_ld3_2d_pi},
    {"ld3_2d_pr", f_ld3_2d_pr},
    {"st3_2d", f_st3_2d},
    {"st3_2d_pi", f_st3_2d_pi},
    {"st3_2d_pr", f_st3_2d_pr},
    {"ld4_8b", f_ld4_8b},
    {"ld4_8b_pi", f_ld4_8b_pi},
    {"ld4_8b_pr", f_ld4_8b_pr},
    {"st4_8b", f_st4_8b},
    {"st4_8b_pi", f_st4_8b_pi},
    {"st4_8b_pr", f_st4_8b_pr},
    {"ld4_16b", f_ld4_16b},
    {"ld4_16b_pi", f_ld4_16b_pi},
    {"ld4_16b_pr", f_ld4_16b_pr},
    {"st4_16b", f_st4_16b},
    {"st4_16b_pi", f_st4_16b_pi},
    {"st4_16b_pr", f_st4_16b_pr},
    {"ld4_4h", f_ld4_4h},
    {"ld4_4h_pi", f_ld4_4h_pi},
    {"ld4_4h_pr", f_ld4_4h_pr},
    {"st4_4h", f_st4_4h},
    {"st4_4h_pi", f_st4_4h_pi},
    {"st4_4h_pr", f_st4_4h_pr},
    {"ld4_8h", f_ld4_8h},
    {"ld4_8h_pi", f_ld4_8h_pi},
    {"ld4_8h_pr", f_ld4_8h_pr},
    {"st4_8h", f_st4_8h},
    {"st4_8h_pi", f_st4_8h_pi},
    {"st4_8h_pr", f_st4_8h_pr},
    {"ld4_2s", f_ld4_2s},
    {"ld4_2s_pi", f_ld4_2s_pi},
    {"ld4_2s_pr", f_ld4_2s_pr},
    {"st4_2s", f_st4_2s},
    {"st4_2s_pi", f_st4_2s_pi},
    {"st4_2s_pr", f_st4_2s_pr},
    {"ld4_4s", f_ld4_4s},
    {"ld4_4s_pi", f_ld4_4s_pi},
    {"ld4_4s_pr", f_ld4_4s_pr},
    {"st4_4s", f_st4_4s},
    {"st4_4s_pi", f_st4_4s_pi},
    {"st4_4s_pr", f_st4_4s_pr},
    {"ld4_2d", f_ld4_2d},
    {"ld4_2d_pi", f_ld4_2d_pi},
    {"ld4_2d_pr", f_ld4_2d_pr},
    {"st4_2d", f_st4_2d},
    {"st4_2d_pi", f_st4_2d_pi},
    {"st4_2d_pr", f_st4_2d_pr},
    {"ld1_b0", f_ld1_b0},
    {"st1_b0", f_st1_b0},
    {"ld1_b5", f_ld1_b5},
    {"st1_b5", f_st1_b5},
    {"ld1_b15", f_ld1_b15},
    {"st1_b15", f_st1_b15},
    {"ld1_b15_pi", f_ld1_b15_pi},
    {"ld1_b15_pr", f_ld1_b15_pr},
    {"st1_b15_pi", f_st1_b15_pi},
    {"st1_b15_pr", f_st1_b15_pr},
    {"ld1_h0", f_ld1_h0},
    {"st1_h0", f_st1_h0},
    {"ld1_h3", f_ld1_h3},
    {"st1_h3", f_st1_h3},
    {"ld1_h7", f_ld1_h7},
    {"st1_h7", f_st1_h7},
    {"ld1_h7_pi", f_ld1_h7_pi},
    {"ld1_h7_pr", f_ld1_h7_pr},
    {"st1_h7_pi", f_st1_h7_pi},
    {"st1_h7_pr", f_st1_h7_pr},
    {"ld1_s0", f_ld1_s0},
    {"st1_s0", f_st1_s0},
    {"ld1_s2", f_ld1_s2},
    {"st1_s2", f_st1_s2},
    {"ld1_s3", f_ld1_s3},
    {"st1_s3", f_st1_s3},
    {"ld1_s3_pi", f_ld1_s3_pi},
    {"ld1_s3_pr", f_ld1_s3_pr},
    {"st1_s3_pi", f_st1_s3_pi},
    {"st1_s3_pr", f_st1_s3_pr},
    {"ld1_d0", f_ld1_d0},
    {"st1_d0", f_st1_d0},
    {"ld1_d1", f_ld1_d1},
    {"st1_d1", f_st1_d1},
    {"ld1_d1_pi", f_ld1_d1_pi},
    {"ld1_d1_pr", f_ld1_d1_pr},
    {"st1_d1_pi", f_st1_d1_pi},
    {"st1_d1_pr", f_st1_d1_pr},
    {"ld2_b0", f_ld2_b0},
    {"st2_b0", f_st2_b0},
    {"ld2_b5", f_ld2_b5},
    {"st2_b5", f_st2_b5},
    {"ld2_b15", f_ld2_b15},
    {"st2_b15", f_st2_b15},
    {"ld2_b15_pi", f_ld2_b15_pi},
    {"ld2_b15_pr", f_ld2_b15_pr},
    {"st2_b15_pi", f_st2_b15_pi},
    {"st2_b15_pr", f_st2_b15_pr},
    {"ld2_h0", f_ld2_h0},
    {"st2_h0", f_st2_h0},
    {"ld2_h3", f_ld2_h3},
    {"st2_h3", f_st2_h3},
    {"ld2_h7", f_ld2_h7},
    {"st2_h7", f_st2_h7},
    {"ld2_h7_pi", f_ld2_h7_pi},
    {"ld2_h7_pr", f_ld2_h7_pr},
    {"st2_h7_pi", f_st2_h7_pi},
    {"st2_h7_pr", f_st2_h7_pr},
    {"ld2_s0", f_ld2_s0},
    {"st2_s0", f_st2_s0},
    {"ld2_s2", f_ld2_s2},
    {"st2_s2", f_st2_s2},
    {"ld2_s3", f_ld2_s3},
    {"st2_s3", f_st2_s3},
    {"ld2_s3_pi", f_ld2_s3_pi},
    {"ld2_s3_pr", f_ld2_s3_pr},
    {"st2_s3_pi", f_st2_s3_pi},
    {"st2_s3_pr", f_st2_s3_pr},
    {"ld2_d0", f_ld2_d0},
    {"st2_d0", f_st2_d0},
    {"ld2_d1", f_ld2_d1},
    {"st2_d1", f_st2_d1},
    {"ld2_d1_pi", f_ld2_d1_pi},
    {"ld2_d1_pr", f_ld2_d1_pr},
    {"st2_d1_pi", f_st2_d1_pi},
    {"st2_d1_pr", f_st2_d1_pr},
    {"ld3_b0", f_ld3_b0},
    {"st3_b0", f_st3_b0},
    {"ld3_b5", f_ld3_b5},
    {"st3_b5", f_st3_b5},
    {"ld3_b15", f_ld3_b15},
    {"st3_b15", f_st3_b15},
    {"ld3_b15_pi", f_ld3_b15_pi},
    {"ld3_b15_pr", f_ld3_b15_pr},
    {"st3_b15_pi", f_st3_b15_pi},
    {"st3_b15_pr", f_st3_b15_pr},
    {"ld3_h0", f_ld3_h0},
    {"st3_h0", f_st3_h0},
    {"ld3_h3", f_ld3_h3},
    {"st3_h3", f_st3_h3},
    {"ld3_h7", f_ld3_h7},
    {"st3_h7", f_st3_h7},
    {"ld3_h7_pi", f_ld3_h7_pi},
    {"ld3_h7_pr", f_ld3_h7_pr},
    {"st3_h7_pi", f_st3_h7_pi},
    {"st3_h7_pr", f_st3_h7_pr},
    {"ld3_s0", f_ld3_s0},
    {"st3_s0", f_st3_s0},
    {"ld3_s2", f_ld3_s2},
    {"st3_s2", f_st3_s2},
    {"ld3_s3", f_ld3_s3},
    {"st3_s3", f_st3_s3},
    {"ld3_s3_pi", f_ld3_s3_pi},
    {"ld3_s3_pr", f_ld3_s3_pr},
    {"st3_s3_pi", f_st3_s3_pi},
    {"st3_s3_pr", f_st3_s3_pr},
    {"ld3_d0", f_ld3_d0},
    {"st3_d0", f_st3_d0},
    {"ld3_d1", f_ld3_d1},
    {"st3_d1", f_st3_d1},
    {"ld3_d1_pi", f_ld3_d1_pi},
    {"ld3_d1_pr", f_ld3_d1_pr},
    {"st3_d1_pi", f_st3_d1_pi},
    {"st3_d1_pr", f_st3_d1_pr},
    {"ld4_b0", f_ld4_b0},
    {"st4_b0", f_st4_b0},
    {"ld4_b5", f_ld4_b5},
    {"st4_b5", f_st4_b5},
    {"ld4_b15", f_ld4_b15},
    {"st4_b15", f_st4_b15},
    {"ld4_b15_pi", f_ld4_b15_pi},
    {"ld4_b15_pr", f_ld4_b15_pr},
    {"st4_b15_pi", f_st4_b15_pi},
    {"st4_b15_pr", f_st4_b15_pr},
    {"ld4_h0", f_ld4_h0},
    {"st4_h0", f_st4_h0},
    {"ld4_h3", f_ld4_h3},
    {"st4_h3", f_st4_h3},
    {"ld4_h7", f_ld4_h7},
    {"st4_h7", f_st4_h7},
    {"ld4_h7_pi", f_ld4_h7_pi},
    {"ld4_h7_pr", f_ld4_h7_pr},
    {"st4_h7_pi", f_st4_h7_pi},
    {"st4_h7_pr", f_st4_h7_pr},
    {"ld4_s0", f_ld4_s0},
    {"st4_s0", f_st4_s0},
    {"ld4_s2", f_ld4_s2},
    {"st4_s2", f_st4_s2},
    {"ld4_s3", f_ld4_s3},
    {"st4_s3", f_st4_s3},
    {"ld4_s3_pi", f_ld4_s3_pi},
    {"ld4_s3_pr", f_ld4_s3_pr},
    {"st4_s3_pi", f_st4_s3_pi},
    {"st4_s3_pr", f_st4_s3_pr},
    {"ld4_d0", f_ld4_d0},
    {"st4_d0", f_st4_d0},
    {"ld4_d1", f_ld4_d1},
    {"st4_d1", f_st4_d1},
    {"ld4_d1_pi", f_ld4_d1_pi},
    {"ld4_d1_pr", f_ld4_d1_pr},
    {"st4_d1_pi", f_st4_d1_pi},
    {"st4_d1_pr", f_st4_d1_pr},
    {"ld1r_8b", f_ld1r_8b},
    {"ld1r_8b_pi", f_ld1r_8b_pi},
    {"ld1r_8b_pr", f_ld1r_8b_pr},
    {"ld1r_16b", f_ld1r_16b},
    {"ld1r_16b_pi", f_ld1r_16b_pi},
    {"ld1r_16b_pr", f_ld1r_16b_pr},
    {"ld1r_4h", f_ld1r_4h},
    {"ld1r_4h_pi", f_ld1r_4h_pi},
    {"ld1r_4h_pr", f_ld1r_4h_pr},
    {"ld1r_8h", f_ld1r_8h},
    {"ld1r_8h_pi", f_ld1r_8h_pi},
    {"ld1r_8h_pr", f_ld1r_8h_pr},
    {"ld1r_2s", f_ld1r_2s},
    {"ld1r_2s_pi", f_ld1r_2s_pi},
    {"ld1r_2s_pr", f_ld1r_2s_pr},
    {"ld1r_4s", f_ld1r_4s},
    {"ld1r_4s_pi", f_ld1r_4s_pi},
    {"ld1r_4s_pr", f_ld1r_4s_pr},
    {"ld1r_1d", f_ld1r_1d},
    {"ld1r_1d_pi", f_ld1r_1d_pi},
    {"ld1r_1d_pr", f_ld1r_1d_pr},
    {"ld1r_2d", f_ld1r_2d},
    {"ld1r_2d_pi", f_ld1r_2d_pi},
    {"ld1r_2d_pr", f_ld1r_2d_pr},
    {"ld2r_8b", f_ld2r_8b},
    {"ld2r_8b_pi", f_ld2r_8b_pi},
    {"ld2r_8b_pr", f_ld2r_8b_pr},
    {"ld2r_16b", f_ld2r_16b},
    {"ld2r_16b_pi", f_ld2r_16b_pi},
    {"ld2r_16b_pr", f_ld2r_16b_pr},
    {"ld2r_4h", f_ld2r_4h},
    {"ld2r_4h_pi", f_ld2r_4h_pi},
    {"ld2r_4h_pr", f_ld2r_4h_pr},
    {"ld2r_8h", f_ld2r_8h},
    {"ld2r_8h_pi", f_ld2r_8h_pi},
    {"ld2r_8h_pr", f_ld2r_8h_pr},
    {"ld2r_2s", f_ld2r_2s},
    {"ld2r_2s_pi", f_ld2r_2s_pi},
    {"ld2r_2s_pr", f_ld2r_2s_pr},
    {"ld2r_4s", f_ld2r_4s},
    {"ld2r_4s_pi", f_ld2r_4s_pi},
    {"ld2r_4s_pr", f_ld2r_4s_pr},
    {"ld2r_1d", f_ld2r_1d},
    {"ld2r_1d_pi", f_ld2r_1d_pi},
    {"ld2r_1d_pr", f_ld2r_1d_pr},
    {"ld2r_2d", f_ld2r_2d},
    {"ld2r_2d_pi", f_ld2r_2d_pi},
    {"ld2r_2d_pr", f_ld2r_2d_pr},
    {"ld3r_8b", f_ld3r_8b},
    {"ld3r_8b_pi", f_ld3r_8b_pi},
    {"ld3r_8b_pr", f_ld3r_8b_pr},
    {"ld3r_16b", f_ld3r_16b},
    {"ld3r_16b_pi", f_ld3r_16b_pi},
    {"ld3r_16b_pr", f_ld3r_16b_pr},
    {"ld3r_4h", f_ld3r_4h},
    {"ld3r_4h_pi", f_ld3r_4h_pi},
    {"ld3r_4h_pr", f_ld3r_4h_pr},
    {"ld3r_8h", f_ld3r_8h},
    {"ld3r_8h_pi", f_ld3r_8h_pi},
    {"ld3r_8h_pr", f_ld3r_8h_pr},
    {"ld3r_2s", f_ld3r_2s},
    {"ld3r_2s_pi", f_ld3r_2s_pi},
    {"ld3r_2s_pr", f_ld3r_2s_pr},
    {"ld3r_4s", f_ld3r_4s},
    {"ld3r_4s_pi", f_ld3r_4s_pi},
    {"ld3r_4s_pr", f_ld3r_4s_pr},
    {"ld3r_1d", f_ld3r_1d},
    {"ld3r_1d_pi", f_ld3r_1d_pi},
    {"ld3r_1d_pr", f_ld3r_1d_pr},
    {"ld3r_2d", f_ld3r_2d},
    {"ld3r_2d_pi", f_ld3r_2d_pi},
    {"ld3r_2d_pr", f_ld3r_2d_pr},
    {"ld4r_8b", f_ld4r_8b},
    {"ld4r_8b_pi", f_ld4r_8b_pi},
    {"ld4r_8b_pr", f_ld4r_8b_pr},
    {"ld4r_16b", f_ld4r_16b},
    {"ld4r_16b_pi", f_ld4r_16b_pi},
    {"ld4r_16b_pr", f_ld4r_16b_pr},
    {"ld4r_4h", f_ld4r_4h},
    {"ld4r_4h_pi", f_ld4r_4h_pi},
    {"ld4r_4h_pr", f_ld4r_4h_pr},
    {"ld4r_8h", f_ld4r_8h},
    {"ld4r_8h_pi", f_ld4r_8h_pi},
    {"ld4r_8h_pr", f_ld4r_8h_pr},
    {"ld4r_2s", f_ld4r_2s},
    {"ld4r_2s_pi", f_ld4r_2s_pi},
    {"ld4r_2s_pr", f_ld4r_2s_pr},
    {"ld4r_4s", f_ld4r_4s},
    {"ld4r_4s_pi", f_ld4r_4s_pi},
    {"ld4r_4s_pr", f_ld4r_4s_pr},
    {"ld4r_1d", f_ld4r_1d},
    {"ld4r_1d_pi", f_ld4r_1d_pi},
    {"ld4r_1d_pr", f_ld4r_1d_pr},
    {"ld4r_2d", f_ld4r_2d},
    {"ld4r_2d_pi", f_ld4r_2d_pi},
    {"ld4r_2d_pr", f_ld4r_2d_pr},
    {"ldr_b", f_ldr_b},
    {"str_b", f_str_b},
    {"ldur_b", f_ldur_b},
    {"stur_b", f_stur_b},
    {"ldr_b_post", f_ldr_b_post},
    {"str_b_post", f_str_b_post},
    {"ldr_b_pre", f_ldr_b_pre},
    {"str_b_pre", f_str_b_pre},
    {"ldr_h", f_ldr_h},
    {"str_h", f_str_h},
    {"ldur_h", f_ldur_h},
    {"stur_h", f_stur_h},
    {"ldr_h_post", f_ldr_h_post},
    {"str_h_post", f_str_h_post},
    {"ldr_h_pre", f_ldr_h_pre},
    {"str_h_pre", f_str_h_pre},
  };
  int count = (int)(sizeof ops / sizeof ops[0]);
  static State s;
  for (int i = 0; i < n; i++) {
    for (int o = 0; o < count; o++) {
      for (int k = 0; k < 8; k++) s.reg[k] = rnd();
      for (int k = 0; k < 512; k += 8) { u64 v = rnd(); memcpy(s.buf + k, &v, 8); }
      u64 inc = 16 + 16 * (rnd() % 3);
      ops[o].fn(&s, inc);
      u64 bh = 1469598103934665603ull;
      for (int k = 0; k < 512; k += 8) { u64 v; memcpy(&v, s.buf + k, 8); bh ^= v; bh *= 1099511628211ull; }
      for (int k = 0; k < 8; k++) mix(s.reg[k]);
      mix(s.base); mix(bh);
      if (dump) printf("%s %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %llx %016llx\n", ops[o].name,
                       s.reg[0], s.reg[1], s.reg[2], s.reg[3], s.reg[4], s.reg[5], s.reg[6], s.reg[7], s.base, bh);
    }
  }
  printf("hash %016llx\n", h);
  return 0;
}
