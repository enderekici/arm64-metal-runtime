#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static uint64_t call_send(void *data, uint32_t size, mach_port_t remote, int32_t id) {
  const uint32_t bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  /* Match libSystem's generic mach_msg2 send classification. */
  const uint64_t options = MACH_SEND_MSG | (UINT64_C(8) << 32);
  return mach_msg2_raw(data, options, ((uint64_t)size << 32) | bits,
      remote, (uint64_t)(uint32_t)id << 32, 0, 0, 0);
}

static uint64_t call_receive(void *data, mach_port_name_t name, uint32_t size,
                             uint64_t options, uint64_t timeout) {
  return mach_msg2_raw(data, options, 0, 0, 0,
      (uint64_t)name << 32, size, timeout);
}

int main(void) {
  const mach_port_t task = mach_task_self();
  mach_port_t port = MACH_PORT_NULL;
  const kern_return_t allocated = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port);
  const kern_return_t inserted = allocated == KERN_SUCCESS
      ? mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND) : KERN_FAILURE;

  struct {
    mach_msg_header_t header;
    uint32_t payload;
  } message = {0};
  message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  message.header.msgh_size = sizeof(message);
  message.header.msgh_remote_port = port;
  message.header.msgh_id = 0x4d534731;
  message.payload = 0x1234abcd;
  const uint64_t invalid_destination = call_send(&message, sizeof(message),
      MACH_PORT_NULL, message.header.msgh_id);
  const int invalid_destination_rejected = invalid_destination == MACH_SEND_INVALID_DEST;
  const uint64_t sent = call_send(&message, sizeof(message), port, message.header.msgh_id);

  struct {
    mach_msg_header_t header;
    uint32_t payload;
    mach_msg_max_trailer_t trailer;
  } received = {0};
  const uint64_t receive = call_receive(&received, port, sizeof(received), MACH_RCV_MSG, 0);
  const int received_message = receive == MACH_MSG_SUCCESS &&
      received.header.msgh_id == 0x4d534731 && received.payload == 0x1234abcd;

  message.header.msgh_id = 1000;
  message.payload = 0x87654321;
  const uint64_t local_id_1000_send = call_send(&message, sizeof(message), port, 1000);
  memset(&received, 0, sizeof(received));
  const uint64_t local_id_1000_receive = call_receive(
      &received, port, sizeof(received), MACH_RCV_MSG, 0);
  const int local_id_1000 = local_id_1000_send == MACH_MSG_SUCCESS &&
      local_id_1000_receive == MACH_MSG_SUCCESS && received.header.msgh_id == 1000 &&
      received.payload == 0x87654321;

  struct {
    mach_msg_header_t header;
    mach_msg_max_trailer_t trailer;
  } empty = {0};
  const uint64_t timeout = call_receive(&empty, port, sizeof(empty),
      MACH_RCV_MSG | MACH_RCV_TIMEOUT, 1);

  struct { mach_msg_header_t header; uint32_t payload; } bad_message = {0};
  bad_message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  bad_message.header.msgh_size = sizeof(bad_message);
  bad_message.header.msgh_remote_port = port;
  bad_message.header.msgh_id = 0x4d534732;
  mach_port_t receive_only = MACH_PORT_NULL;
  const kern_return_t receive_only_allocated = mach_port_allocate(
      task, MACH_PORT_RIGHT_RECEIVE, &receive_only);
  bad_message.header.msgh_remote_port = receive_only;
  const uint64_t receive_only_send = call_send(&bad_message, sizeof(bad_message),
                                               receive_only, bad_message.header.msgh_id);
  const uint64_t invalid_receive = call_receive(&empty, 0x12345678,
      sizeof(empty), MACH_RCV_MSG | MACH_RCV_TIMEOUT, 1);

  printf("task=%d allocate=%d insert=%d invalid_destination=%d send=%d "
         "receive=%d payload=%d local_id_1000=%d timeout=%llu receive_only_send=%llu "
         "invalid_receive=%llu\n",
      task != MACH_PORT_NULL, allocated == KERN_SUCCESS, inserted == KERN_SUCCESS,
      invalid_destination_rejected, sent == MACH_MSG_SUCCESS,
      receive == MACH_MSG_SUCCESS, received_message, local_id_1000,
      (unsigned long long)timeout, (unsigned long long)receive_only_send,
      (unsigned long long)invalid_receive);

  if (receive_only_allocated == KERN_SUCCESS) {
    (void)mach_port_destruct(task, receive_only, 0, MACH_PORT_NULL);
  }
  (void)mach_port_destruct(task, port, 0, MACH_PORT_NULL);
  return !(task != MACH_PORT_NULL && allocated == KERN_SUCCESS &&
      inserted == KERN_SUCCESS && invalid_destination_rejected &&
      sent == MACH_MSG_SUCCESS && receive == MACH_MSG_SUCCESS && received_message &&
      local_id_1000 &&
      receive_only_allocated == KERN_SUCCESS &&
      timeout == MACH_RCV_TIMED_OUT && receive_only_send == MACH_SEND_INVALID_DEST &&
      invalid_receive == MACH_RCV_INVALID_NAME);
}
