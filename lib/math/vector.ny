;; Keywords: vector linear-algebra math
;; Vector construction and vector algebra for 2D, 3D, and 4D numeric values.
;; References:
;; - std.math
module std.math.vector(vec2, vec3, vec4, Vector2, Vector3, Vector4, vec2_canonical, vec3_canonical, vec4_canonical, canonical, dim, at, set, x, y, z, w, xyz, is_vector, is_vec2, is_vec3, is_vec4, type_name, runtime_type, v_add, v_sub, v_mul, v_div, add, sub, mul, div, scale, divs, dot, dot3, hadamard, hadamard_div, cross3, len2, length3, magnitude, normalize, normalize3, lerp, op)
use std.core as core
use std.core
use std.math.float (float)

fn _vec_sqrt(number x) f64 { __flt_sqrt(float(x)) }

@pure
@jit
fn is_vector(any v) bool {
   "Returns true for vector-compatible values. Any list remains a vector, matching the original module behavior."
   if is_list(v) { return true }
   _dict_dim(v) > 0
}

@pure
@jit
fn _type_dim(str t) int {
   case t {
      "vec2", "Vector2" -> 2
      "vec3", "Vector3" -> 3
      "vec4", "Vector4" -> 4
      _ -> 0
   }
}

fn _dict_dim(any v) int { is_dict(v) ? _type_dim(v.get("__type", "")) : 0 }

fn _is_dim(any v, int n) bool {
   if is_list(v) { return __list_len(v) == n }
   _dict_dim(v) == n
}

fn _typed_vec(str t, any x0, any y0, any z0=0, any w0=0) dict {
   mut d = dict(8)
   def n = _type_dim(t)
   d["__type"] = t
   d["x"] = x0
   d["y"] = y0
   if n >= 3 { d["z"] = z0 }
   if n >= 4 { d["w"] = w0 }
   d
}

fn Vector2(...args) vec2 {
   "Creates a named runtime 2D vector with `.x` and `.y` member access."
   def n = args.len
   if n == 0 { return _typed_vec("vec2", 0.0, 0.0) }
   def x = args.get(0, 0.0)
   mut y = args.get(1, nil)
   if is_vector(x) { if y == nil { return _typed_vec("vec2", at(x, 0, 0.0), at(x, 1, 0.0)) } }
   if y == nil { y = 0.0 }
   _typed_vec("vec2", x, y)
}

fn Vector3(...args) vec3 {
   "Creates a named runtime 3D vector with `.x`, `.y`, and `.z` member access."
   def n = args.len
   if n == 0 { return _typed_vec("vec3", 0.0, 0.0, 0.0) }
   def x = args.get(0, 0.0)
   mut y, z = args.get(1, nil), args.get(2, nil)
   if is_vector(x) { if y == nil { if z == nil { return _typed_vec("vec3", at(x, 0, 0.0), at(x, 1, 0.0), at(x, 2, 0.0)) } } }
   if y == nil { y = 0.0 }
   if z == nil { z = 0.0 }
   _typed_vec("vec3", x, y, z)
}

fn Vector4(...args) vec4 {
   "Creates a named runtime 4D vector with `.x`, `.y`, `.z`, and `.w` member access."
   def n = args.len
   if n == 0 { return _typed_vec("vec4", 0.0, 0.0, 0.0, 0.0) }
   def x = args.get(0, 0.0)
   mut y, z = args.get(1, nil), args.get(2, nil)
   mut w = args.get(3, nil)
   if is_vector(x) {
      if y == nil { if z == nil { if w == nil { return _typed_vec("vec4", at(x, 0, 0.0), at(x, 1, 0.0), at(x, 2, 0.0), at(x, 3, 0.0)) } } }
   }
   if y == nil { y = 0.0 }
   if z == nil { z = 0.0 }
   if w == nil { w = 0.0 }
   _typed_vec("vec4", x, y, z, w)
}

fn _vector_type_name(any v) str {
   if is_dict(v) && _dict_dim(v) > 0 { return v.get("__type", "") }
   if is_list(v) {
      def n = __list_len(v)
      if n == 2 { return "vec2" }
      if n == 3 { return "vec3" }
      if n == 4 { return "vec4" }
   }
   type(v)
}

@pure
@jit
fn type_name(any v) str {
   "Returns `vec2`, `vec3`, or `vec4` for vector values, else the regular runtime type name."
   _vector_type_name(v)
}

