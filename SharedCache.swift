import Foundation

struct SharedCache {
    static let swiftPrespecializedLibrary = "/usr/lib/libswiftPrespecialized.dylib"
    struct Rebase {
        struct Authentication {
            let diversity: UInt16
            let addressDiversity: Bool
            let dataKey: Bool
        }

        let location: UInt64
        let target: UInt64
        let highByte: UInt8
        let authentication: Authentication?
    }

    struct Segment {
        let name: String
        let address: UInt64
        let size: UInt64
        let fileOffset: UInt64
        let fileSize: UInt64
        let writable: Bool
    }

    struct Dependency {
        let name: String
        let weak: Bool
        let reexported: Bool
    }

    private struct Mapping {
        let start: UInt64
        let size: UInt64
        let fileOffset: UInt64
        let file: Data
        let slideInfo: Range<Int>?
        let flags: UInt64
        let protection: UInt64
        let maximumProtection: UInt64
    }

    private let mappings: [Mapping]
    private let images: [String: UInt64]
    private let base: UInt64
    let mappedSize: UInt64
    private let objcOptsOffset: UInt64
    private let swiftOptsOffset: UInt64
    private let swiftOptsSize: UInt64
    private let objcOptsSize: UInt64
    let uuid: Data

    init() throws {
        let directories = [
            "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld",
            "/System/Library/dyld",
        ]
        guard let url = directories.map({
            URL(fileURLWithPath: $0).appendingPathComponent("dyld_shared_cache_arm64e")
        }).first(where: { FileManager.default.fileExists(atPath: $0.path) }) else {
            throw GuestError("Unable to locate the ARM64e dyld shared cache")
        }
        let main = try Data(contentsOf: url, options: .alwaysMapped)
        func word(_ data: Data, _ offset: Int) throws -> Int {
            Int(try MachO.integer(data, offset, 4))
        }
        func cacheHeader(_ data: Data) throws -> Int {
            let magic = String(decoding: data.prefix(16).prefix { $0 != 0 }, as: UTF8.self)
            guard magic == "dyld_v1  arm64e" else { throw GuestError("Unsupported dyld shared-cache magic") }
            return try word(data, 16)
        }
        let headerSize = try cacheHeader(main)
        base = try MachO.integer(main, 224, 8)
        let declaredSize = try MachO.integer(main, 232, 8)
        guard declaredSize > 0, declaredSize <= UInt64(Self.guestLimit - Self.guestBase),
              declaredSize <= UInt64.max - base else {
            throw GuestError("Invalid dyld shared-cache span")
        }
        mappedSize = declaredSize
        swiftOptsOffset = headerSize >= 392 ? try MachO.integer(main, 376, 8) : 0
        swiftOptsSize = headerSize >= 392 ? try MachO.integer(main, 384, 8) : 0
        objcOptsOffset = headerSize >= 480 ? try MachO.integer(main, 464, 8) : 0
        objcOptsSize = headerSize >= 480 ? try MachO.integer(main, 472, 8) : 0
        uuid = main.subdata(in: 88..<104)
        guard headerSize >= 456 else { throw GuestError("Unsupported dyld shared-cache header") }
        // Offsets in dyld_cache_header, through imagesCount.
        let subOffset = try word(main, 392), subCount = try word(main, 396)
        let imageOffset = try word(main, 448), imageCount = try word(main, 452)
        let subStride = headerSize > 456 ? 56 : 24
        guard subOffset <= main.count, subCount <= (main.count - subOffset) / subStride,
              imageOffset <= main.count, imageCount <= (main.count - imageOffset) / 32 else {
            throw GuestError("Invalid dyld shared-cache index")
        }

        var images = [String: UInt64]()
        for index in 0..<imageCount {
            let entry = imageOffset + index * 32
            let address = try MachO.integer(main, entry, 8)
            let start = try word(main, entry + 24)
            guard start < main.count, let end = main[start...].firstIndex(of: 0), end > start else {
                throw GuestError("Invalid dyld shared-cache install name")
            }
            images[String(decoding: main[start..<end], as: UTF8.self)] = address
        }
        self.images = images

        var mappings = [Mapping]()
        func addMappings(_ file: Data) throws {
            let header = try cacheHeader(file)
            let modernCount = header >= 320 ? try word(file, 316) : 0
            let stride = modernCount > 0 ? 56 : 32
            let offset = modernCount > 0 ? try word(file, 312) : header
            let count = modernCount > 0 ? modernCount : try word(file, 20)
            guard offset <= file.count, count <= (file.count - offset) / stride else {
                throw GuestError("Invalid dyld shared-cache mapping table")
            }
            for index in 0..<count {
                let entry = offset + index * stride
                let start = try MachO.integer(file, entry, 8)
                let size = try MachO.integer(file, entry + 8, 8)
                let position = try MachO.integer(file, entry + 16, 8)
                guard size <= UInt64.max - start, position <= UInt64(file.count),
                      size <= UInt64(file.count) - position else {
                    throw GuestError("Invalid dyld shared-cache mapping range")
                }
                var slideInfo: Range<Int>?
                if modernCount > 0 {
                    let infoOffset = try MachO.integer(file, entry + 24, 8)
                    let infoSize = try MachO.integer(file, entry + 32, 8)
                    guard infoOffset <= UInt64(file.count), infoSize <= UInt64(file.count) - infoOffset else {
                        throw GuestError("Invalid dyld slide info range")
                    }
                    if infoSize > 0 { slideInfo = Int(infoOffset)..<Int(infoOffset + infoSize) }
                }
                let flags = modernCount > 0 ? try MachO.integer(file, entry + 40, 8) : 0
                let protection = try MachO.integer(file, entry + (modernCount > 0 ? 52 : 28), 4)
                let maximumProtection = try MachO.integer(file, entry + (modernCount > 0 ? 48 : 24), 4)
                mappings.append(Mapping(start: start, size: size, fileOffset: position, file: file,
                                        slideInfo: slideInfo, flags: flags, protection: protection,
                                        maximumProtection: maximumProtection))
            }
        }
        try addMappings(main)
        for index in 0..<subCount {
            let entry = subOffset + index * subStride
            let suffix = subStride == 56
                ? String(decoding: main[(entry + 24)..<(entry + 56)].prefix { $0 != 0 }, as: UTF8.self)
                : ".\(index + 1)"
            guard suffix.hasPrefix("."), !suffix.contains("/") else {
                throw GuestError("Invalid dyld subcache suffix")
            }
            let file = try Data(contentsOf: URL(fileURLWithPath: url.path + suffix), options: .alwaysMapped)
            guard file.count >= 104, main[entry..<(entry + 16)].elementsEqual(file[88..<104]) else {
                throw GuestError("Dyld subcache UUID mismatch")
            }
            try addMappings(file)
        }
        self.mappings = mappings
    }

