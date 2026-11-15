// Known-positive control for the tracer: makes calls whose syscall/trap/msg ids are known.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <unistd.h>
int main(void) {
  int ncpu = 0; size_t l = sizeof ncpu;
  getpid();                                    // SYS_getpid 20
  int fd = open("/dev/null", O_RDONLY);        // SYS_open 5
  fcntl(fd, F_GETFD);                          // SYS_fcntl 92 cmd 1
  close(fd);                                   // SYS_close 6
  sysctlbyname("hw.ncpu", &ncpu, &l, 0, 0);    // sysctlbyname text event
  mach_port_t p; mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p); // trap -16 / MIG 3204 path
  struct task_basic_info ti; mach_msg_type_number_t c = TASK_BASIC_INFO_COUNT;
  task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&ti, &c);              // MIG 3405 flavor 5
  mach_port_t bp = MACH_PORT_NULL, sp;
  task_get_bootstrap_port(mach_task_self(), &bp);
  bootstrap_look_up(bp, "com.apple.trace.control.nonexistent", &sp);              // bootstrap text event
  mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);
  struct { mach_msg_header_t h; char b[16]; } m = {0};
  m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0); m.h.msgh_size = sizeof m; m.h.msgh_remote_port = p; m.h.msgh_id = 7777;
  mach_msg(&m.h, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);      // msg id 7777 send
  struct { mach_msg_header_t h; char b[16]; mach_msg_trailer_t t; } r = {0};
  mach_msg(&r.h, MACH_RCV_MSG, 0, sizeof r, p, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);                    // rcv 7777
  bootstrap_look_up(bootstrap_port, "com.apple.system.notification_center", &sp);
  puts("control done");
  return 0;
}
