import Foundation

enum MachExport {
    case address(Int)
    case reexport(ordinal: Int, name: String)
}

func machExports(_ trie: Data, memorySize: Int) throws -> [String: MachExport] {
    func readULEB(_ cursor: inout Int, end: Int) throws -> UInt64 {
        var result: UInt64 = 0
        for shift in stride(from: 0, through: 63, by: 7) {
            guard cursor < end else { throw GuestError("Truncated Mach-O export ULEB") }
            let byte = trie[cursor]
            cursor += 1
            guard shift < 63 || byte <= 1 else { throw GuestError("Overflowing Mach-O export ULEB") }
            result |= UInt64(byte & 127) << shift
            if byte & 128 == 0 { return result }
        }
        throw GuestError("Unterminated Mach-O export ULEB")
    }

    var exports = [String: MachExport]()
    var pending: [(offset: Int, prefix: [UInt8])] = [(0, [])]
    var visited = Set<Int>()
    while let node = pending.popLast() {
        guard node.offset < trie.count, visited.insert(node.offset).inserted,
              visited.count <= 100_000 else { throw GuestError("Invalid or excessive Mach-O export nodes") }
        var cursor = node.offset
        let terminal = try readULEB(&cursor, end: trie.count)
        guard terminal < UInt64(trie.count - cursor) else { throw GuestError("Invalid Mach-O export terminal") }
        let terminalEnd = cursor + Int(terminal)
        if terminal != 0 {
            let flags = try readULEB(&cursor, end: terminalEnd)
            let name = String(decoding: node.prefix, as: UTF8.self)
            if flags & 8 != 0 {
                let ordinal = try readULEB(&cursor, end: terminalEnd)
                guard !name.isEmpty, ordinal > 0, let dependency = Int(exactly: ordinal),
                      let end = trie[cursor..<terminalEnd].firstIndex(of: 0), end == terminalEnd - 1 else {
                    throw GuestError("Invalid Mach-O re-export")
                }
                let imported = cursor == end ? name : String(decoding: trie[cursor..<end], as: UTF8.self)
                exports[name] = .reexport(ordinal: dependency, name: imported)
            } else if flags & 0x1b == 0 {
                let address = try readULEB(&cursor, end: terminalEnd)
                guard address < UInt64(memorySize), !node.prefix.isEmpty else {
                    throw GuestError("Mach-O export outside guest image")
                }
                exports[name] = .address(Int(address))
            }
        }
        cursor = terminalEnd
        let children = Int(trie[cursor])
        cursor += 1
        for _ in 0..<children {
            guard cursor < trie.count, let end = trie[cursor...].firstIndex(of: 0),
                  end > cursor, node.prefix.count + end - cursor <= 4096 else {
                throw GuestError("Invalid Mach-O export edge")
            }
            let name = node.prefix + trie[cursor..<end]
            cursor = end + 1
            let offset = try readULEB(&cursor, end: trie.count)
            guard offset < UInt64(trie.count) else { throw GuestError("Invalid Mach-O export child") }
            pending.append((Int(offset), name))
        }
    }
    return exports
}