    func bytes(at address: UInt64, count: Int) throws -> Data {
        guard count >= 0, let mapping = mappings.first(where: {
            address >= $0.start && address - $0.start <= $0.size &&
                UInt64(count) <= $0.size - (address - $0.start)
        }) else { throw GuestError("Dyld shared-cache address is unmapped") }
        let start = Int(mapping.fileOffset + (address - mapping.start))
        return mapping.file.subdata(in: start..<(start + count))
    }

    private struct Image {
        let address: UInt64
        let segments: [Segment]
        let text: [Range<UInt64>]
        let exports: (offset: UInt64, size: UInt64)?
        let reexports: [String]
        let dependencies: [Dependency]
        let lazyLoads: [(offset: UInt64, size: UInt64)]
    }

    private func image(_ path: String) throws -> Image {
        guard let address = images[path] else { throw GuestError("Image is absent from the dyld shared cache: \(path)") }
        let header = try bytes(at: address, count: 32)
        guard try MachO.integer(header, 0, 4) == 0xfeedfacf,
              try MachO.integer(header, 4, 4) == 0x0100000c else {
            throw GuestError("Invalid cached ARM64 Mach-O header")
        }
        let count = Int(try MachO.integer(header, 16, 4))
        let length = Int(try MachO.integer(header, 20, 4))
        guard count <= length / 8 else { throw GuestError("Invalid cached Mach-O load commands") }
        let commands = try bytes(at: address + 32, count: length)
        var segments = [Segment]()
        var text = [Range<UInt64>]()
        var exports: (offset: UInt64, size: UInt64)?
        var reexports = [String]()
        var dependencies = [Dependency]()
        var lazyLoads = [(offset: UInt64, size: UInt64)]()
        var cursor = 0
        for _ in 0..<count {
            let command = try MachO.integer(commands, cursor, 4)
            let size = Int(try MachO.integer(commands, cursor + 4, 4))
            guard size >= 8, size <= length - cursor else { throw GuestError("Invalid cached Mach-O load command") }
            func read(_ offset: Int, _ width: Int) throws -> UInt64 {
                try MachO.integer(commands, cursor + offset, width)
            }
            if command == 0x19, size >= 72 {
                let name = String(decoding: commands[(cursor + 8)..<(cursor + 24)].prefix { $0 != 0 }, as: UTF8.self)
                let start = try read(24, 8), length = try read(32, 8)
                let fileSize = try read(48, 8)
                guard length <= UInt64.max - start, fileSize <= length else {
                    throw GuestError("Invalid cached Mach-O segment")
                }
                let writable = try read(60, 4) & 2 != 0 && read(68, 4) & 0x10 == 0
                segments.append(Segment(name: name, address: start, size: length,
                                        fileOffset: try read(40, 8), fileSize: fileSize, writable: writable))
                let sectionCount = Int(try read(64, 4))
                guard sectionCount <= (size - 72) / 80 else { throw GuestError("Invalid cached Mach-O sections") }
                if name == "__TEXT" {
                    for section in 0..<sectionCount {
                        let offset = 72 + section * 80
                        let flags = try read(offset + 64, 4)
                        if flags & 0x80000400 == 0 { continue }
                        let address = try read(offset + 32, 8), length = try read(offset + 40, 8)
                        guard length <= UInt64.max - address else { throw GuestError("Invalid cached Mach-O text") }
                        text.append(address..<(address + length))
                    }
                }
            }
            if command == 0x80000033, size >= 16 {
                exports = (try read(8, 4), try read(12, 4))
            } else if (command == 0x22 || command == 0x80000022), size >= 48, exports == nil {
                exports = (try read(40, 4), try read(44, 4))
            }
            if command == 0x3a, size >= 16 {
                lazyLoads.append((try read(8, 4), try read(12, 4)))
            }
            if command == 0x8000001f, size >= 24 {
                let offset = Int(try read(8, 4))
                guard offset >= 24, offset < size,
                      let end = commands[(cursor + offset)..<(cursor + size)].firstIndex(of: 0) else {
                    throw GuestError("Invalid cached dylib re-export name")
                }
                reexports.append(String(decoding: commands[(cursor + offset)..<end], as: UTF8.self))
            }
            if command == 0xc || command == 0x80000018 || command == 0x8000001f ||
                command == 0x20 || command == 0x80000023 {
                guard size >= 24 else { throw GuestError("Truncated cached dylib dependency") }
                let offset = Int(try read(8, 4))
                guard offset >= 24, offset < size,
                      let end = commands[(cursor + offset)..<(cursor + size)].firstIndex(of: 0) else {
                    throw GuestError("Invalid cached dylib dependency name")
                }
                dependencies.append(Dependency(name: String(decoding: commands[(cursor + offset)..<end], as: UTF8.self),
                                                weak: command == 0x80000018, reexported: command == 0x8000001f))
            }
            cursor += size
        }
        return Image(address: address, segments: segments, text: text, exports: exports, reexports: reexports,
                     dependencies: dependencies, lazyLoads: lazyLoads)
    }

