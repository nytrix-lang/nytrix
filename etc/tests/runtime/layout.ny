use std.core *

layout Point {
   x: i32,
   y: i32
}

layout Mixed {
   a: u8,
   b: u32,
   c: u16
}

layout Vec4 {
   x: f32,
   y: f32,
   z: f32,
   w: f32
}

layout Vec2 {
   x: f32,
   y: f32
}

layout Nested {
   a: Vec2,
   b: u8
}

layout Wide {
   a: u8,
   b: f128,
   c: u8
}

layout Packed pack(1) {
   a: u8,
   b: u32
}

layout Aligned align(16) {
   a: u8
}

layout FieldAligned {
   a: u8,
   b: u8 align(8)
}

struct Pair {
   a: i64,
   b: u8
}

struct Tail {
   a: u8,
   b: i64,
   c: u8
}

assert(__layout_size("Point") == 8, "Point size")
assert(__layout_align("Point") == 4, "Point align")
assert(__layout_offset("Point", "x") == 0, "Point x offset")
assert(__layout_offset("Point", "y") == 4, "Point y offset")

assert(__layout_size("Mixed") == 12, "Mixed size")
assert(__layout_align("Mixed") == 4, "Mixed align")
assert(__layout_offset("Mixed", "a") == 0, "Mixed a offset")
assert(__layout_offset("Mixed", "b") == 4, "Mixed b offset")
assert(__layout_offset("Mixed", "c") == 8, "Mixed c offset")

assert(__layout_size("Vec4") == 16, "Vec4 size")
assert(__layout_align("Vec4") == 4, "Vec4 align")
assert(__layout_offset("Vec4", "w") == 12, "Vec4 w offset")

assert(__layout_size("Vec2") == 8, "Vec2 size")
assert(__layout_align("Vec2") == 4, "Vec2 align")
assert(__layout_offset("Vec2", "y") == 4, "Vec2 y offset")

assert(__layout_size("Nested") == 12, "Nested size")
assert(__layout_align("Nested") == 4, "Nested align")
assert(__layout_offset("Nested", "b") == 8, "Nested b offset")

assert(__layout_size("Wide") == 48, "Wide size")
assert(__layout_align("Wide") == 16, "Wide align")
assert(__layout_offset("Wide", "b") == 16, "Wide b offset")
assert(__layout_offset("Wide", "c") == 32, "Wide c offset")

assert(__layout_size("Packed") == 5, "Packed size")
assert(__layout_align("Packed") == 1, "Packed align")
assert(__layout_offset("Packed", "b") == 1, "Packed b offset")

assert(__layout_size("Aligned") == 16, "Aligned size")
assert(__layout_align("Aligned") == 16, "Aligned align")
assert(__layout_offset("Aligned", "a") == 0, "Aligned a offset")

assert(__layout_size("FieldAligned") == 16, "FieldAligned size")
assert(__layout_align("FieldAligned") == 8, "FieldAligned align")
assert(__layout_offset("FieldAligned", "b") == 8, "FieldAligned b offset")

assert(__layout_size("Pair") == 16, "Pair size")
assert(__layout_align("Pair") == 8, "Pair align")
assert(__layout_offset("Pair", "b") == 8, "Pair b offset")

assert(__layout_size("Tail") == 24, "Tail size")
assert(__layout_align("Tail") == 8, "Tail align")
assert(__layout_offset("Tail", "b") == 8, "Tail b offset")
assert(__layout_offset("Tail", "c") == 16, "Tail c offset")

print("✓ all runtime layout tests passed")
