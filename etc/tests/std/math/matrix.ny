use std.math.matrix as mat
use std.math.vector as vec
use std.core *

;; std.math.matrix (Test)

def I = mat.mat4_identity()
assert(mat.mat4_get(I, 0, 0, 0) == 1, "mat ident 00")
assert(mat.mat4_get(I, 1, 1, 0) == 1, "mat ident 11")
assert(mat.mat4_get(I, 2, 2, 0) == 1, "mat ident 22")
assert(mat.mat4_get(I, 3, 3, 0) == 1, "mat ident 33")
def I2 = I + I
assert(mat.mat4_get(I2, 0, 0, 0) == 2, "mat add 00")
assert(mat.mat4_get(I2, 3, 3, 0) == 2, "mat add 33")

def S = mat.mat4_scale(2, 3, 4)
def T = mat.mat4_translate(10, 20, 30)
def M = mat.mat4_mul(T, S)

def p = vec.vec4(1, 2, 3, 1)
def o = mat.mat4_mul_vec4(M, p)
;; scale then translate: (2,6,12,1) -> (12,26,42,1)
assert(get(o, 0, 0) == 12, "mat mul vec x")
assert(get(o, 1, 0) == 26, "mat mul vec y")
assert(get(o, 2, 0) == 42, "mat mul vec z")
assert(get(o, 3, 0) == 1, "mat mul vec w")

def TT = mat.mat4_transpose(T)
assert(mat.mat4_get(TT, 3, 0, 0) == 10, "mat transpose tx")
assert(mat.mat4_get(TT, 3, 1, 0) == 20, "mat transpose ty")
assert(mat.mat4_get(TT, 3, 2, 0) == 30, "mat transpose tz")

print("✓ std.math.matrix tests passed")
