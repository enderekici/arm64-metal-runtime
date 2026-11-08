import Foundation

// Indices follow _dyld_section_location_kind in dyld_priv.h.
let dyldSectionNames = [
    "__swift5_protos", "__swift5_proto", "__swift5_types", "__swift5_replace", "__swift5_replac2", "__swift5_acfuncs",
    "__objc_imageinfo", "__objc_selrefs", "__objc_msgrefs", "__objc_classrefs", "__objc_superrefs",
    "__objc_protorefs", "__objc_classlist", "__objc_nlclslist", "__objc_stublist", "__objc_catlist",
    "__objc_catlist2", "__objc_nlcatlist", "__objc_protolist", "__objc_fork_ok", "__objc_rawisa",
]

func dyldSection(in commands: Data, count: Int, kind: Int) throws -> (address: UInt64, size: UInt64)? {
    func name(at offset: Int) -> String {
        String(decoding: commands[offset..<(offset + 16)].prefix { $0 != 0 }, as: UTF8.self)
    }
    var cursor = 0
    var result: (UInt64, UInt64)?
    for _ in 0..<count {
        let type = try MachO.integer(commands, cursor, 4)
        let size = Int(try MachO.integer(commands, cursor + 4, 4))
        guard size >= 8, size <= commands.count - cursor else { throw GuestError("Invalid guest Mach-O command") }
        defer { cursor += size }
        guard type == 0x19, size >= 72 else { continue }
        let segment = name(at: cursor + 8)
        guard kind < 6 ? segment == "__TEXT" : segment.hasPrefix("__DATA") else { continue }
        let sections = Int(try MachO.integer(commands, cursor + 64, 4))
        guard sections <= (size - 72) / 80 else { throw GuestError("Invalid guest Mach-O section table") }
        for index in 0..<sections {
            let section = cursor + 72 + 80 * index
            if name(at: section) == dyldSectionNames[kind] {
                result = (try MachO.integer(commands, section + 32, 8),
                          try MachO.integer(commands, section + 40, 8))
            }
        }
    }
    return result
}

private struct ImageDependency {
    let name: String
    let weak: Bool
    let reexported: Bool
    let currentVersion: UInt32
}

struct GuestBuildVersion {
    let platform: UInt32
    let minimum: UInt32
    let sdk: UInt32

    func token(sdk querySDK: Bool) -> UInt64 {
        UInt64(querySDK ? sdk : minimum) << 32 | UInt64(platform)
    }
}

private struct MachImage {
    let memory: Data
    let entry: Int?
    let preferredBase: UInt64
    let uuid: Data
    let restricted: Bool
    let buildVersion: GuestBuildVersion?
    let code: [Range<Int>]
    let stubs: [Int: ChainedImport]
    let bindings: [Int: ChainedImport]
    let authentication: [Int: ChainedAuthentication]
    let writableRegions: [Range<Int>]
    let immutableRegions: [Range<Int>]
    let dependencies: [ImageDependency]
    let rpaths: [String]
    let exports: [String: MachExport]
    let initializers: [Int]

