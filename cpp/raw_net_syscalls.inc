{
    const auto netSyscall = static_cast<std::int64_t>(syscall);
    const auto netFinish = [&](const GuestHostPollResult& result) {
        if (result.carry) rawFail(static_cast<int>(result.x0));
        else rawOk(result.x0);
    };
    // Runs `attempt` now; without a result, parks the calling thread and lets the
    // scheduler re-run it until it produces one.
    const auto netTryOrPark = [&](GuestHostPollFn attempt) {
        const auto first = attempt(monotonicNanoseconds());
        if (first) {
            netFinish(*first);
            return;
        }
        auto& thread = guestScheduler.current();
        if (thread.wait.has_value()) {
            rawFail(EALREADY);
            return;
        }
        thread.wait = GuestHostPollWait{monotonicNanoseconds() + 500'000ULL};
        guestHostPollers[guestScheduler.current_id()] = std::move(attempt);
        registers[0] = 0;
        registers[32] &= ~std::uint64_t{8};
        registers[34] = 0;
        const auto* state = static_cast<const std::uint64_t*>(stateBuffer.data());
        auto live = std::vector<std::uint64_t>(state,
            state + stateBuffer.size() / sizeof(std::uint64_t));
        if (!syncThreadSwitch(false, live)) {
            throw std::runtime_error("Guest host-poll wait has no runnable thread");
        }
    };
    const auto netDeadline = [&](std::int64_t seconds, std::int64_t nanoseconds)
        -> std::uint64_t {
        constexpr std::uint64_t cap = std::numeric_limits<std::uint64_t>::max() / 2;
        const auto total = seconds > 9'000'000'000LL ? cap
            : static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
                static_cast<std::uint64_t>(nanoseconds);
        return monotonicNanoseconds() + std::min(total, cap);
    };
    const auto netReadGuest = [&](std::uint64_t address, void* destination,
                                  std::size_t size) {
        try {
            if (size != 0) std::memcpy(destination, guestMemory.buffer(address, size), size);
            return true;
        } catch (const std::out_of_range&) {
            return false;
        }
    };
    const auto netWriteGuest = [&](std::uint64_t address, const void* source,
                                   std::size_t size) {
        try {
            if (size != 0) std::memcpy(guestMemory.buffer(address, size, true), source, size);
            return true;
        } catch (const std::out_of_range&) {
            return false;
        }
    };

    if (netSyscall == 230 || netSyscall == 417) {  // poll, poll_nocancel
        const auto count = static_cast<std::uint32_t>(registers[1]);
        const auto timeoutMs = static_cast<std::int32_t>(registers[2]);
        if (count > 10240U) {
            rawFail(EINVAL);
            continue;
        }
        std::vector<pollfd> entries(count);
        if (!netReadGuest(registers[0], entries.data(), count * sizeof(pollfd))) {
            rawFail(EFAULT);
            continue;
        }
        const bool immediate = timeoutMs == 0;
        std::optional<std::uint64_t> deadline;
        if (timeoutMs > 0) deadline = netDeadline(timeoutMs / 1000,
            static_cast<std::int64_t>(timeoutMs % 1000) * 1'000'000);
        const auto address = registers[0];
        netTryOrPark([&guestMemory, &netPollDescriptors, entries, deadline, immediate,
                      address](std::uint64_t now) mutable -> std::optional<GuestHostPollResult> {
            const auto ready = netPollDescriptors(entries);
            if (ready == 0 && !immediate && !(deadline && now >= *deadline)) return std::nullopt;
            try {
                if (!entries.empty()) {
                    std::memcpy(guestMemory.buffer(address, entries.size() * sizeof(pollfd), true),
                                entries.data(), entries.size() * sizeof(pollfd));
                }
            } catch (const std::out_of_range&) {
                return GuestHostPollResult{EFAULT, true};
            }
            return GuestHostPollResult{static_cast<std::uint64_t>(ready), false};
        });
        continue;
    }

    if (netSyscall == 93 || netSyscall == 407 || netSyscall == 394 || netSyscall == 395) {
        // select, select_nocancel, pselect, pselect_nocancel
        const bool precise = netSyscall == 394 || netSyscall == 395;
        const auto nfds = static_cast<std::int32_t>(registers[0]);
        const std::uint64_t setAddress[3] = {registers[1], registers[2], registers[3]};
        if (nfds < 0 || nfds > 1024 || (precise && registers[5] != 0)) {
            rawFail(nfds < 0 || nfds > 1024 ? EINVAL : ENOTSUP);
            continue;
        }
        const auto setBytes = static_cast<std::size_t>((nfds + 31) / 32) * 4U;
        std::vector<std::uint8_t> sets[3];
        bool failed = false;
        for (int which = 0; which < 3 && !failed; ++which) {
            sets[which].assign(setBytes, 0);
            if (setAddress[which] != 0 &&
                !netReadGuest(setAddress[which], sets[which].data(), setBytes)) {
                rawFail(EFAULT);
                failed = true;
            }
        }
        if (failed) continue;
        bool immediate = false;
        std::optional<std::uint64_t> deadline;
        if (registers[4] != 0) {
            std::int64_t seconds = 0;
            std::int64_t fraction = 0;
            std::int64_t scale = precise ? 1 : 1000;
            std::int64_t limit = precise ? 1'000'000'000 : 1'000'000;
            std::int64_t pair[2] = {0, 0};
            if (!precise) {
                std::int64_t secondsValue = 0;
                std::int32_t microsValue = 0;
                if (!netReadGuest(registers[4], &secondsValue, sizeof(secondsValue)) ||
                    !netReadGuest(registers[4] + 8, &microsValue, sizeof(microsValue))) {
                    rawFail(EFAULT);
                    continue;
                }
                pair[0] = secondsValue;
                pair[1] = microsValue;
            } else if (!netReadGuest(registers[4], pair, sizeof(pair))) {
                rawFail(EFAULT);
                continue;
            }
            seconds = pair[0];
            fraction = pair[1];
            if (seconds < 0 || fraction < 0 || fraction >= limit) {
                rawFail(EINVAL);
                continue;
            }
            immediate = seconds == 0 && fraction == 0;
            if (!immediate) deadline = netDeadline(seconds, fraction * scale);
        }
        std::vector<pollfd> entries;
        bool invalid = false;
        std::int32_t selectTableSize = 25;
        for (const auto open : hostServiceState.openDescriptors) selectTableSize = std::max(selectTableSize, open + 1);
        for (const auto& open : hostServiceState.guestDirectoryDescriptors) selectTableSize = std::max(selectTableSize, open.first + 1);
        for (std::int32_t descriptor = 0; descriptor < nfds; ++descriptor) {
            short events = 0;
            const auto byte = static_cast<std::size_t>(descriptor) / 8U;
            const auto bit = static_cast<unsigned>(descriptor) % 8U;
            if (!sets[0].empty() && setAddress[0] != 0 && ((sets[0][byte] >> bit) & 1U))
                events |= POLLIN;
            if (!sets[1].empty() && setAddress[1] != 0 && ((sets[1][byte] >> bit) & 1U))
                events |= POLLOUT;
            if (!sets[2].empty() && setAddress[2] != 0 && ((sets[2][byte] >> bit) & 1U))
                events |= POLLPRI;
            if (events == 0) continue;
            if (descriptor >= selectTableSize) continue;
            if (!netFdUsable(descriptor)) invalid = true;
            entries.push_back(pollfd{descriptor, events, 0});
        }
        if (invalid) {
            rawFail(EBADF);
            continue;
        }
        netTryOrPark([&guestMemory, &netPollDescriptors, entries, deadline, immediate, setBytes,
                      setAddress](std::uint64_t now) mutable
                         -> std::optional<GuestHostPollResult> {
            const auto ready = netPollDescriptors(entries);
            if (ready == 0 && !immediate && !(deadline && now >= *deadline)) return std::nullopt;
            std::vector<std::uint8_t> out[3];
            for (auto& set : out) set.assign(setBytes, 0);
            std::uint64_t total = 0;
            for (const auto& entry : entries) {
                const auto byte = static_cast<std::size_t>(entry.fd) / 8U;
                const auto mask = static_cast<std::uint8_t>(1U << (entry.fd % 8));
                if ((entry.events & POLLIN) && (entry.revents & (POLLIN | POLLHUP | POLLERR))) {
                    out[0][byte] |= mask;
                    ++total;
                }
                if ((entry.events & POLLOUT) && (entry.revents & (POLLOUT | POLLERR))) {
                    out[1][byte] |= mask;
                    ++total;
                }
                if ((entry.events & POLLPRI) && (entry.revents & POLLPRI)) {
                    out[2][byte] |= mask;
                    ++total;
                }
            }
            try {
                for (int which = 0; which < 3; ++which) {
                    if (setAddress[which] != 0 && setBytes != 0) {
                        std::memcpy(guestMemory.buffer(setAddress[which], setBytes, true),
                                    out[which].data(), setBytes);
                    }
                }
            } catch (const std::out_of_range&) {
                return GuestHostPollResult{EFAULT, true};
            }
            return GuestHostPollResult{total, false};
        });
        continue;
    }

    if (netSyscall == 362) {  // kqueue
        const auto descriptor = ::kqueue();
        if (descriptor < 0) {
            rawFail(errno);
        } else {
            hostServiceState.openDescriptors.insert(descriptor);
            rawOk(static_cast<std::uint64_t>(descriptor));
        }
        continue;
    }

    if (netSyscall == 363 || netSyscall == 369 || netSyscall == 374) {
        // kevent, kevent64, kevent_qos
        constexpr std::uint32_t keventImmediate = 0x1U;
        constexpr std::uint32_t keventStackData = 0x8U;
        constexpr std::uint32_t keventWorkq = 0x20U;
        constexpr std::uint32_t keventWorkqManager = 0x200U;
        constexpr std::uint32_t keventWorkloop = 0x400U;
        const int kind = netSyscall == 363 ? 0 : netSyscall == 369 ? 1 : 2;
        const std::size_t stride = kind == 0 ? 32U : kind == 1 ? 48U
                                                               : netKeventQosStride;
        auto kq = static_cast<std::int32_t>(registers[0]);
        const auto changesAddress = registers[1];
        const auto changeCount = std::max<std::int32_t>(static_cast<std::int32_t>(registers[2]), 0);
        const auto eventsAddress = registers[3];
        const auto eventCount = static_cast<std::int32_t>(registers[4]);
        std::uint32_t flags = 0;
        std::uint64_t timeoutAddress = 0;
        if (kind == 1) {
            flags = static_cast<std::uint32_t>(registers[5]);
            timeoutAddress = registers[6];
        } else if (kind == 2) {
            flags = static_cast<std::uint32_t>(registers[7]);
        } else {
            timeoutAddress = registers[5];
        }
        if (eventCount < 0 || changeCount > (1 << 16) || eventCount > (1 << 16)) {
            rawFail(EINVAL);
            continue;
        }
        const bool workqKevent = (flags & (keventWorkq | keventWorkqManager)) != 0;
        if ((flags & keventWorkloop) != 0 ||
            (workqKevent && (kq != -1 || kind != 2))) {
            // Workloops are not modeled; only the process workq kqueue (kq -1) is.
            rawFail(kq == -1 ? ENOTSUP : EINVAL);
            continue;
        }
        if (workqKevent) kq = workqHostKqueue();
        if (!workqKevent && !netFdUsable(kq)) {
            rawFail(EBADF);
            continue;
        }
        bool immediate = ((flags & keventImmediate) != 0 && kind != 0) || workqKevent;
        std::optional<std::uint64_t> deadline;
        if (timeoutAddress != 0) {
            std::int64_t pair[2] = {0, 0};
            if (!netReadGuest(timeoutAddress, pair, sizeof(pair))) {
                rawFail(EFAULT);
                continue;
            }
            if (pair[0] < 0 || pair[1] < 0 || pair[1] >= 1'000'000'000) {
                rawFail(EINVAL);
                continue;
            }
            if (pair[0] == 0 && pair[1] == 0) immediate = true;
            else deadline = netDeadline(pair[0], pair[1]);
        }
        std::vector<std::uint8_t> changes(static_cast<std::size_t>(changeCount) * stride);
        if (!netReadGuest(changesAddress, changes.data(), changes.size())) {
            rawFail(EFAULT);
            continue;
        }
        int keptChanges = 0;
        for (int index = 0; index < changeCount; ++index) {
            std::int16_t filter = 0;
            std::memcpy(&filter, changes.data() + static_cast<std::size_t>(index) * stride + 8,
                        sizeof(filter));
            if (filter == EVFILT_MACHPORT) continue;
            if (keptChanges != index) {
                std::memmove(changes.data() + static_cast<std::size_t>(keptChanges) * stride,
                             changes.data() + static_cast<std::size_t>(index) * stride, stride);
            }
            ++keptChanges;
        }
        const auto hostFlags = flags & ~(keventStackData | keventWorkq | keventWorkqManager);
        std::vector<std::uint8_t> events(static_cast<std::size_t>(eventCount) * stride);
        const auto delivered = netHostKevent(kind, kq, changes, keptChanges, events,
                                             eventCount, hostFlags);
        if (workqKevent) {
            // The host kqueue is not a workq kqueue and may not echo the
            // registered priority, so remember it for delivery.
            for (int i = 0; i < keptChanges; ++i) {
                const auto* c = changes.data() + static_cast<std::size_t>(i) * stride;
                std::int16_t f; std::uint16_t fl; std::uint32_t q; std::uint64_t id;
                std::memcpy(&id, c, 8); std::memcpy(&f, c + 8, 2); std::memcpy(&fl, c + 10, 2);
                std::memcpy(&q, c + 12, 4);
                if ((fl & EV_ADD) != 0 && q != 0) guestWorkqKeventQos[{f, id}] = q;
            }
        }
        if (workqKevent && std::getenv("ARM64_METAL_TRACE_WORKQ")) {
            for (int i = 0; i < keptChanges; ++i) {
                std::int16_t f; std::uint16_t fl; std::uint32_t ff; std::int64_t d;
                std::uint64_t id;
                const auto* c = changes.data() + static_cast<std::size_t>(i) * stride;
                std::memcpy(&id, c, 8); std::memcpy(&f, c + 8, 2); std::memcpy(&fl, c + 10, 2);
                std::memcpy(&ff, c + 24, 4); std::memcpy(&d, c + 32, 8);
                std::cerr << "  change id=0x" << std::hex << id << " f=" << std::dec << f
                          << " fl=0x" << std::hex << fl << " ff=0x" << ff << std::dec << " qos=0x" << std::hex << *reinterpret_cast<const std::uint32_t*>(c + 12) << std::dec
                          << " data=" << d << '\n';
            }
        }
        if (workqKevent && std::getenv("ARM64_METAL_TRACE_WORKQ"))
            std::cerr << "workq-kevent changes=" << keptChanges << " flags=0x" << std::hex
                      << flags << std::dec << " result=" << delivered << " errno=" << errno
                      << '\n';
        if (delivered < 0) {
            rawFail(errno);
            continue;
        }
        if (delivered > 0) {
            if (!netWriteGuest(eventsAddress, events.data(),
                               static_cast<std::size_t>(delivered) * stride)) {
                rawFail(EFAULT);
            } else {
                rawOk(static_cast<std::uint64_t>(delivered));
            }
            continue;
        }
        if (eventCount == 0 || immediate) {
            rawOk(0);
            continue;
        }
        netTryOrPark([&guestMemory, &netHostKevent, kind, kq, eventsAddress, eventCount,
                      hostFlags, deadline, stride](std::uint64_t now) mutable
                         -> std::optional<GuestHostPollResult> {
            const std::vector<std::uint8_t> none;
            std::vector<std::uint8_t> ready(static_cast<std::size_t>(eventCount) * stride);
            const auto count = netHostKevent(kind, kq, none, 0, ready, eventCount, hostFlags);
            if (count < 0) return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
            if (count > 0) {
                try {
                    std::memcpy(guestMemory.buffer(eventsAddress,
                                    static_cast<std::size_t>(count) * stride, true),
                                ready.data(), static_cast<std::size_t>(count) * stride);
                } catch (const std::out_of_range&) {
                    return GuestHostPollResult{EFAULT, true};
                }
                return GuestHostPollResult{static_cast<std::uint64_t>(count), false};
            }
            if (deadline && now >= *deadline) return GuestHostPollResult{0, false};
            return std::nullopt;
        });
        continue;
    }

    if (netSyscall == 375) {  // kevent_id
        constexpr std::uint32_t workloopFlag = 0x400U;
        constexpr std::uint32_t ignoreStale = 0x100U;
        constexpr std::uint32_t qosClassMask = 0x00003f00U;
        constexpr std::uint32_t schedPriorityFlag = 0x20000000U;
        constexpr std::uint32_t eventManagerFlag = 0x02000000U;
        constexpr std::uint16_t evAdd = 0x0001U;
        constexpr std::uint16_t evDelete = 0x0002U;
        constexpr std::uint16_t evEnable = 0x0004U;
        constexpr std::uint16_t evClear = 0x0020U;
        constexpr std::size_t eventStride = netKeventQosStride;
        const auto workloopId = registers[0];
        const auto changesAddress = registers[1];
        const auto changeCount = static_cast<std::int32_t>(registers[2]);
        const auto eventCount = static_cast<std::int32_t>(registers[4]);
        const auto flags = static_cast<std::uint32_t>(registers[7]);
        if ((flags & workloopFlag) == 0) {
            rawFail(EINVAL);
            continue;
        }
        if ((flags & ~(workloopFlag | 0x3U)) != 0) {
            rawFail(ENOTSUP);
            continue;
        }
        if (workloopId == 0 || changeCount < 0 || eventCount < 0 ||
            changeCount > (1 << 16) || eventCount > (1 << 16)) {
            rawFail(EINVAL);
            continue;
        }
        if (changeCount != 1) {
            rawFail(ENOTSUP);
            continue;
        }
        std::vector<std::uint8_t> changes(static_cast<std::size_t>(changeCount) * eventStride);
        if (!netReadGuest(changesAddress, changes.data(), changes.size())) {
            rawFail(EFAULT);
            continue;
        }
        const auto* change = changes.data();
        std::uint64_t ident = 0;
        std::int16_t filter = 0;
        std::uint16_t changeFlags = 0;
        std::int32_t qos = 0;
        std::uint32_t fflags = 0;
        std::memcpy(&ident, change, sizeof(ident));
        std::memcpy(&filter, change + 8, sizeof(filter));
        std::memcpy(&changeFlags, change + 10, sizeof(changeFlags));
        std::memcpy(&qos, change + 12, sizeof(qos));
        std::memcpy(&fflags, change + 24, sizeof(fflags));
        if (filter != -17 || ident != workloopId ||
            (changeFlags == (evAdd | evEnable) ? (fflags != 0x111U && fflags != 0x11U)
             : (changeFlags != evDelete || fflags != 0x121U))) {
            rawFail(ENOTSUP);
            continue;
        }
        const auto priority = static_cast<std::uint32_t>(qos);
        if ((priority & qosClassMask) == 0 ||
            (priority & (schedPriorityFlag | eventManagerFlag)) != 0) {
            rawFail(ERANGE);
            continue;
        }
        std::uint64_t debounceAddress = 0;
        std::uint64_t mask = 0;
        std::uint64_t expected = 0;
        std::memcpy(&debounceAddress, change + 48, sizeof(debounceAddress));
        std::memcpy(&mask, change + 56, sizeof(mask));
        std::memcpy(&expected, change + 64, sizeof(expected));
        if (debounceAddress != 0) {
            std::uint64_t observed = 0;
            if (!netReadGuest(debounceAddress, &observed, sizeof(observed))) {
                rawFail(EFAULT);
                continue;
            }
            if ((observed & mask) != (expected & mask) &&
                (fflags & ignoreStale) != 0) {
                // XNU masks ESTALE and performs no attach/update when requested.
                rawOk(0);
                continue;
            }
            if ((observed & mask) != (expected & mask)) {
                rawFail(ESTALE);
                continue;
            }
        }
        const auto key = std::make_pair(ident, filter);
        auto workloop = guestDynamicWorkloops.find(workloopId);
        std::uint64_t ownerThread = 0;
        if (changeFlags == (evAdd | evEnable)) {
            // Native XNU fires a thread request at successful attach. This lane
            // supports the measured timer QoS and its worker upcall ABI only.
            constexpr std::uint32_t observedTimerQos = 0x800008ffU;
            constexpr std::uint64_t workqFlagPrioQos = 0x00004000U;
            constexpr std::uint64_t workqFlagOvercommit = 0x00010000U;
            constexpr std::uint64_t workqFlagNewSpi = 0x00040000U;
            constexpr std::uint64_t workqFlagKevent = 0x00080000U;
            constexpr std::uint64_t workqFlagTsdBaseSet = 0x00200000U;
            constexpr std::uint64_t workqFlagWorkloop = 0x00400000U;
            if (priority != observedTimerQos || !guestBsdthreadRegistration ||
                !guestWorkqueueDispatchConfig) {
                rawFail(ENOTSUP);
                continue;
            }
            if (workloop != guestDynamicWorkloops.end() &&
                workloop->second.ownerThread != 0) {
                rawFail(EALREADY);
                continue;
            }
            if (workloop != guestDynamicWorkloops.end() &&
                std::any_of(workloop->second.knotes.begin(), workloop->second.knotes.end(),
                    [&](const auto& entry) {
                        return entry.first != key && entry.second.active &&
                               (entry.second.fflags & 0x0fU) == 0x1U;
                    })) {
                rawFail(EALREADY);
                continue;
            }
            auto delivered = changes;
            const auto deliveredWorkloopFlags = static_cast<std::uint16_t>(
                changeFlags | evClear);
            std::memcpy(delivered.data() + 10, &deliveredWorkloopFlags,
                        sizeof(deliveredWorkloopFlags));
            const auto qosIndex = static_cast<std::uint64_t>(__builtin_ffs(
                static_cast<int>((priority & qosClassMask) >> 8U)));
            const auto observedTimerWorkerFlags = workqFlagNewSpi | workqFlagKevent |
                workqFlagTsdBaseSet | workqFlagWorkloop | workqFlagPrioQos | qosIndex |
                ((priority & 0x80000000U) ? workqFlagOvercommit : 0);
            if (!spawnKeventWorker(delivered, 1, observedTimerWorkerFlags, workloopId, &ownerThread)) {
                rawFail(EAGAIN);
                continue;
            }
            if (std::getenv("ARM64_METAL_TRACE_WORKQ"))
                std::cerr << "workloop-worker-spawn id=0x" << std::hex << workloopId
                          << " thread=" << ownerThread << std::dec << '\n';
        }
        if (changeFlags == 0x2U) {
            if (workloop == guestDynamicWorkloops.end()) {
                rawFail(ENOENT);
                continue;
            }
            const auto note = workloop->second.knotes.find(key);
            if (note == workloop->second.knotes.end()) {
                rawFail(ENOENT);
                continue;
            }
            if ((fflags & 0x20U) != 0 &&
                workloop->second.ownerThread == guestScheduler.current_id()) {
                workloop->second.ownerThread = 0;
            }
            workloop->second.knotes.erase(note);
            if (workloop->second.knotes.empty()) guestDynamicWorkloops.erase(workloop);
            rawOk(0);
            continue;
        }
        if (workloop == guestDynamicWorkloops.end()) {
            workloop = guestDynamicWorkloops.emplace(workloopId, GuestDynamicWorkloop{}).first;
        }
        auto note = workloop->second.knotes.find(key);
        if (note == workloop->second.knotes.end()) {
            if (std::any_of(workloop->second.knotes.begin(), workloop->second.knotes.end(),
                    [](const auto& entry) {
                        return entry.second.active && (entry.second.fflags & 0x0fU) == 0x1U;
                    })) {
                rawFail(EALREADY);
                continue;
            }
            note = workloop->second.knotes.emplace(key, GuestDynamicWorkloopKnote{}).first;
        }
        auto& registration = note->second;
        registration.ident = ident;
        registration.filter = filter;
        registration.flags = changeFlags;
        registration.qos = qos;
        std::memcpy(&registration.udata, change + 16, sizeof(registration.udata));
        registration.fflags = fflags;
        std::memcpy(&registration.xflags, change + 28, sizeof(registration.xflags));
        std::memcpy(&registration.data, change + 32, sizeof(registration.data));
        std::memcpy(registration.ext.data(), change + 40, sizeof(registration.ext));
        if (debounceAddress != 0) registration.ext[3] = expected;
        registration.active = true;
        if (ownerThread != 0) workloop->second.ownerThread = ownerThread;
        rawOk(0);
        continue;
    }

    if (netSyscall == 443 || netSyscall == 530) {
        // guarded_kqueue_np and kqueue_workloop_ctl are not yet modeled.
        rawFail(ENOTSUP);
        continue;
    }

    if (netSyscall == 478) {  // bsdthread_ctl
        const auto command = static_cast<std::uint32_t>(registers[0]);
        switch (command) {
        case 0x40: case 0x80: case 0x100: case 0x200: case 0x400: case 0x401: case 0x402:
        case 0x1000: case 0x4000:
            // QoS override and kill-permission hints: this runtime schedules guest
            // threads round-robin, so there is no priority state to adjust.
            rawOk(0);
            break;
        case 0x800: {  // QOS_MAX_PARALLELISM(qos, flags): counts follow the host perflevels.
            constexpr std::uint64_t realtime = 0x2;
            constexpr std::uint64_t clusterShared = 0x4;
            const auto qos = registers[1];
            const auto flags = registers[2];
            const auto sysctlInt = [](const std::string& name) -> std::int64_t {
                std::int32_t value = 0;
                std::size_t length = sizeof(value);
                return ::sysctlbyname(name.c_str(), &value, &length, nullptr, 0) == 0 ? value : -1;
            };
            const auto levels = std::max<std::int64_t>(sysctlInt("hw.nperflevels"), 1);
            const auto clusters = [&](std::int64_t level) -> std::int64_t {
                const auto prefix = "hw.perflevel" + std::to_string(level);
                const auto logical = sysctlInt(prefix + ".logicalcpu");
                const auto perCluster = sysctlInt(prefix + ".cpusperl2");
                if (logical < 1 || perCluster < 1) return 1;
                return (logical + perCluster - 1) / perCluster;
            };
            std::int64_t result = -1;
            if ((flags & ~std::uint64_t{7}) != 0 || qos > 6) {
                result = -1;
            } else if (qos == 0) {
                if ((flags & realtime) != 0)
                    result = (flags & clusterShared) != 0 ? clusters(0) : sysctlInt("hw.perflevel0.logicalcpu");
            } else if ((flags & realtime) == 0) {
                const bool efficiencyOnly = qos <= 2 && levels > 1;
                if ((flags & clusterShared) != 0) {
                    std::int64_t total = 0;
                    for (std::int64_t level = efficiencyOnly ? levels - 1 : 0; level < levels; ++level)
                        total += clusters(level);
                    result = total;
                } else {
                    result = efficiencyOnly ? sysctlInt("hw.perflevel" + std::to_string(levels - 1) + ".logicalcpu")
                                            : sysctlInt("hw.logicalcpu");
                }
            }
            if (result < 1) {
                rawFail(EINVAL);
            } else {
                rawOk(static_cast<std::uint64_t>(result));
            }
            break;
        }
        default:
            rawFail(EINVAL);
            break;
        }
        continue;
    }

    const auto netSocketDescriptor = [&](std::int32_t descriptor) -> int {
        if (!netFdUsable(descriptor)) return EBADF;
        if (hostServiceState.socketDescriptors.count(descriptor) == 0) return ENOTSOCK;
        return 0;
    };
    const auto netUnixAddress = [&](const HostServiceState::GuestUnixSocket& socket,
                                    bool peer, std::vector<std::uint8_t>& bytes) {
        const std::optional<std::string>* path = &socket.boundPath;
        std::uint32_t length = socket.boundAddressLength;
        if (peer) {
            const auto target = socket.connectedPeer
                ? hostServiceState.guestUnixSockets.find(*socket.connectedPeer)
                : hostServiceState.guestUnixSockets.end();
            if (target == hostServiceState.guestUnixSockets.end()) return false;
            path = &target->second.boundPath;
            length = target->second.boundAddressLength;
        }
        bytes.assign(sizeof(sockaddr_un), 0);
        if (path->has_value()) {
            const auto copied = std::min<std::size_t>((*path)->size(), sizeof(sockaddr_un) - 2U);
            std::memcpy(bytes.data() + 2, (*path)->data(), copied);
            bytes[0] = static_cast<std::uint8_t>(std::min<std::uint32_t>(
                length, static_cast<std::uint32_t>(sizeof(sockaddr_un))));
        } else {
            bytes[0] = 16;
        }
        bytes[1] = AF_UNIX;
        return true;
    };

    if (netSyscall == 135) {  // socketpair
        int pair[2] = {-1, -1};
        if (::socketpair(static_cast<int>(registers[0]), static_cast<int>(registers[1]),
                         static_cast<int>(registers[2]), pair) != 0) {
            rawFail(errno);
            continue;
        }
        if (registers[3] < ExecutableImage::minimum_image_address) {
            ::close(pair[0]);
            ::close(pair[1]);
            rawFail(EFAULT);
            continue;
        }
        std::uint8_t* guestPair = nullptr;
        try {
            guestPair = guestMemory.buffer(registers[3], sizeof(int) * 2, true);
        } catch (const std::out_of_range&) {
            ::close(pair[0]);
            ::close(pair[1]);
            rawFail(EFAULT);
            continue;
        }
        std::memcpy(guestPair, pair, sizeof(pair));
        for (const auto descriptor : pair) {
            hostServiceState.openDescriptors.insert(descriptor);
            hostServiceState.socketDescriptors.insert(descriptor);
            hostServiceState.writableDescriptors.insert(descriptor);
        }
        rawOk(0);
        continue;
    }

    if (netSyscall == 105 || netSyscall == 118) {  // setsockopt, getsockopt
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        const auto level = static_cast<int>(registers[1]);
        const auto name = static_cast<int>(registers[2]);
        constexpr std::uint32_t maximumOption = 4096;
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        if (netSyscall == 105) {
            const auto length = static_cast<std::uint32_t>(registers[4]);
            std::vector<std::uint8_t> value(length);
            if (length > maximumOption) {
                rawFail(EINVAL);
            } else if (registers[3] == 0 && length != 0) {
                rawFail(EFAULT);
            } else if (!netReadGuest(registers[3], value.data(), length)) {
                rawFail(EFAULT);
            } else if (::setsockopt(descriptor, level, name, value.data(), length) != 0) {
                rawFail(errno);
            } else {
                rawOk(0);
            }
            continue;
        }
        std::uint32_t length = 0;
        if (registers[4] == 0 || !netReadGuest(registers[4], &length, sizeof(length))) {
            rawFail(EFAULT);
            continue;
        }
        if (length > maximumOption) length = maximumOption;
        std::vector<std::uint8_t> value(length);
        auto hostLength = static_cast<socklen_t>(length);
        if (::getsockopt(descriptor, level, name, value.data(), &hostLength) != 0) {
            rawFail(errno);
            continue;
        }
        const auto produced = std::min<std::uint32_t>(hostLength, length);
        if ((produced != 0 && !netWriteGuest(registers[3], value.data(), produced)) ||
            !netWriteGuest(registers[4], &produced, sizeof(produced))) {
            rawFail(EFAULT);
        } else {
            rawOk(0);
        }
        continue;
    }

    if (netSyscall == 31 || netSyscall == 32) {  // getpeername, getsockname
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        std::uint32_t capacity = 0;
        if (registers[2] == 0 || !netReadGuest(registers[2], &capacity, sizeof(capacity))) {
            rawFail(EFAULT);
            continue;
        }
        std::vector<std::uint8_t> address;
        const auto modeled = hostServiceState.guestUnixSockets.find(descriptor);
        if (modeled != hostServiceState.guestUnixSockets.end()) {
            if (!netUnixAddress(modeled->second, netSyscall == 31, address)) {
                rawFail(ENOTCONN);
                continue;
            }
            address.resize(address[0]);
        } else {
            sockaddr_storage storage{};
            socklen_t storageLength = sizeof(storage);
            const auto status = netSyscall == 31
                ? ::getpeername(descriptor, reinterpret_cast<sockaddr*>(&storage), &storageLength)
                : ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&storage), &storageLength);
            if (status != 0) {
                rawFail(errno);
                continue;
            }
            address.assign(reinterpret_cast<std::uint8_t*>(&storage),
                           reinterpret_cast<std::uint8_t*>(&storage) +
                               std::min<std::size_t>(storageLength, sizeof(storage)));
        }
        const auto produced = static_cast<std::uint32_t>(address.size());
        const auto copied = std::min<std::uint32_t>(capacity, produced);
        if ((copied != 0 && (registers[1] == 0 ||
                             !netWriteGuest(registers[1], address.data(), copied))) ||
            !netWriteGuest(registers[2], &produced, sizeof(produced))) {
            rawFail(EFAULT);
        } else {
            rawOk(0);
        }
        continue;
    }

    if (netSyscall == 106) {  // listen
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        const auto modeled = hostServiceState.guestUnixSockets.find(descriptor);
        if (modeled != hostServiceState.guestUnixSockets.end()) {
            // Guest AF_UNIX stream sockets cannot connect, so none can listen.
            rawFail(EOPNOTSUPP);
        } else if (::listen(descriptor, static_cast<int>(registers[1])) != 0) {
            rawFail(errno);
        } else {
            rawOk(0);
        }
        continue;
    }

    if (netSyscall == 30 || netSyscall == 404) {  // accept, accept_nocancel
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        const auto modeled = hostServiceState.guestUnixSockets.find(descriptor);
        if (modeled != hostServiceState.guestUnixSockets.end()) {
            rawFail(modeled->second.type == SOCK_DGRAM ? EOPNOTSUPP : EINVAL);
            continue;
        }
        // Only host-backed socketpair descriptors reach here; they never listen, so the
        // host call returns immediately.
        sockaddr_storage peer{};
        socklen_t peerLength = sizeof(peer);
        const auto accepted = ::accept(descriptor, reinterpret_cast<sockaddr*>(&peer),
                                       &peerLength);
        if (accepted < 0) {
            rawFail(errno);
        } else {
            ::close(accepted);
            rawFail(EINVAL);
        }
        continue;
    }

    if (netSyscall == 134) {  // shutdown
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        const auto modeled = hostServiceState.guestUnixSockets.find(descriptor);
        if (modeled != hostServiceState.guestUnixSockets.end() &&
            modeled->second.connectedPeer) {
            rawFail(EOPNOTSUPP);
        } else if (::shutdown(descriptor, static_cast<int>(registers[1])) != 0) {
            rawFail(errno);
        } else {
            rawOk(0);
        }
        continue;
    }

    if (netSyscall == 133 || netSyscall == 413 || netSyscall == 29 || netSyscall == 403) {
        // sendto, recvfrom (+ nocancel) on host-backed sockets; guest AF_UNIX datagram
        // sockets are served by their own handler earlier in this chain.
        const bool sending = netSyscall == 133 || netSyscall == 413;
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        const auto flags = static_cast<std::int32_t>(registers[3]);
        constexpr std::uint64_t maximumTransfer = 64ULL * 1024 * 1024;
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        const auto length = std::min<std::uint64_t>(registers[2], maximumTransfer);
        const bool blocking = (flags & MSG_DONTWAIT) == 0 &&
            (::fcntl(descriptor, F_GETFL) & O_NONBLOCK) == 0;
        if (sending) {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
            std::vector<std::uint8_t> name(registers[4] != 0 ? registers[5] : 0);
            if (name.size() > sizeof(sockaddr_storage)) {
                rawFail(EINVAL);
                continue;
            }
            if (!netReadGuest(registers[1], payload.data(), payload.size()) ||
                !netReadGuest(registers[4], name.data(), name.size())) {
                rawFail(EFAULT);
                continue;
            }
            netTryOrPark([descriptor, flags, blocking, payload, name](std::uint64_t)
                             -> std::optional<GuestHostPollResult> {
                const auto sent = ::sendto(descriptor, payload.data(), payload.size(),
                    flags | MSG_DONTWAIT,
                    name.empty() ? nullptr : reinterpret_cast<const sockaddr*>(name.data()),
                    static_cast<socklen_t>(name.size()));
                if (sent >= 0) return GuestHostPollResult{static_cast<std::uint64_t>(sent), false};
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && blocking) return std::nullopt;
                return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
            });
            continue;
        }
        std::uint32_t sourceCapacity = 0;
        if (registers[4] != 0 && (registers[5] == 0 ||
            !netReadGuest(registers[5], &sourceCapacity, sizeof(sourceCapacity)))) {
            rawFail(EFAULT);
            continue;
        }
        const auto bufferAddress = registers[1];
        const auto sourceAddress = registers[4];
        const auto sourceLengthAddress = registers[5];
        netTryOrPark([&guestMemory, descriptor, flags, blocking, length, bufferAddress,
                      sourceAddress, sourceLengthAddress](std::uint64_t)
                         -> std::optional<GuestHostPollResult> {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
            sockaddr_storage source{};
            socklen_t sourceLength = sizeof(source);
            const auto received = ::recvfrom(descriptor, payload.data(), payload.size(),
                flags | MSG_DONTWAIT,
                sourceAddress != 0 ? reinterpret_cast<sockaddr*>(&source) : nullptr,
                sourceAddress != 0 ? &sourceLength : nullptr);
            if (received < 0) {
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && blocking) return std::nullopt;
                return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
            }
            try {
                if (received != 0) {
                    std::memcpy(guestMemory.buffer(bufferAddress,
                                    static_cast<std::size_t>(received), true),
                                payload.data(), static_cast<std::size_t>(received));
                }
                if (sourceAddress != 0) {
                    std::uint32_t capacity = 0;
                    std::memcpy(&capacity, guestMemory.buffer(sourceLengthAddress,
                                    sizeof(capacity)), sizeof(capacity));
                    const auto copied = std::min<std::uint32_t>(capacity, sourceLength);
                    if (copied != 0) {
                        std::memcpy(guestMemory.buffer(sourceAddress, copied, true),
                                    &source, copied);
                    }
                    std::memcpy(guestMemory.buffer(sourceLengthAddress, sizeof(capacity), true),
                                &sourceLength, sizeof(sourceLength));
                }
            } catch (const std::out_of_range&) {
                return GuestHostPollResult{EFAULT, true};
            }
            return GuestHostPollResult{static_cast<std::uint64_t>(received), false};
        });
        continue;
    }

    if (netSyscall == 28 || netSyscall == 402 || netSyscall == 27 || netSyscall == 401) {
        // sendmsg, recvmsg (+ nocancel)
        const bool sending = netSyscall == 28 || netSyscall == 402;
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        const auto flags = static_cast<std::int32_t>(registers[2]);
        constexpr std::uint64_t maximumTransfer = 64ULL * 1024 * 1024;
        constexpr std::uint32_t maximumControl = 4096;
        if (const auto error = netSocketDescriptor(descriptor)) {
            rawFail(error);
            continue;
        }
        std::uint8_t header[48] = {};
        if (!netReadGuest(registers[1], header, sizeof(header))) {
            rawFail(EFAULT);
            continue;
        }
        std::uint64_t nameAddress = 0, vectorAddress = 0, controlAddress = 0;
        std::uint32_t nameLength = 0, controlLength = 0;
        std::int32_t vectorCount = 0;
        std::memcpy(&nameAddress, header, 8);
        std::memcpy(&nameLength, header + 8, 4);
        std::memcpy(&vectorAddress, header + 16, 8);
        std::memcpy(&vectorCount, header + 24, 4);
        std::memcpy(&controlAddress, header + 32, 8);
        std::memcpy(&controlLength, header + 40, 4);
        if (vectorCount < 0 || vectorCount > 1024) {
            rawFail(EMSGSIZE);
            continue;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> segments(
            static_cast<std::size_t>(vectorCount));
        std::uint64_t total = 0;
        bool bad = false;
        for (std::int32_t index = 0; index < vectorCount && !bad; ++index) {
            std::uint64_t pair[2] = {0, 0};
            if (!netReadGuest(vectorAddress + static_cast<std::uint64_t>(index) * 16, pair,
                              sizeof(pair))) {
                rawFail(EFAULT);
                bad = true;
            } else if (total > maximumTransfer || pair[1] > maximumTransfer - total) {
                rawFail(EMSGSIZE);
                bad = true;
            } else {
                segments[static_cast<std::size_t>(index)] = {pair[0], pair[1]};
                total += pair[1];
            }
        }
        if (bad) continue;
        const bool blocking = (flags & MSG_DONTWAIT) == 0 &&
            (::fcntl(descriptor, F_GETFL) & O_NONBLOCK) == 0;
        const auto modeled = hostServiceState.guestUnixSockets.find(descriptor);
        if (sending) {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(total));
            std::size_t cursor = 0;
            for (const auto& segment : segments) {
                if (!netReadGuest(segment.first, payload.data() + cursor,
                                  static_cast<std::size_t>(segment.second))) {
                    bad = true;
                    break;
                }
                cursor += static_cast<std::size_t>(segment.second);
            }
            std::vector<std::uint8_t> name(nameAddress != 0 ? nameLength : 0);
            std::vector<std::uint8_t> control(controlAddress != 0 ? controlLength : 0);
            if (name.size() > sizeof(sockaddr_storage) || control.size() > maximumControl) {
                rawFail(EINVAL);
                continue;
            }
            if (bad || !netReadGuest(nameAddress, name.data(), name.size()) ||
                !netReadGuest(controlAddress, control.data(), control.size())) {
                rawFail(EFAULT);
                continue;
            }
            if (modeled != hostServiceState.guestUnixSockets.end()) {
                if (modeled->second.type != SOCK_DGRAM || !control.empty() || flags != 0) {
                    rawFail(EOPNOTSUPP);
                    continue;
                }
                std::int32_t peerDescriptor = -1;
                std::uint32_t error = 0;
                if (name.empty()) {
                    if (modeled->second.connectedPeerClosed) error = ECONNRESET;
                    else if (!modeled->second.connectedPeer) error = EDESTADDRREQ;
                    else peerDescriptor = *modeled->second.connectedPeer;
                } else {
                    std::vector<std::uint8_t> padded(name);
                    const auto path = guest_unix_socket_path(guestMemory, nameAddress,
                        nameLength, error);
                    if (path) {
                        const auto target = hostServiceState.guestUnixSocketPaths.find(*path);
                        if (target == hostServiceState.guestUnixSocketPaths.end()) error = ENOENT;
                        else peerDescriptor = target->second;
                    }
                }
                const auto peer = hostServiceState.guestUnixSockets.find(peerDescriptor);
                if (error == 0 && peer == hostServiceState.guestUnixSockets.end())
                    error = ECONNREFUSED;
                else if (error == 0 && peer->second.type != SOCK_DGRAM) error = EPROTOTYPE;
                if (error != 0) {
                    rawFail(static_cast<int>(error));
                    continue;
                }
                HostServiceState::GuestUnixDatagram datagram;
                datagram.payload = std::move(payload);
                if (!peer->second.captureSink) {
                    datagram.sourcePath = modeled->second.boundPath;
                    datagram.sourceLength = modeled->second.boundPath
                        ? modeled->second.boundAddressLength : 16U;
                }
                const auto sent = datagram.payload.size();
                if (peer->second.captureSink) {
                    constexpr std::size_t maximumCapturedDatagrams = 64;
                    if (hostServiceState.guestSyslogCapture.size() == maximumCapturedDatagrams)
                        hostServiceState.guestSyslogCapture.pop_front();
                    hostServiceState.guestSyslogCapture.push_back(std::move(datagram));
                } else {
                    peer->second.receiveQueue.push_back(std::move(datagram));
                    deliverGuestUnixDatagrams(peerDescriptor);
                }
                rawOk(sent);
                continue;
            }
            for (std::size_t offset = 0; offset + sizeof(cmsghdr) <= control.size();) {
                cmsghdr item{};
                std::memcpy(&item, control.data() + offset, sizeof(item));
                if (item.cmsg_len < sizeof(cmsghdr) || offset + item.cmsg_len > control.size()) {
                    break;
                }
                if (item.cmsg_level == SOL_SOCKET && item.cmsg_type == SCM_RIGHTS) {
                    const auto count = (item.cmsg_len - sizeof(cmsghdr)) / sizeof(int);
                    for (std::size_t index = 0; index < count; ++index) {
                        std::int32_t passed = -1;
                        std::memcpy(&passed, control.data() + offset + sizeof(cmsghdr) +
                                    index * sizeof(int), sizeof(passed));
                        if (!netFdUsable(passed)) bad = true;
                    }
                }
                offset += (item.cmsg_len + 3U) & ~std::size_t{3};
            }
            if (bad) {
                rawFail(EBADF);
                continue;
            }
            netTryOrPark([descriptor, flags, blocking, payload, name, control](std::uint64_t)
                             mutable -> std::optional<GuestHostPollResult> {
                iovec vector{payload.empty() ? nullptr : payload.data(), payload.size()};
                msghdr message{};
                message.msg_name = name.empty() ? nullptr : name.data();
                message.msg_namelen = static_cast<socklen_t>(name.size());
                message.msg_iov = &vector;
                message.msg_iovlen = 1;
                message.msg_control = control.empty() ? nullptr : control.data();
                message.msg_controllen = static_cast<socklen_t>(control.size());
                const auto sent = ::sendmsg(descriptor, &message, flags | MSG_DONTWAIT);
                if (sent >= 0) return GuestHostPollResult{static_cast<std::uint64_t>(sent), false};
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && blocking) return std::nullopt;
                return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
            });
            continue;
        }
        const auto messageAddress = registers[1];
        const bool modeledSocket = modeled != hostServiceState.guestUnixSockets.end();
        if (modeledSocket && (modeled->second.type != SOCK_DGRAM || controlLength != 0 ||
                              (flags & ~MSG_DONTWAIT) != 0)) {
            rawFail(EOPNOTSUPP);
            continue;
        }
        netTryOrPark([&hostServiceState, &guestMemory, &netFdUsable, descriptor, flags,
                      blocking, segments, total, nameAddress, nameLength, controlAddress,
                      controlLength, messageAddress, modeledSocket](std::uint64_t)
                         mutable -> std::optional<GuestHostPollResult> {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(total));
            std::vector<std::uint8_t> name(nameAddress != 0 ? nameLength : 0);
            std::vector<std::uint8_t> control(controlAddress != 0 ? controlLength : 0);
            std::int32_t messageFlags = 0;
            std::size_t received = 0;
            std::uint32_t producedName = 0;
            std::uint32_t producedControl = 0;
            if (modeledSocket) {
                const auto socket = hostServiceState.guestUnixSockets.find(descriptor);
                if (socket == hostServiceState.guestUnixSockets.end())
                    return GuestHostPollResult{EBADF, true};
                if (socket->second.receiveQueue.empty()) {
                    if (!blocking) return GuestHostPollResult{EAGAIN, true};
                    return std::nullopt;
                }
                const auto& datagram = socket->second.receiveQueue.front();
                received = std::min<std::size_t>(payload.size(), datagram.payload.size());
                std::memcpy(payload.data(), datagram.payload.data(), received);
                if (received < datagram.payload.size()) messageFlags |= MSG_TRUNC;
                if (!name.empty()) {
                    std::vector<std::uint8_t> address(sizeof(sockaddr_un), 0);
                    address[0] = static_cast<std::uint8_t>(datagram.sourceLength);
                    address[1] = AF_UNIX;
                    if (datagram.sourcePath) {
                        std::memcpy(address.data() + 2, datagram.sourcePath->data(),
                            std::min<std::size_t>(datagram.sourcePath->size(),
                                                  address.size() - 2));
                    }
                    producedName = datagram.sourceLength;
                    std::memcpy(name.data(), address.data(),
                                std::min<std::size_t>(name.size(), producedName));
                }
                if ((flags & MSG_PEEK) == 0) socket->second.receiveQueue.pop_front();
            } else {
                iovec vector{payload.empty() ? nullptr : payload.data(), payload.size()};
                msghdr message{};
                message.msg_name = name.empty() ? nullptr : name.data();
                message.msg_namelen = static_cast<socklen_t>(name.size());
                message.msg_iov = &vector;
                message.msg_iovlen = 1;
                message.msg_control = control.empty() ? nullptr : control.data();
                message.msg_controllen = static_cast<socklen_t>(control.size());
                const auto count = ::recvmsg(descriptor, &message, flags | MSG_DONTWAIT);
                if (count < 0) {
                    if ((errno == EAGAIN || errno == EWOULDBLOCK) && blocking)
                        return std::nullopt;
                    return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
                }
                received = static_cast<std::size_t>(count);
                producedName = message.msg_namelen;
                producedControl = message.msg_controllen;
                messageFlags = message.msg_flags;
                for (std::size_t offset = 0; offset + sizeof(cmsghdr) <= producedControl;) {
                    cmsghdr item{};
                    std::memcpy(&item, control.data() + offset, sizeof(item));
                    if (item.cmsg_len < sizeof(cmsghdr) || offset + item.cmsg_len > producedControl)
                        break;
                    if (item.cmsg_level == SOL_SOCKET && item.cmsg_type == SCM_RIGHTS) {
                        const auto fds = (item.cmsg_len - sizeof(cmsghdr)) / sizeof(int);
                        for (std::size_t index = 0; index < fds; ++index) {
                            std::int32_t received_fd = -1;
                            std::memcpy(&received_fd, control.data() + offset + sizeof(cmsghdr) +
                                        index * sizeof(int), sizeof(received_fd));
                            hostServiceState.openDescriptors.insert(received_fd);
                            const auto access = ::fcntl(received_fd, F_GETFL);
                            if (access >= 0 && (access & O_ACCMODE) != O_RDONLY)
                                hostServiceState.writableDescriptors.insert(received_fd);
                        }
                    }
                    offset += (item.cmsg_len + 3U) & ~std::size_t{3};
                }
            }
            try {
                std::size_t cursor = 0;
                for (const auto& segment : segments) {
                    const auto piece = std::min<std::size_t>(segment.second, received - cursor);
                    if (piece == 0) break;
                    std::memcpy(guestMemory.buffer(segment.first, piece, true),
                                payload.data() + cursor, piece);
                    cursor += piece;
                }
                if (!name.empty()) {
                    std::memcpy(guestMemory.buffer(nameAddress,
                        std::min<std::size_t>(name.size(), producedName), true), name.data(),
                        std::min<std::size_t>(name.size(), producedName));
                }
                if (producedControl != 0) {
                    std::memcpy(guestMemory.buffer(controlAddress, producedControl, true),
                                control.data(), producedControl);
                }
                std::memcpy(guestMemory.buffer(messageAddress + 8, 4, true), &producedName, 4);
                std::memcpy(guestMemory.buffer(messageAddress + 40, 4, true), &producedControl, 4);
                std::memcpy(guestMemory.buffer(messageAddress + 44, 4, true), &messageFlags, 4);
            } catch (const std::out_of_range&) {
                return GuestHostPollResult{EFAULT, true};
            }
            (void)netFdUsable;
            return GuestHostPollResult{received, false};
        });
        continue;
    }

    if (netSyscall == 448) {  // disconnectx: the multipath socket layer is absent
        rawFail(ENOSYS);
        continue;
    }
    if (netSyscall == 480 || netSyscall == 481 || netSyscall == 447 || netSyscall == 449) {
        // recvmsg_x, sendmsg_x, connectx, disconnectx, peeloff: batch and multipath
        // socket variants are not modeled.
        rawFail(netFdUsable(static_cast<std::int32_t>(registers[0])) ? EOPNOTSUPP : EBADF);
        continue;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    switch (netSyscall) {  // kernel networking and memorystatus control surfaces
    case 450: case 522:  // socket_delegate, necp_session_open: entitlement checks
        rawFail(EACCES);
        continue;
    case 501: case 439: case 440:  // necp_open, kas_info, memorystatus_control: privileged
        rawFail(EPERM);
        continue;
    case 436: case 460: case 490: case 503: case 504: case 505: case 506: case 507: case 510:
    case 525:  // pid_shutdown_sockets, necp_match_policy, netagent_trigger, nexus and channel setup
        rawFail(EINVAL);
        continue;
    case 502: case 508: case 509: case 511: case 512: case 513: case 514: case 523:
        rawFail(ENODEV);  // necp client, nexus and channel operations: no such device
        continue;
    case 296: {  // vm_pressure_monitor: only the non-waiting query is served
        if (registers[0] != 0) {
            rawFail(ENOTSUP);
            continue;
        }
        int pages = 0;
        const auto level = ::syscall(296, 0, static_cast<int>(registers[1]), &pages);
        if (level < 0) {
            rawFail(errno);
        } else if (registers[2] != 0 && !netWriteGuest(registers[2], &pages, sizeof(pages))) {
            rawFail(EFAULT);
        } else {
            rawOk(static_cast<std::uint64_t>(level));
        }
        continue;
    }
    case 453: {  // memorystatus_get_level
        int level = 0;
        if (::syscall(453, &level) < 0) rawFail(errno);
        else if (!netWriteGuest(registers[0], &level, sizeof(level))) rawFail(EFAULT);
        else rawOk(0);
        continue;
    }
    case 534: {  // memorystatus_available_memory
        const auto result = ::syscall(534);
        if (result < 0) rawFail(errno);
        else rawOk(static_cast<std::uint64_t>(result));
        continue;
    }
    default:
        break;
    }
#pragma clang diagnostic pop

    const auto netRangeMapped = [&](std::uint64_t address, std::uint64_t length) {
        if (length > std::numeric_limits<std::uint64_t>::max() - address) return false;
        if (!hostServiceState.machVmRegion) return guestMemory.is_guest_readable(address, length);
        auto cursor = address;
        const auto end = address + length;
        while (cursor < end) {
            const auto region = hostServiceState.machVmRegion(cursor);
            if (!region || region->begin > cursor || region->end <= cursor) return false;
            cursor = region->end;
        }
        return true;
    };
    const auto netCallService = [&](std::uint64_t service) {
        std::vector<std::uint64_t> trap(registers, registers + 36);
        auto* errnoBytes = guestMemory.buffer(bootstrap.errnoSlot, sizeof(std::uint32_t), true);
        std::uint32_t saved = 0;
        std::memcpy(&saved, errnoBytes, sizeof(saved));
        (void)dispatch_guest_host_service(trap, guestMemory, bootstrap, hostServiceState,
                                          service);
        std::uint32_t error = 0;
        std::memcpy(&error, errnoBytes, sizeof(error));
        std::memcpy(errnoBytes, &saved, sizeof(saved));
        if (trap[0] == std::numeric_limits<std::uint64_t>::max() ||
            trap[0] == std::numeric_limits<std::uint32_t>::max()) {
            rawFail(error != 0 ? static_cast<int>(error) : EINVAL);
        } else {
            rawOk(trap[0]);
        }
    };
    const auto netPage = bootstrap.pageSize != 0 ? bootstrap.pageSize : std::uint64_t{16384};

    if (netSyscall == 197) {  // mmap
        netCallService(107);
        continue;
    }
    if (netSyscall == 73) {  // munmap
        if (registers[0] % netPage != 0 || registers[1] == 0) {
            rawFail(EINVAL);
            continue;
        }
        if (registers[1] <= std::numeric_limits<std::uint64_t>::max() - netPage) {
            const auto span = (registers[1] + netPage - 1) & ~(netPage - 1);
            if (!netRangeMapped(registers[0], span) && !netRangeMapped(registers[0], netPage)) {
                rawOk(0);  // unmapping an unmapped range succeeds on Darwin
                continue;
            }
        }
        netCallService(108);
        continue;
    }
    if (netSyscall == 74) {  // mprotect
        netCallService(116);
        continue;
    }
    if (netSyscall == 266) {  // shm_open
        netCallService(105);
        continue;
    }
    if (netSyscall == 267) {  // shm_unlink
        netCallService(106);
        continue;
    }

    if (netSyscall == 75) {  // madvise
        const auto address = registers[0] & ~(netPage - 1);
        const auto length = registers[1];
        const auto advice = static_cast<std::int32_t>(registers[2]);
        if (advice < 0 || advice > 11 ||
            length > std::numeric_limits<std::uint64_t>::max() - netPage) {
            rawFail(EINVAL);
            continue;
        }
        const auto span = (length + netPage - 1) & ~(netPage - 1);
        if (span != 0 && !netRangeMapped(address, span)) {
            rawFail(ENOMEM);
            continue;
        }
        constexpr std::int32_t madvZero = 11;
        if (advice == madvZero && span != 0) {
            try {
                std::memset(guestMemory.buffer(address, span, true), 0,
                            static_cast<std::size_t>(span));
            } catch (const std::out_of_range&) {
                rawFail(EFAULT);
                continue;
            }
        }
        // Every other advice value is a paging hint; guest memory is always resident.
        rawOk(0);
        continue;
    }

    if (netSyscall == 65 || netSyscall == 405) {  // msync, msync_nocancel
        constexpr std::int32_t msAsync = 0x1, msSync = 0x10;
        const auto address = registers[0];
        const auto length = registers[1];
        const auto flags = static_cast<std::int32_t>(registers[2]);
        if (address % netPage != 0 ||
            ((flags & msAsync) != 0 && (flags & msSync) != 0) ||
            length > std::numeric_limits<std::uint64_t>::max() - netPage) {
            rawFail(EINVAL);
            continue;
        }
        const auto span = (length + netPage - 1) & ~(netPage - 1);
        if (span != 0 && !netRangeMapped(address, span)) rawFail(ENOMEM);
        else rawOk(0);  // guest file mappings are read-only copies; nothing to write back
        continue;
    }

    if (netSyscall == 78) {  // mincore
        const auto address = registers[0];
        const auto length = registers[1];
        if (address % netPage != 0 ||
            length > std::numeric_limits<std::uint64_t>::max() - netPage) {
            rawFail(EINVAL);
            continue;
        }
        const auto span = (length + netPage - 1) & ~(netPage - 1);
        std::vector<std::uint8_t> residency(static_cast<std::size_t>(span / netPage), 1);
        for (std::size_t page = 0; page < residency.size(); ++page) {
            if (!netRangeMapped(address + page * netPage, netPage)) residency[page] = 0x80;
        }
        if (!netWriteGuest(registers[2], residency.data(), residency.size())) rawFail(EFAULT);
        else rawOk(0);
        continue;
    }

    if (netSyscall == 203 || netSyscall == 204) {  // mlock, munlock
        rawOk(0);  // guest memory is always resident; the host kernel does not range-check either
        continue;
    }

    if (netSyscall == 324 || netSyscall == 325) {  // mlockall, munlockall
        constexpr std::int32_t mclCurrent = 0x1, mclFuture = 0x2;
        const auto flags = static_cast<std::int32_t>(registers[0]);
        if (netSyscall == 324 && ((flags & ~(mclCurrent | mclFuture)) != 0 || flags == 0)) {
            rawFail(EINVAL);
        } else {
            rawOk(0);
        }
        continue;
    }

    if (netSyscall == 250) {  // minherit
        // Only the inheritance value is validated: the guest has no fork, so the
        // setting has no observer, and the host kernel does not range-check either.
        if (static_cast<std::int32_t>(registers[2]) < 0 || static_cast<std::int32_t>(registers[2]) > 3)
            rawFail(EINVAL);
        else
            rawOk(0);
        continue;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (netSyscall == 255) {  // semget
        const auto result = ::syscall(SYS_semget, static_cast<int>(registers[0]),
                                      static_cast<int>(registers[1]),
                                      static_cast<int>(registers[2]));
        if (result < 0) rawFail(errno);
        else rawOk(static_cast<std::uint64_t>(result));
        continue;
    }
    if (netSyscall == 256) {  // semop
        const auto count = static_cast<std::uint32_t>(registers[2]);
        if (count > 100) {
            rawFail(E2BIG);
            continue;
        }
        std::vector<std::int16_t> operations(static_cast<std::size_t>(count) * 3U);
        if (!netReadGuest(registers[1], operations.data(), operations.size() * 2U)) {
            rawFail(EFAULT);
            continue;
        }
        bool wait = false;
        for (std::size_t index = 0; index < count; ++index) {
            if ((operations[index * 3 + 2] & IPC_NOWAIT) == 0) wait = true;
            operations[index * 3 + 2] = static_cast<std::int16_t>(
                operations[index * 3 + 2] | IPC_NOWAIT);
        }
        const auto semid = static_cast<int>(registers[0]);
        netTryOrPark([semid, count, wait, operations](std::uint64_t)
                         mutable -> std::optional<GuestHostPollResult> {
            const auto result = ::syscall(SYS_semop, semid, operations.data(),
                                          static_cast<unsigned long>(count));
            if (result >= 0) return GuestHostPollResult{0, false};
            if (errno == EAGAIN && wait) return std::nullopt;
            return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
        });
        continue;
    }
    if (netSyscall == 254) {  // semctl
        const auto semid = static_cast<int>(registers[0]);
        const auto semnum = static_cast<int>(registers[1]);
        const auto command = static_cast<int>(registers[2]);
        const auto argument = registers[3];
        constexpr int ipcRmid = 0, ipcSet = 1, ipcStat = 2, getAll = 6, setVal = 8,
                      setAll = 9;
        if (command == ipcStat || command == ipcSet) {
            std::uint8_t buffer[256] = {};
            if (command == ipcSet && !netReadGuest(argument, buffer, sizeof(semid_ds))) {
                rawFail(EFAULT);
                continue;
            }
            const auto result = ::syscall(SYS_semctl, semid, semnum, command, buffer);
            if (result < 0) rawFail(errno);
            else if (command == ipcStat && !netWriteGuest(argument, buffer, sizeof(semid_ds)))
                rawFail(EFAULT);
            else rawOk(static_cast<std::uint64_t>(result));
        } else if (command == getAll || command == setAll) {
            std::uint8_t status[256] = {};
            if (::syscall(SYS_semctl, semid, 0, ipcStat, status) < 0) {
                rawFail(errno);
                continue;
            }
            semid_ds statistics{};
            std::memcpy(&statistics, status, sizeof(statistics));
            std::vector<std::uint16_t> values(statistics.sem_nsems);
            if (command == setAll && !netReadGuest(argument, values.data(), values.size() * 2U)) {
                rawFail(EFAULT);
                continue;
            }
            const auto result = ::syscall(SYS_semctl, semid, semnum, command, values.data());
            if (result < 0) rawFail(errno);
            else if (command == getAll && !netWriteGuest(argument, values.data(),
                                                         values.size() * 2U)) rawFail(EFAULT);
            else rawOk(static_cast<std::uint64_t>(result));
        } else {
            const auto result = ::syscall(SYS_semctl, semid, semnum, command,
                command == setVal ? static_cast<int>(argument) : 0);
            if (result < 0) rawFail(errno);
            else rawOk(static_cast<std::uint64_t>(result));
            (void)ipcRmid;
        }
        continue;
    }
    if (netSyscall == 259) {  // msgget
        const auto result = ::syscall(SYS_msgget, static_cast<long>(registers[0]),
                                      static_cast<int>(registers[1]));
        if (result < 0) rawFail(errno);
        else rawOk(static_cast<std::uint64_t>(result));
        continue;
    }
    if (netSyscall == 260 || netSyscall == 418) {  // msgsnd
        const auto size = registers[2];
        if (size > 64ULL * 1024 * 1024) {
            rawFail(EINVAL);
            continue;
        }
        std::vector<std::uint8_t> message(sizeof(long) + static_cast<std::size_t>(size));
        if (!netReadGuest(registers[1], message.data(), message.size())) {
            rawFail(EFAULT);
            continue;
        }
        const auto queue = static_cast<int>(registers[0]);
        const auto flags = static_cast<int>(registers[3]);
        netTryOrPark([queue, size, flags, message](std::uint64_t)
                         -> std::optional<GuestHostPollResult> {
            const auto result = ::syscall(SYS_msgsnd, queue, message.data(),
                                          static_cast<size_t>(size), flags | IPC_NOWAIT);
            if (result >= 0) return GuestHostPollResult{0, false};
            if (errno == EAGAIN && (flags & IPC_NOWAIT) == 0) return std::nullopt;
            return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
        });
        continue;
    }
    if (netSyscall == 261 || netSyscall == 419) {  // msgrcv
        const auto size = registers[2];
        if (size > 64ULL * 1024 * 1024) {
            rawFail(EINVAL);
            continue;
        }
        const auto queue = static_cast<int>(registers[0]);
        const auto address = registers[1];
        const auto type = static_cast<long>(registers[3]);
        const auto flags = static_cast<int>(registers[4]);
        netTryOrPark([&guestMemory, queue, address, size, type, flags](std::uint64_t)
                         -> std::optional<GuestHostPollResult> {
            std::vector<std::uint8_t> message(sizeof(long) + static_cast<std::size_t>(size));
            const auto result = ::syscall(SYS_msgrcv, queue, message.data(),
                                          static_cast<size_t>(size), type, flags | IPC_NOWAIT);
            if (result < 0) {
                if ((errno == ENOMSG || errno == EAGAIN) && (flags & IPC_NOWAIT) == 0)
                    return std::nullopt;
                return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
            }
            try {
                std::memcpy(guestMemory.buffer(address,
                                sizeof(long) + static_cast<std::size_t>(result), true),
                            message.data(), sizeof(long) + static_cast<std::size_t>(result));
            } catch (const std::out_of_range&) {
                return GuestHostPollResult{EFAULT, true};
            }
            return GuestHostPollResult{static_cast<std::uint64_t>(result), false};
        });
        continue;
    }
    if (netSyscall == 258) {  // msgctl
        const auto queue = static_cast<int>(registers[0]);
        const auto command = static_cast<int>(registers[1]);
        std::uint8_t buffer[256] = {};
        constexpr int ipcSet = 1, ipcStat = 2;
        if (command == ipcSet && !netReadGuest(registers[2], buffer, sizeof(msqid_ds))) {
            rawFail(EFAULT);
            continue;
        }
        const auto result = ::syscall(SYS_msgctl, queue, command,
                                      command == ipcSet || command == ipcStat ? buffer : nullptr);
        if (result < 0) rawFail(errno);
        else if (command == ipcStat && !netWriteGuest(registers[2], buffer, sizeof(msqid_ds)))
            rawFail(EFAULT);
        else rawOk(static_cast<std::uint64_t>(result));
        continue;
    }
    if (netSyscall == 251 || netSyscall == 252 || netSyscall == 253) {
        rawFail(EINVAL);  // the multiplexed semsys/msgsys/shmsys entry points are obsolete
        continue;
    }
    if (netSyscall == 262 || netSyscall == 263 || netSyscall == 264 || netSyscall == 265) {
        // SysV shared memory cannot be attached into guest memory.
        rawFail(ENOSYS);
        continue;
    }

    if (netSyscall == 268) {  // sem_open
        const auto path = rawPath(registers[0]);
        if (!path) {
            rawFail(EFAULT);
            continue;
        }
        const auto result = ::syscall(SYS_sem_open, path->c_str(),
            static_cast<int>(registers[1]), static_cast<int>(registers[2]),
            static_cast<int>(registers[3]));
        if (result == -1) rawFail(errno);
        else rawOk(static_cast<std::uint64_t>(result));
        continue;
    }
    if (netSyscall == 270) {  // sem_unlink
        const auto path = rawPath(registers[0]);
        if (!path) rawFail(EFAULT);
        else if (::syscall(SYS_sem_unlink, path->c_str()) != 0) rawFail(errno);
        else rawOk(0);
        continue;
    }
    if (netSyscall == 269 || netSyscall == 272 || netSyscall == 273) {
        // sem_close, sem_trywait, sem_post
        const auto number = netSyscall == 269 ? SYS_sem_close
            : netSyscall == 272 ? SYS_sem_trywait : SYS_sem_post;
        if (::syscall(number, static_cast<int>(registers[0])) != 0) rawFail(errno);
        else rawOk(0);
        continue;
    }
    if (netSyscall == 271 || netSyscall == 420) {  // sem_wait, sem_wait_nocancel
        const auto handle = static_cast<int>(registers[0]);
        netTryOrPark([handle](std::uint64_t) -> std::optional<GuestHostPollResult> {
            if (::syscall(SYS_sem_trywait, handle) == 0) return GuestHostPollResult{0, false};
            if (errno == EAGAIN) return std::nullopt;
            return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
        });
        continue;
    }
#pragma clang diagnostic pop

    if (netSyscall == 92 || netSyscall == 406) {  // fcntl, fcntl_nocancel
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        const auto command = static_cast<int>(registers[1]);
        const auto argument = registers[2];
        const bool standard = descriptor >= STDIN_FILENO && descriptor <= STDERR_FILENO;
        if (!netFdUsable(descriptor)) {
            rawFail(EBADF);
            continue;
        }
        const auto trackDuplicate = [&](int duplicate) {
            hostServiceState.openDescriptors.insert(duplicate);
            if (hostServiceState.writableDescriptors.count(descriptor) != 0 ||
                (standard && descriptor != STDIN_FILENO))
                hostServiceState.writableDescriptors.insert(duplicate);
            if (hostServiceState.socketDescriptors.count(descriptor) != 0)
                hostServiceState.socketDescriptors.insert(duplicate);
        };
        switch (command) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            const auto duplicate = ::fcntl(descriptor, command, static_cast<int>(argument));
            if (duplicate < 0) {
                rawFail(errno);
            } else {
                trackDuplicate(duplicate);
                rawOk(static_cast<std::uint64_t>(duplicate));
            }
            break;
        }
        case F_GETFD: {
            const auto found = hostServiceState.standardDescriptorFlags.find(descriptor);
            const auto result = found == hostServiceState.standardDescriptorFlags.end()
                ? ::fcntl(descriptor, command) : found->second;
            if (result < 0) rawFail(errno);
            else rawOk(static_cast<std::uint64_t>(result));
            break;
        }
        case F_SETFD: {
            if (standard) {
                hostServiceState.standardDescriptorFlags[descriptor] =
                    static_cast<std::int32_t>(argument);
                rawOk(0);
            } else if (::fcntl(descriptor, command, static_cast<int>(argument)) < 0) {
                rawFail(errno);
            } else {
                rawOk(0);
            }
            break;
        }
        case F_GETFL: {
            const auto native = ::fcntl(descriptor, command);
            if (native < 0) {
                rawFail(errno);
                break;
            }
            const auto found = hostServiceState.standardStatusFlags.find(descriptor);
            rawOk(static_cast<std::uint64_t>(found == hostServiceState.standardStatusFlags.end()
                ? native : ((native & ~O_NONBLOCK) | (found->second & O_NONBLOCK))));
            break;
        }
        case F_SETFL: {
            const auto value = static_cast<std::int32_t>(argument);
            if (standard) {
                const auto native = ::fcntl(descriptor, F_GETFL);
                if (native < 0) rawFail(errno);
                else if (((native ^ value) & (O_APPEND | O_ASYNC)) != 0) rawFail(EINVAL);
                else {
                    hostServiceState.standardStatusFlags[descriptor] = value;
                    rawOk(0);
                }
            } else if (::fcntl(descriptor, command, value) < 0) {
                rawFail(errno);
            } else {
                rawOk(0);
            }
            break;
        }
        case F_GETOWN:
        case F_GETNOSIGPIPE:
        case F_GETPROTECTIONCLASS:
        case F_FULLFSYNC:
        case F_BARRIERFSYNC: {
            const auto result = ::fcntl(descriptor, command);
            if (result < 0) rawFail(errno);
            else rawOk(static_cast<std::uint64_t>(result));
            break;
        }
        case F_SETOWN:
        case F_RDAHEAD:
        case F_NOCACHE:
        case F_SETNOSIGPIPE:
        case F_NODIRECT:
        case F_SETPROTECTIONCLASS:
        case F_SINGLE_WRITER: {
            if (::fcntl(descriptor, command, static_cast<int>(argument)) < 0) rawFail(errno);
            else rawOk(0);
            break;
        }
        case F_GETLK:
        case F_SETLK:
        case F_GETLKPID:
        case F_OFD_GETLK:
        case F_OFD_SETLK:
        case F_SETLKW:
        case F_OFD_SETLKW: {
            struct flock lock{};
            if (!netReadGuest(argument, &lock, sizeof(lock))) {
                rawFail(EFAULT);
                break;
            }
            const bool waits = command == F_SETLKW || command == F_OFD_SETLKW;
            const int hostCommand = command == F_SETLKW ? F_SETLK
                : command == F_OFD_SETLKW ? F_OFD_SETLK : command;
            netTryOrPark([&guestMemory, descriptor, hostCommand, waits, lock, argument](
                             std::uint64_t) mutable -> std::optional<GuestHostPollResult> {
                if (::fcntl(descriptor, hostCommand, &lock) < 0) {
                    if (waits && (errno == EAGAIN || errno == EACCES)) return std::nullopt;
                    return GuestHostPollResult{static_cast<std::uint64_t>(errno), true};
                }
                if (hostCommand == F_GETLK || hostCommand == F_GETLKPID ||
                    hostCommand == F_OFD_GETLK) {
                    try {
                        std::memcpy(guestMemory.buffer(argument, sizeof(lock), true), &lock,
                                    sizeof(lock));
                    } catch (const std::out_of_range&) {
                        return GuestHostPollResult{EFAULT, true};
                    }
                }
                return GuestHostPollResult{0, false};
            });
            break;
        }
        case F_GETPATH: {
            std::array<char, MAXPATHLEN> path{};
            if (::fcntl(descriptor, command, path.data()) < 0) rawFail(errno);
            else if (!netWriteGuest(argument, path.data(), std::strlen(path.data()) + 1))
                rawFail(EFAULT);
            else rawOk(0);
            break;
        }
        case F_PREALLOCATE: {
            fstore store{};
            if (!netReadGuest(argument, &store, sizeof(store))) rawFail(EFAULT);
            else if (::fcntl(descriptor, command, &store) < 0) rawFail(errno);
            else if (!netWriteGuest(argument, &store, sizeof(store))) rawFail(EFAULT);
            else rawOk(0);
            break;
        }
        case F_SETSIZE: {
            off_t size = 0;
            if (!netReadGuest(argument, &size, sizeof(size))) rawFail(EFAULT);
            else if (::fcntl(descriptor, command, &size) < 0) rawFail(errno);
            else rawOk(0);
            break;
        }
        case F_RDADVISE: {
            radvisory advice{};
            if (!netReadGuest(argument, &advice, sizeof(advice))) rawFail(EFAULT);
            else if (::fcntl(descriptor, command, &advice) < 0) rawFail(errno);
            else rawOk(0);
            break;
        }
        case F_LOG2PHYS: {
            log2phys map{};
            if (!netReadGuest(argument, &map, sizeof(map))) rawFail(EFAULT);
            else if (::fcntl(descriptor, command, &map) < 0) rawFail(errno);
            else if (!netWriteGuest(argument, &map, sizeof(map))) rawFail(EFAULT);
            else rawOk(0);
            break;
        }
        case F_PUNCHHOLE: {
            fpunchhole hole{};
            if (!netReadGuest(argument, &hole, sizeof(hole))) rawFail(EFAULT);
            else if (::fcntl(descriptor, command, &hole) < 0) rawFail(errno);
            else rawOk(0);
            break;
        }
        default:
            survey("fcntl", netSyscall, std::to_string(command));
            if (::fcntl(descriptor, command, 0) < 0) rawFail(errno);
            else rawOk(0);
            break;
        }
        continue;
    }

    if (netSyscall == 54) {  // ioctl
        constexpr std::uint64_t ioctlVoid = 0x20000000ULL, ioctlOut = 0x40000000ULL,
                                ioctlIn = 0x80000000ULL;
        const auto descriptor = static_cast<std::int32_t>(registers[0]);
        const auto request = static_cast<std::uint32_t>(registers[1]);
        const auto argument = registers[2];
        if (!netFdUsable(descriptor)) {
            rawFail(EBADF);
            continue;
        }
        const auto size = static_cast<std::size_t>((request >> 16) & 0x1fffU);
        const bool in = (request & ioctlIn) != 0;
        const bool out = (request & ioctlOut) != 0;
        if ((request & ioctlVoid) != 0 || (!in && !out)) {
            if (::ioctl(descriptor, request, static_cast<int>(argument)) < 0) rawFail(errno);
            else rawOk(0);
            continue;
        }
        if (size > 4096) {
            rawFail(EINVAL);
            continue;
        }
        std::vector<std::uint8_t> buffer(size);
        if (argument == 0) {
            rawFail(EFAULT);
            continue;
        }
        if (in && !netReadGuest(argument, buffer.data(), size)) {
            rawFail(EFAULT);
            continue;
        }
        const auto result = ::ioctl(descriptor, request, buffer.data());
        if (result < 0) rawFail(errno);
        else if (out && !netWriteGuest(argument, buffer.data(), size)) rawFail(EFAULT);
        else rawOk(static_cast<std::uint64_t>(result));
        continue;
    }
}
