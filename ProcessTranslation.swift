import Foundation

let hostServiceIDs = [
    "_puts": 1, "_write": 2, "__exit": 3, "_exit": 3, "_getenv": 4, "_strlen": 5,
    "_putchar": 6, "_putwchar": 7, "_mbtowc": 8, "_fflush": 9,
    "_getopt": 10, "_memcmp": 11, "_getcwd": 12, "_stat": 13, "_fwrite": 14,
    "_dirname": 15, "_basename": 16, "_ferror": 17, "_setlocale": 18, "_strcmp": 19, "_mbrlen": 20,
    "_open": 21, "_close": 22, "_read": 23, "_fstat": 24, "_fileno": 25,
    "_malloc_type_malloc": 26, "_malloc": 26, "_sysconf": 27, "___error": 28,
    "_strrchr": 29, "_getprogname": 30, "_fclose": 31, "___chkstk_darwin": 32, "_printf": 33, "_fcntl": 34, "_isatty": 35, "_getuid": 36, "_geteuid": 37, "_getgid": 38, "_getegid": 39,
    "_calloc": 40, "_malloc_type_calloc": 40, "_getrlimit": 41, "_pipe": 42,
    "_sigaction": 43, "_sigprocmask": 44, "_pthread_sigmask": 45,
    "_raise": 46, "_kill": 47, "_getpid": 48, "_sysctlbyname": 49, "_sysctl": 50, "_strncmp": 51, "_nanosleep": 52, "_strstr": 53, "_getopt_long": 54, "_fprintf": 55, "_free": 56, "_realloc": 57, "_mach_timebase_info": 58, "_mach_absolute_time": 59, "_clock": 60, "_clock_gettime": 61, "_fopen": 62, "_fread": 63, "_feof": 64,
    "_pthread_mutex_init": 65, "_pthread_mutex_destroy": 66, "_pthread_mutex_lock": 67,
    "_pthread_mutex_trylock": 68, "_pthread_mutex_unlock": 69, "_pthread_create": 70,
    "_pthread_join": 71, "_pthread_exit": 72, "_pthread_self": 73, "_pthread_equal": 74,
    "_sched_yield": 75, "_pthread_mutexattr_init": 76, "_pthread_mutexattr_destroy": 77,
    "_pthread_mutexattr_settype": 78, "_pthread_cond_init": 79, "_pthread_cond_destroy": 80,
    "_pthread_cond_wait": 81, "_pthread_cond_signal": 82, "_pthread_cond_broadcast": 83, "_pthread_cond_timedwait": 84, "_pthread_cond_timedwait_relative_np": 85,
    "_lseek": 86, "_mbrtowc": 87, "___maskrune": 88, "_vfprintf": 89, "_fputc": 90, "_signal": 91,
    "_fcopyfile": 92, "_snprintf": 93,
    "___snprintf_chk": 94, "_fgetattrlist": 95, "_fsetattrlist": 96, "_vwarnx": 97,
    "_fgetc": 98, "_ungetc": 99,
    "_posix_memalign": 100, "_malloc_type_posix_memalign": 100,
    "_issetugid": 101,
    "_dyld_program_sdk_at_least": 102, "_dyld_program_minos_at_least": 103,
    "_shm_open": 104, "___shm_open": 105, "_shm_unlink": 106,
    "_mmap": 107, "___mmap": 107, "_munmap": 108, "_ftruncate": 109,
    "__NSGetEnviron": 110,
    "__dyld_get_objc_selector": 111,
    "_mach_vm_allocate": 112, "_vm_allocate": 112,
    "_mach_vm_deallocate": 113, "_vm_deallocate": 113, "_mach_vm_map": 114,
    "__dyld_get_image_slide": 115,
    "_mprotect": 116,
    "_NSVersionOfLinkTimeLibrary": 117,
    "_gettimeofday": 118, "___commpage_gettimeofday": 119,
    "_getentropy": 120, "__dyld_is_memory_immutable": 121,
    "__dyld_get_lib_msg_send_offsets": 122,
    "__dyld_register_func_for_add_image": 123,
    "__dyld_lookup_section_info": 124,
    "__dyld_objc_register_callbacks": 125,
    "__dyld_get_shared_cache_range": 126,
    "__dyld_for_objc_header_opt_ro": 127,
    "__dyld_for_objc_header_opt_rw": 128,
    "__dyld_get_prog_image_header": 129, "__NSGetMachExecuteHeader": 129,
    "_dyld_get_active_platform": 130,
    "__guest_objc_make_image_mutable": 131,
    "_dyld_shared_cache_some_image_overridden": 132,
    "__dyld_for_each_objc_class": 133,
    "__dyld_objc_class_count": 134,
    "_dlopen": 135, "_dlclose": 136,
    "_mach_continuous_time": 137,
    "__os_feature_enabled_impl": 138,
    "__NSGetArgc": 139, "__NSGetArgv": 140,
    "_dyld_process_is_restricted": 141,
    "__NSGetExecutablePath": 142,
    "__dyld_get_swift_prespecialized_data": 143,
    "__dyld_find_pointer_hash_table_entry": 144,
    "__dyld_lazy_load": 145,
    "_malloc_size": 146,
    "__dyld_swift_optimizations_version": 147,
    "__dyld_has_preoptimized_swift_protocol_conformances": 148,
    "__dyld_find_protocol_conformance": 149,


]

let guestMappingBase = 1 << 45

private let patternWidths = ["_memset_pattern4": 4, "_memset_pattern8": 8, "_memset_pattern16": 16]
let guestMemoryImports = Set(["_memcpy", "_memmove", "_memset", "_bzero"])
    .union(patternWidths.keys)

private func mappedOffset(_ address: String, _ count: String, write: Bool) -> String {
    "guest_offset(\(address), \(count), size, writable, \(write ? "true" : "false"), x, resident)"
}

private func hostCall(_ name: String, returnAddress: Int?, cached: Bool) throws -> Instruction {
    if let instruction = guestStringCall(name, returnAddress: returnAddress, cached: cached,
                                        mapOffset: { address, count, write in mappedOffset(address, count, write: write) }) {
        return instruction
    }
    if guestMemoryImports.contains(name) {
        let copy = name == "_memcpy" || name == "_memmove"
        let zero = name == "_bzero"
        let count = zero ? "x[1]" : "x[2]"
        let destination = cached ? mappedOffset("x[0]", count, write: true) : "x[0]"
        let sourceWidth = patternWidths[name].map { "\($0)ul" } ?? count
        let source = cached ? mappedOffset("x[1]", sourceWidth, write: false) : "x[1]"
        let hasSource = copy || patternWidths[name] != nil
        let sourceBounds = !hasSource ? "" : cached ? " || \(source) == ~0ul"
            : " || x[1] > size || \(sourceWidth) > size - x[1]"
        let destinationBounds = cached ? "\(destination) == ~0ul"
            : "x[0] > size || \(count) > size - x[0] || !guest_write(x[0], \(count), writable)"
        let operation: String
        if let width = patternWidths[name] {
            operation = "guest_pattern(memory + \(destination), memory + \(source), \(count), \(width));"
        } else if copy {
            operation = "guest_move(memory + \(destination), memory + \(source), \(count));"
        } else {
            operation = "guest_fill(memory + \(destination), \(zero ? "0" : "uchar(x[1])"), \(count));"
        }
        return Instruction(code: [
            returnAddress.map { "x[30] = \($0)ul;" } ?? "",
            "if (\(count) != 0) {",
            "if (\(destinationBounds)\(sourceBounds)) { x[33] = pc; x[34] = 2; return; }",
            operation, "}",
            "if (x[30] == ~0ul) { x[34] = 0; return; }",
            "if (x[30] >= \(guestCodeLimit)ul) { x[34] = 3; return; }",
            "pc = x[30]; break;",
        ], next: returnAddress.map { [$0] } ?? [])
    }
    guard let service = hostServiceIDs[name] else { throw GuestError("Unsupported executable import \(name)") }
    return Instruction(code: [
        returnAddress.map { "x[30] = \($0)ul;" } ?? "",
        "x[33] = x[30];",
        "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
        "x[35] = \(service)ul; x[34] = 4; return;",
    ], next: [3, 72].contains(service) ? [] : returnAddress.map { [$0] } ?? [])
}

