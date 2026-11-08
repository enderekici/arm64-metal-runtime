import Darwin
import Foundation
import Metal
import Security

private func guestSigningPolicy(path: String, legacyRestriction: Bool) -> (restricted: Bool, status: UInt32) {
    let startupRestricted = legacyRestriction || Darwin.issetugid() != 0
    var code: SecStaticCode?
    guard SecStaticCodeCreateWithPath(URL(fileURLWithPath: path) as CFURL, [], &code) == errSecSuccess,
          let code else { return (startupRestricted, 0) }
    var information: CFDictionary?
    guard SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation),
                                        &information) == errSecSuccess,
          let signing = information as? [String: Any] else { return (startupRestricted, 0) }
    let entitlements = signing[kSecCodeInfoEntitlementsDict as String] as? [String: Any]
    let flags = signing[kSecCodeInfoFlags as String] as? UInt32 ?? 0
    let platform = signing["platform-identifier"] as? UInt32 ?? 0
    let allowDyldEnvironment = entitlements?["com.apple.security.cs.allow-dyld-environment-variables"] as? Bool == true
    let restricted = startupRestricted || (!allowDyldEnvironment && (flags & 0x10800 != 0 || platform != 0))
    // Model exec's signature status while keeping the guest identity separate from the launcher.
    var status: UInt32 = 0x0200_0000 // CS_NO_UNTRUSTED_HELPERS
    if signing[kSecCodeInfoUnique as String] != nil {
        status |= 0x2000_0000 // CS_SIGNED
        if SecStaticCodeCheckValidity(code, [], nil) == errSecSuccess { status |= 1 } // CS_VALID
        status |= flags & 0x0003_3f00
        status |= 0x200 // CS_KILL on Apple silicon
        if flags & 0x10000 != 0 {
            status |= 0x1100 // CS_HARD and CS_ENFORCEMENT
            if entitlements?["com.apple.security.cs.disable-library-validation"] as? Bool != true {
                status = (status & ~0x2000) | 0x10 // CS_FORCED_LV
            }
        }
        if platform != 0 { status |= 0x0400_0000 } // CS_PLATFORM_BINARY
    }
    return (restricted, status)
}

func captureEnvironment() throws -> [String] {
    guard let process = dlopen(nil, RTLD_LAZY), let symbol = dlsym(process, "environ") else {
        throw GuestError("Host environment unavailable")
    }
    defer { dlclose(process) }
    typealias Environment = UnsafePointer<UnsafePointer<CChar>?>
    var result = [String]()
    if let entries = symbol.assumingMemoryBound(to: Environment?.self).pointee {
        var index = 0
        while let entry = entries[index] {
            result.append(String(cString: entry))
            index += 1
        }
    }
    return result
}

