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
    mat4_ortho, mat4_ortho_into, mat4_perspective, mat4_look_at,
    mat4_to_buffer,
    mat4_perspective_into, mat4_look_at_into,
    mat4_mul_into, mat4_rotate_into, mat4_translate_into,
    mat4_identity_into
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
    [4, 4,
     1.0, 0.0, 0.0, 0.0,
     0.0, 1.0, 0.0, 0.0,
     0.0, 0.0, 1.0, 0.0,
     0.0, 0.0, 0.0, 1.0]
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
   def a00 = get(a,2)  def a01 = get(a,3)  def a02 = get(a,4)  def a03 = get(a,5)
   def a10 = get(a,6)  def a11 = get(a,7)  def a12 = get(a,8)  def a13 = get(a,9)
   def a20 = get(a,10) def a21 = get(a,11) def a22 = get(a,12) def a23 = get(a,13)
   def a30 = get(a,14) def a31 = get(a,15) def a32 = get(a,16) def a33 = get(a,17)
   def b00 = get(b,2)  def b01 = get(b,3)  def b02 = get(b,4)  def b03 = get(b,5)
   def b10 = get(b,6)  def b11 = get(b,7)  def b12 = get(b,8)  def b13 = get(b,9)
   def b20 = get(b,10) def b21 = get(b,11) def b22 = get(b,12) def b23 = get(b,13)
   def b30 = get(b,14) def b31 = get(b,15) def b32 = get(b,16) def b33 = get(b,17)
   [4, 4,
    a00*b00+a01*b10+a02*b20+a03*b30, a00*b01+a01*b11+a02*b21+a03*b31, a00*b02+a01*b12+a02*b22+a03*b32, a00*b03+a01*b13+a02*b23+a03*b33,
    a10*b00+a11*b10+a12*b20+a13*b30, a10*b01+a11*b11+a12*b21+a13*b31, a10*b02+a11*b12+a12*b22+a13*b32, a10*b03+a11*b13+a12*b23+a13*b33,
    a20*b00+a21*b10+a22*b20+a23*b30, a20*b01+a21*b11+a22*b21+a23*b31, a20*b02+a21*b12+a22*b22+a23*b32, a20*b03+a21*b13+a22*b23+a23*b33,
    a30*b00+a31*b10+a32*b20+a33*b30, a30*b01+a31*b11+a32*b21+a33*b31, a30*b02+a31*b12+a32*b22+a33*b32, a30*b03+a31*b13+a32*b23+a33*b33]
}
fn mat4_mul_vec4(m, v){
   "Returns the product of 4x4 matrix `m` and 4D vector `v`."
   def vx = get(v, 0, 0.0) def vy = get(v, 1, 0.0) def vz = get(v, 2, 0.0) def vw = get(v, 3, 0.0)
   [_mat_at(m,0,0)*vx + _mat_at(m,0,1)*vy + _mat_at(m,0,2)*vz + _mat_at(m,0,3)*vw,
    _mat_at(m,1,0)*vx + _mat_at(m,1,1)*vy + _mat_at(m,1,2)*vz + _mat_at(m,1,3)*vw,
    _mat_at(m,2,0)*vx + _mat_at(m,2,1)*vy + _mat_at(m,2,2)*vz + _mat_at(m,2,3)*vw,
    _mat_at(m,3,0)*vx + _mat_at(m,3,1)*vy + _mat_at(m,3,2)*vz + _mat_at(m,3,3)*vw]
}

fn mat4_translate(tx, ty, tz){
   "Returns a 4x4 translation matrix for offsets (tx, ty, tz)."
    [4, 4,
     1.0, 0.0, 0.0, tx,
     0.0, 1.0, 0.0, ty,
     0.0, 0.0, 1.0, tz,
     0.0, 0.0, 0.0, 1.0]
}

fn mat4_scale(sx, sy, sz){
   "Returns a 4x4 scaling matrix with factors (sx, sy, sz)."
    [4, 4,
     sx,  0.0, 0.0, 0.0,
     0.0, sy,  0.0, 0.0,
     0.0, 0.0, sz,  0.0,
     0.0, 0.0, 0.0, 1.0]
}

fn mat4_rotate(angle, axis){
   "Returns a 4x4 rotation matrix for `angle` radians around `axis` vector."
    use std.math.vector *
    def v = normalize(axis)
    def x = get(v, 0, 1.0) def y = get(v, 1, 0.0) def z = get(v, 2, 0.0)
    def s = sin(angle) def c = cos(angle) def oc = 1.0 - c
    [4, 4,
     x*x*oc+c,   x*y*oc-z*s, x*z*oc+y*s, 0.0,
     y*x*oc+z*s, y*y*oc+c,   y*z*oc-x*s, 0.0,
     z*x*oc-y*s, z*y*oc+x*s, z*z*oc+c,   0.0,
     0.0,        0.0,        0.0,        1.0]
}

