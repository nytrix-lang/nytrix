;; Keywords: 3d gltf glb parse
;; glTF-specific matrix and vector math for column-major transforms.
;; lists with an optional trailing tag, not the generic std matrix shape.
;; References:
;; - std.parse.3d
;; - std.parse
module std.parse.3d.gltf_math(mat4_identity, mat4_mul, node_local_matrix, mat4_apply_pos, mat4_apply_dir, mat4_from_trs, mat4_inverse_affine, mat4_transform_point, mat4_transform_dir, safe_model_mat4)
use std.core
use std.math

fn mat4_identity() list {
   "Runs the mat4 identity operation."
   [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, "mat4", 400]
}

@jit
fn mat4_mul(any a, any b) list {
   "Runs the mat4 mul operation."
   def raw = __gltf_mat4_mul_list(a, b)
   if(is_list(raw)){ return raw }
   mut o, c = [0.0, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0, "mat4", 400], 0
   while(c < 4){
      mut r = 0
      while(r < 4){
         def val = (0.0 + a.get(0 * 4 + r, 0.0)) * (0.0 + b.get(c * 4 + 0, 0.0)) +
         (0.0 + a.get(1 * 4 + r, 0.0)) * (0.0 + b.get(c * 4 + 1, 0.0)) +
         (0.0 + a.get(2 * 4 + r, 0.0)) * (0.0 + b.get(c * 4 + 2, 0.0)) +
         (0.0 + a.get(3 * 4 + r, 0.0)) * (0.0 + b.get(c * 4 + 3, 0.0))
         o[r + c * 4] = val
         r += 1
      }
      c += 1
   }
   o
}

fn node_local_matrix(any node) list {
   "Runs the node local matrix operation."
   if(is_dict(node)){
      def raw_m = node.get("matrix")
      if(is_list(raw_m) && raw_m.len >= 16){
         mut out = [0.0, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0,  0.0, 0.0, 0.0, 0.0, "mat4", 400]
         mut i = 0
         while(i < 16){
            out[i] = 0.0 + raw_m.get(i, (i % 5) == 0 ? 1.0 : 0.0)
            i += 1
         }
         return out
      }
   }
   def t = is_dict(node) ? node.get("translation", [0.0, 0.0, 0.0]) : [0.0, 0.0, 0.0]
   def s = is_dict(node) ? node.get("scale", [1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0]
   def q = is_dict(node) ? node.get("rotation", [0.0, 0.0, 0.0, 1.0]) : [0.0, 0.0, 0.0, 1.0]
   mat4_from_trs(t, q, s)
}

@jit
fn mat4_apply_pos(any m, any x, any y, any z) list {
   "Runs the mat4 apply pos operation."
   [
      (0.0 + m.get(0 * 4 + 0, 1.0)) * x + (0.0 + m.get(1 * 4 + 0, 0.0)) * y + (0.0 + m.get(2 * 4 + 0, 0.0)) * z + (0.0 + m.get(3 * 4 + 0, 0.0)),
      (0.0 + m.get(0 * 4 + 1, 0.0)) * x + (0.0 + m.get(1 * 4 + 1, 1.0)) * y + (0.0 + m.get(2 * 4 + 1, 0.0)) * z + (0.0 + m.get(3 * 4 + 1, 0.0)),
      (0.0 + m.get(0 * 4 + 2, 0.0)) * x + (0.0 + m.get(1 * 4 + 2, 0.0)) * y + (0.0 + m.get(2 * 4 + 2, 1.0)) * z + (0.0 + m.get(3 * 4 + 2, 0.0))
   ]
}

@jit
fn mat4_apply_dir(any m, any x, any y, any z) list {
   "Runs the mat4 apply dir operation."
   [
      (0.0 + m.get(0 * 4 + 0, 1.0)) * x + (0.0 + m.get(1 * 4 + 0, 0.0)) * y + (0.0 + m.get(2 * 4 + 0, 0.0)) * z,
      (0.0 + m.get(0 * 4 + 1, 0.0)) * x + (0.0 + m.get(1 * 4 + 1, 1.0)) * y + (0.0 + m.get(2 * 4 + 1, 0.0)) * z,
      (0.0 + m.get(0 * 4 + 2, 0.0)) * x + (0.0 + m.get(1 * 4 + 2, 0.0)) * y + (0.0 + m.get(2 * 4 + 2, 1.0)) * z
   ]
}

fn mat4_from_trs(any t, any r, any s) list {
   "Builds a tagged column-major mat4 from translation, rotation(quat xyzw) and scale lists."
   def tx = 0.0 + t.get(0, 0.0) def ty = 0.0 + t.get(1, 0.0) def tz = 0.0 + t.get(2, 0.0)
   def sx = 0.0 + s.get(0, 1.0) def sy = 0.0 + s.get(1, 1.0) def sz = 0.0 + s.get(2, 1.0)
   def qx = 0.0 + r.get(0, 0.0) def qy = 0.0 + r.get(1, 0.0) def qz = 0.0 + r.get(2, 0.0) def qw = 0.0 + r.get(3, 1.0)
   def xx = qx*qx def yy = qy*qy def zz = qz*qz
   def xy = qx*qy def xz = qx*qz def yz = qy*qz
   def wx = qw*qx def wy = qw*qy def wz = qw*qz
   [
      (1.0 - 2.0*(yy+zz))*sx, (2.0*(xy+wz))*sx,       (2.0*(xz-wy))*sx,       0.0,
      (2.0*(xy-wz))*sy,       (1.0 - 2.0*(xx+zz))*sy, (2.0*(yz+wx))*sy,       0.0,
      (2.0*(xz+wy))*sz,       (2.0*(yz-wx))*sz,       (1.0 - 2.0*(xx+yy))*sz, 0.0,
   tx,                      ty,                      tz,                      1.0, "mat4", 400 ]
}

fn mat4_inverse_affine(any m) list {
   "Inverts an affine tagged mat4. Returns identity when the matrix is singular."
   if(!is_list(m) || m.len < 16){ return mat4_identity() }
   def a00, a01 = 0.0 + m.get(0, 1.0), 0.0 + m.get(4, 0.0)
   def a02 = 0.0 + m.get(8, 0.0)
   def a10 = 0.0 + m.get(1, 0.0)
   def a11 = 0.0 + m.get(5, 1.0)
   def a12 = 0.0 + m.get(9, 0.0)
   def a20 = 0.0 + m.get(2, 0.0)
   def a21 = 0.0 + m.get(6, 0.0)
   def a22 = 0.0 + m.get(10, 1.0)
   def det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20)
   if(abs(det) < 0.0000001){ return mat4_identity() }
   def inv_det = 1.0 / det
   def b00 =  (a11 * a22 - a12 * a21) * inv_det
   def b01 = -(a01 * a22 - a02 * a21) * inv_det
   def b02 =  (a01 * a12 - a02 * a11) * inv_det
   def b10 = -(a10 * a22 - a12 * a20) * inv_det
   def b11 =  (a00 * a22 - a02 * a20) * inv_det
   def b12 = -(a00 * a12 - a02 * a10) * inv_det
   def b20 =  (a10 * a21 - a11 * a20) * inv_det
   def b21 = -(a00 * a21 - a01 * a20) * inv_det
   def b22 =  (a00 * a11 - a01 * a10) * inv_det
   def tx = 0.0 + m.get(12, 0.0)
   def ty = 0.0 + m.get(13, 0.0)
   def tz = 0.0 + m.get(14, 0.0)
   def itx = -(b00 * tx + b01 * ty + b02 * tz)
   def ity = -(b10 * tx + b11 * ty + b12 * tz)
   def itz = -(b20 * tx + b21 * ty + b22 * tz)
   [b00, b10, b20, 0.0,
      b01, b11, b21, 0.0,
      b02, b12, b22, 0.0,
   itx, ity, itz, 1.0, "mat4", 400]
}

