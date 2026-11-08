#include <mach/clock.h>
#include <mach/clock_types.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdio.h>

_Static_assert(sizeof(__Request__host_get_clock_service_t) == 36,
               "host_get_clock_service request ABI");
_Static_assert(sizeof(__Reply__host_get_clock_service_t) == 40,
               "host_get_clock_service success ABI");
_Static_assert(offsetof(__Reply__host_get_clock_service_t, clock_serv) == 28,
               "host_get_clock_service descriptor offset");

static int check_clock(host_t host, clock_id_t clock_id) {
  clock_serv_t service = MACH_PORT_NULL;
  const kern_return_t result = host_get_clock_service(host, clock_id, &service);
  if (result != KERN_SUCCESS || service == MACH_PORT_NULL) {
    printf("clock id %d acquisition failed: %d\n", clock_id, result);
    return 1;
  }

  mach_port_type_t type = 0;
  mach_port_urefs_t references = 0;
  mach_timespec_t current_time = {0, 0};
  const kern_return_t type_result = mach_port_type(mach_task_self(), service, &type);
  const kern_return_t refs_result = mach_port_get_refs(
      mach_task_self(), service, MACH_PORT_RIGHT_SEND, &references);
  const kern_return_t time_result = clock_get_time(service, &current_time);
  if (type_result != KERN_SUCCESS || (type & MACH_PORT_TYPE_SEND) == 0 ||
      refs_result != KERN_SUCCESS || references == 0 || time_result != KERN_SUCCESS ||
      current_time.tv_nsec < 0 || current_time.tv_nsec >= 1000000000) {
    printf("clock id %d service probe failed\n", clock_id);
    (void)mach_port_deallocate(mach_task_self(), service);
    return 1;
  }

  if (mach_port_deallocate(mach_task_self(), service) != KERN_SUCCESS) {
    printf("clock id %d right release failed\n", clock_id);
    return 1;
  }
  if (clock_id == CALENDAR_CLOCK &&
      mach_port_type(mach_task_self(), service, &type) != KERN_INVALID_NAME) {
    printf("calendar clock send right survived its last uref\n");
    return 1;
  }
  printf("clock id %d: service send right, clock_get_time, release passed\n", clock_id);
  return 0;
}

int main(void) {
  host_t host = mach_host_self();
  int failed = check_clock(host, SYSTEM_CLOCK) | check_clock(host, CALENDAR_CLOCK);

  clock_serv_t invalid = MACH_PORT_NULL;
  const kern_return_t invalid_result = host_get_clock_service(host, (clock_id_t)2, &invalid);
  if (invalid_result != KERN_INVALID_ARGUMENT || invalid != MACH_PORT_NULL) {
    printf("unsupported clock id result mismatch: %d\n", invalid_result);
    failed = 1;
  } else {
    puts("clock id 2: invalid argument, null right passed");
  }

  invalid = MACH_PORT_NULL;
  const kern_return_t null_host_result = host_get_clock_service(
      MACH_PORT_NULL, SYSTEM_CLOCK, &invalid);
  if (null_host_result == KERN_SUCCESS || invalid != MACH_PORT_NULL) {
    puts("null host unexpectedly returned a clock service");
    failed = 1;
  } else {
    puts("null host: rejected without returning a right");
  }

  (void)mach_port_deallocate(mach_task_self(), host);
  return failed;
}
