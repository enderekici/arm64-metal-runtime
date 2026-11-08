#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
typedef jint (JNICALL *default_args_fn)(void *);
typedef jint (JNICALL *created_vms_fn)(JavaVM **, jsize, jsize *);
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  void *image = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (!image) { puts(dlerror()); return 3; }
  default_args_fn default_args = (default_args_fn)dlsym(image, "JNI_GetDefaultJavaVMInitArgs");
  created_vms_fn created_vms = (created_vms_fn)dlsym(image, "JNI_GetCreatedJavaVMs");
  if (!default_args || !created_vms) return 4;
  JavaVMInitArgs args = {.version = JNI_VERSION_1_8};
  JavaVM *vms[1] = {NULL};
  jsize count = -1;
  jint args_status = default_args(&args);
  jint vms_status = created_vms(vms, 1, &count);
  printf("args=%d version=%d created=%d count=%d\n", args_status, args.version, vms_status, count);
  return args_status != JNI_OK || args.version != JNI_VERSION_1_8 ||
         vms_status != JNI_OK || count != 0;
}
