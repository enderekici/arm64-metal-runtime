import Foundation

// Darwin ARM64 passes unnamed integer and pointer arguments in stack slots.
// Only format conversions that can be marshaled without host pointers are enabled.
func formatGuestOutput(_ format: [UInt8], argument: () throws -> UInt64,
                       string: (UInt64) throws -> [UInt8]) throws -> [UInt8] {
    var output = [UInt8]()
    var index = 0
    while index < format.count {
        let byte = format[index]
        index += 1
        if byte != 37 {
            output.append(byte)
            continue
        }
        var wide = false
        if index < format.count, [106, 108, 116, 122].contains(format[index]) {
            wide = true
            let modifier = format[index]
            index += 1
            if modifier == 108, index < format.count, format[index] == 108 { index += 1 }
        }
        guard index < format.count else { throw GuestError("Incomplete guest printf conversion") }
        let conversion = format[index]
        index += 1
        if conversion == 37, !wide {
            output.append(37)
            continue
        }
        guard [100, 105, 117, 120, 88, 111, 99, 115].contains(conversion),
              !wide || conversion != 99 && conversion != 115 else {
            throw GuestError("Unsupported guest printf conversion")
        }
        let raw = try argument()
        let number = wide ? raw : UInt64(UInt32(truncatingIfNeeded: raw))
        let text: String
        switch conversion {
        case 100, 105:
            text = wide ? String(Int64(bitPattern: raw)) : String(Int32(bitPattern: UInt32(number)))
        case 117:
            text = String(number)
        case 120, 88:
            text = String(number, radix: 16, uppercase: conversion == 88)
        case 111:
            text = String(number, radix: 8)
        case 99:
            output.append(UInt8(truncatingIfNeeded: raw))
            continue
        default:
            output.append(contentsOf: try string(raw))
            continue
        }
        output.append(contentsOf: text.utf8)
    }
    return output
}
