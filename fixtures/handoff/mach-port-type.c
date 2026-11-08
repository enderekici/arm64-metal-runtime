#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <servers/bootstrap.h>
#include <stdint.h>
#include <stdio.h>

#define TYPE_SEND ((mach_port_type_t)0x00010000U)
#define TYPE_RECEIVE ((mach_port_type_t)0x00020000U)
#define TYPE_SEND_RECEIVE (TYPE_SEND | TYPE_RECEIVE)
#define TYPE_DEAD_NAME ((mach_port_type_t)0x00100000U)
#define TYPE_DNREQUEST ((mach_port_type_t)0x80000000U)
#define TYPE_SPREQUEST ((mach_port_type_t)0x40000000U)
#define TYPE_SPREQUEST_DELAYED ((mach_port_type_t)0x20000000U)
#define TYPE_KERNEL_FLAGS (TYPE_DNREQUEST | TYPE_SPREQUEST | TYPE_SPREQUEST_DELAYED)
#define TYPE_SENTINEL ((mach_port_type_t)0xdecafbadU)

static kern_return_t raw_type(mach_port_t task, mach_port_name_t name,
                              mach_port_type_t *type) {
  return _kernelrpc_mach_port_type_trap(task, name, type);
}

static int type_is(mach_port_t task, mach_port_name_t name,
                   mach_port_type_t expected) {
  mach_port_type_t type = TYPE_SENTINEL;
  return raw_type(task, name, &type) == KERN_SUCCESS && type == expected;
}

static kern_return_t status_for(mach_port_t task, mach_port_name_t name,
                                mach_port_type_t *type) {
  *type = TYPE_SENTINEL;
  return raw_type(task, name, type);
}

static int failure_untouched(mach_port_t task, mach_port_name_t name,
                             kern_return_t expected) {
  mach_port_type_t type = TYPE_SENTINEL;
  return status_for(task, name, &type) == expected && type == TYPE_SENTINEL;
}

