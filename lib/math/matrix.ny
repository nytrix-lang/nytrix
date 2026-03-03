;; Keywords: math matrix
;; Column-major 4x4 matrix library — pure Ny, no C runtime deps.
;; Storage: [rows, cols, c0r0, c0r1, c0r2, c0r3, c1r0, ...]
;; mat4 element (row,col) = m[2 + col*4 + row]

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
    mat4_identity_into,
    mat4_from_buffer,
    mat4_inverse, mat4_inverse_into,
    mat4_scale_into,
    mat4_look_at_into_xyz,
    mat4_rotate_x_into, mat4_rotate_y_into, mat4_rotate_z_into
)

use std.core *
use std.math *

;; Generic NxM Matrix

fn matrix(r, c){
   "Creates an `r` by `c` zero matrix."
    def n = r * c
    mut m = list(2 + n)
    __list_set_len(m, 2 + n)
    m[0] = r
    m[1] = c
    mut i = 0
    while(i < n){ m[2 + i] = 0.0 i = i + 1 }
    m
}

fn rows(m){
   "Returns the row count stored in matrix `m`."
   m[0]
}
fn cols(m){
   "Returns the column count stored in matrix `m`."
   m[1]
}

fn at(m, r, c, default=0.0){
   "Returns the matrix element at row `r`, column `c`, or `default` when out of bounds."
    if(r < 0 || r >= m[0] || c < 0 || c >= m[1]){ return default }
    m[2 + c * m[0] + r]
}

fn set(m, r, c, v){
   "Stores `v` at row `r`, column `c` and returns `m`."
    if(r < 0 || r >= m[0] || c < 0 || c >= m[1]){ return m }
    m[2 + c * m[0] + r] = v
    m
}

fn transpose(m){
   "Returns the transpose of a generic matrix."
    def r = m[0] def c = m[1]
    mut out = matrix(c, r)
    mut i = 0 while(i < r){
        mut j = 0 while(j < c){
            out[2 + i*c + j] = m[2 + j*r + i]
            j = j + 1
        }
        i = i + 1
    }
    out
}

fn mul(a, b){
   "Returns the matrix product `a * b` for generic matrices."
    def ar = a[0] def ac = a[1] def br = b[0] def bc = b[1]
    if(ac != br){ return 0 }
    mut out = matrix(ar, bc)
    mut c = 0 while(c < bc){
        mut r = 0 while(r < ar){
            mut sum = 0.0
            mut k = 0 while(k < ac){
                sum = sum + a[2 + k*ar + r] * b[2 + c*br + k]
                k = k + 1
            }
            out[2 + c*ar + r] = sum
            r = r + 1
        }
        c = c + 1
    }
    out
}

fn add(a, b){
   "Returns the element-wise sum of generic matrices `a` and `b`."
    def r = a[0] def c = a[1]
    mut out = matrix(r, c)
    def n = r * c
    mut i = 0 while(i < n){ out[2+i] = a[2+i] + b[2+i] i = i + 1 }
    out
}

fn sub(a, b){
   "Returns the element-wise difference of generic matrices `a` and `b`."
    def r = a[0] def c = a[1]
    mut out = matrix(r, c)
    def n = r * c
    mut i = 0 while(i < n){ out[2+i] = a[2+i] - b[2+i] i = i + 1 }
    out
}

;; mat4 — all direct indexed, zero overhead

fn mat4_zero(){
   "Returns a zero-initialized 4x4 matrix."
   matrix(4, 4)
}

fn mat4_identity(){
   "Returns a 4x4 identity matrix."
    mut m = mat4_zero()
    m[2] = 1.0 m[7] = 1.0 m[12] = 1.0 m[17] = 1.0
    m
}

fn mat4_identity_into(m){
   "Writes the 4x4 identity matrix into `m`."
    mut i = 0 while(i < 16){ m[2+i] = 0.0 i = i + 1 }
    m[2] = 1.0 m[7] = 1.0 m[12] = 1.0 m[17] = 1.0
    m
}

