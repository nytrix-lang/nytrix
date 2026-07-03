;; Keywords: render matrix linear-algebra os ui
;; Matrix construction and transform operations for renderer cameras and models.
;; References:
;; - std.os.ui.render
module std.os.ui.render.matrix(mat4_zero, mat4_identity, mat4_identity_into, mat4_get, mat4_set, mat4_mul, mat4_mul_into, mat4_to_buffer, mat4_from_buffer, mat4_mul_vec4, mat4_add, mat4_translate, mat4_translate_into, mat4_scale, mat4_scale_into, mat4_rotate, mat4_rotate_into, mat4_rotate_x, mat4_rotate_x_into, mat4_rotate_y, mat4_rotate_y_into, mat4_rotate_z, mat4_rotate_z_into, mat4_perspective, mat4_perspective_into, mat4_ortho, mat4_ortho_into, mat4_look_at, mat4_look_at_xyz, mat4_look_at_into, mat4_look_at_into_xyz, mat4_transpose, mat4_inverse, mat4_inverse_into)
use std.core
use std.core.mem
use std.math
use std.math.simmd as simmd

fn _mat4_zero_raw() list {
   "Returns a zero-initialized 4x4 matrix."
   [4, 4, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
}

fn mat4_zero() list { _mat4_zero_raw() }

fn mat4_identity() list {
   "Returns a 4x4 identity matrix."
   mut m = _mat4_zero_raw()
   m[2] = 1.0 m[7] = 1.0 m[12] = 1.0 m[17] = 1.0
   m
}

fn mat4_identity_into(list m) list {
   "Writes the 4x4 identity matrix into `m`."
   mut i = 0 while i < 16 { m[2 + i] = 0.0 i += 1 }
   m[2] = 1.0 m[7] = 1.0 m[12] = 1.0 m[17] = 1.0
   m
}

fn mat4_get(list m, int r, int c, f64 default=0.0) f64 {
   "Returns the 4x4 matrix element at row `r`, column `c`, or `default` when out of bounds."
   if r < 0 || r >= 4 || c < 0 || c >= 4 { return default }
   m[2 + c * 4 + r]
}

fn mat4_set(list m, int r, int c, f64 v) list {
   "Stores `v` at row `r`, column `c` in 4x4 matrix `m` and returns `m`."
   m[2 + c * 4 + r] = v
   m
}

fn mat4_mul(list a, list b) list {
   "Returns the 4x4 matrix product `a * b`."
   mut o = _mat4_zero_raw()
   mat4_mul_into(a, b, o)
   o
}

fn mat4_mul_into(list a, list b, list o) list {
   "Writes the 4x4 matrix product `a * b` into `o` (column-major lists)."
   simmd.mat4_mul(a, b, o)
}

@inline
fn mat4_to_buffer(list m, ptr buf) list {
   "Writes 16 float components from matrix `m` into raw buffer `buf`."
   __mat4_to_buffer(m, buf)
   m
}

@inline
fn mat4_from_buffer(list m, ptr buf) list {
   "Loads 16 float components from raw buffer `buf` into matrix `m`."
   __mat4_from_buffer(m, buf)
   m
}

@inline
fn mat4_mul_vec4(list m, list v) list {
   "Multiplies 4x4 matrix `m` by homogeneous vector `v`."
   def vx=v[0] def vy=v[1] def vz=v[2] def vw=v[3]
   [m[2]*vx  + m[6]*vy  + m[10]*vz + m[14]*vw,
      m[3]*vx  + m[7]*vy  + m[11]*vz + m[15]*vw,
      m[4]*vx  + m[8]*vy  + m[12]*vz + m[16]*vw,
   m[5]*vx  + m[9]*vy  + m[13]*vz + m[17]*vw]
}

fn mat4_add(list a, list b) list {
   "Returns the element-wise sum of 4x4 matrices `a` and `b`."
   mut o = _mat4_zero_raw()
   mut i = 0 while i < 16 { o[2 + i] = a[2+i] + b[2+i] i += 1 }
   o
}

fn mat4_translate(f64 tx, f64 ty, f64 tz) list {
   "Returns a translation matrix for offsets `tx`, `ty`, and `tz`."
   mut m = mat4_identity()
   m[14] = float(tx) m[15] = float(ty) m[16] = float(tz)
   m
}

fn mat4_translate_into(f64 tx, f64 ty, f64 tz, list m) list {
   "Writes a translation matrix into `m`."
   mat4_identity_into(m)
   m[14] = float(tx) m[15] = float(ty) m[16] = float(tz)
   m
}

fn mat4_scale(f64 sx, f64 sy, f64 sz) list {
   "Returns a scaling matrix for factors `sx`, `sy`, and `sz`."
   mut m = _mat4_zero_raw()
   mat4_scale_into(sx, sy, sz, m)
   m
}

fn mat4_scale_into(f64 sx, f64 sy, f64 sz, list m) list {
   "Writes a scaling matrix into `m`."
   mat4_identity_into(m)
   m[2] = float(sx) m[7] = float(sy) m[12] = float(sz)
   m
}

fn mat4_rotate(f64 angle, any axis) list {
   "Returns an axis-angle rotation matrix."
   mut m = _mat4_zero_raw()
   mat4_rotate_into(angle, axis, m)
   m
}

fn mat4_rotate_into(f64 angle, any axis, list m) list {
   "Writes an axis-angle rotation matrix into `m`."
   mut ax, ay, az = 0.0, 0.0, 0.0
   if is_list(axis) || is_tuple(axis) {
      def n = axis.len
      if n == 5 && is_int(axis[0]) && is_int(axis[1]) && axis[0]==1 && axis[1]==3 {
         ax = axis[2] ay = axis[3] az = axis[4]
      } elif n >= 3 {
         ax, ay, az = axis[0], axis[1], axis[2]
      }
   }
   def l = sqrt(ax*ax + ay*ay + az*az)
   mut x, y, z = ax, ay, az
   if l > 0.0001 { def il=1.0/l x *= il y *= il z *= il }
   def s=sin(angle) def c=cos(angle) def oc=1.0-c
   m[2] = x*x*oc + c   m[6] = x*y*oc + z*s m[10] = x*z*oc - y*s m[14] = 0.0
   m[3] = y*x*oc - z*s m[7] = y*y*oc + c   m[11] = y*z*oc + x*s m[15] = 0.0
   m[4] = z*x*oc + y*s m[8] = z*y*oc - x*s m[12] = z*z*oc + c   m[16] = 0.0
   m[5] = 0.0          m[9] = 0.0           m[13] = 0.0          m[17] = 1.0
   m
}

comptime template _mat4_emit_rotate_wrapper(name, name_into){
   fn ${name}(f64 angle) list {
      mut m = _mat4_zero_raw()
      ${name_into}(angle, m)
      m
   }
}

comptime emit _mat4_emit_rotate_wrapper(mat4_rotate_x, mat4_rotate_x_into)
comptime emit _mat4_emit_rotate_wrapper(mat4_rotate_y, mat4_rotate_y_into)
comptime emit _mat4_emit_rotate_wrapper(mat4_rotate_z, mat4_rotate_z_into)

fn mat4_rotate_x_into(f64 angle, list m) list {
   "Writes an X-axis rotation matrix into `m`."
   def s = sin(angle) def c = cos(angle)
   m[2] = 1.0  m[3] = 0.0  m[4] = 0.0 m[5] = 0.0
   m[6] = 0.0  m[7] = c    m[8] = s   m[9] = 0.0
   m[10] = 0.0 m[11] = -s  m[12] = c  m[13] = 0.0
   m[14] = 0.0 m[15] = 0.0 m[16] = 0.0 m[17] = 1.0
   m
}

fn mat4_rotate_y_into(f64 angle, list m) list {
   "Writes a Y-axis rotation matrix into `m`."
   def s = sin(angle) def c = cos(angle)
   m[2] = c    m[3] = 0.0  m[4] = -s  m[5] = 0.0
   m[6] = 0.0  m[7] = 1.0  m[8] = 0.0 m[9] = 0.0
   m[10] = s   m[11] = 0.0 m[12] = c  m[13] = 0.0
   m[14] = 0.0 m[15] = 0.0 m[16] = 0.0 m[17] = 1.0
   m
}

fn mat4_rotate_z_into(f64 angle, list m) list {
   "Writes a Z-axis rotation matrix into `m`."
   def s = sin(angle) def c = cos(angle)
   m[2] = c    m[3] = s    m[4] = 0.0  m[5] = 0.0
   m[6] = -s   m[7] = c    m[8] = 0.0  m[9] = 0.0
   m[10] = 0.0 m[11] = 0.0 m[12] = 1.0 m[13] = 0.0
   m[14] = 0.0 m[15] = 0.0 m[16] = 0.0 m[17] = 1.0
   m
}

fn mat4_perspective(f64 fovy, f64 aspect, f64 near, f64 far) list {
   "Returns a perspective projection matrix in Vulkan clip-space conventions."
   mut m = _mat4_zero_raw()
   mat4_perspective_into(fovy, aspect, near, far, m)
   m
}

fn mat4_perspective_into(f64 fovy, f64 aspect, f64 near, f64 far, list m) list {
   "Writes a perspective projection matrix into `m`."
   def f = 1.0 / tan(float(fovy) / 2.0)
   def nf = float(near) - float(far)
   mut i = 0 while i < 16 { m[2 + i] = 0.0 i += 1 }
   m[2] = f / float(aspect)
   m[7] = f
   m[12] = float(far) / nf
   m[13] = -1.0
   m[16] = (float(far) * float(near)) / nf
   m[17] = 0.0
   m
}

fn mat4_ortho(f64 l, f64 r, f64 b, f64 t, f64 n, f64 f) list {
   "Returns an orthographic projection matrix in Vulkan clip-space conventions."
   mut m = _mat4_zero_raw()
   mat4_ortho_into(l, r, b, t, n, f, m)
   m
}

fn mat4_ortho_into(f64 l, f64 r, f64 b, f64 t, f64 n, f64 f, list m) list {
   "Writes an orthographic projection matrix into `m`."
   def rl = float(r) - float(l)
   def tb = float(t) - float(b)
   def fn_ = float(f) - float(n)
   mut i = 0 while i < 16 { m[2 + i] = 0.0 i += 1 }
   m[2] = 2.0 / rl
   m[7] = 2.0 / tb
   m[12] = -1.0 / fn_
   m[14] = -(float(r) + float(l)) / rl
   m[15] = -(float(t) + float(b)) / tb
   m[16] = -float(n) / fn_
   m[17] = 1.0
   m
}

fn mat4_look_at(any eye, any center, any up) list {
   "Returns a view matrix looking from `eye` toward `center` with `up` direction."
   mut m = _mat4_zero_raw()
   mat4_look_at_into(eye, center, up, m)
   m
}

fn mat4_look_at_xyz(f64 ex, f64 ey, f64 ez, f64 cx, f64 cy, f64 cz, f64 ux, f64 uy, f64 uz) list {
   "Returns a view matrix from explicit eye, center, and up coordinates."
   mut m = _mat4_zero_raw()
   mat4_look_at_into_xyz(ex, ey, ez, cx, cy, cz, ux, uy, uz, m)
   m
}

fn mat4_look_at_into_xyz(f64 ex, f64 ey, f64 ez, f64 cx, f64 cy, f64 cz, f64 ux, f64 uy, f64 uz, list m) list {
   "Writes a view matrix into `m` from explicit eye, center, and up coordinates."
   def fx=cx-ex def fy=cy-ey def fz=cz-ez
   def fl = sqrt(fx*fx + fy*fy + fz*fz)
   if fl < 0.0001 { return mat4_identity_into(m) }
   def ifl=1.0/fl def fnx=fx*ifl def fny=fy*ifl def fnz=fz*ifl
   mut sx, sy, sz = fny*uz-fnz*uy, fnz*ux-fnx*uz, fnx*uy-fny*ux
   def sl = sqrt(sx*sx + sy*sy + sz*sz)
   if sl > 0.0001 { def isl=1.0/sl sx *= isl sy *= isl sz *= isl }
   def ux2=sy*fnz-sz*fny def uy2=sz*fnx-sx*fnz def uz2=sx*fny-sy*fnx
   m[2] = sx    m[6] = sy    m[10] = sz    m[14] = -(sx*ex + sy*ey + sz*ez)
   m[3] = ux2   m[7] = uy2   m[11] = uz2   m[15] = -(ux2*ex + uy2*ey + uz2*ez)
   m[4] = -fnx  m[8] = -fny  m[12] = -fnz  m[16] = fnx*ex + fny*ey + fnz*ez
   m[5] = 0.0   m[9] = 0.0   m[13] = 0.0   m[17] = 1.0
   m
}

fn mat4_look_at_into(any eye, any center, any up, list m) list {
   "Writes a view matrix into `m` from vector inputs."
   mut ex, ey, ez = 0.0, 0.0, 0.0
   if is_list(eye) || is_tuple(eye) {
      def n = eye.len
      if n == 5 && is_int(eye[0]) && is_int(eye[1]) && eye[0]==1 && eye[1]==3 {
         ex = eye[2] ey = eye[3] ez = eye[4]
      } elif n >= 3 {
         ex, ey, ez = eye[0], eye[1], eye[2]
      }
   }
   mut cx, cy, cz = 0.0, 0.0, 0.0
   if is_list(center) || is_tuple(center) {
      def n = center.len
      if n == 5 && is_int(center[0]) && is_int(center[1]) && center[0]==1 && center[1]==3 {
         cx = center[2] cy = center[3] cz = center[4]
      } elif n >= 3 {
         cx, cy, cz = center[0], center[1], center[2]
      }
   }
   mut ux, uy, uz = 0.0, 0.0, 0.0
   if is_list(up) || is_tuple(up) {
      def n = up.len
      if n == 5 && is_int(up[0]) && is_int(up[1]) && up[0]==1 && up[1]==3 {
         ux = up[2] uy = up[3] uz = up[4]
      } elif n >= 3 {
         ux, uy, uz = up[0], up[1], up[2]
      }
   }
   mat4_look_at_into_xyz(ex, ey, ez, cx, cy, cz, ux, uy, uz, m)
}

fn mat4_transpose(list m) list {
   "Returns the transpose of 4x4 matrix `m`."
   mut o, i = _mat4_zero_raw(), 0 while i < 4 {
      mut j = 0 while j < 4 {
         o[2 + j*4 + i] = m[2 + i*4 + j]
         j += 1
      }
      i += 1
   }
   o
}

fn mat4_inverse(list m) list {
   "Returns the inverse of 4x4 matrix `m`."
   mut o = _mat4_zero_raw()
   mat4_inverse_into(m, o)
   o
}

fn mat4_inverse_into(list src, list dst) list {
   "Writes the inverse of `src` into `dst` when it is invertible."
   def m00=src[2]  def m10=src[3]  def m20=src[4]  def m30=src[5]
   def m01=src[6]  def m11=src[7]  def m21=src[8]  def m31=src[9]
   def m02=src[10] def m12=src[11] def m22=src[12] def m32=src[13]
   def m03=src[14] def m13=src[15] def m23=src[16] def m33=src[17]
   def c00, c01 = m00*m11 - m10*m01, m00*m21 - m20*m01
   def c02, c03 = m00*m31 - m30*m01, m10*m21 - m20*m11
   def c04, c05 = m10*m31 - m30*m11, m20*m31 - m30*m21
   def c06, c07 = m02*m13 - m12*m03, m02*m23 - m22*m03
   def c08, c09 = m02*m33 - m32*m03, m12*m23 - m22*m13
   def c10, c11 = m12*m33 - m32*m13, m22*m33 - m32*m23
   def det = c00*c11 - c01*c10 + c02*c09 + c03*c08 - c04*c07 + c05*c06
   if abs(det) < 1e-12 { return dst }
   def id = 1.0 / det
   dst[2]  = ( m11*c11 - m21*c10 + m31*c09) * id
   dst[3]  = (-m10*c11 + m20*c10 - m30*c09) * id
   dst[4]  = ( m13*c05 - m23*c04 + m33*c03) * id
   dst[5]  = (-m12*c05 + m22*c04 - m32*c03) * id
   dst[6]  = (-m01*c11 + m21*c08 - m31*c07) * id
   dst[7]  = ( m00*c11 - m20*c08 + m30*c07) * id
   dst[8]  = (-m03*c05 + m23*c02 - m33*c01) * id
   dst[9]  = ( m02*c05 - m22*c02 + m32*c01) * id
   dst[10] = ( m01*c10 - m11*c08 + m31*c06) * id
   dst[11] = (-m00*c10 + m10*c08 - m30*c06) * id
   dst[12] = ( m03*c04 - m13*c02 + m33*c00) * id
   dst[13] = (-m02*c04 + m12*c02 - m32*c00) * id
   dst[14] = (-m01*c09 + m11*c07 - m31*c06) * id
   dst[15] = ( m00*c09 - m10*c07 + m30*c06) * id
   dst[16] = (-m03*c03 + m13*c01 - m33*c00) * id
   dst[17] = ( m02*c03 - m12*c01 + m32*c00) * id
   dst
}

#main {
   def zero = mat4_zero()
   assert(zero.len == 18, "mat4 zero shape")
   zero[17] = 1.0
   assert(zero[17] == 1.0, "mat4 raw write capacity")
   def id = mat4_identity()
   assert(id.len == 18 && mat4_get(id, 0, 0) == 1.0 && mat4_get(id, 3, 3) == 1.0, "mat4 identity")
   mat4_set(id, 1, 2, 5.0)
   assert(mat4_get(id, 1, 2) == 5.0 && mat4_get(id, 4, 0, 7.0) == 7.0, "mat4 get set")
   def translated = mat4_translate(2.0, 3.0, 4.0)
   assert(mat4_get(translated, 0, 3) == 2.0 && mat4_get(translated, 2, 3) == 4.0, "mat4 translate")
   def scaled = mat4_scale(2.0, 3.0, 4.0)
   assert(mat4_get(scaled, 0, 0) == 2.0 && mat4_get(scaled, 2, 2) == 4.0, "mat4 scale")
   def prod = mat4_mul(mat4_identity(), translated)
   assert(mat4_get(prod, 0, 3) == 2.0 && mat4_get(prod, 1, 3) == 3.0, "mat4 multiply")
   def vec = mat4_mul_vec4(translated, [1.0, 1.0, 1.0, 1.0])
   assert(vec[0] == 3.0 && vec[1] == 4.0 && vec[2] == 5.0, "mat4 vec4 multiply")
   def buf = malloc(128)
   assert(mat4_to_buffer(translated, buf) == translated, "mat4 buffer store returns matrix")
   def roundtrip = mat4_zero()
   assert(mat4_from_buffer(roundtrip, buf) == roundtrip, "mat4 buffer load returns matrix")
   assert(mat4_get(roundtrip, 0, 3) == 2.0 && mat4_get(roundtrip, 2, 3) == 4.0, "mat4 buffer roundtrip")
   free(buf)
   def inv = mat4_inverse(translated)
   assert(abs(mat4_get(inv, 0, 3) + 2.0) < 0.0001, "mat4 inverse")
   print("✓ std.os.ui.render.matrix self-test passed")
}
