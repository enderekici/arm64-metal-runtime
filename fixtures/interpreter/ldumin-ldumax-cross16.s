.text
.globl _main
_main:
    sub sp, sp, #128
    cmp x0, #1
    b.ne .Lsmax

    // A 32-bit unsigned min-max crossing the 16-byte granule must SIGBUS.
    add x14, sp, #15
    mov w9, #-1
    strb w9, [x14]
    strb w9, [x14, #1]
    strb w9, [x14, #2]
    strb w9, [x14, #3]
    mov w10, #0
    ldumin w10, w11, [x14]
    b .Lunexpected

.Lsmax:
    // A 64-bit signed min-max crossing the 16-byte granule must SIGBUS.
    add x14, sp, #12
    mov x9, #1
    strb w9, [x14]
    strb wzr, [x14, #1]
    strb wzr, [x14, #2]
    strb wzr, [x14, #3]
    strb wzr, [x14, #4]
    strb wzr, [x14, #5]
    strb wzr, [x14, #6]
    strb wzr, [x14, #7]
    mov x10, #2
    ldsmax x10, x11, [x14]
    b .Lunexpected

.Lunexpected:
    add sp, sp, #128
    mov x0, #43
    ret
