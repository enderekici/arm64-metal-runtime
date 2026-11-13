struct GuestHostPollResult {
    std::uint64_t x0 = 0;
    bool carry = false;
};
using GuestHostPollFn = std::function<std::optional<GuestHostPollResult>(std::uint64_t now)>;
std::map<std::uint64_t, GuestHostPollFn> guestHostPollers;
const auto pollGuestHostWaiters = [&](std::uint64_t now) {
    for (auto poller = guestHostPollers.begin(); poller != guestHostPollers.end();) {
        const auto id = poller->first;
        auto* thread = guestScheduler.find(id);
        auto* wait = thread != nullptr && thread->wait.has_value()
            ? std::get_if<GuestHostPollWait>(&*thread->wait) : nullptr;
        if (wait == nullptr) {
            poller = guestHostPollers.erase(poller);
            continue;
        }
        if (now < wait->nextPoll) {
            ++poller;
            continue;
        }
        const auto result = poller->second(now);
        if (!result) {
            wait->nextPoll = now + 500'000ULL;
            ++poller;
            continue;
        }
        poller = guestHostPollers.erase(poller);
        guestScheduler.wake(id, 0);
        thread->registers[0] = result->x0;
        if (result->carry) {
            thread->registers[32] |= 8U;
            const auto error = static_cast<std::uint32_t>(result->x0);
            std::memcpy(guestMemory.buffer(static_cast<std::uint64_t>(thread->errnoAddress),
                                           sizeof(error), true), &error, sizeof(error));
        } else {
            thread->registers[32] &= ~std::uint64_t{8};
        }
    }
};
const auto netFdUsable = [&](std::int32_t descriptor) {
    if (descriptor == STDIN_FILENO)
        return hostServiceState.closedStreams.count(bootstrap.stdinSlot) == 0;
    if (descriptor == STDOUT_FILENO)
        return hostServiceState.closedStreams.count(bootstrap.stdoutSlot) == 0;
    if (descriptor == STDERR_FILENO)
        return hostServiceState.closedStreams.count(bootstrap.stderrSlot) == 0;
    return hostServiceState.openDescriptors.count(descriptor) != 0 ||
           hostServiceState.guestDirectoryDescriptors.count(descriptor) != 0;
};
// Readiness of a pollfd set against host descriptors, with guest-modeled AF_UNIX
// datagram sockets answered from their guest receive queues. Returns the number
// of entries with revents set.
const auto netPollDescriptors = [&](std::vector<pollfd>& entries) {
    std::vector<pollfd> hostEntries;
    std::vector<std::size_t> hostIndex;
    int ready = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto& entry = entries[index];
        entry.revents = 0;
        if (entry.fd < 0) continue;
        if (!netFdUsable(entry.fd)) {
            entry.revents = POLLNVAL;
            ++ready;
            continue;
        }
        const auto modeled = hostServiceState.guestUnixSockets.find(entry.fd);
        if (modeled != hostServiceState.guestUnixSockets.end() &&
            modeled->second.type == SOCK_DGRAM) {
            if (!modeled->second.receiveQueue.empty())
                entry.revents |= entry.events & (POLLIN | POLLRDNORM);
            entry.revents |= entry.events & (POLLOUT | POLLWRNORM);
            if (entry.revents != 0) ++ready;
            continue;
        }
        hostEntries.push_back(entry);
        hostIndex.push_back(index);
    }
    if (!hostEntries.empty()) {
        const auto result = ::poll(hostEntries.data(),
                                   static_cast<nfds_t>(hostEntries.size()), 0);
        if (result > 0) {
            for (std::size_t item = 0; item < hostEntries.size(); ++item) {
                entries[hostIndex[item]].revents = hostEntries[item].revents;
                if (hostEntries[item].revents != 0) ++ready;
            }
        }
    }
    return ready;
};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// Zero-timeout host kevent on a guest kqueue; kind 0 = kevent, 1 = kevent64,
// 2 = kevent_qos. Never blocks the host thread: waiting is done by the scheduler.
const auto netHostKevent = [](int kind, int kq, const std::vector<std::uint8_t>& changes,
                              int changeCount, std::vector<std::uint8_t>& events,
                              int eventCount, std::uint32_t flags) -> int {
    const timespec zero{0, 0};
    if (kind == 0) {
        return ::kevent(kq, reinterpret_cast<const struct kevent*>(changes.data()), changeCount,
                        reinterpret_cast<struct kevent*>(events.data()), eventCount, &zero);
    }
    if (kind == 1) {
        return ::kevent64(kq, reinterpret_cast<const struct kevent64_s*>(changes.data()),
                          changeCount, reinterpret_cast<struct kevent64_s*>(events.data()),
                          eventCount, flags, &zero);
    }
    constexpr std::uint32_t immediate = 0x1U;
    return static_cast<int>(::syscall(SYS_kevent_qos, kq, changes.data(), changeCount,
                                      events.data(), eventCount, nullptr, nullptr,
                                      flags | immediate));
};
#pragma clang diagnostic pop
