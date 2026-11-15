// Runs libpthread's own TSD destructor pass in the guest before a pthread
// created by the host service finishes (pthread_exit or start routine return).
{
    std::optional<std::int64_t> tsdCleanupEntry;
    bool tsdCleanupResolved = false;
    std::set<std::uint64_t> tsdCleanedThreads;
    beginThreadTsdCleanup = [&, tsdCleanupEntry, tsdCleanupResolved,
                             tsdCleanedThreads](std::uint64_t result) mutable {
        const auto threadId = guestScheduler.current_id();
        if (threadId == 1 || guestWorkqueueThreadIds.count(threadId) != 0) return false;
        if (!tsdCleanupResolved) {
            tsdCleanupResolved = true;
            if (image.cache_reader) {
                tsdCleanupEntry = cache_symtab_symbol(
                    *image.cache_reader, "/usr/lib/system/libsystem_pthread.dylib",
                    "__pthread_tsd_cleanup");
            }
        }
        if (!tsdCleanupEntry) return false;
        if (!tsdCleanedThreads.insert(threadId).second) return false;
        const auto tsd = guestScheduler.current().tsdAddress;
        if (tsd <= 0) return false;
        // Host-created threads keep their TSD array at the pthread_t + 0xe0 layout.
        constexpr std::uint64_t tsdOffsetInPthread = 0xe0;
        const auto self = static_cast<std::uint64_t>(tsd) - tsdOffsetInPthread;
        auto* live = static_cast<std::uint64_t*>(stateBuffer.data());
        GuestCallbackFrame frame;
        frame.savedRegisters.assign(live, live + 36);
        frame.savedRegisters[34] = 0;
        frame.savedVectorAndAuth.assign(live + 806, live + guestTokenCount);
        frame.authenticationDepth = live[37];
        frame.exitThreadResult = result;
        callbackFramesByThread[threadId].push_back(std::move(frame));
        live[0] = self;
        live[30] = guestReturnSentinel;
        live[33] = static_cast<std::uint64_t>(*tsdCleanupEntry);
        live[34] = 0;
        selectKernel(*tsdCleanupEntry);
        return true;
    };
}
