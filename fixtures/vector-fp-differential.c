#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long long u64;
typedef u64 V __attribute__((vector_size(16)));
static u64 st = 0x9e3779b97f4a7c15ull;
static u64 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static u64 pick64(void) {
  u64 r = rnd(), sign = (rnd() & 1) << 63;
  switch (r % 12) {
    case 0: return sign | 0x7ff0000000000000ull;
    case 1: return sign | 0x7ff8000000000000ull | (rnd() & 0xff);
    case 2: return sign | 0x7ff0000000000000ull | (rnd() & 0x7ffffffffffffull) | 1;
    case 3: return sign;
    case 4: return sign | (rnd() & 0xfffffffffffffull);
    case 5: return sign | (0x3ffull << 52) | (rnd() & 0xfffffffffffffull);
    case 6: return sign | ((u64)(1023 + (int)(rnd() % 120) - 60) << 52) | (rnd() & 0xfffffffffffffull);
    case 7: return sign | ((u64)(1 + rnd() % 2046) << 52) | (rnd() & 0xfffffffffffffull);
    case 8: return sign | ((u64)(1023 + (int)(rnd() % 8)) << 52) | ((rnd() & 3) << 50);
    default: return sign | ((u64)(1023 + (int)(rnd() % 70) - 5) << 52) | (rnd() & 0xfffffffffffffull);
  }
}
static uint32_t pick32(void) {
  uint32_t r = rnd(), sign = (uint32_t)(rnd() & 1) << 31;
  switch (r % 12) {
    case 0: return sign | 0x7f800000u;
    case 1: return sign | 0x7fc00000u | (rnd() & 0xff);
    case 2: return sign | 0x7f800000u | (rnd() & 0x3fffffu) | 1;
    case 3: return sign;
    case 4: return sign | (rnd() & 0x7fffffu);
    case 5: return sign | (0x7fu << 23) | (rnd() & 0x7fffffu);
    case 6: return sign | ((uint32_t)(127 + (int)(rnd() % 60) - 30) << 23) | (rnd() & 0x7fffffu);
    case 7: return sign | ((uint32_t)(1 + rnd() % 254) << 23) | (rnd() & 0x7fffffu);
    default: return sign | ((uint32_t)(127 + (int)(rnd() % 40) - 4) << 23) | (rnd() & 0x7fffffu);
  }
}
static uint32_t pickint32(void) {
  switch (rnd() % 6) {
    case 0: return 0;
    case 1: return (uint32_t)(int32_t)(rnd() % 7) - 3;
    case 2: return 0x80000000u;
    case 3: return 0x7fffffffu;
    case 4: return (uint32_t)rnd() >> (rnd() % 32);
    default: return (uint32_t)rnd();
  }
}
static u64 pickint64(void) {
  switch (rnd() % 6) {
    case 0: return 0;
    case 1: return (u64)(int64_t)(rnd() % 7) - 3;
    case 2: return 0x8000000000000000ull;
    case 3: return 0x7fffffffffffffffull;
    case 4: return rnd() >> (rnd() % 64);
    default: return rnd();
  }
}
static V gen(int kind) {
  V v;
  if (kind == 0) { uint32_t w[4]; for (int i = 0; i < 4; i++) w[i] = pick32(); memcpy(&v, w, 16); }
  else if (kind == 1) { u64 w[2] = {pick64(), pick64()}; memcpy(&v, w, 16); }
  else if (kind == 2) { uint32_t w[4]; for (int i = 0; i < 4; i++) w[i] = pickint32(); memcpy(&v, w, 16); }
  else { u64 w[2] = {pickint64(), pickint64()}; memcpy(&v, w, 16); }
  return v;
}
static u64 h = 1469598103934665603ull;
static void mix(u64 v) { h ^= v; h *= 1099511628211ull; }
static u64 getfpsr(void) { u64 v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static void clrfpsr(void) { __asm__ volatile("msr fpsr, xzr"); }
static void setfpcr(u64 v) { __asm__ volatile("msr fpcr, %0" ::"r"(v)); }
static int dump;
static void out(const char *tag, V r, u64 fpsr) {
  mix(r[0]); mix(r[1]); mix(fpsr);
  if (dump) printf("%s %016llx %016llx %llx\n", tag, r[0], r[1], fpsr);
}
typedef V (*Fn)(V, V, V);
typedef struct { const char *name; Fn fn; int kind; } Op;
#define B3(nm, ins, arr, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %0." arr ", %1." arr ", %2." arr : "+w"(r) : "w"(a), "w"(b)); return r; }
#define B3N(nm, ins, arr, kind) static V f_##nm(V a, V b, V c) { V r = a; __asm__ volatile(ins " %0." arr ", %0." arr ", %1." arr : "+w"(r) : "w"(b)); return r; }
#define B3M(nm, ins, arr, kind) static V f_##nm(V a, V b, V c) { V r = b; __asm__ volatile(ins " %0." arr ", %1." arr ", %0." arr : "+w"(r) : "w"(a)); return r; }
#define U2(nm, ins, arr, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %0." arr ", %1." arr : "+w"(r) : "w"(a)); return r; }
#define U2Z(nm, ins, arr, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %0." arr ", %1." arr ", #0.0" : "+w"(r) : "w"(a)); return r; }
#define S3(nm, ins, rg, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %" rg "0, %" rg "1, %" rg "2" : "+w"(r) : "w"(a), "w"(b)); return r; }
#define S2(nm, ins, rg, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %" rg "0, %" rg "1" : "+w"(r) : "w"(a)); return r; }
#define S2Z(nm, ins, rg, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %" rg "0, %" rg "1, #0.0" : "+w"(r) : "w"(a)); return r; }
#define SP(nm, ins, dst, src, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %" dst "0, %1." src : "+w"(r) : "w"(a)); return r; }
#define EL(nm, ins, arr, el, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %0." arr ", %1." arr ", %2." el : "+w"(r) : "w"(a), "w"(b)); return r; }
#define SEL(nm, ins, rg, el, kind) static V f_##nm(V a, V b, V c) { V r = c; __asm__ volatile(ins " %" rg "0, %" rg "1, %2." el : "+w"(r) : "w"(a), "w"(b)); return r; }
#define FIXD(nm, ins, rd, rs, kind) static V f_##nm(V a, V b, V c) { V r = c; u64 g; __asm__ volatile(ins : "=r"(g) : "w"(a)); r[0] = g; r[1] = 0; return r; }
#define ALL3(nm, ins) B3(nm##_4s, ins, "4s", 0) B3(nm##_2s, ins, "2s", 0) B3(nm##_2d, ins, "2d", 1)
#define ALL2(nm, ins) U2(nm##_4s, ins, "4s", 0) U2(nm##_2s, ins, "2s", 0) U2(nm##_2d, ins, "2d", 1)
#define ALL2Z(nm, ins) U2Z(nm##_4s, ins, "4s", 0) U2Z(nm##_2s, ins, "2s", 0) U2Z(nm##_2d, ins, "2d", 1)
ALL3(fadd, "fadd") ALL3(fsub, "fsub") ALL3(fmul, "fmul") ALL3(fdiv, "fdiv") ALL3(fmax, "fmax") ALL3(fmin, "fmin")
ALL3(fmaxnm, "fmaxnm") ALL3(fminnm, "fminnm") ALL3(fabd, "fabd") ALL3(fmla, "fmla") ALL3(fmls, "fmls")
ALL3(faddp, "faddp") ALL3(fmaxp, "fmaxp") ALL3(fminp, "fminp") ALL3(fmaxnmp, "fmaxnmp") ALL3(fminnmp, "fminnmp")
ALL3(fcmeq, "fcmeq") ALL3(fcmge, "fcmge") ALL3(fcmgt, "fcmgt") ALL3(facge, "facge") ALL3(facgt, "facgt")
B3N(fmul_rn_4s, "fmul", "4s", 0) B3M(fdiv_rm_2d, "fdiv", "2d", 1) B3N(fmla_rn_2d, "fmla", "2d", 1) B3M(fmls_rm_4s, "fmls", "4s", 0)
B3N(faddp_rn_4s, "faddp", "4s", 0) B3M(fmaxp_rm_2d, "fmaxp", "2d", 1)
ALL2(fabs, "fabs") ALL2(fneg, "fneg") ALL2(fsqrt, "fsqrt")
ALL2(frintn, "frintn") ALL2(frintm, "frintm") ALL2(frintp, "frintp") ALL2(frintz, "frintz") ALL2(frinta, "frinta") ALL2(frintx, "frintx") ALL2(frinti, "frinti")
ALL2(fcvtns, "fcvtns") ALL2(fcvtms, "fcvtms") ALL2(fcvtas, "fcvtas") ALL2(fcvtps, "fcvtps") ALL2(fcvtzs, "fcvtzs")
ALL2(fcvtnu, "fcvtnu") ALL2(fcvtmu, "fcvtmu") ALL2(fcvtau, "fcvtau") ALL2(fcvtpu, "fcvtpu") ALL2(fcvtzu, "fcvtzu")
U2(scvtf_4s, "scvtf", "4s", 2) U2(scvtf_2s, "scvtf", "2s", 2) U2(scvtf_2d, "scvtf", "2d", 3)
U2(ucvtf_4s, "ucvtf", "4s", 2) U2(ucvtf_2s, "ucvtf", "2s", 2) U2(ucvtf_2d, "ucvtf", "2d", 3)
ALL2Z(fcmeq0, "fcmeq") ALL2Z(fcmgt0, "fcmgt") ALL2Z(fcmge0, "fcmge") ALL2Z(fcmle0, "fcmle") ALL2Z(fcmlt0, "fcmlt")
S3(sfabd, "fabd", "s", 0) S3(dfabd, "fabd", "d", 1) S3(sfcmeq, "fcmeq", "s", 0) S3(dfcmeq, "fcmeq", "d", 1)
S3(sfcmge, "fcmge", "s", 0) S3(dfcmge, "fcmge", "d", 1) S3(sfcmgt, "fcmgt", "s", 0) S3(dfcmgt, "fcmgt", "d", 1)
S3(sfacge, "facge", "s", 0) S3(dfacge, "facge", "d", 1) S3(sfacgt, "facgt", "s", 0) S3(dfacgt, "facgt", "d", 1)
SP(sfaddp, "faddp", "s", "2s", 0) SP(dfaddp, "faddp", "d", "2d", 1) SP(sfmaxp, "fmaxp", "s", "2s", 0) SP(dfminp, "fminp", "d", "2d", 1)
SP(sfmaxnmp, "fmaxnmp", "s", "2s", 0) SP(dfminnmp, "fminnmp", "d", "2d", 1)
S2(sfcvtzs, "fcvtzs", "s", 0) S2(dfcvtzs, "fcvtzs", "d", 1) S2(sfcvtnu, "fcvtnu", "s", 0) S2(dfcvtpu, "fcvtpu", "d", 1)
S2(sfcvtas, "fcvtas", "s", 0) S2(dfcvtms, "fcvtms", "d", 1) S2(dfcvtzu, "fcvtzu", "d", 1) S2(sfcvtzu, "fcvtzu", "s", 0)
S2Z(sfcmeq0, "fcmeq", "s", 0) S2Z(dfcmlt0, "fcmlt", "d", 1) S2Z(dfcmge0, "fcmge", "d", 1)
EL(fmla_4s_0, "fmla", "4s", "s[0]", 0) EL(fmla_4s_3, "fmla", "4s", "s[3]", 0) EL(fmla_2s_1, "fmla", "2s", "s[1]", 0)
EL(fmls_4s_2, "fmls", "4s", "s[2]", 0) EL(fmul_4s_1, "fmul", "4s", "s[1]", 0) EL(fmul_2s_3, "fmul", "2s", "s[3]", 0)
EL(fmla_2d_0, "fmla", "2d", "d[0]", 1) EL(fmla_2d_1, "fmla", "2d", "d[1]", 1) EL(fmls_2d_1, "fmls", "2d", "d[1]", 1)
EL(fmul_2d_0, "fmul", "2d", "d[0]", 1) EL(fmul_2d_1, "fmul", "2d", "d[1]", 1)
SEL(sfmul_el, "fmul", "s", "s[2]", 0) SEL(dfmul_el, "fmul", "d", "d[1]", 1) SEL(sfmla_el, "fmla", "s", "s[1]", 0) SEL(dfmls_el, "fmls", "d", "d[0]", 1)
FIXD(fixzs_w_d, "fcvtzs %w0, %d1, #7", 0, 0, 1) FIXD(fixzs_x_d, "fcvtzs %0, %d1, #33", 0, 0, 1) FIXD(fixzu_w_d, "fcvtzu %w0, %d1, #20", 0, 0, 1)
FIXD(fixzu_x_d, "fcvtzu %0, %d1, #64", 0, 0, 1) FIXD(fixzs_w_s, "fcvtzs %w0, %s1, #31", 0, 0, 0) FIXD(fixzs_x_s, "fcvtzs %0, %s1, #3", 0, 0, 0)
FIXD(fixzu_w_s, "fcvtzu %w0, %s1, #1", 0, 0, 0) FIXD(fixzu_x_s, "fcvtzu %0, %s1, #40", 0, 0, 0)
#define FIXI(nm, ins, kind) static V f_##nm(V a, V b, V c) { V r = c; u64 g = a[0]; __asm__ volatile(ins : "=w"(r) : "r"(g)); return r; }
FIXI(fixscvtf_d_w, "scvtf %d0, %w1, #9", 3) FIXI(fixscvtf_d_x, "scvtf %d0, %1, #50", 3) FIXI(fixucvtf_d_w, "ucvtf %d0, %w1, #32", 3)
FIXI(fixucvtf_d_x, "ucvtf %d0, %1, #1", 3) FIXI(fixscvtf_s_w, "scvtf %s0, %w1, #12", 3) FIXI(fixscvtf_s_x, "scvtf %s0, %1, #60", 3)
FIXI(fixucvtf_s_w, "ucvtf %s0, %w1, #1", 3) FIXI(fixucvtf_s_x, "ucvtf %s0, %1, #64", 3)
#define OP(nm) {#nm, f_##nm, K_##nm}
#define KINDS(nm, k) enum { K_##nm = k };
int main(int argc, char **argv) {
  int n = 100;
  if (argc > 1) { n = 0; for (char *p = argv[1]; *p; p++) n = n * 10 + (*p - '0'); }
  dump = argc > 2;
#define L(nm, k) {#nm, f_##nm, k},
#define L3(nm) L(nm##_4s, 0) L(nm##_2s, 0) L(nm##_2d, 1)
  static const Op ops[] = {
    L3(fadd) L3(fsub) L3(fmul) L3(fdiv) L3(fmax) L3(fmin) L3(fmaxnm) L3(fminnm) L3(fabd) L3(fmla) L3(fmls)
    L3(faddp) L3(fmaxp) L3(fminp) L3(fmaxnmp) L3(fminnmp) L3(fcmeq) L3(fcmge) L3(fcmgt) L3(facge) L3(facgt)
    L(fmul_rn_4s, 0) L(fdiv_rm_2d, 1) L(fmla_rn_2d, 1) L(fmls_rm_4s, 0) L(faddp_rn_4s, 0) L(fmaxp_rm_2d, 1)
    L3(fabs) L3(fneg) L3(fsqrt) L3(frintn) L3(frintm) L3(frintp) L3(frintz) L3(frinta) L3(frintx) L3(frinti)
    L3(fcvtns) L3(fcvtms) L3(fcvtas) L3(fcvtps) L3(fcvtzs) L3(fcvtnu) L3(fcvtmu) L3(fcvtau) L3(fcvtpu) L3(fcvtzu)
    L(scvtf_4s, 2) L(scvtf_2s, 2) L(scvtf_2d, 3) L(ucvtf_4s, 2) L(ucvtf_2s, 2) L(ucvtf_2d, 3)
    L3(fcmeq0) L3(fcmgt0) L3(fcmge0) L3(fcmle0) L3(fcmlt0)
    L(sfabd, 0) L(dfabd, 1) L(sfcmeq, 0) L(dfcmeq, 1) L(sfcmge, 0) L(dfcmge, 1) L(sfcmgt, 0) L(dfcmgt, 1)
    L(sfacge, 0) L(dfacge, 1) L(sfacgt, 0) L(dfacgt, 1)
    L(sfaddp, 0) L(dfaddp, 1) L(sfmaxp, 0) L(dfminp, 1) L(sfmaxnmp, 0) L(dfminnmp, 1)
    L(sfcvtzs, 0) L(dfcvtzs, 1) L(sfcvtnu, 0) L(dfcvtpu, 1) L(sfcvtas, 0) L(dfcvtms, 1) L(dfcvtzu, 1) L(sfcvtzu, 0)
    L(sfcmeq0, 0) L(dfcmlt0, 1) L(dfcmge0, 1)
    L(fmla_4s_0, 0) L(fmla_4s_3, 0) L(fmla_2s_1, 0) L(fmls_4s_2, 0) L(fmul_4s_1, 0) L(fmul_2s_3, 0)
    L(fmla_2d_0, 1) L(fmla_2d_1, 1) L(fmls_2d_1, 1) L(fmul_2d_0, 1) L(fmul_2d_1, 1)
    L(sfmul_el, 0) L(dfmul_el, 1) L(sfmla_el, 0) L(dfmls_el, 1)
    L(fixzs_w_d, 1) L(fixzs_x_d, 1) L(fixzu_w_d, 1) L(fixzu_x_d, 1) L(fixzs_w_s, 0) L(fixzs_x_s, 0) L(fixzu_w_s, 0) L(fixzu_x_s, 0)
    L(fixscvtf_d_w, 2) L(fixscvtf_d_x, 3) L(fixucvtf_d_w, 2) L(fixucvtf_d_x, 3) L(fixscvtf_s_w, 2) L(fixscvtf_s_x, 3)
    L(fixucvtf_s_w, 2) L(fixucvtf_s_x, 3)
  };
  u64 modes[4] = {0, 1ull << 22, 2ull << 22, 3ull << 22};
  /* include flush-to-zero and default-NaN runs as well */
  u64 extra[2] = {1ull << 24, 1ull << 25};
  int count = (int)(sizeof ops / sizeof ops[0]);
  for (int m = 0; m < 6; m++) {
    setfpcr(m < 4 ? modes[m] : extra[m - 4]);
    st = 0x9e3779b97f4a7c15ull + m;
    for (int i = 0; i < n; i++) {
      for (int o = 0; o < count; o++) {
        V a = gen(ops[o].kind), b = gen(ops[o].kind), c = gen(ops[o].kind);
        if (ops[o].kind <= 1 && (rnd() & 3) == 0) { /* correlated inputs for fused ops */
          b = a; b[0] ^= rnd() & 0xff;
        }
        clrfpsr();
        V r = ops[o].fn(a, b, c);
        out(ops[o].name, r, getfpsr());
      }
    }
  }
  setfpcr(0);
  printf("hash %016llx\n", h);
  return 0;
}