fn mat4_get(m, r, c, default=0.0){
   "Returns the 4x4 matrix element at row `r`, column `c`, or `default` when out of bounds."
    if(r < 0 || r >= 4 || c < 0 || c >= 4){ return default }
    m[2 + c * 4 + r]
}
fn mat4_set(m, r, c, v){
   "Stores `v` at row `r`, column `c` in 4x4 matrix `m` and returns `m`."
   m[2 + c * 4 + r] = v m
}

fn mat4_mul(a, b){
   "Returns the 4x4 matrix product `a * b`."
    mut o = mat4_zero()
    mat4_mul_into(a, b, o)
    o
}

fn mat4_mul_into(a, b, o){
   "Writes the 4x4 matrix product `a * b` into `o`."
    ;; Unrolled column-major multiply
    def a0=a[2]  def a1=a[3]  def a2=a[4]  def a3=a[5]
    def a4=a[6]  def a5=a[7]  def a6=a[8]  def a7=a[9]
    def a8=a[10] def a9=a[11] def a10=a[12] def a11=a[13]
    def a12=a[14] def a13=a[15] def a14=a[16] def a15=a[17]
    ;; col 0
    def b0=b[2] def b1=b[3] def b2=b[4] def b3=b[5]
    o[2]  = a0*b0 + a4*b1 + a8*b2  + a12*b3
    o[3]  = a1*b0 + a5*b1 + a9*b2  + a13*b3
    o[4]  = a2*b0 + a6*b1 + a10*b2 + a14*b3
    o[5]  = a3*b0 + a7*b1 + a11*b2 + a15*b3
    ;; col 1
    def b4=b[6] def b5=b[7] def b6=b[8] def b7=b[9]
    o[6]  = a0*b4 + a4*b5 + a8*b6  + a12*b7
    o[7]  = a1*b4 + a5*b5 + a9*b6  + a13*b7
    o[8]  = a2*b4 + a6*b5 + a10*b6 + a14*b7
    o[9]  = a3*b4 + a7*b5 + a11*b6 + a15*b7
    ;; col 2
    def b8=b[10] def b9=b[11] def b10_=b[12] def b11_=b[13]
    o[10] = a0*b8 + a4*b9 + a8*b10_  + a12*b11_
    o[11] = a1*b8 + a5*b9 + a9*b10_  + a13*b11_
    o[12] = a2*b8 + a6*b9 + a10*b10_ + a14*b11_
    o[13] = a3*b8 + a7*b9 + a11*b10_ + a15*b11_
    ;; col 3
    def b12_=b[14] def b13_=b[15] def b14_=b[16] def b15_=b[17]
    o[14] = a0*b12_ + a4*b13_ + a8*b14_  + a12*b15_
    o[15] = a1*b12_ + a5*b13_ + a9*b14_  + a13*b15_
    o[16] = a2*b12_ + a6*b13_ + a10*b14_ + a14*b15_
    o[17] = a3*b12_ + a7*b13_ + a11*b14_ + a15*b15_
}

fn mat4_to_buffer(m, buf){
   "Writes 16 float components from matrix `m` into raw buffer `buf`."
    if(!is_ptr(buf)){ return buf }
    mut i = 0
    while(i < 16){
        store32_f32(buf, float(m[2 + i]), i * 4)
        i = i + 1
    }
    buf
}
fn mat4_from_buffer(m, buf){
   "Loads 16 float components from raw buffer `buf` into matrix `m`."
    if(!is_ptr(buf)){ return m }
    mut i = 0
    while(i < 16){
        m[2 + i] = load32_f32(buf, i * 4)
        i = i + 1
    }
    m
}

fn mat4_mul_vec4(m, v){
   "Multiplies 4x4 matrix `m` by homogeneous vector `v`."
    def vx=v[0] def vy=v[1] def vz=v[2] def vw=v[3]
    [m[2]*vx  + m[6]*vy  + m[10]*vz + m[14]*vw,
     m[3]*vx  + m[7]*vy  + m[11]*vz + m[15]*vw,
     m[4]*vx  + m[8]*vy  + m[12]*vz + m[16]*vw,
     m[5]*vx  + m[9]*vy  + m[13]*vz + m[17]*vw]
}

