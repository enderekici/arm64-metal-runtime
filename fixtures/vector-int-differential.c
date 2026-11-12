#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long long u64;
typedef u64 V __attribute__((vector_size(16)));
static u64 st = 0x243f6a8885a308d3ull;
static u64 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static u64 pick(void) {
  switch (rnd() % 8) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return 0x8080808080808080ull;
    case 3: return 0x7f7f7f7f7f7f7f7full;
    case 4: return rnd() >> (rnd() % 64);
    case 5: return 0x0001000100010001ull * (rnd() & 0xffff);
    default: return rnd();
  }
}
static u64 h = 1469598103934665603ull;
static void mix(u64 v) { h ^= v; h *= 1099511628211ull; }
static int dump;
typedef V (*Fn)(V, V, V);
typedef struct { const char *name; Fn fn; } Op;
#define OP(nm, ins) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins : "+w"(r) : "w"(a), "w"(b)); return r; }
OP(smax_8b, "smax %0.8b, %1.8b, %2.8b")
OP(smax_16b, "smax %0.16b, %1.16b, %2.16b")
OP(smax_4h, "smax %0.4h, %1.4h, %2.4h")
OP(smax_8h, "smax %0.8h, %1.8h, %2.8h")
OP(smax_2s, "smax %0.2s, %1.2s, %2.2s")
OP(smax_4s, "smax %0.4s, %1.4s, %2.4s")
OP(smin_8b, "smin %0.8b, %1.8b, %2.8b")
OP(smin_16b, "smin %0.16b, %1.16b, %2.16b")
OP(smin_4h, "smin %0.4h, %1.4h, %2.4h")
OP(smin_8h, "smin %0.8h, %1.8h, %2.8h")
OP(smin_2s, "smin %0.2s, %1.2s, %2.2s")
OP(smin_4s, "smin %0.4s, %1.4s, %2.4s")
OP(umax_8b, "umax %0.8b, %1.8b, %2.8b")
OP(umax_16b, "umax %0.16b, %1.16b, %2.16b")
OP(umax_4h, "umax %0.4h, %1.4h, %2.4h")
OP(umax_8h, "umax %0.8h, %1.8h, %2.8h")
OP(umax_2s, "umax %0.2s, %1.2s, %2.2s")
OP(umax_4s, "umax %0.4s, %1.4s, %2.4s")
OP(umin_8b, "umin %0.8b, %1.8b, %2.8b")
OP(umin_16b, "umin %0.16b, %1.16b, %2.16b")
OP(umin_4h, "umin %0.4h, %1.4h, %2.4h")
OP(umin_8h, "umin %0.8h, %1.8h, %2.8h")
OP(umin_2s, "umin %0.2s, %1.2s, %2.2s")
OP(umin_4s, "umin %0.4s, %1.4s, %2.4s")
OP(smaxp_8b, "smaxp %0.8b, %1.8b, %2.8b")
OP(smaxp_16b, "smaxp %0.16b, %1.16b, %2.16b")
OP(smaxp_4h, "smaxp %0.4h, %1.4h, %2.4h")
OP(smaxp_8h, "smaxp %0.8h, %1.8h, %2.8h")
OP(smaxp_2s, "smaxp %0.2s, %1.2s, %2.2s")
OP(smaxp_4s, "smaxp %0.4s, %1.4s, %2.4s")
OP(sminp_8b, "sminp %0.8b, %1.8b, %2.8b")
OP(sminp_16b, "sminp %0.16b, %1.16b, %2.16b")
OP(sminp_4h, "sminp %0.4h, %1.4h, %2.4h")
OP(sminp_8h, "sminp %0.8h, %1.8h, %2.8h")
OP(sminp_2s, "sminp %0.2s, %1.2s, %2.2s")
OP(sminp_4s, "sminp %0.4s, %1.4s, %2.4s")
OP(uminp_8b, "uminp %0.8b, %1.8b, %2.8b")
OP(uminp_16b, "uminp %0.16b, %1.16b, %2.16b")
OP(uminp_4h, "uminp %0.4h, %1.4h, %2.4h")
OP(uminp_8h, "uminp %0.8h, %1.8h, %2.8h")
OP(uminp_2s, "uminp %0.2s, %1.2s, %2.2s")
OP(uminp_4s, "uminp %0.4s, %1.4s, %2.4s")
OP(umaxp_8b, "umaxp %0.8b, %1.8b, %2.8b")
OP(umaxp_16b, "umaxp %0.16b, %1.16b, %2.16b")
OP(umaxp_4h, "umaxp %0.4h, %1.4h, %2.4h")
OP(umaxp_8h, "umaxp %0.8h, %1.8h, %2.8h")
OP(umaxp_2s, "umaxp %0.2s, %1.2s, %2.2s")
OP(umaxp_4s, "umaxp %0.4s, %1.4s, %2.4s")
OP(addp_8b, "addp %0.8b, %1.8b, %2.8b")
OP(addp_16b, "addp %0.16b, %1.16b, %2.16b")
OP(addp_4h, "addp %0.4h, %1.4h, %2.4h")
OP(addp_8h, "addp %0.8h, %1.8h, %2.8h")
OP(addp_2s, "addp %0.2s, %1.2s, %2.2s")
OP(addp_4s, "addp %0.4s, %1.4s, %2.4s")
OP(addp_2d, "addp %0.2d, %1.2d, %2.2d")
OP(cmtst_8b, "cmtst %0.8b, %1.8b, %2.8b")
OP(cmtst_16b, "cmtst %0.16b, %1.16b, %2.16b")
OP(cmtst_4h, "cmtst %0.4h, %1.4h, %2.4h")
OP(cmtst_8h, "cmtst %0.8h, %1.8h, %2.8h")
OP(cmtst_2s, "cmtst %0.2s, %1.2s, %2.2s")
OP(cmtst_4s, "cmtst %0.4s, %1.4s, %2.4s")
OP(cmtst_2d, "cmtst %0.2d, %1.2d, %2.2d")
OP(cmgt0_8b, "cmgt %0.8b, %1.8b, #0")
OP(cmgt0_16b, "cmgt %0.16b, %1.16b, #0")
OP(cmgt0_4h, "cmgt %0.4h, %1.4h, #0")
OP(cmgt0_8h, "cmgt %0.8h, %1.8h, #0")
OP(cmgt0_2s, "cmgt %0.2s, %1.2s, #0")
OP(cmgt0_4s, "cmgt %0.4s, %1.4s, #0")
OP(cmgt0_2d, "cmgt %0.2d, %1.2d, #0")
OP(cmge0_8b, "cmge %0.8b, %1.8b, #0")
OP(cmge0_16b, "cmge %0.16b, %1.16b, #0")
OP(cmge0_4h, "cmge %0.4h, %1.4h, #0")
OP(cmge0_8h, "cmge %0.8h, %1.8h, #0")
OP(cmge0_2s, "cmge %0.2s, %1.2s, #0")
OP(cmge0_4s, "cmge %0.4s, %1.4s, #0")
OP(cmge0_2d, "cmge %0.2d, %1.2d, #0")
OP(cmle0_8b, "cmle %0.8b, %1.8b, #0")
OP(cmle0_16b, "cmle %0.16b, %1.16b, #0")
OP(cmle0_4h, "cmle %0.4h, %1.4h, #0")
OP(cmle0_8h, "cmle %0.8h, %1.8h, #0")
OP(cmle0_2s, "cmle %0.2s, %1.2s, #0")
OP(cmle0_4s, "cmle %0.4s, %1.4s, #0")
OP(cmle0_2d, "cmle %0.2d, %1.2d, #0")
OP(cmlt0_8b, "cmlt %0.8b, %1.8b, #0")
OP(cmlt0_16b, "cmlt %0.16b, %1.16b, #0")
OP(cmlt0_4h, "cmlt %0.4h, %1.4h, #0")
OP(cmlt0_8h, "cmlt %0.8h, %1.8h, #0")
OP(cmlt0_2s, "cmlt %0.2s, %1.2s, #0")
OP(cmlt0_4s, "cmlt %0.4s, %1.4s, #0")
OP(cmlt0_2d, "cmlt %0.2d, %1.2d, #0")
OP(abs_8b, "abs %0.8b, %1.8b")
OP(abs_16b, "abs %0.16b, %1.16b")
OP(abs_4h, "abs %0.4h, %1.4h")
OP(abs_8h, "abs %0.8h, %1.8h")
OP(abs_2s, "abs %0.2s, %1.2s")
OP(abs_4s, "abs %0.4s, %1.4s")
OP(abs_2d, "abs %0.2d, %1.2d")
OP(neg_8b, "neg %0.8b, %1.8b")
OP(neg_16b, "neg %0.16b, %1.16b")
OP(neg_4h, "neg %0.4h, %1.4h")
OP(neg_8h, "neg %0.8h, %1.8h")
OP(neg_2s, "neg %0.2s, %1.2s")
OP(neg_4s, "neg %0.4s, %1.4s")
OP(neg_2d, "neg %0.2d, %1.2d")
OP(saddlp_4h_8b, "saddlp %0.4h, %1.8b")
OP(saddlp_8h_16b, "saddlp %0.8h, %1.16b")
OP(saddlp_2s_4h, "saddlp %0.2s, %1.4h")
OP(saddlp_4s_8h, "saddlp %0.4s, %1.8h")
OP(saddlp_1d_2s, "saddlp %0.1d, %1.2s")
OP(saddlp_2d_4s, "saddlp %0.2d, %1.4s")
OP(uaddlp_4h_8b, "uaddlp %0.4h, %1.8b")
OP(uaddlp_8h_16b, "uaddlp %0.8h, %1.16b")
OP(uaddlp_2s_4h, "uaddlp %0.2s, %1.4h")
OP(uaddlp_4s_8h, "uaddlp %0.4s, %1.8h")
OP(uaddlp_1d_2s, "uaddlp %0.1d, %1.2s")
OP(uaddlp_2d_4s, "uaddlp %0.2d, %1.4s")
OP(sadalp_4h_8b, "sadalp %0.4h, %1.8b")
OP(sadalp_8h_16b, "sadalp %0.8h, %1.16b")
OP(sadalp_2s_4h, "sadalp %0.2s, %1.4h")
OP(sadalp_4s_8h, "sadalp %0.4s, %1.8h")
OP(sadalp_1d_2s, "sadalp %0.1d, %1.2s")
OP(sadalp_2d_4s, "sadalp %0.2d, %1.4s")
OP(uadalp_4h_8b, "uadalp %0.4h, %1.8b")
OP(uadalp_8h_16b, "uadalp %0.8h, %1.16b")
OP(uadalp_2s_4h, "uadalp %0.2s, %1.4h")
OP(uadalp_4s_8h, "uadalp %0.4s, %1.8h")
OP(uadalp_1d_2s, "uadalp %0.1d, %1.2s")
OP(uadalp_2d_4s, "uadalp %0.2d, %1.4s")
OP(saddl_8h_8b, "saddl %0.8h, %1.8b, %2.8b")
OP(saddl2_8h_16b, "saddl2 %0.8h, %1.16b, %2.16b")
OP(saddl_4s_4h, "saddl %0.4s, %1.4h, %2.4h")
OP(saddl2_4s_8h, "saddl2 %0.4s, %1.8h, %2.8h")
OP(saddl_2d_2s, "saddl %0.2d, %1.2s, %2.2s")
OP(saddl2_2d_4s, "saddl2 %0.2d, %1.4s, %2.4s")
OP(uaddl_8h_8b, "uaddl %0.8h, %1.8b, %2.8b")
OP(uaddl2_8h_16b, "uaddl2 %0.8h, %1.16b, %2.16b")
OP(uaddl_4s_4h, "uaddl %0.4s, %1.4h, %2.4h")
OP(uaddl2_4s_8h, "uaddl2 %0.4s, %1.8h, %2.8h")
OP(uaddl_2d_2s, "uaddl %0.2d, %1.2s, %2.2s")
OP(uaddl2_2d_4s, "uaddl2 %0.2d, %1.4s, %2.4s")
OP(ssubl_8h_8b, "ssubl %0.8h, %1.8b, %2.8b")
OP(ssubl2_8h_16b, "ssubl2 %0.8h, %1.16b, %2.16b")
OP(ssubl_4s_4h, "ssubl %0.4s, %1.4h, %2.4h")
OP(ssubl2_4s_8h, "ssubl2 %0.4s, %1.8h, %2.8h")
OP(ssubl_2d_2s, "ssubl %0.2d, %1.2s, %2.2s")
OP(ssubl2_2d_4s, "ssubl2 %0.2d, %1.4s, %2.4s")
OP(usubl_8h_8b, "usubl %0.8h, %1.8b, %2.8b")
OP(usubl2_8h_16b, "usubl2 %0.8h, %1.16b, %2.16b")
OP(usubl_4s_4h, "usubl %0.4s, %1.4h, %2.4h")
OP(usubl2_4s_8h, "usubl2 %0.4s, %1.8h, %2.8h")
OP(usubl_2d_2s, "usubl %0.2d, %1.2s, %2.2s")
OP(usubl2_2d_4s, "usubl2 %0.2d, %1.4s, %2.4s")
OP(sabdl_8h_8b, "sabdl %0.8h, %1.8b, %2.8b")
OP(sabdl2_8h_16b, "sabdl2 %0.8h, %1.16b, %2.16b")
OP(sabdl_4s_4h, "sabdl %0.4s, %1.4h, %2.4h")
OP(sabdl2_4s_8h, "sabdl2 %0.4s, %1.8h, %2.8h")
OP(sabdl_2d_2s, "sabdl %0.2d, %1.2s, %2.2s")
OP(sabdl2_2d_4s, "sabdl2 %0.2d, %1.4s, %2.4s")
OP(uabdl_8h_8b, "uabdl %0.8h, %1.8b, %2.8b")
OP(uabdl2_8h_16b, "uabdl2 %0.8h, %1.16b, %2.16b")
OP(uabdl_4s_4h, "uabdl %0.4s, %1.4h, %2.4h")
OP(uabdl2_4s_8h, "uabdl2 %0.4s, %1.8h, %2.8h")
OP(uabdl_2d_2s, "uabdl %0.2d, %1.2s, %2.2s")
OP(uabdl2_2d_4s, "uabdl2 %0.2d, %1.4s, %2.4s")
OP(sabal_8h_8b, "sabal %0.8h, %1.8b, %2.8b")
OP(sabal2_8h_16b, "sabal2 %0.8h, %1.16b, %2.16b")
OP(sabal_4s_4h, "sabal %0.4s, %1.4h, %2.4h")
OP(sabal2_4s_8h, "sabal2 %0.4s, %1.8h, %2.8h")
OP(sabal_2d_2s, "sabal %0.2d, %1.2s, %2.2s")
OP(sabal2_2d_4s, "sabal2 %0.2d, %1.4s, %2.4s")
OP(uabal_8h_8b, "uabal %0.8h, %1.8b, %2.8b")
OP(uabal2_8h_16b, "uabal2 %0.8h, %1.16b, %2.16b")
OP(uabal_4s_4h, "uabal %0.4s, %1.4h, %2.4h")
OP(uabal2_4s_8h, "uabal2 %0.4s, %1.8h, %2.8h")
OP(uabal_2d_2s, "uabal %0.2d, %1.2s, %2.2s")
OP(uabal2_2d_4s, "uabal2 %0.2d, %1.4s, %2.4s")
OP(smlal_8h_8b, "smlal %0.8h, %1.8b, %2.8b")
OP(smlal2_8h_16b, "smlal2 %0.8h, %1.16b, %2.16b")
OP(smlal_4s_4h, "smlal %0.4s, %1.4h, %2.4h")
OP(smlal2_4s_8h, "smlal2 %0.4s, %1.8h, %2.8h")
OP(smlal_2d_2s, "smlal %0.2d, %1.2s, %2.2s")
OP(smlal2_2d_4s, "smlal2 %0.2d, %1.4s, %2.4s")
OP(umlal_8h_8b, "umlal %0.8h, %1.8b, %2.8b")
OP(umlal2_8h_16b, "umlal2 %0.8h, %1.16b, %2.16b")
OP(umlal_4s_4h, "umlal %0.4s, %1.4h, %2.4h")
OP(umlal2_4s_8h, "umlal2 %0.4s, %1.8h, %2.8h")
OP(umlal_2d_2s, "umlal %0.2d, %1.2s, %2.2s")
OP(umlal2_2d_4s, "umlal2 %0.2d, %1.4s, %2.4s")
OP(smlsl_8h_8b, "smlsl %0.8h, %1.8b, %2.8b")
OP(smlsl2_8h_16b, "smlsl2 %0.8h, %1.16b, %2.16b")
OP(smlsl_4s_4h, "smlsl %0.4s, %1.4h, %2.4h")
OP(smlsl2_4s_8h, "smlsl2 %0.4s, %1.8h, %2.8h")
OP(smlsl_2d_2s, "smlsl %0.2d, %1.2s, %2.2s")
OP(smlsl2_2d_4s, "smlsl2 %0.2d, %1.4s, %2.4s")
OP(umlsl_8h_8b, "umlsl %0.8h, %1.8b, %2.8b")
OP(umlsl2_8h_16b, "umlsl2 %0.8h, %1.16b, %2.16b")
OP(umlsl_4s_4h, "umlsl %0.4s, %1.4h, %2.4h")
OP(umlsl2_4s_8h, "umlsl2 %0.4s, %1.8h, %2.8h")
OP(umlsl_2d_2s, "umlsl %0.2d, %1.2s, %2.2s")
OP(umlsl2_2d_4s, "umlsl2 %0.2d, %1.4s, %2.4s")
OP(smull_8h_8b, "smull %0.8h, %1.8b, %2.8b")
OP(smull2_8h_16b, "smull2 %0.8h, %1.16b, %2.16b")
OP(smull_4s_4h, "smull %0.4s, %1.4h, %2.4h")
OP(smull2_4s_8h, "smull2 %0.4s, %1.8h, %2.8h")
OP(smull_2d_2s, "smull %0.2d, %1.2s, %2.2s")
OP(smull2_2d_4s, "smull2 %0.2d, %1.4s, %2.4s")
OP(umull_8h_8b, "umull %0.8h, %1.8b, %2.8b")
OP(umull2_8h_16b, "umull2 %0.8h, %1.16b, %2.16b")
OP(umull_4s_4h, "umull %0.4s, %1.4h, %2.4h")
OP(umull2_4s_8h, "umull2 %0.4s, %1.8h, %2.8h")
OP(umull_2d_2s, "umull %0.2d, %1.2s, %2.2s")
OP(umull2_2d_4s, "umull2 %0.2d, %1.4s, %2.4s")
OP(saddw_8h_8b, "saddw %0.8h, %1.8h, %2.8b")
OP(saddw2_8h_16b, "saddw2 %0.8h, %1.8h, %2.16b")
OP(saddw_4s_4h, "saddw %0.4s, %1.4s, %2.4h")
OP(saddw2_4s_8h, "saddw2 %0.4s, %1.4s, %2.8h")
OP(saddw_2d_2s, "saddw %0.2d, %1.2d, %2.2s")
OP(saddw2_2d_4s, "saddw2 %0.2d, %1.2d, %2.4s")
OP(uaddw_8h_8b, "uaddw %0.8h, %1.8h, %2.8b")
OP(uaddw2_8h_16b, "uaddw2 %0.8h, %1.8h, %2.16b")
OP(uaddw_4s_4h, "uaddw %0.4s, %1.4s, %2.4h")
OP(uaddw2_4s_8h, "uaddw2 %0.4s, %1.4s, %2.8h")
OP(uaddw_2d_2s, "uaddw %0.2d, %1.2d, %2.2s")
OP(uaddw2_2d_4s, "uaddw2 %0.2d, %1.2d, %2.4s")
OP(ssubw_8h_8b, "ssubw %0.8h, %1.8h, %2.8b")
OP(ssubw2_8h_16b, "ssubw2 %0.8h, %1.8h, %2.16b")
OP(ssubw_4s_4h, "ssubw %0.4s, %1.4s, %2.4h")
OP(ssubw2_4s_8h, "ssubw2 %0.4s, %1.4s, %2.8h")
OP(ssubw_2d_2s, "ssubw %0.2d, %1.2d, %2.2s")
OP(ssubw2_2d_4s, "ssubw2 %0.2d, %1.2d, %2.4s")
OP(usubw_8h_8b, "usubw %0.8h, %1.8h, %2.8b")
OP(usubw2_8h_16b, "usubw2 %0.8h, %1.8h, %2.16b")
OP(usubw_4s_4h, "usubw %0.4s, %1.4s, %2.4h")
OP(usubw2_4s_8h, "usubw2 %0.4s, %1.4s, %2.8h")
OP(usubw_2d_2s, "usubw %0.2d, %1.2d, %2.2s")
OP(usubw2_2d_4s, "usubw2 %0.2d, %1.2d, %2.4s")
OP(orr_4s_l0, "orr %0.4s, #0x4b, lsl #0")
OP(bic_4s_l0, "bic %0.4s, #0x4b, lsl #0")
OP(movi_4s_l0, "movi %0.4s, #0x4b, lsl #0")
OP(mvni_4s_l0, "mvni %0.4s, #0x4b, lsl #0")
OP(orr_4s_l8, "orr %0.4s, #0x4b, lsl #8")
OP(bic_4s_l8, "bic %0.4s, #0x4b, lsl #8")
OP(movi_4s_l8, "movi %0.4s, #0x4b, lsl #8")
OP(mvni_4s_l8, "mvni %0.4s, #0x4b, lsl #8")
OP(orr_4s_l16, "orr %0.4s, #0x4b, lsl #16")
OP(bic_4s_l16, "bic %0.4s, #0x4b, lsl #16")
OP(movi_4s_l16, "movi %0.4s, #0x4b, lsl #16")
OP(mvni_4s_l16, "mvni %0.4s, #0x4b, lsl #16")
OP(orr_4s_l24, "orr %0.4s, #0x4b, lsl #24")
OP(bic_4s_l24, "bic %0.4s, #0x4b, lsl #24")
OP(movi_4s_l24, "movi %0.4s, #0x4b, lsl #24")
OP(mvni_4s_l24, "mvni %0.4s, #0x4b, lsl #24")
OP(movi_4s_m8, "movi %0.4s, #0xa7, msl #8")
OP(mvni_4s_m8, "mvni %0.4s, #0xa7, msl #8")
OP(movi_4s_m16, "movi %0.4s, #0xa7, msl #16")
OP(mvni_4s_m16, "mvni %0.4s, #0xa7, msl #16")
OP(orr_2s_l0, "orr %0.2s, #0x4b, lsl #0")
OP(bic_2s_l0, "bic %0.2s, #0x4b, lsl #0")
OP(movi_2s_l0, "movi %0.2s, #0x4b, lsl #0")
OP(mvni_2s_l0, "mvni %0.2s, #0x4b, lsl #0")
OP(orr_2s_l8, "orr %0.2s, #0x4b, lsl #8")
OP(bic_2s_l8, "bic %0.2s, #0x4b, lsl #8")
OP(movi_2s_l8, "movi %0.2s, #0x4b, lsl #8")
OP(mvni_2s_l8, "mvni %0.2s, #0x4b, lsl #8")
OP(orr_2s_l16, "orr %0.2s, #0x4b, lsl #16")
OP(bic_2s_l16, "bic %0.2s, #0x4b, lsl #16")
OP(movi_2s_l16, "movi %0.2s, #0x4b, lsl #16")
OP(mvni_2s_l16, "mvni %0.2s, #0x4b, lsl #16")
OP(orr_2s_l24, "orr %0.2s, #0x4b, lsl #24")
OP(bic_2s_l24, "bic %0.2s, #0x4b, lsl #24")
OP(movi_2s_l24, "movi %0.2s, #0x4b, lsl #24")
OP(mvni_2s_l24, "mvni %0.2s, #0x4b, lsl #24")
OP(movi_2s_m8, "movi %0.2s, #0xa7, msl #8")
OP(mvni_2s_m8, "mvni %0.2s, #0xa7, msl #8")
OP(movi_2s_m16, "movi %0.2s, #0xa7, msl #16")
OP(mvni_2s_m16, "mvni %0.2s, #0xa7, msl #16")
OP(orr_8h_l0, "orr %0.8h, #0x93, lsl #0")
OP(bic_8h_l0, "bic %0.8h, #0x93, lsl #0")
OP(movi_8h_l0, "movi %0.8h, #0x93, lsl #0")
OP(mvni_8h_l0, "mvni %0.8h, #0x93, lsl #0")
OP(orr_8h_l8, "orr %0.8h, #0x93, lsl #8")
OP(bic_8h_l8, "bic %0.8h, #0x93, lsl #8")
OP(movi_8h_l8, "movi %0.8h, #0x93, lsl #8")
OP(mvni_8h_l8, "mvni %0.8h, #0x93, lsl #8")
OP(orr_4h_l0, "orr %0.4h, #0x93, lsl #0")
OP(bic_4h_l0, "bic %0.4h, #0x93, lsl #0")
OP(movi_4h_l0, "movi %0.4h, #0x93, lsl #0")
OP(mvni_4h_l0, "mvni %0.4h, #0x93, lsl #0")
OP(orr_4h_l8, "orr %0.4h, #0x93, lsl #8")
OP(bic_4h_l8, "bic %0.4h, #0x93, lsl #8")
OP(movi_4h_l8, "movi %0.4h, #0x93, lsl #8")
OP(mvni_4h_l8, "mvni %0.4h, #0x93, lsl #8")
OP(movi_16b, "movi %0.16b, #0xc5")
OP(movi_8b, "movi %0.8b, #0xc5")
OP(movi_2d, "movi %0.2d, #0xff00ff00ff00ff00")
OP(movi_d, "movi %d0, #0xff0000ff00ff00ff")
OP(hint_yield, "yield")
int main(int argc, char **argv) {
  int n = 200;
  if (argc > 1) { n = 0; for (char *p = argv[1]; *p; p++) n = n * 10 + (*p - '0'); }
  dump = argc > 2;
  static const Op ops[] = {
    {"smax_8b", f_smax_8b},
    {"smax_16b", f_smax_16b},
    {"smax_4h", f_smax_4h},
    {"smax_8h", f_smax_8h},
    {"smax_2s", f_smax_2s},
    {"smax_4s", f_smax_4s},
    {"smin_8b", f_smin_8b},
    {"smin_16b", f_smin_16b},
    {"smin_4h", f_smin_4h},
    {"smin_8h", f_smin_8h},
    {"smin_2s", f_smin_2s},
    {"smin_4s", f_smin_4s},
    {"umax_8b", f_umax_8b},
    {"umax_16b", f_umax_16b},
    {"umax_4h", f_umax_4h},
    {"umax_8h", f_umax_8h},
    {"umax_2s", f_umax_2s},
    {"umax_4s", f_umax_4s},
    {"umin_8b", f_umin_8b},
    {"umin_16b", f_umin_16b},
    {"umin_4h", f_umin_4h},
    {"umin_8h", f_umin_8h},
    {"umin_2s", f_umin_2s},
    {"umin_4s", f_umin_4s},
    {"smaxp_8b", f_smaxp_8b},
    {"smaxp_16b", f_smaxp_16b},
    {"smaxp_4h", f_smaxp_4h},
    {"smaxp_8h", f_smaxp_8h},
    {"smaxp_2s", f_smaxp_2s},
    {"smaxp_4s", f_smaxp_4s},
    {"sminp_8b", f_sminp_8b},
    {"sminp_16b", f_sminp_16b},
    {"sminp_4h", f_sminp_4h},
    {"sminp_8h", f_sminp_8h},
    {"sminp_2s", f_sminp_2s},
    {"sminp_4s", f_sminp_4s},
    {"uminp_8b", f_uminp_8b},
    {"uminp_16b", f_uminp_16b},
    {"uminp_4h", f_uminp_4h},
    {"uminp_8h", f_uminp_8h},
    {"uminp_2s", f_uminp_2s},
    {"uminp_4s", f_uminp_4s},
    {"umaxp_8b", f_umaxp_8b},
    {"umaxp_16b", f_umaxp_16b},
    {"umaxp_4h", f_umaxp_4h},
    {"umaxp_8h", f_umaxp_8h},
    {"umaxp_2s", f_umaxp_2s},
    {"umaxp_4s", f_umaxp_4s},
    {"addp_8b", f_addp_8b},
    {"addp_16b", f_addp_16b},
    {"addp_4h", f_addp_4h},
    {"addp_8h", f_addp_8h},
    {"addp_2s", f_addp_2s},
    {"addp_4s", f_addp_4s},
    {"addp_2d", f_addp_2d},
    {"cmtst_8b", f_cmtst_8b},
    {"cmtst_16b", f_cmtst_16b},
    {"cmtst_4h", f_cmtst_4h},
    {"cmtst_8h", f_cmtst_8h},
    {"cmtst_2s", f_cmtst_2s},
    {"cmtst_4s", f_cmtst_4s},
    {"cmtst_2d", f_cmtst_2d},
    {"cmgt0_8b", f_cmgt0_8b},
    {"cmgt0_16b", f_cmgt0_16b},
    {"cmgt0_4h", f_cmgt0_4h},
    {"cmgt0_8h", f_cmgt0_8h},
    {"cmgt0_2s", f_cmgt0_2s},
    {"cmgt0_4s", f_cmgt0_4s},
    {"cmgt0_2d", f_cmgt0_2d},
    {"cmge0_8b", f_cmge0_8b},
    {"cmge0_16b", f_cmge0_16b},
    {"cmge0_4h", f_cmge0_4h},
    {"cmge0_8h", f_cmge0_8h},
    {"cmge0_2s", f_cmge0_2s},
    {"cmge0_4s", f_cmge0_4s},
    {"cmge0_2d", f_cmge0_2d},
    {"cmle0_8b", f_cmle0_8b},
    {"cmle0_16b", f_cmle0_16b},
    {"cmle0_4h", f_cmle0_4h},
    {"cmle0_8h", f_cmle0_8h},
    {"cmle0_2s", f_cmle0_2s},
    {"cmle0_4s", f_cmle0_4s},
    {"cmle0_2d", f_cmle0_2d},
    {"cmlt0_8b", f_cmlt0_8b},
    {"cmlt0_16b", f_cmlt0_16b},
    {"cmlt0_4h", f_cmlt0_4h},
    {"cmlt0_8h", f_cmlt0_8h},
    {"cmlt0_2s", f_cmlt0_2s},
    {"cmlt0_4s", f_cmlt0_4s},
    {"cmlt0_2d", f_cmlt0_2d},
    {"abs_8b", f_abs_8b},
    {"abs_16b", f_abs_16b},
    {"abs_4h", f_abs_4h},
    {"abs_8h", f_abs_8h},
    {"abs_2s", f_abs_2s},
    {"abs_4s", f_abs_4s},
    {"abs_2d", f_abs_2d},
    {"neg_8b", f_neg_8b},
    {"neg_16b", f_neg_16b},
    {"neg_4h", f_neg_4h},
    {"neg_8h", f_neg_8h},
    {"neg_2s", f_neg_2s},
    {"neg_4s", f_neg_4s},
    {"neg_2d", f_neg_2d},
    {"saddlp_4h_8b", f_saddlp_4h_8b},
    {"saddlp_8h_16b", f_saddlp_8h_16b},
    {"saddlp_2s_4h", f_saddlp_2s_4h},
    {"saddlp_4s_8h", f_saddlp_4s_8h},
    {"saddlp_1d_2s", f_saddlp_1d_2s},
    {"saddlp_2d_4s", f_saddlp_2d_4s},
    {"uaddlp_4h_8b", f_uaddlp_4h_8b},
    {"uaddlp_8h_16b", f_uaddlp_8h_16b},
    {"uaddlp_2s_4h", f_uaddlp_2s_4h},
    {"uaddlp_4s_8h", f_uaddlp_4s_8h},
    {"uaddlp_1d_2s", f_uaddlp_1d_2s},
    {"uaddlp_2d_4s", f_uaddlp_2d_4s},
    {"sadalp_4h_8b", f_sadalp_4h_8b},
    {"sadalp_8h_16b", f_sadalp_8h_16b},
    {"sadalp_2s_4h", f_sadalp_2s_4h},
    {"sadalp_4s_8h", f_sadalp_4s_8h},
    {"sadalp_1d_2s", f_sadalp_1d_2s},
    {"sadalp_2d_4s", f_sadalp_2d_4s},
    {"uadalp_4h_8b", f_uadalp_4h_8b},
    {"uadalp_8h_16b", f_uadalp_8h_16b},
    {"uadalp_2s_4h", f_uadalp_2s_4h},
    {"uadalp_4s_8h", f_uadalp_4s_8h},
    {"uadalp_1d_2s", f_uadalp_1d_2s},
    {"uadalp_2d_4s", f_uadalp_2d_4s},
    {"saddl_8h_8b", f_saddl_8h_8b},
    {"saddl2_8h_16b", f_saddl2_8h_16b},
    {"saddl_4s_4h", f_saddl_4s_4h},
    {"saddl2_4s_8h", f_saddl2_4s_8h},
    {"saddl_2d_2s", f_saddl_2d_2s},
    {"saddl2_2d_4s", f_saddl2_2d_4s},
    {"uaddl_8h_8b", f_uaddl_8h_8b},
    {"uaddl2_8h_16b", f_uaddl2_8h_16b},
    {"uaddl_4s_4h", f_uaddl_4s_4h},
    {"uaddl2_4s_8h", f_uaddl2_4s_8h},
    {"uaddl_2d_2s", f_uaddl_2d_2s},
    {"uaddl2_2d_4s", f_uaddl2_2d_4s},
    {"ssubl_8h_8b", f_ssubl_8h_8b},
    {"ssubl2_8h_16b", f_ssubl2_8h_16b},
    {"ssubl_4s_4h", f_ssubl_4s_4h},
    {"ssubl2_4s_8h", f_ssubl2_4s_8h},
    {"ssubl_2d_2s", f_ssubl_2d_2s},
    {"ssubl2_2d_4s", f_ssubl2_2d_4s},
    {"usubl_8h_8b", f_usubl_8h_8b},
    {"usubl2_8h_16b", f_usubl2_8h_16b},
    {"usubl_4s_4h", f_usubl_4s_4h},
    {"usubl2_4s_8h", f_usubl2_4s_8h},
    {"usubl_2d_2s", f_usubl_2d_2s},
    {"usubl2_2d_4s", f_usubl2_2d_4s},
    {"sabdl_8h_8b", f_sabdl_8h_8b},
    {"sabdl2_8h_16b", f_sabdl2_8h_16b},
    {"sabdl_4s_4h", f_sabdl_4s_4h},
    {"sabdl2_4s_8h", f_sabdl2_4s_8h},
    {"sabdl_2d_2s", f_sabdl_2d_2s},
    {"sabdl2_2d_4s", f_sabdl2_2d_4s},
    {"uabdl_8h_8b", f_uabdl_8h_8b},
    {"uabdl2_8h_16b", f_uabdl2_8h_16b},
    {"uabdl_4s_4h", f_uabdl_4s_4h},
    {"uabdl2_4s_8h", f_uabdl2_4s_8h},
    {"uabdl_2d_2s", f_uabdl_2d_2s},
    {"uabdl2_2d_4s", f_uabdl2_2d_4s},
    {"sabal_8h_8b", f_sabal_8h_8b},
    {"sabal2_8h_16b", f_sabal2_8h_16b},
    {"sabal_4s_4h", f_sabal_4s_4h},
    {"sabal2_4s_8h", f_sabal2_4s_8h},
    {"sabal_2d_2s", f_sabal_2d_2s},
    {"sabal2_2d_4s", f_sabal2_2d_4s},
    {"uabal_8h_8b", f_uabal_8h_8b},
    {"uabal2_8h_16b", f_uabal2_8h_16b},
    {"uabal_4s_4h", f_uabal_4s_4h},
    {"uabal2_4s_8h", f_uabal2_4s_8h},
    {"uabal_2d_2s", f_uabal_2d_2s},
    {"uabal2_2d_4s", f_uabal2_2d_4s},
    {"smlal_8h_8b", f_smlal_8h_8b},
    {"smlal2_8h_16b", f_smlal2_8h_16b},
    {"smlal_4s_4h", f_smlal_4s_4h},
    {"smlal2_4s_8h", f_smlal2_4s_8h},
    {"smlal_2d_2s", f_smlal_2d_2s},
    {"smlal2_2d_4s", f_smlal2_2d_4s},
    {"umlal_8h_8b", f_umlal_8h_8b},
    {"umlal2_8h_16b", f_umlal2_8h_16b},
    {"umlal_4s_4h", f_umlal_4s_4h},
    {"umlal2_4s_8h", f_umlal2_4s_8h},
    {"umlal_2d_2s", f_umlal_2d_2s},
    {"umlal2_2d_4s", f_umlal2_2d_4s},
    {"smlsl_8h_8b", f_smlsl_8h_8b},
    {"smlsl2_8h_16b", f_smlsl2_8h_16b},
    {"smlsl_4s_4h", f_smlsl_4s_4h},
    {"smlsl2_4s_8h", f_smlsl2_4s_8h},
    {"smlsl_2d_2s", f_smlsl_2d_2s},
    {"smlsl2_2d_4s", f_smlsl2_2d_4s},
    {"umlsl_8h_8b", f_umlsl_8h_8b},
    {"umlsl2_8h_16b", f_umlsl2_8h_16b},
    {"umlsl_4s_4h", f_umlsl_4s_4h},
    {"umlsl2_4s_8h", f_umlsl2_4s_8h},
    {"umlsl_2d_2s", f_umlsl_2d_2s},
    {"umlsl2_2d_4s", f_umlsl2_2d_4s},
    {"smull_8h_8b", f_smull_8h_8b},
    {"smull2_8h_16b", f_smull2_8h_16b},
    {"smull_4s_4h", f_smull_4s_4h},
    {"smull2_4s_8h", f_smull2_4s_8h},
    {"smull_2d_2s", f_smull_2d_2s},
    {"smull2_2d_4s", f_smull2_2d_4s},
    {"umull_8h_8b", f_umull_8h_8b},
    {"umull2_8h_16b", f_umull2_8h_16b},
    {"umull_4s_4h", f_umull_4s_4h},
    {"umull2_4s_8h", f_umull2_4s_8h},
    {"umull_2d_2s", f_umull_2d_2s},
    {"umull2_2d_4s", f_umull2_2d_4s},
    {"saddw_8h_8b", f_saddw_8h_8b},
    {"saddw2_8h_16b", f_saddw2_8h_16b},
    {"saddw_4s_4h", f_saddw_4s_4h},
    {"saddw2_4s_8h", f_saddw2_4s_8h},
    {"saddw_2d_2s", f_saddw_2d_2s},
    {"saddw2_2d_4s", f_saddw2_2d_4s},
    {"uaddw_8h_8b", f_uaddw_8h_8b},
    {"uaddw2_8h_16b", f_uaddw2_8h_16b},
    {"uaddw_4s_4h", f_uaddw_4s_4h},
    {"uaddw2_4s_8h", f_uaddw2_4s_8h},
    {"uaddw_2d_2s", f_uaddw_2d_2s},
    {"uaddw2_2d_4s", f_uaddw2_2d_4s},
    {"ssubw_8h_8b", f_ssubw_8h_8b},
    {"ssubw2_8h_16b", f_ssubw2_8h_16b},
    {"ssubw_4s_4h", f_ssubw_4s_4h},
    {"ssubw2_4s_8h", f_ssubw2_4s_8h},
    {"ssubw_2d_2s", f_ssubw_2d_2s},
    {"ssubw2_2d_4s", f_ssubw2_2d_4s},
    {"usubw_8h_8b", f_usubw_8h_8b},
    {"usubw2_8h_16b", f_usubw2_8h_16b},
    {"usubw_4s_4h", f_usubw_4s_4h},
    {"usubw2_4s_8h", f_usubw2_4s_8h},
    {"usubw_2d_2s", f_usubw_2d_2s},
    {"usubw2_2d_4s", f_usubw2_2d_4s},
    {"orr_4s_l0", f_orr_4s_l0},
    {"bic_4s_l0", f_bic_4s_l0},
    {"movi_4s_l0", f_movi_4s_l0},
    {"mvni_4s_l0", f_mvni_4s_l0},
    {"orr_4s_l8", f_orr_4s_l8},
    {"bic_4s_l8", f_bic_4s_l8},
    {"movi_4s_l8", f_movi_4s_l8},
    {"mvni_4s_l8", f_mvni_4s_l8},
    {"orr_4s_l16", f_orr_4s_l16},
    {"bic_4s_l16", f_bic_4s_l16},
    {"movi_4s_l16", f_movi_4s_l16},
    {"mvni_4s_l16", f_mvni_4s_l16},
    {"orr_4s_l24", f_orr_4s_l24},
    {"bic_4s_l24", f_bic_4s_l24},
    {"movi_4s_l24", f_movi_4s_l24},
    {"mvni_4s_l24", f_mvni_4s_l24},
    {"movi_4s_m8", f_movi_4s_m8},
    {"mvni_4s_m8", f_mvni_4s_m8},
    {"movi_4s_m16", f_movi_4s_m16},
    {"mvni_4s_m16", f_mvni_4s_m16},
    {"orr_2s_l0", f_orr_2s_l0},
    {"bic_2s_l0", f_bic_2s_l0},
    {"movi_2s_l0", f_movi_2s_l0},
    {"mvni_2s_l0", f_mvni_2s_l0},
    {"orr_2s_l8", f_orr_2s_l8},
    {"bic_2s_l8", f_bic_2s_l8},
    {"movi_2s_l8", f_movi_2s_l8},
    {"mvni_2s_l8", f_mvni_2s_l8},
    {"orr_2s_l16", f_orr_2s_l16},
    {"bic_2s_l16", f_bic_2s_l16},
    {"movi_2s_l16", f_movi_2s_l16},
    {"mvni_2s_l16", f_mvni_2s_l16},
    {"orr_2s_l24", f_orr_2s_l24},
    {"bic_2s_l24", f_bic_2s_l24},
    {"movi_2s_l24", f_movi_2s_l24},
    {"mvni_2s_l24", f_mvni_2s_l24},
    {"movi_2s_m8", f_movi_2s_m8},
    {"mvni_2s_m8", f_mvni_2s_m8},
    {"movi_2s_m16", f_movi_2s_m16},
    {"mvni_2s_m16", f_mvni_2s_m16},
    {"orr_8h_l0", f_orr_8h_l0},
    {"bic_8h_l0", f_bic_8h_l0},
    {"movi_8h_l0", f_movi_8h_l0},
    {"mvni_8h_l0", f_mvni_8h_l0},
    {"orr_8h_l8", f_orr_8h_l8},
    {"bic_8h_l8", f_bic_8h_l8},
    {"movi_8h_l8", f_movi_8h_l8},
    {"mvni_8h_l8", f_mvni_8h_l8},
    {"orr_4h_l0", f_orr_4h_l0},
    {"bic_4h_l0", f_bic_4h_l0},
    {"movi_4h_l0", f_movi_4h_l0},
    {"mvni_4h_l0", f_mvni_4h_l0},
    {"orr_4h_l8", f_orr_4h_l8},
    {"bic_4h_l8", f_bic_4h_l8},
    {"movi_4h_l8", f_movi_4h_l8},
    {"mvni_4h_l8", f_mvni_4h_l8},
    {"movi_16b", f_movi_16b},
    {"movi_8b", f_movi_8b},
    {"movi_2d", f_movi_2d},
    {"movi_d", f_movi_d},
    {"hint_yield", f_hint_yield},
  };
  int count = (int)(sizeof ops / sizeof ops[0]);
  for (int i = 0; i < n; i++) {
    for (int o = 0; o < count; o++) {
      V a = {pick(), pick()}, b = {pick(), pick()}, c = {pick(), pick()};
      V r = ops[o].fn(a, b, c);
      mix(r[0]); mix(r[1]);
      if (dump) printf("%s %016llx %016llx\n", ops[o].name, r[0], r[1]);
    }
  }
  printf("hash %016llx\n", h);
  return 0;
}
