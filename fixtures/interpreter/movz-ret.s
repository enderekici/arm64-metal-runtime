.text
.globl _return_x42
_return_x42:
    movz x0, #42
    ret

.globl _return_w_beef
_return_w_beef:
    movz w0, #0xbeef, lsl #16
    ret