fn mat4_add(a, b){
   "Returns the element-wise sum of 4x4 matrices `a` and `b`."
    mut o = mat4_zero()
    mut i = 0 while(i < 16){ o[2+i] = a[2+i] + b[2+i] i = i + 1 }
    o
}

;; Transforms

fn mat4_translate(tx, ty, tz){
   "Returns a translation matrix for offsets `tx`, `ty`, and `tz`."
    mut m = mat4_identity()
    m[14] = float(tx) m[15] = float(ty) m[16] = float(tz)
    m
}

fn mat4_translate_into(tx, ty, tz, m){
   "Writes a translation matrix into `m`."
    mat4_identity_into(m)
    m[14] = float(tx) m[15] = float(ty) m[16] = float(tz)
    m
}

fn mat4_scale(sx, sy, sz){
   "Returns a scaling matrix for factors `sx`, `sy`, and `sz`."
    [4, 4, float(sx), 0.0, 0.0, 0.0, 0.0, float(sy), 0.0, 0.0,
           0.0, 0.0, float(sz), 0.0, 0.0, 0.0, 0.0, 1.0]
}

fn mat4_scale_into(sx, sy, sz, m){
   "Writes a scaling matrix into `m`."
    mat4_identity_into(m)
    m[2] = float(sx) m[7] = float(sy) m[12] = float(sz)
    m
}

fn mat4_rotate(angle, axis){
   "Returns an axis-angle rotation matrix."
    mut m = mat4_zero()
    mat4_rotate_into(angle, axis, m)
    m
}

fn mat4_rotate_into(angle, axis, m){
   "Writes an axis-angle rotation matrix into `m`."
    mut ax=0.0 mut ay=0.0 mut az=0.0
    if(is_list(axis) || is_tuple(axis)){
        def n = len(axis)
        if(n == 5 && is_int(axis[0]) && is_int(axis[1]) && axis[0]==1 && axis[1]==3){
            ax = axis[2] ay = axis[3] az = axis[4]
        } else if(n >= 3){
            ax = axis[0] ay = axis[1] az = axis[2]
        }
    }
    def l = sqrt(ax*ax + ay*ay + az*az)
    mut x=ax mut y=ay mut z=az
    if(l > 0.0001){ def il=1.0/l x *= il y *= il z *= il }
    def s=-sin(angle) def c=cos(angle) def oc=1.0-c
    ;; Right-handed, column-major (OpenGL/Vulkan)
    m[2]  = x*x*oc + c      m[3]  = y*x*oc - z*s  m[4]  = z*x*oc + y*s  m[5]  = 0.0
    m[6]  = x*y*oc + z*s    m[7]  = y*y*oc + c    m[8]  = z*y*oc - x*s  m[9]  = 0.0
    m[10] = x*z*oc - y*s    m[11] = y*z*oc + x*s  m[12] = z*z*oc + c    m[13] = 0.0
    m[14] = 0.0         m[15] = 0.0         m[16] = 0.0         m[17] = 1.0
    m
}

fn mat4_rotate_into(angle, axis, m){
   "Writes an axis-angle rotation matrix into `m`."
    mut ax=0.0 mut ay=0.0 mut az=0.0
    if(is_list(axis) || is_tuple(axis)){
        def n = len(axis)
        if(n == 5 && is_int(axis[0]) && is_int(axis[1]) && axis[0]==1 && axis[1]==3){
            ax = axis[2] ay = axis[3] az = axis[4]
        } else if(n >= 3){
            ax = axis[0] ay = axis[1] az = axis[2]
        }
    }
    def l = sqrt(ax*ax + ay*ay + az*az)
    mut x=ax mut y=ay mut z=az
    if(l > 0.0001){ def il=1.0/l x *= il y *= il z *= il }
    def s=sin(angle) def c=cos(angle) def oc=1.0-c
    ;; Right-handed, column-major (standard Rodrigues)
    m[2]  = x*x*oc + c      m[6]  = x*y*oc + z*s  m[10] = x*z*oc - y*s  m[14] = 0.0
    m[3]  = y*x*oc - z*s    m[7]  = y*y*oc + c    m[11] = y*z*oc + x*s  m[15] = 0.0
    m[4]  = z*x*oc + y*s    m[8]  = z*y*oc - x*s  m[12] = z*z*oc + c    m[16] = 0.0
    m[5]  = 0.0             m[9]  = 0.0           m[13] = 0.0           m[17] = 1.0
    m
}