fn mat4_ortho(l, r, b, t, n, f){
    "Creates a Vulkan-compatible orthographic projection matrix (Z in [0,1])."
    def rl = float(r) - float(l)
    def tb = float(t) - float(b)
    def f_n = float(f) - float(n)
    [4, 4,
     2.0/rl, 0.0,    0.0,        -(float(r)+float(l))/rl,
     0.0,    2.0/tb, 0.0,        -(float(t)+float(b))/tb,
     0.0,    0.0,    -1.0/f_n,   -float(n)/f_n,
     0.0,    0.0,    0.0,        1.0]
}

fn mat4_ortho_into(l, r, b, t, n, f, m){
    "Updates matrix m to a Vulkan-compatible orthographic projection (in-place, Z in [0,1])."
    def rl = float(r) - float(l)
    def tb = float(t) - float(b)
    def f_n = float(f) - float(n)
    store_item(m, 2,  2.0/rl) store_item(m, 3,  0.0) store_item(m, 4,  0.0) store_item(m, 5,  -(float(r)+float(l))/rl)
    store_item(m, 6,  0.0)    store_item(m, 7,  2.0/tb) store_item(m, 8,  0.0) store_item(m, 9,  -(float(t)+float(b))/tb)
    store_item(m, 10, 0.0)    store_item(m, 11, 0.0) store_item(m, 12, -1.0/f_n) store_item(m, 13, -float(n)/f_n)
    store_item(m, 14, 0.0)    store_item(m, 15, 0.0) store_item(m, 16, 0.0) store_item(m, 17, 1.0)
    m
}

fn mat4_perspective(fovy, aspect, near, far){
    "Creates a Vulkan-compatible perspective projection matrix (Y-flipped, Z in [0,1])."
    def f = 1.0 / tan(float(fovy) / 2.0)
    def nf = float(near) - float(far)
    def m00 = f / float(aspect)
    def m11 = -f
    def m22 = float(far) / nf
    def m23 = (float(far) * float(near)) / nf
    def m32 = -1.0
    [4, 4,
     m00, 0.0, 0.0, 0.0,
     0.0, m11, 0.0, 0.0,
     0.0, 0.0, m22, m23,
     0.0, 0.0, m32, 0.0]
}

fn mat4_perspective_into(fovy, aspect, near, far, m){
    "Updates matrix m to be Vulkan-compatible perspective projection (in-place)."
    def f = 1.0 / tan(float(fovy) / 2.0)
    def nf = float(near) - float(far)
    store_item(m, 2,  f / float(aspect)) store_item(m, 3,  0.0) store_item(m, 4,  0.0) store_item(m, 5,  0.0)
    store_item(m, 6,  0.0)             store_item(m, 7,  -f)  store_item(m, 8,  0.0) store_item(m, 9,  0.0)
    store_item(m, 10, 0.0)             store_item(m, 11, 0.0) store_item(m, 12, float(far) / nf) store_item(m, 13, (float(far) * float(near)) / nf)
    store_item(m, 14, 0.0)             store_item(m, 15, 0.0) store_item(m, 16, -1.0)        store_item(m, 17, 0.0)
    m
}

fn mat4_look_at(eye, center, up){
   "Returns a view matrix that points from `eye` towards `center` with `up` direction."
    use std.math.vector *
    def d = v_sub(center, eye)
    def dm = magnitude(d)
    if(dm < 0.000001){ return mat4_identity() }
    def inv_dm = 1.0 / dm
    def fx = get(d, 0, 0.0) * inv_dm
    def fy = get(d, 1, 0.0) * inv_dm
    def fz = get(d, 2, 0.0) * inv_dm
    def ux = get(up, 0, 0.0) def uy = get(up, 1, 0.0) def uz = get(up, 2, 0.0)
    mut sx = fy * uz - fz * uy
    mut sy = fz * ux - fx * uz
    mut sz = fx * uy - fy * ux
    def sm = sqrt(sx*sx + sy*sy + sz*sz)
    if(sm > 0.000001){ def inv_sm = 1.0 / sm  sx = sx * inv_sm  sy = sy * inv_sm  sz = sz * inv_sm }
    def rx = sy * fz - sz * fy
    def ry = sz * fx - sx * fz
    def rz = sx * fy - sy * fx
    def ex = get(eye, 0, 0.0) def ey = get(eye, 1, 0.0) def ez = get(eye, 2, 0.0)
    def tx = -(sx * ex + sy * ey + sz * ez)
    def ty = -(rx * ex + ry * ey + rz * ez)
    def tz = fx * ex + fy * ey + fz * ez
    [4, 4,
     sx,  sy,  sz,  tx,
     rx,  ry,  rz,  ty,
     -fx, -fy, -fz, tz,
     0.0, 0.0, 0.0, 1.0]
}

