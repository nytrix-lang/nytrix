;; Keywords: math matrix
;; Matrix mathematics module.
;; Supports arbitrary sized matrices.

module std.math.matrix (
    matrix, rows, cols, at, set,
    transpose, mul, add, sub,
    mat4_zero, mat4_identity,
    mat4_get, mat4_set,
    mat4_transpose,
    mat4_mul, mat4_mul_vec4, mat4_add,
    mat4_translate, mat4_scale, mat4_rotate,
    mat4_ortho, mat4_perspective, mat4_look_at,
    mat4_to_buffer
)

use std.core *
use std.math *

;; Generic Matrix API

fn matrix(r, c){
    "Creates a new `r` x `c` matrix filled with zeros."
    def n = r * c
    mut m = list(2 + n)
    store_item(m, 0, r)
    store_item(m, 1, c)
    mut i = 0
    while(i < n){
        store_item(m, 2 + i, 0)
        i += 1
    }
    store64(m, 2 + n, 0) ; Set Len
    m
}

fn rows(m){ "Returns the number of rows in matrix `m`." get(m, 0) }
fn cols(m){ "Returns the number of columns in matrix `m`." get(m, 1) }

fn _mat_at(m, r, c, default=0){
    if(r < 0 || r >= rows(m) || c < 0 || c >= cols(m)){ return default }
    get(m, 2 + r * cols(m) + c)
}

fn _mat_set(m, r, c, v){
    if(r < 0 || r >= rows(m) || c < 0 || c >= cols(m)){ return m }
    store_item(m, 2 + r * cols(m) + c, v)
    m
}

fn at(m, r, c, default=0){
    "Returns the element at row `r` and column `c`."
    _mat_at(m, r, c, default)
}

fn set(m, r, c, v){
    "Sets the element at row `r` and column `c` to `v`."
    _mat_set(m, r, c, v)
}

fn is_matrix(m){
   "Returns true if `m` is a matrix object with valid row and column counts."
    if(!is_list(m) || len(m) < 2){ return false }
    def r = get(m, 0)
    def c = get(m, 1)
    if(!is_int(r) || !is_int(c)){ return false }
    len(m) == 2 + r * c
}

fn transpose(m){
    "Returns the transpose of matrix `m`."
    def r = rows(m)
    def c = cols(m)
    mut out = matrix(c, r)
    mut i = 0
    while(i < r){
        mut j = 0
        while(j < c){
            _mat_set(out, j, i, _mat_at(m, i, j))
            j += 1
        }
        i += 1
    }
    out
}

fn add(a, b){
    "Matrix addition."
    if(!is_matrix(a) || !is_matrix(b)){ return a + b }
    def r = rows(a)
    def c = cols(a)
    if(r != rows(b) || c != cols(b)){ return 0 }
    mut out = matrix(r, c)
    mut i = 0
    while(i < r * c){
        store_item(out, 2 + i, get(a, 2 + i) + get(b, 2 + i))
        i += 1
    }
    out
}

fn sub(a, b){
    "Matrix subtraction."
    if(!is_matrix(a) || !is_matrix(b)){ return a - b }
    def r = rows(a)
    def c = cols(a)
    if(r != rows(b) || c != cols(b)){ return 0 }
    mut out = matrix(r, c)
    mut i = 0
    while(i < r * c){
        store_item(out, 2 + i, get(a, 2 + i) - get(b, 2 + i))
        i += 1
    }
    out
}

