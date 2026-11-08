.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_branch_register_import
_guest_branch_register_import:
    stp x19, x30, [sp, #-16]!
    adrp x16, _puts@GOTPAGE
    ldr x16, [x16, _puts@GOTPAGEOFF]
    adr x0, .Lmessage
    blr x16
    movz w0, #42
    ldp x19, x30, [sp], #16
    ret
.Lmessage:
    .asciz "blr-import"