    func preboundLazyDependency(owner: String, flag: UInt64) throws -> String? {
        let source = try image(owner)
        guard let linkedit = source.segments.first(where: { $0.name == "__LINKEDIT" }) else { return nil }
        let header = UInt64(try guestAddress(source.address))
        guard flag >= header, flag.isMultiple(of: 4) else { return nil }
        for record in source.lazyLoads {
            guard record.size >= 24, record.offset >= linkedit.fileOffset,
                  record.offset - linkedit.fileOffset <= linkedit.fileSize,
                  record.size <= linkedit.fileSize - (record.offset - linkedit.fileOffset) else {
                throw GuestError("Invalid cached lazy-load record")
            }
            let blob = try bytes(at: linkedit.address + (record.offset - linkedit.fileOffset), count: Int(record.size))
            guard try MachO.integer(blob, 4, 4) == flag - header else { continue }
            guard try MachO.integer(blob, 8, 2) & 2 != 0 else { return nil }
            let start = Int(try MachO.integer(blob, 0, 4))
            guard start >= 24, start < blob.count, let end = blob[start...].firstIndex(of: 0), end > start else {
                throw GuestError("Invalid cached lazy dependency path")
            }
            return String(decoding: blob[start..<end], as: UTF8.self)
        }
        return nil
    }

    private func exportTable(_ segments: [Segment], _ exports: (offset: UInt64, size: UInt64)?) throws -> [String: MachExport] {
        guard let linkedit = segments.first(where: { $0.name == "__LINKEDIT" }),
              let exports, exports.size > 0, exports.offset >= linkedit.fileOffset,
              exports.offset - linkedit.fileOffset <= linkedit.fileSize,
              exports.size <= linkedit.fileSize - (exports.offset - linkedit.fileOffset) else {
            throw GuestError("Cached Mach-O export trie is missing or invalid")
        }
        let trie = try bytes(at: linkedit.address + (exports.offset - linkedit.fileOffset), count: Int(exports.size))
        return try machExports(trie, memorySize: Int.max)
    }

    func export(in path: String, named name: String) throws -> (offset: Int, firstWord: UInt32) {
        let image = try image(path)
        guard case .address(let offset) = try exportTable(image.segments, image.exports)[name] else {
            throw GuestError("Direct export is absent from the cached image: \(name)")
        }
        let contents = try bytes(at: image.address + UInt64(offset), count: 4)
        return (offset, UInt32(try MachO.integer(contents, 0, 4)))
    }

