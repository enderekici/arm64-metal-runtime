#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>

typedef jint (JNICALL *create_vm_fn)(JavaVM **, void **, void *);

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  void *image = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (!image) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 3;
  }
  create_vm_fn create_vm = (create_vm_fn)dlsym(image, "JNI_CreateJavaVM");
  if (!create_vm) {
    fprintf(stderr, "dlsym: %s\n", dlerror());
    return 4;
  }

  char class_path[4096];
  if (snprintf(class_path, sizeof(class_path), "-Djava.class.path=%s", argv[2]) >=
      (int)sizeof(class_path)) return 5;
  JavaVMOption option = {.optionString = class_path, .extraInfo = NULL};
  JavaVMInitArgs args = {
      .version = JNI_VERSION_1_8,
      .nOptions = 1,
      .options = &option,
      .ignoreUnrecognized = JNI_FALSE,
  };
  JavaVM *vm = NULL;
  JNIEnv *env = NULL;
  jint create_status = create_vm(&vm, (void **)&env, &args);
  if (create_status != JNI_OK || vm == NULL || env == NULL) {
    printf("create=%d vm=%d env=%d\n", create_status, vm != NULL, env != NULL);
    return 6;
  }

  jclass klass = (*env)->FindClass(env, "JniSmoke");
  if (klass == NULL) {
    (*env)->ExceptionDescribe(env);
    (*vm)->DestroyJavaVM(vm);
    return 7;
  }
  jmethodID method = (*env)->GetStaticMethodID(env, klass, "run", "()I");
  if (method == NULL) {
    (*env)->ExceptionDescribe(env);
    (*vm)->DestroyJavaVM(vm);
    return 8;
  }
  jint value = (*env)->CallStaticIntMethod(env, klass, method);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*vm)->DestroyJavaVM(vm);
    return 9;
  }
  jint destroy_status = (*vm)->DestroyJavaVM(vm);
  printf("create=%d result=%d destroy=%d\n", create_status, value, destroy_status);
  dlclose(image);
  return value == 42 && destroy_status == JNI_OK ? 0 : 10;
}
