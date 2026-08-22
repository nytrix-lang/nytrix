;; Regression: layout array fields `[elem, n] name` with literal extents.
;; A [u8, n] field must occupy n bytes (so following fields are offset by n)
;; and the layout size must include the array extent.
use std.core

layout Buffer {
   [u8, 32] data,
   i32 tag
}

def sz = __layout_size("Buffer")
def od = __layout_offset("Buffer", "data")
def ot = __layout_offset("Buffer", "tag")
assert(od == 0, "data at offset 0")
assert(ot == 32, "tag after 32-byte [u8,32] array")
assert(sz >= 36, "size includes the 32-byte array field")

layout Packed pack(1) {
   [u8, 40] bytes,
   u16 count
}

def ps = __layout_size("Packed")
assert(ps >= 42, "packed layout includes 40-byte array + u16")
print("layout-array-field OK sz=" + to_str(sz) + " packed=" + to_str(ps))

;; deftype-param array extent resolved from the param's `= default` literal.
layout Fluid(int n = 16) {
   [u8, n] data,
   i32 tag
}

def fs = __layout_size("Fluid")
def ft = __layout_offset("Fluid", "tag")
assert(ft == 16, "tag after [u8,n] with n=16 deftype default")
assert(fs >= 20, "deftype-default array extent sized")
print("layout array-field OK(incl deftype default)")

;; explicit deftype-layout instantiation at a comptime-int bound.
layout Sized(int n = 8) {
   [u8, n] data,
   i32 tag
}

def ds8 = __layout_size("Sized")
def ds16 = __layout_size("Sized<16>")
def ds32 = __layout_size("Sized<32>")
assert(ds8 == 12, "Sized default n=8 -> 12")
assert(ds16 == 20, "Sized<16> -> 16 + i32 = 20")
assert(ds32 == 36, "Sized<32> -> 32 + i32 = 36")
print("layout array-field OK(incl explicit instantiation)")
