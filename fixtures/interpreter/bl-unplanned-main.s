.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_unplanned_main
_guest_unplanned_main:
    stp x29, x30, [sp, #-16]!
    bl _bl_unplanned
    ldp x29, x30, [sp], #16
    ret
