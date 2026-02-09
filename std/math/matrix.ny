;; Keywords: math matrix mat4
;; Matrix mathematics module (4x4).

module std.math.matrix (
   mat4_zero, mat4_identity,
   mat4_get, mat4_set,
   mat4_transpose,
   mat4_mul, mat4_mul_vec4, mat4_add,
   mat4_translate, mat4_scale,
   add, mul
)
use std.core as core
use std.core *

fn _idx(r, c){ 
    "Internal: Converts row and column indices to a 1D list index."
    r * 4 + c 
}

fn mat4_zero(){
   "Creates a new 4x4 matrix filled with zeros."
   mut m = list(16)
   mut i = 0
   while(i < 16){
      m = append(m, 0)
      i = i + 1
   }
   m
}

fn mat4_identity(){
   "Creates a new 4x4 identity matrix."
   mut m = mat4_zero()
   store_item(m, _idx(0,0), 1)
   store_item(m, _idx(1,1), 1)
   store_item(m, _idx(2,2), 1)
   store_item(m, _idx(3,3), 1)
   m
}

fn mat4_get(m, r, c, default=0){ 
    "Returns the element at row `r` and column `c` of the 4x4 matrix `m`."
    get(m, _idx(r, c), default) 
}

fn mat4_set(m, r, c, v){
   "Sets the element at row `r` and column `c` of the 4x4 matrix `m` to `v`. 
   
   Returns the modified matrix."
   store_item(m, _idx(r, c), v)
   m
}

fn mat4_transpose(m){
   "Returns the transpose of the 4x4 matrix `m`."
   mut out = mat4_zero()
   mut r = 0
   while(r < 4){
      mut c = 0
      while(c < 4){
         store_item(out, _idx(r, c), mat4_get(m, c, r, 0))
         c = c + 1
      }
      r = r + 1
   }
   out
}

fn mat4_add(a, b){
   "Returns the element-wise sum of 4x4 matrices `a` and `b`."
   mut out = mat4_zero()
   mut i = 0
   while(i < 16){
      store_item(out, i, get(a, i, 0) + get(b, i, 0))
      i = i + 1
   }
   out
}

fn mat4_mul(a, b){
   "Returns the product of two 4x4 matrices `a` and `b`."
   mut out = mat4_zero()
   mut r = 0
   while(r < 4){
      mut c = 0
      while(c < 4){
         mut s = 0
         mut k = 0
         while(k < 4){
            s = s + mat4_get(a, r, k, 0) * mat4_get(b, k, c, 0)
            k = k + 1
         }
         store_item(out, _idx(r, c), s)
         c = c + 1
      }
      r = r + 1
   }
   out
}

fn mat4_mul_vec4(m, v){
   "Multiplies 4x4 matrix `m` by 4D vector `v`."
   mut out = list(4)
   mut r = 0
   while(r < 4){
      mut s = 0
      mut c = 0
      while(c < 4){
         s = s + mat4_get(m, r, c, 0) * get(v, c, 0)
         c = c + 1
      }
      out = append(out, s)
      r = r + 1
   }
   out
}

fn mat4_translate(tx, ty, tz){
   "Creates a 4x4 translation matrix for offsets [tx, ty, tz]."
   mut m = mat4_identity()
   store_item(m, _idx(0,3), tx)
   store_item(m, _idx(1,3), ty)
   store_item(m, _idx(2,3), tz)
   m
}

fn mat4_scale(sx, sy, sz){
   "Creates a 4x4 scaling matrix for factors [sx, sy, sz]."
   mut m = mat4_zero()
   store_item(m, _idx(0,0), sx)
   store_item(m, _idx(1,1), sy)
   store_item(m, _idx(2,2), sz)
   store_item(m, _idx(3,3), 1)
   m
}

;; Generic Wrappers
fn add(a, b){
   "Generic addition: supports numbers, vectors, and 4x4 matrices."
   if(is_list(a) && len(a) == 16 && is_list(b) && len(b) == 16){
      return mat4_add(a, b)
   }
   core.add(a, b)
}

fn mul(a, b){
   "Generic multiplication: supports numbers, matrices, and matrix-vector products."
   if(is_list(a) && len(a) == 16){
      if(is_list(b) && len(b) == 16){ return mat4_mul(a, b) }
      if(is_list(b) && len(b) == 4){ return mat4_mul_vec4(a, b) }
   }
   core.mul(a, b)
}
