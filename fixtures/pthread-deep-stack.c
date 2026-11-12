#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static volatile char sink;
static size_t deep(size_t n){ volatile char buf[4096]; buf[0]=(char)n; sink=buf[0]; if(n==0) return 0; return deep(n-1)+buf[0]%2+1; }
static void* run(void* a){ size_t kb=(size_t)a; size_t r=deep(kb/4); printf("depth ok %zu KB r=%zu\n",kb,r); return 0; }
int main(void){
  size_t sizes[]={1024,2048,3500};
  for(int i=0;i<3;i++){ pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,4u<<20); pthread_t t; pthread_create(&t,&at,run,(void*)sizes[i]); pthread_join(t,0);} 
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,1u<<20); size_t sz; pthread_attr_getstacksize(&at,&sz); printf("attr size %zu\n",sz);
  pthread_t t; pthread_create(&t,&at,run,(void*)700); pthread_join(t,0);
  return 0;}
