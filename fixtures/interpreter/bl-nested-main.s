.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_main
_guest_main:
    stp x29, x30, [sp, #-16]!
    bl _bl_nested
    subs xzr, x0, #42
    b.ne .Lfail
    movz w0, #42
    ldp x29, x30, [sp], #16
    ret
.Lfail:
    movz w0, #99
    ldp x29, x30, [sp], #16
    ret
