#include <mach/clock.h>
#include <mach/clock_types.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/ndr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(__Request__host_get_clock_service_t) == 36,
               "host_get_clock_service request ABI");
_Static_assert(sizeof(__Reply__host_get_clock_service_t) == 40,
               "host_get_clock_service success ABI");
_Static_assert(offsetof(__Reply__host_get_clock_service_t, clock_serv) == 28,
               "host_get_clock_service descriptor offset");
_Static_assert(sizeof(__Reply__clock_get_time_t) == 44, "clock_get_time reply ABI");

static uint64_t raw_msg2(uint64_t address, uint64_t options, uint64_t bits_and_size,
                         uint64_t ports, uint64_t voucher_and_id,
                         uint64_t descriptors_and_receive, uint64_t size_and_priority,
                         uint64_t timeout) {
  register uint64_t x0 __asm__("x0") = address;
  register uint64_t x1 __asm__("x1") = options;
  register uint64_t x2 __asm__("x2") = bits_and_size;
  register uint64_t x3 __asm__("x3") = ports;
  register uint64_t x4 __asm__("x4") = voucher_and_id;
  register uint64_t x5 __asm__("x5") = descriptors_and_receive;
  register uint64_t x6 __asm__("x6") = size_and_priority;
  register uint64_t x7 __asm__("x7") = timeout;
  register uint64_t x16 __asm__("x16") = (uint64_t)-47;
  __asm__ volatile("svc #0x80"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6),
                     "r"(x7), "r"(x16)
                   : "memory", "cc");
  return x0;
}

static uint64_t raw_trap2(int64_t number, uint64_t arg0, uint64_t arg1) {
  register uint64_t x0 __asm__("x0") = arg0;
  register uint64_t x1 __asm__("x1") = arg1;
  register uint64_t x16 __asm__("x16") = (uint64_t)number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory", "cc");
  return x0;
}

static uint64_t raw_trap0(int64_t number) {
  register uint64_t x0 __asm__("x0") = 0;
  register uint64_t x16 __asm__("x16") = (uint64_t)number;
  __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory", "cc");
  return x0;
}

static const NDR_record_t guest_ndr = {0, 0, 0, 0, NDR_INT_LITTLE_ENDIAN,
                                        NDR_CHAR_ASCII, NDR_FLOAT_IEEE, 0};

static uint64_t send_request(mach_msg_header_t *message, uint32_t send_size,
                             uint32_t remote, mach_port_t reply_port, uint32_t message_id,
                             uint32_t receive_size) {
  message->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                      MACH_MSG_TYPE_MAKE_SEND_ONCE);
  message->msgh_size = send_size;
  message->msgh_remote_port = remote;
  message->msgh_local_port = reply_port;
  message->msgh_voucher_port = MACH_PORT_NULL;
  message->msgh_id = (mach_msg_id_t)message_id;
  return raw_msg2((uint64_t)(uintptr_t)message, UINT64_C(0x200000003),
                  ((uint64_t)send_size << 32) | message->msgh_bits,
                  ((uint64_t)reply_port << 32) | remote,
                  (uint64_t)message_id << 32, (uint64_t)reply_port << 32,
                  receive_size, 0);
}

int main(void) {
  const mach_port_t task = (mach_port_t)raw_trap0(-28);
  const mach_port_t host = (mach_port_t)raw_trap0(-29);
  const mach_port_t reply_port = (mach_port_t)raw_trap0(-26);
  if (task == MACH_PORT_NULL || host == MACH_PORT_NULL || reply_port == MACH_PORT_NULL) {
    puts("self/reply port setup failed");
    return 1;
  }

  union {
    __Request__host_get_clock_service_t request;
    __Reply__host_get_clock_service_t host_reply;
    __Reply__clock_get_time_t time_reply;
    unsigned char bytes[64];
  } wire = {0};
  wire.request.NDR = guest_ndr;
  wire.request.clock_id = CALENDAR_CLOCK;
  uint64_t send_result = send_request(&wire.request.Head, sizeof(wire.request), host,
                                      reply_port, 206, sizeof(wire));
  __Reply__host_get_clock_service_t response = {0};
  memcpy(&response, &wire, sizeof(response));
  const mach_port_t service = response.clock_serv.name;
  const int host_reply_shape = send_result == KERN_SUCCESS &&
      response.Head.msgh_bits == (MACH_MSGH_BITS_COMPLEX | 0x1200) &&
      response.Head.msgh_size == sizeof(response) && response.Head.msgh_id == 306 &&
      response.Head.msgh_remote_port == MACH_PORT_NULL &&
      response.Head.msgh_local_port == reply_port && response.msgh_body.msgh_descriptor_count == 1 &&
      service != MACH_PORT_NULL && response.clock_serv.disposition == MACH_MSG_TYPE_MOVE_SEND &&
      response.clock_serv.type == MACH_MSG_PORT_DESCRIPTOR;

  memset(&wire, 0, sizeof(wire));
  send_result = send_request(&wire.request.Head, sizeof(mach_msg_header_t), service,
                             reply_port, 1000, sizeof(wire));
  __Reply__clock_get_time_t clock_response = {0};
  memcpy(&clock_response, &wire, sizeof(clock_response));
  const int time_reply_shape = send_result == KERN_SUCCESS &&
      clock_response.Head.msgh_bits == 0x1200 && clock_response.Head.msgh_size == 44 &&
      clock_response.Head.msgh_id == 1100 && clock_response.RetCode == KERN_SUCCESS &&
      clock_response.cur_time.tv_nsec >= 0 && clock_response.cur_time.tv_nsec < 1000000000;
  const uint64_t release_service = raw_trap2(-18, task, service);
  memset(&wire, 0, sizeof(wire));
  const uint64_t after_release = send_request(&wire.request.Head,
                                              sizeof(mach_msg_header_t), service,
                                              reply_port, 1000, sizeof(wire));

  memset(&wire, 0, sizeof(wire));
  wire.request.NDR = guest_ndr;
  wire.request.clock_id = 2;
  send_result = send_request(&wire.request.Head, sizeof(wire.request), host, reply_port,
                             206, sizeof(wire));
  struct {
    mach_msg_header_t header;
    NDR_record_t ndr;
    kern_return_t return_code;
  } error_reply = {0};
  memcpy(&error_reply, &wire, sizeof(error_reply));
  const int invalid_id_shape = send_result == KERN_SUCCESS && error_reply.header.msgh_id == 306 &&
      error_reply.header.msgh_size == sizeof(error_reply) &&
      (error_reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0 &&
      error_reply.return_code == KERN_INVALID_ARGUMENT;

  (void)raw_trap2(-18, task, host);
  (void)raw_trap2(-25, task, reply_port);
  (void)raw_trap2(-18, task, task);

  printf("host_reply=%d clock_get_time=%d release=%llu invalid_after_release=%llu "
         "invalid_id=%d\n", host_reply_shape, time_reply_shape,
         (unsigned long long)release_service, (unsigned long long)after_release,
         invalid_id_shape);
  return !host_reply_shape || !time_reply_shape || release_service != KERN_SUCCESS ||
      after_release != MACH_SEND_INVALID_DEST || !invalid_id_shape;
}
