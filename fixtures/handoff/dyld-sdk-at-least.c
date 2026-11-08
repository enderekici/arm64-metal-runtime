#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t platform;
    uint32_t version;
} GuestDyldBuildVersion;

extern const struct mach_header_64 _mh_execute_header;

typedef int (*DyldSdkAtLeast)(const struct mach_header *, GuestDyldBuildVersion);
extern bool dyld_sdk_at_least(const struct mach_header *, GuestDyldBuildVersion);
_Static_assert(sizeof(GuestDyldBuildVersion) == 8, "dyld tuple is one 64-bit register");
_Static_assert(offsetof(GuestDyldBuildVersion, platform) == 0, "platform is low 32 bits");
_Static_assert(offsetof(GuestDyldBuildVersion, version) == 4, "version is high 32 bits");

static int sdk(const struct mach_header *header, uint32_t platform, uint32_t version) {
    static DyldSdkAtLeast predicate;
    if (predicate == NULL) predicate = (DyldSdkAtLeast)dlsym(RTLD_DEFAULT, "dyld_sdk_at_least");
    return predicate != NULL && predicate(header, (GuestDyldBuildVersion){platform, version});
}

int main(void) {
    const struct mach_header *mainHeader = (const struct mach_header *)&_mh_execute_header;
    uint32_t mainPlatform = 0;
    uint32_t mainSdk = 0;
    if (mainHeader->magic == MH_MAGIC_64) {
        const struct mach_header_64 *header = (const struct mach_header_64 *)mainHeader;
        const uint8_t *cursor = (const uint8_t *)(header + 1);
        for (uint32_t i = 0; i < header->ncmds; ++i) {
            const struct load_command *command = (const struct load_command *)cursor;
            if (command->cmd == LC_BUILD_VERSION &&
                command->cmdsize >= sizeof(struct build_version_command)) {
                const struct build_version_command *build = (const struct build_version_command *)cursor;
                mainPlatform = build->platform;
                mainSdk = build->sdk == 0 ? build->minos : build->sdk;
                break;
            }
            cursor += command->cmdsize;
        }
    }
    if (mainPlatform == 0) return 2;
    const int zero = sdk(mainHeader, mainPlatform, 0);
    const int exact = sdk(mainHeader, mainPlatform, mainSdk);
    const int next = sdk(mainHeader, mainPlatform, mainSdk + 1);
    const int other = sdk(mainHeader, 0x7fffffffU, 0);
    const int directExact = dyld_sdk_at_least(mainHeader,
        (GuestDyldBuildVersion){mainPlatform, mainSdk});
    if (!zero || !exact || next || other || !directExact) {
        fprintf(stderr, "native tuple control failed: platform=%u sdk=%u results=%d,%d,%d,%d\n",
                mainPlatform, mainSdk, zero, exact, next, other);
        return 3;
    }

    struct {
        struct mach_header_64 header;
        struct build_version_command build;
    } unknown;
    memset(&unknown, 0, sizeof(unknown));
    unknown.header.magic = MH_MAGIC_64;
    unknown.header.filetype = MH_DYLIB;
    unknown.header.ncmds = 1;
    unknown.header.sizeofcmds = sizeof(unknown.build);
    unknown.build.cmd = LC_BUILD_VERSION;
    unknown.build.cmdsize = sizeof(unknown.build);
    unknown.build.platform = mainPlatform;
    unknown.build.minos = 0;
    unknown.build.sdk = mainSdk;
    if (!sdk((const struct mach_header *)&unknown.header, mainPlatform, mainSdk)) return 4;

    int sawSystemCacheImage = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *name = _dyld_get_image_name(i);
        if (name != NULL && strstr(name, "/usr/lib/system/libsystem_kernel.dylib") != NULL) {
            const struct mach_header *header = _dyld_get_image_header(i);
            sawSystemCacheImage = header != NULL &&
                (sdk(header, mainPlatform, 0) || sdk(header, 0x7fffffffU, 0));
            break;
        }
    }
    if (!sawSystemCacheImage) return 5;
    puts("dyld_sdk_at_least ABI and native tuple controls passed");
    return 0;
}
