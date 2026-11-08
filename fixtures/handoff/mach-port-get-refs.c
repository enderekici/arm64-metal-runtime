#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdio.h>
#include <stdint.h>

#define SENTINEL ((mach_port_urefs_t)0xdecafbadU)

static kern_return_t query(mach_port_t task, mach_port_name_t name,
                           mach_port_right_t right, mach_port_urefs_t *out) {
  *out = SENTINEL;
  return mach_port_get_refs(task, name, right, out);
}

static int invalid_untouched(mach_port_t task, mach_port_name_t name,
                             mach_port_right_t right, kern_return_t expected) {
  mach_port_urefs_t out = SENTINEL;
  return mach_port_get_refs(task, name, right, &out) == expected && out == SENTINEL;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_port_t host = mach_host_self();
  const mach_port_t thread = mach_thread_self();
  mach_port_urefs_t task_refs = SENTINEL;
  mach_port_urefs_t host_refs = SENTINEL;
  mach_port_urefs_t thread_refs = SENTINEL;
  const kern_return_t task_result = query(task, task, MACH_PORT_RIGHT_SEND, &task_refs);
  const kern_return_t host_result = query(task, host, MACH_PORT_RIGHT_SEND, &host_refs);
  const kern_return_t thread_result = query(task, thread, MACH_PORT_RIGHT_SEND, &thread_refs);
  const int special_send = task_result == KERN_SUCCESS && task_refs > 0 &&
      host_result == KERN_SUCCESS && host_refs > 0 &&
      thread_result == KERN_SUCCESS && thread_refs > 0;

  mach_port_t receive = MACH_PORT_NULL;
  const kern_return_t allocate = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &receive);
  mach_port_urefs_t receive_refs = SENTINEL;
  mach_port_urefs_t send_zero = SENTINEL;
  const kern_return_t receive_result = query(
      task, receive, MACH_PORT_RIGHT_RECEIVE, &receive_refs);
  const kern_return_t send_zero_result = query(task, receive, MACH_PORT_RIGHT_SEND, &send_zero);
  const kern_return_t insert = mach_port_insert_right(
      task, receive, receive, MACH_MSG_TYPE_MAKE_SEND);
  mach_port_urefs_t send_refs = SENTINEL;
  const kern_return_t send_result = query(task, receive, MACH_PORT_RIGHT_SEND, &send_refs);
  const kern_return_t remove_send = mach_port_deallocate(task, receive);
  mach_port_urefs_t send_after = SENTINEL;
  const kern_return_t send_after_result = query(
      task, receive, MACH_PORT_RIGHT_SEND, &send_after);
  const kern_return_t destroy_receive = mach_port_destruct(task, receive, 0, MACH_PORT_NULL);
  const int destroyed_gone = invalid_untouched(
      task, receive, MACH_PORT_RIGHT_RECEIVE, KERN_INVALID_NAME);

  mach_port_t dead = MACH_PORT_NULL;
  const kern_return_t allocate_dead = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &dead);
  const kern_return_t insert_dead = allocate_dead == KERN_SUCCESS
      ? mach_port_insert_right(task, dead, dead, MACH_MSG_TYPE_MAKE_SEND) : KERN_FAILURE;
  const kern_return_t destroy_dead = insert_dead == KERN_SUCCESS
      ? mach_port_destruct(task, dead, 0, MACH_PORT_NULL) : KERN_FAILURE;
  mach_port_urefs_t dead_refs = SENTINEL;
  mach_port_urefs_t dead_send_refs = SENTINEL;
  const kern_return_t dead_result = query(task, dead, MACH_PORT_RIGHT_DEAD_NAME, &dead_refs);
  const kern_return_t dead_send_result = query(task, dead, MACH_PORT_RIGHT_SEND, &dead_send_refs);
  const kern_return_t release_dead = mach_port_deallocate(task, dead);
  const int dead_gone = invalid_untouched(
      task, dead, MACH_PORT_RIGHT_DEAD_NAME, KERN_INVALID_NAME);

  mach_port_urefs_t null_send = SENTINEL;
  mach_port_urefs_t dead_sentinel_send = SENTINEL;
  const kern_return_t null_result = query(
      task, MACH_PORT_NULL, MACH_PORT_RIGHT_SEND, &null_send);
  const kern_return_t dead_sentinel_result = query(
      task, MACH_PORT_DEAD, MACH_PORT_RIGHT_SEND, &dead_sentinel_send);
  const int null_receive_invalid = invalid_untouched(
      task, MACH_PORT_NULL, MACH_PORT_RIGHT_RECEIVE, KERN_INVALID_NAME);
  const int dead_sentinel_dead_invalid = invalid_untouched(
      task, MACH_PORT_DEAD, MACH_PORT_RIGHT_DEAD_NAME, KERN_INVALID_NAME);
  const int unknown_invalid = invalid_untouched(
      task, 0xdeadbeefU, MACH_PORT_RIGHT_SEND, KERN_INVALID_NAME);
  const int invalid_task = invalid_untouched(
      MACH_PORT_NULL, task, MACH_PORT_RIGHT_SEND, MACH_SEND_INVALID_DEST);
  const int host_as_task = invalid_untouched(
      host, task, MACH_PORT_RIGHT_SEND, KERN_INVALID_TASK);
  const int invalid_right = invalid_untouched(
      task, task, MACH_PORT_RIGHT_NUMBER, KERN_INVALID_VALUE);
  mach_port_urefs_t absent_send_once = SENTINEL;
  mach_port_urefs_t absent_port_set = SENTINEL;
  const kern_return_t send_once_result = query(
      task, task, MACH_PORT_RIGHT_SEND_ONCE, &absent_send_once);
  const kern_return_t port_set_result = query(
      task, task, MACH_PORT_RIGHT_PORT_SET, &absent_port_set);

  printf("special_send=%d\n", special_send);
  printf("receive allocate=%d right=%d:%u send_zero=%d:%u insert=%d send=%d:%u "
         "remove=%d send_after=%d:%u destroy=%d gone=%d\n",
         allocate, receive_result, receive_refs, send_zero_result, send_zero,
         insert, send_result, send_refs, remove_send, send_after_result,
         send_after, destroy_receive, destroyed_gone);
  printf("dead destroy=%d dead_name=%d:%u send=%d:%u release=%d gone=%d\n",
         destroy_dead, dead_result, dead_refs, dead_send_result, dead_send_refs,
         release_dead, dead_gone);
  printf("sentinels null_send=%d:%u dead_send=%d:%u null_receive_invalid=%d "
         "dead_name_invalid=%d\n",
         null_result, null_send, dead_sentinel_result, dead_sentinel_send,
         null_receive_invalid, dead_sentinel_dead_invalid);
  printf("errors unknown=%d invalid_task=%d host_task=%d right=%d "
         "send_once=%d:%u port_set=%d:%u\n",
         unknown_invalid, invalid_task, host_as_task, invalid_right,
         send_once_result, absent_send_once, port_set_result, absent_port_set);

  (void)mach_port_deallocate(task, thread);
  (void)mach_port_deallocate(task, host);
  (void)mach_port_deallocate(task, task);
  return !(special_send && allocate == KERN_SUCCESS && receive_result == KERN_SUCCESS &&
      receive_refs == 1 && send_zero_result == KERN_SUCCESS && send_zero == 0 &&
      insert == KERN_SUCCESS && send_result == KERN_SUCCESS && send_refs == 1 &&
      remove_send == KERN_SUCCESS && send_after_result == KERN_SUCCESS && send_after == 0 &&
      destroy_receive == KERN_SUCCESS && destroyed_gone && allocate_dead == KERN_SUCCESS &&
      insert_dead == KERN_SUCCESS && destroy_dead == KERN_SUCCESS && dead_result == KERN_SUCCESS &&
      dead_refs == 1 && dead_send_result == KERN_SUCCESS && dead_send_refs == 0 &&
      release_dead == KERN_SUCCESS && dead_gone && null_result == KERN_SUCCESS && null_send == 1 &&
      dead_sentinel_result == KERN_SUCCESS && dead_sentinel_send == 1 && null_receive_invalid &&
      dead_sentinel_dead_invalid && unknown_invalid && invalid_task && host_as_task &&
      invalid_right && send_once_result == KERN_SUCCESS && absent_send_once == 0 &&
      port_set_result == KERN_SUCCESS && absent_port_set == 0);
}