fn mat4_to_buffer(m, buf){
    "Copies 4x4 matrix elements to a raw memory buffer (Column-Major for GPU)."
    ;; Row-Major list indices: 0,1=r,c; 2..17=data
    ;; We want Column 0: rows 0,1,2,3 -> bytes 0,4,8,12
    store32_f32(buf, float(get(m, 2)),  0)
    store32_f32(buf, float(get(m, 6)),  4)
    store32_f32(buf, float(get(m, 10)), 8)
    store32_f32(buf, float(get(m, 14)), 12)
    ;; Column 1: bytes 16,20,24,28
    store32_f32(buf, float(get(m, 3)),  16)
    store32_f32(buf, float(get(m, 7)),  20)
    store32_f32(buf, float(get(m, 11)), 24)
    store32_f32(buf, float(get(m, 15)), 28)
    ;; Column 2: bytes 32,36,40,44
    store32_f32(buf, float(get(m, 4)),  32)
    store32_f32(buf, float(get(m, 8)),  36)
    store32_f32(buf, float(get(m, 12)), 40)
    store32_f32(buf, float(get(m, 16)), 44)
    ;; Column 3: bytes 48,52,56,60
    store32_f32(buf, float(get(m, 5)),  48)
    store32_f32(buf, float(get(m, 9)),  52)
    store32_f32(buf, float(get(m, 13)), 56)
    store32_f32(buf, float(get(m, 17)), 60)
}

fn mat4_look_at_into(eye, center, up, m){
   "Updates matrix m to a look-at view matrix (inline)."
   def ex = get(eye,0) def ey = get(eye,1) def ez = get(eye,2)
   def cx = get(center,0) def cy = get(center,1) def cz = get(center,2)
   def ux = get(up,0) def uy = get(up,1) def uz = get(up,2)
   def fx = cx - ex def fy = cy - ey def fz = cz - ez
   def fl = sqrt(fx*fx + fy*fy + fz*fz)
   if(fl < 0.0001){ return mat4_identity_into(m) }
   def ifl = 1.0 / fl def fnx = fx*ifl def fny = fy*ifl def fnz = fz*ifl
   ;; Side = fnx x up
   mut sx = fny*uz - fnz*uy mut sy = fnz*ux - fnx*uz mut sz = fnx*uy - fny*ux
   def sl = sqrt(sx*sx + sy*sy + sz*sz)
   if(sl > 0.0001){ def isl = 1.0/sl sx *= isl sy *= isl sz *= isl }
   ;; Up_actual = sx x fnx
   def ux_a = sy*fnz - sz*fny def uy_a = sz*fnx - sx*fnz def uz_a = sx*fny - sy*fnx
   ;; Rows
   store_item(m, 2, sx) store_item(m, 3, sy) store_item(m, 4, sz) store_item(m, 5, -(sx*ex + sy*ey + sz*ez))
   store_item(m, 6, ux_a) store_item(m, 7, uy_a) store_item(m, 8, uz_a) store_item(m, 9, -(ux_a*ex + uy_a*ey + uz_a*ez))
   store_item(m, 10, -fnx) store_item(m, 11, -fny) store_item(m, 12, -fnz) store_item(m, 13, (fnx*ex + fny*ey + fnz*ez))
   store_item(m, 14, 0.0) store_item(m, 15, 0.0) store_item(m, 16, 0.0) store_item(m, 17, 1.0)
   m
}

