struct GuestHeap {
    private var freeBlocks: [Range<Int>]
    private var allocations = [Int: Int]()

    init(_ range: Range<Int>) { freeBlocks = [range] }

    mutating func extend(_ range: Range<Int>) {
        if let last = freeBlocks.last, last.upperBound == range.lowerBound {
            freeBlocks[freeBlocks.count - 1] = last.lowerBound..<range.upperBound
        } else {
            freeBlocks.append(range)
        }
    }

    mutating func allocate(_ size: UInt64, alignment: Int = 16) -> Int? {
        guard let count = Int(exactly: max(1, size)), count <= Int.max - 15 else { return nil }
        let aligned = (count + 15) & ~15
        for (index, block) in freeBlocks.enumerated() {
            let padding = (0 &- block.lowerBound) & (alignment - 1)
            guard aligned <= block.count, padding <= block.count - aligned else { continue }
            let address = block.lowerBound + padding
            let end = address + aligned
            var leftovers = [Range<Int>]()
            if block.lowerBound < address { leftovers.append(block.lowerBound..<address) }
            if end < block.upperBound { leftovers.append(end..<block.upperBound) }
            freeBlocks.replaceSubrange(index...index, with: leftovers)
            allocations[address] = aligned
            return address
        }
        return nil
    }

    func size(of address: Int) -> Int? { allocations[address] }

    mutating func release(_ address: Int) throws {
        guard address != 0 else { return }
        guard let count = allocations.removeValue(forKey: address) else {
            throw GuestError("Invalid guest heap pointer")
        }
        let index = freeBlocks.firstIndex { $0.lowerBound > address } ?? freeBlocks.count
        freeBlocks.insert(address..<(address + count), at: index)
        if index + 1 < freeBlocks.count, freeBlocks[index].upperBound == freeBlocks[index + 1].lowerBound {
            freeBlocks[index] = freeBlocks[index].lowerBound..<freeBlocks[index + 1].upperBound
            freeBlocks.remove(at: index + 1)
        }
        if index > 0, freeBlocks[index - 1].upperBound == freeBlocks[index].lowerBound {
            freeBlocks[index - 1] = freeBlocks[index - 1].lowerBound..<freeBlocks[index].upperBound
            freeBlocks.remove(at: index)
        }
    }
}
