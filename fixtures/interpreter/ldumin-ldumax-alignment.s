.text
.globl _main
_main:
    sub sp, sp, #128
    mov w15, #42

    // LDUMIN W on an unaligned address contained in one 16-byte granule.
    add x14, sp, #1
    mov w9, #-1
    str w9, [x14]
    mov w10, #0x80000000
    ldumin w10, w11, [x14]
    cmp w11, w9
    b.ne .Lfail
    ldr w13, [x14]
    cmp w13, w10
    b.ne .Lfail

    // LDUMAX W on another unaligned address in the same granule.
    add x14, sp, #9
    mov w9, #1
    str w9, [x14]
    mov w10, #2
    ldumax w10, w11, [x14]
    cmp w11, w9
    b.ne .Lfail
    ldr w13, [x14]
    cmp w13, w10
    b.ne .Lfail

    // LDSMIN W on an unaligned address contained in one 16-byte granule.
    add x14, sp, #5
    mov w9, #0x7fffffff
    str w9, [x14]
    mov w10, #0x80000000
    ldsmin w10, w11, [x14]
    cmp w11, w9
    b.ne .Lfail
    ldr w13, [x14]
    cmp w13, w10
    b.ne .Lfail

    // LDSMAX W on an unaligned address contained in one granule.
    add x14, sp, #9
    mov w9, #1
    str w9, [x14]
    mov w10, #2
    ldsmax w10, w11, [x14]
    cmp w11, w9
    b.ne .Lfail
    ldr w13, [x14]
    cmp w13, w10
    b.ne .Lfail

    // LDUMAX X on an unaligned address contained in one 16-byte granule.
    add x14, sp, #1
    mov x9, #0x8000000000000000
    str x9, [x14]
    mov x10, #0x7fffffffffffffff
    ldumax x10, x11, [x14]
    cmp x11, x9
    b.ne .Lfail
    ldr x13, [x14]
    cmp x13, x9
    b.ne .Lfail

    // LDUMIN X on an unaligned address contained in one granule.
    add x14, sp, #5
    mov x9, #-1
    str x9, [x14]
    mov x10, #0x8000000000000000
    ldumin x10, x11, [x14]
    cmp x11, x9
    b.ne .Lfail
    ldr x13, [x14]
    cmp x13, x10
    b.ne .Lfail

    // LDSMAX X on an unaligned address contained in one 16-byte granule.
    add x14, sp, #1
    mov x9, #0x8000000000000000
    str x9, [x14]
    mov x10, #0x7fffffffffffffff
    ldsmax x10, x11, [x14]
    cmp x11, x9
    b.ne .Lfail
    ldr x13, [x14]
    cmp x13, x10
    b.ne .Lfail

    // LDSMIN X on an unaligned address contained in one granule.
    add x14, sp, #1
    mov x9, #0x7fffffffffffffff
    str x9, [x14]
    mov x10, #0x8000000000000000
    ldsmin x10, x11, [x14]
    cmp x11, x9
    b.ne .Lfail
    ldr x13, [x14]
    cmp x13, x10
    b.ne .Lfail

    add sp, sp, #128
    mov x0, x15
    ret
.Lfail:
    add sp, sp, #128
    mov x0, #43
    ret