fn mul(a, b){
    "Matrix multiplication (supports matrix-matrix and matrix-vector)."
    if(!is_matrix(a)){ return __mul(a, b) }
    if(is_matrix(b)){
        def r1 = rows(a)
        def c1 = cols(a)
        def r2 = rows(b)
        def c2 = cols(b)
        if(c1 != r2){ return 0 }
        mut out = matrix(r1, c2)
        mut i = 0
        while(i < r1){
            mut j = 0
            while(j < c2){
                mut s = 0
                mut k = 0
                while(k < c1){
                    s = __add(s, __mul(_mat_at(a, i, k), _mat_at(b, k, j)))
                    k += 1
                }
                _mat_set(out, i, j, s)
                j += 1
            }
            i += 1
        }
        return out
    }
    if(is_list(b)){
        def r = rows(a)
        def c = cols(a)
        if(c != len(b)){ return 0 }
        def out = list(r)
        mut i = 0
        while(i < r){
            mut s = 0
            mut k = 0
            while(k < c){
                s = __add(s, __mul(_mat_at(a, i, k), get(b, k)))
                k += 1
            }
            store_item(out, i, s)
            i += 1
        }
        store64(out, r, 0)
        return out
    }
    def r = rows(a)
    def c = cols(a)
    mut out = matrix(r, c)
    mut i = 0
    while(i < r * c){
        store_item(out, 2 + i, __mul(get(a, 2 + i), b))
        i += 1
    }
    out
}

;; Specialized 4x4 API (compat layer)

fn mat4_zero(){
   "Returns a new 4x4 matrix filled with zero values."
   matrix(4, 4)
}

fn mat4_identity(){
   "Returns a new 4x4 identity matrix."
    mut m = mat4_zero()
    _mat_set(m, 0, 0, 1)
    _mat_set(m, 1, 1, 1)
    _mat_set(m, 2, 2, 1)
    _mat_set(m, 3, 3, 1)
    m
}

fn mat4_get(m, r, c, default=0){
   "Returns the element at row `r` and column `c` of a 4x4 matrix."
   _mat_at(m, r, c, default)
}
fn mat4_set(m, r, c, v){
   "Sets the element at row `r` and column `c` of a 4x4 matrix to `v`."
   _mat_set(m, r, c, v)
}
fn mat4_transpose(m){
   "Returns the transpose of a 4x4 matrix."
   transpose(m)
}
fn mat4_add(a, b){
   "Returns the sum of two 4x4 matrices."
   add(a, b)
}
fn mat4_mul(a, b){
   "Returns the product of two 4x4 matrices."
   mul(a, b)
}
fn mat4_mul_vec4(m, v){
   "Returns the product of 4x4 matrix `m` and 4D vector `v`."
   def r = rows(m)
   def c = cols(m)
   if(c != len(v)){ return 0 }
   def out = list(r)
   mut i = 0
   while(i < r){
       mut s = 0
       mut k = 0
       while(k < c){
           s = __add(s, __mul(_mat_at(m, i, k), get(v, k)))
           k += 1
       }
       store_item(out, i, s)
       i += 1
   }
   store64(out, r, 0)
   out
}

fn mat4_translate(tx, ty, tz){
   "Returns a 4x4 translation matrix for offsets (tx, ty, tz)."
    mut m = mat4_identity()
    _mat_set(m, 0, 3, tx)
    _mat_set(m, 1, 3, ty)
    _mat_set(m, 2, 3, tz)
    m
}

fn mat4_scale(sx, sy, sz){
   "Returns a 4x4 scaling matrix with factors (sx, sy, sz)."
    mut m = mat4_zero()
    _mat_set(m, 0, 0, sx)
    _mat_set(m, 1, 1, sy)
    _mat_set(m, 2, 2, sz)
    _mat_set(m, 3, 3, 1)
    m
}

fn mat4_rotate(angle, axis){
   "Returns a 4x4 rotation matrix for `angle` radians around `axis` vector."
    use std.math.vector *
    def v = normalize(axis)
    def x = get(v, 0, 1)
    def y = get(v, 1, 0)
    def z = get(v, 2, 0)
    def s = sin(angle)
    def c = cos(angle)
    def oc = 1 - c
    mut m = mat4_identity()
    _mat_set(m, 0, 0, x * x * oc + c)
    _mat_set(m, 0, 1, x * y * oc - z * s)
    _mat_set(m, 0, 2, x * z * oc + y * s)
    _mat_set(m, 1, 0, y * x * oc + z * s)
    _mat_set(m, 1, 1, y * y * oc + c)
    _mat_set(m, 1, 2, y * z * oc - x * s)
    _mat_set(m, 2, 0, z * x * oc - y * s)
    _mat_set(m, 2, 1, z * y * oc + x * s)
    _mat_set(m, 2, 2, z * z * oc + c)
    m
}

