import Foundation

struct ChainedImport {
    let name: String
    let ordinal: Int
    let weak: Bool
    let addend: Int64
}

struct ChainedAuthentication {
    let key: UInt64
    let modifier: UInt64
}

func applyChainedFixups(_ payload: Data, imageBase: UInt64, placement: Int, memory: inout Data,
                        mappedRegions: [Range<Int>]) throws -> (bindings: [Int: ChainedImport], authentication: [Int: ChainedAuthentication]) {
    func read(_ offset: Int, _ size: Int) throws -> UInt64 {
        try MachO.integer(payload, offset, size)
    }
    guard try read(0, 4) == 0, try read(24, 4) == 0 else {
        throw GuestError("Unsupported dyld chained fixup header")
    }
    let starts = Int(try read(4, 4))
    let importOffset = Int(try read(8, 4))
    let strings = Int(try read(12, 4))
    let importCount = Int(try read(16, 4))
    let format = try read(20, 4)
    guard let recordSize = [1: 4, 2: 8, 3: 16][format], strings <= payload.count,
          importOffset <= payload.count, importCount <= (payload.count - importOffset) / recordSize else {
        throw GuestError("Invalid dyld chained imports")
    }
    var imports = [ChainedImport]()
    for index in 0..<importCount {
        let offset = importOffset + index * recordSize
        let record = try read(offset, format == 3 ? 8 : 4)
        let nameOffset = Int(record >> (format == 3 ? 32 : 9))
        guard nameOffset < payload.count - strings else { throw GuestError("Invalid dyld import name offset") }
        let nameStart = strings + nameOffset
        guard let end = payload[nameStart...].firstIndex(of: 0), end > nameStart else {
            throw GuestError("Unterminated dyld import name")
        }
        let ordinal = format == 3 ? Int(record & 0xffff) : Int(record & 0xff)
        let signedOrdinal = ordinal >= (format == 3 ? 0xfff1 : 0xf1)
            ? ordinal - (format == 3 ? 65536 : 256) : ordinal
        let addend = format == 3 ? Int64(bitPattern: try read(offset + 8, 8))
            : format == 2 ? Int64(Int32(bitPattern: UInt32(try read(offset + 4, 4)))) : 0
        imports.append(ChainedImport(name: String(decoding: payload[nameStart..<end], as: UTF8.self),
                                     ordinal: signedOrdinal,
                                     weak: record & (format == 3 ? 0x10000 : 0x100) != 0,
                                     addend: addend))
    }

    let segments = Int(try read(starts, 4))
    guard segments <= (payload.count - starts - 4) / 4 else {
        throw GuestError("Invalid dyld segment starts")
    }
    var bindings = [Int: ChainedImport]()
    var authentication = [Int: ChainedAuthentication]()
    var visited = Set<Int>()
    for index in 0..<segments {
        let delta = Int(try read(starts + 4 + index * 4, 4))
        if delta == 0 { continue }
        let segment = starts + delta
        let size = Int(try read(segment, 4))
        let pageSize = Int(try read(segment + 4, 2))
        let pointerFormat = try read(segment + 6, 2)
        let offset = try read(segment + 8, 8)
        let pages = Int(try read(segment + 20, 2))
        let arm64e = [1, 9, 12].contains(pointerFormat)
        guard arm64e || [2, 6].contains(pointerFormat) else {
            throw GuestError("Unsupported dyld pointer format \(pointerFormat)")
        }
        guard size >= 22, size <= payload.count - segment,
              pages <= (size - 22) / 2, [4096, 16384].contains(pageSize),
              offset <= UInt64(memory.count) else { throw GuestError("Invalid dyld page starts") }

        let stride = arm64e ? 8 : 4
        for page in 0..<pages {
            let first = Int(try read(segment + 22 + page * 2, 2))
            if first == 0xffff { continue }
            guard first & 0x8000 == 0 else { throw GuestError("Multiple starts per dyld fixup page unsupported") }
            let pageAddress = Int(offset) + page * pageSize
            let pageEnd = pageAddress + pageSize
            var address = pageAddress + first
            while true {
                guard address.isMultiple(of: stride), address <= pageEnd - 8,
                      address <= memory.count - 8,
                      mappedRegions.contains(where: { $0.contains(address) && address <= $0.upperBound - 8 }),
                      visited.insert(address).inserted else { throw GuestError("Invalid dyld fixup address") }
                let raw = try MachO.integer(memory, address, 8)
                let authenticated = arm64e && raw >> 63 != 0
                if authenticated {
                    let diversity = (raw >> 32) & 0xffff
                    let diversified = raw & (1 << 48) != 0
                    let modifier = diversified ? UInt64(placement + address) | (diversity << 48) : diversity
                    authentication[address] = ChainedAuthentication(key: (raw >> 49) & 3, modifier: modifier)
                }
                let bind = raw & (UInt64(1) << (arm64e ? 62 : 63)) != 0
                let next = Int((raw >> 51) & (arm64e ? 0x7ff : 0xfff))
                var replacement: UInt64 = 0
                if bind {
                    let ordinal = Int(raw & (arm64e ? (pointerFormat == 12 ? 0xffffff : 0xffff) : 0xfffff))
                    guard ordinal < imports.count else { throw GuestError("Invalid dyld bind ordinal") }
                    let entry = imports[ordinal]
                    let embedded = authenticated ? 0 : arm64e ? Int64((raw >> 32) & 0x7ffff) : Int64((raw >> 20) & 0xff)
                    guard embedded == 0, entry.addend == 0 else {
                        throw GuestError("Nonzero dyld bind addends unsupported")
                    }
                    bindings[address] = entry
                    // Leave unknown strong symbols as an invalid guest pointer.
                    replacement = entry.weak ? 0 : .max
                } else {
                    let lowBits = authenticated ? 32 : arm64e ? 43 : 36
                    let target = raw & ((UInt64(1) << lowBits) - 1)
                    let absolute = !authenticated && [1, 2].contains(pointerFormat)
                    guard !absolute || target >= imageBase else { throw GuestError("Invalid dyld rebase target") }
                    replacement = absolute ? target - imageBase : target
                    guard replacement < UInt64(memory.count) else { throw GuestError("Dyld rebase target outside guest image") }
                    replacement += UInt64(placement)
                    // Preserve tagged pointers for the guest instruction set to interpret.
                    if !authenticated { replacement |= ((raw >> lowBits) & 0xff) << 56 }
                }
                var encoded = replacement.littleEndian
                withUnsafeBytes(of: &encoded) { memory.replaceSubrange(address..<(address + 8), with: $0) }
                if next == 0 { break }
                guard next * stride >= 8 else { throw GuestError("Overlapping dyld fixups") }
                address += next * stride
            }
        }
    }
    return (bindings, authentication)
}
