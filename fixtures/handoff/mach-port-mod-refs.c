#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <stdint.h>
#include <stdio.h>

static kern_return_t mod_refs(mach_port_t task, mach_port_t name,
                              mach_port_right_t right, mach_port_delta_t delta) {
  return _kernelrpc_mach_port_mod_refs_trap(task, name, right, delta);
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const kern_return_t send_plus = mod_refs(task, task, MACH_PORT_RIGHT_SEND, 1);
  const kern_return_t send_minus = mod_refs(task, task, MACH_PORT_RIGHT_SEND, -1);
  const kern_return_t task_floor_step = mod_refs(
      task, task, MACH_PORT_RIGHT_SEND, -2);
  const kern_return_t task_floor_hit = mod_refs(
      task, task, MACH_PORT_RIGHT_SEND, -1);
  const kern_return_t task_deallocate_floor =
      _kernelrpc_mach_port_deallocate_trap(task, task);
  const kern_return_t send_underflow = mod_refs(task, task, MACH_PORT_RIGHT_SEND, -0x7fff);
  const kern_return_t oversized_delta = mod_refs(
      task, task, MACH_PORT_RIGHT_SEND, 65536);
  const kern_return_t invalid_target = mod_refs(
      MACH_PORT_NULL, task, MACH_PORT_RIGHT_SEND, 1);
  const kern_return_t unknown_name = mod_refs(
      task, 0xdeadbeefU, MACH_PORT_RIGHT_SEND, 1);
  const kern_return_t invalid_right = mod_refs(
      task, task, MACH_PORT_RIGHT_NUMBER, 1);
  const kern_return_t unsupported_right = mod_refs(
      task, task, MACH_PORT_RIGHT_SEND_ONCE, 0);
  const kern_return_t unsupported_unknown = mod_refs(
      task, 0xdeadbeefU, MACH_PORT_RIGHT_SEND_ONCE, 0);
  const kern_return_t receive_on_send = mod_refs(
      task, task, MACH_PORT_RIGHT_RECEIVE, 0);
  const kern_return_t null_plus = mod_refs(
      task, MACH_PORT_NULL, MACH_PORT_RIGHT_SEND, 1);
  const kern_return_t dead_minus = mod_refs(
      task, MACH_PORT_DEAD, MACH_PORT_RIGHT_SEND, -1);

  mach_port_t receive = MACH_PORT_NULL;
  const kern_return_t allocate = _kernelrpc_mach_port_allocate_trap(
      task, MACH_PORT_RIGHT_RECEIVE, &receive);
  const kern_return_t send_on_receive = mod_refs(
      task, receive, MACH_PORT_RIGHT_SEND, 0);
  const kern_return_t receive_zero = mod_refs(
      task, receive, MACH_PORT_RIGHT_RECEIVE, 0);
  const kern_return_t receive_plus = mod_refs(
      task, receive, MACH_PORT_RIGHT_RECEIVE, 1);
  const kern_return_t receive_minus = mod_refs(
      task, receive, MACH_PORT_RIGHT_RECEIVE, -1);
  const kern_return_t receive_missing = mod_refs(
      task, receive, MACH_PORT_RIGHT_RECEIVE, -1);

  mach_port_t saturated = MACH_PORT_NULL;
  const kern_return_t saturated_allocate = _kernelrpc_mach_port_allocate_trap(
      task, MACH_PORT_RIGHT_RECEIVE, &saturated);
  const kern_return_t saturated_insert = _kernelrpc_mach_port_insert_right_trap(
      task, saturated, saturated, MACH_MSG_TYPE_MAKE_SEND);
  const kern_return_t saturated_plus = mod_refs(
      task, saturated, MACH_PORT_RIGHT_SEND, 65535);
  const kern_return_t saturated_deallocate =
      _kernelrpc_mach_port_deallocate_trap(task, saturated);
  const kern_return_t saturated_minus_one = mod_refs(
      task, saturated, MACH_PORT_RIGHT_SEND, -1);
  const kern_return_t saturated_minus_max = mod_refs(
      task, saturated, MACH_PORT_RIGHT_SEND, -65535);
  const kern_return_t saturated_send_missing = mod_refs(
      task, saturated, MACH_PORT_RIGHT_SEND, -1);
  const kern_return_t saturated_destroy = _kernelrpc_mach_port_destruct_trap(
      task, saturated, 0, MACH_PORT_NULL);

  mach_port_t dead = MACH_PORT_NULL;
  const kern_return_t dead_allocate = _kernelrpc_mach_port_allocate_trap(
      task, MACH_PORT_RIGHT_RECEIVE, &dead);
  const kern_return_t dead_insert = _kernelrpc_mach_port_insert_right_trap(
      task, dead, dead, MACH_MSG_TYPE_MAKE_SEND);
  const kern_return_t dead_destroy = _kernelrpc_mach_port_destruct_trap(
      task, dead, 0, MACH_PORT_NULL);
  const kern_return_t send_after_destroy = mod_refs(
      task, dead, MACH_PORT_RIGHT_SEND, -1);
  const kern_return_t receive_after_destroy = mod_refs(
      task, dead, MACH_PORT_RIGHT_RECEIVE, 0);
  const kern_return_t send_once_dead = mod_refs(
      task, dead, MACH_PORT_RIGHT_SEND_ONCE, 0);
  const kern_return_t dead_plus_one = mod_refs(task, dead, MACH_PORT_RIGHT_DEAD_NAME, 1);
  const kern_return_t dead_plus_max = mod_refs(task, dead, MACH_PORT_RIGHT_DEAD_NAME, 65535);
  const kern_return_t dead_minus_one = mod_refs(task, dead, MACH_PORT_RIGHT_DEAD_NAME, -1);
  const kern_return_t dead_minus_max = mod_refs(
      task, dead, MACH_PORT_RIGHT_DEAD_NAME, -65535);
  const kern_return_t dead_missing = mod_refs(
      task, dead, MACH_PORT_RIGHT_DEAD_NAME, -1);

  const int send_ok = send_plus == KERN_SUCCESS && send_minus == KERN_SUCCESS &&
                      task_floor_step == KERN_SUCCESS &&
                      task_floor_hit == KERN_INVALID_CAPABILITY &&
                      task_deallocate_floor == KERN_INVALID_CAPABILITY &&
                      send_underflow == KERN_INVALID_VALUE &&
                      oversized_delta == KERN_INVALID_VALUE;
  const int errors_ok = invalid_target == MACH_SEND_INVALID_DEST &&
                        unknown_name == KERN_INVALID_NAME &&
                        invalid_right == KERN_INVALID_VALUE &&
                        unsupported_right == KERN_INVALID_RIGHT &&
                        unsupported_unknown == KERN_INVALID_NAME &&
                        receive_on_send == KERN_INVALID_RIGHT;
  const int sentinels_ok = null_plus == KERN_SUCCESS && dead_minus == KERN_SUCCESS;
  const int receive_ok = allocate == KERN_SUCCESS && receive_zero == KERN_SUCCESS &&
                         send_on_receive == KERN_INVALID_RIGHT &&
                         receive_plus == KERN_INVALID_VALUE &&
                         receive_minus == KERN_SUCCESS &&
                         receive_missing == KERN_INVALID_NAME;
  const int saturated_ok = saturated_allocate == KERN_SUCCESS &&
                           saturated_insert == KERN_SUCCESS &&
                           saturated_plus == KERN_SUCCESS &&
                           saturated_deallocate == KERN_SUCCESS &&
                           saturated_minus_one == KERN_SUCCESS &&
                           saturated_minus_max == KERN_SUCCESS &&
                           saturated_send_missing == KERN_INVALID_RIGHT &&
                           saturated_destroy == KERN_SUCCESS;
  const int dead_ok = dead_allocate == KERN_SUCCESS && dead_insert == KERN_SUCCESS &&
                      dead_destroy == KERN_SUCCESS &&
                      send_after_destroy == KERN_INVALID_RIGHT &&
                      receive_after_destroy == KERN_INVALID_RIGHT &&
                      send_once_dead == KERN_INVALID_RIGHT &&
                      dead_plus_one == KERN_SUCCESS && dead_plus_max == KERN_SUCCESS &&
                      dead_minus_one == KERN_SUCCESS && dead_minus_max == KERN_SUCCESS &&
                      dead_missing == KERN_INVALID_NAME;

  printf("task_send plus=%d minus=%d floor_step=%d floor_hit=%d "
         "deallocate_floor=%d underflow=%d oversized_delta=%d\n",
         send_plus, send_minus, task_floor_step, task_floor_hit, task_deallocate_floor,
         send_underflow, oversized_delta);
  printf("errors target=%d unknown=%d right=%d unsupported=%d/%d "
         "sentinels null=%d dead=%d\n",
         invalid_target, unknown_name, invalid_right, unsupported_right,
         unsupported_unknown, null_plus, dead_minus);
  printf("receive allocate=%d zero=%d plus=%d minus=%d missing=%d\n",
         allocate, receive_zero, receive_plus, receive_minus, receive_missing);
  printf("saturated allocate=%d insert=%d plus=%d deallocate=%d minus_one=%d "
         "minus_max=%d send_missing=%d destroy=%d\n",
         saturated_allocate, saturated_insert, saturated_plus, saturated_deallocate,
         saturated_minus_one, saturated_minus_max, saturated_send_missing,
         saturated_destroy);
  printf("dead allocate=%d insert=%d destroy=%d send=%d receive=%d send_once=%d "
         "refs=%d/%d/%d/%d missing=%d\n",
         dead_allocate, dead_insert, dead_destroy, send_after_destroy,
         receive_after_destroy, send_once_dead, dead_plus_one, dead_plus_max,
         dead_minus_one, dead_minus_max, dead_missing);
  return send_ok && errors_ok && sentinels_ok && receive_ok && saturated_ok && dead_ok
      ? 0 : 1;
}