private func decodeProcess(_ bits: UInt32, pc: Int, imports: [Int: String], guestStubs: [Int: Int], cached: Bool) throws -> Instruction {
    func physical(_ address: String, _ width: Int, load: Bool) -> String {
        cached ? mappedOffset(address, "\(width)ul", write: !load) : address
    }
    func outside(_ address: String, _ width: Int, load: Bool) -> String {
        if cached { return "\(physical(address, width, load: load)) == ~0ul" }
        return "\(address) < \(ExecutableImage.minimumImageAddress)ul || \(address) > size - \(width)ul"
            + (load ? "" : " || !guest_write(\(address), \(width)ul, writable)")
    }
    func field(_ start: Int, _ width: Int) -> UInt32 { (bits >> start) & ((1 << width) - 1) }
    func signed(_ raw: UInt32, _ width: Int) -> Int {
        let sign = 1 << (width - 1)
        return (Int(raw) ^ sign) - sign
    }
    func reg(_ index: UInt32, sp: Bool = false) -> String {
        index == 31 && !sp ? "0ul" : "x[\(index)]"
    }
    let next = pc + 4
    let rd = field(0, 5), rn = field(5, 5)
    func emit(_ code: String...) -> Instruction {
        Instruction(code: code.map { $0.replacingOccurrences(of: "x[34] = 2;", with: "x[33] = \(pc)ul; x[34] = 2;") },
                    next: [next])
    }

    if bits == 0xd503201f || bits & 0xffffff3f == 0xd503241f { return emit() }
    if bits == 0xd4001001 {
        return Instruction(code: [
            "x[33] = \(next)ul;",
            "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
            "x[34] = 13; return;",
        ], next: [])
    }
    let dataBarrier = bits & 0xfffff0ff
    if bits == 0xd5033fdf ||
        ([0xd503309f, 0xd50330bf].contains(dataBarrier) && field(8, 2) != 0) {
        return emit("threadgroup_barrier(mem_flags::mem_device);")
    }
    if bits & 0xffffffe0 == 0xd53bd060 {
        return rd == 31 ? emit() : emit("\(reg(rd)) = x[\(guestThreadPointer)];")
    }
    if bits & 0xffffffe0 == 0xd53bd040 {
        // Darwin puts the CPU and cluster indices here. The guest has one of each.
        return rd == 31 ? emit() : emit("\(reg(rd)) = 0ul;")
    }

    let conditions: [UInt32: String] = [0: "z", 1: "!z", 2: "c", 3: "!c",
                                       4: "n", 5: "!n", 6: "v", 7: "!v",
                                       8: "c && !z", 9: "!c || z",
                                       10: "n == v", 11: "n != v",
                                       12: "!z && n == v", 13: "z || n != v"]
    func arithmeticFlags(_ lhs: String, _ rhs: String, _ type: String, subtract: Bool) -> [String] {
        let sign = type == "ulong" ? "0x8000000000000000ul" : "0x80000000u"
        let op = subtract ? "-" : "+"
        let result = "\(type)(\(lhs) \(op) \(rhs))"
        let carry = subtract ? "\(lhs) >= \(rhs)" : "\(result) < \(lhs)"
        let overflow = subtract ? "(\(lhs) ^ \(rhs))" : "~(\(lhs) ^ \(rhs))"
        return ["z = (\(result) == 0); n = ((\(result) & \(sign)) != 0); c = (\(carry));",
                "v = ((\(overflow) & (\(lhs) ^ \(result))) & \(sign)) != 0;"]
    }

    if bits == 0xdac1a7fe || bits == 0xd503237f {
        // Model authenticated returns with a private shadow stack of LR, SP, and signer PC.
        return emit("if (x[37] >= 256ul) { x[34] = 6; return; }",
                    "{ ulong slot = 38ul + 3ul * x[37]++;",
                    "x[slot] = x[30]; x[slot + 1] = x[31]; x[slot + 2] = \(pc)ul; }")
    }
    func checkReturn(signer: Int?) -> [String] {
        let check = signer.map { " || x[slot + 2] != ulong(\($0))" } ?? ""
        return [
            "if (x[37] == 0) { x[34] = 6; return; }",
            "{ ulong slot = 38ul + 3ul * --x[37];",
            "if (x[30] != x[slot] || x[31] != x[slot + 1]\(check)) { x[34] = 6; return; } }",
        ]
    }
    if bits == 0xd50323ff || bits & 0xffe0001f == 0xf3a0001f {
        let signer = bits == 0xd50323ff ? nil : pc - Int(field(5, 16)) * 4
        return Instruction(code: checkReturn(signer: signer), next: [next])
    }
    let authenticated = bits & 0xffe0001f == 0x5520001f || bits == 0xd65f0fff
    if bits == 0xd65f03c0 || authenticated {
        let signer = bits == 0xd65f0fff ? nil : pc - Int(field(5, 16)) * 4
        let authentication = authenticated ? checkReturn(signer: signer) : []
        return Instruction(code: authentication + [
            "if (x[30] == ~0ul) { x[34] = 0; return; }",
            "if (x[30] >= \(guestCodeLimit)ul) { x[34] = 3; return; }",
            "x[33] = x[30];",
            "x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);",
            "x[34] = 7; return;",
        ], next: [])
    }
    let immediateBranch = bits & 0xfc000000
    if immediateBranch == 0x94000000 || immediateBranch == 0x14000000 {
        let linked = immediateBranch == 0x94000000
        let target = pc + signed(field(0, 26), 26) * 4
        let usesGuestAllocator = imports[target] == "_malloc_size"
            && (SharedCache.guestBase..<SharedCache.guestLimit).contains(target)
        if let name = imports[target], !usesGuestAllocator {
            return try hostCall(name, returnAddress: linked ? next : nil, cached: cached)
        }
        let destination = guestStubs[target] ?? target
        return Instruction(code: [
            linked ? "x[30] = \(next)ul;" : "",
            "pc = \(destination)ul; break;",
        ], next: linked ? [destination, next] : [destination])
    }
    if bits & 0xffe0fc00 == 0x9ac03000 {
        return rd == 31 ? emit() : emit("\(reg(rd)) = guest_pacga(x, \(reg(rn)), \(reg(field(16, 5), sp: true)));")
    }
    if bits & 0xfffffbe0 == 0xdac143e0 {
        return rd == 31 ? emit() : emit("x[\(rd)] = strip_guest_pointer(x, x[\(rd)]);")
    }
    let authIP = bits & 0xffffff3f == 0xd503211f
    let authZeroModifier = bits & 0xffffe3e0 == 0xdac123e0
    if authIP || ((authZeroModifier || bits & 0xffffe000 == 0xdac10000) && rd != 31) {
        let destination = authIP ? 17 : rd
        let authenticate = authIP ? field(7, 1) == 1 : field(12, 1) == 1
        let operation = authenticate ? "authenticate_guest_pointer" : "sign_guest_pointer"
        let key = authIP ? field(6, 1) : field(10, 2)
        let failure = key >= 2 ? 9 : 12
        let result = authenticate ? "~0ul" : "0ul"
        let modifier = authIP ? "x[16]" : (authZeroModifier ? "0ul" : reg(rn, sp: true))
        return emit("{ ulong result = \(operation)(x, x[\(destination)], \(modifier), \(key)ul);",
                    "if (result == \(result)) { x[\(guestFaultAddress)] = x[\(destination)]; x[\(guestCacheFaultAddress)] = \(modifier); x[33] = \(pc)ul; x[34] = \(failure); return; }",
                    "x[\(destination)] = result; }")
    }
    let authZero = bits & 0xfffff81f
    let authRegister = bits & 0xfffff800
    if [0xd61f081f, 0xd63f081f].contains(authZero) ||
        [0xd71f0800, 0xd73f0800].contains(authRegister) {
        let zero = [0xd61f081f, 0xd63f081f].contains(authZero)
        let linked = field(21, 1) == 1
        let modifier = zero ? "0ul" : reg(rd, sp: true)
        let key = field(10, 1)
        return Instruction(code: [
            "{ ulong target = authenticate_guest_pointer(x, \(reg(rn)), \(modifier), \(key)ul);",
            "if (target >= \(guestCodeLimit)ul) { x[\(guestFaultAddress)] = \(reg(rn)); x[\(guestCacheFaultAddress)] = \(modifier); x[33] = \(pc)ul; x[34] = 10; return; }",
            linked ? "x[30] = \(next)ul;" : "",
            "pc = target; } break;",
        ], next: linked ? [next] : [])
    }
    let indirect = bits & 0xfffffc1f
    if indirect == 0xd61f0000 || indirect == 0xd63f0000, rn != 31 {
        let linked = indirect == 0xd63f0000
        return Instruction(code: [
            "{ ulong target = \(reg(rn));",
            "if (target >= \(guestCodeLimit)ul) { x[34] = 3; return; }",
            linked ? "x[30] = \(next)ul;" : "",
            "pc = target; } break;",
        ], next: linked ? [next] : [])
    }
    if bits & 0x7ffffc00 == 0x5ac00000 {
        let source = reg(rn)
        let low = "reverse_bits(uint(\(source)))"
        let result = field(31, 1) == 0 ? low :
            "(ulong(\(low)) << 32) | ulong(reverse_bits(uint(\(source) >> 32)))"
        return rd == 31 ? emit() : emit("\(reg(rd)) = \(result);")
    }
    if bits & 0xfffffc00 == 0x5ac00800 {
        return emit(rd == 31 ? "" : """
            { uint value = uint(\(reg(rn)));
            value = ((value & 0x00ff00ffu) << 8) | ((value >> 8) & 0x00ff00ffu);
            \(reg(rd)) = (value << 16) | (value >> 16); }
            """)
    }
    if bits & 0xfffffc00 == 0xdac00c00 {
        return emit(rd == 31 ? "" : """
            { ulong value = \(reg(rn));
            value = ((value & 0x00ff00ff00ff00fful) << 8) | ((value >> 8) & 0x00ff00ff00ff00fful);
            value = ((value & 0x0000ffff0000fffful) << 16) | ((value >> 16) & 0x0000ffff0000fffful);
            \(reg(rd)) = (value << 32) | (value >> 32); }
            """)
    }
    let leadingCount = bits & 0x7ffff800
    if leadingCount == 0x5ac01000 {
        let wide = field(31, 1) != 0
        let type = wide ? "ulong" : "uint"
        let width = wide ? 64 : 32
        let signCount = field(10, 1) != 0
        let normalize = signCount ? "value ^= \(type)(0) - (value >> \(width - 1));" : ""
        return emit(rd == 31 ? "" : "{ \(type) value = \(type)(\(reg(rn))); \(normalize) \(reg(rd)) = (value == 0 ? \(width)u : clz(value)) - \(signCount ? 1 : 0)u; }")
    }
    let select = bits & 0x7fe00c00
    if [0x1a800000, 0x1a800400, 0x5a800000, 0x5a800400].contains(select),
       let condition = conditions[field(12, 4)] {
        let type = field(31, 1) == 0 ? "uint" : "ulong"
        let right = reg(field(16, 5))
        let alternative: String
        switch select {
        case 0x1a800400: alternative = "\(right) + 1ul"
        case 0x5a800000: alternative = "~\(right)"
        case 0x5a800400: alternative = "0ul - \(right)"
        default: alternative = right
        }
        return emit(rd == 31 ? "" : "\(reg(rd)) = \(type)((\(condition)) ? \(reg(rn)) : \(alternative));")
    }
    if bits & 0xff000010 == 0x54000000 {
        if let condition = conditions[field(0, 4)] {
            let target = pc + signed(field(5, 19), 19) * 4
            return Instruction(code: ["pc = (\(condition)) ? \(target)ul : \(next)ul; break;"],
                               next: [target, next])
        }
    }
    if bits & 0x7e000000 == 0x36000000 {
        let index = field(19, 5) | (field(31, 1) << 5)
        let test = field(24, 1) == 0 ? "==" : "!="
        let target = pc + signed(field(5, 14), 14) * 4
        return Instruction(code: ["pc = ((\(reg(rd)) >> \(index)) & 1ul) \(test) 0 ? \(target)ul : \(next)ul; break;"],
                           next: [target, next])
    }
    let flagsImmediate = bits & 0x3f800000 == 0x31000000
    let flagsShifted = bits & 0x3f200000 == 0x2b000000
    let flagsExtended = bits & 0x3fe00000 == 0x2b200000
    if flagsImmediate || flagsShifted || flagsExtended {
        let subtract = field(30, 1) == 1
        let wide = field(31, 1) == 1
        let type = wide ? "ulong" : "uint"
        let lhs = "\(type)(\(reg(rn, sp: flagsImmediate || flagsExtended)))"
        let rhs: String
        if flagsImmediate {
            rhs = "\(type)(\(field(10, 12) << (field(22, 1) * 12))u)"
        } else {
            let source = reg(field(16, 5))
            if flagsExtended {
                let distance = field(10, 3)
                guard distance <= 4 else { throw GuestError("Invalid extended arithmetic shift") }
                let conversion = ["uchar", "ushort", "uint", "ulong", "char", "short", "int", "long"][Int(field(13, 3))]
                rhs = "(\(type)(\(conversion)(\(source))) << \(distance))"
            } else {
                let distance = field(10, 6)
                let shift = field(22, 2)
                guard distance < (wide ? 64 : 32), shift < 3 else { throw GuestError("Invalid arithmetic shift") }
                if shift == 2 {
                    rhs = "\(type)(\(wide ? "long" : "int")(\(source)) >> \(distance))"
                } else {
                    rhs = "(\(type)(\(source)) \(shift == 0 ? "<<" : ">>") \(distance))"
                }
            }
        }
        let op = subtract ? "-" : "+"
        let assign = rd == 31 ? "" : "\(reg(rd)) = \(type)(\(lhs) \(op) \(rhs));"
        return Instruction(code: arithmeticFlags(lhs, rhs, type, subtract: subtract) + [assign], next: [next])
    }
    if bits & 0x3fe00410 == 0x3a400000, let condition = conditions[field(12, 4)] {
        let type = field(31, 1) == 1 ? "ulong" : "uint"
        let flags = field(0, 4)
        let lhs = "\(type)(\(reg(rn)))"
        let rhs = field(11, 1) == 1 ? "\(type)(\(field(16, 5))u)" : "\(type)(\(reg(field(16, 5))))"
        return Instruction(code: ["if (\(condition)) {"] + arithmeticFlags(lhs, rhs, type, subtract: field(30, 1) == 1) +
            ["} else { n = \(flags & 8); z = \(flags & 4); c = \(flags & 2); v = \(flags & 1); }"], next: [next])
    }
    if bits & 0x7f800000 == 0x11000000 || bits & 0x7f800000 == 0x51000000 {
        let immediate = field(10, 12) << (field(22, 1) * 12)
        let operation = field(30, 1) == 0 ? "+" : "-"
        let type = field(31, 1) == 0 ? "uint" : "ulong"
        return emit("\(reg(rd, sp: true)) = \(type)(\(reg(rn, sp: true)) \(operation) \(immediate)ul);")
    }
    if bits & 0x1f000000 == 0x10000000, rd != 31 {
        let displacement = signed((field(5, 19) << 2) | field(29, 2), 21)
        let address = field(31, 1) == 1 ? (pc & ~4095) + displacement * 4096 : pc + displacement
        guard address >= 0 else { throw GuestError("ADRP targets below the executable image") }
        return emit("\(reg(rd)) = \(address)ul;")
    }
    let wideImmediate = bits & 0x7f800000
    if [0x12800000, 0x52800000].contains(wideImmediate), rd != 31,
       field(31, 1) == 1 || field(21, 2) < 2 {
        let type = field(31, 1) == 1 ? "ulong" : "uint"
        let invert = wideImmediate == 0x12800000 ? "~" : ""
        let value = UInt64(field(5, 16)) << (16 * field(21, 2))
        return emit("\(reg(rd)) = \(type)(\(invert)\(value)ul);")
    }
    if bits & 0x7fa00000 == 0x13800000, field(22, 1) == field(31, 1), rd != 31 {
        let width = field(31, 1) == 1 ? 64 : 32
        let type = width == 64 ? "ulong" : "uint"
        let shift = Int(field(10, 6))
        guard shift < width else { throw GuestError("Invalid EXTR shift") }
        let lower = "\(type)(\(reg(field(16, 5))))"
        if shift == 0 { return emit("\(reg(rd)) = \(lower);") }
        let upper = "\(type)(\(reg(rn)))"
        return emit("\(reg(rd)) = (\(lower) >> \(shift)) | (\(upper) << \(width - shift));")
    }
    let division = bits & 0x7fe0fc00
    if [0x1ac00800, 0x1ac00c00].contains(division), rd != 31 {
        let wide = field(31, 1) == 1
        let type = wide ? "ulong" : "uint"
        let left = "\(type)(\(reg(rn)))", right = "\(type)(\(reg(field(16, 5))))"
        if division == 0x1ac00800 {
            return emit("\(reg(rd)) = \(right) == 0 ? 0 : \(left) / \(right);")
        }
        let signedType = wide ? "long" : "int"
        let minimum = wide ? "0x8000000000000000ul" : "0x80000000u"
        return emit("\(reg(rd)) = \(right) == 0 ? 0 : (\(left) == \(minimum) && \(right) == ~\(type)(0)) ? \(left) : \(type)(\(signedType)(\(left)) / \(signedType)(\(right)));")
    }
    let variableShift = bits & 0x7fe0fc00
    if [0x1ac02000, 0x1ac02400, 0x1ac02800, 0x1ac02c00].contains(variableShift), rd != 31 {
        let wide = field(31, 1) == 1
        let width = wide ? 64 : 32
        let type = wide ? "ulong" : "uint"
        let source = "\(type)(\(reg(rn)))"
        let shift = "(uint(\(reg(field(16, 5)))) & \(width - 1)u)"
        let value: String
        switch variableShift {
        case 0x1ac02000: value = "\(source) << \(shift)"
        case 0x1ac02400: value = "\(source) >> \(shift)"
        case 0x1ac02800: value = "\(wide ? "long" : "int")(\(source)) >> \(shift)"
        default: value = "(\(source) >> \(shift)) | (\(source) << ((0u - \(shift)) & \(width - 1)u))"
        }
        return emit("\(reg(rd)) = \(type)(\(value));")
    }
    let logicalRegister = bits & 0x7f000000
    if [0x0a000000, 0x2a000000, 0x4a000000, 0x6a000000].contains(logicalRegister),
       field(10, 6) < (field(31, 1) == 0 ? 32 : 64) {
        let wide = field(31, 1) == 1
        let type = wide ? "ulong" : "uint"
        let width = wide ? 64 : 32
        let operation = ["&", "|", "^", "&"][Int(field(29, 2))]
        let left = "\(type)(\(reg(rn)))"
        let source = "\(type)(\(reg(field(16, 5))))"
        let distance = field(10, 6)
        let shifted: String
        switch field(22, 2) {
        case 0: shifted = "(\(source) << \(distance))"
        case 1: shifted = "(\(source) >> \(distance))"
        case 2: shifted = "\(type)(\(wide ? "long" : "int")(\(source)) >> \(distance))"
        default:
            shifted = distance == 0 ? source : "((\(source) >> \(distance)) | (\(source) << \(width - Int(distance))))"
        }
        let right = field(21, 1) == 0 ? shifted : "~\(shifted)"
        let result = "\(type)(\(left) \(operation) \(right))"
        let assign = rd == 31 ? "" : "\(reg(rd)) = \(result);"
        if logicalRegister == 0x6a000000 {
            return emit("z = (\(result) == 0); n = ((\(result) >> \(width - 1)) != 0); c = false; v = false;", assign)
        }
        return emit(assign)
    }
    if bits & 0x7f800000 == 0x72800000, rd != 31, field(31, 1) == 1 || field(21, 2) < 2 {
        let shift = 16 * field(21, 2)
        let mask = ~(UInt64(0xffff) << shift)
        let immediate = UInt64(field(5, 16)) << shift
        let type = field(31, 1) == 0 ? "uint" : "ulong"
        return emit("\(reg(rd)) = \(type)((\(reg(rd)) & \(mask)ul) | \(immediate)ul);")
    }
    if bits & 0xffe0f000 == 0x9a002000, rd != 31, rn != 31 {
        // Guest pointers have no top-byte tags.
        return emit("\(reg(rd)) = \(reg(rn)) + (\(reg(field(16, 5))) << \(field(10, 2)));")
    }
    if bits & 0x7fe0e000 == 0x1ac04000, rd != 31 {
        let operation = field(10, 3)
        let width = 1 << (operation & 3)
        guard (width == 8) == (field(31, 1) == 1) else { throw GuestError("Invalid CRC32 width") }
        let polynomial = operation < 4 ? "0xedb88320u" : "0x82f63b78u"
        return emit("\(reg(rd)) = guest_crc32(uint(\(reg(rn))), \(reg(field(16, 5))), \(width), \(polynomial));")
    }
    let wideningMultiply = bits & 0xffe00000
    if [0x9b200000, 0x9ba00000].contains(wideningMultiply), rd != 31 {
        let sign = wideningMultiply == 0x9b200000
        let operation = field(15, 1) == 0 ? "+" : "-"
        let accumulator = reg(field(10, 5))
        let lhs = "\(sign ? "long(int" : "ulong(uint")(\(reg(rn))))"
        let rhs = "\(sign ? "long(int" : "ulong(uint")(\(reg(field(16, 5)))))"
        return emit("\(reg(rd)) = \(accumulator) \(operation) ulong(\(lhs) * \(rhs));")
    }
    let multiplyHigh = bits & 0xffe0fc00
    if [0x9b407c00, 0x9bc07c00].contains(multiplyHigh), rd != 31 {
        let lhs = reg(rn), rhs = reg(field(16, 5))
        let high = "multiply_high(\(lhs), \(rhs))"
        let result = multiplyHigh == 0x9b407c00
            ? "\(high) - (\(lhs) >> 63) * \(rhs) - (\(rhs) >> 63) * \(lhs)" : high
        return emit("\(reg(rd)) = \(result);")
    }
    if bits & 0x7fe00000 == 0x1b000000, rd != 31 {
        let type = field(31, 1) == 1 ? "ulong" : "uint"
        let operation = field(15, 1) == 0 ? "+" : "-"
        let accumulator = "\(type)(\(reg(field(10, 5))))"
        let left = "\(type)(\(reg(rn)))"
        let right = "\(type)(\(reg(field(16, 5))))"
        return emit("\(reg(rd)) = \(type)(\(accumulator) \(operation) \(left) * \(right));")
    }
    let shiftedArithmetic = bits & 0x7f200000
    if [0x0b000000, 0x4b000000].contains(shiftedArithmetic),
       field(22, 2) < 3, field(10, 6) < (field(31, 1) == 0 ? 32 : 64) {
        let type = field(31, 1) == 0 ? "uint" : "ulong"
        let signedType = field(31, 1) == 0 ? "int" : "long"
        let operation = shiftedArithmetic == 0x0b000000 ? "+" : "-"
        let shift = field(22, 2) == 0 ? "<<" : ">>"
        let operandType = field(22, 2) == 2 ? signedType : type
        let right = "\(type)(\(operandType)(\(reg(field(16, 5)))) \(shift) \(field(10, 6)))"
        return emit(rd == 31 ? "" : "\(reg(rd)) = \(type)(\(type)(\(reg(rn))) \(operation) \(right));")
    }
    let minmax = bits & 0x7fe0f000
    if minmax == 0x1ac06000 {
        let wide = field(31, 1) != 0
        let type = wide ? "ulong" : "uint"
        let operand = field(10, 1) == 0 ? (wide ? "long" : "int") : type
        let function = field(11, 1) == 0 ? "max" : "min"
        return emit(rd == 31 ? "" : "\(reg(rd)) = \(type)(\(function)(\(operand)(\(reg(rn))), \(operand)(\(reg(field(16, 5))))));")
    }
    if bits & 0xfffffc00 == 0x93407c00, rd != 31 {
        return emit("\(reg(rd)) = ulong(long(int(\(reg(rn)))));")
    }
    let extendedArithmetic = bits & 0x7fe00000
    if [0x0b200000, 0x4b200000].contains(extendedArithmetic), field(10, 3) <= 4 {
        let type = field(31, 1) == 0 ? "uint" : "ulong"
        let extensionType = ["uchar", "ushort", "uint", "ulong", "char", "short", "int", "long"][Int(field(13, 3))]
        let rhs = "\(type)(\(extensionType)(\(reg(field(16, 5)))))"
        let operation = extendedArithmetic == 0x0b200000 ? "+" : "-"
        return emit("\(reg(rd, sp: true)) = \(type)(\(reg(rn, sp: true)) \(operation) (\(rhs) << \(field(10, 3))));")
    }
    if bits & 0x1f800000 == 0x12000000 {
        let width = field(31, 1) == 1 ? 64 : 32
        let imms = Int(field(10, 6))
        let marker = (Int(field(22, 1)) << 6) | (~imms & 63)
        guard marker >= 2 else { throw GuestError("Reserved logical immediate") }
        let element = 1 << (Int.bitWidth - 1 - marker.leadingZeroBitCount)
        let length = (imms & (element - 1)) + 1
        guard element <= width, length < element else { throw GuestError("Reserved logical immediate") }
        let rotation = Int(field(16, 6)) & (element - 1)
        let ones = (UInt64(1) << length) - 1
        let elementMask = element == 64 ? UInt64.max : (UInt64(1) << element) - 1
        let pattern = rotation == 0 ? ones : ((ones >> rotation) | (ones << (element - rotation))) & elementMask
        var mask: UInt64 = 0
        for offset in stride(from: 0, to: width, by: element) { mask |= pattern << offset }
        let op = ["&", "|", "^", "&"][Int(field(29, 2))]
        let source = width == 32 ? "uint(\(reg(rn)))" : reg(rn)
        let result = "(\(source) \(op) \(mask)ul)"
        let assign = rd == 31 ? "" : "\(reg(rd)) = \(result);"
        if bits & 0x60000000 == 0x60000000 {
            return emit("z = (\(result) == 0); n = ((\(result) >> \(width - 1)) != 0); c = false; v = false;", assign)
        }
        return emit(assign)
    }
    if bits & 0x7f800000 == 0x53000000 {
        let width = field(31, 1) == 1 ? 64 : 32
        let rotation = Int(field(16, 6)), end = Int(field(10, 6))
        guard rotation < width, end < width, field(22, 1) == field(31, 1) else {
            throw GuestError("Reserved bitfield encoding")
        }
        let count = end >= rotation ? end - rotation + 1 : end + 1
        let mask = count == 64 ? UInt64.max : (UInt64(1) << count) - 1
        let source = reg(rn)
        let result = end >= rotation
            ? "((\(source) >> \(rotation)) & \(mask)ul)"
            : "((\(source) & \(mask)ul) << \(width - rotation))"
        return emit(rd == 31 ? "" : "\(reg(rd)) = \(result);")
    }

    if bits & 0x7f800000 == 0x33000000, field(22, 1) == field(31, 1), rd != 31 {
        let wide = field(31, 1) == 1
        let width = wide ? 64 : 32
        let rotation = Int(field(16, 6)), end = Int(field(10, 6))
        guard rotation < width, end < width else { throw GuestError("Reserved bitfield merge encoding") }
        let count = end >= rotation ? end - rotation + 1 : end + 1
        let mask = count == 64 ? UInt64.max : (UInt64(1) << count) - 1
        let position = end >= rotation ? 0 : width - rotation
        let source = reg(rn)
        let extracted = end >= rotation ? "((\(source) >> \(rotation)) & \(mask)ul)"
            : "((\(source) & \(mask)ul) << \(position))"
        return emit("\(reg(rd)) = \(wide ? "ulong" : "uint")((\(reg(rd)) & ~\(mask << position)ul) | \(extracted));")
    }
    if bits & 0x7f800000 == 0x13000000, field(22, 1) == field(31, 1), rd != 31 {
        let wide = field(31, 1) == 1
        let width = wide ? 64 : 32
        let rotation = Int(field(16, 6)), end = Int(field(10, 6))
        guard rotation < width, end < width else { throw GuestError("Reserved signed bitfield encoding") }
        let count = end >= rotation ? end - rotation + 1 : end + 1
        let mask = count == 64 ? UInt64.max : (UInt64(1) << count) - 1
        let position = end >= rotation ? 0 : width - rotation
        let source = reg(rn)
        let extracted = end >= rotation ? "((\(source) >> \(rotation)) & \(mask)ul)"
            : "((\(source) & \(mask)ul) << \(position))"
        let fill = width - (count + position)
        let type = wide ? "ulong" : "uint", signedType = wide ? "long" : "int"
        return emit("\(reg(rd)) = \(type)(\(signedType)(\(type)(\(extracted)) << \(fill)) >> \(fill));")
    }

    func vector(_ index: UInt32) -> String {
        "ulong2(x[\(806 + 2 * index)], x[\(807 + 2 * index)])"
    }
    func assignVector(_ expression: String) -> Instruction {
        emit("{ ulong2 value = \(expression); x[\(806 + 2 * rd)] = value[0]; x[\(807 + 2 * rd)] = value[1]; }")
    }
    if let lowered = guestVectorCrypto(bits, pc: pc) { return lowered }
    if bits & 0xbfe08400 == 0x2e000000 {
        let lanes = field(30, 1) == 0 ? 8 : 16
        let start = Int(field(11, 4))
        guard start < lanes else { throw GuestError("Invalid vector extract offset") }
        return emit("""
            { ulong2 first = \(vector(rn)), second = \(vector(field(16, 5))), value = ulong2(0);
              for (uint lane = 0; lane < \(lanes); ++lane) {
                  uint index = lane + \(start);
                  ulong byte = index < \(lanes) ? lane_read(first, index, 8)
                                                 : lane_read(second, index - \(lanes), 8);
                  lane_add(value, lane, 8, byte);
              }
              x[\(806 + 2 * rd)] = value[0]; x[\(807 + 2 * rd)] = value[1]; }
            """)
    }
    if bits & 0xbfe08c00 == 0x0e000000 {
        let count = Int(field(13, 2)) + 1
        let table = (0..<count).map { vector((rn + UInt32($0)) & 31) }.joined(separator: ", ")
        let indices = vector(field(16, 5))
        let original = field(12, 1) == 0 ? "ulong2(0)" : vector(rd)
        let lanes = field(30, 1) == 0 ? 8 : 16
        return emit("""
            { ulong2 table[\(count)] = { \(table) };
              ulong2 indices = \(indices), original = \(original), value = ulong2(0);
              for (uint lane = 0; lane < \(lanes); ++lane) {
                  uint index = uint(lane_read(indices, lane, 8));
                  ulong byte = index < \(count * 16) ? lane_read(table[index / 16], index % 16, 8)
                                                      : lane_read(original, lane, 8);
                  lane_add(value, lane, 8, byte);
              }
              x[\(806 + 2 * rd)] = value[0]; x[\(807 + 2 * rd)] = value[1]; }
            """)
    }
    if bits & 0xbf3ffc00 == 0x0e200800 {
        let width = 8 << field(22, 2)
        guard width < 64 else { throw GuestError("Invalid REV64 arrangement") }
        let lanes = (field(30, 1) == 0 ? 64 : 128) / Int(width)
        return emit("""
            { ulong2 source = \(vector(rn)), value = ulong2(0);
              for (uint lane = 0; lane < \(lanes); ++lane)
                  lane_add(value, lane, \(width), lane_read(source, lane ^ \(64 / width - 1), \(width)));
              x[\(806 + 2 * rd)] = value[0]; x[\(807 + 2 * rd)] = value[1]; }
            """)
    }
    if bits & 0xfffffc00 == 0x6e200800 {
        let source = vector(rn)
        return assignVector("""
            ((\(source) & 0x000000ff000000fful) << 24) |
            ((\(source) & 0x0000ff000000ff00ul) << 8) |
            ((\(source) >> 8) & 0x0000ff000000ff00ul) |
            ((\(source) >> 24) & 0x000000ff000000fful)
            """)
    }
    if bits & 0xffe0fc00 == 0x4e001c00 {
        let immediate = field(16, 5)
        guard immediate != 0, immediate.trailingZeroBitCount < 4 else {
            throw GuestError("Invalid vector INS element size")
        }
        let width = 8 << immediate.trailingZeroBitCount
        let lane = Int(immediate) >> (immediate.trailingZeroBitCount + 1)
        let bit = width * lane
        let slot = Int(806 + 2 * rd) + bit / 64
        let shift = bit % 64
        let mask = width == 64 ? UInt64.max : (UInt64(1) << width) - 1
        return emit("x[\(slot)] = (x[\(slot)] & ~\(mask << shift)ul) | ((\(reg(rn)) & \(mask)ul) << \(shift));")
    }
    let vectorBits = field(30, 1) == 1 ? 128 : 64
    if bits & 0x9f80fc00 == 0x0f00a400 {
        let immediate = Int(field(16, 7))
        guard (8..<64).contains(immediate) else { throw GuestError("Invalid widening shift immediate") }
        let width = 1 << (Int.bitWidth - 1 - immediate.leadingZeroBitCount)
        let shift = immediate - width
        let offset = vectorBits == 128 ? 64 / width : 0
        let sign = field(29, 1) == 0 ? UInt64(1) << (width - 1) : 0
        return emit("""
            { ulong2 source = \(vector(rn)), result = ulong2(0);
              for (uint lane = 0; lane < \(64 / width); ++lane) {
                  ulong value = lane_read(source, lane + \(offset), \(width));
                  ulong extended = (value ^ \(sign)ul) - \(sign)ul;
                  lane_add(result, lane, \(2 * width), extended << \(shift));
              }
              x[\(806 + 2 * rd)] = result[0]; x[\(807 + 2 * rd)] = result[1]; }
            """)
    }
    if bits & 0xbf20fc00 == 0x2e20c000 {
        let width = 8 << field(22, 2)
        return assignVector("vector_long(ulong2(0), \(vector(rn)), \(vector(field(16, 5))), \(width), \(vectorBits), false, false, false)")
    }
    if bits & 0xbf20fc00 == 0x2e208000 {
        let width = 8 << field(22, 2)
        let top = field(30, 1) == 1
        return assignVector("vector_long(\(vector(rd)), \(vector(rn)), \(vector(field(16, 5))), \(width), \(vectorBits), true, \(top ? "true" : "false"), false)")
    }
    if bits & 0xbf3ffc00 == 0x2e303800 {
        let sourceWidth = 8 << field(22, 2)
        let destinationWidth = sourceWidth * 2
        return assignVector("vector_reduce_long(\(vector(rn)), \(sourceWidth), \(destinationWidth), \(vectorBits))")
    }
    if bits & 0xbf20fc00 == 0x2e200000 {
        let width = 8 << field(22, 2)
        return assignVector("vector_add_long(\(vector(rn)), \(vector(field(16, 5))), \(width), \(vectorBits), false)")
    }
    if bits & 0xbf20fc00 == 0x0e003800 {
        let width = 8 << field(22, 2)
        return assignVector("vector_zip1(\(vector(rn)), \(vector(field(16, 5))), \(width), \(vectorBits))")
    }
    if bits & 0xff20fc00 == 0x2f20a000 {
        let width = 8 << field(22, 2)
        let lane = (field(11, 1) << 1) | field(21, 1)
        return assignVector("vector_long(ulong2(0), \(vector(rn)), vector_dup(lane_read(\(vector(field(16, 5))), \(lane), \(width)), \(width), \(vectorBits)), \(width), \(vectorBits), false, false, false)")
    }
    if bits & 0xff20fc00 == 0x2f004000 {
        let width = 8 << field(22, 2)
        let lane = field(11, 1) | (field(21, 1) << 1)
        return assignVector("vector_mla(\(vector(rd)), \(vector(rn)), vector_dup(lane_read(\(vector(field(16, 5))), \(lane), \(width)), \(width), \(vectorBits)), \(width), \(vectorBits), true)")
    }
    if bits & 0xffe08400 == 0x6e000400 {
        let immediate = field(16, 5)
        let sourceImmediate = field(11, 4)
        guard immediate != 0, immediate.trailingZeroBitCount < 4 else { throw GuestError("Invalid SIMD INS lane encoding") }
        let width = 8 << immediate.trailingZeroBitCount
        let destinationLane = Int(immediate >> (immediate.trailingZeroBitCount + 1))
        let sourceLane = Int(sourceImmediate >> immediate.trailingZeroBitCount)
        let destinationBit = destinationLane * width, sourceBit = sourceLane * width
        let dslot = Int(806 + 2 * rd) + destinationBit / 64
        let sslot = Int(806 + 2 * rn) + sourceBit / 64
        let mask = width == 64 ? UInt64.max : (UInt64(1) << width) - 1
        return emit("x[\(dslot)] = (x[\(dslot)] & ~(\(mask)ul << \(destinationBit % 64))) | ((x[\(sslot)] >> \(sourceBit % 64) & \(mask)ul) << \(destinationBit % 64));")
    }
    if bits & 0xbfe0fc00 == 0x0e002c00 {
        let immediate = field(16, 5)
        let wide = field(30, 1) != 0
        guard immediate != 0, immediate.trailingZeroBitCount < (wide ? 3 : 2) else {
            throw GuestError("Invalid SMOV lane encoding")
        }
        let width = 8 << immediate.trailingZeroBitCount
        let lane = immediate >> (immediate.trailingZeroBitCount + 1)
        let sign = UInt64(1) << (width - 1)
        let type = wide ? "ulong" : "uint"
        return rd == 31 ? emit() : emit(
            "\(reg(rd)) = \(type)((lane_read(\(vector(rn)), \(lane), \(width)) ^ \(sign)ul) - \(sign)ul);"
        )
    }
    if bits & 0xbf20fc00 == 0x0e003c00 {
        let immediate = field(16, 5)
        let width = 8 << immediate.trailingZeroBitCount
        let lane = immediate >> (immediate.trailingZeroBitCount + 1)
        let sourceBit = Int(lane) * width
        let value = "((x[\(806 + 2 * rn + UInt32(sourceBit / 64))] >> \(sourceBit % 64)) & \(width == 64 ? "~0ul" : "\((UInt64(1) << width) - 1)ul"))"
        return emit(rd == 31 ? "" : "\(reg(rd)) = \(width == 64 ? "ulong" : "uint")\(value);")
    }
    let scalarCopy = bits & 0xffe0fc00 == 0x5e000400
    if scalarCopy || bits & 0xbfe0fc00 == 0x0e000400 {
        let immediate = field(16, 5)
        guard immediate != 0, immediate.trailingZeroBitCount < 4 else {
            throw GuestError("Invalid SIMD lane copy")
        }
        let width = 8 << immediate.trailingZeroBitCount
        guard scalarCopy || width < 64 || vectorBits == 128 else {
            throw GuestError("Invalid vector DUP arrangement")
        }
        let lane = immediate >> (immediate.trailingZeroBitCount + 1)
        let value = "lane_read(\(vector(rn)), \(lane), \(width))"
        return assignVector(scalarCopy ? "ulong2(\(value), 0ul)" : "vector_dup(\(value), \(width), \(vectorBits))")
    }
    if bits & 0xbfe0fc00 == 0x0e000c00 {
        let immediate = field(16, 5)
        guard immediate != 0, immediate.trailingZeroBitCount < 4 else {
            throw GuestError("Invalid vector DUP element size")
        }
        let width = 8 << immediate.trailingZeroBitCount
        guard width < 64 || vectorBits == 128 else { throw GuestError("Invalid vector DUP arrangement") }
        return assignVector("vector_dup(\(reg(rn)), \(width), \(vectorBits))")
    }
    switch bits & 0xffffffe0 {
    case 0xd53b4400, 0xd53b4420:
        let slot = bits & 32 == 0 ? guestFPCR : guestFPSR
        return emit(rd == 31 ? "" : "\(reg(rd)) = x[\(slot)];")
    case 0xd51b4400, 0xd51b4420:
        let fpcr = bits & 32 == 0
        let slot = fpcr ? guestFPCR : guestFPSR
        let mask = fpcr ? "0x00c00000ul" : "0xf800009ful"
        return emit("x[\(slot)] = \(reg(rd)) & \(mask);")
    default:
        break
    }
    if bits & 0x7f3efc00 == 0x1e220000 {
        let wide = field(31, 1) == 1
        let unsigned = field(16, 1) == 1
        let double = field(22, 1) == 1
        let source = wide ? reg(rn) : (unsigned ? "ulong(uint(\(reg(rn))))" : "ulong(long(int(\(reg(rn)))))")
        let negative = unsigned ? "false" : "(long(\(source)) < 0)"
        return assignVector("ulong2(integer_to_float(\(source), \(negative), \(double ? 52 : 23)u, x), 0ul)")
    }
    let vectorMove = bits & 0xfffffc00
    if [0x1e260000, 0x1e270000, 0x9e660000, 0x9e670000].contains(vectorMove) {
        let wide = field(31, 1) == 1
        let type = wide ? "ulong" : "uint"
        if field(16, 1) == 0 {
            return emit(rd == 31 ? "" : "\(reg(rd)) = \(type)(x[\(806 + 2 * rn)]);")
        }
        return assignVector("ulong2(\(type)(\(reg(rn))), 0ul)")
    }
    if bits & 0xbf3ffc00 == 0x0e31b800 {
        let width = 8 << field(22, 2)
        guard width < 64, width != 32 || vectorBits == 128 else {
            throw GuestError("Invalid vector reduction arrangement")
        }
        return assignVector("vector_sum(\(vector(rn)), \(width), \(vectorBits))")
    }
    if bits & 0x9f3efc00 == 0x0e30a800 {
        let width = 8 << field(22, 2)
        guard width < 64, width != 32 || vectorBits == 128 else {
            throw GuestError("Invalid vector min/max arrangement")
        }
        let minimum = field(16, 1) == 1
        let signedLanes = field(29, 1) == 0
        return assignVector("vector_extreme(\(vector(rn)), \(width), \(vectorBits), \(minimum), \(signedLanes))")
    }
    let equalLanes = bits & 0xbf20fc00 == 0x2e208c00
    if equalLanes || bits & 0x9f20f400 == 0x0e203400 {
        let width = 8 << field(22, 2)
        guard width != 64 || vectorBits == 128 else {
            throw GuestError("Invalid vector comparison arrangement")
        }
        let inclusive = field(11, 1) == 1
        let signedLanes = field(29, 1) == 0
        return assignVector("vector_compare(\(vector(rn)), \(vector(field(16, 5))), \(width), \(vectorBits), \(inclusive), \(signedLanes), \(equalLanes))")
    }
    let vectorLogical = bits & 0xbfe0fc00
    if let operation = [0x0e201c00: "&", 0x0e601c00: "&", 0x0ea01c00: "|",
                        0x0ee01c00: "|", 0x2e201c00: "^"][vectorLogical] {
        let right = vector(field(16, 5))
        let inverted = [0x0e601c00, 0x0ee01c00].contains(vectorLogical) ? "~" : ""
        let mask = vectorBits == 128 ? "" : " & ulong2(~0ul, 0ul)"
        return assignVector("(\(vector(rn)) \(operation) \(inverted)\(right))\(mask)")
    }
    let vectorOperation = bits & 0xbf20fc00
    if [0x2e204400, 0x0e001800, 0x0e208400, 0x1e208400, 0x2e208400, 0x0e209400, 0x2e209400, 0x0e209c00].contains(vectorOperation) {
        let width = 8 << field(22, 2)
        guard width < 64 || vectorBits == 128 || vectorOperation == 0x1e208400 else { throw GuestError("Invalid vector arrangement") }
        let arguments = "\(vector(rn)), \(vector(field(16, 5))), \(width), \(vectorBits)"
        if vectorOperation == 0x0e208400 || vectorOperation == 0x1e208400 || vectorOperation == 0x2e208400 {
            return assignVector("vector_add(\(arguments), \(vectorOperation == 0x2e208400 ? "true" : "false"))")
        }
        if [0x0e209400, 0x2e209400, 0x0e209c00].contains(vectorOperation) {
            guard width < 64 else { throw GuestError("Invalid vector multiply-accumulate arrangement") }
            let accumulator = vectorOperation == 0x0e209c00 ? "ulong2(0)" : vector(rd)
            return assignVector("vector_mla(\(accumulator), \(arguments), \(vectorOperation == 0x2e209400 ? "true" : "false"))")
        }
        let helper = vectorOperation == 0x2e204400 ? "vector_ushl" : "vector_uzp1"
        return assignVector("\(helper)(\(arguments))")
    }
    let immediateShift = bits & 0xbf80fc00
    if [0x0f005400, 0x1f005400, 0x2f000400, 0x2f001400].contains(immediateShift), field(16, 7) >= 8 {
        let encoded = Int(field(16, 7))
        let width = 1 << (Int.bitWidth - 1 - encoded.leadingZeroBitCount)
        guard width < 64 || vectorBits == 128 || immediateShift == 0x1f005400 else { throw GuestError("Invalid vector shift arrangement") }
        let shift = immediateShift == 0x0f005400 || immediateShift == 0x1f005400 ? encoded - width : 2 * width - encoded
        let accumulator = immediateShift == 0x2f001400 ? vector(rd) : "ulong2(0)"
        return assignVector("vector_shift(\(accumulator), \(vector(rn)), \(width), \(vectorBits), \(shift), \(immediateShift == 0x0f005400 || immediateShift == 0x1f005400 ? "true" : "false"))")
    }
    if bits & 0xbf800c00 == 0x2f000400 {
        let mode = field(12, 4)
        if [1, 3, 5, 7, 9, 11].contains(mode) {
            let width = mode < 8 ? 32 : 16
            let shift = Int((mode & 7) >> 1) * 8
            let immediate = (field(16, 3) << 5) | field(5, 5)
            let mask = vectorBits == 128 ? "ulong2(~0ul)" : "ulong2(~0ul, 0ul)"
            return assignVector("(\(vector(rd)) & ~vector_dup(\(UInt64(immediate) << shift)ul, \(width), \(vectorBits))) & \(mask)")
        }
    }

    if bits & 0xbf800c00 == 0x0f000400 {
        let mode = field(12, 4)
        if [0, 2, 4, 6, 8, 10, 14].contains(mode) {
            let width = mode < 8 ? 32 : (mode == 14 ? 8 : 16)
            let shift = mode == 14 ? 0 : Int((mode & 7) >> 1) * 8
            let immediate = UInt64((field(16, 3) << 5) | field(5, 5)) << shift
            return assignVector("vector_dup(\(immediate)ul, \(width), \(vectorBits))")
        }
    }
    if bits & 0xbff8fc00 == 0x2f00e400 {
        let immediate = (field(16, 3) << 5) | field(5, 5)
        var value: UInt64 = 0
        for byte in 0..<8 where immediate & (1 << byte) != 0 {
            value |= UInt64(255) << (byte * 8)
        }
        return assignVector("ulong2(\(value)ul, \(vectorBits == 128 ? value : 0)ul)")
    }
    // Each dispatch has one active guest thread. These operations are indivisible
    // at the scheduler boundary and shared buffers are synchronized on dispatch completion.
    let atomicOperation = [0x38200000: "+", 0x38201000: "& ~", 0x38202000: "^", 0x38203000: "|"][bits & 0x3f20fc00]
    let atomicSwap = bits & 0x3f20fc00 == 0x38208000
    let atomicCompare = bits & 0x3fa07c00 == 0x08a07c00
    if bits & 0xbfa07c00 == 0x08207c00 {
        let compare = field(16, 5)
        guard compare.isMultiple(of: 2), rd.isMultiple(of: 2) else {
            throw GuestError("Invalid CASP register pair")
        }
        let width = field(30, 1) == 0 ? 4 : 8
        let count = 2 * width
        let type = width == 4 ? "uint" : "ulong"
        let pointer = physical("address", count, load: false)
        let secondResult = compare == 30 ? "" : "\(reg(compare + 1)) = oldHigh;"
        return emit("""
            { ulong address = \(reg(rn, sp: true));
              if ((address & \(count - 1)ul) || \(outside("address", count, load: false))) {
                  x[\(guestFaultAddress)] = address; x[34] = 2; return;
              }
              device uchar *pair = memory + \(pointer);
              ulong oldLow = load_guest_word(pair, \(width));
              ulong oldHigh = load_guest_word(pair + \(width), \(width));
              if (oldLow == \(type)(\(reg(compare))) && oldHigh == \(type)(\(reg(compare + 1)))) {
                  store_guest_word(pair, \(reg(rd)), \(width));
                  store_guest_word(pair + \(width), \(reg(rd + 1)), \(width));
              }
              \(reg(compare)) = oldLow;
              \(secondResult) }
            """)
    }
    let ordered = bits & 0x3fbffc00 == 0x089ffc00
    let rcpcAcquire = bits & 0x3ffffc00 == 0x38bfc000
    if atomicOperation != nil || atomicSwap || atomicCompare || ordered || rcpcAcquire {
        let width = 1 << field(30, 2)
        let load = rcpcAcquire || (ordered && field(22, 1) == 1)
        let address = reg(rn, sp: true)
        let source = reg(field(16, 5))
        let mask = width == 8 ? UInt64.max : (UInt64(1) << (width * 8)) - 1
        let pointer = physical("address", width, load: load)
        let bound = "if ((address & \(width - 1)ul) || \(outside("address", width, load: load))) { x[\(guestFaultAddress)] = address; x[33] = \(pc)ul; x[34] = 2; return; }"
        let get = "ulong old = load_guest_word(memory + \(pointer), \(width));"
        func store(_ value: String) -> String { "store_guest_word(memory + \(pointer), \(value), \(width));" }
        func save(_ register: UInt32) -> String { register == 31 ? "" : "\(reg(register)) = old;" }
        let operation: [String]
        if let atomicOperation {
            operation = [get, store("old \(atomicOperation) \(source)"), save(rd)]
        } else if atomicSwap {
            operation = [get, store(source), save(rd)]
        } else if atomicCompare {
            operation = [get, "if (old == (\(source) & \(mask)ul)) { \(store(reg(rd))) }", save(field(16, 5))]
        } else if load {
            operation = [get, save(rd)]
        } else {
            operation = [store(reg(rd))]
        }
        return Instruction(code: ["{ ulong address = \(address);", bound] +
                                  operation + ["}"], next: [next])
    }
    if bits & 0xbf200000 == 0x0c000000,
       let registers = [0x7: 1, 0xa: 2, 0x6: 3, 0x2: 4][field(12, 4)] {
        let post = field(23, 1) != 0
        let index = field(16, 5)
        guard post || index == 0 else { throw GuestError("Invalid SIMD structure address") }
        let width = field(30, 1) == 0 ? 8 : 16
        let bytes = registers * width
        let load = field(22, 1) != 0
        let base = reg(rn, sp: true)
        let pointer = physical("address", bytes, load: load)
        var code = ["{ ulong address = \(base);",
                    "if (\(outside("address", bytes, load: load))) { x[33] = \(pc)ul; x[34] = 2; return; }"]
        for index in 0..<registers {
            let slot = 806 + 2 * ((Int(rd) + index) & 31)
            let start = "memory + \(pointer) + \(index * width)"
            if load {
                code.append("x[\(slot)] = load_guest_word(\(start), 8);")
                let high = width == 16 ? "load_guest_word(\(start) + 8, 8)" : "0"
                code.append("x[\(slot + 1)] = \(high);")
            } else {
                code.append("store_guest_word(\(start), x[\(slot)], 8);")
                if width == 16 { code.append("store_guest_word(\(start) + 8, x[\(slot + 1)], 8);") }
            }
        }
        if post { code.append("\(base) += \(index == 31 ? "\(bytes)ul" : reg(index));") }
        return Instruction(code: code + ["}"], next: [next])
    }
    let vectorPair = bits & 0x3fc00000
    if [0x2d000000, 0x2d400000, 0x2d800000, 0x2dc00000, 0x2c800000, 0x2cc00000].contains(vectorPair),
       field(30, 2) < 3 {
        let width = 4 << field(30, 2)
        let base = reg(rn, sp: true)
        let offset = signed(field(15, 7), 7) * width
        let second = field(10, 5)
        let mode = field(23, 2)
        let load = field(22, 1) == 1
        guard !load || rd != second else { throw GuestError("Unpredictable vector pair overlap") }
        let address = mode == 1 ? base : "\(base) + ulong(\(offset))"
        func transfer(_ register: UInt32, at displacement: Int) -> [String] {
            let first = 806 + 2 * register
            let pointer = "memory + \(physical("address", 2 * width, load: load)) + \(displacement)"
            let count = min(8, width)
            if load {
                let upper = width == 16 ? "load_guest_word(\(pointer) + 8, 8)" : "0"
                return ["x[\(first)] = load_guest_word(\(pointer), \(count));", "x[\(first + 1)] = \(upper);"]
            }
            var lines = ["store_guest_word(\(pointer), x[\(first)], \(count));"]
            if width == 16 { lines.append("store_guest_word(\(pointer) + 8, x[\(first + 1)], 8);") }
            return lines
        }
        return Instruction(code: ["{ ulong address = \(address);",
                                  "if (\(outside("address", 2 * width, load: load))) { x[33] = \(pc)ul; x[34] = 2; return; }"] +
                                  transfer(rd, at: 0) + transfer(second, at: width) +
                                  [mode == 2 ? "" : "\(base) += ulong(\(offset));", "}"], next: [next])
    }
    func indexedOffset(width: Int) -> String {
        let source = reg(field(16, 5))
        let offset: String
        switch field(13, 3) {
        case 2: offset = "ulong(uint(\(source)))"
        case 6: offset = "ulong(long(int(\(source))))"
        default: offset = source
        }
        let scale = field(12, 1) == 1 ? width.trailingZeroBitCount : 0
        return "(\(offset) << \(scale))"
    }
    if bits & 0x3f000000 == 0x1c000000 {
        let width = 4 << field(30, 2)
        let address = pc + signed(field(5, 19), 19) * 4
        guard width <= 16, address >= 0 else { throw GuestError("Invalid SIMD literal load") }
        let logical = "\(address)ul"
        let pointer = physical(logical, width, load: true)
        let upper = width == 16 ? "load_guest_word(memory + offset + 8, 8)" : "0ul"
        let slot = 806 + 2 * Int(rd)
        return emit("{ ulong offset = \(pointer);",
                    "if (\(outside(logical, width, load: true))) { x[34] = 2; return; }",
                    "x[\(slot)] = load_guest_word(memory + offset, \(min(width, 8)));",
                    "x[\(slot + 1)] = \(upper); }")
    }
    let vectorMemory = bits & 0xff800000
    let vectorWidth: [UInt32: Int] = [0x3d800000: 16, 0xfd000000: 8, 0xbd000000: 4]
    let unscaledWidths: [UInt32: Int] = [0x3c800000: 16, 0xfc000000: 8, 0xbc000000: 4]
    let unscaledWidth = field(10, 2) != 2 ? unscaledWidths[bits & 0xffa00000] : nil
    let indexedVectorWidths: [UInt32: Int] = [0x3ca00800: 16, 0xfc200800: 8, 0xbc200800: 4,
                                              0x7c200800: 2, 0x3c200800: 1]
    let indexedVectorWidth = [2, 3, 6, 7].contains(field(13, 3))
        ? indexedVectorWidths[bits & 0xffa00c00] : nil
    let vectorUnscaled = unscaledWidth != nil
    if let width = indexedVectorWidth ?? unscaledWidth ?? vectorWidth[vectorMemory] {
        let load = field(22, 1) == 1
        let base = reg(rn, sp: true)
        let mode = vectorUnscaled ? field(10, 2) : 0
        let displacement = vectorUnscaled ? signed(field(12, 9), 9) : Int(field(10, 12)) * width
        let offset = indexedVectorWidth == nil ? "ulong(\(displacement))" : indexedOffset(width: width)
        let address = mode == 1 ? base : "\(base) + \(offset)"
        let first = 806 + 2 * rd
        let count = min(width, 8)
        let pointer = physical("address", width, load: load)
        let upper = width == 16 ? "load_guest_word(memory + \(pointer) + 8, 8)" : "0"
        let operation = load
            ? "x[\(first)] = load_guest_word(memory + \(pointer), \(count)); x[\(first + 1)] = \(upper);"
            : "store_guest_word(memory + \(pointer), x[\(first)], \(count));" +
                (width == 16 ? " store_guest_word(memory + \(pointer) + 8, x[\(first + 1)], 8);" : "")
        return emit("{ ulong address = \(address);",
                    "if (\(outside("address", width, load: load))) { x[34] = 2; return; }",
                    operation, mode == 0 ? "" : "\(base) += ulong(\(displacement));", "}")
    }
    let pair = bits & 0xffc00000
    let pairEncoding = pair & 0x7fffffff
    if [0x29000000, 0x29400000, 0x29800000, 0x29c00000, 0x28800000, 0x28c00000].contains(pairEncoding),
       field(30, 1) == 0 {
        let wide = field(31, 1) == 1
        let width = wide ? 8 : 4
        let type = wide ? "ulong" : "uint"
        let base = reg(rn, sp: true)
        let second = field(10, 5)
        let load = field(22, 1) == 1
        let mode = field(23, 2)
        guard !load || rd != second, rn == 31 || mode == 2 || (rn != rd && rn != second) else {
            throw GuestError("Unpredictable pair register overlap")
        }
        let offset = signed(field(15, 7), 7) * width
        let address = mode == 1 ? base : "(\(base) + ulong(\(offset)))"
        let bounds = "if (\(outside("address", 2 * width, load: load))) { x[\(guestFaultAddress)] = address; x[34] = 2; return; }"
        func transfer(_ register: UInt32, _ offset: Int) -> String {
            let pointer = "memory + \(physical("address", 2 * width, load: load)) + \(offset)"
            if load { return register == 31 ? "" : "\(reg(register)) = load_guest_word(\(pointer), \(width));" }
            return "store_guest_word(\(pointer), \(type)(\(reg(register))), \(width));"
        }
        let firstAccess = transfer(rd, 0)
        let secondAccess = transfer(second, width)
        return emit("{ ulong address = \(address);", bounds, firstAccess, secondAccess,
                    mode == 2 ? "" : "\(base) += ulong(\(offset));", "}")
    }
    let indexed = bits & 0xffe00c00
    let indexedWidths: [UInt32: Int] = [
        0x38600800: 1, 0x78600800: 2, 0xb8600800: 4, 0xf8600800: 8,
        0x38200800: 1, 0x78200800: 2, 0xb8200800: 4, 0xf8200800: 8,
        0x38a00800: 1, 0x38e00800: 1, 0x78a00800: 2, 0x78e00800: 2, 0xb8a00800: 4,
    ]
    if let width = indexedWidths[indexed], [2, 3, 6, 7].contains(field(13, 3)) {
        let signExtend = field(23, 1) == 1
        let load = signExtend || field(22, 1) == 1
        let address = "(\(reg(rn, sp: true)) + \(indexedOffset(width: width)))"
        let pointer = physical(address, width, load: load)
        let value = "load_guest_word(memory + \(pointer), \(width))"
        let signedType = width == 1 ? "char" : (width == 2 ? "short" : "int")
        let destination = field(22, 1) == 1 ? "uint(int" : "ulong(long"
        let result = signExtend ? "\(destination)(\(signedType)(\(value))))" : value
        let operation = load ? (rd == 31 ? "" : "\(reg(rd)) = \(result);")
            : "store_guest_word(memory + \(pointer), \(reg(rd)), \(width));"
        return emit("if (\(outside(address, width, load: load))) { x[\(guestFaultAddress)] = \(address); x[34] = 2; return; }",
                    operation)
    }
    let unsigned = bits & 0xffc00000
    let post = bits & 0xffe00c00
    let loads: [UInt32: UInt32] = [0x39400000: 1, 0x79400000: 2, 0xb9400000: 4, 0xf9400000: 8]
    let stores: [UInt32: UInt32] = [0x39000000: 1, 0x79000000: 2, 0xb9000000: 4, 0xf9000000: 8]
    let postLoads: [UInt32: UInt32] = [0x38400400: 1, 0x38400c00: 1, 0x78400400: 2, 0x78400c00: 2, 0xb8400400: 4, 0xf8400400: 8, 0xf8400c00: 8]
    let postStores: [UInt32: UInt32] = [0x38000400: 1, 0x38000c00: 1, 0x78000400: 2, 0x78000c00: 2,
                                         0xb8000400: 4, 0xb8000c00: 4, 0xf8000400: 8, 0xf8000c00: 8]
    let unscaledLoads: [UInt32: UInt32] = [0x38400000: 1, 0x78400000: 2, 0xb8400000: 4, 0xf8400000: 8]
    let unscaledStores: [UInt32: UInt32] = [0x38000000: 1, 0x78000000: 2, 0xb8000000: 4, 0xf8000000: 8]
    let signedLoads: [UInt32: UInt32] = [0x39800000: 1, 0x39c00000: 1, 0x79800000: 2, 0x79c00000: 2, 0xb9800000: 4]
    let signedIndexed = field(24, 1) == 0 && field(21, 1) == 0 && field(10, 2) != 2
        ? signedLoads[unsigned | 0x01000000] : nil
    let signedWidth = signedLoads[unsigned] ?? signedIndexed
    if let width = signedWidth ?? loads[unsigned] ?? stores[unsigned] ?? postLoads[post] ?? postStores[post]
        ?? unscaledLoads[post] ?? unscaledStores[post] {
        let load = signedWidth != nil || loads[unsigned] != nil || postLoads[post] != nil || unscaledLoads[post] != nil
        let update = (signedIndexed != nil && field(10, 2) != 0) || postLoads[post] != nil || postStores[post] != nil
        let unscaled = signedIndexed != nil || unscaledLoads[post] != nil || unscaledStores[post] != nil
        guard !update || rn == 31 || rn != rd else { throw GuestError("Unpredictable load/store register overlap") }
        let base = reg(rn, sp: true)
        let displacement = signed(field(12, 9), 9)
        let address = update
            ? (field(11, 1) == 1 ? "(\(base) + ulong(\(displacement)))" : base)
            : (unscaled ? "(\(base) + ulong(\(displacement)))" : "(\(base) + \(field(10, 12) * width)ul)")
        let pointer = physical(address, Int(width), load: load)
        let access = "load_guest_word(memory + \(pointer), \(width))"
        var loaded = access
        if signedWidth != nil {
            let type = width == 1 ? "char" : (width == 2 ? "short" : "int")
            loaded = field(22, 1) == 1 ? "uint(int(\(type)(\(access))))" : "ulong(long(\(type)(\(access))))"
        }
        let operation = load ? (rd == 31 ? "" : "\(reg(rd)) = \(loaded);") : "store_guest_word(memory + \(pointer), \(reg(rd)), \(width));"
        return emit("if (\(outside(address, Int(width), load: load))) { x[\(guestFaultAddress)] = \(address); x[34] = 2; return; }",
                    operation, update ? "\(base) += ulong(\(signed(field(12, 9), 9)));" : "")
    }
    return try decode(bits, at: pc, addressSuffix: "ul")
}

// Guest-only pointer authentication tokens occupy a separate virtual range.
let guestCodeLimit: UInt64 = 1 << 48
let guestTokenTag: UInt64 = 0xa64e_0000_0000_0000
let guestTokenIndexShift = 3
let guestFPCR = 870
let guestFPSR = guestFPCR + 1
let guestTokenCount = guestFPSR + 1
let guestTokenCapacity = guestTokenCount + 1
let guestInitialTokenCapacity = 4096
let guestFaultAddress = guestTokenCapacity + 1
let guestCacheFaultAddress = guestFaultAddress + 1
let guestThreadPointer = guestCacheFaultAddress + 1
let guestCacheFaultWidth = guestThreadPointer + 1
let guestPACGAKey = guestCacheFaultWidth + 1
let guestTokenStart = guestPACGAKey + 2
// Data-A signatures keep the address bits exposed by a packed ISA mask.
let guestDataAddressMask: UInt64 = (1 << 47) - 1
let guestDataTokenMarker: UInt64 = 1 << 55
let guestDataTokenBuckets: UInt64 = 255

func encodeGuestPointer(_ address: UInt64, key: UInt64, index: Int) -> UInt64 {
    if key == 2, address <= guestDataAddressMask {
        let code = UInt64(index) % guestDataTokenBuckets + 1
        return guestDataTokenMarker | (code << 47) | address
    }
    return guestTokenTag | (UInt64(index + 1) << guestTokenIndexShift) | (address & 7)
}

struct ProcessTranslation {
    let image: ExecutableImage
    private var instructions = [Int: Instruction]()
    private(set) var faults = [Int: GuestError]()

    let entry: Int
    var entries: Set<Int> { blockEntries(instructions, entry: entry) }

    init(_ image: ExecutableImage, entry: Int) throws {
        self.image = image
        self.entry = entry
        func checkpoint(_ instruction: Instruction, at pc: Int) -> Instruction {
            guard !image.deferredCache.isEmpty else { return instruction }
            return Instruction(code: instruction.code.map {
                $0.replacingOccurrences(of: "x[34] = 2;",
                    with: "x[33] = \(pc)ul; x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3); x[34] = 2;")
            }, next: instruction.next)
        }
        let entryStub = try image.cacheReader?.stubTarget(at: entry)
        let entrySelector = try image.cacheReader?.selectorStubInstruction(at: entry)
        guard image.isCode(entry) || image.imports[entry] != nil || image.guestStubs[entry] != nil ||
              entryStub != nil || entrySelector != nil else {
            throw GuestError("Invalid translated target at 0x\(String(entry, radix: 16))")
        }
        var pending = [entry]
        while instructions.count < 128, let pc = pending.popLast() {
            if instructions[pc] != nil { continue }
            let importName = image.imports[pc]
            let usesGuestAllocator = importName == "_malloc_size" && pc >= SharedCache.guestBase && image.isCode(pc)
            if let importName, !usesGuestAllocator {
                instructions[pc] = try checkpoint(hostCall(importName, returnAddress: nil, cached: true), at: pc)
                continue
            }
            if let target = image.guestStubs[pc] {
                instructions[pc] = Instruction(code: ["pc = \(target)ul; break;"], next: [target])
                pending.append(target)
                continue
            }
            let word: UInt32
            if image.isCode(pc) {
                word = try image.instruction(at: pc)
            } else if let selector = pc == entry ? entrySelector : try image.cacheReader?.selectorStubInstruction(at: pc) {
                word = selector
            } else {
                let target = pc == entry ? entryStub : try image.cacheReader?.stubTarget(at: pc)
                if let target {
                    instructions[pc] = Instruction(code: ["pc = \(target)ul; break;"], next: [target])
                    pending.append(target)
                } else {
                    instructions[pc] = Instruction(code: ["x[33] = \(pc)ul; x[34] = 3; return;"], next: [])
                }
                continue
            }
            do {
                var instruction = try decodeProcess(word, pc: pc, imports: image.imports, guestStubs: image.guestStubs,
                                                    cached: true)
                if usesGuestAllocator {
                    // The guest's libmalloc owns objects allocated inside guest VM mappings.
                    let host = try hostCall("_malloc_size", returnAddress: nil, cached: true)
                    let vmStart = guestMappingBase + image.cacheBackingEnd
                    let vmEnd = guestMappingBase + image.mappingBackingEnd
                    let outsideVM = "if (x[0] < \(vmStart)ul || x[0] >= \(vmEnd)ul) {"
                    instruction = Instruction(code: [outsideVM] + host.code + ["}"] + instruction.code,
                                              next: instruction.next)
                }
                instructions[pc] = checkpoint(instruction, at: pc)
                pending.append(contentsOf: instruction.next)
            } catch let error as GuestError {
                faults[pc] = error
                instructions[pc] = Instruction(code: ["x[33] = \(pc)ul; x[34] = 5; return;"], next: [])
            }
        }
    }

    func source() -> String {
        let cases = renderBlocks(instructions, entry: entry, addressSuffix: "ul")
    return """
    #include <metal_stdlib>
    using namespace metal;
    // Build IEEE-754 scalar bits with integers, including binary64 on Metal.
    ulong integer_to_float(ulong raw, bool negative, uint fraction, device ulong *state) {
        ulong magnitude = negative ? 0ul - raw : raw;
        if (magnitude == 0) return 0ul;
        uint exponent = 63u - clz(magnitude);
        ulong significand;
        if (exponent <= fraction) {
            significand = magnitude << (fraction - exponent);
        } else {
            uint shift = exponent - fraction;
            significand = magnitude >> shift;
            ulong remainder = magnitude & ((1ul << shift) - 1ul);
            if (remainder != 0) {
                state[\(guestFPSR)] |= 16ul;
                ulong halfway = 1ul << (shift - 1);
                uint mode = uint(state[\(guestFPCR)] >> 22) & 3u;
                bool up = (mode == 1u && !negative) || (mode == 2u && negative)
                       || (mode == 0u && (remainder > halfway || (remainder == halfway && (significand & 1ul))));
                significand += ulong(up);
            }
        }
        uint bias = fraction == 52u ? 1023u : 127u;
        uint sign = fraction == 52u ? 63u : 31u;
        return (ulong(negative) << sign) | ((ulong(exponent + bias - 1u) << fraction) + significand);
    }
    ulong lane_mask(uint width) { return ~0ul >> (64 - width); }
    ulong lane_read(ulong2 source, uint index, uint width) {
        uint bit = index * width;
        return (source[bit >> 6] >> (bit & 63)) & lane_mask(width);
    }
    void lane_add(thread ulong2 &target, uint index, uint width, ulong value) {
        uint bit = index * width;
        target[bit >> 6] |= (value & lane_mask(width)) << (bit & 63);
    }
    ulong2 vector_dup(ulong source, uint width, uint bits) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) lane_add(target, lane, width, source);
        return target;
    }
    ulong2 vector_add(ulong2 left, ulong2 right, uint width, uint bits, bool subtract) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong a = lane_read(left, lane, width), b = lane_read(right, lane, width);
            lane_add(target, lane, width, subtract ? a - b : a + b);
        }
        return target;
    }
    ulong2 vector_mla(ulong2 accumulator, ulong2 left, ulong2 right, uint width, uint bits, bool subtract) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong product = lane_read(left, lane, width) * lane_read(right, lane, width);
            ulong prior = lane_read(accumulator, lane, width);
            lane_add(target, lane, width, subtract ? prior - product : prior + product);
        }
        return target;
    }
    ulong2 vector_long(ulong2 accumulator, ulong2 left, ulong2 right, uint width, uint bits, bool accumulate, bool top, bool subtract) {
        ulong2 target = ulong2(0);
        uint count = 128 / (2 * width);
        uint source_offset = top ? bits / width - count : 0;
        for (uint lane = 0; lane < count; ++lane) {
            ulong a = lane_read(left, lane + source_offset, width);
            ulong b = lane_read(right, lane + source_offset, width);
            ulong value = (accumulate ? lane_read(accumulator, lane, 2 * width) : 0) + (subtract ? 0ul - a * b : a * b);
            lane_add(target, lane, 2 * width, value);
        }
        return target;
    }
    \(guestVectorCryptoMetalSource)
    ulong2 vector_reduce_long(ulong2 source, uint source_width, uint destination_width, uint bits) {
        ulong sum = 0;
        for (uint lane = 0; lane < bits / source_width; ++lane) sum += lane_read(source, lane, source_width);
        ulong2 target = ulong2(0);
        lane_add(target, 0, destination_width, sum);
        return target;
    }
    ulong2 vector_add_long(ulong2 left, ulong2 right, uint width, uint bits, bool top) {
        ulong2 target = ulong2(0);
        uint count = 128 / (2 * width);
        for (uint lane = 0; lane < count; ++lane)
            lane_add(target, lane, 2 * width, lane_read(left, lane + (top ? count : 0), width) + lane_read(right, lane + (top ? count : 0), width));
        return target;
    }
    ulong2 vector_sum(ulong2 source, uint width, uint bits) {
        ulong sum = 0;
        for (uint lane = 0; lane < bits / width; ++lane) sum += lane_read(source, lane, width);
        return ulong2(sum & lane_mask(width), 0ul);
    }
    ulong2 vector_compare(ulong2 left, ulong2 right, uint width, uint bits, bool inclusive, bool signed_lanes, bool equal_only) {
        ulong2 target = ulong2(0);
        ulong sign = signed_lanes ? (1ul << (width - 1)) : 0ul;
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong a = lane_read(left, lane, width) ^ sign;
            ulong b = lane_read(right, lane, width) ^ sign;
            bool matched = equal_only ? a == b : (inclusive ? a >= b : a > b);
            if (matched) lane_add(target, lane, width, lane_mask(width));
        }
        return target;
    }
    ulong2 vector_extreme(ulong2 source, uint width, uint bits, bool minimum, bool signed_lanes) {
        ulong best = lane_read(source, 0, width);
        ulong sign = signed_lanes ? (1ul << (width - 1)) : 0ul;
        for (uint lane = 1; lane < bits / width; ++lane) {
            ulong value = lane_read(source, lane, width);
            if (minimum ? (value ^ sign) < (best ^ sign) : (value ^ sign) > (best ^ sign)) best = value;
        }
        return ulong2(best, 0ul);
    }
    ulong2 vector_shift(ulong2 accumulator, ulong2 source, uint width, uint bits, uint shift, bool left) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            ulong value = lane_read(source, lane, width);
            ulong shifted = shift >= width ? 0 : left ? value << shift : value >> shift;
            lane_add(target, lane, width, lane_read(accumulator, lane, width) + shifted);
        }
        return target;
    }
    ulong2 vector_ushl(ulong2 source, ulong2 shifts, uint width, uint bits) {
        ulong2 target = ulong2(0);
        for (uint lane = 0; lane < bits / width; ++lane) {
            int shift = int(char(lane_read(shifts, lane, width)));
            ulong value = lane_read(source, lane, width);
            if (shift > -int(width) && shift < int(width))
                lane_add(target, lane, width, shift < 0 ? value >> -shift : value << shift);
        }
        return target;
    }
    ulong2 vector_uzp1(ulong2 left, ulong2 right, uint width, uint bits) {
        ulong2 target = ulong2(0);
        uint half_count = bits / width / 2;
        for (uint lane = 0; lane < half_count; ++lane) {
            lane_add(target, lane, width, lane_read(left, 2 * lane, width));
            lane_add(target, lane + half_count, width, lane_read(right, 2 * lane, width));
        }
        return target;
    }
    ulong2 vector_zip1(ulong2 left, ulong2 right, uint width, uint bits) {
        ulong2 target = ulong2(0);
        uint half_count = bits / width / 2;
        for (uint lane = 0; lane < half_count; ++lane) {
            lane_add(target, 2 * lane, width, lane_read(left, lane, width));
            lane_add(target, 2 * lane + 1, width, lane_read(right, lane, width));
        }
        return target;
    }
    uint guest_crc32(uint seed, ulong value, uint width, uint polynomial) {
        for (uint byte = 0; byte < width; ++byte) {
            seed ^= uchar(value >> (8 * byte));
            for (uint bit = 0; bit < 8; ++bit) seed = (seed >> 1) ^ ((0u - (seed & 1)) & polynomial);
        }
        return seed;
    }
    ulong multiply_high(ulong left, ulong right) {
        ulong lo_left = uint(left), lo_right = uint(right);
        ulong hi_left = left >> 32, hi_right = right >> 32;
        ulong carry = ((lo_left * lo_right) >> 32) + hi_left * lo_right;
        ulong middle = uint(carry) + lo_left * hi_right;
        return hi_left * hi_right + (carry >> 32) + (middle >> 32);
    }
    ulong load_guest_word(device const uchar *memory, uint count) {
        if (count == 8 && (ulong(memory) & 7ul) == 0) return *((device const ulong *)memory);
        if (count == 4 && (ulong(memory) & 3ul) == 0) return *((device const uint *)memory);
        ulong word = 0;
        for (uint byte = 0; byte < count; ++byte) word |= ulong(memory[byte]) << (8 * byte);
        return word;
    }
    void store_guest_word(device uchar *memory, ulong word, uint count) {
        if (count == 8 && (ulong(memory) & 7ul) == 0) { *((device ulong *)memory) = word; return; }
        if (count == 4 && (ulong(memory) & 3ul) == 0) { *((device uint *)memory) = uint(word); return; }
        for (uint byte = 0; byte < count; ++byte) memory[byte] = uchar(word >> (8 * byte));
    }
    void guest_move(device uchar *dst, device const uchar *src, ulong count) {
        if (dst > src && ulong(dst - src) < count) {
            for (ulong i = count; i > 0; --i) dst[i - 1] = src[i - 1];
        } else {
            for (ulong i = 0; i < count; ++i) dst[i] = src[i];
        }
    }
    void guest_fill(device uchar *dst, uchar value, ulong count) {
        for (ulong i = 0; i < count; ++i) dst[i] = value;
    }
    void guest_pattern(device uchar *dst, device const uchar *src, ulong count, uint width) {
        uchar pattern[16];
        for (uint i = 0; i < width; ++i) pattern[i] = src[i];
        for (ulong i = 0; i < count; ++i) dst[i] = pattern[i & (width - 1)];
    }
    bool guest_write(ulong address, ulong count, ulong stack) {
        if (address >= stack) return true;
        \(image.writableRegions.filter { !$0.isEmpty }.map { range in
            "if (address >= \(range.lowerBound)ul && address < \(range.upperBound)ul && count <= \(range.upperBound)ul - address) return true;"
        }.joined(separator: "\n"))
        return false;
    }

    ulong guest_offset(ulong address, ulong count, ulong size, ulong writable, bool write,
                       device ulong *state, device const ulong *resident) {
        if (address < \(SharedCache.guestBase)ul) {
            ulong limit = address < \(image.linearImageEnd)ul ? \(image.linearImageEnd)ul : size;
            if ((address < \(ExecutableImage.minimumImageAddress)ul && count != 0) ||
                (address >= \(image.linearImageEnd)ul && address < \(image.mappingBackingEnd)ul) ||
                address > limit || count > limit - address ||
                (write && !guest_write(address, count, writable))) return ~0ul;
            return address;
        }
        for (ulong i = 0; i < resident[0]; ++i) {
            device const ulong *entry = resident + 1 + 4 * i;
            if (address >= entry[0] && address < entry[1]) {
                if ((entry[3] & 2ul) != 0 || (write && (entry[3] & 1ul) == 0)) return ~0ul;
                if (count <= entry[1] - address) return entry[2] + address - entry[0];
            }
        }
        if (state[\(guestCacheFaultAddress)] == ~0ul) {
            state[\(guestCacheFaultAddress)] = address;
            state[\(guestCacheFaultWidth)] = count;
        }
        return ~0ul;
    }
    ulong encode_guest_pointer(ulong address, ulong key, ulong index) {
        if (key == 2 && address <= \(guestDataAddressMask)ul) {
            ulong code = index % \(guestDataTokenBuckets)ul + 1;
            return \(guestDataTokenMarker)ul | (code << 47) | address;
        }
        return \(guestTokenTag)ul | ((index + 1) << \(guestTokenIndexShift)) | (address & 7ul);
    }
    ulong pacga_rotate(ulong value, uint shift) {
        return (value << shift) | (value >> (64 - shift));
    }
    void pacga_round(thread ulong4 &v) {
        v.x += v.y; v.y = pacga_rotate(v.y, 13) ^ v.x; v.x = pacga_rotate(v.x, 32);
        v.z += v.w; v.w = pacga_rotate(v.w, 16) ^ v.z;
        v.x += v.w; v.w = pacga_rotate(v.w, 21) ^ v.x;
        v.z += v.y; v.y = pacga_rotate(v.y, 17) ^ v.z; v.z = pacga_rotate(v.z, 32);
    }
    // SipHash-2-4 supplies a process-local code for the fixed 16-byte input.
    ulong guest_pacga(device const ulong *state, ulong value, ulong modifier) {
        ulong k0 = state[\(guestPACGAKey)], k1 = state[\(guestPACGAKey + 1)];
        ulong4 v = ulong4(k0 ^ 0x736f6d6570736575ul, k1 ^ 0x646f72616e646f6dul,
                          k0 ^ 0x6c7967656e657261ul, k1 ^ 0x7465646279746573ul);
        ulong3 words = ulong3(value, modifier, 16ul << 56);
        for (uint i = 0; i < 3; ++i) {
            v.w ^= words[i];
            pacga_round(v); pacga_round(v);
            v.x ^= words[i];
        }
        v.z ^= 0xfful;
        for (uint i = 0; i < 4; ++i) pacga_round(v);
        return (v.x ^ v.y ^ v.z ^ v.w) & 0xffffffff00000000ul;
    }
    ulong sign_guest_pointer(device ulong *state, ulong address, ulong modifier, ulong key) {
        ulong count = state[\(guestTokenCount)];
        for (ulong index = 0; index < count; ++index) {
            device ulong *token = state + \(guestTokenStart) + index * 3;
            if (token[0] == address && token[1] == modifier && token[2] == key)
                return encode_guest_pointer(address, key, index);
        }
        if (count >= state[\(guestTokenCapacity)]) return 0;
        device ulong *token = state + \(guestTokenStart) + count * 3;
        token[0] = address; token[1] = modifier; token[2] = key;
        state[\(guestTokenCount)] = count + 1;
        return encode_guest_pointer(address, key, count);
    }
    device ulong *guest_pointer_token(device ulong *state, ulong pointer) {
        ulong tag = pointer & ~0xfffffffful;
        ulong index = (pointer & 0xfffffffful) >> \(guestTokenIndexShift);
        if (tag != \(guestTokenTag)ul ||
            index == 0 || index > state[\(guestTokenCount)])
            return nullptr;
        device ulong *token = state + \(guestTokenStart) + (index - 1) * 3;
        return encode_guest_pointer(token[0], token[2], index - 1) == pointer ? token : nullptr;
    }
    device ulong *guest_data_token(device ulong *state, ulong pointer, ulong modifier, bool authenticate) {
        ulong bucket = (pointer >> 47) & 0xfful;
        ulong address = pointer & \(guestDataAddressMask)ul;
        ulong upper = pointer >> 55;
        if (!bucket || upper > 1ul) return nullptr;
        for (ulong index = bucket - 1; index < state[\(guestTokenCount)]; index += \(guestDataTokenBuckets)ul) {
            device ulong *token = state + \(guestTokenStart) + index * 3;
            if (token[2] == 2 && token[0] == address &&
                (!upper || encode_guest_pointer(address, 2, index) == pointer) &&
                (!authenticate || token[1] == modifier)) return token;
        }
        return nullptr;
    }
    ulong strip_guest_pointer(device ulong *state, ulong pointer) {
        device ulong *token = guest_pointer_token(state, pointer);
        if (!token) token = guest_data_token(state, pointer, 0ul, false);
        return token ? token[0] : pointer;
    }
    ulong authenticate_guest_pointer(device ulong *state, ulong pointer, ulong modifier, ulong key) {
        device ulong *token = guest_pointer_token(state, pointer);
        if (key == 2 && (!token || token[2] != key)) token = guest_data_token(state, pointer, modifier, true);
        return token && token[1] == modifier && token[2] == key ? token[0] : ~0ul;
    }
    kernel void run_process(device uchar *memory [[buffer(0)]],
                            device ulong *x [[buffer(1)]],
                            constant ulong &size [[buffer(2)]],
                            constant ulong &writable [[buffer(3)]],
                            device const ulong *resident [[buffer(4)]]) {
        // Preserve the guest dispatch variable across switch cases on Metal.
        volatile ulong pc = x[33];
        bool z = (x[32] & 1ul) != 0;
        bool n = (x[32] & 2ul) != 0;
        bool v = (x[32] & 4ul) != 0;
        bool c = (x[32] & 8ul) != 0;
        for (uint blocks = 0; x[36] > 0 && blocks < 8192; ++blocks) {
            --x[36];
            switch (pc) {
    \(cases)
            default:
                x[33] = pc;
                x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);
                x[34] = 7;
                return;
            }
        }
        x[33] = pc;
        x[32] = uint(z) | (uint(n) << 1) | (uint(v) << 2) | (uint(c) << 3);
        x[34] = x[36] == 0 ? 1 : 8;
    }
    """
    }
}