func launchExecutable(path: String, arguments: [String], environment: [String]) throws -> Int32 {
    let image = try ExecutableImage(URL(fileURLWithPath: path),
                                    sharedCache: environment.contains("ARM64_METAL_SHARED_CACHE=1"),
                                    demandCache: environment.contains("ARM64_METAL_CACHE_DEMAND=1"))
    let trace = environment.contains("ARM64_METAL_TRACE=1")
    let signing = guestSigningPolicy(path: path, legacyRestriction: image.mainRestricted)
    let imageHeaders = Dictionary(image.imagePaths.map { ($0.value, $0.key) },
                                  uniquingKeysWith: min)
    var openImageReferences = [UInt64: Int]()
    var startup = image.initializers + [image.entry]
    if environment.contains("ARM64_METAL_CACHE_BOOTSTRAP=1"), let objcBootstrap = image.objcBootstrap {
        startup.insert(objcBootstrap, at: 0)
        if trace {
            let line = "Running cached _objc_init on Metal at 0x\(String(objcBootstrap, radix: 16))\n"
            FileHandle.standardError.write(Data(line.utf8))
        }
    }
    guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
        throw GuestError("Metal compute unavailable")
    }
    struct Region {
        let pipeline: MTLComputePipelineState
        let faults: [Int: GuestError]
    }
    var regions = [Int: Region]()
    var regionEntries = [[Int]](repeating: [], count: 1024)
    var nextRegionSlot = 0
    func compile(at entry: Int) throws -> Region {
        guard entry >= 0, UInt64(entry) < guestCodeLimit else {
            throw GuestError("Guest instruction address outside the process window")
        }
        if let cached = regions[entry] { return cached }
        let translation = try ProcessTranslation(image, entry: entry)
        let library = try device.makeLibrary(source: translation.source(), options: nil)
        guard let function = library.makeFunction(name: "run_process") else {
            throw GuestError("Generated process kernel unavailable")
        }
        let region = Region(pipeline: try device.makeComputePipelineState(function: function),
                            faults: translation.faults)
        // Frames holding a Region keep its pipeline alive after lookup entries are evicted.
        let evicted = regionEntries[nextRegionSlot]
        if trace, nextRegionSlot == 0, !evicted.isEmpty {
            FileHandle.standardError.write(Data("Guest Metal region cache evicting at 1024 retained pipelines\n".utf8))
        }
        for address in evicted { regions.removeValue(forKey: address) }
        let entries = translation.entries.filter { regions[$0] == nil }
        for address in entries { regions[address] = region }
        regionEntries[nextRegionSlot] = Array(entries)
        nextRegionSlot = (nextRegionSlot + 1) % regionEntries.count
        return region
    }
    var region = try compile(at: startup[0])

    let stackBase = image.mappingBackingEnd
    let stackBytes = 8 * 1024 * 1024
    var data = image.memory
    let stackEnd = stackBase + stackBytes
    let heapSize = 64 * 1024 * 1024
    data.count = stackEnd + heapSize
    let argv = [path] + arguments
    // Darwin exposes the launch spelling on macOS, including symlinks.
    let executablePath = path.hasPrefix("/") ? path :
        FileManager.default.currentDirectoryPath + "/" + (path.hasPrefix("./") ? String(path.dropFirst(2)) : path)
    let executablePathBytes = Array(executablePath.utf8) + [0]
    var cursor = stackBase
    func putString(_ string: String) throws -> UInt64 {
        let bytes = Array(string.utf8) + [0]
        guard bytes.count <= data.count - cursor else { throw GuestError("Guest strings exceed stack space") }
        let pointer = UInt64(cursor)
        data.replaceSubrange(cursor..<(cursor + bytes.count), with: bytes)
        cursor += bytes.count
        return pointer
    }
    func store(_ value: UInt64, at address: Int) {
        var little = value.littleEndian
        withUnsafeBytes(of: &little) { data.replaceSubrange(address..<(address + 8), with: $0) }
    }
    let argumentsVector = try argv.map(putString)
    let programName = try putString(String(path.split(separator: "/").last ?? ""))
    var environmentPointers = [String: UInt64]()
    var environmentVector = [UInt64]()
    for string in environment {
        let address = try putString(string)
        environmentVector.append(address)
        if let equals = string.firstIndex(of: "=") {
            let name = String(string[..<equals])
            if environmentPointers[name] == nil {
                environmentPointers[name] = address + UInt64(name.utf8.count) + 1
            }
        }
    }
    cursor = (cursor + 7) & ~7
    let vector = cursor
    let envVector = vector + (argumentsVector.count + 1) * 8
    let empty = envVector + (environmentVector.count + 1) * 8
    let stdoutSlot = empty + 8
    let localeSlot = stdoutSlot + 8
    let stderrSlot = localeSlot + 8
    let optindSlot = stderrSlot + 8
    let environSlot = optindSlot + 8
    let optargSlot = environSlot + 8
    let canarySlot = optargSlot + 8
    let runeLocaleSlot = canarySlot + 8
    let mainErrno = runeLocaleSlot + MemoryLayout<_RuneLocale>.stride
    let stdinSlot = mainErrno + 8
    let vmPageSizeSlot = stdinSlot + 8
    let vmPageShiftSlot = vmPageSizeSlot + 8
    let vmKernelPageSizeSlot = vmPageShiftSlot + 8
    let taskPortSlot = vmKernelPageSizeSlot + 8
    let guestTaskPort: UInt64 = 0x103
    let guestLocaleKey: UInt64 = 10 // Darwin's reserved __LIBC_PTHREAD_KEY_XLOCALE
    let cachedLocaleKey = try image.cacheReader?.localeKeyAddress()
    let mainQueue = image.cachedDispatchMainQueue
    let mainQueueState = mainQueue.map { $0 + 0x38 }
    // libdispatch stores the owning Mach thread name in bits 2 through 31 of dq_state.
    let dispatchOwnerMask: UInt64 = 0xffff_fffc
    let dispatchOwner = guestMachThreadName(1) & dispatchOwnerMask
    let mainTSD = (taskPortSlot + 8 + 15) & ~15
    let argcSlot = mainTSD + guestTSDSize
    let argvSlot = argcSlot + 8
    let hostScratch = argvSlot + 8
    let scratchSize = 4096
    let writeBase = mainErrno
    guard hostScratch + scratchSize <= stackEnd - 16 * 1024 else {
        throw GuestError("Guest process vectors leave insufficient stack space")
    }
    for (index, pointer) in argumentsVector.enumerated() { store(pointer, at: vector + index * 8) }
    for (index, pointer) in environmentVector.enumerated() { store(pointer, at: envVector + index * 8) }
    store(UInt64(argv.count), at: argcSlot)
    store(UInt64(vector), at: argvSlot)
    // Model these libc globals in the guest rather than exposing host pointers.
    store(1, at: mainTSD)
    store(UInt64(mainErrno), at: mainTSD + 8)
    store(guestMachThreadName(1), at: mainTSD + guestMachThreadSlotOffset)
    if let mainQueue { store(UInt64(mainQueue), at: mainTSD + 0xa0) }
    store(UInt64(stdoutSlot), at: stdoutSlot)
    store(1, at: localeSlot)
    store(UInt64(stderrSlot), at: stderrSlot)
    store(UInt64(stdinSlot), at: stdinSlot)
    store(UInt64(getpagesize()), at: vmPageSizeSlot)
    store(UInt64(getpagesize().trailingZeroBitCount), at: vmPageShiftSlot)
    store(UInt64(vm_kernel_page_size), at: vmKernelPageSizeSlot)
    store(guestTaskPort, at: taskPortSlot)
    if let address = image.cachedTaskPort,
       let region = image.cacheRegions.first(where: { $0.logical.contains(address) && address <= $0.logical.upperBound - 4 }) {
        var port = UInt32(guestTaskPort).littleEndian
        let offset = region.backing + address - region.logical.lowerBound
        withUnsafeBytes(of: &port) { data.replaceSubrange(offset..<(offset + 4), with: $0) }
    }
    if let address = cachedLocaleKey,
       let region = image.cacheRegions.first(where: { $0.logical.contains(address) && address <= $0.logical.upperBound - 8 }) {
        store(guestLocaleKey, at: region.backing + address - region.logical.lowerBound)
    }
    if let address = mainQueueState,
       let region = image.cacheRegions.first(where: { $0.logical.contains(address) && address <= $0.logical.upperBound - 8 }) {
        let offset = region.backing + address - region.logical.lowerBound
        let state = try MachO.integer(data, offset, 8)
        store((state & ~dispatchOwnerMask) | dispatchOwner, at: offset)
    }
    store(1, at: optindSlot)
    store(UInt64(envVector), at: environSlot)
    store(UInt64.random(in: 1...UInt64.max), at: canarySlot)
    if image.bindings.values.contains("__DefaultRuneLocale") {
        let source = _DefaultRuneLocale
        var locale = _RuneLocale()
        locale.__magic = source.__magic
        locale.__encoding = source.__encoding
        locale.__invalid_rune = source.__invalid_rune
        locale.__runetype = source.__runetype
        locale.__maplower = source.__maplower
        locale.__mapupper = source.__mapupper
        withUnsafeBytes(of: &locale) {
            data.replaceSubrange(runeLocaleSlot..<(runeLocaleSlot + $0.count), with: $0)
        }
    }
    let modeledGlobals = [
        "___stdoutp": stdoutSlot, "___mb_cur_max": localeSlot, "___stderrp": stderrSlot,
        "___stdinp": stdinSlot, "_optind": optindSlot, "_optarg": optargSlot,
        "_environ": environSlot, "___stack_chk_guard": canarySlot, "__DefaultRuneLocale": runeLocaleSlot,
        "_vm_page_size": vmPageSizeSlot,
        "_vm_page_shift": vmPageShiftSlot, "_vm_kernel_page_size": vmKernelPageSizeSlot,
        "_mach_task_self_": taskPortSlot,
    ]
    for (address, symbol) in image.bindings {
        guard address <= image.memory.count - 8 else { throw GuestError("Invalid executable pointer binding") }
        if let target = modeledGlobals[symbol] ?? image.functionBindings[address] { store(UInt64(target), at: address) }
    }
    let initialTokenCapacity = max(guestInitialTokenCapacity, image.authentication.count)
    var registers = [UInt64](repeating: 0, count: guestTokenStart + 3 * initialTokenCapacity)
    registers[guestPACGAKey] = UInt64.random(in: UInt64.min...UInt64.max)
    registers[guestPACGAKey + 1] = UInt64.random(in: UInt64.min...UInt64.max)
    registers[guestTokenCapacity] = UInt64(initialTokenCapacity)
    for (address, signature) in image.authentication.sorted(by: { $0.key < $1.key }) {
        let pointer = try MachO.integer(data, address, 8)
        if pointer == 0 { continue }
        let index = Int(registers[guestTokenCount])
        let slot = guestTokenStart + 3 * index
        registers[slot] = pointer
        registers[slot + 1] = signature.modifier
        registers[slot + 2] = signature.key
        registers[guestTokenCount] += 1
        store(encodeGuestPointer(pointer, key: signature.key, index: index), at: address)
    }
    guard var memory = data.withUnsafeBytes({
        device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
    }), var state = registers.withUnsafeBytes({
        device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
    }) else { throw GuestError("Process buffer allocation failed") }
    var residentCapacity = 256
    guard var residentTable = device.makeBuffer(length: 8 * (1 + 4 * residentCapacity), options: .storageModeShared) else {
        throw GuestError("Guest resident map allocation failed")
    }
    var resident = residentTable.contents().assumingMemoryBound(to: UInt64.self)
    resident[0] = 0
    func ensureResidentCapacity(_ required: Int) -> Bool {
        if required <= residentCapacity { return true }
        let maximum = (device.maxBufferLength / 8 - 1) / 4
        guard required <= maximum else { return false }
        let capacity = min(maximum, max(required, residentCapacity * 2))
        guard let expanded = device.makeBuffer(length: 8 * (1 + 4 * capacity), options: .storageModeShared) else {
            return false
        }
        expanded.contents().copyMemory(from: residentTable.contents(), byteCount: 8 * (1 + 4 * Int(resident[0])))
        residentTable = expanded
        resident = expanded.contents().assumingMemoryBound(to: UInt64.self)
        residentCapacity = capacity
        if trace { FileHandle.standardError.write(Data("Guest resident map grew to \(capacity) entries\n".utf8)) }
        return true
    }
    var cpu = state.contents().assumingMemoryBound(to: UInt64.self)
    var phase = 0
    func enter(_ address: Int) {
        for index in 0..<36 { cpu[index] = 0 }
        cpu[0] = UInt64(argv.count)
        cpu[1] = UInt64(vector)
        cpu[2] = UInt64(envVector)
        cpu[3] = UInt64(empty)
        cpu[30] = .max
        cpu[31] = UInt64(stackEnd)
        cpu[33] = UInt64(address)
    }
    func advanceStartup() throws -> Bool {
        guard phase < startup.count - 1 else { return false }
        guard cpu[37] == 0 else { throw GuestError("Guest initializer left its authentication stack unbalanced") }
        phase += 1
        enter(startup[phase])
        region = try compile(at: startup[phase])
        return true
    }
    enter(startup[phase])

    func reserveCacheTokens(_ additional: Int) throws {
        let required = Int(cpu[guestTokenCount]) + additional
        let capacity = Int(cpu[guestTokenCapacity])
        if required <= capacity { return }
        let maximum = min((device.maxBufferLength / 8 - guestTokenStart) / 3, Int(UInt32.max >> guestTokenIndexShift))
        guard required <= maximum else { throw GuestError("Guest authentication table exceeds Metal buffer limit") }
        let newCapacity = min(maximum, max(required, capacity * 2))
        guard let expanded = device.makeBuffer(length: 8 * (guestTokenStart + 3 * newCapacity),
                                               options: .storageModeShared) else {
            throw GuestError("Guest authentication table allocation failed")
        }
        expanded.contents().copyMemory(from: state.contents(), byteCount: state.length)
        state = expanded
        cpu = state.contents().assumingMemoryBound(to: UInt64.self)
        cpu[guestTokenCapacity] = UInt64(newCapacity)
        if trace {
            FileHandle.standardError.write(Data("Guest authentication table grew to \(newCapacity) entries\n".utf8))
        }
    }

    var cacheCursor = image.cacheBackingStart
    var residentRegions = [ExecutableImage.CacheRegion]()
    var objcWriteOwner: UInt64?
    var objcWritableBacking = Set<Int>()
    let mappingBase = guestMappingBase
    let pageSize = Int(getpagesize())
    let quiescentAddress = 0xfffffc180
    let quiescentBacking = image.cacheBackingEnd
    let quiescentRange = quiescentAddress..<(quiescentAddress + 8)
    let memorySizeBacking = quiescentBacking + 8
    let capabilitiesBacking = memorySizeBacking + 8
    let bootTimeBacking = capabilitiesBacking + 8
    let aprrSupportBacking = bootTimeBacking + 8
    var quiescentGeneration: UInt64 = 0
    var mappingHeap = GuestHeap((aprrSupportBacking + 8)..<image.mappingBackingEnd)
    var anonymousMappings = [Int: (region: ExecutableImage.CacheRegion, maximum: UInt32)]()
    func publishResident(_ original: ExecutableImage.CacheRegion) {
        var region = original
        if image.cacheReader?.isCacheDataConst(region.logical) == true {
            region.writable = objcWriteOwner != nil
            if region.writable { objcWritableBacking.insert(region.backing) }
        }
        let row = resident + 1 + 4 * Int(resident[0])
        row[0] = UInt64(region.logical.lowerBound)
        row[1] = UInt64(region.logical.upperBound)
        row[2] = UInt64(region.backing)
        row[3] = (region.writable ? 1 : 0) | (region.readable ? 0 : 2)
        resident[0] += 1
        residentRegions.append(region)
    }
    guard ensureResidentCapacity(image.cacheRegions.count + 5) else {
        throw GuestError("Guest resident map allocation failed")
    }
    for region in image.cacheRegions { publishResident(region) }
    func updateObjCCacheWrites(enabled: Bool) {
        if enabled, let reader = image.cacheReader {
            for region in residentRegions where !region.writable && reader.isCacheDataConst(region.logical) {
                objcWritableBacking.insert(region.backing)
            }
        }
        for index in residentRegions.indices where objcWritableBacking.contains(residentRegions[index].backing) {
            residentRegions[index].writable = enabled
        }
        for index in 0..<Int(resident[0]) {
            let row = resident + 1 + 4 * index
            if objcWritableBacking.contains(Int(row[2])) {
                row[3] = (row[3] & ~1) | (enabled ? 1 : 0)
            }
        }
        if !enabled { objcWritableBacking.removeAll() }
    }
    func publishCacheMetadata(_ reader: SharedCache, address: Int, size: Int, writable: Bool = false) throws {
        let range = address..<(address + size)
        if residentRegions.contains(where: {
            $0.readable && $0.writable == writable && $0.logical.contains(address) && range.upperBound <= $0.logical.upperBound
        }) { return }
        guard !residentRegions.contains(where: { $0.logical.overlaps(range) }),
              ensureResidentCapacity(Int(resident[0]) + 1),
              let backing = mappingHeap.allocate(UInt64(size)) else {
            throw GuestError("Cannot map guest cache metadata")
        }
        let bytes = try reader.bytes(at: reader.originalAddress(address), count: size)
        bytes.withUnsafeBytes {
            memory.contents().advanced(by: backing).copyMemory(from: $0.baseAddress!, byteCount: size)
        }
        publishResident(ExecutableImage.CacheRegion(logical: range, backing: backing, writable: writable))
    }
    publishResident(ExecutableImage.CacheRegion(logical: quiescentRange, backing: quiescentBacking, writable: false))
    func publishCommpageWord(_ value: UInt64, address: Int, backing: Int) {
        memory.contents().advanced(by: backing).storeBytes(of: value, as: UInt64.self)
        publishResident(ExecutableImage.CacheRegion(logical: address..<(address + 8), backing: backing, writable: false))
    }
    // One scheduled guest CPU, a readable thread pointer, and ARMv8 CRC32.
    let capabilities: UInt64 = (1 << 16) | (1 << 15) | (1 << 7) | (1 << 26)
    publishCommpageWord(capabilities, address: 0xfffffc010, backing: capabilitiesBacking)
    // Cached code selects CPU-specific permission controls through this byte.
    // The GPU guest has no APRR registers or per-thread JIT write toggles.
    let aprrSupportAddress = 0xfffffc10c
    memory.contents().advanced(by: aprrSupportBacking).storeBytes(of: UInt8(0), as: UInt8.self)
    publishResident(ExecutableImage.CacheRegion(logical: aprrSupportAddress..<(aprrSupportAddress + 1),
                                                backing: aprrSupportBacking, writable: false))
    var physicalMemory: UInt64 = 0
    var memorySizeLength = MemoryLayout<UInt64>.size
    if Darwin.sysctlbyname("hw.memsize", &physicalMemory, &memorySizeLength, nil, 0) == 0,
       memorySizeLength == MemoryLayout<UInt64>.size {
        publishCommpageWord(physicalMemory, address: 0xfffffc038, backing: memorySizeBacking)
    }
    var bootTime = timeval()
    var bootTimeLength = MemoryLayout<timeval>.size
    if Darwin.sysctlbyname("kern.boottime", &bootTime, &bootTimeLength, nil, 0) == 0,
       bootTimeLength == MemoryLayout<timeval>.size, bootTime.tv_sec >= 0, bootTime.tv_usec >= 0 {
        let microseconds = UInt64(bootTime.tv_sec) * 1_000_000 + UInt64(bootTime.tv_usec)
        publishCommpageWord(microseconds, address: 0xfffffc0a0, backing: bootTimeBacking)
    }
    func removeResident(_ address: Int) throws {
        guard let row = (0..<Int(resident[0])).first(where: { resident[1 + 4 * $0] == UInt64(address) }) else {
            throw GuestError("Missing guest VM mapping")
        }
        residentRegions.removeAll { $0.logical.lowerBound == address }
        resident[0] -= 1
        for column in 0..<4 { resident[1 + 4 * row + column] = resident[1 + 4 * Int(resident[0]) + column] }
    }
    var mappedFiles = [Int: (source: UnsafeMutableRawPointer, size: Int, backing: Int)]()
    defer {
        for file in mappedFiles.values { _ = Darwin.munmap(file.source, file.size) }
    }
    var cacheReadWindows = Set<Range<Int>>()
    func makeResident(_ address: Int, minimumBytes: Int = 1) throws -> Bool {
        let minimumBytes = max(1, minimumBytes)
        guard let reader = image.cacheReader, !image.deferredCache.isEmpty,
              address >= SharedCache.guestBase, address < SharedCache.guestLimit,
              minimumBytes <= SharedCache.guestLimit - address,
              !residentRegions.contains(where: {
                  $0.logical.contains(address) && minimumBytes <= $0.logical.upperBound - address
              }) else { return false }
        let pending: ExecutableImage.DeferredCacheSegment
        if let segment = image.deferredCache.first(where: { $0.logical.contains(address) }) {
            let text = segment.segment.name == "__TEXT" && !segment.segment.writable
            if text || segment.segment.name == "__OBJC_RO" {
                let window: Range<Int>
                if text {
                    let page = address & ~(pageSize - 1)
                    window = page..<max(page + pageSize, address + minimumBytes)
                } else {
                    window = try reader.selectorWindow(at: address, count: minimumBytes)
                }
                var lower = max(window.lowerBound, segment.logical.lowerBound)
                var upper = min(window.upperBound, segment.logical.upperBound)
                for region in residentRegions where region.logical.overlaps(window) && !cacheReadWindows.contains(region.logical) {
                    if region.logical.contains(address) { return false }
                    if region.logical.upperBound <= address { lower = max(lower, region.logical.upperBound) }
                    if region.logical.lowerBound > address { upper = min(upper, region.logical.lowerBound) }
                }
                guard minimumBytes <= upper - address else { throw GuestError("Cached read crosses a segment boundary") }
                let delta = UInt64(lower - segment.logical.lowerBound)
                let original = segment.segment
                let length = UInt64(upper - lower)
                let fileSize = delta < original.fileSize ? min(length, original.fileSize - delta) : 0
                let slice = SharedCache.Segment(name: original.name, address: original.address + delta, size: length,
                                               fileOffset: original.fileOffset + delta, fileSize: fileSize, writable: false)
                pending = ExecutableImage.DeferredCacheSegment(logical: lower..<upper, segment: slice)
            } else {
                guard !residentRegions.contains(where: { $0.logical.contains(address) }) else { return false }
                pending = segment
            }
        } else if let slot = try reader.constantRebaseSlot(at: address) {
            let start = address & ~7
            guard !residentRegions.contains(where: { $0.logical.contains(address) }),
                  minimumBytes <= start + 8 - address else { return false }
            pending = ExecutableImage.DeferredCacheSegment(logical: start..<(start + 8), segment: slot)
        } else {
            return false
        }
        let segment = pending.segment
        let offset = ((cacheCursor + 15) & ~15) + (pending.logical.lowerBound & 15)
        let count = Int(resident[0])
        guard pending.logical.count <= image.cacheBackingEnd - offset, ensureResidentCapacity(count + 1) else {
            throw GuestError("Guest cache resident limit at \(segment.name) (\(cacheCursor - image.cacheBackingStart) bytes resident)")
        }
        let contents = memory.contents().advanced(by: offset)
        let bytes = try reader.bytes(at: segment.address, count: Int(segment.fileSize))
        if !bytes.isEmpty {
            bytes.withUnsafeBytes { contents.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
        }
        let rebases = try reader.rebases(in: segment.address..<(segment.address + segment.size))
        try reserveCacheTokens(rebases.reduce(0) { $0 + ($1.authentication == nil ? 0 : 1) })
        for rebase in rebases {
            let logical = try reader.guestAddress(rebase.location)
            let pointerOffset = logical - pending.logical.lowerBound
            var pointer = UInt64(try reader.guestAddress(rebase.target)) | (UInt64(rebase.highByte) << 56)
            if let name = image.deferredBindings[logical], let target = modeledGlobals[name] {
                pointer = UInt64(target)
            } else if let auth = rebase.authentication {
                let index = Int(cpu[guestTokenCount])
                let token = guestTokenStart + 3 * index
                let diversity = UInt64(auth.diversity)
                cpu[token] = pointer
                cpu[token + 1] = auth.addressDiversity ? UInt64(logical) | (diversity << 48) : diversity
                cpu[token + 2] = auth.dataKey ? 2 : 0
                cpu[guestTokenCount] += 1
                pointer = encodeGuestPointer(pointer, key: cpu[token + 2], index: index)
            }
            contents.storeBytes(of: pointer.littleEndian, toByteOffset: pointerOffset, as: UInt64.self)
        }
        if let address = image.cachedTaskPort, pending.logical.contains(address),
           address <= pending.logical.upperBound - 4 {
            contents.storeBytes(of: UInt32(guestTaskPort).littleEndian,
                                toByteOffset: address - pending.logical.lowerBound, as: UInt32.self)
        }
        if let address = cachedLocaleKey, pending.logical.contains(address), address <= pending.logical.upperBound - 8 {
            contents.storeBytes(of: guestLocaleKey.littleEndian,
                                toByteOffset: address - pending.logical.lowerBound, as: UInt64.self)
        }
        if let address = mainQueueState, pending.logical.contains(address), address <= pending.logical.upperBound - 8 {
            let slot = contents.advanced(by: address - pending.logical.lowerBound)
            let state = UInt64(littleEndian: slot.loadUnaligned(as: UInt64.self))
            slot.storeBytes(of: ((state & ~dispatchOwnerMask) | dispatchOwner).littleEndian, as: UInt64.self)
        }
        publishResident(ExecutableImage.CacheRegion(logical: pending.logical, backing: offset, writable: segment.writable))
        if !segment.writable && (segment.name == "__TEXT" || segment.name == "__OBJC_RO") {
            cacheReadWindows.insert(pending.logical)
        }
        cacheCursor = offset + pending.logical.count
        if trace {
            let line = "Mapped cache \(segment.name) at 0x\(String(pending.logical.lowerBound, radix: 16)), \(pending.logical.count) bytes\n"
            FileHandle.standardError.write(Data(line.utf8))
        }
        return true
    }

    func guestExtent(_ address: UInt64, minimumBytes: Int = 1) throws -> (offset: Int, length: Int, cached: ExecutableImage.CacheRegion?) {
        guard let logical = Int(exactly: address) else { throw GuestError("Guest address out of bounds") }
        if logical >= SharedCache.guestBase { _ = try makeResident(logical, minimumBytes: minimumBytes) }
        if let region = residentRegions.filter({ $0.logical.contains(logical) }).max(by: {
            $0.logical.upperBound < $1.logical.upperBound
        }) {
            guard region.readable else { throw GuestError("Guest VM mapping is unreadable") }
            return (region.backing + logical - region.logical.lowerBound, region.logical.upperBound - logical, region)
        }
        if logical < image.mappingBackingEnd {
            guard logical < image.linearImageEnd else { throw GuestError("Guest address overlaps cache backing") }
            return (logical, image.linearImageEnd - logical, nil)
        }
        guard logical <= memory.length else { throw GuestError("Guest address out of bounds") }
        return (logical, memory.length - logical, nil)
    }

    func guestString(_ address: UInt64) throws -> UnsafeBufferPointer<UInt8> {
        let extent = try guestExtent(address)
        guard extent.length > 0 else { throw GuestError("Guest string address out of bounds") }
        let start = memory.contents().assumingMemoryBound(to: UInt8.self) + extent.offset
        let buffer = UnsafeBufferPointer(start: start, count: extent.length)
        guard let end = buffer.firstIndex(of: 0) else {
            if let logical = Int(exactly: address), let reader = image.cacheReader,
               let segment = image.deferredCache.first(where: {
                   $0.segment.name == "__OBJC_RO" && $0.logical.contains(logical)
               }),
               let size = try reader.selectorStringSize(at: logical, limit: segment.logical.upperBound - logical),
               size > extent.length {
                let contiguous = try guestBuffer(address, UInt64(size))
                return UnsafeBufferPointer(start: contiguous.assumingMemoryBound(to: UInt8.self), count: size - 1)
            }
            throw GuestError("Guest string not terminated at 0x\(String(address, radix: 16)) within \(extent.length) bytes")
        }
        return UnsafeBufferPointer(start: start, count: end)
    }

    func guestBuffer(_ address: UInt64, _ length: UInt64, writable: Bool = false) throws -> UnsafeMutableRawPointer {
        guard let minimumBytes = Int(exactly: length) else { throw GuestError("Guest host buffer out of bounds") }
        let extent = try guestExtent(address, minimumBytes: minimumBytes)
        guard length <= UInt64(extent.length) else { throw GuestError("Guest host buffer out of bounds") }
        if writable, let region = extent.cached, !region.writable {
            throw GuestError("Guest cached segment is read-only")
        }
        if writable, extent.cached == nil, address < UInt64(writeBase),
           !image.writableRegions.contains(where: {
               $0.contains(Int(address)) && Int(length) <= $0.upperBound - Int(address)
           }) {
            throw GuestError("Guest host buffer is read-only")
        }
        return memory.contents().advanced(by: extent.offset)
    }
    func writeGlobal(_ value: UInt64, _ address: Int) {
        memory.contents().advanced(by: address).storeBytes(of: value, as: UInt64.self)
    }
    func copyHostString(_ source: UnsafePointer<CChar>?) throws -> UInt64 {
        guard let source else { return 0 }
        let length = strlen(source) + 1
        guard length <= scratchSize else { throw GuestError("Host string exceeds guest scratch space") }
        memory.contents().advanced(by: hostScratch).copyMemory(from: source, byteCount: length)
        return UInt64(hostScratch)
    }
    var optionIndex = 1
    var optionOffset = 1
    var heap = GuestHeap(stackEnd..<memory.length)
    func allocateGuest(_ size: UInt64, alignment: Int = 16) -> Int? {
        if let address = heap.allocate(size, alignment: alignment) { return address }
        let oldLength = memory.length
        let limit = min(device.maxBufferLength, SharedCache.guestBase)
        let available = (limit - oldLength) & ~15
        let padding = (0 &- oldLength) & (alignment - 1)
        guard padding < available, let requested = Int(exactly: max(1, size)),
              requested <= available - padding else { return nil }
        let growth = min(available, max(oldLength, (padding + requested + 15) & ~15)) & ~15
        guard let expanded = device.makeBuffer(length: oldLength + growth, options: .storageModeShared) else {
            return nil
        }
        // Host services run between completed dispatches. Guest pointers are offsets.
        expanded.contents().copyMemory(from: memory.contents(), byteCount: oldLength)
        memory = expanded
        heap.extend(oldLength..<expanded.length)
        if trace { FileHandle.standardError.write(Data("Guest memory grew to \(expanded.length) bytes\n".utf8)) }
        return heap.allocate(size, alignment: alignment)
    }
    let standardSlots = [STDIN_FILENO: UInt64(stdinSlot), STDOUT_FILENO: UInt64(stdoutSlot),
                         STDERR_FILENO: UInt64(stderrSlot)]
    var closedStreams = Set<UInt64>()
    var standardStatusFlags = [Int32: Int32]()
    var standardDescriptorFlags = [Int32: Int32]()
    func readyForNonblockingIO(_ descriptor: Int32, events: Int16) -> Bool {
        guard standardStatusFlags[descriptor, default: 0] & O_NONBLOCK != 0 else { return true }
        var event = pollfd(fd: descriptor, events: events, revents: 0)
        return Darwin.poll(&event, 1, 0) > 0
    }
    var openDescriptors = Set<Int32>()
    var writableDescriptors = Set<Int32>()
    var guestFiles = [UInt64: UnsafeMutablePointer<FILE>]()
    var nextFile: UInt64 = 0xf11e_0000_0000_0001
    func ownsDescriptor(_ descriptor: Int32) -> Bool {
        openDescriptors.contains(descriptor) ||
            (descriptor >= 0 && guestFiles.values.contains { Darwin.fileno($0) == descriptor })
    }
    func allowsDescriptorWrite(_ descriptor: Int32) -> Bool {
        writableDescriptors.contains(descriptor) ||
            ((descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO) &&
             standardSlots[descriptor].map { !closedStreams.contains($0) } == true)
    }
    defer {
        for file in guestFiles.values { _ = Darwin.fclose(file) }
        for descriptor in openDescriptors { _ = Darwin.close(descriptor) }
    }

    let mainThread = GuestThread(id: 1, stack: (hostScratch + scratchSize)..<stackEnd,
                                 allocation: nil, errnoAddress: mainErrno, tsdAddress: mainTSD,
                                 registers: [], signalMask: 0)
    var current = mainThread
    var threads: [UInt64: GuestThread] = [mainThread.id: mainThread]
    var ready = [GuestThread]()
    var nextThreadID: UInt64 = 2
    let replyPortFlag: UInt32 = 0x1000 // MPO_REPLY_PORT in XNU's port type flags.
    let guardedPortFlag = UInt32(MPO_CONTEXT_AS_GUARD)
    var guestReceiveRights = [UInt32: (flags: UInt32, context: UInt64)]()
    var restartableRanges = [(start: UInt64, end: UInt64, recovery: UInt64)]()
    func restartableRecovery(at pc: UInt64) -> UInt64? {
        restartableRanges.first { $0.start < pc && pc < $0.end && $0.recovery != 0 }?.recovery
    }
    var nextGuestReceiveName: UInt32 = 0x8000_0003
    var mutexes = [UInt64: GuestMutex]()
    var mutexAttributes = [UInt64: Int32]()
    var conditions = [UInt64: [UInt64]]()
    var errnoSlot: Int { current.errnoAddress }
    var scheduledSignalStatus: Int32?

    var signals = GuestSignals()
    var queuedProcessSignals: UInt32 = 0
    func queueProcessSignals(_ incoming: UInt32) {
        var remaining = incoming
        while remaining != 0 {
            let number = Int32(remaining.trailingZeroBitCount + 1)
            let bit = UInt32(1) << (number - 1)
            remaining &= remaining - 1
            if GuestSignals.valid(number), signals.action(number).handler != 1 {
                queuedProcessSignals |= bit
            }
        }
    }
    struct SignalFrame {
        let registers: [UInt64]
        let vectors: [UInt64]
        let mask: UInt32
        let authenticationDepth: UInt64
        let region: Region
    }
    var signalFrames = [SignalFrame]()
    struct ImageNotification {
        let callback: Int
        let arguments: [(UInt64, UInt64, UInt64, UInt64)]
        let registers: [UInt64]
        let vectors: [UInt64]
        let authenticationDepth: UInt64
        var mutationBlock: UInt64 = 0
        var objcHeaders: [UInt64] = []
        var stopAddress: Int? = nil
        var next = 0
    }
    var imageNotifications = [UInt64: [ImageNotification]]()
    func continueImageNotification(_ notification: ImageNotification) throws {
        var notification = notification
        for (index, value) in notification.registers.enumerated() { cpu[index] = value }
        for (index, value) in notification.vectors.enumerated() { cpu[806 + index] = value }
        let stopped = notification.stopAddress.map {
            memory.contents().load(fromByteOffset: $0, as: UInt8.self) != 0
        } ?? false
        if stopped || notification.next == notification.arguments.count {
            if let stop = notification.stopAddress { try heap.release(stop) }
            if notification.mutationBlock != 0, objcWriteOwner == current.id {
                updateObjCCacheWrites(enabled: false)
                objcWriteOwner = nil
            }
            if cpu[33] != .max { region = try compile(at: Int(truncatingIfNeeded: cpu[33])) }
            return
        }
        let arguments = notification.arguments[notification.next]
        notification.next += 1
        imageNotifications[current.id, default: []].append(notification)
        cpu[0] = arguments.0
        cpu[1] = arguments.1
        cpu[2] = arguments.2
        cpu[3] = arguments.3
        cpu[30] = .max
        cpu[33] = UInt64(notification.callback)
        region = try compile(at: notification.callback)
    }
    func guestCodeAddress(_ pointer: UInt64, modifier: UInt64 = 0, key: UInt64 = 0) throws -> Int {
        var target = pointer
        if target & ~0xffff_ffff == guestTokenTag {
            let index = Int((target & 0xffff_ffff) >> guestTokenIndexShift)
            guard index > 0, UInt64(index) <= cpu[guestTokenCount] else {
                throw GuestError("Invalid authenticated guest code pointer")
            }
            let slot = guestTokenStart + 3 * (index - 1)
            guard target == encodeGuestPointer(cpu[slot], key: cpu[slot + 2], index: index - 1) else {
                throw GuestError("Invalid authenticated guest code pointer")
            }
            guard cpu[slot + 1] == modifier, cpu[slot + 2] == key else {
                throw GuestError("Unsupported authentication for guest code pointer")
            }
            target = cpu[slot]
        } else if key != 0 {
            throw GuestError("Missing authenticated guest code pointer")
        }
        guard let address = Int(exactly: target), image.isCode(address) else {
            throw GuestError("Guest code pointer is outside translated text")

        }
        return address
    }

    func lookupGuestSection(_ header: UInt64, kind: Int) throws -> (UInt64, UInt64) {
        guard kind < dyldSectionNames.count else { return (0, .max) }
        guard let slide = image.imageSlides[header] else { return (0, 0) }
        func imageBytes(offset: UInt64, size: Int) throws -> Data {
            if header >= UInt64(SharedCache.guestBase), let reader = image.cacheReader {
                let address = try reader.originalAddress(Int(header))
                return try reader.bytes(at: address + offset, count: size)
            }
            return try Data(bytes: guestBuffer(header + offset, UInt64(size)), count: size)
        }
        let machHeader = try imageBytes(offset: 0, size: 32)
        let count = Int(try MachO.integer(machHeader, 16, 4))
        let size = Int(try MachO.integer(machHeader, 20, 4))
        guard count <= size / 8 else { throw GuestError("Invalid guest Mach-O load commands") }
        let commands = try imageBytes(offset: 32, size: size)
        guard let section = try dyldSection(in: commands, count: count, kind: kind),
              section.address &+ slide != header else { return (0, 0) }
        return (section.address &+ slide, section.size)
    }

    func deliverSignal() throws -> Int32? {
        let incoming = guest_signal_take()
        if incoming != 0 {
            signals.ingest(incoming)
            queueProcessSignals(incoming)
        }
        guard let (signal, action) = signals.next(threadPending: &current.pendingSignals) else { return nil }
        queuedProcessSignals &= ~GuestSignals.bit(signal)
        if action.handler == 0 {
            if [SIGCHLD, SIGURG, SIGWINCH, SIGINFO].contains(signal) { return try deliverSignal() }
            if [SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGCONT].contains(signal) {
                throw GuestError("Guest stop and continue signals are unsupported")
            }
            return 128 + signal
        }
        guard signalFrames.count < 32 else { throw GuestError("Guest signal nesting limit exceeded") }
        let address = try guestCodeAddress(action.handler)
        signalFrames.append(SignalFrame(registers: Array(UnsafeBufferPointer(start: cpu, count: 36)),
                                        vectors: Array(UnsafeBufferPointer(start: cpu + 806, count: guestTokenCount - 806)),
                                        mask: signals.mask, authenticationDepth: cpu[37], region: region))
        signals.enter(signal, action: action)
        cpu[0] = UInt64(signal)
        cpu[30] = .max
        cpu[33] = UInt64(address)
        region = try compile(at: address)
        return nil
    }
    func resumeSignal() throws -> Bool {
        guard let frame = signalFrames.popLast() else { return false }
        guard cpu[37] == frame.authenticationDepth else {
            throw GuestError("Guest signal handler left its authentication stack unbalanced")
        }
        for (index, value) in frame.registers.enumerated() { cpu[index] = value }
        for (index, value) in frame.vectors.enumerated() { cpu[806 + index] = value }
        signals.mask = frame.mask
        if let recovery = restartableRecovery(at: cpu[33]) {
            cpu[33] = recovery
            region = try compile(at: Int(truncatingIfNeeded: recovery))
        } else {
            region = frame.region
        }
        return true
    }

    func schedule(requeue: Bool) throws {
        if objcWriteOwner != nil {
            if requeue { return }
            throw GuestError("Guest thread cannot block while Objective-C cache writes are enabled")
        }
        if !signalFrames.isEmpty {
            if requeue { return }
            throw GuestError("Guest thread cannot block inside a signal handler")
        }
        func saveCurrent() {
            current.registers = Array(UnsafeBufferPointer(start: cpu, count: guestTokenCount))
            current.signalMask = signals.mask
        }
        if !requeue, current.result == nil { saveCurrent() }
        try expireWaiters()
        if requeue && ready.isEmpty { return }
        while ready.isEmpty {
            let hostIncoming = guest_signal_take()
            if hostIncoming != 0 {
                signals.ingest(hostIncoming)
                queueProcessSignals(hostIncoming)
            }
            let incoming = hostIncoming | queuedProcessSignals
            if incoming != 0 {
                let now = DispatchTime.now().uptimeNanoseconds
                for thread in threads.values {
                    guard case .sleep(let deadline, let remainder) = thread.wait else { continue }
                    let deliverable = incoming & ~thread.signalMask
                    guard deliverable != 0 else { continue }
                    let number = Int32(deliverable.trailingZeroBitCount + 1)
                    guard GuestSignals.valid(number) else { continue }
                    let action = signals.action(number)
                    guard action.handler != 1,
                          action.handler != 0 || ![SIGCHLD, SIGURG, SIGWINCH, SIGINFO].contains(number) else { continue }
                    if let remainder {
                        let nanoseconds = deadline > now ? deadline - now : 0
                        var value = timespec()
                        value.tv_sec = Int(nanoseconds / 1_000_000_000)
                        value.tv_nsec = Int(nanoseconds % 1_000_000_000)
                        let output = try guestBuffer(remainder, UInt64(MemoryLayout<timespec>.size), writable: true)
                        withUnsafeBytes(of: value) { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                    }
                    writeGlobal(UInt64(EINTR), thread.errnoAddress)
                    thread.wakeError = -1
                    try wake(thread.id)
                    break
                }
                if !ready.isEmpty { break }
            }
            let earliest = threads.values.compactMap { thread -> UInt64? in
                switch thread.wait {
                case .condition(_, _, let deadline): return deadline
                case .sleep(let deadline, _): return deadline
                default: return nil
                }
            }.min()
            guard let earliest else { throw GuestError("Guest threads are deadlocked") }
            let now = DispatchTime.now().uptimeNanoseconds
            if earliest > now {
                let interval: UInt64 = threads.values.contains(where: {
                    if case .sleep = $0.wait { return true }
                    return false
                }) ? 1_000_000 : 100_000_000
                Thread.sleep(forTimeInterval: Double(min(earliest - now, interval)) / 1e9)
            }
            try expireWaiters()
        }
        if requeue {
            saveCurrent()
            ready.append(current)
        }
        current = ready.removeFirst()
        for (index, value) in current.registers.enumerated() { cpu[index] = value }
        signals.mask = current.signalMask
        if cpu[33] != .max { region = try compile(at: Int(truncatingIfNeeded: cpu[33])) }
        if let status = try deliverSignal() { scheduledSignalStatus = status }
    }

    func wake(_ id: UInt64) throws {
        guard let thread = threads[id], thread.wait != nil else { throw GuestError("Invalid guest wakeup") }
        thread.wait = nil
        thread.registers[0] = UInt64(UInt32(bitPattern: thread.wakeError))
        thread.wakeError = 0
        ready.append(thread)
    }

    func writeThreadResult(_ value: UInt64, to address: UInt64) throws {
        if address != 0 {
            let destination = try guestBuffer(address, 8, writable: true)
            withUnsafeBytes(of: value) { destination.copyMemory(from: $0.baseAddress!, byteCount: 8) }
        }
    }

    func reap(_ thread: GuestThread) throws {
        if let allocation = thread.allocation { try heap.release(allocation) }
        threads.removeValue(forKey: thread.id)
    }

    func completeThread(_ result: UInt64) throws -> Bool {
        let thread = current
        thread.result = result
        for waiter in threads.values {
            if case let .join(id, output) = waiter.wait, id == thread.id {
                try writeThreadResult(result, to: output)
                try wake(waiter.id)
                try reap(thread)
                break
            }
        }
        if threads.values.allSatisfy({ $0.result != nil }) { return true }
        try schedule(requeue: false)
        return false
    }

    func mutex(at address: UInt64) throws -> GuestMutex? {
        let pointer = try guestBuffer(address, UInt64(MemoryLayout<pthread_mutex_t>.size), writable: true)
        if let mutex = mutexes[address] { return mutex }
        let signature = pointer.loadUnaligned(as: UInt64.self)
        let kinds: [UInt64: Int32] = [0x32aaaba7: PTHREAD_MUTEX_NORMAL,
                                      0x32aaaba1: PTHREAD_MUTEX_ERRORCHECK,
                                      0x32aaaba2: PTHREAD_MUTEX_RECURSIVE]
        return kinds[signature].map { GuestMutex(kind: $0) }
    }

    func unlockMutex(_ address: UInt64) throws -> Int32 {
        guard var lock = try mutex(at: address) else { return EINVAL }
        let (error, next) = lock.unlock(current.id)
        mutexes[address] = lock
        if let next { try wake(next) }
        return error
    }

    func reacquire(_ thread: GuestThread, lockAddress: UInt64, result: Int32) throws {
        guard var lock = try mutex(at: lockAddress) else { throw GuestError("Invalid guest condition mutex") }
        thread.wakeError = result
        switch lock.lock(thread.id, tryOnly: false) {
        case .acquired: try wake(thread.id)
        case .wait: thread.wait = .mutex(lockAddress)
        case .error: throw GuestError("Guest condition could not reacquire its mutex")
        }
        mutexes[lockAddress] = lock
    }

    func expireWaiters() throws {
        let now = DispatchTime.now().uptimeNanoseconds
        for thread in threads.values {
            if case .sleep(let deadline, _) = thread.wait, deadline <= now {
                try wake(thread.id)
            } else if case .condition(let address, let lockAddress, let deadline) = thread.wait,
                      let deadline, deadline <= now {
                conditions[address]?.removeAll { $0 == thread.id }
                try reacquire(thread, lockAddress: lockAddress, result: ETIMEDOUT)
            }
        }
    }

    func timedWaitDeadline(_ time: timespec, relative: Bool) -> UInt64? {
        guard time.tv_nsec >= 0, time.tv_nsec < 1_000_000_000 else { return nil }
        if time.tv_sec < 0, relative { return nil }
        func nanos(_ value: timespec) -> UInt64 {
            guard value.tv_sec >= 0 else { return 0 }
            let seconds = UInt64(value.tv_sec)
            let fraction = UInt64(value.tv_nsec)
            return seconds <= (UInt64.max - fraction) / 1_000_000_000
                ? seconds * 1_000_000_000 + fraction : .max
        }
        let now = DispatchTime.now().uptimeNanoseconds
        let requested = nanos(time)
        if relative { return now + min(requested, UInt64.max - now) }
        var wall = timespec()
        guard clock_gettime(CLOCK_REALTIME, &wall) == 0 else { return nil }
        let wallNow = nanos(wall)
        let remaining = requested > wallNow ? requested - wallNow : 0
        return now + min(remaining, UInt64.max - now)
    }

    func finishExecutionFrame() throws -> Int32? {
        while try resumeSignal() {
            if let status = try deliverSignal() { return status }
            if cpu[33] != .max { return nil }
        }
        while let notification = imageNotifications[current.id]?.popLast() {
            guard cpu[37] == notification.authenticationDepth else {
                throw GuestError("Guest image callback left its authentication stack unbalanced")
            }
            try continueImageNotification(notification)
            if cpu[33] != .max { return nil }
        }
        if current.id != mainThread.id {
            return try completeThread(cpu[0]) ? 0 : nil
        }
        if try advanceStartup() { return nil }
        return Int32(truncatingIfNeeded: cpu[0])
    }

    while true {
        if let status = scheduledSignalStatus { return status }
        try expireWaiters()
        if cpu[33] == .max {
            if let status = try finishExecutionFrame() { return status }
            continue
        }
        for file in mappedFiles.values {
            var copied: mach_vm_size_t = 0
            let source = mach_vm_address_t(UInt(bitPattern: file.source))
            let destination = mach_vm_address_t(UInt(bitPattern: memory.contents().advanced(by: file.backing)))
            let result = mach_vm_read_overwrite(mach_task_self_, source, mach_vm_size_t(file.size), destination, &copied)
            guard result == KERN_SUCCESS, copied == file.size else {
                throw GuestError("Guest shared mapping could not be refreshed")
            }
        }
        guard let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else {
            throw GuestError("Process command allocation failed")
        }
        encoder.setComputePipelineState(region.pipeline)
        encoder.setBuffer(memory, offset: 0, index: 0)
        encoder.setBuffer(state, offset: 0, index: 1)
        encoder.setBuffer(residentTable, offset: 0, index: 4)
        var size = UInt64(memory.length)
        var writable = UInt64(writeBase)
        encoder.setBytes(&size, length: 8, index: 2)
        encoder.setBytes(&writable, length: 8, index: 3)
        let one = MTLSize(width: 1, height: 1, depth: 1)
        encoder.dispatchThreads(one, threadsPerThreadgroup: one)
        encoder.endEncoding()
        cpu[36] = 8192
        cpu[guestFaultAddress] = .max
        cpu[guestCacheFaultAddress] = .max
        cpu[guestThreadPointer] = UInt64(current.tsdAddress)
        command.commit()
        command.waitUntilCompleted()
        guard command.status == .completed else {
            throw command.error ?? GuestError("GPU process execution failed")
        }
        // Only one guest thread dispatches at a time. Completion is a guest quiescent point.
        quiescentGeneration &+= 1
        writeGlobal(quiescentGeneration, quiescentBacking)
        if trace {
            let milliseconds = (command.gpuEndTime - command.gpuStartTime) * 1000
            let message = "Metal \(device.name): GPU \(milliseconds) ms, guest status \(cpu[34]), service \(cpu[35]), resume 0x\(String(cpu[33], radix: 16)), thread \(current.id)\n"
            FileHandle.standardError.write(Data(message.utf8))

        }
        switch cpu[34] {
        case 0:
            if let status = try finishExecutionFrame() { return status }
        case 1, 8:
            if let status = try deliverSignal() { return status }
            try schedule(requeue: true)
        case 2:
            if let logical = Int(exactly: cpu[guestCacheFaultAddress]),
               let width = Int(exactly: cpu[guestCacheFaultWidth]),
               try makeResident(logical, minimumBytes: width) {
                if trace {
                    let line = "Retry cache fault at guest PC 0x\(String(cpu[33], radix: 16))\n"
                    FileHandle.standardError.write(Data(line.utf8))
                }
                region = try compile(at: Int(truncatingIfNeeded: cpu[33]))
                continue
            }
            let address = cpu[guestFaultAddress]
            let detail = address == .max ? "" : " (effective address 0x\(String(address, radix: 16)))"
            throw GuestError("Guest process memory access out of bounds or read-only at 0x\(String(cpu[33], radix: 16))\(detail)")
        case 3:
            throw GuestError("Guest process reached an untranslated address at 0x\(String(cpu[33], radix: 16))")
        case 13:
            let number = Int64(bitPattern: cpu[16])
            let location = String(cpu[33] - 4, radix: 16)
            if trace {
                let arguments = (0..<(number == -47 ? 8 : (number == 336 ? 6 : 4)))
                    .map { "0x" + String(cpu[$0], radix: 16) }.joined(separator: " ")
                let message = "Guest Darwin trap \(number) at 0x\(location), args \(arguments), lr 0x\(String(cpu[30], radix: 16))\n"
                FileHandle.standardError.write(Data(message.utf8))
            }
            switch number {
            case 340:
                cpu[32] |= 8
                let pathAddress = cpu[0]
                cpu[0] = UInt64(EFAULT)
                if pathAddress != 0, let path = try? guestString(pathAddress), let base = path.baseAddress {
                    var info = stat()
                    let result = base.withMemoryRebound(to: CChar.self, capacity: path.count + 1) {
                        Darwin.lstat($0, &info)
                    }
                    if result != 0 {
                        cpu[0] = UInt64(errno)
                    } else if cpu[1] != 0,
                              let output = try? guestBuffer(cpu[1], UInt64(MemoryLayout<stat>.size), writable: true) {
                        withUnsafeBytes(of: info) { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                        cpu[0] = 0
                        cpu[32] &= ~8
                    }
                }
            case 381:
                guard cpu[1] == 2, try guestString(cpu[0]).elementsEqual("Sandbox".utf8) else {
                    throw GuestError("Unsupported guest MAC policy or call")
                }
                let request = try guestBuffer(cpu[2], 48)
                let resultAddress = request.loadUnaligned(as: UInt64.self)
                let pid = request.loadUnaligned(fromByteOffset: 8, as: Int64.self)
                let operationAddress = request.loadUnaligned(fromByteOffset: 16, as: UInt64.self)
                let filter = request.loadUnaligned(fromByteOffset: 24, as: UInt64.self)
                let pathAddress = request.loadUnaligned(fromByteOffset: 32, as: UInt64.self)
                let flags = request.loadUnaligned(fromByteOffset: 40, as: UInt64.self)
                guard pid == Int64(getpid()), filter == 1, flags == 0 else {
                    throw GuestError("Unsupported guest sandbox query for pid \(pid), filter \(filter), flags \(flags)")
                }
                cpu[32] |= 8
                cpu[0] = UInt64(EFAULT)
                if resultAddress != 0, operationAddress != 0, pathAddress != 0,
                   let operation = try? Array(guestString(operationAddress)) + [0],
                   let path = try? Array(guestString(pathAddress)) + [0],
                   let output = try? guestBuffer(resultAddress, 8, writable: true) {
                    var result: UInt64 = 0
                    let error = operation.withUnsafeBytes { operationBytes in
                        path.withUnsafeBytes { pathBytes in
                            guest_sandbox_check_path(operationBytes.baseAddress!.assumingMemoryBound(to: CChar.self),
                                                     pathBytes.baseAddress!.assumingMemoryBound(to: CChar.self), &result)
                        }
                    }
                    cpu[0] = UInt64(error)
                    if error == 0 {
                        output.copyMemory(from: &result, byteCount: 8)
                        cpu[32] &= ~8
                    }
                }
            case 169:
                guard UInt32(truncatingIfNeeded: cpu[1]) == 0 else {
                    throw GuestError("Unsupported guest csops operation \(cpu[1])")
                }
                let pid = Int32(truncatingIfNeeded: cpu[0])
                cpu[32] |= 8
                if pid != 0 && pid != getpid() {
                    cpu[0] = UInt64(ESRCH)
                } else if cpu[2] == 0 {
                    cpu[0] = 0
                    cpu[32] &= ~8
                } else if let output = try? guestBuffer(cpu[2], 4, writable: true) {
                    output.storeBytes(of: signing.status.littleEndian, as: UInt32.self)
                    cpu[0] = 0
                    cpu[32] &= ~8
                } else {
                    cpu[0] = UInt64(EFAULT)
                }
            case 336:
                // PROC_INFO_CALL_PIDINFO, PROC_PIDUNIQIDENTIFIERINFO for this process.
                guard UInt32(truncatingIfNeeded: cpu[0]) == 2,
                      UInt32(truncatingIfNeeded: cpu[2]) == 17 else {
                    throw GuestError("Unsupported guest proc_info call \(cpu[0]), flavor \(cpu[2])")
                }
                let size: Int32 = 56
                let pid = Int32(truncatingIfNeeded: cpu[1])
                cpu[32] |= 8
                if Int32(truncatingIfNeeded: cpu[5]) < size {
                    cpu[0] = UInt64(ENOMEM)
                } else if pid != getpid() {
                    cpu[0] = UInt64(ESRCH)
                } else if cpu[4] == 0 {
                    cpu[0] = UInt64(EFAULT)
                } else if let output = try? guestBuffer(cpu[4], UInt64(size), writable: true) {
                    var record = [UInt8](repeating: 0, count: Int(size))
                    let result = proc_pidinfo(pid, 17, cpu[3], &record, size)
                    if result == size {
                        record.replaceSubrange(0..<16, with: image.mainUUID)
                        record.withUnsafeBytes { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                        cpu[0] = UInt64(result)
                        cpu[32] &= ~8
                    } else {
                        cpu[0] = UInt64(result == 0 ? errno : EIO)
                    }
                } else {
                    cpu[0] = UInt64(EFAULT)
                }
            case -47:
                let reply = UInt32(cpu[3] >> 32)
                let messageID = UInt32(cpu[4] >> 32)
                guard cpu[1] == 0x2_0000_0003, UInt32(truncatingIfNeeded: cpu[2]) == 0x1513,
                      UInt64(UInt32(truncatingIfNeeded: cpu[3])) == guestTaskPort,
                      UInt32(truncatingIfNeeded: cpu[4]) == 0, messageID == 8000 || messageID == 8001,
                      cpu[5] == UInt64(reply) << 32,
                      cpu[6] >= 44, cpu[6] <= UInt64(UInt32.max), cpu[7] == 0,
                      guestReceiveRights[reply] != nil else {
                    throw GuestError("Unsupported guest Mach message id \(cpu[4] >> 32)")
                }
                let sendSize = cpu[2] >> 32
                guard messageID == 8000 ? (36...1060).contains(sendSize) : sendSize == 24,
                      let request = try? guestBuffer(cpu[0], sendSize),
                      let response = try? guestBuffer(cpu[0], 44, writable: true) else {
                    cpu[0] = UInt64(MACH_SEND_INVALID_DATA)
                    break
                }
                var result = KERN_INVALID_ARGUMENT
                if messageID == 8001 {
                    for thread in threads.values where thread !== current && thread.result == nil {
                        if let recovery = restartableRecovery(at: thread.registers[33]) {
                            thread.registers[33] = recovery
                        }
                    }
                    result = KERN_SUCCESS
                } else {
                    let count = Int(request.loadUnaligned(fromByteOffset: 32, as: UInt32.self))
                    if count > 0, count <= 64, sendSize == UInt64(36 + count * 16) {
                        var ranges = [(start: UInt64, end: UInt64, recovery: UInt64)]()
                        for index in 0..<count {
                            let offset = 36 + index * 16
                            let start = request.loadUnaligned(fromByteOffset: offset, as: UInt64.self)
                            let length = UInt64(request.loadUnaligned(fromByteOffset: offset + 8, as: UInt16.self))
                            let recoveryOffset = UInt64(request.loadUnaligned(fromByteOffset: offset + 10, as: UInt16.self))
                            let flags = request.loadUnaligned(fromByteOffset: offset + 12, as: UInt32.self)
                            guard flags == 0, length <= 4096, recoveryOffset <= 4096,
                                  start <= UInt64.max - max(length, recoveryOffset) else { break }
                            ranges.append((start, start + length, start + recoveryOffset))
                        }
                        ranges.sort { ($0.start, $0.end) < ($1.start, $1.end) }
                        if ranges.count == count, zip(ranges, ranges.dropFirst()).allSatisfy({ $0.end <= $1.start }) {
                            if threads.count > 1 || !restartableRanges.isEmpty {
                                result = KERN_NOT_SUPPORTED
                            } else {
                                restartableRanges = ranges
                                result = KERN_SUCCESS
                            }
                        }
                    }
                }
                let words: [UInt32] = [0x1200, 36, 0, reply, 0, messageID + 100, 0, 1, UInt32(bitPattern: result), 0, 8]
                words.withUnsafeBytes { response.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                cpu[0] = UInt64(MACH_MSG_SUCCESS)
            case -24:
                guard UInt64(UInt32(truncatingIfNeeded: cpu[0])) == guestTaskPort else {
                    cpu[0] = UInt64(MACH_SEND_INVALID_DEST)
                    break
                }
                let size = UInt64(MemoryLayout<mach_port_options_t>.size)
                guard cpu[1] != 0, cpu[3] != 0,
                      let options = try? guestBuffer(cpu[1], size),
                      let output = try? guestBuffer(cpu[3], 4, writable: true) else {
                    cpu[0] = UInt64(KERN_INVALID_ADDRESS)
                    break
                }
                let flags = UInt32(littleEndian: options.loadUnaligned(as: UInt32.self))
                guard flags & ~(guardedPortFlag | replyPortFlag) == 0 else {
                    throw GuestError("Unsupported guest Mach port construction flags 0x\(String(flags, radix: 16))")
                }
                guard nextGuestReceiveName < UInt32.max - 4 else {
                    cpu[0] = UInt64(KERN_NO_SPACE)
                    break
                }
                let name = nextGuestReceiveName
                nextGuestReceiveName += 4
                guestReceiveRights[name] = (flags: flags, context: cpu[2])
                var encodedName = name.littleEndian
                withUnsafeBytes(of: &encodedName) {
                    output.copyMemory(from: $0.baseAddress!, byteCount: $0.count)
                }
                cpu[0] = UInt64(KERN_SUCCESS)
            case -25:
                let target = UInt32(truncatingIfNeeded: cpu[0])
                let name = UInt32(truncatingIfNeeded: cpu[1])
                if UInt64(target) != guestTaskPort {
                    cpu[0] = UInt64(MACH_SEND_INVALID_DEST)
                } else if name == 0 || name == UInt32.max {
                    cpu[0] = UInt64(KERN_INVALID_NAME)
                } else if let right = guestReceiveRights[name],
                          UInt32(truncatingIfNeeded: cpu[2]) == 0,
                          right.flags & guardedPortFlag == 0 || right.context == cpu[3] {
                    guestReceiveRights.removeValue(forKey: name)
                    cpu[0] = UInt64(KERN_SUCCESS)
                } else {
                    throw GuestError("Guest Mach port destruction requires rights tracking for name 0x\(String(name, radix: 16))")
                }
            case -27:
                cpu[0] = guestMachThreadName(current.id)
            case -28:
                cpu[0] = guestTaskPort
            default:
                throw GuestError("Unsupported guest Darwin trap \(number) at 0x\(location)")
            }
            region = try compile(at: Int(cpu[33]))
        case 5:
            throw region.faults[Int(truncatingIfNeeded: cpu[33])] ?? GuestError("Guest reached untranslated code")
        case 6:
            throw GuestError("Guest return authentication failed")
        case 9, 10, 12:
            let kind = cpu[34] == 9 ? "data" : (cpu[34] == 10 ? "branch" : "code")
            if trace {
                let pointer = cpu[guestFaultAddress]
                let modifier = cpu[guestCacheFaultAddress]
                let index = Int((pointer & 0xffff_ffff) >> guestTokenIndexShift)
                let detail = "Guest \(kind) PAC operand 0x\(String(pointer, radix: 16)), modifier 0x\(String(modifier, radix: 16)), x0 0x\(String(cpu[0], radix: 16))\n"
                FileHandle.standardError.write(Data(detail.utf8))
                if pointer & ~0xffff_ffff == guestTokenTag,
                   index > 0, UInt64(index) <= cpu[guestTokenCount] {
                    let slot = guestTokenStart + 3 * (index - 1)
                    let token = "Guest \(kind) PAC token target 0x\(String(cpu[slot], radix: 16)), modifier 0x\(String(cpu[slot + 1], radix: 16)), key \(cpu[slot + 2])\n"
                    FileHandle.standardError.write(Data(token.utf8))
                }
            }
            let operation = cpu[34] == 10 ? "branch" : "\(kind)-pointer"
            throw GuestError("Guest \(operation) authentication failed at 0x\(String(cpu[33], radix: 16))")
        case 7:
            region = try compile(at: Int(truncatingIfNeeded: cpu[33]))
            if let status = try deliverSignal() { return status }
        case 4:
            switch cpu[35] {
            case 1:
                try FileHandle.standardOutput.write(contentsOf: Data(try guestString(cpu[0])) + Data([10]))
                cpu[0] = 0
            case 2:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                let standardOutput = descriptor == STDOUT_FILENO || descriptor == STDERR_FILENO
                let isOpenStandardOutput = standardOutput && standardSlots[descriptor].map { !closedStreams.contains($0) } == true
                guard isOpenStandardOutput || writableDescriptors.contains(descriptor) else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = .max
                    break
                }
                let length = cpu[2]
                let source = try guestBuffer(cpu[1], length)
                if length > 0, !readyForNonblockingIO(descriptor, events: Int16(POLLOUT)) {
                    writeGlobal(UInt64(EAGAIN), errnoSlot)
                    cpu[0] = .max
                    break
                }
                let count = standardStatusFlags[descriptor, default: 0] & O_NONBLOCK != 0
                    ? min(Int(length), Int(PIPE_BUF)) : Int(length)
                let result = Darwin.write(descriptor, source, count)
                if result < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(bitPattern: Int64(result))
            case 3:
                return Int32(truncatingIfNeeded: cpu[0])
            case 4:
                let name = String(decoding: try guestString(cpu[0]), as: UTF8.self)
                cpu[0] = environmentPointers[name] ?? 0
            case 5:
                cpu[0] = UInt64(try guestString(cpu[0]).count)
            case 6, 90:
                let byte = UInt8(truncatingIfNeeded: cpu[0])
                let stream = cpu[35] == 6 ? UInt64(stdoutSlot) : cpu[1]
                guard !closedStreams.contains(stream), stream == UInt64(stdoutSlot) || stream == UInt64(stderrSlot) else {
                    throw GuestError("Guest character output stream unsupported or closed")
                }
                let target = stream == UInt64(stdoutSlot) ? FileHandle.standardOutput : FileHandle.standardError
                try target.write(contentsOf: Data([byte]))
                cpu[0] = UInt64(byte)
            case 7:
                // The launcher models the process's initial C locale.
                var state = mbstate_t()
                var output = [CChar](repeating: 0, count: 32)
                let length = wcrtomb(&output, Int32(truncatingIfNeeded: cpu[0]), &state)
                if length >= 0, length <= output.count {
                    try FileHandle.standardOutput.write(contentsOf: Data(output.prefix(length).map { UInt8(bitPattern: $0) }))
                } else {
                    cpu[0] = UInt64(UInt32.max)
                }
            case 8:
                let output = cpu[0], address = cpu[1], length = cpu[2]
                let extent = try guestExtent(address)
                guard output.isMultiple(of: 4), extent.length > 0, length > 0 else {
                    throw GuestError("Guest mbtowc arguments unsupported")
                }
                let destination = try guestBuffer(output, 4, writable: true)
                let count = min(length, UInt64(extent.length))
                let input = try guestBuffer(address, count).assumingMemoryBound(to: CChar.self)
                var wide: wchar_t = 0
                let converted = mbtowc(&wide, input, Int(count))
                if converted >= 0 {
                    destination.storeBytes(of: wide, as: wchar_t.self)
                }
                cpu[0] = UInt64(UInt32(bitPattern: converted))
            case 9:
                guard cpu[0] == UInt64(stdoutSlot) || cpu[0] == UInt64(stderrSlot) || cpu[0] == 0 else {
                    throw GuestError("Guest fflush stream unsupported")
                }
                // The host bridge writes immediately and holds no stdio buffer.
                cpu[0] = 0
            case 10, 54:
                let options = try guestString(cpu[2])
                let argc = min(cpu[0], UInt64(Int32.max))
                if cpu[35] == 54, optionOffset == 1, UInt64(optionIndex) < argc {
                    let pointer = try guestBuffer(cpu[1] + UInt64(optionIndex) * 8, 8).loadUnaligned(as: UInt64.self)
                    let argument = try guestString(pointer)
                    if argument.count > 2, argument[0] == 45, argument[1] == 45 {
                        let end = argument.firstIndex(of: 61) ?? argument.count
                        let name = argument[2..<end]
                        let table = cpu[3], indexPointer = cpu[4]
                        var exact: (Int, UnsafeMutableRawPointer)?
                        var prefix = [(Int, UnsafeMutableRawPointer)]()
                        for index in 0..<4096 {
                            let record = try guestBuffer(table + UInt64(index * 32), 32)
                            let address = record.loadUnaligned(as: UInt64.self)
                            if address == 0 { break }
                            let candidate = try guestString(address)
                            if candidate.elementsEqual(name) { exact = (index, record); break }
                            if candidate.starts(with: name) { prefix.append((index, record)) }
                        }
                        optionIndex += 1
                        cpu[0] = 63
                        writeGlobal(0, optargSlot)
                        if let (index, record) = exact ?? (prefix.count == 1 ? prefix[0] : nil) {
                            let requires = record.loadUnaligned(fromByteOffset: 8, as: Int32.self)
                            let flag = record.loadUnaligned(fromByteOffset: 16, as: UInt64.self)
                            let value = record.loadUnaligned(fromByteOffset: 24, as: UInt32.self)
                            let attached = end < argument.count
                            if attached && requires == 0 {
                                cpu[0] = 63
                            } else if requires == 1 && !attached && UInt64(optionIndex) >= argc {
                                cpu[0] = options.first == 58 ? 58 : 63
                            } else {
                                if attached {
                                    writeGlobal(pointer + UInt64(end + 1), optargSlot)
                                } else if requires == 1 {
                                    let next = try guestBuffer(cpu[1] + UInt64(optionIndex) * 8, 8)
                                        .loadUnaligned(as: UInt64.self)
                                    writeGlobal(next, optargSlot)
                                    optionIndex += 1
                                }
                                if flag != 0 {
                                    let destination = try guestBuffer(flag, 4, writable: true)
                                    withUnsafeBytes(of: value) { destination.copyMemory(from: $0.baseAddress!, byteCount: 4) }
                                }
                                if indexPointer != 0 {
                                    let destination = try guestBuffer(indexPointer, 4, writable: true)
                                    withUnsafeBytes(of: UInt32(index)) { destination.copyMemory(from: $0.baseAddress!, byteCount: 4) }
                                }
                                cpu[0] = flag == 0 ? UInt64(value) : 0
                            }
                        }
                        writeGlobal(UInt64(optionIndex), optindSlot)
                        break
                    }
                }
                cpu[0] = UInt64(UInt32.max)
                if UInt64(optionIndex) < argc {
                    let pointer = try guestBuffer(cpu[1] + UInt64(optionIndex) * 8, 8).loadUnaligned(as: UInt64.self)
                    let argument = try guestString(pointer)
                    if optionOffset < argument.count, argument[0] == 45 {
                        if argument.elementsEqual([45, 45]) {
                            optionIndex += 1
                        } else {
                            let option = argument[optionOffset]
                            optionOffset += 1
                            if let index = options.firstIndex(of: option), option != 58 {
                                cpu[0] = UInt64(option)
                                if index + 1 < options.count, options[index + 1] == 58 {
                                    if optionOffset < argument.count {
                                        writeGlobal(pointer + UInt64(optionOffset), optargSlot)
                                    } else if UInt64(optionIndex + 1) < argc {
                                        optionIndex += 1
                                        let value = try guestBuffer(cpu[1] + UInt64(optionIndex) * 8, 8)
                                            .loadUnaligned(as: UInt64.self)
                                        writeGlobal(value, optargSlot)
                                    } else {
                                        cpu[0] = options.first == 58 ? 58 : 63
                                    }
                                    optionOffset = argument.count
                                }
                            } else {
                                cpu[0] = 63
                            }
                            if optionOffset >= argument.count {
                                optionIndex += 1
                                optionOffset = 1
                            }
                        }
                    }
                }
                writeGlobal(UInt64(optionIndex), optindSlot)
            case 11:
                let left = try guestBuffer(cpu[0], cpu[2])
                let right = try guestBuffer(cpu[1], cpu[2])
                cpu[0] = UInt64(UInt32(bitPattern: memcmp(left, right, Int(cpu[2]))))
            case 12:
                let address = cpu[0]
                let destination = try guestBuffer(address, cpu[1], writable: true).assumingMemoryBound(to: CChar.self)
                cpu[0] = Darwin.getcwd(destination, Int(cpu[1])) == nil ? 0 : address
            case 13:
                let path = try guestString(cpu[0])
                guard let base = path.baseAddress else { throw GuestError("Missing guest path") }
                var info = stat()
                let result = base.withMemoryRebound(to: CChar.self, capacity: path.count + 1) { stat($0, &info) }
                if result == 0 {
                    let destination = try guestBuffer(cpu[1], UInt64(MemoryLayout<stat>.size), writable: true)
                    withUnsafeBytes(of: info) { destination.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 14:
                let (length, overflow) = cpu[1].multipliedReportingOverflow(by: cpu[2])
                guard !overflow else { throw GuestError("Guest fwrite length overflow") }
                let pointer = try guestBuffer(cpu[0], length)
                let handle: FileHandle
                guard !closedStreams.contains(cpu[3]) else { throw GuestError("Guest fwrite stream closed") }
                if cpu[3] == UInt64(stdoutSlot) { handle = .standardOutput }
                else if cpu[3] == UInt64(stderrSlot) { handle = .standardError }
                else { throw GuestError("Guest fwrite stream unsupported") }
                try handle.write(contentsOf: Data(bytes: pointer, count: Int(length)))
                cpu[0] = length == 0 ? 0 : cpu[2]
            case 15, 16:
                var path = Array(try guestString(cpu[0])).map { CChar(bitPattern: $0) }
                path.append(0)
                cpu[0] = try path.withUnsafeMutableBufferPointer { pointer in
                    try copyHostString(cpu[35] == 15 ? dirname(pointer.baseAddress) : basename(pointer.baseAddress))
                }
            case 17:
                if let file = guestFiles[cpu[0]] {
                    cpu[0] = UInt64(UInt32(bitPattern: Darwin.ferror(file)))
                    break
                }
                if cpu[0] == UInt64(stdinSlot), !closedStreams.contains(cpu[0]) {
                    cpu[0] = UInt64(UInt32(bitPattern: Darwin.ferror(Darwin.stdin)))
                    break
                }
                guard cpu[0] == UInt64(stdoutSlot) || cpu[0] == UInt64(stderrSlot) else {
                    throw GuestError("Guest ferror stream unsupported")
                }
                cpu[0] = 0
            case 18:
                let category = Int32(truncatingIfNeeded: cpu[0])
                let address = cpu[1]
                let requested = address == 0 ? nil : try guestBuffer(address, 1).assumingMemoryBound(to: CChar.self)
                if address != 0 { _ = try guestString(address) }
                cpu[0] = try copyHostString(setlocale(category, requested))
                writeGlobal(UInt64(___mb_cur_max()), localeSlot)
            case 19:
                let left = try guestString(cpu[0]), right = try guestString(cpu[1])
                let count = min(left.count, right.count)
                let difference = (0..<count).lazy.map { Int(left[$0]) - Int(right[$0]) }.first { $0 != 0 }
                cpu[0] = UInt64(UInt32(bitPattern: Int32(difference ?? (left.count - right.count))))
            case 20:
                let input = try guestBuffer(cpu[0], cpu[1]).assumingMemoryBound(to: CChar.self)
                let size = UInt64(MemoryLayout<mbstate_t>.size)
                let state = try guestBuffer(cpu[2], size, writable: true).assumingMemoryBound(to: mbstate_t.self)
                cpu[0] = UInt64(bitPattern: Int64(mbrlen(input, Int(cpu[1]), state)))
            case 21:
                let path = try guestString(cpu[0])
                let flags = Int32(truncatingIfNeeded: cpu[1])
                guard flags & ~(O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_DIRECTORY) == O_RDONLY else {
                    throw GuestError("Guest open flags unsupported: \(flags)")
                }
                let result = path.baseAddress!.withMemoryRebound(to: CChar.self, capacity: path.count + 1) {
                    Darwin.open($0, flags | O_CLOEXEC)
                }
                if result >= 0 { openDescriptors.insert(result) }
                else { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 22:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                if let slot = standardSlots[descriptor] {
                    if closedStreams.insert(slot).inserted { cpu[0] = 0 }
                    else {
                        writeGlobal(UInt64(EBADF), errnoSlot)
                        cpu[0] = UInt64(UInt32.max)
                    }
                } else if openDescriptors.remove(descriptor) != nil {
                    writableDescriptors.remove(descriptor)
                    let result = Darwin.close(descriptor)
                    if result < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                    cpu[0] = UInt64(UInt32(bitPattern: result))
                } else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                }
            case 23:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                guard (descriptor == STDIN_FILENO && !closedStreams.contains(UInt64(stdinSlot))) ||
                    ownsDescriptor(descriptor) else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = .max
                    break
                }
                let buffer = try guestBuffer(cpu[1], cpu[2], writable: true)
                if cpu[2] > 0, !readyForNonblockingIO(descriptor, events: Int16(POLLIN)) {
                    writeGlobal(UInt64(EAGAIN), errnoSlot)
                    cpu[0] = .max
                    break
                }
                let result = Darwin.read(descriptor, buffer, Int(cpu[2]))
                if result < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(bitPattern: Int64(result))
            case 24:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                guard standardSlots[descriptor].map({ !closedStreams.contains($0) }) == true ||
                    ownsDescriptor(descriptor) else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                var info = stat()
                let result = Darwin.fstat(descriptor, &info)
                if result == 0 {
                    let destination = try guestBuffer(cpu[1], UInt64(MemoryLayout<stat>.size), writable: true)
                    withUnsafeBytes(of: info) { destination.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                } else {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 25:
                let stream = cpu[0]
                if let file = guestFiles[stream] {
                    let descriptor = Darwin.fileno(file)
                    if descriptor < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                    cpu[0] = UInt64(UInt32(bitPattern: descriptor))
                } else if let descriptor = standardSlots.first(where: { $0.value == stream })?.key,
                          !closedStreams.contains(stream) {
                    cpu[0] = UInt64(descriptor)
                } else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                }
            case 26, 40:
                let (size, overflow) = cpu[0].multipliedReportingOverflow(by: cpu[35] == 40 ? cpu[1] : 1)
                if !overflow, let address = allocateGuest(size) {
                    if cpu[35] == 40 {
                        memset(memory.contents().advanced(by: address), 0, Int(size))
                    }
                    cpu[0] = UInt64(address)
                } else {
                    cpu[0] = 0
                    writeGlobal(UInt64(ENOMEM), errnoSlot)
                }
            case 27:
                cpu[0] = UInt64(bitPattern: Int64(Darwin.sysconf(Int32(truncatingIfNeeded: cpu[0]))))
            case 28:
                cpu[0] = UInt64(errnoSlot)
            case 29:
                let string = try guestString(cpu[0])
                let byte = UInt8(truncatingIfNeeded: cpu[1])
                if byte == 0 { cpu[0] += UInt64(string.count) }
                else if let index = string.lastIndex(of: byte) { cpu[0] += UInt64(index) }
                else { cpu[0] = 0 }
            case 30:
                cpu[0] = programName
            case 31:
                if let file = guestFiles.removeValue(forKey: cpu[0]) {
                    let result = Darwin.fclose(file)
                    if result != 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                    cpu[0] = UInt64(UInt32(bitPattern: result))
                    break
                }
                guard [UInt64(stdinSlot), UInt64(stdoutSlot), UInt64(stderrSlot)].contains(cpu[0]),
                      closedStreams.insert(cpu[0]).inserted else {
                    throw GuestError("Guest fclose stream unsupported or closed")
                }
                cpu[0] = 0
            case 32:
                let floor = UInt64(current.stack.lowerBound)
                guard cpu[31] >= floor, cpu[31] <= UInt64(current.stack.upperBound),
                      cpu[9] <= cpu[31] - floor else { throw GuestError("Guest stack probe failed") }
            case 33, 55, 89, 93, 94, 97:
                let checked = cpu[35] == 94
                let warning = cpu[35] == 97
                let toBuffer = cpu[35] == 93 || checked
                let toStream = cpu[35] == 55 || cpu[35] == 89
                let stream = warning ? UInt64(stderrSlot) : toStream ? cpu[0] : UInt64(stdoutSlot)
                guard toBuffer || (!closedStreams.contains(stream) &&
                      (stream == UInt64(stdoutSlot) || stream == UInt64(stderrSlot))) else {
                    throw GuestError("Guest printf stream unsupported")
                }
                guard !checked || cpu[1] <= cpu[3] else { throw GuestError("Guest snprintf exceeds object size") }
                var offset = warning ? cpu[1] : cpu[35] == 89 ? cpu[2] : cpu[31]
                let formatIndex = checked ? 4 : toBuffer ? 2 : toStream ? 1 : 0
                let format = warning && cpu[0] == 0 ? [] : Array(try guestString(cpu[formatIndex]))
                let output = try formatGuestOutput(format, argument: {
                    let value = try guestBuffer(offset, 8).loadUnaligned(as: UInt64.self)
                    offset += 8
                    return value
                }, string: { Array(try guestString($0)) })
                if toBuffer {
                    let capacity = cpu[1]
                    if capacity > 0 {
                        let count = Int(min(UInt64(output.count), capacity - 1))
                        let destination = try guestBuffer(cpu[0], UInt64(count + 1), writable: true)
                        if count > 0 {
                            output.withUnsafeBytes { destination.copyMemory(from: $0.baseAddress!, byteCount: count) }
                        }
                        destination.storeBytes(of: UInt8(0), toByteOffset: count, as: UInt8.self)
                    }
                } else {
                    let target = stream == UInt64(stdoutSlot) ? FileHandle.standardOutput : FileHandle.standardError
                    if warning {
                        var line = Array(try guestString(programName))
                        if cpu[0] != 0 { line += [58, 32] + output }
                        line.append(10)
                        try target.write(contentsOf: Data(line))
                    } else {
                        try target.write(contentsOf: Data(output))
                    }
                }
                cpu[0] = UInt64(output.count)
            case 34, 35:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                if standardSlots[descriptor].map({ !closedStreams.contains($0) }) != true &&
                    !ownsDescriptor(descriptor) {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = cpu[35] == 34 ? UInt64(UInt32.max) : 0
                    break
                }
                let result: Int32
                if cpu[35] == 34 {
                    let operation = Int32(truncatingIfNeeded: cpu[1])
                    switch operation {
                    case F_GETFD:
                        result = standardDescriptorFlags[descriptor] ?? Darwin.fcntl(descriptor, operation)
                    case F_GETFL:
                        let native = Darwin.fcntl(descriptor, operation)
                        if native >= 0, let shadow = standardStatusFlags[descriptor] {
                            result = (native & ~O_NONBLOCK) | (shadow & O_NONBLOCK)
                        } else { result = native }
                    case F_SETFD, F_SETFL:
                        let value = try guestBuffer(cpu[31], 4).loadUnaligned(as: Int32.self)
                        if standardSlots[descriptor] != nil {
                            if operation == F_SETFD { standardDescriptorFlags[descriptor] = value }
                            let native = operation == F_SETFL ? Darwin.fcntl(descriptor, F_GETFL) : 0
                            if native < 0 {
                                result = native
                            } else {
                                guard operation == F_SETFD ||
                                    (native ^ value) & (O_APPEND | O_ASYNC) == 0 else {
                                    throw GuestError("Guest cannot change append or async mode on an inherited descriptor")
                                }
                                if operation == F_SETFL { standardStatusFlags[descriptor] = value }
                                result = 0
                            }
                        } else {
                            result = Darwin.fcntl(descriptor, operation, value)
                        }
                    default:
                        throw GuestError("Guest fcntl command \(operation) unsupported")
                    }
                } else {
                    result = Darwin.isatty(descriptor)
                }
                if cpu[35] == 34 ? result < 0 : result == 0 {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 36:
                cpu[0] = UInt64(Darwin.getuid())
            case 37:
                cpu[0] = UInt64(Darwin.geteuid())
            case 38:
                cpu[0] = UInt64(Darwin.getgid())
            case 39:
                cpu[0] = UInt64(Darwin.getegid())
            case 41:
                var limit = rlimit()
                let result = Darwin.getrlimit(Int32(truncatingIfNeeded: cpu[0]), &limit)
                if result == 0 {
                    let destination = try guestBuffer(cpu[1], UInt64(MemoryLayout<rlimit>.size), writable: true)
                    withUnsafeBytes(of: limit) { destination.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                } else {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 42:
                let destination = try guestBuffer(cpu[0], 8, writable: true)
                var descriptors = [Int32](repeating: -1, count: 2)
                let result = Darwin.pipe(&descriptors)
                if result == 0 {
                    openDescriptors.formUnion(descriptors)
                    writableDescriptors.insert(descriptors[1])
                    withUnsafeBytes(of: descriptors[0]) { destination.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                    withUnsafeBytes(of: descriptors[1]) { destination.advanced(by: 4).copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                } else {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 43, 91:
                let signal = Int32(truncatingIfNeeded: cpu[0])
                let simple = cpu[35] == 91
                let requested = cpu[1], previous = simple ? 0 : cpu[2]
                let failure = simple ? UInt64.max : UInt64(UInt32.max)
                guard GuestSignals.valid(signal),
                      (!simple && requested == 0) || (signal != SIGKILL && signal != SIGSTOP) else {
                    writeGlobal(UInt64(EINVAL), errnoSlot)
                    cpu[0] = failure
                    break
                }
                let old = signals.action(signal)
                var action: GuestSignals.Action?
                if simple {
                    action = GuestSignals.Action(handler: requested, flags: SA_RESTART)
                } else if requested != 0 {
                    let record = try guestBuffer(requested, 16)
                    let newAction = GuestSignals.Action(handler: record.loadUnaligned(as: UInt64.self),
                                                 mask: record.loadUnaligned(fromByteOffset: 8, as: UInt32.self),
                                                 flags: record.loadUnaligned(fromByteOffset: 12, as: Int32.self))
                    let supported = SA_RESTART | SA_RESETHAND | SA_NODEFER
                    guard newAction.flags & ~supported == 0 else {
                        throw GuestError("Guest sigaction flags unsupported: \(newAction.flags)")
                    }
                    action = newAction
                }
                if previous != 0 {
                    let record = try guestBuffer(previous, 16, writable: true)
                    var packed = [old.handler, UInt64(old.mask) | UInt64(UInt32(bitPattern: old.flags)) << 32]
                    packed.withUnsafeMutableBytes { record.copyMemory(from: $0.baseAddress!, byteCount: 16) }
                }
                if let action {
                    if guest_signal_listen(signal, action.handler == 1 ? 1 : 0, action.flags & SA_RESTART == 0 ? 0 : 1) != 0 {
                        writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                        cpu[0] = failure
                        break
                    }
                    signals.setAction(action, for: signal)
                    if action.handler == 1 { queuedProcessSignals &= ~GuestSignals.bit(signal) }
                    if action.handler == 1 {
                        for thread in threads.values { thread.pendingSignals &= ~GuestSignals.bit(signal) }
                    }
                }
                cpu[0] = simple ? old.handler : 0
            case 44, 45:
                let how = Int32(truncatingIfNeeded: cpu[0])
                let requested = cpu[1], previous = cpu[2]
                let oldMask = signals.mask
                if requested != 0 {
                    let value = try guestBuffer(requested, 4).loadUnaligned(as: UInt32.self)
                    guard signals.setMask(how, value) else {
                        if cpu[35] == 44 { writeGlobal(UInt64(EINVAL), errnoSlot) }
                        cpu[0] = cpu[35] == 44 ? UInt64(UInt32.max) : UInt64(EINVAL)
                        break
                    }
                }
                if previous != 0 {
                    let record = try guestBuffer(previous, 4, writable: true)
                    withUnsafeBytes(of: oldMask) { record.copyMemory(from: $0.baseAddress!, byteCount: 4) }
                }
                cpu[0] = 0
            case 46, 47:
                let isKill = cpu[35] == 47
                let signal = Int32(truncatingIfNeeded: isKill ? cpu[1] : cpu[0])
                guard !isKill || Int32(truncatingIfNeeded: cpu[0]) == Darwin.getpid() else {
                    throw GuestError("Guest kill only supports the guest process")
                }
                guard GuestSignals.valid(signal) || (isKill && signal == 0) else {
                    writeGlobal(UInt64(EINVAL), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                if signal != 0 {
                    if isKill {
                        signals.raise(signal)
                        queueProcessSignals(GuestSignals.bit(signal))
                    }
                    else if signals.action(signal).handler != 1 { current.pendingSignals |= GuestSignals.bit(signal) }
                }
                cpu[0] = 0
            case 48:
                cpu[0] = UInt64(Darwin.getpid())
            case 49, 50:
                let named = cpu[35] == 49
                let outputIndex = named ? 1 : 2
                guard cpu[outputIndex + 2] == 0, cpu[outputIndex + 3] == 0 else {
                    throw GuestError("Guest sysctl writes are unsupported")
                }
                let length = try guestBuffer(cpu[outputIndex + 1], 8, writable: true)
                var count = length.loadUnaligned(as: Int.self)
                guard count >= 0 else { throw GuestError("Invalid guest sysctl length") }
                let output = cpu[outputIndex] == 0 ? nil : try guestBuffer(cpu[outputIndex], UInt64(count), writable: true)
                let result: Int32
                if named {
                    let name = try guestString(cpu[0])
                    result = name.baseAddress!.withMemoryRebound(to: CChar.self, capacity: name.count + 1) {
                        Darwin.sysctlbyname($0, output, &count, nil, 0)
                    }
                } else {
                    guard cpu[1] > 0, cpu[1] <= 64 else { throw GuestError("Invalid guest sysctl MIB length") }
                    let source = try guestBuffer(cpu[0], cpu[1] * 4)
                    var mib = (0..<Int(cpu[1])).map { source.loadUnaligned(fromByteOffset: $0 * 4, as: Int32.self) }
                    result = Darwin.sysctl(&mib, UInt32(mib.count), output, &count, nil, 0)
                }
                let error = errno
                withUnsafeBytes(of: count) { length.copyMemory(from: $0.baseAddress!, byteCount: 8) }
                if result != 0 { writeGlobal(UInt64(UInt32(bitPattern: error)), errnoSlot) }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 51:
                let limit = cpu[2]
                var result: Int32 = 0
                if limit > 0 {
                    let leftRange = try guestExtent(cpu[0]), rightRange = try guestExtent(cpu[1])
                    let extent = min(limit, UInt64(leftRange.length), UInt64(rightRange.length))
                    let bytes = memory.contents().assumingMemoryBound(to: UInt8.self)
                    let left = bytes + leftRange.offset, right = bytes + rightRange.offset
                    var terminated = false
                    for index in 0..<Int(extent) {
                        result = Int32(left[index]) - Int32(right[index])
                        if result != 0 || left[index] == 0 { terminated = true; break }
                    }
                    guard terminated || extent == limit else { throw GuestError("Guest strncmp exceeds memory") }
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 52:
                let request = try guestBuffer(cpu[0], UInt64(MemoryLayout<timespec>.size))
                let interval = request.loadUnaligned(as: timespec.self)
                if !signalFrames.isEmpty {
                    var hostInterval = interval
                    let remainder = cpu[1] == 0 ? nil : try guestBuffer(cpu[1], UInt64(MemoryLayout<timespec>.size), writable: true)
                    var remaining = timespec()
                    let result = Darwin.nanosleep(&hostInterval, &remaining)
                    if result != 0 {
                        let error = errno
                        if error == EINTR, let remainder {
                            withUnsafeBytes(of: remaining) { remainder.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                        }
                        writeGlobal(UInt64(UInt32(bitPattern: error)), errnoSlot)
                    }
                    cpu[0] = UInt64(UInt32(bitPattern: result))
                    break
                }
                if cpu[1] != 0 {
                    _ = try guestBuffer(cpu[1], UInt64(MemoryLayout<timespec>.size), writable: true)
                }
                guard let deadline = timedWaitDeadline(interval, relative: true) else {
                    writeGlobal(UInt64(EINVAL), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                if deadline <= DispatchTime.now().uptimeNanoseconds {
                    cpu[0] = 0
                } else {
                    current.wait = .sleep(deadline: deadline, remainder: cpu[1] == 0 ? nil : cpu[1])
                    try schedule(requeue: false)
                    continue
                }
            case 53:
                let haystack = try guestString(cpu[0]), needle = try guestString(cpu[1])
                let match = haystack.baseAddress!.withMemoryRebound(to: CChar.self, capacity: haystack.count + 1) { left in
                    needle.baseAddress!.withMemoryRebound(to: CChar.self, capacity: needle.count + 1) { right in
                        strstr(left, right).map { left.distance(to: UnsafePointer($0)) }
                    }
                }
                cpu[0] = match.map { cpu[0] + UInt64($0) } ?? 0
            case 56:
                guard let address = Int(exactly: cpu[0]) else { throw GuestError("Invalid guest heap pointer") }
                try heap.release(address)
            case 146:
                cpu[0] = UInt64(Int(exactly: cpu[0]).flatMap { heap.size(of: $0) } ?? 0)
            case 57:
                guard let address = Int(exactly: cpu[0]) else { throw GuestError("Invalid guest heap pointer") }
                let requested = cpu[1]
                let prior: Int
                if address == 0 { prior = 0 }
                else if let size = heap.size(of: address) { prior = size }
                else { throw GuestError("Invalid guest heap pointer") }
                if address != 0, requested > 0, requested <= UInt64(prior) {
                    cpu[0] = UInt64(address)
                } else if let target = allocateGuest(requested) {
                    if prior > 0 {
                        let source = memory.contents().advanced(by: address)
                        memory.contents().advanced(by: target).copyMemory(from: source, byteCount: min(prior, Int(requested)))
                        try heap.release(address)
                    }
                    cpu[0] = UInt64(target)
                } else {
                    cpu[0] = 0
                    writeGlobal(UInt64(ENOMEM), errnoSlot)
                }
            case 58:
                var info = mach_timebase_info_data_t()
                let result = mach_timebase_info(&info)
                if result == KERN_SUCCESS {
                    let output = try guestBuffer(cpu[0], UInt64(MemoryLayout<mach_timebase_info_data_t>.size), writable: true)
                    withUnsafeBytes(of: info) { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 59:
                cpu[0] = mach_absolute_time()
            case 137:
                cpu[0] = mach_continuous_time()
            case 141:
                cpu[0] = signing.restricted ? 1 : 0
            case 143:
                cpu[0] = UInt64(image.swiftPrespecializedData ?? 0)
            case 147:
                cpu[0] = 1 // Version of the dyld Swift optimization API.
            case 148:
                // The guest image graph has no process prebuilt loader set.
                cpu[0] = 0
            case 149:
                let protocolAddress = cpu[0], metadata = cpu[1], type = cpu[2]
                cpu[0] = 2 // dyld's non-definitive "not found" result.
                cpu[1] = 0
                guard let reader = image.cacheReader else { break }
                let descriptor = try reader.swiftConformance(protocolAddress: protocolAddress, metadata: metadata, type: type) { index in
                    guard let header = try reader.objcImageHeader(at: index) else { return false }
                    return image.imagePaths[UInt64(header)] != nil
                }
                if let descriptor {
                    cpu[0] = 0
                    cpu[1] = UInt64(descriptor)
                }
            case 145:
                let owner = image.imagePaths[cpu[1]] ?? "unknown"
                guard let reader = image.cacheReader, cpu[1] >= UInt64(SharedCache.guestBase),
                      let dependency = try reader.preboundLazyDependency(owner: owner, flag: cpu[0]) else {
                    throw GuestError("Unsupported guest lazy-load flag 0x\(String(cpu[0], radix: 16)) in \(owner)")
                }
                guard imageHeaders[dependency] != nil else {
                    throw GuestError("Guest lazy dependency is not loaded: \(dependency)")
                }
                // dyld is permitted to update this validated flag in cache DATA_CONST.
                let flag = try guestBuffer(cpu[0], 4)
                flag.storeBytes(of: UInt32(1).littleEndian, as: UInt32.self)
                if trace {
                    FileHandle.standardError.write(Data("Guest prebound lazy dependency \(dependency) for \(owner)\n".utf8))
                }
            case 144:
                let table = cpu[0]
                cpu[0] = 0
                guard let reader = image.cacheReader, image.swiftPrespecializedData != nil,
                      cpu[2] < 64 else { break }
                let base = UInt64(SharedCache.guestBase)
                let range = base..<(base + reader.mappedSize)
                guard range.contains(table), range.contains(cpu[1]) else { break }
                var keys = [cpu[1] - base]
                if cpu[2] != 0 {
                    let rest = try guestBuffer(cpu[3], cpu[2] * 8)
                    for index in 0..<Int(cpu[2]) {
                        let key = rest.loadUnaligned(fromByteOffset: index * 8, as: UInt64.self)
                        if !range.contains(key) { break }
                        keys.append(key - base)
                    }
                    guard keys.count == Int(cpu[2]) + 1 else { break }
                }
                var offset: UInt64 = 0
                let result = reader.uuid.withUnsafeBytes {
                    guest_cached_pointer_lookup(table - base, keys, UInt32(keys.count),
                                                $0.bindMemory(to: UInt8.self).baseAddress!, &offset)
                }
                guard result >= 0 else { throw GuestError("Host and guest Swift pointer caches do not match") }
                if result == 1 { cpu[0] = base + offset }
            case 142:
                let capacity = try guestBuffer(cpu[1], 4).loadUnaligned(as: UInt32.self)
                let required = UInt32(executablePathBytes.count)
                if capacity < required {
                    let size = try guestBuffer(cpu[1], 4, writable: true)
                    var value = required.littleEndian
                    withUnsafeBytes(of: &value) { size.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                    cpu[0] = UInt64(UInt32.max)
                } else {
                    let output = try guestBuffer(cpu[0], UInt64(required), writable: true)
                    executablePathBytes.withUnsafeBytes { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                    cpu[0] = 0
                }
            case 138:
                let domain = String(decoding: try guestString(cpu[0]), as: UTF8.self)
                let feature = String(decoding: try guestString(cpu[1]), as: UTF8.self)
                let enabled = guest_feature_enabled(domain, feature)
                if trace {
                    let message = "Guest feature flag \(domain)/\(feature) = \(enabled)\n"
                    FileHandle.standardError.write(Data(message.utf8))
                }
                cpu[0] = UInt64(enabled)
            case 60:
                cpu[0] = UInt64(bitPattern: Int64(Darwin.clock()))
            case 61:
                let output = try guestBuffer(cpu[1], UInt64(MemoryLayout<timespec>.size), writable: true)
                var time = timespec()
                let result = clock_gettime(clockid_t(rawValue: UInt32(truncatingIfNeeded: cpu[0])), &time)
                if result == 0 {
                    withUnsafeBytes(of: time) { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                } else {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 118, 119:
                let commpage = cpu[35] == 119
                let output = !commpage && cpu[0] == 0 ? nil :
                    try guestBuffer(cpu[0], UInt64(MemoryLayout<timeval>.size), writable: true)
                let zoneOutput = commpage || cpu[1] == 0 ? nil : try guestBuffer(cpu[1], 8, writable: true)
                var time = timeval()
                var zone = [Int32](repeating: 0, count: 2)
                let result = zone.withUnsafeMutableBytes { zoneBytes in
                    let destination = zoneOutput == nil ? nil : zoneBytes.baseAddress
                    if output != nil { return Darwin.gettimeofday(&time, destination) }
                    return Darwin.gettimeofday(nil, destination)
                }
                if result == 0 {
                    if let output { withUnsafeBytes(of: time) { output.copyMemory(from: $0.baseAddress!, byteCount: $0.count) } }
                    if let zoneOutput { zone.withUnsafeBytes { zoneOutput.copyMemory(from: $0.baseAddress!, byteCount: $0.count) } }
                } else if !commpage {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = commpage && result != 0 ? 1 : UInt64(UInt32(bitPattern: result))
            case 120:
                let length = cpu[1]
                if length == 0 { cpu[0] = 0; break }
                guard length <= 256 else {
                    writeGlobal(UInt64(EINVAL), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                guard cpu[0] != 0, let output = try? guestBuffer(cpu[0], length, writable: true) else {
                    writeGlobal(UInt64(EFAULT), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                let result = guest_entropy(output, length)
                if result != 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 121:
                let address = cpu[0]
                let (limit, overflow) = address.addingReportingOverflow(cpu[1])
                guard !overflow, let start = Int(exactly: address), let end = Int(exactly: limit) else {
                    cpu[0] = 0
                    break
                }
                let immutable = image.immutableRegions.contains { $0.contains(start) && end <= $0.upperBound }
                    || image.cacheReader?.isImmutable(from: start, to: end) == true
                cpu[0] = immutable ? 1 : 0
            case 122:
                guard let reader = image.cacheReader, let table = try reader.objcMessageSendOffsets() else {
                    cpu[0] = 0
                    cpu[1] = 0
                    break
                }
                try publishCacheMetadata(reader, address: table.address, size: Int(table.count) * 8)
                cpu[0] = UInt64(table.address)
                cpu[1] = UInt64(table.count)
            case 127, 128:
                let writable = cpu[35] == 128
                guard let reader = image.cacheReader, let table = try reader.objcHeaderTable(writable: writable) else {
                    cpu[0] = 0
                    break
                }
                try publishCacheMetadata(reader, address: table.address, size: table.size, writable: writable)
                cpu[0] = UInt64(table.address)
            case 129:
                cpu[0] = UInt64(image.mainHeader)
            case 130:
                cpu[0] = UInt64(image.buildVersion?.platform ?? UInt32(PLATFORM_MACOS))
            case 132:
                // The guest loader resolves system install names from the cache
                // or its service bindings and has no cache override path.
                cpu[0] = 0
            case 134:
                cpu[0] = UInt64(try image.cacheReader?.objcClassCount() ?? 0)
            case 135:
                let mode = Int32(truncatingIfNeeded: cpu[1])
                guard cpu[0] != 0, mode & RTLD_NOLOAD != 0 else {
                    throw GuestError("Guest dlopen requires a named image and RTLD_NOLOAD")
                }
                let path = String(decoding: try guestString(cpu[0]), as: UTF8.self)
                var candidate = URL(fileURLWithPath: path)
                var header = imageHeaders[path]
                // A framework alias can point to a cache-only binary with no file at its final path.
                for _ in 0..<16 where header == nil {
                    let parent = candidate.deletingLastPathComponent().resolvingSymlinksInPath()
                    candidate = parent.appendingPathComponent(candidate.lastPathComponent)
                    header = imageHeaders[candidate.path]
                    guard header == nil,
                          let target = try? FileManager.default.destinationOfSymbolicLink(atPath: candidate.path) else { break }
                    candidate = URL(fileURLWithPath: target, relativeTo: parent).standardizedFileURL
                }
                guard let header else {
                    cpu[0] = 0
                    break
                }
                // Keep the handle outside the guest address space and distinguish RTLD_FIRST.
                let handle = (1 << 63) | header | (mode & RTLD_FIRST != 0 ? 1 : 0)
                openImageReferences[handle, default: 0] += 1
                cpu[0] = handle
            case 136:
                let handle = cpu[0]
                guard let count = openImageReferences[handle] else {
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                openImageReferences[handle] = count == 1 ? nil : count - 1
                cpu[0] = 0
            case 133:
                guard signalFrames.isEmpty else {
                    throw GuestError("Guest class enumeration inside a signal handler is unsupported")
                }
                guard let reader = image.cacheReader else { break }
                let blockAddress = cpu[1]
                let name = try guestString(cpu[0])
                var uuid = [UInt8](repeating: 0, count: 16)
                let offsets = try name.baseAddress!.withMemoryRebound(to: CChar.self, capacity: name.count + 1) { name in
                    let count = guest_cached_classes(name, nil, 0, &uuid)
                    guard count >= 0, reader.uuid.elementsEqual(uuid) else {
                        throw GuestError("Host and guest Objective-C class caches do not match")
                    }
                    var offsets = [UInt64](repeating: 0, count: Int(count))
                    if count > 0, guest_cached_classes(name, &offsets, count, &uuid) != count {
                        throw GuestError("Objective-C cache class enumeration changed")
                    }
                    return offsets
                }
                if offsets.isEmpty { break }
                let block = try guestBuffer(blockAddress, 24)
                let callback = try guestCodeAddress(block.loadUnaligned(fromByteOffset: 16, as: UInt64.self),
                                                    modifier: blockAddress + 16)
                guard let stop = allocateGuest(1) else { throw GuestError("Guest Objective-C class stop flag allocation failed") }
                memory.contents().storeBytes(of: UInt8(0), toByteOffset: stop, as: UInt8.self)
                let rw = try reader.objcHeaderTable(writable: true)
                if let rw { try publishCacheMetadata(reader, address: rw.address, size: rw.size, writable: true) }
                let arguments: [(UInt64, UInt64, UInt64, UInt64)] = try offsets.map { offset in
                    guard offset < reader.mappedSize else { throw GuestError("Invalid Objective-C cache class offset") }
                    let address = SharedCache.guestBase + Int(offset)
                    var loaded: UInt64 = 0
                    if let rw, let owner = image.cachedImageRanges.first(where: { $0.range.contains(address) }),
                       let index = try reader.objcHeaderIndex(owner.header), 8 + index * 8 <= rw.size - 8 {
                        let word = try guestBuffer(UInt64(rw.address + 8 + index * 8), 8)
                        loaded = word.loadUnaligned(as: UInt64.self) & 1
                    }
                    return (blockAddress, UInt64(address), loaded, UInt64(stop))
                }
                let notification = ImageNotification(callback: callback, arguments: arguments,
                    registers: Array(UnsafeBufferPointer(start: cpu, count: 36)),
                    vectors: Array(UnsafeBufferPointer(start: cpu + 806, count: guestTokenCount - 806)),
                    authenticationDepth: cpu[37], stopAddress: stop)
                if trace {
                    FileHandle.standardError.write(Data("Guest Objective-C class query returned \(offsets.count) cache candidates\n".utf8))
                }
                try continueImageNotification(notification)
            case 131:
                let index = Int(UInt32(truncatingIfNeeded: cpu[1]))
                guard let frame = imageNotifications[current.id]?.last,
                      frame.mutationBlock != 0, cpu[0] == frame.mutationBlock,
                      index < frame.objcHeaders.count else {
                    throw GuestError("Invalid guest Objective-C mutation request")
                }
                if frame.objcHeaders[index] >= UInt64(SharedCache.guestBase), objcWriteOwner == nil {
                    objcWriteOwner = current.id
                    updateObjCCacheWrites(enabled: true)
                    if trace {
                        FileHandle.standardError.write(Data("Enabled scoped Objective-C cache writes for image \(index)\n".utf8))
                    }
                }
            case 123:
                guard signalFrames.isEmpty else {
                    throw GuestError("Guest image registration inside a signal handler is unsupported")
                }
                let callback = try guestCodeAddress(cpu[0])
                let loadedImages = image.imageSlides.sorted { $0.key < $1.key }
                    .map { ($0.key, $0.value, UInt64(0), UInt64(0)) }
                let notification = ImageNotification(callback: callback, arguments: loadedImages,
                    registers: Array(UnsafeBufferPointer(start: cpu, count: 36)),
                    vectors: Array(UnsafeBufferPointer(start: cpu + 806, count: guestTokenCount - 806)),
                    authenticationDepth: cpu[37])
                try continueImageNotification(notification)
            case 124:
                let (address, size) = try lookupGuestSection(cpu[0], kind: Int(UInt32(truncatingIfNeeded: cpu[2])))
                cpu[0] = address
                cpu[1] = size
            case 126:
                guard cpu[0] != 0 else { throw GuestError("Null guest dyld cache range output") }
                let output = try guestBuffer(cpu[0], 8, writable: true)
                let size = image.cacheReader?.mappedSize ?? 0
                if let reader = image.cacheReader, !residentRegions.contains(where: { $0.logical.contains(SharedCache.guestBase) }) {
                    let headerSize = 4096
                    let range = SharedCache.guestBase..<(SharedCache.guestBase + headerSize)
                    guard !residentRegions.contains(where: { $0.logical.overlaps(range) }),
                          ensureResidentCapacity(Int(resident[0]) + 1),
                          let backing = mappingHeap.allocate(UInt64(headerSize)) else {
                        throw GuestError("Cannot map guest dyld cache header")
                    }
                    let bytes = try reader.bytes(at: reader.originalAddress(SharedCache.guestBase), count: headerSize)
                    bytes.withUnsafeBytes {
                        memory.contents().advanced(by: backing).copyMemory(from: $0.baseAddress!, byteCount: headerSize)
                    }
                    publishResident(ExecutableImage.CacheRegion(logical: range, backing: backing, writable: false))
                }
                withUnsafeBytes(of: size.littleEndian) {
                    output.copyMemory(from: $0.baseAddress!, byteCount: $0.count)
                }
                cpu[0] = size == 0 ? 0 : UInt64(SharedCache.guestBase)
            case 125:
                let tableAddress = cpu[0]
                let table = try guestBuffer(tableAddress, 40)
                guard signalFrames.isEmpty, table.loadUnaligned(as: UInt64.self) == 4 else {
                    throw GuestError("Unsupported guest Objective-C callback registration")
                }
                let modifier = (tableAddress + 8) & 0x0000_ffff_ffff_ffff | 0xd15a_0000_0000_0000
                let callback = try guestCodeAddress(table.loadUnaligned(fromByteOffset: 8, as: UInt64.self),
                                                    modifier: modifier, key: 1)
                let objcImages = try image.imagePaths.sorted { $0.key < $1.key }.filter {
                    try lookupGuestSection($0.key, kind: 6).1 != 0
                }
                if objcImages.isEmpty { break }
                guard imageNotifications[current.id]?.contains(where: { $0.mutationBlock != 0 }) != true else {
                    throw GuestError("Nested guest Objective-C registration is unsupported")
                }
                let recordBytes = objcImages.count * 32
                let size = recordBytes + objcImages.reduce(0) { $0 + $1.value.utf8.count + 1 }
                guard let batch = allocateGuest(UInt64(size)), let block = allocateGuest(48) else {
                    throw GuestError("Guest Objective-C image batch allocation failed")
                }
                let blockStorage = memory.contents().advanced(by: block)
                memset(blockStorage, 0, 48)
                try reserveCacheTokens(1)
                let tokenIndex = Int(cpu[guestTokenCount])
                let token = guestTokenStart + 3 * tokenIndex
                let invoke = UInt64(ExecutableImage.objcMutationInvoke)
                cpu[token] = invoke
                cpu[token + 1] = UInt64(block + 16)
                cpu[token + 2] = 0
                cpu[guestTokenCount] += 1
                blockStorage.storeBytes(of: encodeGuestPointer(invoke, key: 0, index: tokenIndex),
                                        toByteOffset: 16, as: UInt64.self)
                blockStorage.storeBytes(of: UInt64(block + 32), toByteOffset: 24, as: UInt64.self)
                blockStorage.storeBytes(of: UInt64(32), toByteOffset: 40, as: UInt64.self)
                let storage = memory.contents().advanced(by: batch)
                var pathOffset = recordBytes
                for (index, entry) in objcImages.enumerated() {
                    let record = storage.advanced(by: index * 32)
                    record.storeBytes(of: entry.key, as: UInt64.self)
                    record.storeBytes(of: UInt64(batch + pathOffset), toByteOffset: 8, as: UInt64.self)
                    record.storeBytes(of: UInt64(0), toByteOffset: 16, as: UInt64.self)
                    // Cached dylibs have Objective-C references fixed up by the cache builder.
                    // Report both dyld's optimized bit and its companion flags bit, with TPRO off.
                    let flags: UInt64 = entry.key >= UInt64(SharedCache.guestBase) ? 5 : 0
                    record.storeBytes(of: flags, toByteOffset: 24, as: UInt64.self)
                    for byte in entry.value.utf8 {
                        storage.storeBytes(of: byte, toByteOffset: pathOffset, as: UInt8.self)
                        pathOffset += 1
                    }
                    storage.storeBytes(of: UInt8(0), toByteOffset: pathOffset, as: UInt8.self)
                    pathOffset += 1
                }
                let notification = ImageNotification(callback: callback,
                    arguments: [(UInt64(objcImages.count), UInt64(batch), UInt64(block), 0)],
                    registers: Array(UnsafeBufferPointer(start: cpu, count: 36)),
                    vectors: Array(UnsafeBufferPointer(start: cpu + 806, count: guestTokenCount - 806)),
                    authenticationDepth: cpu[37], mutationBlock: UInt64(block), objcHeaders: objcImages.map(\.key))
                if trace {
                    FileHandle.standardError.write(Data("Dispatching guest Objective-C mapped callback with \(objcImages.count) images\n".utf8))
                }
                try continueImageNotification(notification)
            case 62:
                let path = try guestString(cpu[0]), mode = try guestString(cpu[1])
                guard mode.elementsEqual("rb".utf8) || mode.elementsEqual("r".utf8) else {
                    throw GuestError("Guest fopen supports read-only files")
                }
                let file = path.baseAddress!.withMemoryRebound(to: CChar.self, capacity: path.count + 1) {
                    Darwin.fopen($0, "rb")
                }
                if let file {
                    guestFiles[nextFile] = file
                    cpu[0] = nextFile
                    nextFile += 1
                } else {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                    cpu[0] = 0
                }
            case 63:
                let stream = cpu[3]
                let file = guestFiles[stream] ?? (stream == UInt64(stdinSlot) && !closedStreams.contains(stream) ? Darwin.stdin : nil)
                guard let file else { throw GuestError("Guest fread stream unsupported") }
                let (length, overflow) = cpu[1].multipliedReportingOverflow(by: cpu[2])
                guard !overflow else { throw GuestError("Guest fread length overflow") }
                let destination = try guestBuffer(cpu[0], length, writable: true)
                if length == 0 { cpu[0] = 0 }
                else {
                    let result = Darwin.fread(destination, Int(cpu[1]), Int(cpu[2]), file)
                    if Darwin.ferror(file) != 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                    cpu[0] = UInt64(result)
                }
            case 64:
                let stream = cpu[0]
                let file = guestFiles[stream] ?? (stream == UInt64(stdinSlot) && !closedStreams.contains(stream) ? Darwin.stdin : nil)
                guard let file else { throw GuestError("Guest feof stream unsupported") }
                cpu[0] = Darwin.feof(file) == 0 ? 0 : 1
            case 65:
                let address = cpu[0], attribute = cpu[1]
                _ = try guestBuffer(address, UInt64(MemoryLayout<pthread_mutex_t>.size), writable: true)
                if address == 0 || (attribute != 0 && mutexAttributes[attribute] == nil) {
                    cpu[0] = UInt64(EINVAL)
                } else {
                    mutexes[address] = GuestMutex(kind: mutexAttributes[attribute] ?? PTHREAD_MUTEX_NORMAL)
                    cpu[0] = 0
                }
            case 66:
                let address = cpu[0]
                if let lock = try mutex(at: address) {
                    if lock.owner != nil { cpu[0] = UInt64(EBUSY) }
                    else {
                        mutexes.removeValue(forKey: address)
                        writeGlobal(0, Int(address))
                        cpu[0] = 0
                    }
                } else { cpu[0] = UInt64(EINVAL) }
            case 67, 68:
                let address = cpu[0]
                guard var lock = try mutex(at: address) else { cpu[0] = UInt64(EINVAL); break }
                let outcome = lock.lock(current.id, tryOnly: cpu[35] == 68)
                mutexes[address] = lock
                switch outcome {
                case .acquired: cpu[0] = 0
                case .error(let error): cpu[0] = UInt64(error)
                case .wait:
                    current.wait = .mutex(address)
                    try schedule(requeue: false)
                    continue
                }
            case 69:
                cpu[0] = UInt64(try unlockMutex(cpu[0]))
            case 70:
                let destination = cpu[0]
                _ = try guestBuffer(destination, 8, writable: true)
                guard cpu[1] == 0 else { throw GuestError("Guest pthread creation attributes unsupported") }
                let entry = try guestCodeAddress(cpu[2])
                let size = 512 * 1024
                let allocationSize = 16 + guestTSDSize + size
                guard let allocation = allocateGuest(UInt64(allocationSize)) else {
                    cpu[0] = UInt64(EAGAIN)
                    break
                }
                let tsdAddress = allocation + 16
                let stackStart = tsdAddress + guestTSDSize
                let stack = stackStart..<(stackStart + size)
                memset(memory.contents().advanced(by: allocation), 0, allocationSize)
                try writeThreadResult(nextThreadID, to: UInt64(tsdAddress))
                try writeThreadResult(UInt64(allocation), to: UInt64(tsdAddress + 8))
                try writeThreadResult(guestMachThreadName(nextThreadID),
                                      to: UInt64(tsdAddress + guestMachThreadSlotOffset))
                var registers = [UInt64](repeating: 0, count: guestTokenCount)
                registers[guestFPCR] = cpu[guestFPCR]
                registers[guestFPSR] = cpu[guestFPSR]
                registers[0] = cpu[3]
                registers[30] = .max
                registers[31] = UInt64(stack.upperBound)
                registers[33] = UInt64(entry)
                let thread = GuestThread(id: nextThreadID, stack: stack, allocation: allocation,
                                         errnoAddress: allocation, tsdAddress: tsdAddress,
                                         registers: registers, signalMask: signals.mask)
                nextThreadID += 1
                threads[thread.id] = thread
                ready.append(thread)
                try writeThreadResult(thread.id, to: destination)
                cpu[0] = 0
            case 71:
                let id = cpu[0], output = cpu[1]
                guard id != current.id else { cpu[0] = UInt64(EDEADLK); break }
                guard let thread = threads[id] else { cpu[0] = UInt64(ESRCH); break }
                guard !threads.values.contains(where: {
                    if case .join(let target, _) = $0.wait { return target == id }
                    return false
                }) else { cpu[0] = UInt64(EINVAL); break }
                if let result = thread.result {
                    try writeThreadResult(result, to: output)
                    try reap(thread)
                    cpu[0] = 0
                } else {
                    current.wait = .join(id, output: output)
                    try schedule(requeue: false)
                    continue
                }
            case 72:
                if try completeThread(cpu[0]) { return 0 }
                continue
            case 73:
                cpu[0] = current.id
            case 74:
                cpu[0] = cpu[0] == cpu[1] ? 1 : 0
            case 75:
                cpu[0] = 0
                try schedule(requeue: true)
                continue
            case 76, 77, 78:
                let address = cpu[0]
                _ = try guestBuffer(address, UInt64(MemoryLayout<pthread_mutexattr_t>.size), writable: true)
                if cpu[35] == 76 {
                    mutexAttributes[address] = PTHREAD_MUTEX_NORMAL
                    cpu[0] = 0
                } else if mutexAttributes[address] == nil {
                    cpu[0] = UInt64(EINVAL)
                } else if cpu[35] == 77 {
                    mutexAttributes.removeValue(forKey: address)
                    cpu[0] = 0
                } else {
                    let kind = Int32(truncatingIfNeeded: cpu[1])
                    if [PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_ERRORCHECK, PTHREAD_MUTEX_RECURSIVE].contains(kind) {
                        mutexAttributes[address] = kind
                        cpu[0] = 0
                    } else { cpu[0] = UInt64(EINVAL) }
                }
            case 79, 80, 81, 82, 83, 84, 85:
                let address = cpu[0]
                let pointer = try guestBuffer(address, UInt64(MemoryLayout<pthread_cond_t>.size), writable: true)
                if cpu[35] == 79 {
                    guard cpu[1] == 0 else { throw GuestError("Guest condition attributes unsupported") }
                    conditions[address] = []
                    cpu[0] = 0
                    break
                }
                if conditions[address] == nil, pointer.loadUnaligned(as: UInt64.self) == 0x3cb0b1bb {
                    conditions[address] = []
                }
                guard var waiters = conditions[address] else { cpu[0] = UInt64(EINVAL); break }
                if cpu[35] == 80 {
                    if waiters.isEmpty {
                        conditions.removeValue(forKey: address)
                        writeGlobal(0, Int(address))
                        cpu[0] = 0
                    } else { cpu[0] = UInt64(EBUSY) }
                } else if [81, 84, 85].contains(cpu[35]) {
                    let lockAddress = cpu[1]
                    var deadline: UInt64?
                    if cpu[35] != 81 {
                        let time = try guestBuffer(cpu[2], UInt64(MemoryLayout<timespec>.size)).loadUnaligned(as: timespec.self)
                        guard let value = timedWaitDeadline(time, relative: cpu[35] == 85) else {
                            cpu[0] = UInt64(EINVAL)
                            break
                        }
                        deadline = value
                    }
                    guard let lock = try mutex(at: lockAddress), lock.owner == current.id,
                          lock.depth == 1 else { cpu[0] = UInt64(EPERM); break }
                    waiters.append(current.id)
                    conditions[address] = waiters
                    current.wait = .condition(address, mutex: lockAddress, deadline: deadline)
                    _ = try unlockMutex(lockAddress)
                    try schedule(requeue: false)
                    continue
                } else {
                    let count = cpu[35] == 82 ? min(1, waiters.count) : waiters.count
                    for id in waiters.prefix(count) {
                        guard let thread = threads[id], case .condition(_, let lockAddress, _) = thread.wait else {
                            throw GuestError("Invalid guest condition waiter")
                        }
                        try reacquire(thread, lockAddress: lockAddress, result: 0)
                    }
                    waiters.removeFirst(count)
                    conditions[address] = waiters
                    cpu[0] = 0
                }
            case 86:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                guard standardSlots[descriptor].map({ !closedStreams.contains($0) }) == true ||
                    ownsDescriptor(descriptor) else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = .max
                    break
                }
                let result = Darwin.lseek(descriptor, Int64(bitPattern: cpu[1]), Int32(truncatingIfNeeded: cpu[2]))
                if result < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(bitPattern: result)
            case 87:
                let output = cpu[0] == 0 ? nil : try guestBuffer(cpu[0], UInt64(MemoryLayout<wchar_t>.size), writable: true).assumingMemoryBound(to: wchar_t.self)
                let input = cpu[1] == 0 ? nil : try guestBuffer(cpu[1], cpu[2]).assumingMemoryBound(to: CChar.self)
                let state = cpu[3] == 0 ? nil : try guestBuffer(cpu[3], UInt64(MemoryLayout<mbstate_t>.size), writable: true).assumingMemoryBound(to: mbstate_t.self)
                let result = mbrtowc(output, input, input == nil ? 0 : Int(cpu[2]), state)
                if result == -1 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(bitPattern: Int64(result))
            case 88:
                let result = __maskrune(Int32(truncatingIfNeeded: cpu[0]), UInt(cpu[1]))
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 92:
                let source = Int32(truncatingIfNeeded: cpu[0])
                let destination = Int32(truncatingIfNeeded: cpu[1])
                let flags = UInt32(truncatingIfNeeded: cpu[3])
                let sourceOpen = ownsDescriptor(source) ||
                    (source == STDIN_FILENO && !closedStreams.contains(UInt64(stdinSlot)))
                let destinationOpen = allowsDescriptorWrite(destination)
                if !sourceOpen || !destinationOpen {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                } else if cpu[2] != 0 || flags & ~UInt32(COPYFILE_ACL | COPYFILE_XATTR) != 0 || flags == 0 {
                    writeGlobal(UInt64(ENOTSUP), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                } else {
                    let result = fcopyfile(source, destination, nil, flags)
                    if result < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                    cpu[0] = UInt64(UInt32(bitPattern: result))
                }
            case 95, 96:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                let setting = cpu[35] == 96
                let open = setting ? allowsDescriptorWrite(descriptor) : ownsDescriptor(descriptor) ||
                    standardSlots[descriptor].map { !closedStreams.contains($0) } == true
                guard open else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                var attributes = try guestBuffer(cpu[1], UInt64(MemoryLayout<attrlist>.size)).loadUnaligned(as: attrlist.self)
                guard cpu[4] == 0, !setting || (attributes.bitmapcount == ATTR_BIT_MAP_COUNT &&
                      attributes.commonattr == ATTR_CMN_FNDRINFO && attributes.volattr == 0 &&
                      attributes.dirattr == 0 && attributes.fileattr == 0 && attributes.forkattr == 0) else {
                    writeGlobal(UInt64(ENOTSUP), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                let buffer = try guestBuffer(cpu[2], cpu[3], writable: !setting)
                let result = setting ? fsetattrlist(descriptor, &attributes, buffer, Int(cpu[3]), 0)
                    : fgetattrlist(descriptor, &attributes, buffer, Int(cpu[3]), 0)
                if result < 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 98, 99:
                let pushback = cpu[35] == 99
                let stream = cpu[pushback ? 1 : 0]
                let input = guestFiles[stream] ??
                    (stream == UInt64(stdinSlot) && !closedStreams.contains(stream) ? Darwin.stdin : nil)
                guard let input else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                let value = pushback ? Darwin.ungetc(Int32(truncatingIfNeeded: cpu[0]), input) : Darwin.fgetc(input)
                if !pushback && value < 0 && Darwin.ferror(input) != 0 {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                }
                cpu[0] = UInt64(UInt32(bitPattern: value))
            case 101:
                cpu[0] = UInt64(Darwin.issetugid())
            case 102, 103:
                guard let version = image.buildVersion else {
                    throw GuestError("Guest executable has no supported build version")
                }
                let result = guest_version_token_at_least(version.token(sdk: cpu[35] == 102), cpu[0])
                guard result >= 0 else { throw GuestError("Darwin version-set comparison is unavailable") }
                cpu[0] = UInt64(result)
            case 100:
                let output = cpu[0]
                guard let alignment = Int(exactly: cpu[1]), alignment >= 8,
                      alignment.nonzeroBitCount == 1 else {
                    cpu[0] = UInt64(EINVAL)
                    break
                }
                _ = try guestBuffer(output, 8, writable: true)
                guard let address = allocateGuest(cpu[2], alignment: max(16, alignment)) else {
                    cpu[0] = UInt64(ENOMEM)
                    break
                }
                try writeThreadResult(UInt64(address), to: output)
                cpu[0] = 0
            case 104, 105, 106:
                let flags = Int32(truncatingIfNeeded: cpu[1])
                let unlink = cpu[35] == 106
                var mode: Int32 = 0
                if !unlink, flags & O_CREAT != 0 {
                    let value = cpu[35] == 104 ? try guestBuffer(cpu[31], 4).loadUnaligned(as: UInt32.self)
                        : UInt32(truncatingIfNeeded: cpu[2])
                    mode = Int32(bitPattern: value)
                }
                let name = try guestString(cpu[0])
                let result = name.baseAddress!.withMemoryRebound(to: CChar.self, capacity: name.count + 1) {
                    unlink ? Darwin.shm_unlink($0) : guest_shm_open($0, flags, mode)
                }
                if result < 0 {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                } else if !unlink {
                    openDescriptors.insert(result)
                    if flags & O_ACCMODE != O_RDONLY { writableDescriptors.insert(result) }
                }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 107:
                let descriptor = Int32(truncatingIfNeeded: cpu[4])
                let fileOffset = Int64(bitPattern: cpu[5])
                var failure = EINVAL
                if !ownsDescriptor(descriptor) {
                    failure = EBADF
                } else if cpu[0] == 0, let length = Int(exactly: cpu[1]), length > 0,
                          cpu[2] == UInt64(PROT_READ), cpu[3] == UInt64(MAP_SHARED), fileOffset >= 0,
                          fileOffset.isMultiple(of: Int64(pageSize)) {
                    var info = stat()
                    if Darwin.fstat(descriptor, &info) != 0 {
                        failure = errno
                    } else if fileOffset <= info.st_size, length <= info.st_size - fileOffset {
                        let count = Int(resident[0])
                        failure = ENOMEM
                        if ensureResidentCapacity(count + 1),
                           let backing = mappingHeap.allocate(UInt64(length), alignment: pageSize) {
                            if let source = Darwin.mmap(nil, length, PROT_READ, MAP_SHARED, descriptor, fileOffset),
                               source != MAP_FAILED {
                                let address = mappingBase + backing
                                let logical = address..<(address + length)
                                mappedFiles[address] = (source, length, backing)
                                residentRegions.append(ExecutableImage.CacheRegion(logical: logical, backing: backing, writable: false))
                                let row = resident + 1 + 4 * count
                                row[0] = UInt64(address)
                                row[1] = UInt64(logical.upperBound)
                                row[2] = UInt64(backing)
                                row[3] = 0
                                resident[0] += 1
                                cpu[0] = UInt64(address)
                                break
                            }
                            failure = errno
                            try mappingHeap.release(backing)
                        }
                    }
                }
                writeGlobal(UInt64(UInt32(bitPattern: failure)), errnoSlot)
                cpu[0] = .max
            case 108:
                guard let address = Int(exactly: cpu[0]), let file = mappedFiles[address], cpu[1] == UInt64(file.size),
                      let row = (0..<Int(resident[0])).first(where: { resident[1 + 4 * $0] == UInt64(address) }) else {
                    writeGlobal(UInt64(EINVAL), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                guard Darwin.munmap(file.source, file.size) == 0 else {
                    writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                mappedFiles.removeValue(forKey: address)
                residentRegions.removeAll { $0.logical.lowerBound == address }
                resident[0] -= 1
                for column in 0..<4 { resident[1 + 4 * row + column] = resident[1 + 4 * Int(resident[0]) + column] }
                try mappingHeap.release(file.backing)
                cpu[0] = 0
            case 110:
                cpu[0] = UInt64(environSlot)
            case 139:
                cpu[0] = UInt64(argcSlot)
            case 140:
                cpu[0] = UInt64(argvSlot)
            case 111:
                let name = try guestString(cpu[0])
                var offset: UInt64 = 0
                var uuid = [UInt8](repeating: 0, count: 16)
                let result = name.baseAddress!.withMemoryRebound(to: CChar.self, capacity: name.count + 1) {
                    guest_cached_selector($0, &offset, &uuid)
                }
                if result == 0 { cpu[0] = 0; break }
                guard result > 0, let reader = image.cacheReader, reader.uuid.elementsEqual(uuid),
                      offset < UInt64(SharedCache.guestLimit - SharedCache.guestBase) else {
                    throw GuestError("Host and guest selector caches do not match")
                }
                let address = SharedCache.guestBase + Int(offset)
                let count = name.count + 1
                let expected = Data(name) + Data([0])
                guard try reader.bytes(at: reader.originalAddress(address), count: count) == expected else {
                    throw GuestError("Cached selector bytes do not match the guest name")
                }
                _ = try makeResident(address, minimumBytes: count)
                if let region = residentRegions.filter({ $0.logical.contains(address) }).max(by: {
                    $0.logical.upperBound < $1.logical.upperBound
                }) {
                    guard count <= region.logical.upperBound - address else {
                        throw GuestError("Cached selector crosses its guest mapping")
                    }
                } else {
                    let window = try reader.selectorWindow(at: address, count: count)
                    var lower = window.lowerBound, upper = window.upperBound
                    for region in residentRegions.map(\.logical) + image.deferredCache.map(\.logical) where region.overlaps(window) {
                        if region.upperBound <= address { lower = max(lower, region.upperBound) }
                        if region.lowerBound > address { upper = min(upper, region.lowerBound) }
                    }
                    let rowIndex = Int(resident[0])
                    guard count <= upper - address, ensureResidentCapacity(rowIndex + 1),
                          let backing = mappingHeap.allocate(UInt64(upper - lower)) else {
                        throw GuestError("Cached selector mapping exceeds guest limits")
                    }
                    let bytes = try reader.bytes(at: reader.originalAddress(lower), count: upper - lower)
                    bytes.withUnsafeBytes { memory.contents().advanced(by: backing).copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
                    residentRegions.append(ExecutableImage.CacheRegion(logical: lower..<upper, backing: backing, writable: false))
                    let row = resident + 1 + 4 * rowIndex
                    row[0] = UInt64(lower)
                    row[1] = UInt64(upper)
                    row[2] = UInt64(backing)
                    row[3] = 0
                    resident[0] += 1
                }
                cpu[0] = UInt64(address)
            case 109:
                let descriptor = Int32(truncatingIfNeeded: cpu[0])
                guard ownsDescriptor(descriptor) else {
                    writeGlobal(UInt64(EBADF), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                let result = Darwin.ftruncate(descriptor, Int64(bitPattern: cpu[1]))
                if result != 0 { writeGlobal(UInt64(UInt32(bitPattern: errno)), errnoSlot) }
                cpu[0] = UInt64(UInt32(bitPattern: result))
            case 112, 114:
                let mapping = cpu[35] == 114
                let flags = cpu[mapping ? 4 : 3] & 0x00ff_ffff
                guard cpu[0] == guestTaskPort, flags & 1 == 1, flags & ~9 == 0 else {
                    cpu[0] = UInt64(KERN_INVALID_ARGUMENT)
                    break
                }
                var alignment = pageSize
                var writable = true
                var maximum: UInt32 = 3
                if mapping {
                    guard cpu[5] == 0, cpu[6] == 0, let mask = Int(exactly: cpu[3]), mask < Int.max,
                          (mask + 1).nonzeroBitCount == 1 else {
                        cpu[0] = UInt64(KERN_INVALID_ARGUMENT)
                        break
                    }
                    let args = try guestBuffer(cpu[31], 12)
                    let current = args.loadUnaligned(as: UInt32.self)
                    maximum = args.loadUnaligned(fromByteOffset: 4, as: UInt32.self)
                    guard (current == 1 || current == 3), current & ~maximum == 0 else {
                        cpu[0] = UInt64(KERN_INVALID_ARGUMENT)
                        break
                    }
                    alignment = max(pageSize, mask + 1)
                    writable = current & 2 != 0
                }
                let output = try guestBuffer(cpu[1], 8, writable: true)
                if cpu[2] == 0 {
                    output.storeBytes(of: UInt64(0), as: UInt64.self)
                    cpu[0] = UInt64(KERN_SUCCESS)
                    break
                }
                guard let requested = Int(exactly: cpu[2]), requested <= Int.max - (pageSize - 1) else {
                    cpu[0] = UInt64(KERN_NO_SPACE)
                    break
                }
                let length = (requested + pageSize - 1) & ~(pageSize - 1)
                guard ensureResidentCapacity(Int(resident[0]) + 1), alignment <= 16 * 1024 * 1024,
                      let backing = mappingHeap.allocate(UInt64(length), alignment: alignment) else {
                    cpu[0] = UInt64(KERN_NO_SPACE)
                    break
                }
                let address = mappingBase + backing
                memset(memory.contents().advanced(by: backing), 0, length)
                let region = ExecutableImage.CacheRegion(logical: address..<(address + length), backing: backing, writable: writable)
                publishResident(region)
                anonymousMappings[address] = (region, maximum)
                output.storeBytes(of: UInt64(address), as: UInt64.self)
                cpu[0] = UInt64(KERN_SUCCESS)
            case 115:
                cpu[0] = image.imageSlides[cpu[0]] ?? 0
            case 113:
                guard cpu[0] == guestTaskPort else {
                    cpu[0] = UInt64(KERN_INVALID_ARGUMENT)
                    break
                }
                if cpu[2] == 0 { cpu[0] = UInt64(KERN_SUCCESS); break }
                guard let address = Int(exactly: cpu[1]), let region = anonymousMappings[address]?.region,
                      cpu[2] <= UInt64(region.logical.count),
                      cpu[2] > UInt64(region.logical.count - pageSize) else {
                    cpu[0] = UInt64(KERN_INVALID_ADDRESS)
                    break
                }
                for piece in residentRegions where region.logical.overlaps(piece.logical) {
                    try removeResident(piece.logical.lowerBound)
                }
                try mappingHeap.release(region.backing)
                anonymousMappings.removeValue(forKey: address)
                cpu[0] = UInt64(KERN_SUCCESS)
            case 117:
                let name = String(decoding: try guestString(cpu[0]), as: UTF8.self)
                cpu[0] = UInt64(image.linkTimeVersion(named: name))
            case 116:
                let protection = UInt32(truncatingIfNeeded: cpu[2])
                guard let address = Int(exactly: cpu[0]), address.isMultiple(of: pageSize),
                      let size = Int(exactly: cpu[1]), size <= Int.max - (pageSize - 1),
                      [0, 1, 3].contains(protection) else {
                    writeGlobal(UInt64(EINVAL), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                if size == 0 { cpu[0] = 0; break }
                let length = (size + pageSize - 1) & ~(pageSize - 1)
                if trace {
                    let line = "Guest mprotect at 0x\(String(address, radix: 16)), length \(length), protection \(protection)\n"
                    FileHandle.standardError.write(Data(line.utf8))
                }
                guard let mapping = anonymousMappings.values.first(where: {
                    $0.region.logical.contains(address) && length <= $0.region.logical.upperBound - address
                }) else {
                    writeGlobal(UInt64(ENOMEM), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                guard protection & ~mapping.maximum == 0 else {
                    writeGlobal(UInt64(EACCES), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                let end = address + length
                let original = mapping.region
                let old = residentRegions.filter { original.logical.overlaps($0.logical) }
                    .sorted { $0.logical.lowerBound < $1.logical.lowerBound }
                var replacement = [ExecutableImage.CacheRegion]()
                func appendPiece(_ range: Range<Int>, readable: Bool, writable: Bool) {
                    guard !range.isEmpty else { return }
                    let start = range.lowerBound
                    if let last = replacement.last, last.logical.upperBound == start,
                       last.readable == readable, last.writable == writable {
                        replacement[replacement.count - 1] = ExecutableImage.CacheRegion(
                            logical: last.logical.lowerBound..<range.upperBound, backing: last.backing,
                            writable: writable, readable: readable)
                    } else {
                        replacement.append(ExecutableImage.CacheRegion(logical: range,
                            backing: original.backing + start - original.logical.lowerBound,
                            writable: writable, readable: readable))
                    }
                }
                for piece in old {
                    let lower = piece.logical.lowerBound, upper = piece.logical.upperBound
                    let first = min(upper, max(lower, address))
                    let last = max(first, min(upper, end))
                    appendPiece(lower..<first, readable: piece.readable, writable: piece.writable)
                    appendPiece(first..<last, readable: protection != 0, writable: protection == 3)
                    appendPiece(last..<upper, readable: piece.readable, writable: piece.writable)
                }
                guard ensureResidentCapacity(Int(resident[0]) - old.count + replacement.count) else {
                    writeGlobal(UInt64(ENOMEM), errnoSlot)
                    cpu[0] = UInt64(UInt32.max)
                    break
                }
                for piece in old { try removeResident(piece.logical.lowerBound) }
                for piece in replacement { publishResident(piece) }
                cpu[0] = 0
            default:
                throw GuestError("Unknown guest host service")
            }
            if let status = try deliverSignal() { return status }
            if cpu[33] == UInt64.max, let status = try finishExecutionFrame() { return status }
        default:
            throw GuestError("Unknown guest process status")
        }
    }
}
