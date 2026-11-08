#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

extern const struct mach_header_64 _mh_execute_header;
struct GuestDyldBuildVersion {
    uint32_t platform;
    uint32_t version;
};
extern bool dyld_sdk_at_least(const struct mach_header *, struct GuestDyldBuildVersion);

int main(void) {
    const struct mach_header *mainHeader = (const struct mach_header *)&_mh_execute_header;
    const struct mach_header_64 *header = &_mh_execute_header;
    uint32_t platform = 0;
    uint32_t sdk = 0;
    const uint8_t *cursor = (const uint8_t *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmd == LC_BUILD_VERSION &&
            command->cmdsize >= sizeof(struct build_version_command)) {
            const struct build_version_command *build =
                (const struct build_version_command *)cursor;
            platform = build->platform;
            sdk = build->sdk == 0 ? build->minos : build->sdk;
            break;
        }
        cursor += command->cmdsize;
    }
    if (platform == 0 || sdk == 0) return 2;
    const struct GuestDyldBuildVersion below = {platform, sdk - 1};
    const struct GuestDyldBuildVersion exact = {platform, sdk};
    const struct GuestDyldBuildVersion above = {platform, sdk + 1};
    const struct GuestDyldBuildVersion otherPlatform = {0x7fffffffU, 0};
    if (!dyld_sdk_at_least(mainHeader, below) || !dyld_sdk_at_least(mainHeader, exact) ||
        dyld_sdk_at_least(mainHeader, above) ||
        dyld_sdk_at_least(mainHeader, otherPlatform)) return 3;
    puts("direct dyld_sdk_at_least tuple controls passed");
    return 0;
}