fn mat4_rotate_x_into(angle, m){
   "Writes an X-axis rotation matrix into `m`."
    def s = sin(angle) def c = cos(angle)
    m[2] = 1.0  m[3] = 0.0  m[4] = 0.0  m[5] = 0.0
    m[6] = 0.0  m[7] = c    m[8] = s    m[9] = 0.0
    m[10]= 0.0  m[11]= -s   m[12]= c    m[13]= 0.0
    m[14]= 0.0  m[15]= 0.0  m[16]= 0.0  m[17]= 1.0
    m
}

fn mat4_rotate_y_into(angle, m){
   "Writes a Y-axis rotation matrix into `m`."
    def s = sin(angle) def c = cos(angle)
    m[2] = c    m[3] = 0.0  m[4] = s    m[5] = 0.0
    m[6] = 0.0  m[7] = 1.0  m[8] = 0.0  m[9] = 0.0
    m[10]= -s   m[11]= 0.0  m[12]= c    m[13]= 0.0
    m[14]= 0.0  m[15]= 0.0  m[16]= 0.0  m[17]= 1.0
    m
}

fn mat4_rotate_z_into(angle, m){
   "Writes a Z-axis rotation matrix into `m`."
    def s = sin(angle) def c = cos(angle)
    m[2] = c    m[3] = s    m[4] = 0.0  m[5] = 0.0
    m[6] = -s   m[7] = c    m[8] = 0.0  m[9] = 0.0
    m[10]= 0.0  m[11]= 0.0  m[12]= 1.0  m[13]= 0.0
    m[14]= 0.0  m[15]= 0.0  m[16]= 0.0  m[17]= 1.0
    m
}

;; Projections — Vulkan NDC: Y down (-f for [1,1]), Z [0,1] range

fn mat4_perspective(fovy, aspect, near, far){
   "Returns a perspective projection matrix in Vulkan clip-space conventions."
    mut m = mat4_zero()
    mat4_perspective_into(fovy, aspect, near, far, m)
    m
}

fn mat4_perspective_into(fovy, aspect, near, far, m){
   "Writes a perspective projection matrix into `m`."
    def f = 1.0 / tan(float(fovy) / 2.0)
    def nf = float(near) - float(far)
    mut i = 0 while(i < 16){ m[2+i] = 0.0 i = i + 1 }
    m[2]  = f / float(aspect)   ;; [0,0]
    m[7]  = f                   ;; [1,1]
    ;; Vulkan Z in [0,1]
    m[12] = float(far) / nf     ;; [2,2]
    m[13] = -1.0                ;; [3,2] = col2,row3
    m[16] = (float(far) * float(near)) / nf   ;; [2,3] = col3,row2
    m[17] = 0.0
    m
}

fn mat4_ortho(l, r, b, t, n, f){
   "Returns an orthographic projection matrix in Vulkan clip-space conventions."
    mut m = mat4_zero()
    mat4_ortho_into(l, r, b, t, n, f, m)
    m
}

