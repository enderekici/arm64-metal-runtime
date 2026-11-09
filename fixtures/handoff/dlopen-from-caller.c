#include <dlfcn.h>

extern void *dlopen_from(const char *path, int mode, void *addressInCaller);

static __attribute__((noinline)) void caller_marker(void) {}

int open_rpath_marker(void) {
    void *handle = dlopen_from("@rpath/librpath-marker-target.dylib",
                               RTLD_NOW | RTLD_LOCAL, (void *)&caller_marker);
    if (handle == 0) return -1;
    int (*value)(void) = 0;
    void *symbol = dlsym(handle, "dlopen_from_marker_value");
    __builtin_memcpy(&value, &symbol, sizeof(value));
    if (value == 0) {
        dlclose(handle);
        return -2;
    }
    const int result = value();
    dlclose(handle);
    return result;
}
