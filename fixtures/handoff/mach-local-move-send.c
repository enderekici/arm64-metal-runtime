#include <mach/mach.h>
#include <mach/mach_port.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { RequestId = 0x7152, ReceiveCapacity = 48 };

struct Message {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t moved;
  mach_msg_trailer_t trailer;
};

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

static uint64_t mach_trap4(uint64_t number, uint64_t a0, uint64_t a1,
                           uint64_t a2, uint64_t a3) {
  register uint64_t x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register uint64_t x2 __asm__("x2") = a2;
  register uint64_t x3 __asm__("x3") = a3;
  register uint64_t x16 __asm__("x16") = number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x16) : "memory");
  return x0;
}

static kern_return_t receive_right(mach_port_t task, mach_port_t *name) {
  return (kern_return_t)mach_trap4((uint64_t)-16, task, MACH_PORT_RIGHT_RECEIVE,
                                   (uint64_t)(uintptr_t)name, 0);
}

static kern_return_t insert_send(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-21, task, name, name,
                                   MACH_MSG_TYPE_MAKE_SEND);
}

static kern_return_t deallocate_send(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-18, task, name, 0, 0);
}

static kern_return_t release_receive(mach_port_t task, mach_port_t name) {
  return (kern_return_t)mach_trap4((uint64_t)-19, task, name,
                                   MACH_PORT_RIGHT_RECEIVE, (uint64_t)-1);
}

static kern_return_t raw_port_type(mach_port_t task, mach_port_t name,
                                   mach_port_type_t *type) {
  return (kern_return_t)mach_trap4((uint64_t)-76, task, name,
                                   (uint64_t)(uintptr_t)type, 0);
}

int main(void) {
  const mach_port_t task = mach_task_self();
  mach_port_t service = MACH_PORT_NULL, session = MACH_PORT_NULL;
  kern_return_t allocate_service = receive_right(task, &service);
  kern_return_t allocate_session = receive_right(task, &session);
  kern_return_t insert_service = allocate_service == KERN_SUCCESS
      ? insert_send(task, service) : KERN_FAILURE;
  kern_return_t insert_session = allocate_session == KERN_SUCCESS
      ? insert_send(task, session) : KERN_FAILURE;

  struct Message outgoing;
  memset(&outgoing, 0, sizeof(outgoing));
  outgoing.header.msgh_bits = MACH_MSGH_BITS_COMPLEX |
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  outgoing.header.msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) +
      sizeof(mach_msg_port_descriptor_t);
  outgoing.header.msgh_remote_port = service;
  outgoing.header.msgh_id = RequestId;
  outgoing.body.msgh_descriptor_count = 1;
  outgoing.moved.name = session;
  outgoing.moved.disposition = MACH_MSG_TYPE_MOVE_SEND;
  outgoing.moved.type = MACH_MSG_PORT_DESCRIPTOR;
  const uint64_t send = mach_msg2_raw(&outgoing.header,
      MACH_SEND_MSG | (UINT64_C(8) << 32),
      ((uint64_t)outgoing.header.msgh_size << 32) | outgoing.header.msgh_bits,
      service, (uint64_t)RequestId << 32, 1, 0, 0);

  mach_port_type_t in_transit_type = MACH_PORT_TYPE_NONE;
  const kern_return_t in_transit_type_status = raw_port_type(
      task, session, &in_transit_type);

  struct Message incoming;
  memset(&incoming, 0, sizeof(incoming));
  const uint64_t receive = mach_msg2_raw(&incoming.header, MACH_RCV_MSG, 0, 0, 0,
      (uint64_t)service << 32, ReceiveCapacity, 0);
  mach_port_type_t moved_type = MACH_PORT_TYPE_NONE;
  const kern_return_t moved_type_status = incoming.moved.name == MACH_PORT_NULL
      ? KERN_INVALID_NAME : raw_port_type(task, incoming.moved.name, &moved_type);
  const int move_observed = in_transit_type_status == KERN_SUCCESS &&
      (in_transit_type & MACH_PORT_TYPE_RECEIVE) != 0 &&
      (in_transit_type & MACH_PORT_TYPE_SEND) == 0;
  const int descriptor_valid = receive == MACH_MSG_SUCCESS &&
      incoming.header.msgh_id == RequestId &&
      incoming.header.msgh_size == outgoing.header.msgh_size &&
      incoming.header.msgh_bits ==
          (MACH_MSGH_BITS_COMPLEX | (MACH_MSG_TYPE_MOVE_SEND << 8)) &&
      incoming.header.msgh_remote_port == MACH_PORT_NULL &&
      incoming.header.msgh_local_port == service &&
      incoming.header.msgh_voucher_port == MACH_PORT_NULL &&
      incoming.body.msgh_descriptor_count == 1 &&
      incoming.moved.pad1 == 0 && incoming.moved.pad2 == 0 &&
      incoming.moved.disposition == MACH_MSG_TYPE_MOVE_SEND &&
      incoming.moved.type == MACH_MSG_PORT_DESCRIPTOR && move_observed &&
      incoming.moved.name == session && moved_type_status == KERN_SUCCESS &&
      (moved_type & MACH_PORT_TYPE_SEND) != 0;

  kern_return_t release_session_send = descriptor_valid
      ? deallocate_send(task, incoming.moved.name) : KERN_FAILURE;
  kern_return_t release_session_receive = release_session_send == KERN_SUCCESS
      ? release_receive(task, session) : KERN_FAILURE;
  kern_return_t release_service_send = insert_service == KERN_SUCCESS
      ? deallocate_send(task, service) : KERN_FAILURE;
  kern_return_t release_service_receive = release_service_send == KERN_SUCCESS
      ? release_receive(task, service) : KERN_FAILURE;

  printf("allocate=%d/%d insert=%d/%d send=%llu type_in_transit=%d/%u "
         "receive=%llu descriptor=%d name_match=%d cleanup=%d/%d/%d/%d\n",
      allocate_service, allocate_session, insert_service, insert_session,
      (unsigned long long)send, in_transit_type_status, move_observed,
      (unsigned long long)receive, descriptor_valid,
      incoming.moved.name == session, release_session_send,
      release_session_receive, release_service_send, release_service_receive);
  return !(allocate_service == KERN_SUCCESS && allocate_session == KERN_SUCCESS &&
      insert_service == KERN_SUCCESS && insert_session == KERN_SUCCESS &&
      send == MACH_MSG_SUCCESS && in_transit_type_status == KERN_SUCCESS &&
      move_observed && descriptor_valid &&
      release_session_send == KERN_SUCCESS && release_session_receive == KERN_SUCCESS &&
      release_service_send == KERN_SUCCESS && release_service_receive == KERN_SUCCESS);
}