fn mat4_ortho_into(l, r, b, t, n, f, m){
   "Writes an orthographic projection matrix into `m`."
    def rl = float(r) - float(l)
    def tb = float(t) - float(b)
    def fn_ = float(f) - float(n)
    mut i = 0 while(i < 16){ m[2+i] = 0.0 i = i + 1 }
    m[2]  = 2.0 / rl                        ;; [0,0]
    m[7]  = 2.0 / tb                        ;; [1,1]
    ;; Vulkan Z in [0,1]
    m[12] = -1.0 / fn_                      ;; [2,2]
    m[14] = -(float(r) + float(l)) / rl     ;; [0,3] = col3,row0
    m[15] = -(float(t) + float(b)) / tb     ;; [1,3] = col3,row1
    m[16] = -float(n) / fn_                 ;; [2,3] = col3,row2
    m[17] = 1.0                             ;; [3,3]
    m
}

;; View

fn mat4_look_at(eye, center, up){
   "Returns a view matrix looking from `eye` toward `center` with `up` direction."
    mut m = mat4_zero()
    mat4_look_at_into(eye, center, up, m)
    m
}

fn mat4_look_at_into_xyz(ex, ey, ez, cx, cy, cz, ux, uy, uz, m){
   "Writes a view matrix into `m` from explicit eye, center, and up coordinates."
    def fx=cx-ex def fy=cy-ey def fz=cz-ez
    def fl = sqrt(fx*fx + fy*fy + fz*fz)
    if(fl < 0.0001){ return mat4_identity_into(m) }
    def ifl=1.0/fl def fnx=fx*ifl def fny=fy*ifl def fnz=fz*ifl
    mut sx=fny*uz-fnz*uy mut sy=fnz*ux-fnx*uz mut sz=fnx*uy-fny*ux
    def sl = sqrt(sx*sx + sy*sy + sz*sz)
    if(sl > 0.0001){ def isl=1.0/sl sx *= isl sy *= isl sz *= isl }
    def ux2=sy*fnz-sz*fny def uy2=sz*fnx-sx*fnz def uz2=sx*fny-sy*fnx
    ;; Column-major: m[2 + col*4 + row]
    ;; Row 0
    m[2]=sx    m[6]=sy    m[10]=sz    m[14]=-(sx*ex + sy*ey + sz*ez)
    ;; Row 1
    m[3]=ux2   m[7]=uy2   m[11]=uz2   m[15]=-(ux2*ex + uy2*ey + uz2*ez)
    ;; Row 2
    m[4]=-fnx  m[8]=-fny  m[12]=-fnz  m[16]=(fnx*ex + fny*ey + fnz*ez)
    ;; Row 3
    m[5]=0.0   m[9]=0.0   m[13]=0.0   m[17]=1.0
    m
}

fn mat4_look_at_into(eye, center, up, m){
   "Writes a view matrix into `m` from vector inputs."
    ;; Fast-path for matrix(1,3) vectors to avoid repeated checks
    mut ex=0.0 mut ey=0.0 mut ez=0.0
    if(is_list(eye) || is_tuple(eye)){
        def n = len(eye)
        if(n == 5 && is_int(eye[0]) && is_int(eye[1]) && eye[0]==1 && eye[1]==3){
            ex = eye[2] ey = eye[3] ez = eye[4]
        } else if(n >= 3){
            ex = eye[0] ey = eye[1] ez = eye[2]
        }
    }
    mut cx=0.0 mut cy=0.0 mut cz=0.0
    if(is_list(center) || is_tuple(center)){
        def n = len(center)
        if(n == 5 && is_int(center[0]) && is_int(center[1]) && center[0]==1 && center[1]==3){
            cx = center[2] cy = center[3] cz = center[4]
        } else if(n >= 3){
            cx = center[0] cy = center[1] cz = center[2]
        }
    }
    mut ux=0.0 mut uy=0.0 mut uz=0.0
    if(is_list(up) || is_tuple(up)){
        def n = len(up)
        if(n == 5 && is_int(up[0]) && is_int(up[1]) && up[0]==1 && up[1]==3){
            ux = up[2] uy = up[3] uz = up[4]
        } else if(n >= 3){
            ux = up[0] uy = up[1] uz = up[2]
        }
    }
    mat4_look_at_into_xyz(ex, ey, ez, cx, cy, cz, ux, uy, uz, m)
}

