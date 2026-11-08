#include <bootstrap.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

static int lookup(const char *label, const char *service, int expected_status) {
  name_t name = {0};
  mach_port_t port = MACH_PORT_NULL;
  if (strlen(service) >= sizeof(name)) return 10;
  memcpy(name, service, strlen(service) + 1);
  const kern_return_t status = bootstrap_look_up(bootstrap_port, name, &port);
  kern_return_t deallocate_status = KERN_INVALID_NAME;
  if (status == BOOTSTRAP_SUCCESS && port != MACH_PORT_NULL) {
    deallocate_status = mach_port_deallocate(mach_task_self(), port);
  }
  printf("lookup=%s status=%d port_valid=%d deallocate_status=%d\n",
         label, status, port != MACH_PORT_NULL, deallocate_status);
  if (status != expected_status) return 11;
  if (expected_status == BOOTSTRAP_SUCCESS) {
    return port == MACH_PORT_NULL || deallocate_status != KERN_SUCCESS ? 12 : 0;
  }
  return port != MACH_PORT_NULL ? 13 : 0;
}

int main(void) {
  int result = lookup("windowserver-active", "com.apple.windowserver.active",
                      BOOTSTRAP_SUCCESS);
  if (result != 0) return result;
  result = lookup("windowserver-active-duplicate", "com.apple.windowserver.active",
                  BOOTSTRAP_SUCCESS);
  if (result != 0) return result;
  return lookup("missing", "com.example.arm64-metal.no-such-service",
                BOOTSTRAP_UNKNOWN_SERVICE);
}
