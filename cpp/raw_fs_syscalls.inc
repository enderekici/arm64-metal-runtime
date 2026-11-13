#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
{
    constexpr std::size_t fsScratchLimit = std::size_t{1} << 20;
    const auto fsStandardOpen = [&](std::int32_t descriptor) {
        if (descriptor == STDIN_FILENO) return hostServiceState.closedStreams.count(bootstrap.stdinSlot) == 0;
        if (descriptor == STDOUT_FILENO) return hostServiceState.closedStreams.count(bootstrap.stdoutSlot) == 0;
        if (descriptor == STDERR_FILENO) return hostServiceState.closedStreams.count(bootstrap.stderrSlot) == 0;
        return false;
    };
    const auto fsOwns = [&](std::uint64_t value) {
        return rawOwnsDescriptor(value) || fsStandardOpen(static_cast<std::int32_t>(value));
    };
    const auto fsWritable = [&](std::uint64_t value) {
        const auto descriptor = static_cast<std::int32_t>(value);
        return ((descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO) &&
                fsStandardOpen(descriptor)) ||
               (rawOwnsDescriptor(value) &&
                hostServiceState.writableDescriptors.count(descriptor) != 0);
    };
    const auto fsCstr = [&](std::uint64_t address) -> std::optional<std::string> {
        if (address < bootstrap.pageSize) return std::nullopt;
        return rawPath(address);
    };
    const auto fsAt = [&](std::uint64_t directory,
                          std::uint64_t pathRegister) -> std::optional<std::string> {
        auto path = fsCstr(pathRegister);
        if (!path) {
            rawFail(EFAULT);
            return std::nullopt;
        }
        if (!((!path->empty() && path->front() == '/') ||
              static_cast<std::int32_t>(directory) == AT_FDCWD || rawOwnsDescriptor(directory))) {
            rawFail(EBADF);
            return std::nullopt;
        }
        return path;
    };
    const auto fsCopyOut = [&](std::uint64_t address, const void* data, std::size_t length) {
        if (length == 0) return true;
        if (address < bootstrap.pageSize) return false;
        try {
            std::memcpy(guestMemory.buffer(address, length, true), data, length);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    const auto fsCopyIn = [&](std::uint64_t address, std::size_t length)
        -> std::optional<std::vector<std::uint8_t>> {
        std::vector<std::uint8_t> bytes(length);
        if (length == 0) return bytes;
        if (address < bootstrap.pageSize) return std::nullopt;
        try {
            std::memcpy(bytes.data(), guestMemory.buffer(address, length), length);
            return bytes;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    const auto fsFinish = [&](std::int64_t result) {
        const auto error = errno;
        if (result < 0) rawFail(error);
        else rawOk(static_cast<std::uint64_t>(result));
    };
    const auto fsAdopt = [&](int descriptor, std::int32_t source, bool writable) {
        hostServiceState.openDescriptors.insert(descriptor);
        if (writable || (source >= 0 && fsWritable(static_cast<std::uint64_t>(source)))) {
            hostServiceState.writableDescriptors.insert(descriptor);
        }
    };
    const auto fsService = [&](std::uint64_t service) {
        auto* errnoBytes = guestMemory.buffer(bootstrap.errnoSlot, sizeof(std::uint32_t), true);
        std::uint32_t savedErrno = 0;
        std::memcpy(&savedErrno, errnoBytes, sizeof(savedErrno));
        auto callRegisters = std::vector<std::uint64_t>(registers, registers + 36);
        std::uint32_t error = 0;
        bool failed = false;
        try {
            (void)dispatch_guest_host_service(callRegisters, guestMemory, bootstrap,
                                              hostServiceState, service);
            if (callRegisters[0] == std::numeric_limits<std::uint64_t>::max()) {
                std::memcpy(&error, errnoBytes, sizeof(error));
                if (error == 0) error = EIO;
                failed = true;
            }
        } catch (const std::out_of_range&) {
            error = EFAULT;
            failed = true;
        }
        std::memcpy(errnoBytes, &savedErrno, sizeof(savedErrno));
        if (failed) rawFail(static_cast<int>(error));
        else rawOk(callRegisters[0]);
    };
    const auto fsSecurity = [&](std::uint64_t address, std::vector<std::uint8_t>& storage,
                                void*& argument) -> int {
        constexpr std::size_t headerBytes = 44;
        constexpr std::size_t entryBytes = 24;
        constexpr std::uint32_t maximumEntries = 128;
        argument = reinterpret_cast<void*>(address);
        if (address == 0 || address == ~std::uint64_t{0}) return 0;
        try {
            storage.resize(headerBytes);
            std::memcpy(storage.data(), guestMemory.buffer(address, headerBytes), headerBytes);
            std::uint32_t entries = 0;
            std::memcpy(&entries, storage.data() + 36, sizeof(entries));
            if (entries > maximumEntries && entries != ~std::uint32_t{0}) return EINVAL;
            if (entries != ~std::uint32_t{0} && entries != 0) {
                storage.resize(headerBytes + entries * entryBytes);
                std::memcpy(storage.data() + headerBytes,
                            guestMemory.buffer(address + headerBytes, entries * entryBytes),
                            entries * entryBytes);
            }
            argument = storage.data();
            return 0;
        } catch (const std::exception&) {
            return EFAULT;
        }
    };
    constexpr std::int32_t fsOpenFlagsAllowed = O_ACCMODE | O_APPEND | O_CREAT | O_TRUNC |
        O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_DIRECTORY |
        O_EXEC | O_NOFOLLOW_ANY | O_SYMLINK;
    constexpr std::size_t fsLegacyStatBytes = 120;
    constexpr std::size_t fsLegacyStatfsBytes = 336;

    switch (syscall) {
        case SYS_read:
        case SYS_write:
        case SYS_open:
            fsService(syscall == SYS_read ? 23U : syscall == SYS_write ? 2U : 21U);
            continue;
        case SYS_shm_open:
            fsService(105U);
            continue;
        case SYS_shm_unlink:
            fsService(106U);
            continue;
        case SYS_readv:
        case SYS_readv_nocancel:
        case SYS_writev:
        case SYS_writev_nocancel:
        case SYS_preadv:
        case SYS_preadv_nocancel:
        case SYS_pwritev:
        case SYS_pwritev_nocancel: {
            const bool writing = syscall == SYS_writev || syscall == SYS_writev_nocancel ||
                                 syscall == SYS_pwritev || syscall == SYS_pwritev_nocancel;
            const bool positional = syscall == SYS_preadv || syscall == SYS_preadv_nocancel ||
                                    syscall == SYS_pwritev || syscall == SYS_pwritev_nocancel;
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            const auto count = static_cast<std::int32_t>(registers[2]);
            if (count < 0 || count > 1024) {
                rawFail(EINVAL);
            } else if (writing ? !fsWritable(registers[0])
                               : (!(descriptor == STDIN_FILENO && fsStandardOpen(descriptor)) &&
                                  !rawOwnsDescriptor(registers[0]))) {
                rawFail(EBADF);
            } else if (!writing && hostServiceState.socketDescriptors.count(descriptor) != 0) {
                rawFail(ENOTSUP);
            } else {
                const auto raw = fsCopyIn(registers[1], static_cast<std::size_t>(count) * 16U);
                if (!raw) {
                    rawFail(EFAULT);
                    continue;
                }
                std::vector<struct iovec> vectors(static_cast<std::size_t>(count));
                std::uint64_t total = 0;
                bool bad = false;
                try {
                    for (std::size_t index = 0; index < vectors.size() && !bad; ++index) {
                        std::uint64_t base = 0;
                        std::uint64_t length = 0;
                        std::memcpy(&base, raw->data() + index * 16, 8);
                        std::memcpy(&length, raw->data() + index * 16 + 8, 8);
                        total += length;
                        if (length > 0x7fffffffU || total > 0x7fffffffU) {
                            rawFail(EINVAL);
                            bad = true;
                        } else if (length != 0 && base < bootstrap.pageSize) {
                            rawFail(EFAULT);
                            bad = true;
                        } else if (length != 0) {
                            vectors[index].iov_base = writing
                                ? const_cast<std::uint8_t*>(guestMemory.buffer(base, length))
                                : guestMemory.buffer(base, length, true);
                            vectors[index].iov_len = static_cast<std::size_t>(length);
                        }
                    }
                } catch (const std::exception&) {
                    rawFail(EFAULT);
                    bad = true;
                }
                if (bad) continue;
                const auto offset = static_cast<off_t>(registers[3]);
                ssize_t result;
                if (positional) {
                    result = writing ? ::pwritev(descriptor, vectors.data(), count, offset)
                                     : ::preadv(descriptor, vectors.data(), count, offset);
                } else {
                    result = writing ? ::writev(descriptor, vectors.data(), count)
                                     : ::readv(descriptor, vectors.data(), count);
                }
                fsFinish(result);
            }
            continue;
        }
        case SYS_pread_nocancel:
        case SYS_pwrite_nocancel: {
            const bool writing = syscall == SYS_pwrite_nocancel;
            const auto descriptor = static_cast<std::int32_t>(registers[0]);
            const auto count = static_cast<std::size_t>(registers[2]);
            if (!rawOwnsDescriptor(registers[0]) || (writing && !fsWritable(registers[0]))) {
                rawFail(EBADF);
            } else if (count > 0x7fffffffU) {
                rawFail(EINVAL);
            } else if (count != 0 && registers[1] < bootstrap.pageSize) {
                rawFail(EFAULT);
            } else {
                try {
                    std::vector<std::uint8_t> bytes(count);
                    ssize_t transferred;
                    if (!writing) {
                        transferred = ::pread(descriptor, bytes.data(), count,
                                              static_cast<off_t>(registers[3]));
                        if (transferred > 0) {
                            std::memcpy(guestMemory.buffer(registers[1],
                                            static_cast<std::size_t>(transferred), true),
                                        bytes.data(), static_cast<std::size_t>(transferred));
                        }
                    } else {
                        if (count != 0) {
                            std::memcpy(bytes.data(), guestMemory.buffer(registers[1], count), count);
                        }
                        transferred = ::pwrite(descriptor, bytes.data(), count,
                                               static_cast<off_t>(registers[3]));
                    }
                    fsFinish(transferred);
                } catch (const std::exception&) {
                    rawFail(EFAULT);
                }
            }
            continue;
        }
        case SYS_lseek:
            if (!fsOwns(registers[0])) rawFail(EBADF);
            else fsFinish(::lseek(static_cast<int>(registers[0]),
                                  static_cast<off_t>(registers[1]), static_cast<int>(registers[2])));
            continue;
        case SYS_truncate: {
            const auto path = fsCstr(registers[0]);
            if (!path) rawFail(EFAULT);
            else fsFinish(::truncate(path->c_str(), static_cast<off_t>(registers[1])));
            continue;
        }
        case SYS_ftruncate:
            if (!fsOwns(registers[0])) rawFail(EBADF);
            else fsFinish(::ftruncate(static_cast<int>(registers[0]),
                                      static_cast<off_t>(registers[1])));
            continue;
        case SYS_sync:
            ::sync();
            rawOk(0);
            continue;
        case SYS_fsync:
        case SYS_fsync_nocancel:
        case SYS_fdatasync:
            if (!fsOwns(registers[0])) rawFail(EBADF);
            else fsFinish(syscall == SYS_fdatasync
                              ? ::syscall(SYS_fdatasync, static_cast<int>(registers[0]))
                              : ::fsync(static_cast<int>(registers[0])));
            continue;
        case SYS_flock:
            if (!fsOwns(registers[0])) rawFail(EBADF);
            else fsFinish(::flock(static_cast<int>(registers[0]), static_cast<int>(registers[1])));
            continue;
        case SYS_dup: {
            const auto source = static_cast<std::int32_t>(registers[0]);
            if (!fsOwns(registers[0])) {
                rawFail(EBADF);
            } else if (hostServiceState.socketDescriptors.count(source) != 0) {
                rawFail(ENOTSUP);
            } else {
                const auto copy = ::dup(source);
                if (copy < 0) {
                    rawFail(errno);
                } else {
                    fsAdopt(copy, source, false);
                    rawOk(static_cast<std::uint64_t>(copy));
                }
            }
            continue;
        }
        case SYS_dup2:
        case SYS_dup3: {
            const bool withFlags = syscall == SYS_dup3;
            const auto source = static_cast<std::int32_t>(registers[0]);
            const auto target = static_cast<std::int32_t>(registers[1]);
            const auto flags = static_cast<std::int32_t>(registers[2]);
            const bool targetStandard = target >= STDIN_FILENO && target <= STDERR_FILENO;
            if (!fsOwns(registers[0]) || target < 0) {
                rawFail(EBADF);
            } else if (withFlags && ((flags & ~O_CLOEXEC) != 0 || source == target)) {
                rawFail(EINVAL);
            } else if (source == target) {
                rawOk(static_cast<std::uint64_t>(target));
            } else if (hostServiceState.socketDescriptors.count(source) != 0 ||
                       hostServiceState.socketDescriptors.count(target) != 0) {
                rawFail(ENOTSUP);
            } else if (hostServiceState.guestDirectoryDescriptors.count(target) != 0 ||
                       (!targetStandard && !rawOwnsDescriptor(registers[1]) &&
                        ::fcntl(target, F_GETFD) >= 0)) {
                rawFail(EBADF);
            } else {
                const bool writable = fsWritable(registers[0]);
                const auto result = ::dup2(source, target);
                if (result < 0) {
                    rawFail(errno);
                } else {
                    if (withFlags && (flags & O_CLOEXEC) != 0) ::fcntl(target, F_SETFD, FD_CLOEXEC);
                    hostServiceState.writableDescriptors.erase(target);
                    hostServiceState.standardDescriptorFlags.erase(target);
                    hostServiceState.standardStatusFlags.erase(target);
                    if (targetStandard) {
                        hostServiceState.closedStreams.erase(
                            target == STDIN_FILENO ? bootstrap.stdinSlot
                            : target == STDOUT_FILENO ? bootstrap.stdoutSlot
                                                      : bootstrap.stderrSlot);
                    } else {
                        fsAdopt(target, -1, writable);
                    }
                    rawOk(static_cast<std::uint64_t>(target));
                }
            }
            continue;
        }
        case SYS_pipe:
        case SYS_pipe2: {
            int descriptors[2] = {-1, -1};
            if (::pipe(descriptors) != 0) {
                rawFail(errno);
            } else {
                hostServiceState.openDescriptors.insert(descriptors[0]);
                hostServiceState.openDescriptors.insert(descriptors[1]);
                hostServiceState.writableDescriptors.insert(descriptors[1]);
                rawOk(static_cast<std::uint64_t>(descriptors[0]));
                registers[1] = static_cast<std::uint64_t>(descriptors[1]);
            }
            continue;
        }
        case SYS_link:
        case SYS_symlink: {
            const auto first = fsCstr(registers[0]);
            const auto second = fsCstr(registers[1]);
            if (!first || !second) rawFail(EFAULT);
            else fsFinish(syscall == SYS_link ? ::link(first->c_str(), second->c_str())
                                              : ::symlink(first->c_str(), second->c_str()));
            continue;
        }
        case SYS_readlink:
        case SYS_readlinkat:
        case SYS_freadlink: {
            const bool byDescriptor = syscall == SYS_freadlink;
            const bool relative = syscall == SYS_readlinkat;
            const auto bufferRegister = byDescriptor ? 1 : relative ? 2 : 1;
            const auto size = std::min<std::size_t>(
                static_cast<std::size_t>(registers[bufferRegister + 1]), fsScratchLimit);
            std::optional<std::string> path;
            if (byDescriptor) {
                if (!rawOwnsDescriptor(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
            } else {
                path = relative ? fsAt(registers[0], registers[1]) : fsCstr(registers[0]);
                if (!path) {
                    if (!relative) rawFail(EFAULT);
                    continue;
                }
            }
            std::vector<char> bytes(size);
            const ssize_t result = byDescriptor
                ? static_cast<ssize_t>(::syscall(SYS_freadlink, static_cast<int>(registers[0]),
                                                 bytes.data(), size))
                : relative ? ::readlinkat(static_cast<int>(registers[0]), path->c_str(),
                                          bytes.data(), size)
                           : ::readlink(path->c_str(), bytes.data(), size);
            if (result >= 0 &&
                !fsCopyOut(registers[bufferRegister], bytes.data(), static_cast<std::size_t>(result))) {
                rawFail(EFAULT);
            } else {
                fsFinish(result);
            }
            continue;
        }
        case SYS_mkfifo:
        case SYS_mkdir:
        case SYS_mknod: {
            const auto path = fsCstr(registers[0]);
            const auto mode = static_cast<mode_t>(registers[1]);
            if (!path) rawFail(EFAULT);
            else if (syscall == SYS_mkfifo) fsFinish(::mkfifo(path->c_str(), mode));
            else if (syscall == SYS_mkdir) fsFinish(::mkdir(path->c_str(), mode));
            else fsFinish(::mknod(path->c_str(), mode, static_cast<dev_t>(registers[2])));
            continue;
        }
        case SYS_utimes:
        case SYS_futimes: {
            struct timeval times[2];
            struct timeval* timesPointer = nullptr;
            if (registers[1] != 0) {
                const auto bytes = fsCopyIn(registers[1], sizeof(times));
                if (!bytes) {
                    rawFail(EFAULT);
                    continue;
                }
                std::memcpy(times, bytes->data(), sizeof(times));
                timesPointer = times;
            }
            if (syscall == SYS_futimes) {
                if (!fsOwns(registers[0])) rawFail(EBADF);
                else fsFinish(::futimes(static_cast<int>(registers[0]), timesPointer));
            } else {
                const auto path = fsCstr(registers[0]);
                if (!path) rawFail(EFAULT);
                else fsFinish(::utimes(path->c_str(), timesPointer));
            }
            continue;
        }
        case SYS_delete:
        case SYS_undelete: {
            const auto path = fsCstr(registers[0]);
            if (!path) rawFail(EFAULT);
            else fsFinish(::syscall(static_cast<int>(syscall), path->c_str()));
            continue;
        }
        case SYS_copyfile: {
            const auto from = fsCstr(registers[0]);
            const auto to = fsCstr(registers[1]);
            if (!from || !to) rawFail(EFAULT);
            else fsFinish(::syscall(SYS_copyfile, from->c_str(), to->c_str(),
                                    static_cast<int>(registers[2]), static_cast<int>(registers[3])));
            continue;
        }
        case SYS_exchangedata: {
            const auto first = fsCstr(registers[0]);
            const auto second = fsCstr(registers[1]);
            if (!first || !second) rawFail(EFAULT);
            else fsFinish(::syscall(SYS_exchangedata, first->c_str(), second->c_str(),
                                    static_cast<unsigned>(registers[2])));
            continue;
        }
        case SYS_pathconf:
        case SYS_fpathconf: {
            std::optional<std::string> path;
            if (syscall == SYS_fpathconf) {
                if (!fsOwns(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
            } else {
                path = fsCstr(registers[0]);
                if (!path) {
                    rawFail(EFAULT);
                    continue;
                }
            }
            errno = 0;
            const long result = syscall == SYS_fpathconf
                ? ::fpathconf(static_cast<int>(registers[0]), static_cast<int>(registers[1]))
                : ::pathconf(path->c_str(), static_cast<int>(registers[1]));
            if (result < 0 && errno != 0) rawFail(errno);
            else rawOk(static_cast<std::uint64_t>(result));
            continue;
        }
        case SYS_stat64:
        case SYS_fstat64: {
            struct stat result{};
            int status;
            if (syscall == SYS_fstat64) {
                if (!fsOwns(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
                status = ::fstat(static_cast<int>(registers[0]), &result);
            } else {
                const auto path = fsCstr(registers[0]);
                if (!path) {
                    rawFail(EFAULT);
                    continue;
                }
                status = ::stat(path->c_str(), &result);
            }
            if (status != 0) rawFail(errno);
            else if (!fsCopyOut(registers[1], &result, sizeof(result))) rawFail(EFAULT);
            else rawOk(0);
            continue;
        }
        case SYS_stat:
        case SYS_lstat:
        case SYS_fstat: {
            std::array<std::uint8_t, 256> legacy{};
            long status;
            if (syscall == SYS_fstat) {
                if (!fsOwns(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
                status = ::syscall(SYS_fstat, static_cast<int>(registers[0]), legacy.data());
            } else {
                const auto path = fsCstr(registers[0]);
                if (!path) {
                    rawFail(EFAULT);
                    continue;
                }
                status = ::syscall(static_cast<int>(syscall), path->c_str(), legacy.data());
            }
            if (status != 0) rawFail(errno);
            else if (!fsCopyOut(registers[1], legacy.data(), fsLegacyStatBytes)) rawFail(EFAULT);
            else rawOk(0);
            continue;
        }
        case SYS_fstatat:
        case SYS_fstatat64: {
            const auto path = fsAt(registers[0], registers[1]);
            if (!path) continue;
            const auto directory = static_cast<int>(registers[0]);
            const auto flags = static_cast<int>(registers[3]);
            if (syscall == SYS_fstatat64) {
                struct stat result{};
                if (::fstatat(directory, path->c_str(), &result, flags) != 0) rawFail(errno);
                else if (!fsCopyOut(registers[2], &result, sizeof(result))) rawFail(EFAULT);
                else rawOk(0);
            } else {
                std::array<std::uint8_t, 256> legacy{};
                if (::syscall(SYS_fstatat, directory, path->c_str(), legacy.data(), flags) != 0) {
                    rawFail(errno);
                } else if (!fsCopyOut(registers[2], legacy.data(), fsLegacyStatBytes)) {
                    rawFail(EFAULT);
                } else {
                    rawOk(0);
                }
            }
            continue;
        }
        case SYS_stat_extended:
        case SYS_lstat_extended:
        case SYS_fstat_extended: {
            const bool byDescriptor = syscall == SYS_fstat_extended;
            std::optional<std::string> path;
            if (byDescriptor) {
                if (!fsOwns(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
            } else {
                path = fsCstr(registers[0]);
                if (!path) {
                    rawFail(EFAULT);
                    continue;
                }
            }
            try {
                std::size_t guestSecuritySize = 0;
                if (registers[2] != 0 && registers[3] != 0) {
                    std::memcpy(&guestSecuritySize,
                                guestMemory.buffer(registers[3], sizeof(guestSecuritySize)),
                                sizeof(guestSecuritySize));
                }
                std::vector<std::uint8_t> security(registers[2] != 0 ? guestSecuritySize : 0);
                std::size_t hostSecuritySize = guestSecuritySize;
                std::array<std::uint8_t, 256> legacy{};
                const auto status = ::syscall(
                    static_cast<int>(syscall),
                    byDescriptor ? reinterpret_cast<void*>(registers[0])
                                 : const_cast<char*>(path->c_str()),
                    legacy.data(), registers[2] != 0 ? security.data() : nullptr,
                    registers[3] != 0 ? &hostSecuritySize : nullptr);
                if (status != 0) {
                    rawFail(errno);
                    continue;
                }
                if (!fsCopyOut(registers[1], legacy.data(), fsLegacyStatBytes) ||
                    (registers[3] != 0 &&
                     !fsCopyOut(registers[3], &hostSecuritySize, sizeof(hostSecuritySize))) ||
                    (registers[2] != 0 && hostSecuritySize != 0 &&
                     hostSecuritySize <= security.size() &&
                     !fsCopyOut(registers[2], security.data(), hostSecuritySize))) {
                    rawFail(EFAULT);
                    continue;
                }
                rawOk(0);
            } catch (const std::exception&) {
                rawFail(EFAULT);
            }
            continue;
        }
        case SYS_statfs:
        case SYS_fstatfs: {
            std::array<std::uint8_t, 512> legacy{};
            long status;
            if (syscall == SYS_fstatfs) {
                if (!fsOwns(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
                status = ::syscall(SYS_fstatfs, static_cast<int>(registers[0]), legacy.data());
            } else {
                const auto path = fsCstr(registers[0]);
                if (!path) {
                    rawFail(EFAULT);
                    continue;
                }
                status = ::syscall(SYS_statfs, path->c_str(), legacy.data());
            }
            if (status != 0) rawFail(errno);
            else if (!fsCopyOut(registers[1], legacy.data(), fsLegacyStatfsBytes)) rawFail(EFAULT);
            else rawOk(0);
            continue;
        }
        case SYS_getfsstat:
        case SYS_getfsstat64: {
            const bool legacy = syscall == SYS_getfsstat;
            const std::size_t entryBytes = legacy ? fsLegacyStatfsBytes : sizeof(struct statfs);
            if (registers[0] == 0) {
                if (legacy) fsFinish(::syscall(SYS_getfsstat, nullptr, 0, static_cast<int>(registers[2])));
                else fsFinish(::getfsstat(nullptr, 0, static_cast<int>(registers[2])));
                continue;
            }
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(registers[1]),
                                                    fsScratchLimit);
            std::vector<std::uint8_t> bytes(size);
            const long result = legacy
                ? ::syscall(SYS_getfsstat, bytes.data(), static_cast<int>(size),
                            static_cast<int>(registers[2]))
                : ::getfsstat(reinterpret_cast<struct statfs*>(bytes.data()),
                              static_cast<int>(size), static_cast<int>(registers[2]));
            if (result >= 0 && !fsCopyOut(registers[0], bytes.data(),
                                          std::min(static_cast<std::size_t>(result) * entryBytes,
                                                   size))) {
                rawFail(EFAULT);
            } else {
                fsFinish(result);
            }
            continue;
        }
        case SYS_getdirentries:
        case SYS_getdirentries64: {
            if (!rawOwnsDescriptor(registers[0])) {
                rawFail(EBADF);
                continue;
            }
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(registers[2]),
                                                    fsScratchLimit);
            std::vector<std::uint8_t> bytes(size);
            long base = 0;
            const long result = ::syscall(static_cast<int>(syscall), static_cast<int>(registers[0]),
                                          bytes.data(), size, &base);
            if (result >= 0 &&
                (!fsCopyOut(registers[1], bytes.data(), static_cast<std::size_t>(result)) ||
                 (registers[3] != 0 && !fsCopyOut(registers[3], &base, sizeof(base))))) {
                rawFail(EFAULT);
            } else {
                fsFinish(result);
            }
            continue;
        }
        case SYS_getdirentriesattr: {
            if (!rawOwnsDescriptor(registers[0])) {
                rawFail(EBADF);
                continue;
            }
            const auto list = fsCopyIn(registers[1], sizeof(struct attrlist));
            const auto countIn = registers[4] != 0 ? fsCopyIn(registers[4], 4) : std::nullopt;
            if (!list || (registers[4] != 0 && !countIn)) {
                rawFail(EFAULT);
                continue;
            }
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(registers[3]),
                                                    fsScratchLimit);
            std::vector<std::uint8_t> bytes(size);
            std::uint32_t count = 0;
            if (countIn) std::memcpy(&count, countIn->data(), 4);
            unsigned long base = 0;
            std::uint32_t state = 0;
            const long result = ::syscall(SYS_getdirentriesattr, static_cast<int>(registers[0]),
                                          list->data(), bytes.data(), size, &count, &base, &state,
                                          static_cast<unsigned>(registers[7]));
            if (result >= 0 &&
                (!fsCopyOut(registers[2], bytes.data(), size) ||
                 (registers[4] != 0 && !fsCopyOut(registers[4], &count, 4)) ||
                 (registers[5] != 0 && !fsCopyOut(registers[5], &base, sizeof(base))) ||
                 (registers[6] != 0 && !fsCopyOut(registers[6], &state, 4)))) {
                rawFail(EFAULT);
            } else {
                fsFinish(result);
            }
            continue;
        }
        case SYS_fgetattrlist:
        case SYS_getattrlistat:
        case SYS_getattrlistbulk: {
            const bool at = syscall == SYS_getattrlistat;
            const std::size_t first = at ? 2 : 1;
            std::optional<std::string> path;
            if (at) {
                path = fsAt(registers[0], registers[1]);
                if (!path) continue;
            } else if (!(syscall == SYS_fgetattrlist ? fsOwns(registers[0])
                                                     : rawOwnsDescriptor(registers[0]))) {
                rawFail(EBADF);
                continue;
            }
            const auto list = fsCopyIn(registers[first], sizeof(struct attrlist));
            if (!list) {
                rawFail(EFAULT);
                continue;
            }
            struct attrlist attributes{};
            std::memcpy(&attributes, list->data(), sizeof(attributes));
            const auto size = static_cast<std::size_t>(registers[first + 2]);
            if (size > (std::size_t{32} << 20)) {
                rawFail(EINVAL);
                continue;
            }
            std::vector<std::uint8_t> bytes(size);
            const auto options = registers[first + 3];
            long result;
            if (syscall == SYS_fgetattrlist) {
                result = ::fgetattrlist(static_cast<int>(registers[0]), &attributes, bytes.data(),
                                        size, static_cast<unsigned long>(options));
            } else if (at) {
                result = ::getattrlistat(static_cast<int>(registers[0]), path->c_str(), &attributes,
                                         bytes.data(), size, static_cast<unsigned long>(options));
            } else {
                result = ::getattrlistbulk(static_cast<int>(registers[0]), &attributes,
                                           bytes.data(), size, options);
            }
            if (result >= 0 && !fsCopyOut(registers[first + 1], bytes.data(), size)) {
                rawFail(EFAULT);
            } else {
                fsFinish(result);
            }
            continue;
        }
        case SYS_setattrlist:
        case SYS_fsetattrlist:
        case SYS_setattrlistat: {
            const bool at = syscall == SYS_setattrlistat;
            const bool byDescriptor = syscall == SYS_fsetattrlist;
            const std::size_t first = at ? 2 : 1;
            std::optional<std::string> path;
            if (at) {
                path = fsAt(registers[0], registers[1]);
                if (!path) continue;
            } else if (byDescriptor) {
                if (!fsWritable(registers[0])) {
                    rawFail(EBADF);
                    continue;
                }
            } else {
                path = fsCstr(registers[0]);
                if (!path) {
                    rawFail(EFAULT);
                    continue;
                }
            }
            const auto list = fsCopyIn(registers[first], sizeof(struct attrlist));
            if (!list) {
                rawFail(EFAULT);
                continue;
            }
            struct attrlist attributes{};
            std::memcpy(&attributes, list->data(), sizeof(attributes));
            constexpr attrgroup_t permittedCommon =
                ATTR_CMN_FNDRINFO | ATTR_CMN_MODTIME | ATTR_CMN_ACCTIME;
            const auto size = static_cast<std::size_t>(registers[first + 2]);
            const auto options = registers[first + 3];
            if (attributes.bitmapcount != ATTR_BIT_MAP_COUNT || attributes.commonattr == 0 ||
                (attributes.commonattr & ~permittedCommon) != 0 || attributes.volattr != 0 ||
                attributes.dirattr != 0 || attributes.fileattr != 0 || attributes.forkattr != 0 ||
                (options & ~static_cast<std::uint64_t>(FSOPT_NOFOLLOW)) != 0 ||
                (!at && options != 0)) {
                rawFail(ENOTSUP);
                continue;
            }
            if (size > fsScratchLimit) {
                rawFail(EINVAL);
                continue;
            }
            auto bytes = fsCopyIn(registers[first + 1], size);
            if (!bytes) {
                rawFail(EFAULT);
                continue;
            }
            if (byDescriptor) {
                fsFinish(::fsetattrlist(static_cast<int>(registers[0]), &attributes, bytes->data(),
                                        size, 0));
            } else if (at) {
                fsFinish(::syscall(SYS_setattrlistat, static_cast<int>(registers[0]), path->c_str(),
                                   &attributes, bytes->data(), size,
                                   static_cast<unsigned>(options)));
            } else {
                fsFinish(::setattrlist(path->c_str(), &attributes, bytes->data(), size, 0));
            }
            continue;
        }
        case SYS_renameat:
        case SYS_renameatx_np: {
            const auto from = fsAt(registers[0], registers[1]);
            if (!from) continue;
            const auto to = fsAt(registers[2], registers[3]);
            if (!to) continue;
            if (syscall == SYS_renameat) {
                fsFinish(::renameat(static_cast<int>(registers[0]), from->c_str(),
                                    static_cast<int>(registers[2]), to->c_str()));
            } else {
                fsFinish(::renameatx_np(static_cast<int>(registers[0]), from->c_str(),
                                        static_cast<int>(registers[2]), to->c_str(),
                                        static_cast<unsigned>(registers[4])));
            }
            continue;
        }
        case SYS_faccessat:
        case SYS_fchmodat:
        case SYS_fchownat:
        case SYS_unlinkat:
        case SYS_mkdirat:
        case SYS_mkfifoat:
        case SYS_mknodat:
        case SYS_fchflagsat: {
            const auto path = fsAt(registers[0], registers[1]);
            if (!path) continue;
            const auto directory = static_cast<int>(registers[0]);
            switch (syscall) {
                case SYS_faccessat:
                    fsFinish(::faccessat(directory, path->c_str(), static_cast<int>(registers[2]),
                                         static_cast<int>(registers[3])));
                    break;
                case SYS_fchmodat:
                    fsFinish(::fchmodat(directory, path->c_str(), static_cast<mode_t>(registers[2]),
                                        static_cast<int>(registers[3])));
                    break;
                case SYS_fchownat:
                    fsFinish(::fchownat(directory, path->c_str(), static_cast<uid_t>(registers[2]),
                                        static_cast<gid_t>(registers[3]),
                                        static_cast<int>(registers[4])));
                    break;
                case SYS_unlinkat:
                    fsFinish(::unlinkat(directory, path->c_str(), static_cast<int>(registers[2])));
                    break;
                case SYS_mkdirat:
                    fsFinish(::mkdirat(directory, path->c_str(), static_cast<mode_t>(registers[2])));
                    break;
                case SYS_mkfifoat:
                    fsFinish(::syscall(SYS_mkfifoat, directory, path->c_str(),
                                       static_cast<int>(registers[2])));
                    break;
                case SYS_mknodat:
                    fsFinish(::syscall(SYS_mknodat, directory, path->c_str(),
                                       static_cast<int>(registers[2]),
                                       static_cast<int>(registers[3])));
                    break;
                default:
                    fsFinish(::syscall(SYS_fchflagsat, directory, path->c_str(),
                                       static_cast<unsigned>(registers[2]),
                                       static_cast<int>(registers[3])));
                    break;
            }
            continue;
        }
        case SYS_linkat:
        case SYS_clonefileat: {
            const auto from = fsAt(registers[0], registers[1]);
            if (!from) continue;
            const auto to = fsAt(registers[2], registers[3]);
            if (!to) continue;
            if (syscall == SYS_linkat) {
                fsFinish(::linkat(static_cast<int>(registers[0]), from->c_str(),
                                  static_cast<int>(registers[2]), to->c_str(),
                                  static_cast<int>(registers[4])));
            } else {
                fsFinish(::clonefileat(static_cast<int>(registers[0]), from->c_str(),
                                       static_cast<int>(registers[2]), to->c_str(),
                                       static_cast<uint32_t>(registers[4])));
            }
            continue;
        }
        case SYS_fclonefileat: {
            if (!fsOwns(registers[0])) {
                rawFail(EBADF);
                continue;
            }
            const auto to = fsAt(registers[1], registers[2]);
            if (!to) continue;
            fsFinish(::fclonefileat(static_cast<int>(registers[0]), static_cast<int>(registers[1]),
                                    to->c_str(), static_cast<uint32_t>(registers[3])));
            continue;
        }
        case SYS_symlinkat: {
            const auto target = fsCstr(registers[0]);
            if (!target) {
                rawFail(EFAULT);
                continue;
            }
            const auto path = fsAt(registers[1], registers[2]);
            if (!path) continue;
            fsFinish(::symlinkat(target->c_str(), static_cast<int>(registers[1]), path->c_str()));
            continue;
        }
        case SYS_open_extended: {
            const auto path = fsCstr(registers[0]);
            const auto flags = static_cast<std::int32_t>(registers[1]);
            std::vector<std::uint8_t> storage;
            void* security = nullptr;
            if (!path) {
                rawFail(EFAULT);
            } else if ((flags & ~fsOpenFlagsAllowed) != 0) {
                rawFail(EINVAL);
            } else if (const auto error = fsSecurity(registers[5], storage, security); error != 0) {
                rawFail(error);
            } else {
                const auto descriptor = static_cast<int>(::syscall(
                    SYS_open_extended, path->c_str(), flags | O_CLOEXEC,
                    static_cast<unsigned>(registers[2]), static_cast<unsigned>(registers[3]),
                    static_cast<int>(registers[4]), security));
                if (descriptor < 0) {
                    rawFail(errno);
                } else {
                    fsAdopt(descriptor, -1, (flags & O_ACCMODE) != O_RDONLY);
                    rawOk(static_cast<std::uint64_t>(descriptor));
                }
            }
            continue;
        }
        case SYS_mkfifo_extended:
        case SYS_mkdir_extended: {
            const auto path = fsCstr(registers[0]);
            std::vector<std::uint8_t> storage;
            void* security = nullptr;
            if (!path) {
                rawFail(EFAULT);
            } else if (const auto error = fsSecurity(registers[4], storage, security); error != 0) {
                rawFail(error);
            } else {
                fsFinish(::syscall(static_cast<int>(syscall), path->c_str(),
                                   static_cast<unsigned>(registers[1]),
                                   static_cast<unsigned>(registers[2]),
                                   static_cast<int>(registers[3]), security));
            }
            continue;
        }
        case SYS_umask_extended:
            if (registers[1] != 0) {
                rawFail(ENOTSUP);
            } else {
                rawOk(static_cast<std::uint64_t>(
                    guestUmask.set(static_cast<mode_t>(registers[0]))));
            }
            continue;
        case SYS_getumask: {
            const auto mask = ::umask(0);
            ::umask(mask);
            rawOk(static_cast<std::uint64_t>(mask));
            continue;
        }
        case SYS_access_extended: {
            const auto size = static_cast<std::size_t>(registers[1]);
            if (size < 16 || size > 65536) {
                rawFail(EINVAL);
                continue;
            }
            auto entries = fsCopyIn(registers[0], size);
            if (!entries) {
                rawFail(EFAULT);
                continue;
            }
            std::uint32_t firstOffset = 0;
            std::memcpy(&firstOffset, entries->data(), sizeof(firstOffset));
            const std::size_t count = std::min<std::size_t>(firstOffset / 16U, size / 16U);
            std::vector<int> results(size / 16U + 16U);
            const auto status = ::syscall(SYS_access_extended, entries->data(), size, results.data(),
                                          static_cast<unsigned>(registers[3]));
            if (status != 0) rawFail(errno);
            else if (!fsCopyOut(registers[2], results.data(), count * sizeof(int))) rawFail(EFAULT);
            else rawOk(0);
            continue;
        }
        case SYS_fsgetpath:
        case SYS_fsgetpath_ext: {
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(registers[1]),
                                                    fsScratchLimit);
            const auto identifier = fsCopyIn(registers[2], sizeof(fsid_t));
            if (!identifier) {
                rawFail(EFAULT);
                continue;
            }
            std::vector<char> bytes(size);
            const long result = syscall == SYS_fsgetpath
                ? ::syscall(SYS_fsgetpath, bytes.data(), size, identifier->data(), registers[3])
                : ::syscall(SYS_fsgetpath_ext, bytes.data(), size, identifier->data(), registers[3],
                            registers[4]);
            if (result >= 0 && !fsCopyOut(registers[0], bytes.data(), static_cast<std::size_t>(result))) {
                rawFail(EFAULT);
            } else {
                fsFinish(result);
            }
            continue;
        }
        case SYS_openbyid_np: {
            const auto identifier = fsCopyIn(registers[0], sizeof(fsid_t));
            const auto object = fsCopyIn(registers[1], sizeof(std::uint64_t));
            const auto flags = static_cast<std::int32_t>(registers[2]);
            if (!identifier || !object) {
                rawFail(EFAULT);
            } else if ((flags & ~fsOpenFlagsAllowed) != 0) {
                rawFail(EINVAL);
            } else {
                const auto descriptor = static_cast<int>(::syscall(
                    SYS_openbyid_np, identifier->data(), object->data(), flags | O_CLOEXEC));
                if (descriptor < 0) {
                    rawFail(errno);
                } else {
                    fsAdopt(descriptor, -1, (flags & O_ACCMODE) != O_RDONLY);
                    rawOk(static_cast<std::uint64_t>(descriptor));
                }
            }
            continue;
        }
        case SYS_revoke: {
            const auto path = fsCstr(registers[0]);
            struct stat info{};
            if (!path) rawFail(EFAULT);
            else if (::stat(path->c_str(), &info) != 0) rawFail(errno);
            else if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) rawFail(EPERM);
            else fsFinish(::revoke(path->c_str()));
            continue;
        }
        case SYS_chroot:
        case SYS_mount:
        case SYS_unmount:
        case SYS_funmount:
        case SYS_fmount:
        case SYS_vfs_purge:
        case SYS_fs_snapshot:
        case SYS_graftdmg:
        case SYS_ungraftdmg:
        case SYS_pivot_root:
            rawFail(EPERM);
            continue;
        case SYS_fsctl:
        case SYS_ffsctl:
        case SYS_fsctlat:
            rawFail(ENOTSUP);
            continue;
        default:
            break;
    }
}
#pragma clang diagnostic pop
