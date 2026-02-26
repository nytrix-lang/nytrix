;; Keywords: math quaternion quat
;; Quaternion mathematics module.

module std.math.quat (
   quat, quat_identity,
   quat_mul, quat_dot, quat_norm,
   quat_slerp, quat_to_mat4,
   mul
)

use std.core *
use std.math *

fn quat(x=0, y=0, z=0, w=1){
   "Creates a quaternion [x, y, z, w]."
   def q = list(4)
   store_item(q, 0, x)
   store_item(q, 1, y)
   store_item(q, 2, z)
   store_item(q, 3, w)
   store64(q, 4, 0)
   q
}

fn quat_identity(){
   "Creates an identity quaternion [0, 0, 0, 1]."
   quat(0, 0, 0, 1)
}

fn quat_dot(a, b){
   "Returns the dot product of quaternions `a` and `b`."
   get(a,0)*get(b,0) + get(a,1)*get(b,1) + get(a,2)*get(b,2) + get(a,3)*get(b,3)
}

fn quat_norm(q){
   "Returns the normalized version of quaternion `q`."
   def d2 = quat_dot(q, q)
   if(d2 == 0){ return quat_identity() }
   def inv_l = 1 / sqrt(d2)
   quat(get(q,0)*inv_l, get(q,1)*inv_l, get(q,2)*inv_l, get(q,3)*inv_l)
}

fn quat_mul(a, b){
   "Multiplies two quaternions `a` and `b`."
   def ax = get(a,0) def ay = get(a,1) def az = get(a,2) def aw = get(a,3)
   def bx = get(b,0) def by = get(b,1) def bz = get(b,2) def bw = get(b,3)
   quat(
      (aw * bx) + (ax * bw) + (ay * bz) - (az * by),
      (aw * by) - (ax * bz) + (ay * bw) + (az * bx),
      (aw * bz) + (ax * by) - (ay * bx) + (az * bw),
      (aw * bw) - (ax * bx) - (ay * by) - (az * bz)
   )
}

fn quat_slerp(a, b, t){
   "Spherical linear interpolation between quaternions `a` and `b` by factor `t`."
   mut cos_theta = quat_dot(a, b)
   mut b_rot = b
   if(cos_theta < 0){
      cos_theta = -cos_theta
      b_rot = quat(-get(b,0), -get(b,1), -get(b,2), -get(b,3))
   }
   if(cos_theta > 0.9995){
      ;; Linear interpolation for very close angles
      def it = 1 - t
      return quat_norm(quat(
         get(a,0)*it + get(b_rot,0)*t,
         get(a,1)*it + get(b_rot,1)*t,
         get(a,2)*it + get(b_rot,2)*t,
         get(a,3)*it + get(b_rot,3)*t
      ))
   } else {
      def theta = acos(cos_theta)
      def sin_theta = sin(theta)
      def mix_a = sin((1 - t) * theta) / sin_theta
      def mix_b = sin(t * theta) / sin_theta
      return quat(
         get(a,0)*mix_a + get(b_rot,0)*mix_b,
         get(a,1)*mix_a + get(b_rot,1)*mix_b,
         get(a,2)*mix_a + get(b_rot,2)*mix_b,
         get(a,3)*mix_a + get(b_rot,3)*mix_b
      )
   }
}

fn quat_to_mat4(q){
   "Converts a quaternion to a 4x4 rotation matrix."
   def nq = quat_norm(q)
   def x = get(nq, 0) def y = get(nq, 1) def z = get(nq, 2) def w = get(nq, 3)
   def xx = x*x def yy = y*y def zz = z*z
   def xy = x*y def xz = x*z def yz = y*z
   def wx = w*x def wy = w*y def wz = w*z
   use std.math.matrix *
   mut m = mat4_identity()
   store_item(m, 0, 1 - 2*(yy + zz))
   store_item(m, 1, 2*(xy + wz))
   store_item(m, 2, 2*(xz - wy))
   store_item(m, 4, 2*(xy - wz))
   store_item(m, 5, 1 - 2*(xx + zz))
   store_item(m, 6, 2*(yz + wx))
   store_item(m, 8, 2*(xz + wy))
   store_item(m, 9, 2*(yz - wx))
   store_item(m, 10, 1 - 2*(xx + yy))
   m
}

fn mul(a, b){
   "Generic multiplication: supports quaternion-quaternion products."
   if(is_list(a) && len(a) == 4 && is_list(b) && len(b) == 4){
      return quat_mul(a, b)
   }
   a * b
}

if(comptime{__main()}){
   use std.math.quat as q
   use std.math.matrix as mat
   
   def q1 = q.quat_identity()
   assert(get(q1, 3) == 1, "quat identity w")
   
   def q2 = q.quat(1, 0, 0, 0) ;; 180 deg around X? 
   ;; No, quat(sin(th/2)*ax, ..., cos(th/2))
   
   def qm = q1 * q2
   assert(get(qm, 0) == 1, "quat mul x")
   
   def m = q.quat_to_mat4(q2)
   assert(mat.mat4_get(m, 0, 0) == 1, "quat to mat 00")
   assert(mat.mat4_get(m, 1, 1) == -1, "quat to mat 11")
   
   print("✓ std.math.quat tests passed")
}