@pure
@jit
fn runtime_type(any v) str {
   "Returns the vector runtime type name for vec2/3/4 values."
   _vector_type_name(v)
}

@pure
@jit
fn is_vec2(any v) bool {
   "Returns true when `v` is a two-component vector."
   _is_dim(v, 2)
}

@pure
@jit
fn is_vec3(any v) bool {
   "Returns true when `v` is a three-component vector."
   _is_dim(v, 3)
}

@pure
@jit
fn is_vec4(any v) bool {
   "Returns true when `v` is a four-component vector."
   _is_dim(v, 4)
}

@jit
fn vec2(any x=0, any y=0) vec2 {
   "Creates a typed 2D vector value with `.x` and `.y` member access."
   _typed_vec("vec2", x, y)
}

@jit
fn vec3(any x=0, any y=0, any z=0) vec3 {
   "Creates a typed 3D vector value with `.x`, `.y`, and `.z` member access."
   _typed_vec("vec3", x, y, z)
}

@jit
fn vec4(any x=0, any y=0, any z=0, any w=0) vec4 {
   "Creates a typed 4D vector value with `.x`, `.y`, `.z`, and `.w` member access."
   _typed_vec("vec4", x, y, z, w)
}

fn vec2_canonical(any v) vec2 {
   "Normalizes any vector-compatible value into the dict-backed vec2 representation."
   Vector2(v)
}

fn vec3_canonical(any v) vec3 {
   "Normalizes any vector-compatible value into the dict-backed vec3 representation."
   Vector3(v)
}

fn vec4_canonical(any v) vec4 {
   "Normalizes any vector-compatible value into the dict-backed vec4 representation."
   Vector4(v)
}

fn canonical(any v) any {
   "Normalizes a vec2/vec3/vec4-like value into the dict-backed vector representation."
   def n = dim(v)
   if n == 2 { return vec2_canonical(v) }
   if n == 3 { return vec3_canonical(v) }
   if n == 4 { return vec4_canonical(v) }
   v
}

@readonly
@jit
fn dim(any v) int {
   "Returns the dimension(number of elements) of vector `v`."
   if is_list(v) { return __list_len(v) }
   _dict_dim(v)
}

@readonly
@jit
fn _list_at(any v, int i, any default=0) any {
   def n = __list_len(v)
   if i < 0 { i = i + n }
   if i < 0 || i >= n { return default }
   __load_item_fast(v, i)
}

@readonly
@jit
fn _dict_at(any v, int i, any default=0) any {
   case i {
      0 -> v.get("x", default)
      1 -> v.get("y", default)
      2 -> v.get("z", default)
      3 -> v.get("w", default)
      _ -> default
   }
}

@readonly
@jit
fn at(any v, int i, any default=0) any {
   "Returns the element at index `i` of vector `v`, or `default` if not found."
   if is_list(v) { return _list_at(v, i, default) }
   is_dict(v) ? _dict_at(v, i, default) : default
}

fn _dict_set_at(any v, int i, any x) any {
   case i {
      0 -> v.set("x", x)
      1 -> v.set("y", x)
      2 -> v.set("z", x)
      3 -> v.set("w", x)
      _ -> v
   }
}

fn set(any v, int i, any x) any {
   "Sets the element at index `i` of vector `v` to value `x` and returns the vector."
   if is_list(v) {
      v[i] = x
      return v
   }
   is_dict(v) ? _dict_set_at(v, i, x) : v
}

@readonly
@jit
fn x(any v, any default=0) any { at(v, 0, default) }

@readonly
@jit
fn y(any v, any default=0) any { at(v, 1, default) }

@readonly
@jit
fn z(any v, any default=0) any { at(v, 2, default) }

@readonly
@jit
fn w(any v, any default=0) any { at(v, 3, default) }

@pure
@jit
fn xyz(any v) vec3 {
   "Returns the first three vector components as the fast list-backed vec3 representation."
   vec3(x(v), y(v), z(v))
}

@pure
@jit
@inline
fn _zip2_op(any av, any bv, int op) any {
   case op {
      0 -> core.add(av, bv)
      1 -> core.sub(av, bv)
      2 -> core.mul(av, bv)
      3 -> core.div(core.mul(av, 1.0), core.mul(bv, 1.0))
      _ -> nil
   }
}

