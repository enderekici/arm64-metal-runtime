#include <dlfcn.h>

int dlsym_arm64e_pac_function(void) { return 42; }

int dlsym_arm64e_pac_data = 7;

int main(void) {
  void *first = dlsym(RTLD_DEFAULT, "dlsym_arm64e_pac_function");
  void *second = dlsym(RTLD_DEFAULT, "dlsym_arm64e_pac_function");
  int (*function)(void) = (int (*)(void))first;
  int *data = (int *)dlsym(RTLD_DEFAULT, "dlsym_arm64e_pac_data");
  return first == second && function != 0 && data != 0 && function() == 42 && *data == 7
      ? 42 : 1;
}
