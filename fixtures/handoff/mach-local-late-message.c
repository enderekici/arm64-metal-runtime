#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
  mach_port_name_t port;
  atomic_int entered;
  uint64_t result;
} receive_context_t;

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

static void *timed_receiver(void *argument) {
  receive_context_t *context = (receive_context_t *)argument;
  struct {
    mach_msg_header_t header;
    mach_msg_max_trailer_t trailer;
  } message = {0};
  atomic_store_explicit(&context->entered, 1, memory_order_release);
  context->result = mach_msg2_raw(&message.header,
      MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 0, 0,
      (uint64_t)context->port << 32, sizeof(message), 10);
  return NULL;
}

static uint64_t send_message(mach_port_t port, int32_t id, uint32_t payload) {
  struct {
    mach_msg_header_t header;
    uint32_t payload;
  } message = {0};
  message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  message.header.msgh_size = sizeof(message);
  message.header.msgh_remote_port = port;
  message.header.msgh_id = id;
  message.payload = payload;
  const uint64_t options = MACH_SEND_MSG | (UINT64_C(8) << 32);
  return mach_msg2_raw(&message.header, options,
      ((uint64_t)sizeof(message) << 32) | message.header.msgh_bits,
      port, (uint64_t)(uint32_t)id << 32, 0, 0, 0);
}

static uint64_t receive_message(mach_port_t port, mach_msg_header_t *message,
                                uint32_t capacity) {
  return mach_msg2_raw(message, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 0, 0,
      (uint64_t)port << 32, capacity, 20);
}

static uint64_t monotonic_nanoseconds(void) {
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  mach_port_t port = MACH_PORT_NULL;
  const kern_return_t allocated = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port);
  const kern_return_t inserted = allocated == KERN_SUCCESS
      ? mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND) : KERN_FAILURE;
  receive_context_t context = {.port = port};
  pthread_t receiver;
  const int created = inserted == KERN_SUCCESS &&
      pthread_create(&receiver, NULL, timed_receiver, &context) == 0;
  if (created) {
    const uint64_t waitStart = monotonic_nanoseconds();
    while (atomic_load_explicit(&context.entered, memory_order_acquire) == 0 &&
           monotonic_nanoseconds() - waitStart < UINT64_C(1000000000)) {
    }
  }

  // Keep this sender runnable well beyond the receiver's 10 ms deadline.
  const uint64_t runStart = monotonic_nanoseconds();
  volatile uint64_t work = 0;
  while (monotonic_nanoseconds() - runStart < UINT64_C(100000000)) ++work;
  const uint64_t runnableNanoseconds = monotonic_nanoseconds() - runStart;
  // Send immediately after the busy interval, before joining or yielding.
  const uint64_t sendFirst = created && runnableNanoseconds >= UINT64_C(100000000)
      ? send_message(port, 0x4d534741, 0x13579bdf) : KERN_FAILURE;
  const uint64_t sendSecond = sendFirst == MACH_MSG_SUCCESS
      ? send_message(port, 0x4d534742, 0x2468ace0) : KERN_FAILURE;
  const int joined = created && pthread_join(receiver, NULL) == 0;
  const int timedOut = joined && context.result == MACH_RCV_TIMED_OUT;
  struct {
    mach_msg_header_t header;
    uint32_t payload;
    mach_msg_max_trailer_t trailer;
  } first = {0}, second = {0};
  const uint64_t receiveFirst = joined && sendSecond == MACH_MSG_SUCCESS
      ? receive_message(port, &first.header, sizeof(first)) : KERN_FAILURE;
  const uint64_t receiveSecond = receiveFirst == MACH_MSG_SUCCESS
      ? receive_message(port, &second.header, sizeof(second)) : KERN_FAILURE;
  const int lateReceive = receiveFirst == MACH_MSG_SUCCESS &&
      receiveSecond == MACH_MSG_SUCCESS;
  const int fifo = lateReceive && first.header.msgh_id == 0x4d534741 &&
      first.payload == 0x13579bdf && second.header.msgh_id == 0x4d534742 &&
      second.payload == 0x2468ace0;
  const int senderStayedRunnable = runnableNanoseconds >= UINT64_C(100000000) && work != 0;
  const kern_return_t deallocated = port != MACH_PORT_NULL
      ? mach_port_deallocate(task, port) : KERN_FAILURE;
  const kern_return_t destroyed = port != MACH_PORT_NULL
      ? mach_port_destruct(task, port, 0, MACH_PORT_NULL) : KERN_FAILURE;

  printf("receiver_entered=%d receiver_timed_out=%d sender_runnable_past_deadline=%d "
         "late_sends=%d late_receive=%d fifo=%d cleanup=%d\n",
      created && atomic_load_explicit(&context.entered, memory_order_acquire) != 0,
      timedOut, senderStayedRunnable,
      sendFirst == MACH_MSG_SUCCESS && sendSecond == MACH_MSG_SUCCESS,
      lateReceive, fifo, deallocated == KERN_SUCCESS && destroyed == KERN_SUCCESS);
  return !(task != MACH_PORT_NULL && allocated == KERN_SUCCESS &&
      inserted == KERN_SUCCESS && created && joined && timedOut &&
      senderStayedRunnable && sendFirst == MACH_MSG_SUCCESS &&
      sendSecond == MACH_MSG_SUCCESS && lateReceive && fifo &&
      deallocated == KERN_SUCCESS && destroyed == KERN_SUCCESS);
}
