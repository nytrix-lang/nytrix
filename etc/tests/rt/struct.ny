use std.core

struct Vec2 {
   x: i32,
   y: i32
}

struct Mixed {
   flag: bool,
   count: i32,
   total: i64
}

struct PackedPair pack(1){
   left: i32,
   right: i64
}

assert(__layout_size("Vec2") == 8, "Vec2 size")
assert(__layout_align("Vec2") == 4, "Vec2 align")
assert(__layout_offset("Vec2", "x") == 0, "Vec2.x offset")
assert(__layout_offset("Vec2", "y") == 4, "Vec2.y offset")
assert(__layout_size("Mixed") == 16, "Mixed size")
assert(__layout_align("Mixed") == 8, "Mixed align")
assert(__layout_offset("Mixed", "flag") == 0, "Mixed.flag offset")
assert(__layout_offset("Mixed", "count") == 4, "Mixed.count offset")
assert(__layout_offset("Mixed", "total") == 8, "Mixed.total offset")
assert(__layout_size("PackedPair") == 12, "PackedPair size")
assert(__layout_align("PackedPair") == 1, "PackedPair align")
assert(__layout_offset("PackedPair", "left") == 0, "PackedPair.left offset")
assert(__layout_offset("PackedPair", "right") == 4, "PackedPair.right offset")
print("✓ struct runtime test passed")
