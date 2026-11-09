#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/task.h>
#include <mach/task_special_ports.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t specialPort;
  NDR_record_t ndr;
  int32_t whichPort;
} TaskSetSpecialPortRequest;

static uint64_t mach_msg2_raw(void *data, uint64_t options,
                              uint64_t bits_and_send_size,
                              uint64_t remote_and_local_port,
                              uint64_t voucher_and_id,
                              uint64_t desc_count_and_receive_name,
                              uint64_t receive_size_and_priority,
                              uint64_t timeout) {
  register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)data;
  register uint64_t x1 __asm__("x1") = options;
  register uint64_t x2 __asm__("x2") = bits_and_send_size;
  register uint64_t x3 __asm__("x3") = remote_and_local_port;
  register uint64_t x4 __asm__("x4") = voucher_and_id;
  register uint64_t x5 __asm__("x5") = desc_count_and_receive_name;
  register uint64_t x6 __asm__("x6") = receive_size_and_priority;
  register uint64_t x7 __asm__("x7") = timeout;
  register uint64_t x16 __asm__("x16") = (uint64_t)-47;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x6), "r"(x7), "r"(x16) : "memory");
  return x0;
}

static uint64_t set_with_short_reply(mach_port_t task, mach_port_t port) {
  mach_port_t reply = MACH_PORT_NULL;
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS) {
    return KERN_FAILURE;
  }
  TaskSetSpecialPortRequest request = {0};
  request.header.msgh_bits = MACH_MSGH_BITS_COMPLEX |
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
  request.header.msgh_size = sizeof(request);
  request.header.msgh_remote_port = task;
  request.header.msgh_local_port = reply;
  request.header.msgh_id = 3410;
  request.body.msgh_descriptor_count = 1;
  request.specialPort.name = port;
  request.specialPort.disposition = MACH_MSG_TYPE_COPY_SEND;
  request.specialPort.type = MACH_MSG_PORT_DESCRIPTOR;
  request.ndr = NDR_record;
  request.whichPort = TASK_DEBUG_CONTROL_PORT;
  const uint64_t result = mach_msg2_raw(&request,
      MACH_SEND_MSG | MACH_RCV_MSG | (UINT64_C(2) << 32),
      ((uint64_t)sizeof(request) << 32) | request.header.msgh_bits,
      ((uint64_t)reply << 32) | task,
      (uint64_t)3410 << 32,
      ((uint64_t)reply << 32) | 1,
      sizeof(mach_msg_header_t) + sizeof(NDR_record_t) + sizeof(kern_return_t), 0);
  (void)mach_port_destruct(task, reply, 0, 0);
  return result;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_port_context_t context = UINT64_C(0x71b75ace);
  mach_port_options_t options = {0};
  options.flags = MPO_CONTEXT_AS_GUARD | MPO_INSERT_SEND_RIGHT | MPO_STRICT;
  mach_port_t name = MACH_PORT_NULL;
  const kern_return_t created = mach_port_construct(task, &options, context, &name);
  const uint64_t short_reply = created == KERN_SUCCESS
      ? set_with_short_reply(task, name) : created;
  mach_port_t short_reply_get_name = MACH_PORT_NULL;
  const kern_return_t short_reply_get = short_reply == MACH_RCV_TOO_LARGE
      ? task_get_special_port(task, TASK_DEBUG_CONTROL_PORT, &short_reply_get_name)
      : (kern_return_t)short_reply;
  const int short_reply_mutated = short_reply_get == KERN_SUCCESS &&
      short_reply_get_name == name;
  const kern_return_t short_reply_deallocate = short_reply_mutated
      ? mach_port_deallocate(task, short_reply_get_name) : short_reply_get;
  mach_port_urefs_t refs_after_create = 0;
  const kern_return_t refs0 = created == KERN_SUCCESS
      ? mach_port_get_refs(task, name, MACH_PORT_RIGHT_SEND, &refs_after_create) : created;
  const kern_return_t set = short_reply_deallocate == KERN_SUCCESS
      ? task_set_special_port(task, TASK_DEBUG_CONTROL_PORT, name)
      : short_reply_deallocate;
  mach_port_urefs_t refs_after_set = 0;
  const kern_return_t refs1 = set == KERN_SUCCESS
      ? mach_port_get_refs(task, name, MACH_PORT_RIGHT_SEND, &refs_after_set) : set;
  mach_port_t returned = MACH_PORT_NULL;
  const kern_return_t get = set == KERN_SUCCESS
      ? task_get_special_port(task, TASK_DEBUG_CONTROL_PORT, &returned) : set;
  mach_port_urefs_t refs_after_get = 0;
  const kern_return_t refs2 = get == KERN_SUCCESS
      ? mach_port_get_refs(task, name, MACH_PORT_RIGHT_SEND, &refs_after_get) : get;
  const kern_return_t returned_deallocate = get == KERN_SUCCESS
      ? mach_port_deallocate(task, returned) : get;
  const kern_return_t caller_deallocate = returned_deallocate == KERN_SUCCESS
      ? mach_port_deallocate(task, name) : returned_deallocate;
  returned = MACH_PORT_NULL;
  const kern_return_t retained_get = caller_deallocate == KERN_SUCCESS
      ? task_get_special_port(task, TASK_DEBUG_CONTROL_PORT, &returned) : caller_deallocate;
  const int retained = retained_get == KERN_SUCCESS && returned == name;
  const kern_return_t retained_deallocate = retained_get == KERN_SUCCESS
      ? mach_port_deallocate(task, returned) : retained_get;
  const kern_return_t clear = retained_deallocate == KERN_SUCCESS
      ? task_set_special_port(task, TASK_DEBUG_CONTROL_PORT, MACH_PORT_NULL)
      : retained_deallocate;
  returned = MACH_PORT_DEAD;
  const kern_return_t cleared_get = clear == KERN_SUCCESS
      ? task_get_special_port(task, TASK_DEBUG_CONTROL_PORT, &returned) : clear;
  const int cleared = cleared_get == KERN_SUCCESS && returned == MACH_PORT_NULL;
  const kern_return_t destroy = clear == KERN_SUCCESS
      ? mach_port_destruct(task, name, 0, context) : clear;
  const mach_port_context_t dead_context = UINT64_C(0x71b75acf);
  mach_port_t dead_name = MACH_PORT_NULL;
  const kern_return_t dead_create = destroy == KERN_SUCCESS
      ? mach_port_construct(task, &options, dead_context, &dead_name) : destroy;
  const kern_return_t dead_set = dead_create == KERN_SUCCESS
      ? task_set_special_port(task, TASK_DEBUG_CONTROL_PORT, dead_name) : dead_create;
  const kern_return_t dead_destroy = dead_set == KERN_SUCCESS
      ? mach_port_destruct(task, dead_name, 0, dead_context) : dead_set;
  returned = MACH_PORT_NULL;
  const kern_return_t dead_get = dead_destroy == KERN_SUCCESS
      ? task_get_special_port(task, TASK_DEBUG_CONTROL_PORT, &returned) : dead_destroy;
  const int dead_slot = dead_get == KERN_SUCCESS && returned == MACH_PORT_DEAD;
  const kern_return_t dead_clear = dead_get == KERN_SUCCESS
      ? task_set_special_port(task, TASK_DEBUG_CONTROL_PORT, MACH_PORT_NULL) : dead_get;
  const kern_return_t dead_name_deallocate = dead_clear == KERN_SUCCESS
      ? mach_port_deallocate(task, dead_name) : dead_clear;
  const int pass = created == KERN_SUCCESS && short_reply == MACH_RCV_TOO_LARGE &&
      short_reply_mutated && short_reply_deallocate == KERN_SUCCESS &&
      refs0 == KERN_SUCCESS &&
      refs_after_create == 1 && set == KERN_SUCCESS && refs1 == KERN_SUCCESS &&
      refs_after_set == 1 && get == KERN_SUCCESS && returned_deallocate == KERN_SUCCESS &&
      refs2 == KERN_SUCCESS && refs_after_get == 2 && caller_deallocate == KERN_SUCCESS &&
      retained && retained_deallocate == KERN_SUCCESS && clear == KERN_SUCCESS &&
      cleared && destroy == KERN_SUCCESS && dead_create == KERN_SUCCESS &&
      dead_set == KERN_SUCCESS && dead_destroy == KERN_SUCCESS && dead_slot &&
      dead_clear == KERN_SUCCESS && dead_name_deallocate == KERN_SUCCESS;
  printf("create=%d short_reply=%llu short_mutated=%u refs=%u set=%d "
         "refs_after_set=%u get=%d refs_after_get=%u "
         "retained=%u clear=%d cleared=%u destruct=%d dead_slot=%u "
         "dead_clear=%d dead_destruct=%d result=%s\n",
         created, (unsigned long long)short_reply, short_reply_mutated,
         refs_after_create, set, refs_after_set, get, refs_after_get,
         retained, clear, cleared, destroy, dead_slot, dead_clear, dead_destroy,
         pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
