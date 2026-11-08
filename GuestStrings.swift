import Foundation

/// Lowers bounded C string imports into the guest Metal kernel. `mapOffset`
/// maps one guest byte to its backing-store offset when a shared cache is active.
func guestStringCall(
    _ name: String,
    returnAddress: Int?,
    cached: Bool,
    mapOffset: (String, String, Bool) -> String
) -> Instruction? {
    guard ["_strncpy", "___strncpy_chk", "_strlcpy", "___strlcpy_chk", "_strcpy", "___strcpy_chk"].contains(name) else { return nil }

    let strncpy = name == "_strncpy" || name == "___strncpy_chk"
    let strlcpy = name == "_strlcpy" || name == "___strlcpy_chk"
    let fortified = name.hasSuffix("_chk")
    let body: String
    if strncpy {
        body = """
        {
        ulong copied = 0;
        for (; copied < x[2]; ++copied) {
            if (x[1] > ~0ul - copied || x[0] > ~0ul - copied) { x[33] = pc; x[34] = 2; return; }
            ulong so = \(cached ? mapOffset("x[1] + copied", "1ul", false) : "x[1] + copied");
            ulong doff = \(cached ? mapOffset("x[0] + copied", "1ul", true) : "x[0] + copied");
            if (\(cached ? "so == ~0ul || doff == ~0ul" : "x[1] + copied >= size || x[0] + copied >= size || !guest_write(x[0] + copied, 1ul, writable)")) { x[33] = pc; x[34] = 2; return; }
            uchar ch = memory[so]; memory[doff] = ch;
            if (ch == 0) { ++copied; break; }
        }
        for (; copied < x[2]; ++copied) {
            if (x[0] > ~0ul - copied) { x[33] = pc; x[34] = 2; return; }
            ulong doff = \(cached ? mapOffset("x[0] + copied", "1ul", true) : "x[0] + copied");
            if (\(cached ? "doff == ~0ul" : "x[0] + copied >= size || !guest_write(x[0] + copied, 1ul, writable)")) { x[33] = pc; x[34] = 2; return; }
            memory[doff] = 0;
        }
        }
        """
    } else if strlcpy {
        body = """
        {
        ulong sourceLength = 0;
        for (;;) {
            if (x[1] > ~0ul - sourceLength) { x[33] = pc; x[34] = 2; return; }
            ulong so = \(cached ? mapOffset("x[1] + sourceLength", "1ul", false) : "x[1] + sourceLength");
            if (\(cached ? "so == ~0ul" : "x[1] + sourceLength >= size")) { x[33] = pc; x[34] = 2; return; }
            if (memory[so] == 0) break;
            ++sourceLength;
        }
        ulong copied = min(sourceLength, x[2] == 0 ? 0ul : x[2] - 1ul);
        for (ulong i = 0; i < copied; ++i) {
            if (x[1] > ~0ul - i || x[0] > ~0ul - i) { x[33] = pc; x[34] = 2; return; }
            ulong so = \(cached ? mapOffset("x[1] + i", "1ul", false) : "x[1] + i");
            ulong doff = \(cached ? mapOffset("x[0] + i", "1ul", true) : "x[0] + i");
            if (\(cached ? "so == ~0ul || doff == ~0ul" : "x[1] + i >= size || x[0] + i >= size || !guest_write(x[0] + i, 1ul, writable)")) { x[33] = pc; x[34] = 2; return; }
            memory[doff] = memory[so];
        }
        if (x[2] != 0) {
            if (x[0] > ~0ul - copied) { x[33] = pc; x[34] = 2; return; }
            ulong doff = \(cached ? mapOffset("x[0] + copied", "1ul", true) : "x[0] + copied");
            if (\(cached ? "doff == ~0ul" : "x[0] + copied >= size || !guest_write(x[0] + copied, 1ul, writable)")) { x[33] = pc; x[34] = 2; return; }
            memory[doff] = 0;
        }
        x[0] = sourceLength;
        }
        """
    } else {
        body = """
        {
        ulong sourceLength = 0;
        for (;;) {
            if (x[1] > ~0ul - sourceLength) { x[33] = pc; x[34] = 2; return; }
            ulong so = \(cached ? mapOffset("x[1] + sourceLength", "1ul", false) : "x[1] + sourceLength");
            if (\(cached ? "so == ~0ul" : "x[1] + sourceLength >= size")) { x[33] = pc; x[34] = 2; return; }
            if (memory[so] == 0) break;
            ++sourceLength;
        }
        if (sourceLength == ~0ul || (\(fortified ? "sourceLength + 1ul > x[2] || " : "")x[0] > ~0ul - sourceLength)) { x[33] = pc; x[34] = 2; return; }
        for (ulong i = 0; i <= sourceLength; ++i) {
            if (x[1] > ~0ul - i || x[0] > ~0ul - i) { x[33] = pc; x[34] = 2; return; }
            ulong so = \(cached ? mapOffset("x[1] + i", "1ul", false) : "x[1] + i");
            ulong doff = \(cached ? mapOffset("x[0] + i", "1ul", true) : "x[0] + i");
            if (\(cached ? "so == ~0ul || doff == ~0ul" : "x[1] + i >= size || x[0] + i >= size || !guest_write(x[0] + i, 1ul, writable)")) { x[33] = pc; x[34] = 2; return; }
            memory[doff] = memory[so];
        }
        }
        """
    }
    let fortifyCheck: String
    if name == "___strcpy_chk" {
        fortifyCheck = ""
    } else {
        fortifyCheck = fortified ? "if (x[2] > x[3]) { x[33] = pc; x[34] = 2; return; }" : ""
    }
    let code = [
        returnAddress.map { "x[30] = \($0)ul;" } ?? "",
        fortifyCheck,
        body,
        "if (x[30] == ~0ul) { x[34] = 0; return; }",
        "if (x[30] >= \(guestCodeLimit)ul) { x[34] = 3; return; }",
        "pc = x[30]; break;",
    ].filter { !$0.isEmpty }
    return Instruction(code: code, next: returnAddress.map { [$0] } ?? [])
}