fn mat4_mul_into(a, b, m){
   "Multiplies matrices a and b, storing the result in m (inline)."
   def a00 = get(a,2) def a01 = get(a,3) def a02 = get(a,4) def a03 = get(a,5)
   def a10 = get(a,6) def a11 = get(a,7) def a12 = get(a,8) def a13 = get(a,9)
   def a20 = get(a,10) def a21 = get(a,11) def a22 = get(a,12) def a23 = get(a,13)
   def a30 = get(a,14) def a31 = get(a,15) def a32 = get(a,16) def a33 = get(a,17)
   def b00 = get(b,2) def b01 = get(b,3) def b02 = get(b,4) def b03 = get(b,5)
   def b10 = get(b,6) def b11 = get(b,7) def b12 = get(b,8) def b13 = get(b,9)
   def b20 = get(b,10) def b21 = get(b,11) def b22 = get(b,12) def b23 = get(b,13)
   def b30 = get(b,14) def b31 = get(b,15) def b32 = get(b,16) def b33 = get(b,17)
   store_item(m, 2,  a00*b00+a01*b10+a02*b20+a03*b30) store_item(m, 3,  a00*b01+a01*b11+a02*b21+a03*b31)
   store_item(m, 4,  a00*b02+a01*b12+a02*b22+a03*b32) store_item(m, 5,  a00*b03+a01*b13+a02*b23+a03*b33)
   store_item(m, 6,  a10*b00+a11*b10+a12*b20+a13*b30) store_item(m, 7,  a10*b01+a11*b11+a12*b21+a13*b31)
   store_item(m, 8,  a10*b02+a11*b12+a12*b22+a13*b32) store_item(m, 9,  a10*b03+a11*b13+a12*b23+a13*b33)
   store_item(m, 10, a20*b00+a21*b10+a22*b20+a23*b30) store_item(m, 11, a20*b01+a21*b11+a22*b21+a23*b31)
   store_item(m, 12, a20*b02+a21*b12+a22*b22+a23*b32) store_item(m, 13, a20*b03+a21*b13+a22*b23+a23*b33)
   store_item(m, 14, a30*b00+a31*b10+a32*b20+a33*b30) store_item(m, 15, a30*b01+a31*b11+a32*b21+a33*b31)
   store_item(m, 16, a30*b02+a31*b12+a32*b22+a33*b32) store_item(m, 17, a30*b03+a31*b13+a32*b23+a33*b33)
   m
}

fn mat4_rotate_into(angle, axis, m){
   "Updates matrix m to be specialized rotation (inline, zero alloc)."
   def ax = get(axis, 0, 0.0) def ay = get(axis, 1, 0.0) def az = get(axis, 2, 0.0)
   def l = sqrt(ax*ax + ay*ay + az*az)
   mut x = ax mut y = ay mut z = az
   if(l > 0.0001){ def il = 1.0/l x *= il y *= il z *= il }
   def s = sin(angle) def c = cos(angle) def oc = 1.0 - c
   store_item(m, 2,  x*x*oc+c)   store_item(m, 3,  x*y*oc-z*s) store_item(m, 4,  x*z*oc+y*s) store_item(m, 5,  0.0)
   store_item(m, 6,  y*x*oc+z*s) store_item(m, 7,  y*y*oc+c)   store_item(m, 8,  y*z*oc-x*s) store_item(m, 9,  0.0)
   store_item(m, 10, z*x*oc-y*s) store_item(m, 11, z*y*oc+x*s) store_item(m, 12, z*z*oc+c)   store_item(m, 13, 0.0)
   store_item(m, 14, 0.0)        store_item(m, 15, 0.0)        store_item(m, 16, 0.0)        store_item(m, 17, 1.0)
   m
}

fn mat4_translate_into(tx, ty, tz, m){
   "Updates matrix m to be a translation matrix (inline)."
   store_item(m, 2, 1.0) store_item(m, 3, 0.0) store_item(m, 4, 0.0) store_item(m, 5, float(tx))
   store_item(m, 6, 0.0) store_item(m, 7, 1.0) store_item(m, 8, 0.0) store_item(m, 9, float(ty))
   store_item(m, 10, 0.0) store_item(m, 11, 0.0) store_item(m, 12, 1.0) store_item(m, 13, float(tz))
   store_item(m, 14, 0.0) store_item(m, 15, 0.0) store_item(m, 16, 0.0) store_item(m, 17, 1.0)
   m
}

fn mat4_identity_into(m){
   "Resets matrix m to identity (inline)."
   store_item(m, 2, 1.0) store_item(m, 3, 0.0) store_item(m, 4, 0.0) store_item(m, 5, 0.0)
   store_item(m, 6, 0.0) store_item(m, 7, 1.0) store_item(m, 8, 0.0) store_item(m, 9, 0.0)
   store_item(m, 10, 0.0) store_item(m, 11, 0.0) store_item(m, 12, 1.0) store_item(m, 13, 0.0)
   store_item(m, 14, 0.0) store_item(m, 15, 0.0) store_item(m, 16, 0.0) store_item(m, 17, 1.0)
   m
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
