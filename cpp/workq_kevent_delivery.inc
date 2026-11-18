// Process workqueue kqueue (kevent_qos on kq -1 with KEVENT_FLAG_WORKQ). The
// registrations live on a private host kqueue; ready events are delivered to a
// guest workqueue thread started with WQ_FLAG_THREAD_KEVENT, as XNU does.
{
    constexpr std::uint64_t keventSize = netKeventQosStride;
    constexpr int maximumEvents = 16;
    constexpr std::uint64_t flagNewSpi = 0x00040000, flagPrioQos = 0x00004000,
        flagKevent = 0x00080000, flagManager = 0x00100000, flagTsdBaseSet = 0x00200000;
    constexpr std::uint32_t managerPriorityFlag = 0x02000000;
    workqHostKqueue = [&]() {
        if (guestWorkqHostKq < 0) guestWorkqHostKq = ::kqueue();
        return guestWorkqHostKq;
    };
    workqApplyChanges = [&](std::uint64_t address, std::uint64_t count) {
        if (guestWorkqHostKq < 0 || count == 0 || count > maximumEvents) return;
        std::vector<std::uint8_t> changes(static_cast<std::size_t>(count * keventSize));
        try {
            std::memcpy(changes.data(), guestMemory.buffer(address, changes.size()),
                        changes.size());
        } catch (const std::out_of_range&) {
            return;
        }
        std::vector<std::uint8_t> ignored(changes.size());
        const auto applied = netHostKevent(2, guestWorkqHostKq, changes,
                                           static_cast<int>(count), ignored, 0, 0x2U);
        if (std::getenv("ARM64_METAL_TRACE_WORKQ")) {
            std::cerr << "workq-return-changes count=" << count << " result=" << applied
                      << " errno=" << errno;
            for (std::uint64_t i = 0; i < count; ++i) {
                std::int16_t filter; std::uint16_t flags; std::uint64_t ident;
                std::memcpy(&ident, changes.data() + i * keventSize, 8);
                std::memcpy(&filter, changes.data() + i * keventSize + 8, 2);
                std::memcpy(&flags, changes.data() + i * keventSize + 10, 2);
                std::cerr << " [id=" << ident << " f=" << filter << " fl=0x" << std::hex
                          << flags << std::dec << "]";
            }
            std::cerr << '\n';
        }
    };
    spawnKeventWorker = [&](const std::vector<std::uint8_t>& events, int count,
                            std::uint64_t workerFlags, std::uint64_t* workerIdOut) {
        if (!guestBsdthreadRegistration || !guestWorkqueueDispatchConfig) return false;
        const auto* state = static_cast<const std::uint64_t*>(stateBuffer.data());
        const auto workerEntry = decode_guest_ia0_callback(
            state, stateBuffer.size() / sizeof(std::uint64_t),
            guestBsdthreadRegistration->workqueueStart);
        if (!workerEntry || *workerEntry > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
            !image.is_code(static_cast<std::int64_t>(*workerEntry))) return false;
        constexpr std::uint64_t minimumPthreadSize = 0x18e0;
        constexpr std::uint64_t pthreadStackOffset = 12U * 1024U;
        constexpr std::uint64_t stackSize = 512U * 1024U;
        constexpr std::uint64_t errnoOffset = 172, threadIdOffset = 216, tsdOffset = 224;
        const auto pageSize = bootstrap.pageSize;
        const auto pthreadSize = guestBsdthreadRegistration->pthreadSize;
        if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0 ||
            pthreadSize < minimumPthreadSize) return false;
        const auto pthreadTail = (pthreadSize + pthreadStackOffset + pageSize - 1) &
            ~(pageSize - 1);
        const auto baseSize = pageSize + stackSize + pthreadTail;
        const auto allocationSize = baseSize + pageSize;
        const auto allocation = allocateGuestHeap(allocationSize, pageSize);
        if (!allocation) return false;
        if (nextGuestThreadPortName >= std::numeric_limits<std::uint32_t>::max() - 4) {
            hostServiceState.heap->release(*allocation);
            return false;
        }
        const auto allocationAddress = static_cast<std::uint64_t>(*allocation);
        const auto stackBottom = allocationAddress + pageSize;
        const auto self = stackBottom + stackSize + pthreadStackOffset;
        const auto listAddress = allocationAddress + baseSize;
        const auto threadId = guestScheduler.next_thread_id();
        const auto threadPort = nextGuestThreadPortName;
        nextGuestThreadPortName += 4;
        std::uint32_t qos = 0;
        std::memcpy(&qos, events.data() + 12, sizeof(qos));
        const bool manager = (qos & managerPriorityFlag) != 0;
        std::uint64_t qosClass = 0;
        if (!manager) {
            const auto encoded = (qos & 0x00003f00U) >> 8;
            qosClass = encoded == 0 || (encoded & (encoded - 1)) != 0 ? 4U
                : static_cast<unsigned>(__builtin_ffs(static_cast<int>(encoded)));
        }
        try {
            auto* bytes = guestMemory.buffer(allocationAddress, allocationSize, true);
            std::memset(bytes, 0, static_cast<std::size_t>(allocationSize));
            std::memcpy(bytes + baseSize, events.data(),
                        static_cast<std::size_t>(count) * keventSize);
            std::uint64_t idValue = threadId;
            std::memcpy(bytes + (self - allocationAddress) + threadIdOffset, &idValue,
                        sizeof(idValue));
            std::uint64_t portValue = threadPort;
            std::memcpy(bytes + (self - allocationAddress) + tsdOffset +
                        guestMachThreadSlotOffset, &portValue, sizeof(portValue));
            const auto* live = static_cast<const std::uint64_t*>(stateBuffer.data());
            auto thread = GuestScheduler::make_thread_frame(
                {static_cast<std::int64_t>(stackBottom), static_cast<std::int64_t>(self)},
                *allocation, static_cast<std::int64_t>(self + errnoOffset),
                static_cast<std::int64_t>(self + tsdOffset), *workerEntry, self,
                live[guestSchedulerFPCR], live[guestSchedulerFPSR],
                hostServiceState.threadSignalMask);
            thread.id = threadId;
            thread.detached = true;
            thread.registers[1] = threadPort;
            thread.registers[2] = stackBottom;
            thread.registers[3] = listAddress;
            thread.registers[4] = workerFlags != 0 ? workerFlags
                : flagNewSpi | flagKevent | flagTsdBaseSet |
                    (manager ? flagManager : (flagPrioQos | qosClass));
            thread.registers[5] = static_cast<std::uint64_t>(count);
            guestWorkqueueThreadIds.insert(threadId);
            guestThreadPortNames.emplace(threadId, threadPort);
            guestSendRightUrefs.emplace(threadPort, 1);
            liveThreadStacks.emplace(threadId, GuestMemoryRange{stackBottom, self});
            if (guestScheduler.add_thread(std::move(thread)) != threadId) {
                throw std::logic_error("Unexpected guest workqueue thread id");
            }
            if (workerIdOut != nullptr) *workerIdOut = threadId;
            if (workerFlags == 0) guestWorkqKeventWorker = threadId;
            return true;
        } catch (...) {
            guestWorkqueueThreadIds.erase(threadId);
            guestThreadPortNames.erase(threadId);
            guestSendRightUrefs.erase(threadPort);
            liveThreadStacks.erase(threadId);
            hostServiceState.heap->release(*allocation);
            throw;
        }
    };
    const auto pollWorkqKevents = [&]() {
        if (guestWorkqHostKq < 0 || !guestWorkqueueDispatchConfig) return;
        nextWorkqPollNanoseconds = monotonicNanoseconds() + 1'000'000ULL;
        if (guestWorkqKeventWorker != 0) {
            if (guestScheduler.find(guestWorkqKeventWorker) != nullptr) return;
            guestWorkqKeventWorker = 0;
        }
        std::vector<std::uint8_t> events(maximumEvents * keventSize);
        const std::vector<std::uint8_t> none;
        const auto ready = netHostKevent(2, guestWorkqHostKq, none, 0, events, maximumEvents,
                                         0);
        for (int i = 0; i < ready; ++i) {
            auto* event = events.data() + static_cast<std::size_t>(i) * keventSize;
            std::uint64_t ident;
            std::int16_t filter;
            std::uint32_t qos;
            std::memcpy(&ident, event, 8);
            std::memcpy(&filter, event + 8, 2);
            std::memcpy(&qos, event + 12, 4);
            const auto remembered = guestWorkqKeventQos.find({filter, ident});
            if (qos == 0 && remembered != guestWorkqKeventQos.end()) {
                std::memcpy(event + 12, &remembered->second, 4);
            }
        }
        if (ready > 0) {
            const bool spawned = spawnKeventWorker(events, ready, 0, nullptr);
            if (std::getenv("ARM64_METAL_TRACE_WORKQ"))
                std::cerr << "workq-kevent-deliver events=" << ready << " spawned=" << spawned;
            if (std::getenv("ARM64_METAL_TRACE_WORKQ")) {
                for (int i = 0; i < ready; ++i) {
                    std::int16_t filter; std::uint16_t flags; std::uint64_t ident;
                    std::memcpy(&ident, events.data() + i * keventSize, 8);
                    std::memcpy(&filter, events.data() + i * keventSize + 8, 2);
                    std::memcpy(&flags, events.data() + i * keventSize + 10, 2);
                    std::cerr << " [id=" << ident << " f=" << filter << " fl=0x" << std::hex
                              << flags << std::dec << " qos=0x" << std::hex
                              << *reinterpret_cast<const std::uint32_t*>(
                                     events.data() + i * keventSize + 12)
                              << std::dec << "]";
                }
                std::cerr << '\n';
            }
        }
    };
    const auto previousFireDueMachTimers = fireDueMachTimers;
    fireDueMachTimers = [previousFireDueMachTimers, pollWorkqKevents]() {
        if (previousFireDueMachTimers) previousFireDueMachTimers();
        pollWorkqKevents();
    };
    const auto previousEarliestMachTimer = earliestMachTimerNanoseconds;
    earliestMachTimerNanoseconds = [&, previousEarliestMachTimer]()
        -> std::optional<std::uint64_t> {
        auto deadline = previousEarliestMachTimer ? previousEarliestMachTimer()
                                                  : std::nullopt;
        if (guestWorkqHostKq >= 0 && guestWorkqueueDispatchConfig) {
            if (!deadline || nextWorkqPollNanoseconds < *deadline)
                deadline = nextWorkqPollNanoseconds;
        }
        return deadline;
    };
}
