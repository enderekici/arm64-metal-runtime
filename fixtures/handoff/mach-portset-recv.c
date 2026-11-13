#include <mach/mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
extern mach_port_name_t mk_timer_create(void); extern kern_return_t mk_timer_arm(mach_port_name_t, uint64_t);
struct M { mach_msg_header_t h; char b[256]; mach_msg_trailer_t t; };
static uint64_t abs_in_ms(int ms){ mach_timebase_info_data_t b; mach_timebase_info(&b); return mach_absolute_time() + (uint64_t)ms*1000000ull*b.denom/b.numer; }
int main(void){
  mach_port_t set=0,timer=mk_timer_create();
  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &set);
  printf("insert=%d\n", mach_port_insert_member(mach_task_self(), timer, set));
  struct M m; mach_msg_return_t r;
  mk_timer_arm(timer, abs_in_ms(50));
  r = mach_msg(&m.h, MACH_RCV_MSG|MACH_RCV_LARGE|MACH_RCV_VOUCHER|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AV), 0, sizeof(m), set, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  printf("A blocking: r=0x%x local_is_timer=%d size=%u id=%d\n", r, m.h.msgh_local_port==timer, m.h.msgh_size, m.h.msgh_id);
  r = mach_msg(&m.h, MACH_RCV_MSG|MACH_RCV_TIMEOUT, 0, sizeof(m), set, 20, MACH_PORT_NULL);
  printf("B timeout: r=0x%x\n", r);
  mk_timer_arm(timer, abs_in_ms(10)); usleep(100000);
  r = mach_msg(&m.h, MACH_RCV_MSG|MACH_RCV_TIMEOUT|MACH_RCV_LARGE, 0, sizeof(m), set, 1000, MACH_PORT_NULL);
  printf("C queued: r=0x%x local_is_timer=%d size=%u\n", r, m.h.msgh_local_port==timer, m.h.msgh_size);
  mk_timer_arm(timer, abs_in_ms(10)); usleep(100000);
  char tiny[24]; 
  r = mach_msg((mach_msg_header_t*)tiny, MACH_RCV_MSG|MACH_RCV_TIMEOUT|MACH_RCV_LARGE, 0, sizeof(tiny), set, 1000, MACH_PORT_NULL);
  printf("D large: r=0x%x size=%u\n", r, ((mach_msg_header_t*)tiny)->msgh_size);
  r = mach_msg(&m.h, MACH_RCV_MSG|MACH_RCV_TIMEOUT, 0, sizeof(m), set, 1000, MACH_PORT_NULL);
  printf("E after large: r=0x%x size=%u\n", r, m.h.msgh_size);
  return 0;
}