fn _vector_shape_out(list out, int n, bool typed) any {
   __list_set_len(out, n)
   if typed {
      if n == 2 { return Vector2(_list_at(out, 0, 0), _list_at(out, 1, 0)) }
      if n == 3 { return Vector3(_list_at(out, 0, 0), _list_at(out, 1, 0), _list_at(out, 2, 0)) }
      if n == 4 { return Vector4(_list_at(out, 0, 0), _list_at(out, 1, 0), _list_at(out, 2, 0), _list_at(out, 3, 0)) }
   }
   out
}

fn _zip2(any a, any b, int op) any {
   if !is_vector(a) { return [] }
   if !is_vector(b) { return [] }
   def both_list = is_list(a) && is_list(b)
   def na, nb = both_list ? __list_len(a) : dim(a), both_list ? __list_len(b) : dim(b)
   def n = (na < nb) ? na : nb
   mut out = list(n)
   mut i = 0
   if both_list {
      while i < n {
         __store_item_fast(out, i, _zip2_op(__load_item_fast(a, i), __load_item_fast(b, i), op))
         i += 1
      }
   } else {
      while i < n {
         __store_item_fast(out, i, _zip2_op(at(a, i, 0), at(b, i, 0), op))
         i += 1
      }
   }
   _vector_shape_out(out, n, !both_list)
}

@pure
@jit
fn v_add(any a, any b) any {
   "Returns the element-wise sum of vectors `a` and `b`."
   _zip2(a, b, 0)
}

@pure
@jit
fn v_sub(any a, any b) any {
   "Returns the element-wise difference of vectors `a` and `b` (a - b)."
   _zip2(a, b, 1)
}

@pure
@jit
fn hadamard(any a, any b) any {
   "Returns the Hadamard(element-wise) product of vectors `a` and `b`."
   _zip2(a, b, 2)
}

@pure
@jit
fn hadamard_div(any a, any b) any {
   "Returns the element-wise quotient of vectors `a` and `b`."
   _zip2(a, b, 3)
}

@pure
@jit
fn v_mul(any a, any b) any {
   "Returns the product of vector `a` and `b`. If `b` is a scalar, performs scaling. If `b` is a vector, performs a dot product."
   def out = _mul_vector(a, b)
   if out != nil { return out }
   scale(a, b)
}

@pure
@jit
fn v_div(any a, any b) any {
   "Returns vector division: component-wise for vector/vector, scalar division for vector/scalar."
   if is_vector(b) { return hadamard_div(a, b) }
   divs(a, b)
}

@pure
@jit
fn add(any a, any b) any {
   "Generic addition: supports both numbers and vectors."
   if is_vector(a) { if is_vector(b) { return v_add(a, b) } }
   core.add(a, b)
}

@pure
@jit
fn sub(any a, any b) any {
   "Generic subtraction: supports both numbers and vectors."
   if is_vector(a) { if is_vector(b) { return v_sub(a, b) } }
   core.sub(a, b)
}

@pure
@jit
fn mul(any a, any b) any {
   "Generic multiplication: supports numbers, scalar-vector, and vector-vector products."
   def out = _mul_vector(a, b)
   if out != nil { return out }
   core.mul(a, b)
}

@pure
@jit
fn div(any a, any b) any {
   "Generic division: supports numbers, vector-scalar division, and component-wise vector division."
   if is_vector(a) {
      if is_vector(b) { return hadamard_div(a, b) }
      if is_int(b) || is_float(b) { return divs(a, b) }
   }
   core.div(a, b)
}

@pure
@jit
@inline
fn _scalar_op(any v, any s, int op) any {
   op == 0 ? core.mul(v, s) : core.div(core.mul(v, 1.0), core.mul(s, 1.0))
}

fn _map_s(any v, any s, int op) any {
   if !is_vector(v) { return [] }
   def list_src = is_list(v)
   def n = list_src ? __list_len(v) : dim(v)
   mut out = list(n)
   mut i = 0
   while i < n {
      def value = list_src ? __load_item_fast(v, i) : at(v, i, 0)
      __store_item_fast(out, i, _scalar_op(value, s, op))
      i += 1
   }
   _vector_shape_out(out, n, !list_src)
}

@pure
@jit
fn scale(any v, any s) any {
   "Multiplies vector `v` by scalar `s`."
   _map_s(v, s, 0)
}

@pure
@jit
fn divs(any v, any s) any {
   "Divides vector `v` by scalar `s`."
   _map_s(v, s, 1)
}

