;; Keywords: math vector
;; Vector mathematics module (2D, 3D, 4D).

module std.math.vector (
   vec2, vec3, vec4,
   dim, at, set, is_vector,
   v_add, v_sub, v_mul, v_div, add, sub, mul, div, scale, divs,
   dot, hadamard, cross3,
   len2, magnitude, normalize, lerp
)
use std.core as core
use std.core *
use std.math *

fn is_vector(v){ 
    "Returns true if `v` is a vector (represented as a list)."
    is_list(v) 
}

fn _mk(n, fill=0){
   "Internal: Creates a list of size `n` filled with `fill`."
   mut out = list(n)
   mut i = 0
   while(i < n){
      out = append(out, fill)
      i = i + 1
   }
   out
}

fn vec2(x=0, y=0){
   "Creates a 2D vector [x, y]."
   mut v = list(2)
   v = append(v, x)
   v = append(v, y)
   v
}

fn vec3(x=0, y=0, z=0){
   "Creates a 3D vector [x, y, z]."
   mut v = list(3)
   v = append(v, x)
   v = append(v, y)
   v = append(v, z)
   v
}

fn vec4(x=0, y=0, z=0, w=0){
   "Creates a 4D vector [x, y, z, w]."
   mut v = list(4)
   v = append(v, x)
   v = append(v, y)
   v = append(v, z)
   v = append(v, w)
   v
}

fn dim(v){ 
    "Returns the dimension (number of elements) of vector `v`."
    len(v) 
}

fn at(v, i, default=0){ 
    "Returns the element at index `i` of vector `v`, or `default` if not found."
    get(v, i, default) 
}

fn set(v, i, x){
   "Sets the element at index `i` of vector `v` to `x`.
   
   Returns the modified vector."
   store_item(v, i, x)
   v
}

fn _zip2(a, b, op){
   "Internal: Performs element-wise operation `op` on two vectors."
   def na = len(a)
   def nb = len(b)
   def n = (na < nb) ? na : nb
   mut out = list(n)
   mut i = 0
   while(i < n){
      def x = get(a, i, 0)
      def y = get(b, i, 0)
      if(op == 0){ out = append(out, x + y) }
      else {
          if(op == 1){ out = append(out, x - y) }
          else { out = append(out, x * y) }
      }
      i = i + 1
   }
   out
}

fn v_add(a, b){
   "Returns the element-wise sum of vectors `a` and `b`."
   _zip2(a, b, 0)
}

fn v_sub(a, b){ 
    "Returns the element-wise difference of vectors `a` and `b` (a - b)."
    _zip2(a, b, 1) 
}

fn hadamard(a, b){ 
    "Returns the Hadamard (element-wise) product of vectors `a` and `b`."
    _zip2(a, b, 2) 
}

fn v_mul(a, b){
   "Multiplies vector `a` by vector or scalar `b`."
   if(is_int(b) || is_float(b)){ return scale(a, b) }
   if(is_vector(b)){ return hadamard(a, b) }
   scale(a, b)
}

fn v_div(a, b){
   "Divides vector `a` by scalar `b`."
   if(is_int(b) || is_float(b)){ return divs(a, b) }
   divs(a, b)
}

;; Generic Dispatch wrappers
fn add(a, b){ 
    "Generic addition: supports both numbers and vectors."
    if(is_vector(a) && is_vector(b)){ return v_add(a, b) }
    core.add(a, b)
}

fn sub(a, b){
    "Generic subtraction: supports both numbers and vectors."
    if(is_vector(a) && is_vector(b)){ return v_sub(a, b) }
    core.sub(a, b)
}

fn mul(a, b){
    "Generic multiplication: supports numbers, scalar-vector, and vector-vector products."
    if(is_vector(a) && is_vector(b)){ return hadamard(a, b) }
    if(is_vector(a) && (is_int(b) || is_float(b))){ return scale(a, b) }
    if(is_vector(b) && (is_int(a) || is_float(a))){ return scale(b, a) }
    core.mul(a, b)
}

fn div(a, b){
    "Generic division: supports numbers and vector-scalar division."
    if(is_vector(a) && (is_int(b) || is_float(b))){ return divs(a, b) }
    core.div(a, b)
}

fn scale(v, s){
   "Multiplies vector `v` by scalar `s`."
   def n = len(v)
   mut out = list(n)
   mut i = 0
   while(i < n){
      out = append(out, get(v, i, 0) * s)
      i = i + 1
   }
   out
}

fn divs(v, s){
   "Divides vector `v` by scalar `s`."
   def n = len(v)
   mut out = list(n)
   mut i = 0
   while(i < n){
      out = append(out, get(v, i, 0) / s)
      i = i + 1
   }
   out
}

fn dot(a, b){
   "Returns the dot product of vectors `a` and `b`."
   def na = len(a)
   def nb = len(b)
   def n = (na < nb) ? na : nb
   mut acc = 0
   mut i = 0
   while(i < n){
      acc = acc + get(a, i, 0) * get(b, i, 0)
      i = i + 1
   }
   acc
}

fn cross3(a, b){
   "Returns the cross product of 3D vectors `a` and `b`."
   if(len(a) < 3 || len(b) < 3){ return vec3(0, 0, 0) }
   def ax = get(a, 0, 0)
   def ay = get(a, 1, 0)
   def az = get(a, 2, 0)
   def bx = get(b, 0, 0)
   def by = get(b, 1, 0)
   def bz = get(b, 2, 0)
   vec3(
      ay * bz - az * by,
      az * bx - ax * bz,
      ax * by - ay * bx
   )
}

fn len2(v){ 
    "Returns the squared magnitude (Euclidean length) of vector `v`."
    dot(v, v) 
}

fn magnitude(v){ 
    "Returns the magnitude (Euclidean length) of vector `v`."
    sqrt(len2(v)) 
}

fn normalize(v){
   "Returns a normalized version of vector `v` (unit vector)."
   def l = magnitude(v)
   if(l == 0){ return list_clone(v) }
   divs(v, l)
}

fn lerp(a, b, t){
   "Performs linear interpolation between vectors `a` and `b` by factor `t`."
   add(a, scale(sub(b, a), t))
}
