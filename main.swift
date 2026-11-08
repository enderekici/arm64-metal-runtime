import Darwin
import Foundation
import Metal

struct Invocation: Codable {
    let words: [UInt32]
    let seed: UInt32
}

struct Execution: Codable, Equatable {
    let words: [UInt32]
    let result: UInt32
}

struct MetalGuest {
    let device: MTLDevice
    let queue: MTLCommandQueue
    let pipeline: MTLComputePipelineState

    init(source: String) throws {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            throw GuestError("Metal compute unavailable")
        }
        let library = try device.makeLibrary(source: source, options: nil)
        guard let function = library.makeFunction(name: "run_guest") else {
            throw GuestError("Generated kernel unavailable")
        }
        self.device = device
        self.queue = queue
        pipeline = try device.makeComputePipelineState(function: function)
    }

    func run(_ inputs: [Invocation], budget: UInt32) throws -> [Execution] {
        guard !inputs.isEmpty else { return [] }
        let stride = max(1, inputs.map { $0.words.count }.max()!)
        guard stride <= 65_536, inputs.count <= 65_536, stride * inputs.count <= 16_777_216 else {
            throw GuestError("Guest batch exceeds the runtime's buffer limits")
        }
        func buffer(_ data: [UInt32]) throws -> MTLBuffer {
            guard let result = data.withUnsafeBytes({
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
            }) else { throw GuestError("Metal buffer allocation failed") }
            return result
        }

        var words = [UInt32](repeating: 0, count: stride * inputs.count)
        for (index, input) in inputs.enumerated() {
            let start = index * stride
            words.replaceSubrange(start..<(start + input.words.count), with: input.words)
        }
        let memory = try buffer(words)
        let counts = try buffer(inputs.map { UInt32($0.words.count) })
        let seeds = try buffer(inputs.map(\.seed))
        let output = try buffer([UInt32](repeating: 0, count: inputs.count))
        let status = try buffer([UInt32](repeating: .max, count: inputs.count))
        guard let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else {
            throw GuestError("Metal command allocation failed")
        }

        encoder.setComputePipelineState(pipeline)
        for (index, value) in [memory, counts, seeds, output, status].enumerated() {
            encoder.setBuffer(value, offset: 0, index: index)
        }
        var strideArgument = UInt32(stride)
        var budgetArgument = budget
        encoder.setBytes(&strideArgument, length: 4, index: 5)
        encoder.setBytes(&budgetArgument, length: 4, index: 6)
        encoder.dispatchThreads(MTLSize(width: inputs.count, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: pipeline.threadExecutionWidth, height: 1, depth: 1))
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        guard command.status == .completed else {
            throw command.error ?? GuestError("Metal execution did not complete")
        }

        let statuses = status.contents().assumingMemoryBound(to: UInt32.self)
        let results = output.contents().assumingMemoryBound(to: UInt32.self)
        let data = memory.contents().assumingMemoryBound(to: UInt32.self)
        return try inputs.enumerated().map { index, input in
            guard statuses[index] == 0 else {
                let reasons = [1: "block budget exhausted", 2: "guest memory out of bounds", 3: "invalid guest PC"]
                let reason = reasons[Int(statuses[index])] ?? "unknown guest status"
                throw GuestError("Invocation \(index): \(reason)")
            }
            let start = data + index * stride
            guard UnsafeBufferPointer(start: start + input.words.count, count: stride - input.words.count)
                .allSatisfy({ $0 == 0 }) else { throw GuestError("Invocation \(index): modified padding") }
            return Execution(words: Array(UnsafeBufferPointer(start: start, count: input.words.count)),
                             result: results[index])
        }
    }
}