fn mat4_transform_point(any m, any p) list {
   "Runs the mat4 transform point operation."
   if(!is_list(m) || m.len < 16){ return [0.0 + p.get(0,0.0), 0.0 + p.get(1,0.0), 0.0 + p.get(2,0.0)] }
   def x = 0.0 + p.get(0,0.0) def y = 0.0 + p.get(1,0.0) def z = 0.0 + p.get(2,0.0)
   mat4_apply_pos(m, x, y, z)
}

fn mat4_transform_dir(any m, any d) list {
   "Runs the mat4 transform dir operation."
   if(!is_list(m) || m.len < 16){ return [0.0 + d.get(0,0.0), 0.0 + d.get(1,0.0), 0.0 + d.get(2,-1.0)] }
   def x = 0.0 + d.get(0,0.0) def y = 0.0 + d.get(1,0.0) def z = 0.0 + d.get(2,-1.0)
   mut out = mat4_apply_dir(m, x, y, z)
   def ox, oy = 0.0 + out.get(0,0.0), 0.0 + out.get(1,0.0)
   def oz = 0.0 + out.get(2,0.0)
   def len2 = ox*ox + oy*oy + oz*oz
   if(len2 > 0.000001){
      def inv = 1.0 / sqrt(len2)
      out = [ox*inv, oy*inv, oz*inv]
   }
   out
}

fn safe_model_mat4(any m) list {
   "Normalizes possibly malformed model matrices to a strict 16-float array."
   if(!is_list(m) || m.len < 16){ return mat4_identity() }
   [
      float(m.get(0, 1.0)),  float(m.get(1, 0.0)),  float(m.get(2, 0.0)),  float(m.get(3, 0.0)),
      float(m.get(4, 0.0)),  float(m.get(5, 1.0)),  float(m.get(6, 0.0)),  float(m.get(7, 0.0)),
      float(m.get(8, 0.0)),  float(m.get(9, 0.0)),  float(m.get(10, 1.0)),  float(m.get(11, 0.0)),
      float(m.get(12, 0.0)),  float(m.get(13, 0.0)),  float(m.get(14, 0.0)),  float(m.get(15, 1.0))
   ]
}
