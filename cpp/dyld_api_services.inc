// dyld4::APIs methods served through the extra façade slots of
// dyld_api_slots.hpp. Included inside the host-service dispatch of
// process_runner.cpp; x0 is the published APIs object, x1 the first argument.
            if (service >= 260 && service <= 263) {
                if (dyldApis.object == 0 || hostRegisters[0] != dyldApis.object) {
                    throw std::runtime_error(
                        "Dyld API slot method did not use the published API object");
                }
                auto& slotState = dyld_api_slot_state();
                if (service == 260) {
                    // bool _dyld_get_shared_cache_uuid(uuid_t): the guest runs the
                    // host's cache file, so its UUID is the cache's own.
                    const auto* cache = image.metadata_cache_reader();
                    if (cache == nullptr || cache->uuid().size() != 16) {
                        registers[0] = 0;
                    } else {
                        std::memcpy(guestMemory.buffer(hostRegisters[1], 16, true),
                                    cache->uuid().data(), 16);
                        registers[0] = 1;
                    }
                } else if (service == 261) {
                    // void _dyld_register_func_for_remove_image(func): dyld calls
                    // it only when an image is unloaded, never at registration.
                    slotState.register_remove_image(hostRegisters[1]);
                } else {
                    // dyld strips pointer authentication from the queried address;
                    // a signed arm64e function pointer is a guest token here.
                    auto address = hostRegisters[1];
                    if ((address & 0xffff000000000000ULL) == 0xa64e000000000000ULL) {
                        const auto stripped = decode_guest_ia0_callback(
                            hostRegisters.data(), hostRegisters.size(), address);
                        if (stripped) address = *stripped;
                    }
                    const auto found = slotState.image_containing(
                        address, image.mapped_image_segments,
                        image.metadata_cache_reader());
                    if (!found) {
                        registers[0] = 0;
                    } else if (service == 262) {
                        registers[0] = found->header;
                    } else {
                        registers[0] = stableGuestString(found->path).value_or(0);
                    }
                }
                continue;
            }