    init(_ binary: URL, executable: Bool, placement: Int) throws {
        let file = try MachO(binary, allowARM64e: true)
        guard try file.word(12) == (executable ? 2 : 6) else {
            throw GuestError(executable ? "Launch requires an MH_EXECUTE Mach-O" : "Expected a guest Mach-O dylib at \(binary.path)")
        }
        let commandCount = Int(try file.word(16))
        let commandEnd = 32 + Int(try file.word(20))
        guard commandEnd <= file.bytes.count, commandCount <= (commandEnd - 32) / 8 else {
            throw GuestError("Invalid executable load commands")
        }
        func quad(_ offset: Int) throws -> UInt64 { try MachO.integer(file.bytes, offset, 8) }
        var cursor = 32
        var segments: [(address: UInt64, size: UInt64, offset: UInt64, bytes: UInt64, writable: Bool)] = []
        var base: UInt64?
        var entryOffset: UInt64?
        var uuid = Data(repeating: 0, count: 16)
        var restricted = false
        var buildVersion: GuestBuildVersion?
        var textSection: (address: UInt64, size: UInt64)?
        var messageStubs: [(address: UInt64, size: UInt64)] = []
        var stubs: (address: UInt64, size: UInt64, index: Int, stride: Int)?
        var pointerSections: [(address: UInt64, size: UInt64, index: Int, stride: Int)] = []
        var symbols: (offset: Int, count: Int, strings: Int, size: Int)?
        var indirect: (offset: Int, count: Int)?
        var fixupRange: Range<Int>?
        var exportsRange: Range<Int>?
        var initializerSections: [(address: UInt64, size: UInt64, pointers: Bool)] = []
        var dependencies = [ImageDependency]()
        var rpaths = [String]()
        for _ in 0..<commandCount {
            guard cursor <= commandEnd - 8 else { throw GuestError("Truncated executable load command") }
            let command = try file.word(cursor)
            let length = Int(try file.word(cursor + 4))
            guard length >= 8, length <= commandEnd - cursor else {
                throw GuestError("Invalid executable load command")
            }
            switch command {
            case 0x19:
                guard length >= 72 else { throw GuestError("Truncated executable segment") }
                let name = String(decoding: file.bytes[(cursor + 8)..<(cursor + 24)].prefix { $0 != 0 }, as: UTF8.self)
                let address = try quad(cursor + 24)
                if name == "__TEXT" { base = address }
                if name != "__PAGEZERO" && name != "__LINKEDIT" {
                    segments.append((address, try quad(cursor + 32), try quad(cursor + 40), try quad(cursor + 48),
                                     try file.word(cursor + 60) & 2 != 0))
                }
                let sectionCount = Int(try file.word(cursor + 64))
                guard sectionCount <= (length - 72) / 80 else { throw GuestError("Invalid executable sections") }
                for index in 0..<sectionCount {
                    let section = cursor + 72 + index * 80
                    let sectionName = String(decoding: file.bytes[section..<(section + 16)].prefix { $0 != 0 }, as: UTF8.self)
                    if name == "__RESTRICT", sectionName == "__restrict" { restricted = true }
                    if name == "__TEXT", sectionName == "__text" {
                        textSection = (try quad(section + 32), try quad(section + 40))
                    }
                    if name == "__TEXT", sectionName == "__objc_stubs" {
                        let start = try quad(section + 32)
                        let size = try quad(section + 40)
                        let fileSize = try quad(cursor + 48)
                        guard try file.word(cursor + 60) & 4 != 0,
                              start >= address, start - address <= fileSize,
                              size <= fileSize - (start - address),
                              start.isMultiple(of: 4), size.isMultiple(of: 4) else {
                            throw GuestError("Invalid Objective-C message stub section")
                        }
                        if size != 0 { messageStubs.append((start, size)) }
                    }
                    let type = try file.word(section + 64) & 0xff
                    if type == 10 { throw GuestError("Guest image finalizers are unsupported") }
                    if type == 9 || type == 0x16 {
                        initializerSections.append((try quad(section + 32), try quad(section + 40), type == 9))
                    }
                    if type == 6 {
                        pointerSections.append((try quad(section + 32), try quad(section + 40),
                                                Int(try file.word(section + 68)), 8))
                    }
                    if type == 8 {
                        guard stubs == nil else { throw GuestError("Multiple import stub sections are unsupported") }
                        stubs = (try quad(section + 32), try quad(section + 40),
                                 Int(try file.word(section + 68)), Int(try file.word(section + 72)))
                    }
                }
            case 0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023:
                guard length >= 24 else { throw GuestError("Truncated dylib load command") }
                let name = Int(try file.word(cursor + 8))
                guard name >= 24, name < length,
                      let end = file.bytes[(cursor + name)..<(cursor + length)].firstIndex(of: 0) else {
                    throw GuestError("Invalid dylib install name")
                }
                dependencies.append(ImageDependency(name: String(decoding: file.bytes[(cursor + name)..<end], as: UTF8.self),
                                                    weak: command == 0x80000018, reexported: command == 0x8000001f,
                                                    currentVersion: try file.word(cursor + 16)))
            case 0x1b where executable:
                guard length >= 24 else { throw GuestError("Truncated executable UUID") }
                uuid = file.bytes.subdata(in: (cursor + 8)..<(cursor + 24))
            case 0x32 where executable:
                guard length >= 24 else { throw GuestError("Truncated guest build version") }
                let platform = try file.word(cursor + 8)
                let minimum = try file.word(cursor + 12)
                let sdk = try file.word(cursor + 16)
                buildVersion = GuestBuildVersion(platform: platform, minimum: minimum, sdk: sdk == 0 ? minimum : sdk)
            case 0x24 where executable && buildVersion == nil:
                guard length >= 16 else { throw GuestError("Truncated guest minimum version") }
                let minimum = try file.word(cursor + 8)
                let sdk = try file.word(cursor + 12)
                buildVersion = GuestBuildVersion(platform: 1, minimum: minimum, sdk: sdk == 0 ? minimum : sdk)
            case 0x8000001c:
                guard length >= 12 else { throw GuestError("Truncated rpath load command") }
                let start = Int(try file.word(cursor + 8))
                guard start >= 12, start < length,
                      let end = file.bytes[(cursor + start)..<(cursor + length)].firstIndex(of: 0) else {
                    throw GuestError("Invalid Mach-O rpath")
                }
                rpaths.append(String(decoding: file.bytes[(cursor + start)..<end], as: UTF8.self))
            case 0x80000033 where length >= 16, 0x22 where length >= 48, 0x80000022 where length >= 48:
                let delta = command == 0x80000033 ? 8 : 40
                let start = Int(try file.word(cursor + delta))
                let size = Int(try file.word(cursor + delta + 4))
                guard start <= file.bytes.count, size <= file.bytes.count - start else {
                    throw GuestError("Invalid Mach-O exports trie range")
                }
                if size > 0 { exportsRange = start..<(start + size) }
            case 0x80000034 where length >= 16:
                let start = Int(try file.word(cursor + 8))
                let size = Int(try file.word(cursor + 12))
                guard start <= file.bytes.count, size <= file.bytes.count - start else {
                    throw GuestError("Invalid dyld chained fixup payload")
                }
                fixupRange = start..<(start + size)
            case 0x80000028 where length >= 24:
                entryOffset = try quad(cursor + 8)
            case 2 where length >= 24:
                symbols = (Int(try file.word(cursor + 8)), Int(try file.word(cursor + 12)),
                           Int(try file.word(cursor + 16)), Int(try file.word(cursor + 20)))
            case 0xb where length >= 80:
                indirect = (Int(try file.word(cursor + 56)), Int(try file.word(cursor + 60)))
            default:
                break
            }
            cursor += length
        }
        guard let base, base.isMultiple(of: 4096), let textSection else {
            throw GuestError("Missing executable entry point or text segment")
        }

        let cap: UInt64 = 32 * 1024 * 1024
        func region(_ address: UInt64, _ size: UInt64) throws -> Range<Int> {
            guard address >= base, address - base <= cap, size <= cap - (address - base) else {
                throw GuestError("Executable exceeds the guest image limit")
            }
            return Int(address - base)..<Int(address - base + size)
        }
        let text = try region(textSection.address, textSection.size)
        if executable {
            guard let entryOffset, entryOffset <= cap, text.contains(Int(entryOffset)),
                  entryOffset.isMultiple(of: 4) else { throw GuestError("LC_MAIN entry lies outside executable text") }
        }
        var memory = Data()
        var writableRegions = [Range<Int>]()
        var mappedRegions = [Range<Int>]()
        for segment in segments {
            let range = try region(segment.address, segment.size)
            mappedRegions.append(range)
            if segment.writable { writableRegions.append(range) }
            guard segment.offset <= file.bytes.count, segment.bytes <= UInt64(file.bytes.count) - segment.offset,
                  segment.bytes <= segment.size else { throw GuestError("Invalid executable segment bounds") }
            if memory.count < range.upperBound { memory.count = range.upperBound }
            memory.replaceSubrange(range.lowerBound..<(range.lowerBound + Int(segment.bytes)),
                                   with: file.bytes[Int(segment.offset)..<Int(segment.offset + segment.bytes)])
        }
        guard text.upperBound <= memory.count else { throw GuestError("Missing executable text bytes") }

        func names(in stubs: (address: UInt64, size: UInt64, index: Int, stride: Int)) throws -> [Int: ChainedImport] {
            var imports = [Int: ChainedImport]()
            let range = try region(stubs.address, stubs.size)
            guard let symbols, let indirect, stubs.stride > 0, range.count.isMultiple(of: stubs.stride),
                  symbols.offset <= file.bytes.count, symbols.count <= (file.bytes.count - symbols.offset) / 16,
                  symbols.strings <= file.bytes.count, symbols.size <= file.bytes.count - symbols.strings,
                  indirect.offset <= file.bytes.count, indirect.count <= (file.bytes.count - indirect.offset) / 4,
                  stubs.index <= indirect.count, range.count / stubs.stride <= indirect.count - stubs.index else {
                throw GuestError("Invalid executable import tables")
            }
            for (index, address) in stride(from: range.lowerBound, to: range.upperBound, by: stubs.stride).enumerated() {
                let symbol = Int(try file.word(indirect.offset + (stubs.index + index) * 4))
                guard symbol < symbols.count else { continue }
                let string = Int(try file.word(symbols.offset + symbol * 16))
                guard string < symbols.size else { throw GuestError("Invalid import name") }
                let start = symbols.strings + string
                let name = file.bytes[start..<(symbols.strings + symbols.size)].prefix { $0 != 0 }
                let ordinal = Int(file.bytes[symbols.offset + symbol * 16 + 7])
                imports[address] = ChainedImport(name: String(decoding: name, as: UTF8.self),
                                                 ordinal: ordinal, weak: false, addend: 0)
            }
            return imports
        }
        self.stubs = try stubs.map { try names(in: $0) } ?? [:]
        var bindings = [Int: ChainedImport]()
        for section in pointerSections {
            for (address, name) in try names(in: section) { bindings[address] = name }
        }
        if let fixupRange {
            let chained = try applyChainedFixups(file.bytes.subdata(in: fixupRange), imageBase: base, placement: placement,
                                                 memory: &memory, mappedRegions: mappedRegions)
            authentication = chained.authentication
            for (address, reference) in chained.bindings { bindings[address] = reference }
        } else {
            authentication = [:]
        }

        var exports = [String: MachExport]()
        if let exportsRange {
            exports = try machExports(file.bytes.subdata(in: exportsRange), memorySize: memory.count)
        } else if let symbols {
            guard symbols.offset <= file.bytes.count, symbols.count <= (file.bytes.count - symbols.offset) / 16,
                  symbols.strings <= file.bytes.count, symbols.size <= file.bytes.count - symbols.strings else {
                throw GuestError("Invalid guest export symbol table")
            }
            for index in 0..<symbols.count {
                let offset = symbols.offset + index * 16
                let type = file.bytes[offset + 4]
                guard type & 0xff == 0x0f else { continue }
                let address = try quad(offset + 8)
                guard address >= base, address - base < UInt64(memory.count) else { continue }
                let start = Int(try file.word(offset))
                guard start < symbols.size else { throw GuestError("Invalid guest export name") }
                let name = file.bytes[(symbols.strings + start)..<(symbols.strings + symbols.size)].prefix { $0 != 0 }
                exports[String(decoding: name, as: UTF8.self)] = .address(Int(address - base))
            }
        }

        var initializers = [Int]()
        for section in initializerSections {
            let range = try region(section.address, section.size)
            let width = section.pointers ? 8 : 4
            guard range.count.isMultiple(of: width), range.upperBound <= memory.count else {
                throw GuestError("Invalid guest initializer section")
            }
            for offset in stride(from: range.lowerBound, to: range.upperBound, by: width) {
                let raw = try MachO.integer(memory, offset, width)
                guard raw <= UInt32.max else { throw GuestError("Unsupported guest initializer pointer") }
                let address = section.pointers ? Int(raw) - placement : Int(raw)
                guard text.contains(address), address.isMultiple(of: 4) else {
                    throw GuestError("Guest initializer outside its image text")
                }
                initializers.append(address)
            }
        }

        self.exports = exports
        self.initializers = initializers
        self.buildVersion = buildVersion
        self.uuid = uuid
        self.restricted = restricted
        self.preferredBase = base
        self.bindings = bindings
        self.dependencies = dependencies
        self.rpaths = rpaths
        self.writableRegions = writableRegions
        immutableRegions = mappedRegions.filter { !writableRegions.contains($0) }
        self.memory = memory
        entry = executable ? entryOffset.flatMap(Int.init(exactly:)) : nil
        code = [text] + (try messageStubs.map { try region($0.address, $0.size) })
    }
}

