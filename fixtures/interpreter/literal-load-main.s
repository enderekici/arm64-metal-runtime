.text
.p2align 2
.globl _main
_main:
    ldr w1, .Lliteral_w
    movz w2, #0xcdef
    movk w2, #0x89ab, lsl #16
    cmp w1, w2
    b.ne .Lfail

    ldr x1, .Lliteral_x
    movz x2, #0xdef0
    movk x2, #0x9abc, lsl #16
    movk x2, #0x5678, lsl #32
    movk x2, #0x1234, lsl #48
    cmp x1, x2
    b.ne .Lfail

    ldrsw x1, .Lliteral_sw
    movn x2, #41
    cmp x1, x2
    b.ne .Lfail

    mov x0, #42
    ret
.Lfail:
    mov x0, #1
    ret

.p2align 3
.Lliteral_w:
    .long 0x89abcdef
.p2align 3
.Lliteral_x:
    .quad 0x123456789abcdef0
.Lliteral_sw:
    .long -42
