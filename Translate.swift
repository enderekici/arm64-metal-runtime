import Foundation

struct GuestError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

struct MachO {
    let bytes: Data

    private static func supportedARM64Subtype(_ subtype: UInt64, allowARM64e: Bool) -> Bool {
        switch subtype & 0x00ff_ffff {
        case 0, 1: return true
        case 2: return allowARM64e
        default: return false
        }
    }

    init(_ url: URL, allowARM64e: Bool = false) throws {
        let file = try Data(contentsOf: url, options: .mappedIfSafe)
        func read(_ offset: Int, _ width: Int, big: Bool = false) throws -> UInt64 {
            try Self.integer(file, offset, width, big: big)
        }
        let magic = try read(0, 4, big: true)
        if magic == 0xcafebabe || magic == 0xcafebabf {
            let wide = magic == 0xcafebabf
            let size = wide ? 32 : 20
            let count = Int(try read(4, 4, big: true))
            guard count <= (file.count - 8) / size else { throw GuestError("Invalid universal header") }
            var slice: Data?
            for index in 0..<count {
                let start = 8 + index * size
                guard try read(start, 4, big: true) == 0x0100000c else { continue }
                let subtype = try read(start + 4, 4, big: true)
                guard Self.supportedARM64Subtype(subtype, allowARM64e: allowARM64e) else { continue }
                let arm64e = subtype & 0x00ff_ffff == 2
                if arm64e, slice != nil { continue }
                let width = wide ? 8 : 4
                let offset = try read(start + 8, width, big: true)
                let length = try read(start + 8 + width, width, big: true)
                guard offset <= UInt64(file.count), length <= UInt64(file.count) - offset else {
                    throw GuestError("Invalid ARM64 slice bounds")
                }
                slice = file.subdata(in: Int(offset)..<Int(offset + length))
                if !arm64e { break }
            }
            guard let slice else { throw GuestError("No supported ARM64 slice") }
            bytes = slice
        } else {
            bytes = file
        }
        guard try word(0) == 0xfeedfacf, try word(4) == 0x0100000c,
              Self.supportedARM64Subtype(UInt64(try word(8)), allowARM64e: allowARM64e) else {
            throw GuestError("Expected a little-endian ARM64 Mach-O")
        }
    }

    static func integer(_ data: Data, _ offset: Int, _ width: Int, big: Bool = false) throws -> UInt64 {
        guard offset >= 0, offset <= data.count, width <= data.count - offset else {
            throw GuestError("Truncated Mach-O")
        }
        var result: UInt64 = 0
        for index in 0..<width {
            result |= UInt64(data[offset + index]) << ((big ? width - 1 - index : index) * 8)
        }
        return result
    }

    func word(_ offset: Int) throws -> UInt32 { UInt32(try Self.integer(bytes, offset, 4)) }

    func symbol(_ name: String) throws -> (entry: Int, limit: Int, textOffset: Int) {
        let commands = Int(try word(16))
        let commandEnd = 32 + Int(try word(20))
        guard commandEnd <= bytes.count, commands <= (commandEnd - 32) / 8 else {
            throw GuestError("Invalid load commands")
        }
        var offset = 32
        var sectionIndex = 0
        var text: (index: Int, address: UInt64, size: UInt64, offset: Int)?
        var table: (offset: Int, count: Int, strings: Int, size: Int)?
        for _ in 0..<commands {
            guard offset <= commandEnd - 8 else { throw GuestError("Truncated load command") }
            let command = try word(offset)
            let length = Int(try word(offset + 4))
            guard length >= 8, length <= commandEnd - offset else { throw GuestError("Invalid load command") }
            if command == 0x19 {
                guard length >= 72 else { throw GuestError("Truncated segment") }
                let count = Int(try word(offset + 64))
                guard count <= (length - 72) / 80 else { throw GuestError("Truncated section table") }
                for index in 0..<count {
                    let start = offset + 72 + index * 80
                    sectionIndex += 1
                    let section = bytes[start..<(start + 16)].prefix { $0 != 0 }
                    let segment = bytes[(start + 16)..<(start + 32)].prefix { $0 != 0 }
                    if String(decoding: section, as: UTF8.self) == "__text",
                       String(decoding: segment, as: UTF8.self) == "__TEXT" {
                        text = (sectionIndex, try Self.integer(bytes, start + 32, 8),
                                try Self.integer(bytes, start + 40, 8), Int(try word(start + 48)))
                    }
                }
            } else if command == 2 {
                guard length >= 24 else { throw GuestError("Truncated symbol command") }
                table = (Int(try word(offset + 8)), Int(try word(offset + 12)),
                         Int(try word(offset + 16)), Int(try word(offset + 20)))
            }
            offset += length
        }
        guard let text, let table, text.offset <= bytes.count,
              text.size <= UInt64(bytes.count - text.offset), text.size <= UInt32.max,
              table.offset <= bytes.count, table.count <= (bytes.count - table.offset) / 16,
              table.strings <= bytes.count, table.size <= bytes.count - table.strings else {
            throw GuestError("Missing or invalid __text and symbol tables")
        }

        let symbolName = Array(("_" + name).utf8)
        var entry: Int?
        var symbolOffsets = [Int]()
        for index in 0..<table.count {
            let start = table.offset + index * 16
            let type = bytes[start + 4]
            guard type & 0xee == 0x0e, Int(bytes[start + 5]) == text.index else { continue }
            let address = try Self.integer(bytes, start + 8, 8)
            guard address >= text.address, address - text.address < text.size else { continue }
            let relative = Int(address - text.address)
            symbolOffsets.append(relative)
            let nameOffset = Int(try word(start))
            guard nameOffset < table.size, symbolName.count < table.size - nameOffset else { continue }
            let first = table.strings + nameOffset
            let last = first + symbolName.count
            if bytes[last] == 0, bytes[first..<last].elementsEqual(symbolName) { entry = relative }
        }
        guard let entry, entry.isMultiple(of: 4) else { throw GuestError("ARM64 text symbol not found: \(name)") }
        let limit = symbolOffsets.filter { $0 > entry }.min() ?? Int(text.size)
        return (entry, limit, text.offset)
    }
}

