use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.text.io *
use std.text *

;; comptime (Test)

mut v1 = comptime{ return 1 + 2 + 3 }
assert(v1 == 6, "comptime basic")

mut v2 = comptime{ def x = 10 }
assert(v2 == 0, "comptime fallthrough")

mut v3 = comptime{
   mut sum = 0
   mut i = 0
   while(i < 5){
      sum = sum + i
      i = i + 1
   }
   if(sum == 10){ return sum }
   return 0
}
assert(v3 == 10, "comptime control flow")

mut v4 = comptime{
   def inner = comptime{ return 5 }
   return inner * 2
}
assert(v4 == 10, "comptime nested")

print("✓ comptime tests passed")

;; Test basic integer operations in comptime

;; Addition
mut v_add = comptime { return 10 + 20 }
assert(v_add == 30, "comptime add")

;; Subtraction
mut v_sub = comptime { return 50 - 20 }
assert(v_sub == 30, "comptime sub")

;; Multiplication
mut v_mul = comptime { return 6 * 5 }
assert(v_mul == 30, "comptime mul")

;; Division
mut v_div = comptime { return 60 / 2 }
assert(v_div == 30, "comptime div")

;; Modulo
mut v_mod = comptime { return 35 % 32 }
assert(v_mod == 3, "comptime mod")

;; Comparisons
mut v_lt = comptime { return 10 < 20 }
assert(v_lt == true, "comptime lt")

mut v_le = comptime { return 20 <= 20 }
assert(v_le == true, "comptime le")

mut v_gt = comptime { return 30 > 20 }
assert(v_gt == true, "comptime gt")

mut v_ge = comptime { return 20 >= 20 }
assert(v_ge == true, "comptime ge")

;; Division by zero check (should fail comptime evaluation and fallback to runtime or error if strictly comptime,
;; but here we just want to ensure the logic doesn't crash the compiler.
;; The current implementation returns false if r == 0, which means `ny_try_eval_comptime_fast` returns false,
;; so it falls back to JIT or other mechanisms.
;; We won't test the failure case explicitly in this file as we want it to pass.

print("✓ comptime ops tests passed")