;; Transpose

fn mat4_transpose(m){
   "Returns the transpose of 4x4 matrix `m`."
    mut o = mat4_zero()
    mut i = 0 while(i < 4){
        mut j = 0 while(j < 4){
            o[2 + j*4 + i] = m[2 + i*4 + j]
            j = j + 1
        }
        i = i + 1
    }
    o
}

;; Inverse (Laplace expansion)

fn mat4_inverse(m){
   "Returns the inverse of 4x4 matrix `m`."
    mut o = mat4_zero()
    mat4_inverse_into(m, o)
    o
}

fn mat4_inverse_into(src, dst){
   "Writes the inverse of `src` into `dst` when it is invertible."
    ;; Column-major: (row,col) at 2+col*4+row
    def m00=src[2]  def m10=src[3]  def m20=src[4]  def m30=src[5]
    def m01=src[6]  def m11=src[7]  def m21=src[8]  def m31=src[9]
    def m02=src[10] def m12=src[11] def m22=src[12] def m32=src[13]
    def m03=src[14] def m13=src[15] def m23=src[16] def m33=src[17]
    def c00 = m00*m11 - m10*m01
    def c01 = m00*m21 - m20*m01
    def c02 = m00*m31 - m30*m01
    def c03 = m10*m21 - m20*m11
    def c04 = m10*m31 - m30*m11
    def c05 = m20*m31 - m30*m21
    def c06 = m02*m13 - m12*m03
    def c07 = m02*m23 - m22*m03
    def c08 = m02*m33 - m32*m03
    def c09 = m12*m23 - m22*m13
    def c10 = m12*m33 - m32*m13
    def c11 = m22*m33 - m32*m23
    def det = c00*c11 - c01*c10 + c02*c09 + c03*c08 - c04*c07 + c05*c06
    if(abs(det) < 1e-12){ return dst }
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

if(comptime{__main()}){
    use std.core *

    fn _eq(a, b, eps=1e-6){
       "Internal helper for approximate floating-point comparisons."
       abs(a - b) <= eps
    }

    def id = mat4_identity()
    assert(_eq(id[2], 1.0), "mat4_identity m00")
    assert(_eq(id[7], 1.0), "mat4_identity m11")
    assert(_eq(id[12], 1.0), "mat4_identity m22")
    assert(_eq(id[17], 1.0), "mat4_identity m33")

    def t = mat4_translate(1, 2, 3)
    assert(_eq(t[14], 1.0), "mat4_translate tx")
    assert(_eq(t[15], 2.0), "mat4_translate ty")
    assert(_eq(t[16], 3.0), "mat4_translate tz")
    assert(_eq(t[17], 1.0), "mat4_translate w")

    def s = mat4_scale(2, 3, 4)
    def st = mat4_mul(s, t)
    assert(_eq(st[14], 2.0), "mat4_mul scale*translate tx")
    assert(_eq(st[15], 6.0), "mat4_mul scale*translate ty")
    assert(_eq(st[16], 12.0), "mat4_mul scale*translate tz")

    def v = [1.0, 2.0, 3.0, 1.0]
    def sv = mat4_mul_vec4(s, v)
    assert(_eq(get(sv, 0), 2.0), "mat4_mul_vec4 x")
    assert(_eq(get(sv, 1), 6.0), "mat4_mul_vec4 y")
    assert(_eq(get(sv, 2), 12.0), "mat4_mul_vec4 z")
    assert(_eq(get(sv, 3), 1.0), "mat4_mul_vec4 w")

    def invs = mat4_inverse(s)
    assert(_eq(invs[2], 0.5), "mat4_inverse sx")
    assert(_eq(invs[7], 1.0/3.0), "mat4_inverse sy")
    assert(_eq(invs[12], 0.25), "mat4_inverse sz")
    assert(_eq(invs[17], 1.0), "mat4_inverse w")

    print("✓ std.math.matrix tests passed")
}
