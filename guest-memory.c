#include <stdbool.h>
#include <crt_externs.h>
#include <stdlib.h>
#include <stdint.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/random.h>
#include <errno.h>
#include <sys/mman.h>

extern int __commpage_gettimeofday(struct timeval *time);

extern size_t __strlcpy_chk(char *destination, const char *source, size_t size, size_t destination_size);

extern intptr_t _dyld_get_image_slide(const struct mach_header *header);

struct build_version { uint32_t platform, version; };
extern bool dyld_program_sdk_at_least(struct build_version version);
extern bool dyld_program_minos_at_least(struct build_version version);

int main(int argc, char **argv) {
    if (!dyld_program_sdk_at_least((struct build_version){1, 0x000b0000}) ||
        dyld_program_sdk_at_least((struct build_version){1, 0x00ff0000}) ||
        dyld_program_sdk_at_least((struct build_version){2, 0x000b0000}) ||
        !dyld_program_sdk_at_least((struct build_version){UINT32_MAX, 0x07e40901}) ||
        !dyld_program_minos_at_least((struct build_version){1, 0x000b0000}) ||
        dyld_program_minos_at_least((struct build_version){1, 0x00ff0000})) return 40;
    uint64_t published = UINT64_C(0x1020304050607080);
    uint64_t observed;
    __asm__ volatile(".arch_extension rcpc\n\tldapr %0, [%1]"
                     : "=r"(observed) : "r"(&published) : "memory");
    if (observed != published) return 41;
    uint64_t replacement = UINT64_C(0x000001008010ff00);
    __asm__ volatile(".arch_extension lse\n\tswpl %2, %0, [%1]"
                     : "=&r"(observed) : "r"(&published), "r"(replacement) : "memory");
    if (observed != UINT64_C(0x1020304050607080) || published != replacement) return 42;
    volatile const uint64_t *generation = (const uint64_t *)UINT64_C(0xfffffc180);
    uint64_t before = *generation;
    if (sched_yield() || *generation < before) return 43;
    const volatile uint64_t *capabilities = (const uint64_t *)UINT64_C(0xfffffc010);
    uint64_t features = *capabilities;
    if (!(features & UINT64_C(0x00ff0000)) ||
        (features & UINT64_C(0x04000080)) != UINT64_C(0x04000080)) return 54;
    volatile uint64_t ordered = features;
    __asm__ volatile("dsb sy\n\tisb\n\tdmb sy\n\tdmb ish" ::: "memory");
    if (ordered != features) return 55;
    uint64_t memory_size = 0;
    size_t memory_length = sizeof(memory_size);
    const volatile uint64_t *memory_field = (const uint64_t *)UINT64_C(0xfffffc038);
    if (sysctlbyname("hw.memsize", &memory_size, &memory_length, NULL, 0) ||
        memory_length != sizeof(memory_size) || !memory_size || *memory_field != memory_size) return 52;
    struct timeval boot;
    size_t boot_length = sizeof(boot);
    const volatile uint64_t *boot_field = (const uint64_t *)UINT64_C(0xfffffc0a0);
    if (sysctlbyname("kern.boottime", &boot, &boot_length, NULL, 0) ||
        boot_length != sizeof(boot) || *boot_field != (uint64_t)boot.tv_sec * 1000000 + boot.tv_usec) return 56;
    struct timeval first_wall, fast_wall, last_wall;
    if (gettimeofday(&first_wall, NULL) || __commpage_gettimeofday(&fast_wall) ||
        gettimeofday(&last_wall, NULL) || fast_wall.tv_usec < 0 || fast_wall.tv_usec >= 1000000 ||
        timercmp(&fast_wall, &first_wall, <) || timercmp(&last_wall, &fast_wall, <)) return 57;
    volatile unsigned char entropy[258];
    memset((void *)entropy, 0xa5, sizeof(entropy));
    if (getentropy((void *)(entropy + 1), 257) != -1 || errno != EINVAL) return 58;
    for (size_t i = 0; i < sizeof(entropy); ++i) if (entropy[i] != 0xa5) return 59;
    if (getentropy(NULL, 0) || getentropy(NULL, 1) != -1 || errno != EFAULT ||
        getentropy((void *)(entropy + 1), 256) || entropy[0] != 0xa5 || entropy[257] != 0xa5) return 60;
    size_t changed = 0;
    for (size_t i = 1; i <= 256; ++i) changed += entropy[i] != 0xa5;
    if (!changed) return 61;
    uint64_t zero_count, sign_count;
    uint32_t narrow_count;
    uint64_t zero = 0, negative = UINT64_MAX;
    uint32_t narrow = 0x00008000;
    __asm__ volatile("clz %0, %1" : "=r"(zero_count) : "r"(zero));
    __asm__ volatile("cls %0, %1" : "=r"(sign_count) : "r"(negative));
    __asm__ volatile("clz %w0, %w1" : "=r"(narrow_count) : "r"(narrow));
    if (zero_count != 64 || sign_count != 63 || narrow_count != 16) return 62;
    const char *expected = getenv("ARM64_METAL_ENV_TEST");
    char **environment = *_NSGetEnviron();
    if (!environment) return 44;
    if (argc == 2 && !strcmp(argv[1], "environment")) {
        int found = 0;
        for (char **entry = environment; *entry; ++entry) {
            if (!strcmp(*entry, "ARM64_METAL_ENV_TEST=guest-vector")) found = 1;
        }
        if (!found || !expected || strcmp(expected, "guest-vector")) return 45;
    }
    mach_vm_address_t address = 0;
    mach_port_t task = mach_task_self();
    size_t page = vm_page_size;
    if (!task || mach_vm_allocate(task, &address, 2 * page + 17, VM_FLAGS_ANYWHERE)) return 46;
    volatile unsigned char *mapped = (volatile unsigned char *)(uintptr_t)address;
    if (address % vm_page_size || mapped[0] || mapped[16]) return 47;
    mapped[16] = 71;
    mapped[page] = 72;
    if (mprotect((void *)(mapped + page), page, PROT_READ) || mapped[page] != 72 ||
        mprotect((void *)(mapped + page), page, PROT_NONE) || mapped[16] != 71 || mapped[2 * page] != 0 ||
        mprotect((void *)(mapped + page), page, PROT_READ | PROT_WRITE)) return 53;
    mapped[page]++;
    if (mapped[page] != 73 || mach_vm_deallocate(task, address, 2 * page + 17)) return 48;
    address = 0;
    if (mach_vm_map(task, &address, 17, 0, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, 0,
                    VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_DEFAULT)) return 49;
    mapped = (volatile unsigned char *)(uintptr_t)address;
    if (mapped[16] || mach_vm_deallocate(task, address, 17)) return 50;
    const struct mach_header_64 *header = &_mh_execute_header;
    const struct segment_command_64 *first = (const void *)(header + 1);
    const struct segment_command_64 *text = first + 1;
    if (first->cmd != LC_SEGMENT_64 || first->cmdsize != sizeof(*first) ||
        text->cmd != LC_SEGMENT_64 || strcmp(text->segname, "__TEXT") ||
        text->vmaddr + _dyld_get_image_slide((const struct mach_header *)header) != (uintptr_t)header) return 51;
    char bytes[16];
    memset(bytes, 'x', sizeof(bytes));
    if (memcpy(bytes, "0123456789", 10) != bytes) return 1;
    memmove(bytes + 3, bytes, 9);
    memmove(bytes, bytes + 1, 10);
    bzero(bytes + 12, 3);
    if (write(STDOUT_FILENO, bytes, sizeof(bytes)) != sizeof(bytes)) return 2;
    char pattern[19];
    memset_pattern4(pattern, "1234", sizeof(pattern));
    if (write(STDOUT_FILENO, pattern, sizeof(pattern)) != sizeof(pattern)) return 3;
    memset_pattern8(pattern, "abcdefgh", sizeof(pattern));
    if (write(STDOUT_FILENO, pattern, sizeof(pattern)) != sizeof(pattern)) return 4;
    memset_pattern16(pattern, "0123456789abcdef", sizeof(pattern));
    if (write(STDOUT_FILENO, pattern, sizeof(pattern)) != sizeof(pattern)) return 5;
    char strings[16];
    strncpy(strings, "metal", sizeof(strings));
    if (write(STDOUT_FILENO, strings, sizeof(strings)) != sizeof(strings)) return 6;
    if (strlcpy(strings, "bounded copy", sizeof(strings)) != 12) return 7;
    if (write(STDOUT_FILENO, strings, 13) != 13) return 8;
    if (__strlcpy_chk(strings, "fortified", sizeof(strings), sizeof(strings)) != 9) return 9;
    if (write(STDOUT_FILENO, strings, 10) != 10) return 10;
    if (strcpy(strings, "copy") != strings) return 11;
    if (write(STDOUT_FILENO, strings, 5) != 5) return 12;
    if (snprintf(strings, 5, "%s:%u", "guest", 42u) != 8) return 13;
    if (snprintf(NULL, 0, "%s:%u", "guest", 42u) != 8) return 14;
    return write(STDOUT_FILENO, strings, 5) == 5 ? 0 : 15;
}
