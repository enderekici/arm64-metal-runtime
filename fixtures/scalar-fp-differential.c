#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint64_t st=0x9e3779b97f4a7c15ull;
static uint64_t rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
static uint64_t pick64(void){
  uint64_t r=rnd(); uint64_t sign=(rnd()&1)<<63;
  switch(r%12){
   case 0: return sign|0x7ff0000000000000ull; case 1: return sign|0x7ff8000000000000ull|(rnd()&0xff);
   case 2: return sign|0x7ff0000000000000ull|(rnd()&0x7ffffffffffffull)|1; case 3: return sign;
   case 4: return sign|(rnd()&0xfffffffffffffull); case 5: return sign|(0x3ffull<<52)|(rnd()&0xfffffffffffffull);
   case 6: return sign|((uint64_t)(1023+(int)(rnd()%120)-60)<<52)|(rnd()&0xfffffffffffffull);
   case 7: return sign|((uint64_t)(1+rnd()%2046)<<52)|(rnd()&0xfffffffffffffull);
   case 8: return sign|((uint64_t)(1023+(int)(rnd()%8))<<52)|((rnd()&3)<<50);
   default: return sign|((uint64_t)(1023+(int)(rnd()%70)-5)<<52)|(rnd()&0xfffffffffffffull);}
}
static uint32_t pick32(void){
  uint32_t r=rnd(); uint32_t sign=(uint32_t)(rnd()&1)<<31;
  switch(r%12){
   case 0: return sign|0x7f800000u; case 1: return sign|0x7fc00000u|(rnd()&0xff);
   case 2: return sign|0x7f800000u|(rnd()&0x3fffffu)|1; case 3: return sign;
   case 4: return sign|(rnd()&0x7fffffu); case 5: return sign|(0x7fu<<23)|(rnd()&0x7fffffu);
   case 6: return sign|((uint32_t)(127+(int)(rnd()%60)-30)<<23)|(rnd()&0x7fffffu);
   case 7: return sign|((uint32_t)(1+rnd()%254)<<23)|(rnd()&0x7fffffu);
   default: return sign|((uint32_t)(127+(int)(rnd()%40)-4)<<23)|(rnd()&0x7fffffu);}
}
static uint64_t h=1469598103934665603ull;
static void mix(uint64_t v){h^=v;h*=1099511628211ull;}
static uint64_t getfpsr(void){uint64_t v;__asm__ volatile("mrs %0, fpsr":"=r"(v));return v;}
static void clrfpsr(void){__asm__ volatile("msr fpsr, xzr");}
static void setfpcr(uint64_t v){__asm__ volatile("msr fpcr, %0"::"r"(v));}
#define D2(name,ins) static uint64_t name(uint64_t a,uint64_t b){double x,y,r;memcpy(&x,&a,8);memcpy(&y,&b,8);__asm__ volatile(ins " %d0, %d1, %d2":"=w"(r):"w"(x),"w"(y));uint64_t o;memcpy(&o,&r,8);return o;}
#define S2(name,ins) static uint32_t name(uint32_t a,uint32_t b){float x,y,r;memcpy(&x,&a,4);memcpy(&y,&b,4);__asm__ volatile(ins " %s0, %s1, %s2":"=w"(r):"w"(x),"w"(y));uint32_t o;memcpy(&o,&r,4);return o;}
#define D1(name,ins) static uint64_t name(uint64_t a){double x,r;memcpy(&x,&a,8);__asm__ volatile(ins " %d0, %d1":"=w"(r):"w"(x));uint64_t o;memcpy(&o,&r,8);return o;}
#define S1(name,ins) static uint32_t name(uint32_t a){float x,r;memcpy(&x,&a,4);__asm__ volatile(ins " %s0, %s1":"=w"(r):"w"(x));uint32_t o;memcpy(&o,&r,4);return o;}
#define DI(name,ins) static uint64_t name(uint64_t a){double x;uint64_t r;memcpy(&x,&a,8);__asm__ volatile(ins " %0, %d1":"=r"(r):"w"(x));return r;}
#define DW(name,ins) static uint32_t name(uint64_t a){double x;uint32_t r;memcpy(&x,&a,8);__asm__ volatile(ins " %w0, %d1":"=r"(r):"w"(x));return r;}
#define SI(name,ins) static uint64_t name(uint32_t a){float x;uint64_t r;memcpy(&x,&a,4);__asm__ volatile(ins " %0, %s1":"=r"(r):"w"(x));return r;}
#define SW(name,ins) static uint32_t name(uint32_t a){float x;uint32_t r;memcpy(&x,&a,4);__asm__ volatile(ins " %w0, %s1":"=r"(r):"w"(x));return r;}
D2(dadd,"fadd")D2(dsub,"fsub")D2(dmul,"fmul")D2(dnmul,"fnmul")D2(dmax,"fmax")D2(dmin,"fmin")D2(dmaxnm,"fmaxnm")D2(dminnm,"fminnm")
S2(sadd,"fadd")S2(ssub,"fsub")S2(smul,"fmul")S2(snmul,"fnmul")S2(smax,"fmax")S2(smin,"fmin")S2(smaxnm,"fmaxnm")S2(sminnm,"fminnm")
D1(dsqrt,"fsqrt")D1(dabs,"fabs")D1(dmov,"fmov")D1(drintn,"frintn")D1(drintm,"frintm")D1(drintp,"frintp")D1(drintz,"frintz")D1(drinta,"frinta")D1(drintx,"frintx")D1(drinti,"frinti")
S1(ssqrt,"fsqrt")S1(sabs,"fabs")S1(smov,"fmov")S1(srintn,"frintn")S1(srintm,"frintm")S1(srintp,"frintp")S1(srintz,"frintz")S1(srinta,"frinta")S1(srintx,"frintx")S1(srinti,"frinti")
DI(dxzs,"fcvtzs")DI(dxzu,"fcvtzu")DI(dxns,"fcvtns")DI(dxps,"fcvtps")DI(dxms,"fcvtms")DI(dxas,"fcvtas")DI(dxnu,"fcvtnu")DI(dxpu,"fcvtpu")DI(dxmu,"fcvtmu")DI(dxau,"fcvtau")
DW(dwzs,"fcvtzs")DW(dwzu,"fcvtzu")DW(dwns,"fcvtns")DW(dwau,"fcvtau")DW(dwps,"fcvtps")DW(dwmu,"fcvtmu")
SI(sxzs,"fcvtzs")SI(sxzu,"fcvtzu")SI(sxns,"fcvtns")SI(sxps,"fcvtps")
SW(swzs,"fcvtzs")SW(swzu,"fcvtzu")SW(swas,"fcvtas")SW(swms,"fcvtms")
static uint64_t dsel(uint64_t a,uint64_t b,uint64_t c){double x,y,r;memcpy(&x,&a,8);memcpy(&y,&b,8);__asm__ volatile("cmp %2, #5\n fcsel %d0, %d1, %d3, lt":"=w"(r):"w"(x),"r"(c),"w"(y):"cc");uint64_t o;memcpy(&o,&r,8);return o;}
static uint32_t ssel(uint32_t a,uint32_t b,uint64_t c){float x,y,r;memcpy(&x,&a,4);memcpy(&y,&b,4);__asm__ volatile("cmp %2, #5\n fcsel %s0, %s1, %s3, hi":"=w"(r):"w"(x),"r"(c),"w"(y):"cc");uint32_t o;memcpy(&o,&r,4);return o;}
#define D3(name,ins) static uint64_t name(uint64_t a,uint64_t b,uint64_t c){double x,y,z,r;memcpy(&x,&a,8);memcpy(&y,&b,8);memcpy(&z,&c,8);__asm__ volatile(ins " %d0, %d1, %d2, %d3":"=w"(r):"w"(x),"w"(y),"w"(z));uint64_t o;memcpy(&o,&r,8);return o;}
#define S3(name,ins) static uint32_t name(uint32_t a,uint32_t b,uint32_t c){float x,y,z,r;memcpy(&x,&a,4);memcpy(&y,&b,4);memcpy(&z,&c,4);__asm__ volatile(ins " %s0, %s1, %s2, %s3":"=w"(r):"w"(x),"w"(y),"w"(z));uint32_t o;memcpy(&o,&r,4);return o;}
D3(dfma,"fmadd")D3(dfms,"fmsub")D3(dfnma,"fnmadd")D3(dfnms,"fnmsub")S3(sfma,"fmadd")S3(sfms,"fmsub")S3(sfnma,"fnmadd")S3(sfnms,"fnmsub")
static uint64_t pickc64(uint64_t a,uint64_t b){uint64_t r=rnd()%4;if(r==0)return pick64();double x,y,p;memcpy(&x,&a,8);memcpy(&y,&b,8);p=-(x*y);uint64_t o;memcpy(&o,&p,8);if(r==2)o+=rnd()%5;if(r==3)o^=rnd()&0xff;return o;}
static uint32_t pickc32(uint32_t a,uint32_t b){uint32_t r=rnd()%4;if(r==0)return pick32();float x,y,p;memcpy(&x,&a,4);memcpy(&y,&b,4);p=-(x*y);uint32_t o;memcpy(&o,&p,4);if(r==2)o+=rnd()%5;if(r==3)o^=rnd()&0xff;return o;}
static int dump;
#define OUT(tag,v,fp) do{uint64_t _v=(uint64_t)(v),_f=(fp);mix(_v);mix(_f);if(dump)printf("%s %016llx %llx\n",tag,(unsigned long long)_v,(unsigned long long)_f);}while(0)
#define T2D(tag,fn) {clrfpsr();uint64_t r=fn(a,b);OUT(tag,r,getfpsr());}
#define T2S(tag,fn) {clrfpsr();uint32_t r=fn(sa,sb);OUT(tag,r,getfpsr());}
#define T3D(tag,fn) {clrfpsr();uint64_t r=fn(a,b,c);OUT(tag,r,getfpsr());}
#define T3S(tag,fn) {clrfpsr();uint32_t r=fn(sa,sb,sc);OUT(tag,r,getfpsr());}
#define T1(tag,fn,arg) {clrfpsr();uint64_t r=fn(arg);OUT(tag,r,getfpsr());}
int main(int argc,char**argv){
  int n=2000; if(argc>1){n=0;for(char*p=argv[1];*p;p++)n=n*10+(*p-'0');} dump=argc>2;
  uint64_t modes[4]={0,1ull<<22,2ull<<22,3ull<<22};
  for(int m=0;m<4;m++){ setfpcr(modes[m]); st=0x9e3779b97f4a7c15ull+m;
   for(int i=0;i<n;i++){uint64_t a=pick64(),b=pick64();uint32_t sa=pick32(),sb=pick32();uint64_t c=pickc64(a,b);uint32_t sc=pickc32(sa,sb);
    T2D("dadd",dadd)T2D("dsub",dsub)T2D("dmul",dmul)T2D("dnmul",dnmul)T2D("dmax",dmax)T2D("dmin",dmin)T2D("dmaxnm",dmaxnm)T2D("dminnm",dminnm)
    T2S("sadd",sadd)T2S("ssub",ssub)T2S("smul",smul)T2S("snmul",snmul)T2S("smax",smax)T2S("smin",smin)T2S("smaxnm",smaxnm)T2S("sminnm",sminnm)
    T1("dsqrt",dsqrt,a)T1("dabs",dabs,a)T1("dmov",dmov,a)T1("drintn",drintn,a)T1("drintm",drintm,a)T1("drintp",drintp,a)T1("drintz",drintz,a)T1("drinta",drinta,a)T1("drintx",drintx,a)T1("drinti",drinti,a)
    T1("ssqrt",ssqrt,sa)T1("sabs",sabs,sa)T1("smov",smov,sa)T1("srintn",srintn,sa)T1("srintm",srintm,sa)T1("srintp",srintp,sa)T1("srintz",srintz,sa)T1("srinta",srinta,sa)T1("srintx",srintx,sa)T1("srinti",srinti,sa)
    T1("dxzs",dxzs,a)T1("dxzu",dxzu,a)T1("dxns",dxns,a)T1("dxps",dxps,a)T1("dxms",dxms,a)T1("dxas",dxas,a)T1("dxnu",dxnu,a)T1("dxpu",dxpu,a)T1("dxmu",dxmu,a)T1("dxau",dxau,a)
    T1("dwzs",dwzs,a)T1("dwzu",dwzu,a)T1("dwns",dwns,a)T1("dwau",dwau,a)T1("dwps",dwps,a)T1("dwmu",dwmu,a)
    T1("sxzs",sxzs,sa)T1("sxzu",sxzu,sa)T1("sxns",sxns,sa)T1("sxps",sxps,sa)T1("swzs",swzs,sa)T1("swzu",swzu,sa)T1("swas",swas,sa)T1("swms",swms,sa)
    T3D("dfma",dfma)T3D("dfms",dfms)T3D("dfnma",dfnma)T3D("dfnms",dfnms)T3S("sfma",sfma)T3S("sfms",sfms)T3S("sfnma",sfnma)T3S("sfnms",sfnms)
    {clrfpsr();uint64_t r=dsel(a,b,rnd()%11);OUT("dsel",r,getfpsr());}
    {clrfpsr();uint64_t r=ssel(sa,sb,rnd()%11);OUT("ssel",r,getfpsr());}
   }}
  printf("hash %016llx\n",(unsigned long long)h);return 0;}
