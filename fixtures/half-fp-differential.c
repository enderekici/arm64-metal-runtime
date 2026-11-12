/* Native vs arm64-metal differential for half-precision scalar FP and B/H memory forms.
   usage: half-fp-differential [n] [run-filter] [dump]; prints "tag hash" per op; a third argument also dumps values. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t st = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static uint16_t pick16(void) {
  uint16_t sign = (uint16_t)(rnd() & 1) << 15;
  switch (rnd() % 12) {
  case 0: return sign | 0x7c00;
  case 1: return sign | 0x7e00 | (rnd() & 0xff);
  case 2: return sign | 0x7c00 | (rnd() & 0x3ff) | 1;
  case 3: return sign;
  case 4: return sign | (rnd() & 0x3ff);
  case 5: return sign | (15 << 10) | (rnd() & 0x3ff);
  case 6: return sign | ((15 + (int)(rnd() % 20) - 10) << 10) | (rnd() & 0x3ff);
  case 7: return sign | ((1 + rnd() % 30) << 10) | (rnd() & 0x3ff);
  default: return sign | ((15 + (int)(rnd() % 16) - 3) << 10) | (rnd() & 0x3ff);
  }
}
static uint32_t pick32(void) {
  uint32_t r = rnd(); uint32_t sign = (uint32_t)(rnd() & 1) << 31;
  switch (r % 12) {
  case 0: return sign | 0x7f800000u; case 1: return sign | 0x7fc00000u | (rnd() & 0xff);
  case 2: return sign | 0x7f800000u | (rnd() & 0x3fffffu) | 1; case 3: return sign;
  case 4: return sign | (rnd() & 0x7fffffu);
  case 5: return sign | ((uint32_t)(127 + (int)(rnd() % 50) - 25) << 23) | (rnd() & 0x7fffffu);
  case 6: return sign | ((uint32_t)(100 + rnd() % 20) << 23) | (rnd() & 0x7fffffu);
  case 7: return sign | ((uint32_t)(1 + rnd() % 254) << 23) | (rnd() & 0x7fffffu);
  case 8: return sign | ((uint32_t)(112 + rnd() % 8) << 23) | ((rnd() & 0x3ff) << 13) | (rnd() & 1) << 12 | (rnd() & 1);
  default: return sign | ((uint32_t)(127 + (int)(rnd() % 40) - 8) << 23) | (rnd() & 0x7fffffu);
  }
}
static uint64_t pick64(void) {
  uint64_t r = rnd(); uint64_t sign = (rnd() & 1) << 63;
  switch (r % 12) {
  case 0: return sign | 0x7ff0000000000000ull; case 1: return sign | 0x7ff8000000000000ull | (rnd() & 0xff);
  case 2: return sign | 0x7ff0000000000000ull | (rnd() & 0x7ffffffffffffull) | 1; case 3: return sign;
  case 4: return sign | (rnd() & 0xfffffffffffffull);
  case 5: return sign | ((uint64_t)(1023 + (int)(rnd() % 50) - 25) << 52) | (rnd() & 0xfffffffffffffull);
  case 6: return sign | ((uint64_t)(1023 - 30 + rnd() % 20) << 52) | (rnd() & 0xfffffffffffffull);
  case 7: return sign | ((uint64_t)(1 + rnd() % 2046) << 52) | (rnd() & 0xfffffffffffffull);
  default: return sign | ((uint64_t)(1023 + (int)(rnd() % 40) - 8) << 52) | (rnd() & 0xfffffffffffffull);
  }
}
static uint64_t pickint(void) {
  switch (rnd() % 6) {
  case 0: return rnd() % 70000; case 1: return -(int64_t)(rnd() % 70000);
  case 2: return rnd(); case 3: return (uint64_t)(int32_t)rnd();
  case 4: return 65504 + rnd() % 40 - 20; default: return rnd() >> (rnd() % 64);
  }
}
static uint64_t h = 1469598103934665603ull;
static void mix(uint64_t v) { h ^= v; h *= 1099511628211ull; }
static uint64_t getfpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static void clrfpsr(void) { __asm__ volatile("msr fpsr, xzr"); }
static void setfpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" ::"r"(v)); }
static int dump; static const char *filter;
static int active(const char *tag) { return dump && strstr(tag, filter); }
#define HW(x) (*(_Float16 *)&(x))
#define H2(name, ins) static uint16_t name(uint16_t a, uint16_t b) { _Float16 x, y, r; memcpy(&x,&a,2); memcpy(&y,&b,2); __asm__ volatile(ins " %h0, %h1, %h2" : "=w"(r) : "w"(x), "w"(y)); uint16_t o; memcpy(&o,&r,2); return o; }
#define H1(name, ins) static uint16_t name(uint16_t a) { _Float16 x, r; memcpy(&x,&a,2); __asm__ volatile(ins " %h0, %h1" : "=w"(r) : "w"(x)); uint16_t o; memcpy(&o,&r,2); return o; }
#define H3(name, ins) static uint16_t name(uint16_t a, uint16_t b, uint16_t c) { _Float16 x, y, z, r; memcpy(&x,&a,2); memcpy(&y,&b,2); memcpy(&z,&c,2); __asm__ volatile(ins " %h0, %h1, %h2, %h3" : "=w"(r) : "w"(x), "w"(y), "w"(z)); uint16_t o; memcpy(&o,&r,2); return o; }
H2(hadd,"fadd")H2(hsub,"fsub")H2(hmul,"fmul")H2(hdiv,"fdiv")H2(hnmul,"fnmul")H2(hmax,"fmax")H2(hmin,"fmin")H2(hmaxnm,"fmaxnm")H2(hminnm,"fminnm")
H1(hsqrt,"fsqrt")H1(habs,"fabs")H1(hneg,"fneg")H1(hmov,"fmov")H1(hrintn,"frintn")H1(hrintm,"frintm")H1(hrintp,"frintp")H1(hrintz,"frintz")H1(hrinta,"frinta")H1(hrintx,"frintx")H1(hrinti,"frinti")
H3(hfma,"fmadd")H3(hfms,"fmsub")H3(hfnma,"fnmadd")H3(hfnms,"fnmsub")
/* h -> int */
#define HX(name, ins) static uint64_t name(uint16_t a) { _Float16 x; uint64_t r; memcpy(&x,&a,2); __asm__ volatile(ins " %0, %h1" : "=r"(r) : "w"(x)); return r; }
#define HW_(name, ins) static uint32_t name(uint16_t a) { _Float16 x; uint32_t r; memcpy(&x,&a,2); __asm__ volatile(ins " %w0, %h1" : "=r"(r) : "w"(x)); return r; }
HX(hxzs,"fcvtzs")HX(hxzu,"fcvtzu")HX(hxns,"fcvtns")HX(hxnu,"fcvtnu")HX(hxps,"fcvtps")HX(hxpu,"fcvtpu")HX(hxms,"fcvtms")HX(hxmu,"fcvtmu")HX(hxas,"fcvtas")HX(hxau,"fcvtau")
HW_(hwzs,"fcvtzs")HW_(hwzu,"fcvtzu")HW_(hwns,"fcvtns")HW_(hwas,"fcvtas")HW_(hwps,"fcvtps")HW_(hwmu,"fcvtmu")
static uint64_t hxzs5(uint16_t a){_Float16 x;uint64_t r;memcpy(&x,&a,2);__asm__ volatile("fcvtzs %0, %h1, #5":"=r"(r):"w"(x));return r;}
static uint64_t hxzu9(uint16_t a){_Float16 x;uint64_t r;memcpy(&x,&a,2);__asm__ volatile("fcvtzu %0, %h1, #9":"=r"(r):"w"(x));return r;}
static uint32_t hwzs3(uint16_t a){_Float16 x;uint32_t r;memcpy(&x,&a,2);__asm__ volatile("fcvtzs %w0, %h1, #3":"=r"(r):"w"(x));return r;}
static uint32_t hwzu16(uint16_t a){_Float16 x;uint32_t r;memcpy(&x,&a,2);__asm__ volatile("fcvtzu %w0, %h1, #16":"=r"(r):"w"(x));return r;}
/* int -> h */
static uint16_t hscx(uint64_t a){_Float16 r;__asm__ volatile("scvtf %h0, %1":"=w"(r):"r"(a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t hucx(uint64_t a){_Float16 r;__asm__ volatile("ucvtf %h0, %1":"=w"(r):"r"(a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t hscw(uint64_t a){_Float16 r;__asm__ volatile("scvtf %h0, %w1":"=w"(r):"r"((uint32_t)a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t hucw(uint64_t a){_Float16 r;__asm__ volatile("ucvtf %h0, %w1":"=w"(r):"r"((uint32_t)a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t hscx7(uint64_t a){_Float16 r;__asm__ volatile("scvtf %h0, %1, #7":"=w"(r):"r"(a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t hucw12(uint64_t a){_Float16 r;__asm__ volatile("ucvtf %h0, %w1, #12":"=w"(r):"r"((uint32_t)a));uint16_t o;memcpy(&o,&r,2);return o;}
/* fixed point for s/d */
static uint64_t sxzs6(uint32_t a){float x;uint64_t r;memcpy(&x,&a,4);__asm__ volatile("fcvtzs %0, %s1, #6":"=r"(r):"w"(x));return r;}
static uint32_t swzu20(uint32_t a){float x;uint32_t r;memcpy(&x,&a,4);__asm__ volatile("fcvtzu %w0, %s1, #20":"=r"(r):"w"(x));return r;}
static uint64_t dxzs2(uint64_t a){double x;uint64_t r;memcpy(&x,&a,8);__asm__ volatile("fcvtzs %0, %d1, #2":"=r"(r):"w"(x));return r;}
static uint32_t dwzs31(uint64_t a){double x;uint32_t r;memcpy(&x,&a,8);__asm__ volatile("fcvtzs %w0, %d1, #31":"=r"(r):"w"(x));return r;}
static uint32_t sscw8(uint64_t a){float r;__asm__ volatile("scvtf %s0, %w1, #8":"=w"(r):"r"((uint32_t)a));uint32_t o;memcpy(&o,&r,4);return o;}
static uint64_t dscx4(uint64_t a){double r;__asm__ volatile("scvtf %d0, %1, #4":"=w"(r):"r"(a));uint64_t o;memcpy(&o,&r,8);return o;}
static uint64_t ducx33(uint64_t a){double r;__asm__ volatile("ucvtf %d0, %1, #33":"=w"(r):"r"(a));uint64_t o;memcpy(&o,&r,8);return o;}
/* fcvt */
static uint32_t cvt_sh(uint16_t a){_Float16 x;float r;memcpy(&x,&a,2);__asm__ volatile("fcvt %s0, %h1":"=w"(r):"w"(x));uint32_t o;memcpy(&o,&r,4);return o;}
static uint64_t cvt_dh(uint16_t a){_Float16 x;double r;memcpy(&x,&a,2);__asm__ volatile("fcvt %d0, %h1":"=w"(r):"w"(x));uint64_t o;memcpy(&o,&r,8);return o;}
static uint16_t cvt_hs(uint32_t a){float x;_Float16 r;memcpy(&x,&a,4);__asm__ volatile("fcvt %h0, %s1":"=w"(r):"w"(x));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t cvt_hd(uint64_t a){double x;_Float16 r;memcpy(&x,&a,8);__asm__ volatile("fcvt %h0, %d1":"=w"(r):"w"(x));uint16_t o;memcpy(&o,&r,2);return o;}
/* fmov gpr */
static uint32_t fmov_wh(uint16_t a){_Float16 x;uint32_t r;memcpy(&x,&a,2);__asm__ volatile("fmov %w0, %h1":"=r"(r):"w"(x));return r;}
static uint64_t fmov_xh(uint16_t a){_Float16 x;uint64_t r;memcpy(&x,&a,2);__asm__ volatile("fmov %0, %h1":"=r"(r):"w"(x));return r;}
static uint16_t fmov_hw(uint64_t a){_Float16 r;__asm__ volatile("fmov %h0, %w1":"=w"(r):"r"((uint32_t)a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t fmov_hx(uint64_t a){_Float16 r;__asm__ volatile("fmov %h0, %1":"=w"(r):"r"(a));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t fmov_imm1(void){_Float16 r;__asm__ volatile("fmov %h0, #1.0":"=w"(r));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t fmov_imm2(void){_Float16 r;__asm__ volatile("fmov %h0, #-2.5":"=w"(r));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t fmov_imm3(void){_Float16 r;__asm__ volatile("fmov %h0, #0.1875":"=w"(r));uint16_t o;memcpy(&o,&r,2);return o;}
static uint16_t fmov_imm4(void){_Float16 r;__asm__ volatile("fmov %h0, #31.0":"=w"(r));uint16_t o;memcpy(&o,&r,2);return o;}
/* compare -> nzcv, select */
static uint64_t hcmp(uint16_t a,uint16_t b){_Float16 x,y;memcpy(&x,&a,2);memcpy(&y,&b,2);uint64_t f;__asm__ volatile("fcmp %h1, %h2\n mrs %0, nzcv":"=r"(f):"w"(x),"w"(y):"cc");return f>>28;}
static uint64_t hcmpe(uint16_t a,uint16_t b){_Float16 x,y;memcpy(&x,&a,2);memcpy(&y,&b,2);uint64_t f;__asm__ volatile("fcmpe %h1, %h2\n mrs %0, nzcv":"=r"(f):"w"(x),"w"(y):"cc");return f>>28;}
static uint64_t hcmp0(uint16_t a){_Float16 x;memcpy(&x,&a,2);uint64_t f;__asm__ volatile("fcmp %h1, #0.0\n mrs %0, nzcv":"=r"(f):"w"(x):"cc");return f>>28;}
static uint64_t hcmpe0(uint16_t a){_Float16 x;memcpy(&x,&a,2);uint64_t f;__asm__ volatile("fcmpe %h1, #0.0\n mrs %0, nzcv":"=r"(f):"w"(x):"cc");return f>>28;}
static uint64_t hccmp(uint16_t a,uint16_t b,uint64_t c){_Float16 x,y;memcpy(&x,&a,2);memcpy(&y,&b,2);uint64_t f;__asm__ volatile("cmp %3, #5\n fccmp %h1, %h2, #5, lt\n mrs %0, nzcv":"=r"(f):"w"(x),"w"(y),"r"(c):"cc");return f>>28;}
static uint16_t hsel(uint16_t a,uint16_t b,uint64_t c){_Float16 x,y,r;memcpy(&x,&a,2);memcpy(&y,&b,2);__asm__ volatile("cmp %2, #5\n fcsel %h0, %h1, %h3, lt":"=w"(r):"w"(x),"r"(c),"w"(y):"cc");uint16_t o;memcpy(&o,&r,2);return o;}
/* B/H memory forms: returns loaded value; buffer-relative */
static uint8_t buf[256] __attribute__((aligned(16)));
static uint64_t mem_ldr_h(uint16_t v){memcpy(buf+6,&v,2);_Float16 r;__asm__ volatile("ldr %h0, [%1, #6]":"=w"(r):"r"(buf));uint16_t o;memcpy(&o,&r,2);return o;}
static uint64_t mem_ldur_h(uint16_t v){memcpy(buf+37,&v,2);_Float16 r;__asm__ volatile("ldur %h0, [%1, #37]":"=w"(r):"r"(buf));uint16_t o;memcpy(&o,&r,2);return o;}
static uint64_t mem_ldr_b(uint16_t v){buf[9]=(uint8_t)v;uint8_t r8;__asm__ volatile("ldr b0, [%1, #9]\n umov %w0, v0.b[0]":"=r"(r8):"r"(buf):"v0");return r8;}
static uint64_t mem_ldur_b(uint16_t v){buf[21]=(uint8_t)v;uint32_t r;__asm__ volatile("ldur b0, [%1, #-3]\n umov %w0, v0.b[0]":"=r"(r):"r"(buf+24):"v0");return r;}
static uint64_t mem_str_h(uint16_t v){memset(buf,0xa5,32);_Float16 x;memcpy(&x,&v,2);__asm__ volatile("str %h0, [%1, #10]"::"w"(x),"r"(buf):"memory");uint64_t r=0;memcpy(&r,buf+8,4);return r;}
static uint64_t mem_stur_h(uint16_t v){memset(buf,0x5a,32);_Float16 x;memcpy(&x,&v,2);__asm__ volatile("stur %h0, [%1, #-5]"::"w"(x),"r"(buf+16):"memory");uint64_t r=0;memcpy(&r,buf+10,4);return r;}
static uint64_t mem_str_b(uint16_t v){memset(buf,0xa5,32);uint32_t w=v;__asm__ volatile("fmov s0, %w0\n str b0, [%1, #3]"::"r"(w),"r"(buf):"memory","v0");uint64_t r=0;memcpy(&r,buf+2,4);return r;}
static uint64_t mem_ldr_h_post(uint16_t v){memcpy(buf+8,&v,2);_Float16 r;const uint8_t*p=buf+8;__asm__ volatile("ldr %h0, [%1], #6":"=w"(r),"+r"(p));uint16_t o;memcpy(&o,&r,2);return o|((uint64_t)(p-buf)<<16);}
static uint64_t mem_ldr_h_pre(uint16_t v){memcpy(buf+14,&v,2);_Float16 r;const uint8_t*p=buf+8;__asm__ volatile("ldr %h0, [%1, #6]!":"=w"(r),"+r"(p));uint16_t o;memcpy(&o,&r,2);return o|((uint64_t)(p-buf)<<16);}
static uint64_t mem_str_h_post(uint16_t v){memset(buf,0x33,32);_Float16 x;memcpy(&x,&v,2);uint8_t*p=buf+8;__asm__ volatile("str %h1, [%0], #-4":"+r"(p):"w"(x):"memory");uint64_t r=0;memcpy(&r,buf+6,4);return r|((uint64_t)(p-buf)<<32);}
static uint64_t mem_str_h_pre(uint16_t v){memset(buf,0x33,32);_Float16 x;memcpy(&x,&v,2);uint8_t*p=buf+8;__asm__ volatile("str %h1, [%0, #2]!":"+r"(p):"w"(x):"memory");uint64_t r=0;memcpy(&r,buf+8,4);return r|((uint64_t)(p-buf)<<32);}
static uint64_t mem_str_b_post(uint16_t v){memset(buf,0x33,32);uint32_t w=v;uint8_t*p=buf+8;__asm__ volatile("fmov s0, %w1\n str b0, [%0], #7":"+r"(p):"r"(w):"memory","v0");uint64_t r=0;memcpy(&r,buf+6,4);return r|((uint64_t)(p-buf)<<32);}
#define OUT(tag, v, fp) do { uint64_t _v = (uint64_t)(v), _f = (fp); th ^= _v; th *= 1099511628211ull; th ^= _f; th *= 1099511628211ull; if (active(tag)) printf("%s %016llx %llx\n", tag, (unsigned long long)_v, (unsigned long long)_f); } while (0)
#define T2(tag, fn) { clrfpsr(); uint64_t r = fn(a, b); OUT(tag, r, getfpsr()); } { static int dummy; (void)dummy; }
int main(int argc, char **argv) {
  int n = 300; if (argc > 1) { n = 0; for (char *p = argv[1]; *p; p++) n = n * 10 + (*p - '0'); }
  filter = argc > 2 ? argv[2] : ""; dump = argc > 3;
  uint64_t modes[4] = {0, 1ull << 22, 2ull << 22, 3ull << 22};
  /* per-tag hashes: run each tag over the whole sweep so a diff names the tag */
#define SWEEP(tag, body) if (strstr(tag, filter)) { uint64_t th = 1469598103934665603ull; for (int m = 0; m < 4; m++) { setfpcr(modes[m]); st = 0x9e3779b97f4a7c15ull + m; \
    for (int i = 0; i < n; i++) { uint16_t a = pick16(), b = pick16(), c = pick16(); uint32_t sa = pick32(); uint64_t da = pick64(); uint64_t ia = pickint(); (void)a;(void)b;(void)c;(void)sa;(void)da;(void)ia; \
      clrfpsr(); body; } } setfpcr(0); printf("%s %016llx\n", tag, (unsigned long long)th); }
#define H2T(tag, fn) SWEEP(tag, { uint64_t r = fn(a, b); OUT(tag, r, getfpsr()); })
#define H1T(tag, fn) SWEEP(tag, { uint64_t r = fn(a); OUT(tag, r, getfpsr()); })
#define H3T(tag, fn) SWEEP(tag, { uint64_t r = fn(a, b, c); OUT(tag, r, getfpsr()); })
#define SIT(tag, fn) SWEEP(tag, { uint64_t r = fn(sa); OUT(tag, r, getfpsr()); })
#define DIT(tag, fn) SWEEP(tag, { uint64_t r = fn(da); OUT(tag, r, getfpsr()); })
#define IIT(tag, fn) SWEEP(tag, { uint64_t r = fn(ia); OUT(tag, r, getfpsr()); })
  H2T("hadd",hadd)H2T("hsub",hsub)H2T("hmul",hmul)H2T("hdiv",hdiv)H2T("hnmul",hnmul)H2T("hmax",hmax)H2T("hmin",hmin)H2T("hmaxnm",hmaxnm)H2T("hminnm",hminnm)
  H1T("hsqrt",hsqrt)H1T("habs",habs)H1T("hneg",hneg)H1T("hmov",hmov)H1T("hrintn",hrintn)H1T("hrintm",hrintm)H1T("hrintp",hrintp)H1T("hrintz",hrintz)H1T("hrinta",hrinta)H1T("hrintx",hrintx)H1T("hrinti",hrinti)
  H3T("hfma",hfma)H3T("hfms",hfms)H3T("hfnma",hfnma)H3T("hfnms",hfnms)
  H1T("hxzs",hxzs)H1T("hxzu",hxzu)H1T("hxns",hxns)H1T("hxnu",hxnu)H1T("hxps",hxps)H1T("hxpu",hxpu)H1T("hxms",hxms)H1T("hxmu",hxmu)H1T("hxas",hxas)H1T("hxau",hxau)
  H1T("hwzs",hwzs)H1T("hwzu",hwzu)H1T("hwns",hwns)H1T("hwas",hwas)H1T("hwps",hwps)H1T("hwmu",hwmu)
  H1T("hxzs5",hxzs5)H1T("hxzu9",hxzu9)H1T("hwzs3",hwzs3)H1T("hwzu16",hwzu16)
  IIT("hscx",hscx)IIT("hucx",hucx)IIT("hscw",hscw)IIT("hucw",hucw)IIT("hscx7",hscx7)IIT("hucw12",hucw12)
  SIT("sxzs6",sxzs6)SIT("swzu20",swzu20)DIT("dxzs2",dxzs2)DIT("dwzs31",dwzs31)IIT("sscw8",sscw8)IIT("dscx4",dscx4)IIT("ducx33",ducx33)
  H1T("cvt_sh",cvt_sh)H1T("cvt_dh",cvt_dh)SIT("cvt_hs",cvt_hs)DIT("cvt_hd",cvt_hd)
  H1T("fmov_wh",fmov_wh)H1T("fmov_xh",fmov_xh)IIT("fmov_hw",fmov_hw)IIT("fmov_hx",fmov_hx)
  SWEEP("fmov_imm", { OUT("i1",fmov_imm1(),0); OUT("i2",fmov_imm2(),0); OUT("i3",fmov_imm3(),0); OUT("i4",fmov_imm4(),0); })
  H2T("hcmp",hcmp)H2T("hcmpe",hcmpe)H1T("hcmp0",hcmp0)H1T("hcmpe0",hcmpe0)
  SWEEP("hccmp", { uint64_t r = hccmp(a, b, rnd() % 11); OUT("hccmp", r, getfpsr()); })
  SWEEP("hsel", { uint64_t r = hsel(a, b, rnd() % 11); OUT("hsel", r, 0); })
  H1T("mem_ldr_h",mem_ldr_h)H1T("mem_ldur_h",mem_ldur_h)H1T("mem_ldr_b",mem_ldr_b)H1T("mem_ldur_b",mem_ldur_b)
  H1T("mem_str_h",mem_str_h)H1T("mem_stur_h",mem_stur_h)H1T("mem_str_b",mem_str_b)
  H1T("mem_ldr_h_post",mem_ldr_h_post)H1T("mem_ldr_h_pre",mem_ldr_h_pre)H1T("mem_str_h_post",mem_str_h_post)H1T("mem_str_h_pre",mem_str_h_pre)H1T("mem_str_b_post",mem_str_b_post)
  return 0;
}