fn mat4_ortho(l, r, b, t, n, f){
    "Creates an orthographic projection matrix."
    def rl = float(r) - float(l)
    def tb = float(t) - float(b)
    def f_n = float(f) - float(n)
    mut m = mat4_zero()
    _mat_set(m, 0, 0, 2.0 / rl)
    _mat_set(m, 1, 1, 2.0 / tb)
    _mat_set(m, 2, 2, -2.0 / f_n)
    _mat_set(m, 3, 3, 1.0)
    _mat_set(m, 0, 3, -(float(r) + float(l)) / rl)
    _mat_set(m, 1, 3, -(float(t) + float(b)) / tb)
    _mat_set(m, 2, 3, -(float(f) + float(n)) / f_n)
    m
}

fn mat4_perspective(fovy, aspect, near, far){
    "Creates a perspective projection matrix."
    def tan_half_fovy = tan(float(fovy) / 2.0)
    mut m = mat4_zero()
    _mat_set(m, 0, 0, 1.0 / (float(aspect) * tan_half_fovy))
    _mat_set(m, 1, 1, 1.0 / tan_half_fovy)
    _mat_set(m, 2, 2, -(float(far) + float(near)) / (float(far) - float(near)))
    _mat_set(m, 2, 3, -1.0)
    _mat_set(m, 3, 2, -(2.0 * float(far) * float(near)) / (float(far) - float(near)))
    m
}

fn mat4_look_at(eye, center, up){
   "Returns a view matrix that points from `eye` towards `center` with `up` direction."
    use std.math.vector *
    def f = normalize(sub(center, eye))
    def s = normalize(cross3(f, up))
    def u = cross3(s, f)
    mut m = mat4_identity()
    _mat_set(m, 0, 0, get(s, 0))
    _mat_set(m, 0, 1, get(s, 1))
    _mat_set(m, 0, 2, get(s, 2))
    _mat_set(m, 1, 0, get(u, 0))
    _mat_set(m, 1, 1, get(u, 1))
    _mat_set(m, 1, 2, get(u, 2))
    _mat_set(m, 2, 0, -get(f, 0))
    _mat_set(m, 2, 1, -get(f, 1))
    _mat_set(m, 2, 2, -get(f, 2))
    _mat_set(m, 3, 0, -dot(s, eye))
    _mat_set(m, 3, 1, -dot(u, eye))
    _mat_set(m, 3, 2, dot(f, eye))
    m
}

fn mat4_to_buffer(m, buf){
    "Copies 4x4 matrix elements to a raw memory buffer (Column-Major for GPU)."
    mut i = 0
    while(i < 4){
        mut j = 0
        while(j < 4){
            store32_f32(buf, _mat_at(m, j, i), (i * 4 + j) * 4)
            j += 1
        }
        i += 1
    }
}

if(comptime{__main()}){
    def m2x3 = matrix(2, 3)
    assert(rows(m2x3) == 2, "rows")
    assert(cols(m2x3) == 3, "cols")

    _mat_set(m2x3, 0, 1, 5)
    assert(_mat_at(m2x3, 0, 1) == 5, "at/set")

    def m3x2 = transpose(m2x3)
    assert(rows(m3x2) == 3, "transpose rows")
    assert(_mat_at(m3x2, 1, 0) == 5, "transpose value")

    def I = mat4_identity()
    assert(_mat_at(I, 0, 0) == 1, "mat4 identity")

    def v = [1, 2, 3, 4]
    def res = mat4_mul_vec4(I, v)
    assert(get(res, 0) == 1, "mat mul vec x")
    assert(get(res, 3) == 4, "mat mul vec w")

    print("✓ std.math.matrix (generic) tests passed")
}
