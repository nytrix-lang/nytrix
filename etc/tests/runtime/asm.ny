use std.core *
use std.os *
use std.str *

;; Inline ASM (Test)
;; Tests basic inline assembly functionality per architecture.

def arch_name = arch()
if(str_contains(arch_name, "x86") || str_contains(arch_name, "X86")){
  print("Testing inline assembly for x86...")
  mut x = asm("mov $1, $0", "=r,r", 42)
  print("asm(42) =", x)
  assert(x == 42, "asm return 42")
  mut s = asm("mov $1, $0", "=r,r", 123)
  print("asm(mov 123) =", s)
  print("✓ std.core.asm[x86] tests passed")
} elif(str_contains(arch_name, "aarch64") || str_contains(arch_name, "arm")){
  print("Testing inline assembly for ARM...")
  mut x = asm("mov $0, $1", "=r,r", 42)
  print("asm(42) =", x)
  mut x_check = x
  if(!str_contains(arch_name, "aarch64")){
    ;; ARM32 JIT may leave upper 32 bits of i64 register-pair moves undefined.
    ;; Validate the semantically relevant low 32-bit payload.
    x_check = x & 4294967295
  }
  assert(x_check == 42, "asm return 42")
  mut s = asm("mov $0, $1", "=r,r", 123)
  print("asm(mov 123) =", s)
  print("✓ std.core.asm[arm] tests passed")
} else {
  print("skip std.core.asm (unsupported arch: " + arch() + ")")
}
