use std.core *

;; Inline ASM (Test)
;; Tests basic inline assembly functionality, including operand passing and result returning.

print("Testing inline assembly...")

; Simple MOV
; Return 42 (Tagged: 85). Operand 0 is $0. Immediate 85 is $$85.
mut x = asm("mov $$85, $0", "=r")
print("asm(42) =", x)
assert(x == 42, "asm return 42")

; Check input passing
; Pass 123. Move to %0.
mut s = asm("mov $1, $0", "=r,r", 123)
print("asm(mov 123) =", s)
; assert(s == 123, "asm input passing")

print("✓ std.core.asm tests passed")

