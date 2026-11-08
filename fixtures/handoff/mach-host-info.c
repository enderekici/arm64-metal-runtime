#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdio.h>

int main(void) {
  host_priority_info_data_t priority = {0};
  mach_msg_type_number_t priority_count = HOST_PRIORITY_INFO_COUNT;
  const kern_return_t priority_result = host_info(
      mach_host_self(), HOST_PRIORITY_INFO, (host_info_t)&priority, &priority_count);

  host_priority_info_data_t invalid_host_info = {0};
  mach_msg_type_number_t invalid_host_count = HOST_PRIORITY_INFO_COUNT;
  const kern_return_t invalid_host_result = host_info(
      MACH_PORT_NULL, HOST_PRIORITY_INFO, (host_info_t)&invalid_host_info,
      &invalid_host_count);

  host_priority_info_data_t invalid_flavor_info = {0};
  mach_msg_type_number_t invalid_flavor_count = HOST_PRIORITY_INFO_COUNT;
  const kern_return_t invalid_flavor_result = host_info(
      mach_host_self(), (host_flavor_t)0x7fffffff,
      (host_info_t)&invalid_flavor_info, &invalid_flavor_count);

  host_priority_info_data_t short_info = {0};
  mach_msg_type_number_t short_count = 0;
  const kern_return_t short_result = host_info(
      mach_host_self(), HOST_PRIORITY_INFO, (host_info_t)&short_info, &short_count);

  const mach_port_t released_host = mach_host_self();
  const kern_return_t release_results[] = {
      mach_port_deallocate(mach_task_self(), released_host),
      mach_port_deallocate(mach_task_self(), released_host),
      mach_port_deallocate(mach_task_self(), released_host),
      mach_port_deallocate(mach_task_self(), released_host),
  };
  host_priority_info_data_t released_info = {0};
  mach_msg_type_number_t released_count = HOST_PRIORITY_INFO_COUNT;
  const kern_return_t released_result = host_info(
      released_host, HOST_PRIORITY_INFO, (host_info_t)&released_info, &released_count);

  printf("priority=%d,%d,%d,%d,%d,%d,%d,%d count=%u result=%d\n",
         priority.kernel_priority, priority.system_priority,
         priority.server_priority, priority.user_priority,
         priority.depress_priority, priority.idle_priority,
         priority.minimum_priority, priority.maximum_priority,
         priority_count, priority_result);
  printf("invalid-host=%d invalid-flavor=%d flavor-count=%u short-count=%d count=%u\n",
         invalid_host_result, invalid_flavor_result, invalid_flavor_count,
         short_result, short_count);
  printf("released-host=%d release-results=%d,%d,%d,%d\n", released_result,
         release_results[0], release_results[1], release_results[2], release_results[3]);

  return priority_result != KERN_SUCCESS ||
         priority_count != HOST_PRIORITY_INFO_COUNT ||
         invalid_host_result != MACH_SEND_INVALID_DEST ||
         invalid_flavor_result != KERN_INVALID_ARGUMENT ||
         invalid_flavor_count != HOST_PRIORITY_INFO_COUNT ||
         short_result != KERN_FAILURE || short_count != 0 ||
         released_result != MACH_SEND_INVALID_DEST || release_results[0] != KERN_SUCCESS ||
         release_results[1] != KERN_SUCCESS || release_results[2] != KERN_SUCCESS ||
         release_results[3] != KERN_SUCCESS;
}
