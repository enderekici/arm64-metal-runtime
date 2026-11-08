#include <mach/mach.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t trap(uint64_t number, uint64_t a, uint64_t b) {
  register uint64_t x0 __asm__("x0") = a;
  register uint64_t x1 __asm__("x1") = b;
  register uint64_t x16 __asm__("x16") = number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory");
  return x0;
}
static int type_is(mach_port_t task, mach_port_name_t name, mach_port_type_t bit) {
  mach_port_type_t type = 0;
  return mach_port_type(task, name, &type) == KERN_SUCCESS && type == bit;
}
static int destroy(mach_port_t task, mach_port_name_t name) {
  return mach_port_destruct(task, name, 0, MACH_PORT_NULL) == KERN_SUCCESS;
}
int main(void) {
  mach_port_t task = mach_task_self();
  mach_port_urefs_t task_refs_before = 0;
  kern_return_t task_refs_before_result = mach_port_get_refs(task, task, MACH_PORT_RIGHT_SEND, &task_refs_before);
  mach_port_t task_trap1 = (mach_port_t)trap((uint64_t)-28, 0, 0);
  mach_port_t task_trap2 = (mach_port_t)trap((uint64_t)-28, 0, 0);
  mach_port_urefs_t task_refs_after = 0;
  kern_return_t task_refs_after_result = mach_port_get_refs(task, task, MACH_PORT_RIGHT_SEND, &task_refs_after);
  uint64_t task_dealloc1 = trap((uint64_t)-18, task, task_trap1);
  mach_port_urefs_t task_refs_after_dealloc1 = 0;
  kern_return_t task_refs_after_dealloc1_result = mach_port_get_refs(task, task, MACH_PORT_RIGHT_SEND, &task_refs_after_dealloc1);
  uint64_t task_dealloc2 = trap((uint64_t)-18, task, task_trap2);
  mach_port_urefs_t task_refs_after_dealloc2 = 0;
  kern_return_t task_refs_after_dealloc2_result = mach_port_get_refs(task, task, MACH_PORT_RIGHT_SEND, &task_refs_after_dealloc2);
  mach_port_t h1 = (mach_port_t)trap((uint64_t)-29, 0, 0);
  mach_port_t h2 = (mach_port_t)trap((uint64_t)-29, 0, 0);
  mach_port_urefs_t refs0 = 0, refs1 = 0;
  mach_port_type_t host_type_before = 0;
  kern_return_t host_type_before_result = mach_port_type(task, h1, &host_type_before);
  kern_return_t gr0 = mach_port_get_refs(task, h1, MACH_PORT_RIGHT_SEND, &refs0);
  uint64_t d1 = trap((uint64_t)-18, task, h1);
  kern_return_t gr1 = mach_port_get_refs(task, h1, MACH_PORT_RIGHT_SEND, &refs1);
  uint64_t d2 = trap((uint64_t)-18, task, h2);
  uint64_t d3 = trap((uint64_t)-18, task, h1);

  mach_port_t p = MACH_PORT_NULL;
  int pcreated = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &p) == KERN_SUCCESS &&
    mach_port_insert_right(task, p, p, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS;
  uint64_t badtask = trap((uint64_t)-18, MACH_PORT_NULL, p);
  uint64_t badname = trap((uint64_t)-18, task, 0x12345678);
  uint64_t psend = pcreated ? trap((uint64_t)-18, task, p) : KERN_FAILURE;
  int premoved = pcreated && type_is(task, p, MACH_PORT_TYPE_RECEIVE);
  int pdestroyed = pcreated && destroy(task, p);

  mach_port_t r = MACH_PORT_NULL;
  int rcreated = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &r) == KERN_SUCCESS;
  uint64_t rdealloc = rcreated ? trap((uint64_t)-18, task, r) : KERN_FAILURE;
  int rpreserved = rcreated && type_is(task, r, MACH_PORT_TYPE_RECEIVE);
  int rdestroyed = rcreated && destroy(task, r);

  mach_port_t dead = MACH_PORT_NULL;
  int deadcreated = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &dead) == KERN_SUCCESS &&
    mach_port_insert_right(task, dead, dead, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS;
  int dead_destroyed_receive = deadcreated && destroy(task, dead);
  int dead_is_dead = deadcreated && type_is(task, dead, MACH_PORT_TYPE_DEAD_NAME);
  uint64_t dead_dealloc = deadcreated ? trap((uint64_t)-18, task, dead) : KERN_FAILURE;

  mach_port_t once = MACH_PORT_NULL;
  mach_port_t transit = MACH_PORT_NULL;
  int oncecreated = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &once) == KERN_SUCCESS &&
    mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &transit) == KERN_SUCCESS &&
    mach_port_insert_right(task, transit, transit, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS;
  typedef struct { mach_msg_header_t header; mach_msg_max_trailer_t trailer; } once_message_t;
  once_message_t message;
  memset(&message, 0, sizeof(message));
  message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                             MACH_MSG_TYPE_MAKE_SEND_ONCE);
  message.header.msgh_size = sizeof(mach_msg_header_t);
  message.header.msgh_remote_port = transit;
  message.header.msgh_local_port = once;
  message.header.msgh_id = 0x4f4e4345;
  kern_return_t once_send = oncecreated ? mach_msg(&message.header, MACH_SEND_MSG,
      sizeof(mach_msg_header_t), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
      MACH_PORT_NULL) : KERN_FAILURE;
  kern_return_t once_receive_result = once_send == MACH_MSG_SUCCESS
      ? mach_msg(&message.header, MACH_RCV_MSG, 0, sizeof(message), transit,
          MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) : KERN_FAILURE;
  mach_port_name_t once_name = once_receive_result == MACH_MSG_SUCCESS
      ? message.header.msgh_remote_port : MACH_PORT_NULL;
  mach_port_type_t once_type = 0;
  int once_present = once_name != MACH_PORT_NULL &&
      mach_port_type(task, once_name, &once_type) == KERN_SUCCESS &&
      (once_type & MACH_PORT_TYPE_SEND_ONCE) != 0;
  uint64_t once_dealloc = once_present ? trap((uint64_t)-18, task, once_name) : KERN_FAILURE;
  int once_receive = once_present && type_is(task, once, MACH_PORT_TYPE_RECEIVE);
  uint64_t once_again = once_present ? trap((uint64_t)-18, task, once_name) : KERN_FAILURE;
  int once_destroyed = oncecreated && destroy(task, once);
  int transit_destroyed = oncecreated && destroy(task, transit);

  printf("task_same=%d task_incremented_twice=%d task_decremented_once=%d task_decremented_twice=%d host_same=%d host_send=%d refs_ge2=%d refs_minus1=%d d1_ok=%d d2_ok=%d d3_invalid=%d "
    "bad_task_invalid_dest=%d bad_name_invalid_name=%d send_dealloc_ok=%d send_removed=%d "
    "send_receive_destroyed=%d receive_dealloc_invalid_right=%d receive_preserved=%d "
    "receive_destroyed=%d dead_receive_destroyed=%d dead_name_present=%d "
    "dead_dealloc_ok=%d once_present=%d once_dealloc_ok=%d once_receive_preserved=%d "
    "once_again_invalid_right=%d once_dead_name_removed=%d once_destroyed=%d transit_destroyed=%d\n",
    task_trap1 == task && task_trap2 == task,
    task_refs_before_result == KERN_SUCCESS && task_refs_after_result == KERN_SUCCESS && task_refs_after == task_refs_before + 2,
    task_refs_after_dealloc1_result == KERN_SUCCESS && task_refs_after_dealloc1 + 1 == task_refs_after,
    task_refs_after_dealloc2_result == KERN_SUCCESS && task_refs_after_dealloc2 + 1 == task_refs_after_dealloc1,
    h1 != MACH_PORT_NULL && h1 == h2, type_is(task, h1, MACH_PORT_TYPE_SEND_RECEIVE) ||
      (host_type_before_result == KERN_SUCCESS && (host_type_before & MACH_PORT_TYPE_SEND) != 0),
    gr0 == KERN_SUCCESS && refs0 >= 2, gr1 == KERN_SUCCESS && refs1 + 1 == refs0,
    d1 == KERN_SUCCESS, d2 == KERN_SUCCESS, d3 == KERN_INVALID_NAME,
    badtask == MACH_SEND_INVALID_DEST, badname == KERN_INVALID_NAME,
    psend == KERN_SUCCESS, premoved, pdestroyed,
    rdealloc == KERN_INVALID_RIGHT, rpreserved, rdestroyed,
    dead_destroyed_receive, dead_is_dead, dead_dealloc == KERN_SUCCESS,
    once_present, once_dealloc == KERN_SUCCESS, once_receive,
    once_again == KERN_INVALID_RIGHT || once_again == KERN_INVALID_NAME,
    mach_port_type(task, once_name, &once_type) == KERN_INVALID_NAME, once_destroyed, transit_destroyed);
  return !(task_trap1 == task && task_trap2 == task && task_refs_before_result == KERN_SUCCESS && task_refs_after_result == KERN_SUCCESS && task_refs_after == task_refs_before + 2 && task_refs_after_dealloc1_result == KERN_SUCCESS && task_refs_after_dealloc1 + 1 == task_refs_after && task_refs_after_dealloc2_result == KERN_SUCCESS && task_refs_after_dealloc2 + 1 == task_refs_after_dealloc1 && task_dealloc1 == KERN_SUCCESS && task_dealloc2 == KERN_SUCCESS && h1 != MACH_PORT_NULL && h1 == h2 && gr0 == KERN_SUCCESS && refs0 >= 2 &&
    gr1 == KERN_SUCCESS && refs1 + 1 == refs0 && d1 == KERN_SUCCESS && d2 == KERN_SUCCESS &&
    (d3 == KERN_INVALID_NAME || d3 == KERN_INVALID_RIGHT) && badtask == MACH_SEND_INVALID_DEST &&
    badname == KERN_INVALID_NAME && pcreated && psend == KERN_SUCCESS && premoved && pdestroyed &&
    rcreated && rdealloc == KERN_INVALID_RIGHT && rpreserved && rdestroyed && deadcreated &&
    dead_destroyed_receive && dead_is_dead && dead_dealloc == KERN_SUCCESS && oncecreated &&
    once_present && once_dealloc == KERN_SUCCESS && once_receive &&
    (once_again == KERN_INVALID_RIGHT || once_again == KERN_INVALID_NAME) &&
    mach_port_type(task, once_name, &once_type) == KERN_INVALID_NAME && once_destroyed && transit_destroyed);
}