struct ExecutableImage {
    static let minimumImageAddress = 16_384
    static let objcMutationInvoke = Int(guestCodeLimit) - 4
    struct CacheRegion {
        let logical: Range<Int>
        let backing: Int
        var writable: Bool
        var readable = true
    }
    struct DeferredCacheSegment {
        let logical: Range<Int>
        let segment: SharedCache.Segment
    }

    let memory: Data
    let entry: Int
    let mainHeader: Int
    let mainUUID: Data
    let mainRestricted: Bool
    let textRegions: [Range<Int>]
    let initializers: [Int]
    let objcBootstrap: Int?
    let imageSlides: [UInt64: UInt64]
    let imagePaths: [UInt64: String]
    let cachedImageRanges: [(range: Range<Int>, header: Int)]
    private let linkedLibraries: [ImageDependency]
    let buildVersion: GuestBuildVersion?
    let imports: [Int: String]
    let guestStubs: [Int: Int]
    let bindings: [Int: String]
    let functionBindings: [Int: Int]
    let authentication: [Int: ChainedAuthentication]
    let writableRegions: [Range<Int>]
    let immutableRegions: [Range<Int>]
    let cacheRegions: [CacheRegion]
    let linearImageEnd: Int
    let cacheReader: SharedCache?
    let deferredCache: [DeferredCacheSegment]
    let deferredBindings: [Int: String]
    let cachedTaskPort: Int?
    let swiftPrespecializedData: Int?
    let cachedDispatchMainQueue: Int?
    var cacheEnabled: Bool { !cacheRegions.isEmpty || !deferredCache.isEmpty }
    var cacheBackingStart: Int { (memory.count + 15) & ~15 }
    var cacheBackingEnd: Int { cacheBackingStart + (deferredCache.isEmpty ? 0 : 64 * 1024 * 1024) }
    var mappingBackingEnd: Int { cacheBackingEnd + 16 * 1024 * 1024 }

