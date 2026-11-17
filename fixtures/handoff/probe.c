#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct notification {
  char image[128];
  uintptr_t header;
  intptr_t slide;
  int header_matches_dladdr;
  int slide_matches_text;
  uint64_t text_vmaddr;
};

static struct notification notifications[128];
static unsigned notification_count;

static void added(const struct mach_header *header, intptr_t slide) {
  if (notification_count >= sizeof(notifications) / sizeof(notifications[0])) return;

  struct notification *event = &notifications[notification_count++];
  Dl_info callback_info;
  memset(&callback_info, 0, sizeof(callback_info));
  if (dladdr((const void *)header, &callback_info) && callback_info.dli_fname != NULL) {
    const char *callback_leaf = strrchr(callback_info.dli_fname, '/');
    callback_leaf = callback_leaf == NULL ? callback_info.dli_fname : callback_leaf + 1;
    if (strcmp(callback_leaf, "libdyld-add-image-new.dylib") == 0)
      (void)write(1, "A_R\n", 4);
    else if (strcmp(callback_leaf, "libdyld-add-image-dep.dylib") == 0)
      (void)write(1, "A_D\n", 4);
  }
  memset(event, 0, sizeof(*event));
  event->header = (uintptr_t)header;
  event->slide = slide;

  Dl_info info;
  memset(&info, 0, sizeof(info));
  if (dladdr((const void *)header, &info) && info.dli_fname != NULL) {
    const char *leaf = strrchr(info.dli_fname, '/');
    leaf = leaf == NULL ? info.dli_fname : leaf + 1;
    snprintf(event->image, sizeof(event->image), "%s", leaf);
    event->header_matches_dladdr = (uintptr_t)info.dli_fbase == (uintptr_t)header;
  }

  if (header == NULL || header->magic != MH_MAGIC_64) return;
  const struct mach_header_64 *header64 = (const struct mach_header_64 *)header;
  const uint8_t *cursor = (const uint8_t *)(header64 + 1);
  for (uint32_t i = 0; i < header64->ncmds; ++i) {
    const struct load_command *command = (const struct load_command *)cursor;
    if (command->cmd == LC_SEGMENT_64) {
      const struct segment_command_64 *segment =
          (const struct segment_command_64 *)command;
      if (strcmp(segment->segname, "__TEXT") == 0) {
        event->text_vmaddr = segment->vmaddr;
        event->slide_matches_text =
            (__int128)(uintptr_t)header - (__int128)segment->vmaddr ==
            (__int128)slide;
        break;
      }
    }
    cursor += command->cmdsize;
  }
}

int main(int argc, char **argv) {
  const int stable_compare = argc == 4 && strcmp(argv[1], "--stable-compare") == 0;
  const int argument = stable_compare ? 2 : 1;
  if ((!stable_compare && argc != 3) || (stable_compare && argc != 4)) return 64;
  _dyld_register_func_for_add_image(added);
  const unsigned initial_callbacks = notification_count;
  const unsigned current_images = _dyld_image_count();
  if (initial_callbacks != current_images) {
    fprintf(stderr, "registration callbacks %u != image count %u\n",
            initial_callbacks, current_images);
    return 1;
  }

  void *bad = dlopen(argv[argument + 1], RTLD_NOW);
  const unsigned failed_callbacks = notification_count - initial_callbacks;
  if (bad != NULL) dlclose(bad);
  if (bad != NULL || failed_callbacks != 0) {
    fprintf(stderr, "failed dlopen changed image notifications\n");
    return 2;
  }

  void *good = dlopen(argv[argument], RTLD_NOW | RTLD_LOCAL);
  const unsigned successful_callbacks = notification_count - initial_callbacks;
  if (good == NULL) {
    fprintf(stderr, "dlerror=%s\n", dlerror());
    return 3;
  }

  int (*get_value)(void) = dlsym(good, "dyld_add_image_value");
  const unsigned index = initial_callbacks;
  const int order_matches =
      successful_callbacks == 2 &&
      strcmp(notifications[index].image, "libdyld-add-image-new.dylib") == 0 &&
      strcmp(notifications[index + 1].image, "libdyld-add-image-dep.dylib") == 0;
  int callbacks_valid = order_matches;
  unsigned initial_header_slide_matches = 0;
  for (unsigned i = 0; i < initial_callbacks; ++i) {
    const int matches = notifications[i].header != 0 &&
        notifications[i].header_matches_dladdr &&
        notifications[i].slide_matches_text;
    initial_header_slide_matches += matches;
    if (!matches && i < 4) {
      fprintf(stderr, "initial[%u]=%s header=%d slide=%d\n", i,
              notifications[i].image, notifications[i].header_matches_dladdr,
              notifications[i].slide_matches_text);
      fprintf(stderr, "  header=0x%llx slide=%lld text_vmaddr=0x%llx\n",
              (unsigned long long)notifications[i].header,
              (long long)notifications[i].slide,
              (unsigned long long)notifications[i].text_vmaddr);
    }
  }
  callbacks_valid &= initial_header_slide_matches == initial_callbacks;
  for (unsigned i = index; i < index + successful_callbacks; ++i) {
    callbacks_valid &= notifications[i].header != 0;
    callbacks_valid &= notifications[i].header_matches_dladdr;
    callbacks_valid &= notifications[i].slide_matches_text;
  }

  if (stable_compare) {
    printf("initial=validated failed=%u added=%u order=%d headers_slides=all value=%d\n",
           failed_callbacks, successful_callbacks, order_matches,
           get_value == NULL ? -1 : get_value());
  } else {
    printf("initial=%u failed=%u added=%u order=%d headers_slides=%d/%u value=%d\n",
           initial_callbacks, failed_callbacks, successful_callbacks,
           order_matches, initial_header_slide_matches, initial_callbacks,
           get_value == NULL ? -1 : get_value());
  }
  dlclose(good);
  return !callbacks_valid || get_value == NULL;
}