@pure
@jit
fn dot(any a, any b) any {
   "Returns the dot product of vectors `a` and `b`."
   if !is_vector(a) { return 0 }
   if !is_vector(b) { return 0 }
   mut both_list = false
   if is_list(a) { if is_list(b) { both_list = true } }
   def na, nb = both_list ? __list_len(a) : dim(a), both_list ? __list_len(b) : dim(b)
   def n = (na < nb) ? na : nb
   mut acc = 0
   mut i = 0
   if both_list {
      while i < n {
         acc = core.add(acc, core.mul(__load_item_fast(a, i), __load_item_fast(b, i)))
         i += 1
      }
   } else {
      while i < n {
         acc = core.add(acc, core.mul(at(a, i, 0), at(b, i, 0)))
         i += 1
      }
   }
   acc
}

@pure
@jit
fn dot3(any a, any b) any {
   "Returns the 3D dot product without generic loop overhead."
   core.add(core.add(core.mul(x(a), x(b)), core.mul(y(a), y(b))), core.mul(z(a), z(b)))
}

impl vec2 {
   @pure
   @jit
   fn add(self a, self b) self { v_add(a, b) }
   @pure
   @jit
   fn sub(self a, self b) self { v_sub(a, b) }
   @pure
   @jit
   fn dot(self a, self b) f64 { dot(a, b) }
   @pure
   @jit
   fn scale(self v, f64 s) self { scale(v, s) }
   @pure
   @jit
   fn divs(self v, f64 s) self { divs(v, s) }
   @pure
   @jit
   fn div(self a, self b) self { hadamard_div(a, b) }
   operator + self: self = add
   operator - self: self = sub
   operator * self: f64 = dot
   operator * f64: self = scale
   operator / self: self = div
   operator / f64: self = divs
}

impl vec3 {
   @pure
   @jit
   fn add(self a, self b) self { v_add(a, b) }
   @pure
   @jit
   fn sub(self a, self b) self { v_sub(a, b) }
   @pure
   @jit
   fn dot(self a, self b) f64 { dot3(a, b) }
   @pure
   @jit
   fn scale(self v, f64 s) self { scale(v, s) }
   @pure
   @jit
   fn divs(self v, f64 s) self { divs(v, s) }
   @pure
   @jit
   fn div(self a, self b) self { hadamard_div(a, b) }
   operator + self: self = add
   operator - self: self = sub
   operator * self: f64 = dot
   operator * f64: self = scale
   operator / self: self = div
   operator / f64: self = divs
}

impl vec4 {
   @pure
   @jit
   fn add(self a, self b) self { v_add(a, b) }
   @pure
   @jit
   fn sub(self a, self b) self { v_sub(a, b) }
   @pure
   @jit
   fn dot(self a, self b) f64 { dot(a, b) }
   @pure
   @jit
   fn scale(self v, f64 s) self { scale(v, s) }
   @pure
   @jit
   fn divs(self v, f64 s) self { divs(v, s) }
   @pure
   @jit
   fn div(self a, self b) self { hadamard_div(a, b) }
   operator + self: self = add
   operator - self: self = sub
   operator * self: f64 = dot
   operator * f64: self = scale
   operator / self: self = div
   operator / f64: self = divs
}

impl f64 {
   @pure
   @jit
   fn scale_vec2(self s, vec2 v) vec2 { scale(v, s) }
   @pure
   @jit
   fn scale_vec3(self s, vec3 v) vec3 { scale(v, s) }
   @pure
   @jit
   fn scale_vec4(self s, vec4 v) vec4 { scale(v, s) }
   operator * vec2: vec2 = scale_vec2
   operator * vec3: vec3 = scale_vec3
   operator * vec4: vec4 = scale_vec4
}

@pure
@jit
fn cross3(any a, any b) vec3 {
   "Returns the vector cross product of two 3D vectors `a` and `b`."
   if dim(a) < 3 || dim(b) < 3 { return vec3(0, 0, 0) }
   def ax, ay = x(a), y(a)
   def az = z(a)
   def bx = x(b)
   def by = y(b)
   def bz = z(b)
   def rx = core.sub(core.mul(ay, bz), core.mul(az, by))
   def ry = core.sub(core.mul(az, bx), core.mul(ax, bz))
   def rz = core.sub(core.mul(ax, by), core.mul(ay, bx))
   if is_dict(a) || is_dict(b) { return Vector3(rx, ry, rz) }
   vec3(
      rx,
      ry,
      rz
   )
}

@pure
@jit
fn len2(any v) any {
   "Returns the squared magnitude(Euclidean length) of vector `v`."
   dot(v, v)
}