func verify(_ guest: MetalGuest, binary: String, symbol: String) throws {
    guard let handle = dlopen(binary, RTLD_NOW | RTLD_LOCAL) else {
        throw GuestError(String(cString: dlerror()))
    }
    defer { dlclose(handle) }
    guard let address = dlsym(handle, symbol) else { throw GuestError("Native symbol unavailable: \(symbol)") }
    typealias Native = @convention(c) (UnsafeMutablePointer<UInt32>?, UInt64, UInt32) -> UInt32
    let native = unsafeBitCast(address, to: Native.self)

    var state: UInt32 = 0x136cf03d
    func next() -> UInt32 {
        state ^= state << 13
        state ^= state >> 17
        state ^= state << 5
        return state
    }
    let inputs = (0..<1024).map { index in
        Invocation(words: (0..<(index % 65)).map { _ in next() }, seed: next())
    }
    let actual = try guest.run(inputs, budget: 66)
    let expected = inputs.map { input in
        var words = input.words
        let result = words.withUnsafeMutableBufferPointer {
            native($0.baseAddress, UInt64($0.count), input.seed)
        }
        return Execution(words: words, result: result)
    }
    guard actual == expected else {
        let mismatch = actual.indices.first { actual[$0] != expected[$0] }
        throw GuestError("GPU output differs from native output: \(mismatch ?? -1)")
    }
    do {
        _ = try guest.run([inputs[0]], budget: 0)
        throw GuestError("Guest accepted a zero block budget")
    } catch let error as GuestError where error.message.hasSuffix("block budget exhausted") {
        // Expected guest exit.
    }
    print("\(symbol) on \(guest.device.name): \(actual.count) exact native/GPU matches; block budget enforced")
}

func main() throws {
    let args = Array(CommandLine.arguments.dropFirst())
    if args == ["--help"] || args.isEmpty {
        print("""
        Usage:
          arm64-metal launch EXECUTABLE [ARG ...]

          arm64-metal run BINARY SYMBOL [INPUT.json|-] [BLOCK_BUDGET]
          arm64-metal verify TRUSTED_DYLIB SYMBOL
          arm64-metal translate BINARY SYMBOL
          arm64-metal inspect-cache INSTALL_NAME SYMBOL
          arm64-metal inspect-cache-rebases INSTALL_NAME

        Input is a JSON array of {"words":[1,2,3],"seed":7} invocations.
        Run reads stdin by default. Verify loads and runs the trusted dylib natively.
        """)
        return
    }
    if args.first == "launch", args.count >= 2 {
        exit(try launchExecutable(path: args[1], arguments: Array(args.dropFirst(2)), environment: captureEnvironment()))
    }
    if args.first == "inspect-cache", args.count == 3 {
        let symbol = try SharedCache().export(in: args[1], named: args[2])
        print(String(format: "%@ offset 0x%llx first word 0x%08x", args[2], UInt64(symbol.offset), symbol.firstWord))
        return
    }
    if args.first == "inspect-cache-rebases", args.count == 2 {
        for rebase in try SharedCache().rebases(in: args[1]) {
            let details: String
            if let auth = rebase.authentication {
                details = "key=\(auth.dataKey ? "DA" : "IA") diversity=\(auth.diversity) address-diversity=\(auth.addressDiversity)"
            } else {
                details = "high-byte=\(rebase.highByte)"
            }
            print("0x\(String(rebase.location, radix: 16)) 0x\(String(rebase.target, radix: 16)) \(details)")
        }
        return
    }

    guard args.count >= 3 else { throw GuestError("Missing command, binary, or symbol. See --help.") }
    let binary = URL(fileURLWithPath: args[1])
    let symbol = args[2]
    switch args[0] {
    case "translate" where args.count == 3:
        print(try translate(binary: binary, symbol: symbol))
    case "verify" where args.count == 3:
        try verify(MetalGuest(source: translate(binary: binary, symbol: symbol)),
                   binary: binary.path, symbol: symbol)
    case "run" where args.count <= 5:
        let path = args.count >= 4 ? args[3] : "-"
        let budget = args.count == 5 ? UInt32(args[4]) : 65_536
        guard let budget, budget > 0, budget <= 1_000_000 else { throw GuestError("Invalid block budget") }
        let data = path == "-" ? FileHandle.standardInput.readDataToEndOfFile() : try Data(contentsOf: URL(fileURLWithPath: path))
        let inputs = try JSONDecoder().decode([Invocation].self, from: data)
        let guest = try MetalGuest(source: translate(binary: binary, symbol: symbol))
        let results = try guest.run(inputs, budget: budget)
        let output = JSONEncoder()
        output.outputFormatting = [.sortedKeys]
        FileHandle.standardOutput.write(try output.encode(results))
        FileHandle.standardOutput.write(Data([10]))
    default:
        throw GuestError("Unknown command or wrong arguments. See --help.")
    }
}

do {
    try main()
} catch {
    FileHandle.standardError.write(Data("arm64-metal: \(error.localizedDescription)\n".utf8))
    exit(1)
}