int main(void) {
  const mach_port_t task = mach_task_self();
  const mach_port_t host = mach_host_self();
  const mach_port_t thread = mach_thread_self();
  const int task_send = type_is(task, task, TYPE_SEND);
  const int host_send = type_is(task, host, TYPE_SEND);
  const int thread_send = type_is(task, thread, TYPE_SEND);

  mach_port_t receive = MACH_PORT_NULL;
  kern_return_t allocate = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &receive);
  const int receive_only = allocate == KERN_SUCCESS && type_is(task, receive, TYPE_RECEIVE);
  kern_return_t insert = mach_port_insert_right(task, receive, receive,
                                                 MACH_MSG_TYPE_MAKE_SEND);
  const int send_receive = insert == KERN_SUCCESS &&
                           type_is(task, receive, TYPE_SEND_RECEIVE);
  kern_return_t drop_send = mach_port_deallocate(task, receive);
  const int send_removed = drop_send == KERN_SUCCESS &&
                           type_is(task, receive, TYPE_RECEIVE);
  kern_return_t destroy_receive = mach_port_destruct(task, receive, 0, MACH_PORT_NULL);
  const int destroyed_name = destroy_receive == KERN_SUCCESS &&
                             failure_untouched(task, receive, KERN_INVALID_NAME);

  mach_port_t dead = MACH_PORT_NULL;
  kern_return_t allocate_dead = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &dead);
  kern_return_t insert_dead = allocate_dead == KERN_SUCCESS
      ? mach_port_insert_right(task, dead, dead, MACH_MSG_TYPE_MAKE_SEND)
      : KERN_FAILURE;
  kern_return_t destroy_dead_receive = insert_dead == KERN_SUCCESS
      ? mach_port_destruct(task, dead, 0, MACH_PORT_NULL)
      : KERN_FAILURE;
  const int dead_name = destroy_dead_receive == KERN_SUCCESS &&
                        type_is(task, dead, TYPE_DEAD_NAME);
  kern_return_t release_dead = mach_port_deallocate(task, dead);
  const int dead_released = release_dead == KERN_SUCCESS &&
                            failure_untouched(task, dead, KERN_INVALID_NAME);

  mach_port_type_t dead_sentinel_type = TYPE_SENTINEL;
  kern_return_t dead_sentinel_result = raw_type(task, MACH_PORT_DEAD,
                                                 &dead_sentinel_type);
  mach_port_type_t null_type = TYPE_SENTINEL;
  kern_return_t null_result = status_for(task, MACH_PORT_NULL, &null_type);
  mach_port_type_t unknown_type = TYPE_SENTINEL;
  kern_return_t unknown_result = status_for(task, 0xdeadbeefU, &unknown_type);
  mach_port_type_t invalid_task_type = TYPE_SENTINEL;
  kern_return_t invalid_task_result = status_for(MACH_PORT_NULL, task, &invalid_task_type);
  mach_port_type_t host_task_type = TYPE_SENTINEL;
  kern_return_t host_task_result = status_for(host, task, &host_task_type);
  const int dead_sentinel = dead_sentinel_result == KERN_SUCCESS &&
                            dead_sentinel_type == TYPE_DEAD_NAME;
  const int null_name = failure_untouched(task, MACH_PORT_NULL, KERN_INVALID_NAME);
  const int unknown_name = failure_untouched(task, 0xdeadbeefU, KERN_INVALID_NAME);
  const int invalid_task = failure_untouched(MACH_PORT_NULL, task,
                                              MACH_SEND_INVALID_DEST);
  const int host_not_task = failure_untouched(host, task, MACH_SEND_INVALID_DEST);
  kern_return_t null_name_null_output = raw_type(task, MACH_PORT_NULL, NULL);
  kern_return_t unknown_name_null_output = raw_type(task, 0xdeadbeefU, NULL);
  mach_port_type_t bad_task_null_name_type = TYPE_SENTINEL;
  kern_return_t bad_task_null_name = raw_type(MACH_PORT_NULL, MACH_PORT_NULL,
                                               &bad_task_null_name_type);
  const int bad_task_precedes_name = bad_task_null_name == MACH_SEND_INVALID_DEST &&
                                     bad_task_null_name_type == TYPE_SENTINEL;
  kern_return_t invalid_output = raw_type(task, task, NULL);

  mach_port_t clock = MACH_PORT_NULL;
  kern_return_t clock_result = host_get_clock_service(host, SYSTEM_CLOCK, &clock);
  const int clock_send = clock_result == KERN_SUCCESS && type_is(task, clock, TYPE_SEND);

  mach_port_t bootstrap_service = MACH_PORT_NULL;
  kern_return_t bootstrap_result = bootstrap_look_up(
      bootstrap_port, "com.apple.windowserver.active", &bootstrap_service);
  const int bootstrap_send = bootstrap_result == BOOTSTRAP_SUCCESS &&
                             type_is(task, bootstrap_service, TYPE_SEND);
  if (clock != MACH_PORT_NULL) (void)mach_port_deallocate(task, clock);
  kern_return_t release_bootstrap = bootstrap_service != MACH_PORT_NULL
      ? mach_port_deallocate(task, bootstrap_service) : KERN_FAILURE;
  const int bootstrap_released_invalid = release_bootstrap == KERN_SUCCESS &&
      failure_untouched(task, bootstrap_service, KERN_INVALID_NAME);
  (void)mach_port_deallocate(task, thread);
  (void)mach_port_deallocate(task, host);
  (void)mach_port_deallocate(task, task);

  printf("special_ports task=0x%08x/%d host=0x%08x/%d thread=0x%08x/%d\n",
         TYPE_SEND, task_send, TYPE_SEND, host_send, TYPE_SEND, thread_send);
  printf("local_rights receive=0x%08x/%d send_receive=0x%08x/%d "
         "after_send_release=0x%08x/%d destroyed_name=%d\n",
         TYPE_RECEIVE, receive_only, TYPE_SEND_RECEIVE, send_receive,
         TYPE_RECEIVE, send_removed, destroyed_name);
  printf("dead_names right=0x%08x/%d sentinel=0x%08x/%d released=%d\n",
         TYPE_DEAD_NAME, dead_name, TYPE_DEAD_NAME, dead_sentinel, dead_released);
  printf("errors null=%d/%d unknown=%d/%d invalid_task=0x%08x/%d "
         "host_as_task=0x%08x/%d null_name_null_output=%d "
         "unknown_null_output=%d bad_task_precedence=%d invalid_output=%d\n",
         null_result, null_type == TYPE_SENTINEL, unknown_result,
         unknown_type == TYPE_SENTINEL, MACH_SEND_INVALID_DEST,
         invalid_task_result == MACH_SEND_INVALID_DEST &&
             invalid_task_type == TYPE_SENTINEL,
         MACH_SEND_INVALID_DEST,
         host_task_result == MACH_SEND_INVALID_DEST && host_task_type == TYPE_SENTINEL,
         null_name_null_output, unknown_name_null_output, bad_task_precedes_name,
         invalid_output);
  printf("services clock=0x%08x/%d bootstrap=0x%08x/%d "
         "bootstrap_released_invalid=%d kernel_flags_absent=%d\n",
         TYPE_SEND, clock_send, TYPE_SEND, bootstrap_send, bootstrap_released_invalid,
         (TYPE_SEND & TYPE_KERNEL_FLAGS) == 0 &&
         (TYPE_RECEIVE & TYPE_KERNEL_FLAGS) == 0 &&
         (TYPE_SEND_RECEIVE & TYPE_KERNEL_FLAGS) == 0 &&
         (TYPE_DEAD_NAME & TYPE_KERNEL_FLAGS) == 0);
  return task_send && host_send && thread_send && receive_only && send_receive &&
         send_removed && destroyed_name && dead_name && dead_sentinel && dead_released &&
         null_name && unknown_name && invalid_task && host_not_task &&
         null_name_null_output == KERN_INVALID_NAME &&
         unknown_name_null_output == KERN_INVALID_NAME && bad_task_precedes_name &&
         invalid_output == KERN_MEMORY_ERROR && clock_send && bootstrap_send &&
         bootstrap_released_invalid ? 0 : 1;
}