struct Instruction {
    let code: [String]
    let next: [Int]

    func endsBlock(at pc: Int) -> Bool {
        next != [pc + 4] || code.last?.hasSuffix("break;") == true ||
            code.last?.hasSuffix("return;") == true
    }
}

func decode(_ bits: UInt32, at pc: Int, addressSuffix: String = "u") throws -> Instruction {
    func field(_ start: Int, _ count: Int) -> UInt32 { (bits >> start) & ((1 << count) - 1) }
    func signed(_ value: UInt32, _ width: Int) -> Int {
        let sign = 1 << (width - 1)
        return (Int(value) ^ sign) - sign
    }
    func reg(_ index: UInt32, _ wide: Bool) -> String {
        if index == 31 { return wide ? "0ul" : "0u" }
        return wide ? "x[\(index)]" : "uint(x[\(index)])"
    }
    let rd = field(0, 5), rn = field(5, 5), rm = field(16, 5)
    let wide = field(31, 1) == 1
    let cast = wide ? "ulong" : "uint"
    let following = pc + 4
    func assign(_ value: String) -> String {
        rd == 31 ? "" : "x[\(rd)] = \(cast)(\(value));"
    }
    func advance(_ code: String...) -> Instruction { Instruction(code: code, next: [following]) }
    func branch(_ condition: String, _ target: Int) -> Instruction {
        Instruction(code: ["pc = \(condition) ? \(target)\(addressSuffix) : \(following)\(addressSuffix); break;"],
                    next: [target, following])
    }

    if bits == 0xd65f03c0 {
        return Instruction(code: ["returns[id] = uint(x[0]); status[id] = 0; return;"], next: [])
    }
    if bits & 0x7e000000 == 0x34000000 {
        let op = field(24, 1) == 0 ? "==" : "!="
        return branch("\(reg(rd, wide)) \(op) 0", pc + signed(field(5, 19), 19) * 4)
    }
    if bits & 0xff00001e == 0x54000000 {
        return branch(field(0, 1) == 0 ? "z" : "!z", pc + signed(field(5, 19), 19) * 4)
    }
    if bits & 0xfc000000 == 0x14000000 {
        let target = pc + signed(field(0, 26), 26) * 4
        return Instruction(code: ["pc = \(target)\(addressSuffix); break;"], next: [target])
    }
    if bits & 0x7f200000 == 0x0b000000 || bits & 0x7f200000 == 0x4a000000 {
        let shift = field(10, 6)
        if field(22, 2) == 0, shift < (wide ? 64 : 32) {
            let op = bits & 0x7f000000 == 0x0b000000 ? "+" : "^"
            return advance(assign("\(reg(rn, wide)) \(op) (\(reg(rm, wide)) << \(shift))"))
        }
    }
    if bits & 0x7f800000 == 0x71000000, rn != 31 {
        let immediate = field(10, 12) << (field(22, 1) * 12)
        return advance("z = (\(reg(rn, wide)) == \(cast)(\(immediate)));",
                       assign("\(reg(rn, wide)) - \(cast)(\(immediate))"))
    }
    if bits & 0x7fe0ffe0 == 0x2a0003e0 { return advance(assign(reg(rm, wide))) }
    if bits & 0xffc0fc00 == 0x53007c00, field(16, 6) < 32 {
        return advance(assign("\(reg(rn, false)) >> \(field(16, 6))"))
    }
    // ANDS to WZR with the single-bit immediate 1.
    if bits & 0xfffffc1f == 0x7200001f {
        return advance("z = ((\(reg(rn, false)) & 1u) == 0);")
    }
    if bits & 0x7fe0ec00 == 0x1a800400 {
        let condition = field(12, 1) == 0 ? "z" : "!z"
        return advance(assign("\(condition) ? \(reg(rn, wide)) : \(reg(rm, wide)) + \(cast)(1)"))
    }
    let wordMemory = bits & 0xffc00000
    let postMemory = bits & 0xffe00c00
    if rn != 31, wordMemory == 0xb9400000 || rn != 31 && wordMemory == 0xb9000000 ||
        rn != 31 && (postMemory == 0xb8400400 || postMemory == 0xb8000400) && rn != rd {
        let post = postMemory == 0xb8400400 || postMemory == 0xb8000400
        let load = wordMemory == 0xb9400000 || postMemory == 0xb8400400
        let displacement = post ? 0 : field(10, 12) * 4
        let base = reg(rn, true)
        let address = "(\(base) + \(displacement)ul)"
        let index = "(\(address) >> 2)"
        let bounds = "if ((\(address) & 3ul) || \(index) >= count) { status[id] = 2; return; }"
        let access = load ? assign("words[\(index)]") : "words[\(index)] = \(reg(rd, false));"
        if post { return advance(bounds, access, "\(base) += ulong(\(signed(field(12, 9), 9)));") }
        return advance(bounds, access)
    }
    throw GuestError(String(format: "Unsupported ARM64 instruction 0x%08x at __text+0x%x", bits, pc))
}