    func localeKeyAddress() throws -> Int? {
        let path = "/usr/lib/system/libsystem_c.dylib"
        let libc = try image(path)
        guard case .address(let offset) = try exportTable(libc.segments, libc.exports)["_strtoll"] else { return nil }
        let pc = libc.address + UInt64(offset)
        let code = try bytes(at: pc, count: 16)
        let adrp = try MachO.integer(code, 0, 4)
        let load = try MachO.integer(code, 4, 4)
        // strtoll starts by loading the private locale key and indexing the thread slots.
        guard adrp & 0x9f00_001f == 0x9000_0008, load & 0xffc0_03ff == 0xf940_0108,
              try MachO.integer(code, 8, 4) == 0xd53b_d069,
              try MachO.integer(code, 12, 4) == 0xf868_7928 else { return nil }
        let immediate = Int64(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3))
        let pages = immediate >= 1 << 20 ? immediate - (1 << 21) : immediate
        let slot = Int64(pc & ~4095) + pages * 4096 + Int64((load >> 10) & 0xfff) * 8
        guard slot >= 0 else { return nil }
        let address = UInt64(slot)
        guard libc.segments.contains(where: {
            $0.writable && address >= $0.address && address - $0.address < $0.fileSize &&
                $0.fileSize - (address - $0.address) >= 8
        }), try MachO.integer(bytes(at: address, count: 8), 0, 8) == UInt64.max else { return nil }
        return try guestAddress(address)
    }

    static let guestBase = 0x1_0000_0000
    static let guestLimit = 1 << 44

    func guestAddress(_ address: UInt64) throws -> Int {
        guard address >= base, address - base < UInt64(Self.guestLimit - Self.guestBase) else {
            throw GuestError("Dyld cache address 0x\(String(address, radix: 16)) exceeds guest window")
        }
        return Self.guestBase + Int(address - base)
    }

    func originalAddress(_ guest: Int) throws -> UInt64 {
        guard guest >= Self.guestBase, guest < Self.guestLimit else {
            throw GuestError("Guest address is outside the dyld cache window")
        }
        return base + UInt64(guest - Self.guestBase)
    }

    func isImmutable(from start: Int, to end: Int) -> Bool {
        guard let address = try? originalAddress(start), let limit = try? originalAddress(end) else { return false }
        return mappings.contains { address > $0.start && limit < $0.start + $0.size && $0.maximumProtection & 2 == 0 }
    }

    func isCacheDataConst(_ range: Range<Int>) -> Bool {
        guard let address = try? originalAddress(range.lowerBound) else { return false }
        return mappings.contains {
            $0.flags & 4 != 0 && $0.protection & 3 == 1 && $0.maximumProtection & 2 != 0 &&
                address >= $0.start && address - $0.start < $0.size &&
                UInt64(range.count) <= $0.size - (address - $0.start)
        }
    }

    private func objcCacheAddress(_ offset: UInt64) throws -> UInt64 {
        guard offset < mappedSize else {
            throw GuestError("Objective-C metadata exceeds dyld cache span")
        }
        return base + offset
    }

    func objcHeaderTable(writable: Bool) throws -> (address: Int, size: Int)? {
        let offsetField = writable ? 16 : 8
        guard objcOptsOffset != 0, objcOptsSize >= UInt64(offsetField + 8) else { return nil }
        let opts = try bytes(at: objcCacheAddress(objcOptsOffset), count: offsetField + 8)
        let offset = try MachO.integer(opts, offsetField, 8)
        guard offset != 0 else { return nil }
        let address = try objcCacheAddress(offset)
        let header = try bytes(at: address, count: 8)
        let count = try MachO.integer(header, 0, 4)
        let stride = try MachO.integer(header, 4, 4)
        guard stride >= 8, stride.isMultiple(of: 8), mappedSize - offset >= 8,
              count <= (mappedSize - offset - 8) / stride else {
            throw GuestError("Invalid Objective-C header table size")
        }
        let size = 8 + count * stride
        guard mappings.contains(where: {
            (writable ? $0.protection & 3 == 3 : $0.slideInfo == nil && $0.maximumProtection & 2 == 0 && $0.protection & 1 != 0) &&
                address >= $0.start && address - $0.start < $0.size &&
                size <= $0.size - (address - $0.start)
        }) else { throw GuestError("Invalid Objective-C header table mapping") }
        if writable, try !rebases(in: address..<(address + size)).isEmpty {
            throw GuestError("Unsupported rebase in writable Objective-C header table")
        }
        return (try guestAddress(address), Int(size))
    }

    func swiftPrespecializedAddress() throws -> Int? {
        guard swiftOptsOffset != 0, swiftOptsSize >= 40, swiftOptsOffset < mappedSize else { return nil }
        let header = try bytes(at: base + swiftOptsOffset, count: 40)
        guard try MachO.integer(header, 0, 4) >= 2 else { return nil }
        let offset = try MachO.integer(header, 32, 8)
        guard offset != 0, offset < mappedSize, images[Self.swiftPrespecializedLibrary] != nil else { return nil }
        let address = base + offset
        let owner = try image(Self.swiftPrespecializedLibrary)
        guard owner.segments.contains(where: {
            $0.name == "__DATA_CONST" && address >= $0.address && address - $0.address < $0.fileSize
        }) else { throw GuestError("Swift prespecialized data is outside its cache image") }
        return try guestAddress(address)
    }

    // Bob Jenkins' public-domain lookup8, specialized for one little-endian word.
    private func swiftKeyHash(_ key: UInt64, salt: UInt64) -> UInt64 {
        var a = salt &+ key
        var b = salt
        var c: UInt64 = 0x9e3779b97f4a7c1b
        for (first, second, third) in [(43, 9, 8), (38, 23, 5), (35, 49, 11), (12, 18, 22)] {
            a = (a &- b &- c) ^ (c >> first)
            b = (b &- c &- a) ^ (a << second)
            c = (c &- a &- b) ^ (b >> third)
        }
        return c
    }

    func swiftConformance(protocolAddress: UInt64, metadata: UInt64, type: UInt64,
                          isLoaded: (Int) throws -> Bool) throws -> Int? {
        let guestBase = UInt64(Self.guestBase)
        guard protocolAddress >= guestBase, protocolAddress - guestBase < mappedSize,
              swiftOptsOffset != 0, swiftOptsSize >= 24, swiftOptsOffset < mappedSize else { return nil }
        let header = try bytes(at: base + swiftOptsOffset, count: 24)
        let protocolOffset = protocolAddress - guestBase
        // Dyld tries the type descriptor before the metadata pointer.
        for (keyAddress, headerField) in [(type, 8), (metadata, 16)] {
            guard keyAddress >= guestBase, keyAddress - guestBase < mappedSize else { continue }
            let tableOffset = try MachO.integer(header, headerField, 8)
            guard tableOffset != 0, tableOffset < mappedSize else { continue }
            let table = base + tableOffset
            let fixedSize: UInt64 = 32 + 256 * 4
            let fixed = try bytes(at: table, count: Int(fixedSize))
            let capacity = try MachO.integer(fixed, 0, 4)
            if capacity == 0 { continue }
            let shift = try MachO.integer(fixed, 8, 4)
            let mask = try MachO.integer(fixed, 12, 4)
            let tabSize = try MachO.integer(fixed, 20, 4)
            let salt = try MachO.integer(fixed, 24, 8)
            let targetsStart = fixedSize + tabSize + capacity
            let recordsStart = targetsStart + 4 * capacity
            guard shift <= 64, mask < tabSize, recordsStart <= mappedSize - tableOffset else {
                throw GuestError("Invalid Swift cache conformance table")
            }
            let key = keyAddress - guestBase
            let hash = swiftKeyHash(key, salt: salt) ^ swiftKeyHash(protocolOffset, salt: salt)
            let tab = try MachO.integer(bytes(at: table + fixedSize + (hash & mask), count: 1), 0, 1)
            let scramble = try MachO.integer(fixed, 32 + 4 * Int(tab), 4)
            let bucket = UInt64(UInt32(truncatingIfNeeded: hash >> shift)) ^ scramble
            guard bucket < capacity else { throw GuestError("Invalid Swift conformance bucket") }
            let check = try MachO.integer(bytes(at: table + fixedSize + tabSize + bucket, count: 1), 0, 1)
            guard check == ((key & 7) << 5) | 16 else { continue }
            let target = try MachO.integer(bytes(at: table + targetsStart + 4 * bucket, count: 4), 0, 4)
            if target == UInt64(UInt32.max) { continue }
            guard target >= recordsStart else { throw GuestError("Invalid Swift conformance target") }
            var address = table + target
            while true {
                let record = try bytes(at: address, count: 24)
                guard try MachO.integer(record, 0, 8) == key,
                      try MachO.integer(record, 8, 8) == protocolOffset else { break }
                let value = try MachO.integer(record, 16, 8)
                if try isLoaded(Int(value >> 48)) {
                    let offset = (value >> 1) & ((1 << 47) - 1)
                    guard offset < mappedSize else { throw GuestError("Invalid Swift conformance descriptor") }
                    return try guestAddress(base + offset)
                }
                if value & 1 == 0 { break }
                address += 24
            }
        }
        return nil
    }

    func objcClassCount() throws -> UInt32 {
        guard objcOptsOffset != 0, objcOptsSize >= 40 else { return 0 }
        let opts = try bytes(at: objcCacheAddress(objcOptsOffset), count: 40)
        let offset = try MachO.integer(opts, 32, 8)
        guard offset != 0 else { return 0 }
        let table = try bytes(at: objcCacheAddress(offset), count: 20)
        let capacity = try MachO.integer(table, 4, 4)
        let occupied = try MachO.integer(table, 8, 4)
        let tabSize = try MachO.integer(table, 16, 4) + 1
        // Fixed hash header, tab, and one check byte, string offset, and object record per bucket.
        let duplicateOffset = offset + 0x420 + tabSize + capacity * 13
        guard occupied <= capacity, tabSize.nonzeroBitCount == 1 else {
            throw GuestError("Invalid Objective-C class table")
        }
        let duplicates = try bytes(at: objcCacheAddress(duplicateOffset), count: 4)
        let total = occupied + (try MachO.integer(duplicates, 0, 4))
        guard let count = UInt32(exactly: total) else {
            throw GuestError("Invalid Objective-C class count")
        }
        return count
    }

    func objcMessageSendOffsets() throws -> (address: Int, count: UInt32)? {
        guard objcOptsOffset != 0, objcOptsSize >= 0x88 else { return nil }
        let header = try bytes(at: objcCacheAddress(objcOptsOffset), count: 0x88)
        guard try MachO.integer(header, 0, 4) >= 4 else { return nil }
        let descriptorOffset = try MachO.integer(header, 0x80, 8)
        guard descriptorOffset != 0 else { return nil }
        let descriptor = try bytes(at: objcCacheAddress(descriptorOffset), count: 16)
        let count = UInt32(try MachO.integer(descriptor, 0, 4))
        let tableOffset = try MachO.integer(descriptor, 8, 8)
        guard count != 0, tableOffset != 0 else { return nil }
        let address = try objcCacheAddress(tableOffset)
        let size = UInt64(count) * 8
        guard size <= UInt64(Self.guestLimit - Self.guestBase) - tableOffset,
              mappings.contains(where: {
                  $0.slideInfo == nil && $0.maximumProtection & 2 == 0 &&
                      address >= $0.start && address - $0.start < $0.size &&
                      size <= $0.size - (address - $0.start)
              }) else { throw GuestError("Invalid Objective-C message-send offsets table") }
        return (try guestAddress(address), count)
    }

    func objcImageHeader(at index: Int) throws -> Int? {
        guard let table = try objcHeaderTable(writable: false) else { return nil }
        let address = try originalAddress(table.address)
        let prefix = try bytes(at: address, count: 8)
        let count = Int(try MachO.integer(prefix, 0, 4))
        let stride = Int(try MachO.integer(prefix, 4, 4))
        guard index >= 0, index < count else { throw GuestError("Invalid Objective-C cache image index") }
        let entry = address + UInt64(8 + index * stride)
        let relative = try MachO.integer(bytes(at: entry, count: 8), 0, 8)
        return try guestAddress(entry &+ relative)
    }

    func objcHeaderIndex(_ header: Int) throws -> Int? {
        guard let table = try objcHeaderTable(writable: false) else { return nil }
        let address = try originalAddress(table.address)
        let prefix = try bytes(at: address, count: 8)
        let count = Int(try MachO.integer(prefix, 0, 4))
        let stride = Int(try MachO.integer(prefix, 4, 4))
        let target = try originalAddress(header)
        var lower = 0, upper = count
        while lower < upper {
            let index = lower + (upper - lower) / 2
            let entry = address + UInt64(8 + index * stride)
            let relative = try MachO.integer(bytes(at: entry, count: 8), 0, 8)
            let candidate = entry &+ relative
            if candidate == target { return index }
            if candidate < target { lower = index + 1 } else { upper = index }
        }
        return nil
    }

    func selectorWindow(at guest: Int, count: Int) throws -> Range<Int> {
        let address = try originalAddress(guest)
        guard let mapping = mappings.first(where: { address >= $0.start && address - $0.start < $0.size }),
              mapping.slideInfo == nil, mapping.protection & 3 == 1 else {
            throw GuestError("Selector is outside immutable cache mappings")
        }
        let windowSize: UInt64 = 16 * 1024
        let windowStart = address & ~(windowSize - 1)
        let lower = max(windowStart, mapping.start)
        let upper = min(max(windowStart + windowSize, address + UInt64(count)), mapping.start + mapping.size)
        let start = try guestAddress(lower)
        return start..<(start + Int(upper - lower))
    }

    func selectorStringSize(at guest: Int, limit: Int) throws -> Int? {
        let address = try originalAddress(guest)
        guard let mapping = mappings.first(where: {
            address >= $0.start && address - $0.start < $0.size &&
                $0.slideInfo == nil && $0.maximumProtection & 2 == 0 && $0.protection & 1 != 0
        }) else { return nil }
        let size = min(limit, Int(mapping.size - (address - mapping.start)))
        let start = Int(mapping.fileOffset + address - mapping.start)
        guard let end = mapping.file[start..<(start + size)].firstIndex(of: 0) else { return nil }
        return end - start + 1
    }

    func hostExportAddresses() throws -> (functions: [Int: String], data: [UInt64: String]) {
        let platformMemory = [
            "___bzero": "_bzero", "__platform_bzero": "_bzero",
            "__platform_memmove": "_memmove", "__platform_memset": "_memset",
            "__platform_memset_pattern4": "_memset_pattern4",
            "__platform_memset_pattern8": "_memset_pattern8",
            "__platform_memset_pattern16": "_memset_pattern16",
        ]
        var pending = ["/usr/lib/libSystem.B.dylib"]
        var visited = Set<String>()
        var result = [Int: String]()
        var data = [UInt64: String]()
        let modeledData: Set<String> = ["___stack_chk_guard", "___stdoutp", "___stderrp", "___stdinp",
                                        "___mb_cur_max", "_optind", "_optarg", "_environ", "_vm_page_size", "_vm_page_shift", "_vm_kernel_page_size", "_mach_task_self_"]
        var ambiguous = Set<Int>()
        while let path = pending.popLast() {
            guard images[path] != nil, visited.insert(path).inserted else { continue }
            let image = try image(path)
            pending.append(contentsOf: image.reexports)
            guard let range = image.exports, range.size > 0 else { continue }
            let exports = try exportTable(image.segments, range)
            for symbol in exports.keys.sorted() {
                let name = platformMemory[symbol] ?? symbol
                guard modeledData.contains(name) || hostServiceIDs[name] != nil || guestMemoryImports.contains(name),
                      case .address(let offset) = exports[symbol] else { continue }
                let address = image.address + UInt64(offset)
                if modeledData.contains(name) {
                    data[address] = name
                    continue
                }
                guard image.text.contains(where: { $0.contains(address) }) else { continue }
                let guest = try guestAddress(address)
                if let prior = result[guest], prior != name, hostServiceIDs[prior] == nil ||
                    hostServiceIDs[prior] != hostServiceIDs[name] {
                    ambiguous.insert(guest)
                } else {
                    result[guest] = name
                }
            }
        }
        for address in ambiguous { result.removeValue(forKey: address) }
        return (result, data)
    }

    func mergedDataBindings(targets: [UInt64: String]) throws -> [(Rebase, String)] {
        guard !targets.isEmpty else { return [] }
        let addresses = Set(targets.keys)
        return try mappings.filter { $0.flags & 4 != 0 && $0.size > 0 }.flatMap {
            try rebases(in: $0.start..<($0.start + $0.size), targets: addresses)
        }.compactMap { rebase in
            guard rebase.authentication == nil, rebase.highByte == 0,
                  let name = targets[rebase.target] else { return nil }
            return (rebase, name)
        }
    }

    func constantRebaseSlot(at guest: Int) throws -> Segment? {
        let address = try originalAddress(guest & ~7)
        // Some cache-global pointer tables are outside all image segments.
        // Only expose actual chain entries in initially read-only const mappings.
        guard mappings.contains(where: { $0.flags & 4 != 0 && $0.protection & 3 == 1 && address >= $0.start &&
            address - $0.start < $0.size && 8 <= $0.size - (address - $0.start) }),
              try !rebases(in: address..<(address + 8)).isEmpty else { return nil }
        return Segment(name: "cache const rebase", address: address, size: 8, fileOffset: 0, fileSize: 8, writable: false)
    }

    func selectorStubInstruction(at guest: Int) throws -> UInt32? {
        guard guest >= Self.guestBase, guest < Self.guestLimit, guest.isMultiple(of: 4) else { return nil }
        let offset = guest & 15
        guard offset < 12 else { return nil }
        let address = try originalAddress(guest - offset)
        guard mappings.contains(where: {
            $0.protection & 4 != 0 &&
                address >= $0.start && address - $0.start < $0.size &&
                16 <= $0.size - (address - $0.start)
        }) else { return nil }
        let code = try bytes(at: address, count: 16)
        // The cache builder materializes the selector in X1 and branches to a message-send entry.
        guard try MachO.integer(code, 0, 4) & 0x9f00_001f == 0x9000_0001,
              try MachO.integer(code, 4, 4) & 0xffc0_03ff == 0x9100_0021,
              try MachO.integer(code, 8, 4) & 0xfc00_0000 == 0x1400_0000,
              try MachO.integer(code, 12, 4) == 0xd420_0020 else { return nil }
        return UInt32(try MachO.integer(code, offset, 4))
    }

    func stubTarget(at guest: Int) throws -> Int? {
        guard guest >= Self.guestBase, guest < Self.guestLimit, guest.isMultiple(of: 16) else { return nil }
        let address = base + UInt64(guest - Self.guestBase)
        guard mappings.contains(where: {
            $0.flags & 8 != 0 && address >= $0.start && address - $0.start < $0.size &&
                16 <= $0.size - (address - $0.start)
        }) else { return nil }
        let code = try bytes(at: address, count: 16)
        let adrp = try MachO.integer(code, 0, 4), add = try MachO.integer(code, 4, 4)
        guard adrp & 0x9f00_001f == 0x9000_0011, add & 0xffc0_03ff == 0x9100_0231,
              try MachO.integer(code, 8, 4) == 0xf940_0230,
              try MachO.integer(code, 12, 4) == 0xd71f_0a11 else { return nil }
        let immediate = Int64(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3))
        let pages = immediate >= 1 << 20 ? immediate - (1 << 21) : immediate
        let slot = Int64(address & ~4095) + pages * 4096 + Int64((add >> 10) & 0xfff)
        guard slot >= 0, slot.isMultiple(of: 8) else { return nil }
        let location = UInt64(slot)
        guard let rebase = try rebases(in: location..<(location + 8)).first,
              rebase.location == location, let auth = rebase.authentication,
              auth.addressDiversity, !auth.dataKey, auth.diversity == 0 else { return nil }
        return try guestAddress(rebase.target)
    }

    func guestImage(_ path: String) throws -> (segments: [Segment], text: [Range<UInt64>], exports: [String: Int],
                                               dependencies: [Dependency])? {
        guard images[path] != nil else { return nil }
        let cachedImage = try image(path)
        struct Lookup: Hashable { let path: String; let symbol: String }
        var parsedImages = [path: cachedImage]
        var exportTables = [String: [String: MachExport]]()
        func parsedImage(_ path: String) throws -> Image {
            if let existing = parsedImages[path] { return existing }
            let value = try self.image(path)
            parsedImages[path] = value
            return value
        }
        func exports(_ image: Image, path: String) throws -> [String: MachExport] {
            if let existing = exportTables[path] { return existing }
            let value = try exportTable(image.segments, image.exports)
            exportTables[path] = value
            return value
        }
        func resolve(_ symbol: String, in path: String, visited: inout Set<Lookup>) throws -> Int? {
            guard visited.insert(Lookup(path: path, symbol: symbol)).inserted else { return nil }
            guard images[path] != nil else { return nil }
            let owner = try parsedImage(path)
            let table = try exports(owner, path: path)
            if case .address(let offset) = table[symbol] {
                return try guestAddress(owner.address + UInt64(offset))
            }
            if case .reexport(let ordinal, let imported) = table[symbol],
               ordinal > 0, ordinal <= owner.dependencies.count {
                return try resolve(imported, in: owner.dependencies[ordinal - 1].name, visited: &visited)
            }
            for dependency in owner.dependencies where dependency.reexported {
                if let address = try resolve(symbol, in: dependency.name, visited: &visited) { return address }
            }
            return nil
        }
        func names(in path: String, visited: inout Set<String>) throws -> Set<String> {
            guard visited.insert(path).inserted, images[path] != nil else { return [] }
            let owner = try parsedImage(path)
            var result = Set(try exports(owner, path: path).keys)
            for dependency in owner.dependencies where dependency.reexported {
                result.formUnion(try names(in: dependency.name, visited: &visited))
            }
            return result
        }
        var resolvedExports = [String: Int]()
        var visitedImages = Set<String>()
        for name in try names(in: path, visited: &visitedImages) {
            var visited = Set<Lookup>()
            if let address = try resolve(name, in: path, visited: &visited) { resolvedExports[name] = address }
        }
        return (cachedImage.segments.filter { $0.name != "__LINKEDIT" && $0.size > 0 }, cachedImage.text, resolvedExports,
                cachedImage.dependencies)
    }

    func rebases(in path: String) throws -> [Rebase] {
        let image = try image(path)
        return try image.segments.filter { $0.name != "__LINKEDIT" && $0.size > 0 }.flatMap {
            try rebases(in: $0.address..<($0.address + $0.size))
        }.sorted { $0.location < $1.location }
    }

    func rebases(in range: Range<UInt64>, targets: Set<UInt64>? = nil) throws -> [Rebase] {
        var result = [Rebase]()
        for mapping in mappings {
            guard let info = mapping.slideInfo else { continue }
            let low = max(range.lowerBound, mapping.start), high = min(range.upperBound, mapping.start + mapping.size)
            guard low < high else { continue }
            func read(_ offset: Int, _ width: Int) throws -> UInt64 {
                guard offset <= info.count, width <= info.count - offset else { throw GuestError("Truncated dyld slide info") }
                return try MachO.integer(mapping.file, info.lowerBound + offset, width)
            }
            guard try read(0, 4) == 5 else { throw GuestError("Unsupported dyld slide info version") }
            let pageSize = try read(4, 4), count = try read(8, 4), base = try read(16, 8)
            guard pageSize == 4096 || pageSize == 16384, info.count >= 24,
                  count <= UInt64((info.count - 24) / 2), count == mapping.size / pageSize else {
                throw GuestError("Invalid dyld slide page table")
            }
            let firstPage = (low - mapping.start) / pageSize
            let lastPage = (high - 1 - mapping.start) / pageSize
            for page in firstPage...lastPage {
                var offset = try read(24 + Int(page) * 2, 2)
                if offset == 0xffff { continue }
                let pageOffset = page * pageSize
                while true {
                    guard offset.isMultiple(of: 8), offset <= pageSize - 8 else {
                        throw GuestError("Invalid dyld slide chain offset")
                    }
                    let location = mapping.start + pageOffset + offset
                    if location >= high { break }
                    let raw = try MachO.integer(mapping.file, Int(mapping.fileOffset + pageOffset + offset), 8)
                    if location >= low {
                        guard high - location >= 8 else { throw GuestError("Dyld rebase crosses a segment boundary") }
                        let runtimeOffset = raw & ((1 << 34) - 1)
                        guard runtimeOffset <= UInt64.max - base else { throw GuestError("Dyld rebase target overflow") }
                        let target = base + runtimeOffset
                        if targets?.contains(target) != false {
                            let auth = raw >> 63 != 0
                            let authentication = auth ? Rebase.Authentication(diversity: UInt16(truncatingIfNeeded: raw >> 34),
                                addressDiversity: (raw >> 50) & 1 != 0, dataKey: (raw >> 51) & 1 != 0) : nil
                            result.append(Rebase(location: location, target: target,
                                                 highByte: auth ? 0 : UInt8(truncatingIfNeeded: raw >> 34),
                                                 authentication: authentication))
                        }
                    }
                    let next = (raw >> 52) & 0x7ff
                    if next == 0 { break }
                    offset += next * 8
                }
            }
        }
        return result
    }
}
