.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_branch_register_main
_guest_branch_register_main:
    stp x19, x30, [sp, #-16]!
    adr x30, .Lblr_target
    blr x30
    add x0, x0, #35
    ldp x19, x30, [sp], #16
    adr x10, .Lbr_target
    br x10
.Lbr_target:
    ret
.Lblr_target:
    movz x0, #7
    ret
