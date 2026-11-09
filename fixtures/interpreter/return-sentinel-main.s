.text
.globl _main
.p2align 2
_main:
    b _objc_msg_send_uncached_tail

_objc_msg_send_uncached_tail:
    pacibsp
    autibsp
    tbz x30, #62, 1f
    brk #0xc471
1:
    mov w0, #42
    ret
