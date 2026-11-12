#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fenv.h>
static uint64_t d2b(double d){uint64_t u;memcpy(&u,&d,8);return u;}
static uint32_t f2b(float f){uint32_t u;memcpy(&u,&f,4);return u;}
int main(void){
  uint64_t ds[]={0x3ff0000000000000ull,0x3ff0000010000000ull,0x3ff0000010000001ull,0x3ff000000fffffffull,0x47efffffe0000000ull,0x47effffff0000000ull,0x47f0000000000000ull,
   0x3810000000000000ull,0x380fffffe0000000ull,0x36a0000000000000ull,0x36a0000000000001ull,0x3690000000000000ull,0x0000000000000001ull,0x8000000000000000ull,
   0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000000001ull,0x7ff4000000000000ull,0xc00921fb54442d18ull,0x3e7ad7f29abcaf48ull,0x3810000010000000ull,0x380ffffff0000000ull,0xbff0000008000000ull};
  uint32_t fs[]={0x3f800000,0x00000001,0x007fffff,0x00800000,0x80000001,0x7f800000,0xff800000,0x7fc00001,0x7fa00000,0x7f7fffff,0x80000000,0x40490fdb};
  uint64_t acc=0;
  for(unsigned i=0;i<sizeof ds/8;i++){volatile double d;d=0;uint64_t b=ds[i];memcpy((void*)&d,&b,8);volatile float f=(float)d;printf("d2f %016llx -> %08x\n",(unsigned long long)b,f2b(f));}
  for(unsigned i=0;i<sizeof fs/4;i++){uint32_t b=fs[i];volatile float f;memcpy((void*)&f,&b,4);volatile double d=(double)f;printf("f2d %08x -> %016llx\n",b,(unsigned long long)d2b(d));}
  return 0;}
