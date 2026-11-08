import Darwin

struct GuestSignals {
    struct Action {
        var handler: UInt64 = 0
        var mask: UInt32 = 0
        var flags: Int32 = 0
    }

    private var actions = [Int32: Action]()
    private var pending: UInt32 = 0
    var mask: UInt32 = 0 {
        didSet { mask &= ~(Self.bit(SIGKILL) | Self.bit(SIGSTOP)) }
    }

    static func bit(_ signal: Int32) -> UInt32 { 1 << (signal - 1) }
    static func valid(_ signal: Int32) -> Bool { (1...31).contains(signal) }

    func action(_ signal: Int32) -> Action { actions[signal] ?? Action() }

    mutating func setAction(_ action: Action, for signal: Int32) {
        actions[signal] = action
        if action.handler == 1 { pending &= ~Self.bit(signal) }
    }

    mutating func setMask(_ how: Int32, _ value: UInt32) -> Bool {
        switch how {
        case SIG_BLOCK: mask |= value
        case SIG_UNBLOCK: mask &= ~value
        case SIG_SETMASK: mask = value
        default: return false
        }
        return true
    }

    mutating func raise(_ signal: Int32) {
        if action(signal).handler != 1 { pending |= Self.bit(signal) }
    }

    mutating func ingest(_ incoming: UInt32) {
        var remaining = incoming
        while remaining != 0 {
            let signal = Int32(remaining.trailingZeroBitCount + 1)
            remaining &= remaining - 1
            if Self.valid(signal) { raise(signal) }
        }
    }

    mutating func next(threadPending: inout UInt32) -> (Int32, Action)? {
        let ready = (pending | threadPending) & ~mask
        guard ready != 0 else { return nil }
        let signal = Int32(ready.trailingZeroBitCount + 1)
        let bit = Self.bit(signal)
        if threadPending & bit != 0 { threadPending &= ~bit }
        else { pending &= ~bit }
        let action = action(signal)
        if action.flags & SA_RESETHAND != 0 { actions[signal] = Action() }
        return (signal, action)
    }

    mutating func enter(_ signal: Int32, action: Action) {
        mask |= action.mask
        if action.flags & SA_NODEFER == 0 { mask |= Self.bit(signal) }
    }
}
