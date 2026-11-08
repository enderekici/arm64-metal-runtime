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
  int32_t message_id;
  uint32_t payload;
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

static void *blocked_receiver(void *argument) {
  receive_context_t *context = (receive_context_t *)argument;
  struct {
    mach_msg_header_t header;
    uint32_t payload;
    mach_msg_max_trailer_t trailer;
  } message = {0};
  atomic_store_explicit(&context->entered, 1, memory_order_release);
  context->result = mach_msg2_raw(&message.header, MACH_RCV_MSG, 0, 0, 0,
      (uint64_t)context->port << 32, sizeof(message), 0);
  context->message_id = message.header.msgh_id;
  context->payload = message.payload;
  return NULL;
}

static void wait_until_entered(receive_context_t *context) {
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (atomic_load_explicit(&context->entered, memory_order_acquire) != 0) {
      nanosleep(&delay, NULL);
      return;
    }
    nanosleep(&delay, NULL);
  }
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

static mach_port_t make_port(mach_port_t task, int with_send) {
  mach_port_t port = MACH_PORT_NULL;
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) {
    return MACH_PORT_NULL;
  }
  if (with_send && mach_port_insert_right(task, port, port,
          MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
    (void)mach_port_destruct(task, port, 0, MACH_PORT_NULL);
    return MACH_PORT_NULL;
  }
  return port;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_port_t send_port = make_port(task, 1);
  receive_context_t send_context = {.port = send_port};
  pthread_t send_thread;
  const int send_thread_created = send_port != MACH_PORT_NULL &&
      pthread_create(&send_thread, NULL, blocked_receiver, &send_context) == 0;
  if (send_thread_created) wait_until_entered(&send_context);
  const uint64_t send_result = send_thread_created
      ? send_message(send_port, 0x4d534741, 0x13579bdf) : KERN_FAILURE;
  if (send_thread_created) (void)pthread_join(send_thread, NULL);

  const mach_port_t timeout_port = make_port(task, 0);
  struct { mach_msg_header_t header; mach_msg_max_trailer_t trailer; } empty = {0};
  const uint64_t timeout_result = timeout_port == MACH_PORT_NULL ? KERN_FAILURE :
      mach_msg2_raw(&empty.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 0, 0,
          (uint64_t)timeout_port << 32, sizeof(empty), 5);

  const mach_port_t destroy_port = make_port(task, 0);
  receive_context_t destroy_context = {.port = destroy_port};
  pthread_t destroy_thread;
  const int destroy_thread_created = destroy_port != MACH_PORT_NULL &&
      pthread_create(&destroy_thread, NULL, blocked_receiver, &destroy_context) == 0;
  if (destroy_thread_created) wait_until_entered(&destroy_context);
  const kern_return_t destroy_result = destroy_thread_created
      ? mach_port_destruct(task, destroy_port, 0, MACH_PORT_NULL) : KERN_FAILURE;
  if (destroy_thread_created) (void)pthread_join(destroy_thread, NULL);

  const int send_woke = send_thread_created && send_result == MACH_MSG_SUCCESS &&
      send_context.result == MACH_MSG_SUCCESS && send_context.message_id == 0x4d534741 &&
      send_context.payload == 0x13579bdf;
  printf("send_woke=%d timeout=%llu destroy_result=%d destroy_wake=%llu\n",
      send_woke, (unsigned long long)timeout_result, destroy_result,
      (unsigned long long)destroy_context.result);

  if (send_port != MACH_PORT_NULL) {
    (void)mach_port_deallocate(task, send_port);
    (void)mach_port_destruct(task, send_port, 0, MACH_PORT_NULL);
  }
  if (timeout_port != MACH_PORT_NULL) {
    (void)mach_port_destruct(task, timeout_port, 0, MACH_PORT_NULL);
  }
  return !(send_woke && timeout_result == MACH_RCV_TIMED_OUT &&
      destroy_result == KERN_SUCCESS &&
      destroy_context.result == MACH_RCV_PORT_CHANGED);
}