func translate(binary: URL, symbol: String) throws -> String {
    let image = try MachO(binary)
    let region = try image.symbol(symbol)
    var instructions = [Int: Instruction]()
    var pending = [region.entry]
    while let pc = pending.popLast() {
        if instructions[pc] != nil { continue }
        guard pc >= region.entry, pc <= region.limit - 4, pc.isMultiple(of: 4) else {
            throw GuestError("Control flow leaves supported function extent")
        }
        let instruction = try decode(image.word(region.textOffset + pc), at: pc)
        instructions[pc] = instruction
        pending.append(contentsOf: instruction.next)
    }
    let cases = renderBlocks(instructions, entry: region.entry)

    return """
    #include <metal_stdlib>
    using namespace metal;
    kernel void run_guest(device uint *memory [[buffer(0)]],
                          device const uint *counts [[buffer(1)]],
                          device const uint *seeds [[buffer(2)]],
                          device uint *returns [[buffer(3)]],
                          device uint *status [[buffer(4)]],
                          constant uint &stride [[buffer(5)]],
                          constant uint &max_blocks [[buffer(6)]],
                          uint id [[thread_position_in_grid]]) {
        device uint *words = memory + ulong(id) * stride;
        uint count = counts[id];
        if (count > stride) { status[id] = 2; return; }
        ulong x[31] = {};
        x[1] = count;
        x[2] = seeds[id];
        bool z = false;
        uint pc = \(region.entry)u;
        for (uint step = 0; step < max_blocks; ++step) {
            switch (pc) {
    \(cases)
            default: status[id] = 3; return;
            }
        }
        status[id] = 1;
    }
    """
}

func blockEntries(_ instructions: [Int: Instruction], entry: Int) -> Set<Int> {
    Set([entry] + instructions.flatMap { pc, instruction in
        instruction.endsBlock(at: pc) ? instruction.next : []
    }).filter { instructions[$0] != nil }
}

func renderBlocks(_ instructions: [Int: Instruction], entry: Int, addressSuffix: String = "u") -> String {
    let leaders = blockEntries(instructions, entry: entry)
    return leaders.sorted().map { entry in
        var lines = ["case \(entry)\(addressSuffix):"]
        var pc = entry
        while let instruction = instructions[pc] {
            lines += instruction.code.filter { !$0.isEmpty }.map { "    " + $0 }
            if instruction.endsBlock(at: pc) { break }
            pc += 4
            if leaders.contains(pc) || instructions[pc] == nil {
                lines.append("    pc = \(pc)\(addressSuffix); break;")
                break
            }
        }
        return lines.joined(separator: "\n")
    }.joined(separator: "\n")
}
