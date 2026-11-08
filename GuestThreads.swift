import Darwin

let guestTSDSize = 1024 * MemoryLayout<UInt64>.stride
let guestMachThreadSlotOffset = 3 * MemoryLayout<UInt64>.stride

func guestMachThreadName(_ id: UInt64) -> UInt64 {
    // Keep guest thread names separate from the task port and set the low
    // bit that Darwin's unfair-lock implementation uses for no waiters.
    0x10003 + (id << 8)
}

final class GuestThread {
    enum Wait {
        case join(UInt64, output: UInt64)
        case mutex(UInt64)
        case condition(UInt64, mutex: UInt64, deadline: UInt64?)
        case sleep(deadline: UInt64, remainder: UInt64?)
    }

    let id: UInt64
    let stack: Range<Int>
    let allocation: Int?
    let errnoAddress: Int
    let tsdAddress: Int
    var registers: [UInt64]
    var signalMask: UInt32
    var pendingSignals: UInt32 = 0
    var wait: Wait?
    var wakeError: Int32 = 0
    var result: UInt64?

    init(id: UInt64, stack: Range<Int>, allocation: Int?, errnoAddress: Int,
         tsdAddress: Int, registers: [UInt64], signalMask: UInt32) {
        self.id = id
        self.stack = stack
        self.allocation = allocation
        self.errnoAddress = errnoAddress
        self.tsdAddress = tsdAddress
        self.registers = registers
        self.signalMask = signalMask
    }
}

struct GuestMutex {
    let kind: Int32
    var owner: UInt64?
    var depth = 0
    var waiters = [UInt64]()

    enum Lock {
        case acquired
        case wait
        case error(Int32)
    }

    mutating func lock(_ thread: UInt64, tryOnly: Bool) -> Lock {
        if owner == nil {
            owner = thread
            depth = 1
            return .acquired
        }
        if owner == thread, kind == PTHREAD_MUTEX_RECURSIVE {
            guard depth < Int32.max else { return .error(EAGAIN) }
            depth += 1
            return .acquired
        }
        if tryOnly { return .error(EBUSY) }
        if owner == thread, kind == PTHREAD_MUTEX_ERRORCHECK { return .error(EDEADLK) }
        waiters.append(thread)
        return .wait
    }

    mutating func unlock(_ thread: UInt64) -> (Int32, UInt64?) {
        guard owner == thread else { return (EPERM, nil) }
        depth -= 1
        if depth != 0 { return (0, nil) }
        owner = waiters.isEmpty ? nil : waiters.removeFirst()
        depth = owner == nil ? 0 : 1
        return (0, owner)
    }
}
