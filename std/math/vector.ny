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
      store_item(out, i, fill)
      i += 1
   }
   store64(out, n, 0)
   out
}

fn vec2(x=0, y=0){
   "Creates a 2D vector [x, y]."
   def v = list(2)
   store_item(v, 0, x)
   store_item(v, 1, y)
   store64(v, 2, 0) ; Update Length
   v
}

fn vec3(x=0, y=0, z=0){
   "Creates a 3D vector [x, y, z]."
   def v = list(3)
   store_item(v, 0, x)
   store_item(v, 1, y)
   store_item(v, 2, z)
   store64(v, 3, 0) ; Update Length
   v
}

fn vec4(x=0, y=0, z=0, w=0){
   "Creates a 4D vector [x, y, z, w]."
   def v = list(4)
   store_item(v, 0, x)
   store_item(v, 1, y)
   store_item(v, 2, z)
   store_item(v, 3, w)
   store64(v, 4, 0) ; Update Length
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
   if(op == 0){
      while(i < n){
         store_item(out, i, get(a, i, 0) + get(b, i, 0))
         i += 1
      }
   } elif(op == 1){
      while(i < n){
         store_item(out, i, get(a, i, 0) - get(b, i, 0))
         i += 1
      }
   } else {
      while(i < n){
         store_item(out, i, get(a, i, 0) * get(b, i, 0))
         i += 1
      }
   }
   store64(out, n, 0)
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

;; Generic dispatch wrappers
fn add(a, b){
    "Generic addition: supports both numbers and vectors."
    if(is_vector(a) && is_vector(b)){ return v_add(a, b) }
    a + b
}

fn sub(a, b){
    "Generic subtraction: supports both numbers and vectors."
    if(is_vector(a) && is_vector(b)){ return v_sub(a, b) }
    a - b
}

fn mul(a, b){
    "Generic multiplication: supports numbers, scalar-vector, and vector-vector products."
    if(is_vector(a) && is_vector(b)){ return hadamard(a, b) }
    if(is_vector(a) && (is_int(b) || is_float(b))){ return scale(a, b) }
    if(is_vector(b) && (is_int(a) || is_float(a))){ return scale(b, a) }
    a * b
}

fn div(a, b){
    "Generic division: supports numbers and vector-scalar division."
    if(is_vector(a) && (is_int(b) || is_float(b))){ return divs(a, b) }
    a / b
}

fn scale(v, s){
   "Multiplies vector `v` by scalar `s`."
   def n = len(v)
   mut out = list(n)
   mut i = 0
   while(i < n){
      store_item(out, i, get(v, i, 0) * s)
      i += 1
   }
   store64(out, n, 0)
   out
}

fn divs(v, s){
   "Divides vector `v` by scalar `s`."
   def n = len(v)
   mut out = list(n)
   mut i = 0
   while(i < n){
      store_item(out, i, get(v, i, 0) / s)
      i += 1
   }
   store64(out, n, 0)
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
      i += 1
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

if(comptime{__main()}){
    use std.math.vector as v
    use std.core *

    def a = v.vec3(1, 2, 3)
    def b = v.vec3(4, 5, 6)

    def s = a + b
    assert(get(s, 0, 0) == 5, "vector add x")
    assert(get(s, 1, 0) == 7, "vector add y")
    assert(get(s, 2, 0) == 9, "vector add z")

    assert(v.dot(a, b) == 32, "vector dot")

    def c = v.cross3(a, b)
    assert(get(c, 0, 0) == -3, "vector cross x")
    assert(get(c, 1, 0) == 6, "vector cross y")
    assert(get(c, 2, 0) == -3, "vector cross z")

    def h = v.hadamard(a, b)
    assert(get(h, 0, 0) == 4, "vector hadamard x")
    assert(get(h, 1, 0) == 10, "vector hadamard y")
    assert(get(h, 2, 0) == 18, "vector hadamard z")

    print("✓ std.math.vector tests passed")
}
