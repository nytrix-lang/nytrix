use std.core

mut m = [
   4, 4,
   1.0, 2.0, 3.0, 4.0,
   5.0, 6.0, 7.0, 8.0,
   9.0, 10.0, 11.0, 12.0,
   13.0, 14.0, 15.0, 16.0
]

def mat_buf = malloc(64)
assert(__mat4_to_buffer(m, mat_buf) == mat_buf, "__mat4_to_buffer returns buffer")
assert(load32(mat_buf, 0) == 0x3f800000, "__mat4_to_buffer first cell")
assert(load32(mat_buf, 60) == 0x41800000, "__mat4_to_buffer last cell")
mut loaded = [
   4, 4,
   0.0, 0.0, 0.0, 0.0,
   0.0, 0.0, 0.0, 0.0,
   0.0, 0.0, 0.0, 0.0,
   0.0, 0.0, 0.0, 0.0
]

assert(__mat4_from_buffer(loaded, mat_buf) == loaded, "__mat4_from_buffer returns matrix")
assert(loaded[2] == 1.0, "__mat4_from_buffer first cell")
assert(loaded[17] == 16.0, "__mat4_from_buffer last cell")
free(mat_buf)
def in_ptr = malloc(8)
store32(in_ptr, 0x00000000, 0)
store32(in_ptr, 0x3f800000, 4)
def out_ptr = malloc(24)
store32(out_ptr, 0x00000000, 0)
store32(out_ptr, 0x00000000, 4)
store32(out_ptr, 0x00000000, 8)
store32(out_ptr, 0x40000000, 12)
store32(out_ptr, 0x40800000, 16)
store32(out_ptr, 0x40c00000, 20)
mut rec = [0, 1, in_ptr, out_ptr, 2, 4, 12, 3, 0]
def interp = __gltf_anim_fast_value_raw(rec, 0.5)
assert(interp == [1.0, 2.0, 3.0], "__gltf_anim_fast_value_raw interpolates vec3")
assert(rec[8] == 0, "__gltf_anim_fast_value_raw updates bracket cache")
free(in_ptr)
free(out_ptr)
print("✓ runtime graphics tests passed")
