use std.core
use std.core.error

; Regression: native concat/f-string formatting previously converted bool
; operands through rt_native_i64_to_cstr, printing the raw 0/1 bits, and
; routed f64 operands through the i64 formatter ("0").  JIT printed the
; spelled value; native printed garbage.  All of these must agree.
assert_eq("x" + (1 < 2), "xtrue")
assert_eq("x" + (1 > 2), "xfalse")
assert_eq("x" + true, "xtrue")
assert_eq("x" + 5, "x5")
assert_eq("x" + 1.5, "x1.5")
assert_eq("x" + "y", "xy")

; f-string parts with bool / int / float / str / nested to_str
assert_eq(f"{1 < 2}", "true")
assert_eq(f"{1 > 2}", "false")
assert_eq(f"n={42}", "n=42")
assert_eq(f"f={1.5}", "f=1.5")
assert_eq(f"{to_str(1 < 2)}", "true")
assert_eq(f"{str(1 < 2)}", "true")

; to_str of a bool-typed local keeps the bool spelling
def i = 3 < 5
assert_eq(to_str(i), "true")
assert_eq(to_str(i), str(i))

; str() on any-typed inputs routes through the formatter dispatch
assert_eq(str(true), "true")
assert_eq(str(false), "false")
assert_eq(str(5), "5")
assert_eq(str(1 < 2), "true")
print("native str concat/bool/float ok")