    func linkTimeVersion(named name: String) -> UInt32 {
        for dependency in linkedLibraries.reversed() {
            let leaf = dependency.name.split(separator: "/").last ?? ""
            if leaf == name || (leaf.hasPrefix("lib\(name).") && leaf.hasSuffix(".dylib")) {
                return dependency.currentVersion
            }
        }
        return .max
    }

    func isCode(_ address: Int) -> Bool {
        address.isMultiple(of: 4) && textRegions.contains { $0.contains(address) && address <= $0.upperBound - 4 }
    }

    func instruction(at address: Int) throws -> UInt32 {
        if let cacheReader, address >= SharedCache.guestBase, !deferredCache.isEmpty {
            let bytes = try cacheReader.bytes(at: cacheReader.originalAddress(address), count: 4)
            return UInt32(try MachO.integer(bytes, 0, 4))
        }
        let backing = cacheRegions.first(where: { $0.logical.contains(address) })
            .map { $0.backing + address - $0.logical.lowerBound } ?? address
        return UInt32(try MachO.integer(memory, backing, 4))
    }

    init(_ binary: URL, sharedCache: Bool = false, demandCache: Bool = false) throws {
        struct Loaded {
            let image: MachImage
            let base: Int
            var dependencies: [Int?]
        }
        let root = binary.standardizedFileURL
        var loaded = [Loaded]()
        var known = [String: Int]()
        var initializationOrder = [Int]()
        var memory = Data()
        let files = FileManager.default
        let libSystem = "/usr/lib/libSystem.B.dylib"

        func expanded(_ path: String, loader: URL) -> String {
            for (prefix, directory) in [("@loader_path", loader.deletingLastPathComponent()),
                                        ("@executable_path", root.deletingLastPathComponent())] {
                if path == prefix { return directory.path }
                if path.hasPrefix(prefix + "/") {
                    return directory.appendingPathComponent(String(path.dropFirst(prefix.count + 1))).path
                }
            }
            return path
        }
        func load(_ url: URL, rootImage: Bool, inheritedRpaths: [String]) throws -> Int {
            let canonical = url.resolvingSymlinksInPath().standardizedFileURL
            if let existing = known[canonical.path] { return existing }
            let base = max(Self.minimumImageAddress, (memory.count + 16_383) & ~16_383)
            let image = try MachImage(canonical, executable: rootImage, placement: base)
            guard base + image.memory.count <= 64 * 1024 * 1024, loaded.count < 256 else {
                throw GuestError("Guest image graph exceeds loader limits")
            }
            memory.count = base
            memory.append(image.memory)
            let index = loaded.count
            loaded.append(Loaded(image: image, base: base, dependencies: []))
            known[canonical.path] = index
            let rpaths = image.rpaths.map { expanded($0, loader: canonical) } + inheritedRpaths
            var dependencies = [Int?]()
            for dependency in image.dependencies {
                let name = dependency.name
                if name == libSystem || name.hasPrefix("/usr/lib/") || name.hasPrefix("/System/Library/") {
                    // Shared-cache images are still outside the guest loader.
                    dependencies.append(nil)
                    continue
                }
                let candidates: [String]
                if name.hasPrefix("@rpath/") {
                    let leaf = String(name.dropFirst("@rpath/".count))
                    candidates = rpaths.map { URL(fileURLWithPath: $0).appendingPathComponent(leaf).path }
                } else {
                    candidates = [expanded(name, loader: canonical)]
                }
                if let path = candidates.first(where: { !$0.hasPrefix("@") && files.fileExists(atPath: $0) }) {
                    dependencies.append(try load(URL(fileURLWithPath: path), rootImage: false, inheritedRpaths: rpaths))
                } else if dependency.weak {
                    dependencies.append(nil)
                } else {
                    throw GuestError("Unable to resolve guest dylib \(name)")
                }
            }
            loaded[index].dependencies = dependencies
            initializationOrder.append(index)
            return index
        }

        _ = try load(root, rootImage: true, inheritedRpaths: [])
        guard let entry = loaded[0].image.entry else { throw GuestError("Missing executable entry") }
        var imports = [Int: String]()
        var guestStubs = [Int: Int]()
        var bindings = [Int: String]()
        var functionBindings = [Int: Int]()
        var authentication = [Int: ChainedAuthentication]()
        var writable = [Range<Int>]()
        var text = [Range<Int>]()
        var synthetic = [String: Int]()
        let linearImageEnd = memory.count
        var cacheRegions = [CacheRegion]()
        var slides = Dictionary(uniqueKeysWithValues: loaded.map { (UInt64($0.base), UInt64($0.base) &- $0.image.preferredBase) })
        var paths = Dictionary(uniqueKeysWithValues: known.map { (UInt64(loaded[$0.value].base), $0.key) })
        var cacheExports = [String: [String: Int]]()
        var imageRanges = [(range: Range<Int>, header: Int)]()
        var cacheReader: SharedCache?
        var deferredCache = [DeferredCacheSegment]()
        var deferredBindings = [Int: String]()
        var cachedTaskPort: Int?
        var swiftPrespecializedData: Int?
        if sharedCache {
            let roots = Set(loaded.flatMap { $0.image.dependencies.map(\.name) }).filter {
                $0 != libSystem && ($0.hasPrefix("/usr/lib/") || $0.hasPrefix("/System/Library/"))
            }
            let cache = try SharedCache()
            var names = Set<String>()
            var cachedImages = [String: (segments: [SharedCache.Segment], text: [Range<UInt64>],
                                          exports: [String: Int], dependencies: [SharedCache.Dependency])]()
            var cachedBytes: UInt64 = 0
            var pending = roots.sorted(by: >)
            if loaded.contains(where: {
                $0.image.stubs.values.contains { $0.name == "__dyld_get_swift_prespecialized_data" }
            }), let address = try cache.swiftPrespecializedAddress() {
                swiftPrespecializedData = address
                pending.append(SharedCache.swiftPrespecializedLibrary)
            }
            while let name = pending.popLast() {
                guard names.insert(name).inserted, let cached = try cache.guestImage(name) else { continue }
                guard names.count <= (demandCache ? 1024 : 256) else { throw GuestError("Cached guest image graph exceeds loader limits") }
                for segment in cached.segments where !demandCache {
                    guard segment.size <= 64 * 1024 * 1024, cachedBytes <= 64 * 1024 * 1024 - segment.size else {
                        throw GuestError("Cached guest image graph exceeds 64 MiB at \(name) (\(cachedBytes) bytes before \(segment.name))")
                    }
                    cachedBytes += segment.size
                }
                cachedImages[name] = cached
                if name == "/usr/lib/swift/libswiftCore.dylib", swiftPrespecializedData == nil,
                   let address = try cache.swiftPrespecializedAddress() {
                    swiftPrespecializedData = address
                    pending.append(SharedCache.swiftPrespecializedLibrary)
                }
                pending.append(contentsOf: cached.dependencies.map(\.name).filter {
                    $0 != libSystem && ($0.hasPrefix("/usr/lib/") || $0.hasPrefix("/System/Library/"))
                })
            }
            if !names.isEmpty {
                cacheReader = cache
                for name in names.sorted() {
                    guard let image = cachedImages[name] else { continue }
                    cacheExports[name] = image.exports
                    var imageHeader: Int?
                    if let textSegment = image.segments.first(where: { $0.name == "__TEXT" }) {
                        let address = UInt64(try cache.guestAddress(textSegment.address))
                        imageHeader = Int(address)
                        slides[address] = address &- textSegment.address
                        paths[address] = name
                    }
                    for segment in image.segments {
                        let address = try cache.guestAddress(segment.address)
                        let length = Int(segment.size)
                        if let imageHeader, segment.name.hasPrefix("__DATA") || segment.name.hasPrefix("__AUTH") {
                            imageRanges.append((address..<(address + length), imageHeader))
                        }
                        if demandCache {
                            guard length <= SharedCache.guestLimit - address else {
                                throw GuestError("Deferred cache segment exceeds guest window")
                            }
                            let logical = address..<(address + length)
                            if let existing = deferredCache.first(where: { $0.logical.overlaps(logical) }) {
                                guard existing.logical == logical, existing.segment.fileSize == segment.fileSize,
                                      existing.segment.writable == segment.writable else {
                                    throw GuestError("Overlapping deferred cache segments")
                                }
                            } else {
                                deferredCache.append(DeferredCacheSegment(logical: logical, segment: segment))
                            }
                            continue
                        }
                        let backing = ((memory.count + 15) & ~15) + (address & 15)
                        guard length <= 64 * 1024 * 1024 - backing,
                              length <= SharedCache.guestLimit - address,
                              !cacheRegions.contains(where: { $0.logical.overlaps(address..<(address + length)) }) else {
                            throw GuestError("Invalid or excessive cached guest segments")
                        }
                        memory.count = backing + length
                        let bytes = try cache.bytes(at: segment.address, count: Int(segment.fileSize))
                        memory.replaceSubrange(backing..<(backing + bytes.count), with: bytes)
                        cacheRegions.append(CacheRegion(logical: address..<(address + length), backing: backing,
                                                        writable: segment.writable))
                    }
                    for range in image.text where !range.isEmpty {
                        let start = try cache.guestAddress(range.lowerBound)
                        let end = try cache.guestAddress(range.upperBound)
                        let mapped = demandCache
                            ? deferredCache.contains { $0.logical.lowerBound <= start && end <= $0.logical.upperBound }
                            : cacheRegions.contains { $0.logical.lowerBound <= start && end <= $0.logical.upperBound }
                        guard mapped else {
                            throw GuestError("Cached text lies outside guest segments")
                        }
                        text.append(start..<end)
                    }
                    if demandCache { continue }
                    for rebase in try cache.rebases(in: name) {
                        let address = try cache.guestAddress(rebase.location)
                        guard let region = cacheRegions.first(where: { $0.logical.contains(address) && address <= $0.logical.upperBound - 8 }) else {
                            throw GuestError("Cached rebase lies outside guest segments")
                        }
                        let offset = region.backing + address - region.logical.lowerBound
                        var pointer = (UInt64(try cache.guestAddress(rebase.target)) | UInt64(rebase.highByte) << 56).littleEndian
                        withUnsafeBytes(of: &pointer) { memory.replaceSubrange(offset..<(offset + 8), with: $0) }
                        if let auth = rebase.authentication {
                            let diversity = UInt64(auth.diversity)
                            let modifier = auth.addressDiversity ? UInt64(address) | (diversity << 48) : diversity
                            authentication[offset] = ChainedAuthentication(key: auth.dataKey ? 2 : 0, modifier: modifier)
                        }
                    }
                }
                if !cacheRegions.isEmpty || !deferredCache.isEmpty {
                    let hostExports = try cache.hostExportAddresses()
                    if let address = hostExports.data.first(where: { $0.value == "_mach_task_self_" })?.key {
                        cachedTaskPort = try cache.guestAddress(address)
                    }
                    imports.merge(hostExports.functions) { original, _ in original }
                    for (rebase, name) in try cache.mergedDataBindings(targets: hostExports.data) {
                        guard let address = try? cache.guestAddress(rebase.location),
                              address <= SharedCache.guestLimit - 8 else { continue }
                        let range = address..<(address + 8)
                        if demandCache, deferredCache.contains(where: {
                            $0.logical.contains(address) && range.upperBound <= $0.logical.upperBound
                        }) {
                            deferredBindings[address] = name
                            continue
                        }
                        if let region = cacheRegions.first(where: { $0.logical.overlaps(range) }) {
                            guard region.logical.contains(address), range.upperBound <= region.logical.upperBound else {
                                throw GuestError("Cached global binding crosses a segment boundary")
                            }
                            bindings[region.backing + address - region.logical.lowerBound] = name
                        } else {
                            let backing = (memory.count + 7) & ~7
                            guard backing <= 64 * 1024 * 1024 - 8 else {
                                throw GuestError("Cached global bindings exceed loader limits")
                            }
                            memory.count = backing + 8
                            cacheRegions.append(CacheRegion(logical: range, backing: backing, writable: false))
                            bindings[backing] = name
                        }
                    }
                }
            }
        }

        struct ExportLookup: Hashable {
            let image: Int
            let name: String
        }
        func resolveExport(_ name: String, in index: Int, visited: inout Set<ExportLookup>) -> Int? {
            guard visited.insert(ExportLookup(image: index, name: name)).inserted else { return nil }
            let owner = loaded[index]
            switch owner.image.exports[name] {
            case .address(let offset):
                return owner.base + offset
            case .reexport(let ordinal, let imported):
                guard ordinal <= owner.dependencies.count, let dependency = owner.dependencies[ordinal - 1] else { return nil }
                return resolveExport(imported, in: dependency, visited: &visited)
            case nil:
                for (ordinal, entry) in owner.image.dependencies.enumerated() where entry.reexported {
                    if let dependency = owner.dependencies[ordinal],
                       let target = resolveExport(name, in: dependency, visited: &visited) { return target }
                }
                return nil
            }
        }
        func resolve(_ reference: ChainedImport, in index: Int) -> (target: Int?, name: String) {
            let owner = loaded[index]
            let ordinal = reference.ordinal
            var visited = Set<ExportLookup>()
            if ordinal == 0 {
                let target = resolveExport(reference.name, in: index, visited: &visited)
                return (target, "self:\(reference.name)")
            }
            guard ordinal > 0, ordinal <= owner.dependencies.count else {
                return (nil, "ordinal \(ordinal):\(reference.name)")
            }
            let library = owner.image.dependencies[ordinal - 1].name
            if let dependency = owner.dependencies[ordinal - 1] {
                let target = resolveExport(reference.name, in: dependency, visited: &visited)
                return (target, "\(library):\(reference.name)")
            }
            return (cacheExports[library]?[reference.name], library == libSystem ? reference.name : "\(library):\(reference.name)")
        }

        for (index, loadedImage) in loaded.enumerated() {
            let image = loadedImage.image
            let base = loadedImage.base
            text += image.code.map { (base + $0.lowerBound)..<(base + $0.upperBound) }
            writable += image.writableRegions.map { (base + $0.lowerBound)..<(base + $0.upperBound) }
            for (offset, reference) in image.stubs {
                let address = base + offset
                let resolved = resolve(reference, in: index)
                if let target = resolved.target { guestStubs[address] = target }
                else { imports[address] = resolved.name }
            }
            for (offset, reference) in image.bindings.sorted(by: { $0.key < $1.key }) {
                let address = base + offset
                let resolved = resolve(reference, in: index)
                bindings[address] = resolved.name
                if let target = resolved.target {
                    functionBindings[address] = target
                } else if !reference.weak || hostServiceIDs[resolved.name] != nil || guestMemoryImports.contains(resolved.name) {
                    let stubBase = cacheRegions.isEmpty && deferredCache.isEmpty ? 0x8000_0000 : SharedCache.guestLimit
                    let target = synthetic[resolved.name] ?? (stubBase + 4 * synthetic.count)
                    synthetic[resolved.name] = target
                    imports[target] = resolved.name
                    functionBindings[address] = target
                }
            }
            for (offset, signature) in image.authentication { authentication[base + offset] = signature }
        }
        self.memory = memory
        mainHeader = loaded[0].base
        mainUUID = loaded[0].image.uuid
        mainRestricted = loaded[0].image.restricted
        self.entry = mainHeader + entry
        imageSlides = slides
        imagePaths = paths
        cachedImageRanges = imageRanges
        linkedLibraries = loaded[0].image.dependencies
        buildVersion = loaded[0].image.buildVersion
        objcBootstrap = cacheExports["/usr/lib/libobjc.A.dylib"]?["__objc_init"]
        initializers = initializationOrder.flatMap { index in
            loaded[index].image.initializers.map { loaded[index].base + $0 }
        }
        textRegions = text
        imports[Self.objcMutationInvoke] = "__guest_objc_make_image_mutable"
        self.imports = imports
        self.guestStubs = guestStubs
        self.bindings = bindings
        self.functionBindings = functionBindings
        self.authentication = authentication
        writableRegions = writable
        immutableRegions = loaded.flatMap { loadedImage in
            loadedImage.image.immutableRegions.map {
                (loadedImage.base + $0.lowerBound)..<(loadedImage.base + $0.upperBound)
            }
        }
        self.cacheRegions = cacheRegions
        self.linearImageEnd = linearImageEnd
        self.cacheReader = cacheRegions.isEmpty && deferredCache.isEmpty ? nil : cacheReader
        self.deferredCache = deferredCache.sorted { $0.logical.lowerBound < $1.logical.lowerBound }
        self.deferredBindings = deferredBindings
        self.cachedTaskPort = cachedTaskPort
        self.swiftPrespecializedData = swiftPrespecializedData
        cachedDispatchMainQueue = cacheExports["/usr/lib/system/libdispatch.dylib"]?["__dispatch_main_q"]
    }
}
