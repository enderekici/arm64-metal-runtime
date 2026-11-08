.text
.p2align 12
.Lprevious_page:
    .long 0
    .space 4092
.globl _main
_main:
    adrp x1, .Lprevious_page@PAGE
    adrp x2, .Lnext_page@PAGE
    sub x3, x2, x1
    subs xzr, x3, #2, lsl #12
    b.ne .Lfail
    adr x4, .Lsuccess
    adr x5, _main
    subs xzr, x4, x5
    b.eq .Lfail
    adr x6, .Lsuccess
    subs xzr, x4, x6
    b.ne .Lfail
    adr xzr, .Lsuccess
    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
.Lsuccess:
    nop
.p2align 12
.Lnext_page:
    .long 0
