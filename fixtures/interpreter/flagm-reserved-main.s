.text
.p2align 2
.globl _main
_main:
    cmp x0, #3
    b.eq _reserved_axflag
    cmp x0, #1
    b.ne _reserved_setf
.globl _reserved_rmif
_reserved_rmif:
    .long 0xba000410  // RMIF with fixed bit 4 set
.globl _reserved_setf
_reserved_setf:
    .long 0x3a00080c  // SETF8 with fixed bit 0 clear
.globl _reserved_axflag
_reserved_axflag:
    .long 0xd500405e  // adjacent inaccessible system-register write, Rt != 31
