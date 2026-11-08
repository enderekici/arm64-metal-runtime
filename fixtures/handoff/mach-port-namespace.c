#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t raw_trap(uint64_t number, uint64_t arg0, uint64_t arg1,
                         uint64_t arg2, uint64_t arg3) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = arg1;
  register uint64_t x2 __asm__("x2") = arg2;
  register uint64_t x3 __asm__("x3") = arg3;
  register uint64_t x16 __asm__("x16") = number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x16) : "memory");
  return x0;
}

int main(void) {
  const mach_port_t task = (mach_port_t)raw_trap((uint64_t)-28, 0, 0, 0, 0);
  mach_port_name_t first = MACH_PORT_NULL;
  mach_port_name_t second = MACH_PORT_NULL;
  const uint64_t alloc_first = raw_trap((uint64_t)-16, task,
      MACH_PORT_RIGHT_RECEIVE, (uint64_t)(uintptr_t)&first, 0);
  const uint64_t alloc_second = raw_trap((uint64_t)-16, task,
      MACH_PORT_RIGHT_RECEIVE, (uint64_t)(uintptr_t)&second, 0);
  const mach_port_name_t reply = (mach_port_name_t)raw_trap((uint64_t)-26, 0, 0, 0, 0);
  const uint64_t insert = raw_trap((uint64_t)-21, task, first, first,
                                   MACH_MSG_TYPE_MAKE_SEND);
  const uint64_t deallocate_send = raw_trap((uint64_t)-18, task, first, 0, 0);
  const uint64_t deallocate_receive_only = raw_trap((uint64_t)-18, task, first, 0, 0);
  const uint64_t destroy_first = raw_trap((uint64_t)-25, task, first, 0, 0);
  const uint64_t destroy_second = raw_trap((uint64_t)-25, task, second, 0, 0);
  const uint64_t destroy_reply = raw_trap((uint64_t)-25, task, reply, 0, 0);
  const uint64_t destroy_again = raw_trap((uint64_t)-25, task, first, 0, 0);
  mach_port_name_t invalid_right_name = MACH_PORT_NULL;
  const uint64_t invalid_right = raw_trap((uint64_t)-16, task,
      MACH_PORT_RIGHT_SEND, (uint64_t)(uintptr_t)&invalid_right_name, 0);
  mach_port_name_t wrong_task_name = MACH_PORT_NULL;
  const uint64_t wrong_task = raw_trap((uint64_t)-16, MACH_PORT_NULL,
      MACH_PORT_RIGHT_RECEIVE, (uint64_t)(uintptr_t)&wrong_task_name, 0);

  const int names_distinct = first != MACH_PORT_NULL && second != MACH_PORT_NULL &&
      reply != MACH_PORT_NULL && first != second && first != reply && second != reply;
  printf("task=%d allocated=%d names_distinct=%d inserted=%d send_deallocated=%d "
         "receive_only_invalid_right=%d destroyed=%d destroy_reply=%d "
         "destroy_again_invalid_name=%d invalid_right=%llu wrong_task_invalid_dest=%d\n",
      task != MACH_PORT_NULL,
      alloc_first == KERN_SUCCESS && alloc_second == KERN_SUCCESS &&
          first != MACH_PORT_NULL && second != MACH_PORT_NULL,
      names_distinct,
      insert == KERN_SUCCESS,
      deallocate_send == KERN_SUCCESS,
      deallocate_receive_only == KERN_INVALID_RIGHT,
      destroy_first == KERN_SUCCESS && destroy_second == KERN_SUCCESS,
      destroy_reply == KERN_SUCCESS,
      destroy_again == KERN_INVALID_NAME,
      (unsigned long long)invalid_right,
      wrong_task == MACH_SEND_INVALID_DEST);
  return !(task != MACH_PORT_NULL && alloc_first == KERN_SUCCESS &&
      alloc_second == KERN_SUCCESS && names_distinct && insert == KERN_SUCCESS &&
      deallocate_send == KERN_SUCCESS &&
      deallocate_receive_only == KERN_INVALID_RIGHT &&
      destroy_first == KERN_SUCCESS && destroy_second == KERN_SUCCESS &&
      destroy_reply == KERN_SUCCESS && destroy_again == KERN_INVALID_NAME &&
      invalid_right == KERN_INVALID_VALUE && wrong_task == MACH_SEND_INVALID_DEST);
}
