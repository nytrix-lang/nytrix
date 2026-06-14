;; Keywords: quat quaternion math crypto
;; Quaternion mathematics for Nytrix
;; References:
;; - std.math.crypto
module std.math.crypto.quat(quat, quat_identity, quat_mul, quat_dot, quat_norm, quat_slerp, quat_to_mat4, mul)
use std.core
use std.math

fn quat(any x=0, any y=0, any z=0, any w=1) list {
   "Creates a 4-component quaternion [x, y, z, w]. Defaults to identity if no arguments provided."
   def q = list(4)
   q[0] = x
   q[1] = y
   q[2] = z
   q[3] = w
   store64(q, 4, 0)
   q
}

fn quat_identity() list {
   "Returns a new identity quaternion [0, 0, 0, 1] representing no rotation."
   quat(0, 0, 0, 1)
}

fn quat_dot(list a, list b) any {
   "Returns the scalar dot product of quaternions `a` and `b`."
   a.get(0) * b.get(0) + a.get(1) * b.get(1) + a.get(2) * b.get(2) + a.get(3) * b.get(3)
}

fn quat_norm(list q) list {
   "Returns a new normalized(unit-length) quaternion derived from `q`. If `q` has zero length, returns identity."
   def d2 = quat_dot(q, q)
   if d2 == 0 { return quat_identity() }
   def inv_l = 1 / sqrt(d2)
   quat(q.get(0) * inv_l, q.get(1) * inv_l, q.get(2) * inv_l, q.get(3) * inv_l)
}

fn quat_mul(list a, list b) list {
   "Returns the Hamilton product of quaternions `a` and `b` (composing rotations `a` after `b`)."
   def ax, ay, az, aw = a.get(0), a.get(1), a.get(2), a.get(3)
   def bx, by, bz, bw = b.get(0), b.get(1), b.get(2), b.get(3)
   quat(
      (aw * bx) + (ax * bw) + (ay * bz) - (az * by),
      (aw * by) - (ax * bz) + (ay * bw) + (az * bx),
      (aw * bz) + (ax * by) - (ay * bx) + (az * bw),
      (aw * bw) - (ax * bx) - (ay * by) - (az * bz)
   )
}

fn quat_slerp(list a, list b, any t) list {
   "Performs spherical linear interpolation(SLERP) between quaternions `a` and `b` by factor `t` [0, 1]."
   mut cos_theta = quat_dot(a, b)
   mut b_rot = b
   if cos_theta < 0 {
      cos_theta = -cos_theta
      b_rot = quat(-b.get(0), -b.get(1), -b.get(2), -b.get(3))
   }
   if cos_theta > 0.9995 {
      def it = 1 - t
      return quat_norm(quat(
            a.get(0) * it + b_rot.get(0) * t,
            a.get(1) * it + b_rot.get(1) * t,
            a.get(2) * it + b_rot.get(2) * t,
            a.get(3) * it + b_rot.get(3) * t
      ))
   }
   def theta = acos(cos_theta)
   def sin_theta = sin(theta)
   def mix_a = sin((1 - t) * theta) / sin_theta
   def mix_b = sin(t * theta) / sin_theta
   quat(
      a.get(0) * mix_a + b_rot.get(0) * mix_b,
      a.get(1) * mix_a + b_rot.get(1) * mix_b,
      a.get(2) * mix_a + b_rot.get(2) * mix_b,
      a.get(3) * mix_a + b_rot.get(3) * mix_b
   )
}

fn quat_to_mat4(list q) list {
   "Converts quaternion `q` to a 4x4 rotation matrix representation."
   def nq = quat_norm(q)
   def x, y, z, w = nq.get(0), nq.get(1), nq.get(2), nq.get(3)
   def xx, yy, zz = x * x, y * y, z * z
   def xy, xz, yz = x * y, x * z, y * z
   def wx, wy, wz = w * x, w * y, w * z
   use std.math.matrix
   mut m = mat4_identity()
   m[0] = 1 - 2 * (yy + zz)
   m[1] = 2 * (xy + wz)
   m[2] = 2 * (xz - wy)
   m[4] = 2 * (xy - wz)
   m[5] = 1 - 2 * (xx + zz)
   m[6] = 2 * (yz + wx)
   m[8] = 2 * (xz + wy)
   m[9] = 2 * (yz - wx)
   m[10] = 1 - 2 * (xx + yy)
   m
}

fn mul(any a, any b) any {
   "Generic multiplication operator supporting quaternion-quaternion products."
   if is_list(a) && a.len == 4 && is_list(b) && b.len == 4 { return quat_mul(a, b) }
   a * b
}

#main {
   def id = quat_identity()
   assert(id == [0, 0, 0, 1] && quat_dot(id, id) == 1, "quat identity dot")
   assert(quat_norm([0, 0, 0, 0]) == id, "quat zero normalizes to identity")
   def m = quat_to_mat4(quat(1, 0, 0, 0))
   assert(m.len == 16 && m[0] == 1.0 && m[5] == -1.0 && m[10] == -1.0, "quat mat4 axes")
   assert(quat_mul(id, quat(1, 2, 3, 4)) == quat(1, 2, 3, 4), "quat multiply identity")
   print("✓ std.math.crypto.quat self-test passed")
}
