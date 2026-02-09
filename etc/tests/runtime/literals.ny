use std.core *

; Numeric literal suffix coverage

def a = 1i32

def b = 1u32

def c = 1f32

def d = 1f64

def e = 1.25f32

def f = 2.5f128

def g = 0xffu32

def h = 0xf32

assert(is_int(a), "i32 literal is int")
assert(is_int(b), "u32 literal is int")
assert(is_float(c), "f32 literal is float")
assert(is_float(d), "f64 literal is float")
assert(is_float(e), "f32 float literal is float")
assert(is_float(f), "f128 literal is float")

assert(a == 1, "i32 literal value")
assert(b == 1, "u32 literal value")
assert(g == 255, "hex u32 literal")
assert(h == 0xf32, "hex without suffix")

print("✓ all runtime literal tests passed")