@pure
@jit
fn magnitude(any v) f64 {
   "Returns the magnitude(Euclidean length) of vector `v`."
   _vec_sqrt(len2(v))
}

@pure
@jit
fn length3(any v) f64 {
   "Returns the 3D vector length without generic loop overhead."
   def vx, vy = x(v), y(v)
   def vz = z(v)
   _vec_sqrt(core.add(core.add(core.mul(vx, vx), core.mul(vy, vy)), core.mul(vz, vz)))
}

@pure
@jit
fn normalize3(any v) vec3 {
   "Returns a unit-length 3D vector, preserving named Vector3 dictionaries."
   def vx, vy = x(v), y(v)
   def vz = z(v)
   def l = _vec_sqrt(core.add(core.add(core.mul(vx, vx), core.mul(vy, vy)), core.mul(vz, vz)))
   if l == 0 { return is_dict(v) ? Vector3(0.0, 0.0, 0.0) : vec3(0.0, 0.0, 0.0) }
   if is_dict(v) { return Vector3(core.div(vx, l), core.div(vy, l), core.div(vz, l)) }
   vec3(core.div(vx, l), core.div(vy, l), core.div(vz, l))
}

@pure
@jit
fn normalize(any v) any {
   "Returns a unit-length vector pointing in the same direction as `v`. Returns a copy of `v` if it has zero magnitude."
   if dim(v) == 3 { return normalize3(v) }
   def l = magnitude(v)
   if l == 0 {
      if is_dict(v) {
         def n = dim(v)
         if n == 2 { return Vector2(0.0, 0.0) }
         if n == 4 { return Vector4(0.0, 0.0, 0.0, 0.0) }
      }
      return clone(v)
   }
   divs(v, l)
}

@pure
@jit
fn lerp(any a, any b, f64 t) any {
   "Linearly interpolates between vectors `a` and `b` using factor `t` (usually in range [0, 1])."
   v_add(a, scale(v_sub(b, a), t))
}

fn _mul_vector(any a, any b) any {
   if is_vector(a) {
      if is_vector(b) { return dot(a, b) }
      if is_int(b) || is_float(b) { return scale(a, b) }
   }
   if is_vector(b) { if is_int(a) || is_float(a) { return scale(b, a) } }
   nil
}

fn _div_vector(any a, any b) any {
   if is_vector(a) {
      if is_int(b) || is_float(b) { return divs(a, b) }
      if is_vector(b) { return hadamard_div(a, b) }
   }
   nil
}

fn _add_vector(any a, any b) any {
   if is_vector(a) { if is_vector(b) { return v_add(a, b) } }
   nil
}

fn _sub_vector(any a, any b) any {
   if is_vector(a) { if is_vector(b) { return v_sub(a, b) } }
   nil
}

fn op(str name, any a, any b=0) any {
   "Runtime vector operator dispatcher for examples and dynamic code."
   match name {
      "+" -> { def out = _add_vector(a, b) return out != nil ? out : core.add(a, b) }
      "add" -> { def out = _add_vector(a, b) return out != nil ? out : core.add(a, b) }
      "-" -> { def out = _sub_vector(a, b) return out != nil ? out : core.sub(a, b) }
      "sub" -> { def out = _sub_vector(a, b) return out != nil ? out : core.sub(a, b) }
      "*" -> { def out = _mul_vector(a, b) return out != nil ? out : core.mul(a, b) }
      "mul" -> { def out = _mul_vector(a, b) return out != nil ? out : core.mul(a, b) }
      "/" -> { def out = _div_vector(a, b) return out != nil ? out : core.div(a, b) }
      "div" -> { def out = _div_vector(a, b) return out != nil ? out : core.div(a, b) }
      "dot" -> { return dot(a, b) }
      "dot3" -> { return dot3(a, b) }
      "cross" -> { return cross3(a, b) }
      "cross3" -> { return cross3(a, b) }
      "neg" -> { return scale(a, -1) }
      "normalize" -> { return normalize(a) }
      "normalize3" -> { return normalize3(a) }
      _ -> { return nil }
   }
}

#main {
   def v2 = Vector2([7, 8])
   assert(v2.get("x") == 7 && v2.get("y") == 8, "Vector2 canonicalizes list")
   def v3 = vec3_canonical([1, 2, 3])
   assert(dim(v3) == 3 && v3.get("z") == 3, "vec3 canonicalizes list")
   print("✓ std.math.vector self-test passed")
}
