;; Keywords: math
;; Math module.

module std.math ( fmod,
   abs, min, max, pow, mod, clamp, sign, sqrt, gcd, lcm, factorial, lerp,
   sin, cos, tan, asin, acos, atan, atan2,
   PI, PHI, E, TAU, LN2, LN10
)
use std.core *
use std.math.float *

fn abs(x){
   "Return the absolute value of `x`."
   if(x < 0){ return 0 - x }
   return x
}

fn min(a,b){
   "Return the smaller of two values `a` and `b`."
   if(a<b){ return a  } return b
}

fn max(a,b){
   "Return the larger of two values `a` and `b`."
   if(a>b){ return a  } return b
}

fn pow(a,b){
   "Return `a` raised to the power of `b` (a^b) using an iterative loop."
   mut res = 1  mut i = 0
   while(i < b){ res = res * a  i += 1 }
   return res
}

fn mod(a,b){
   "Return the remainder of `a` divided by `b`. The result always has the same sign as `b`."
   mut res = a - (a / b) * b
   if(res < 0){ res = res + abs(b) }
   return res
}

fn clamp(x, lo, hi){
   "Clamps `x` to the range [lo, hi]."
   if(x < lo){ return lo  }
   if(x > hi){ return hi  }
   return x
}

fn sign(x){
   "Return the sign of `x`: -1 if negative, 0 if zero, 1 if positive."
   if(x == 0){ return 0  }
   if(x < 0){ return -1  }
   return 1
}

fn sqrt(x){
   "Return the square root of `x` using Newton's method (16 iterations)."
   if(x <= 0){ return 0  }
   mut r = x
   mut i = 0
   while(i < 16){
      r = (r + x / r) / 2
      i += 1
   }
   return r
}

fn gcd(a,b){
   "Return the greatest common divisor of `a` and `b`."
   mut x = abs(a)
   mut y = abs(b)
   while(y != 0){
      def t = x % y
      x = y
      y = t
   }
   return x
}

fn lcm(a,b){
   "Return the least common multiple of `a` and `b`."
   if(a == 0 || b == 0){ return 0  }
   return abs((a / gcd(a,b)) * b)
}

fn factorial(n){
   "Return the factorial of `n`."
   if(n <= 1){ return 1 }
   mut res = 1
   mut i = 2
   while(i <= n){
      res = res * i
      i += 1
   }
   return res
}

fn lerp(a,b,t){
   "Performs linear interpolation between `a` and `b` using factor `t` (usually 0.0 to 1.0)."
   return a + (b - a) * t
}

def PI   = float(3.14159265358979323846)
def PHI  = float(1.61803398874989484820)
def E    = float(2.71828182845904523536)
def TAU  = fmul(float(2.0), PI)
def LN2  = float(0.69314718055994530941)
def LN10 = float(2.30258509299404568402)

def _HALF_PI = fdiv(PI, float(2.0))
def _ATAN_K  = float(0.273)

fn fmod(a, b){
   "Auto-generated docstring: _fmod."
   a = float(a)
   b = float(b)
   if(feq(b, 0.0) || is_inf(a) || is_nan(a)){ return nan() }
   def q = floor(fdiv(a, b))
   fsub(a, fmul(b, float(q)))
}

fn _sqrt_pos(x){
   "Auto-generated docstring: _sqrt_pos."
   x = float(x)
   if(!fgt(x, 0.0)){ return 0.0 }
   if(is_inf(x)){ return inf() }
   mut r = x
   mut i = 0
   while(i < 10){
      r = fmul(0.5, fadd(r, fdiv(x, r)))
      i += 1
   }
   r
}

fn sin(x){
   "Sine (radians), Nytrix implementation."
   mut fx = float(x)
   if(is_nan(fx) || is_inf(fx)){ return nan() }
   fx = fmod(fx, TAU)
   if(flt(fx, 0.0)){ fx = fadd(fx, TAU) }
   mut sgn = float(1)
   if(fgt(fx, PI)){
      fx = fsub(fx, PI)
      sgn = float(-1)
   }
   if(fgt(fx, _HALF_PI)){ fx = fsub(PI, fx) }
   mut res = fx
   mut term = fx
   def x2 = fmul(fx, fx)
   mut i = 3
   mut sign_v = -1
   while(i <= 15){
      term = fmul(term, x2)
      term = fdiv(term, float((i - 1) * i))
      if(sign_v == -1){ res = fsub(res, term) }
      else { res = fadd(res, term) }
      sign_v = 0 - sign_v
      i += 2
   }
   fmul(sgn, res)
}

fn cos(x){
   "Cosine (radians), Nytrix implementation."
   mut fx = float(x)
   if(is_nan(fx) || is_inf(fx)){ return nan() }
   sin(fadd(fx, _HALF_PI))
}

fn tan(x){
   "Tangent (radians), Nytrix implementation."
   def s = sin(x)
   def c = cos(x)
   if(is_nan(s) || is_nan(c)){ return nan() }
   if(feq(c, 0.0)){
      return fgt(s, 0.0) ? inf() : fsub(0.0, inf())
   }
   fdiv(s, c)
}

fn _atan_unit(x){
   "Auto-generated docstring: _atan_unit."
   fmul(x, fadd(fdiv(PI, 4.0), fmul(float(_ATAN_K), fsub(1.0, abs(x)))))
}

fn atan(x){
   "Arc tangent in radians, Nytrix implementation."
   x = float(x)
   if(is_nan(x)){ return nan() }
   if(is_inf(x)){ return fgt(x, 0.0) ? _HALF_PI : fsub(0.0, _HALF_PI) }
   if(feq(x, 0.0)){ return 0.0 }
   def ax = abs(x)
   if(!fgt(ax, 1.0)){ return _atan_unit(x) }
   if(fgt(x, 0.0)){ return fsub(_HALF_PI, _atan_unit(fdiv(1.0, x))) }
   fsub(fsub(0.0, _HALF_PI), _atan_unit(fdiv(1.0, x)))
}

fn atan2(y, x){
   "Arc tangent of y/x with quadrant handling, in radians."
   y = float(y)
   x = float(x)
   if(is_nan(y) || is_nan(x)){ return nan() }
   if(fgt(x, 0.0)){ return atan(fdiv(y, x)) }
   if(flt(x, 0.0) && !flt(y, 0.0)){ return fadd(atan(fdiv(y, x)), PI) }
   if(flt(x, 0.0) && flt(y, 0.0)){ return fsub(atan(fdiv(y, x)), PI) }
   if(fgt(y, 0.0)){ return _HALF_PI }
   if(flt(y, 0.0)){ return fsub(0.0, _HALF_PI) }
   if(is_inf(x)){ ; x is inf, y is finite
       return fgt(x, 0.0) ? 0.0 : PI
   }
   0.0
}

fn asin(x){
   "Arc sine in radians, Nytrix implementation."
   x = clamp(float(x), -1.0, 1.0)
   if(is_nan(x)){ return nan() }
   atan2(x, _sqrt_pos(fsub(1.0, fmul(x, x))))
}

fn acos(x){
   "Arc cosine in radians, Nytrix implementation."
   fsub(_HALF_PI, asin(x))
}

if(comptime{__main()}){
   use std.math *
   use std.math.float *
   use std.core.error *

   assert(abs(-5) == 5, "abs neg")
   assert(abs(5) == 5, "abs pos")
   assert(abs(0) == 0, "abs zero")

   assert(min(3,7) == 3, "min")
   assert(min(7,3) == 3, "min rev")
   assert(max(3,7) == 7, "max")
   assert(max(7,3) == 7, "max rev")

   assert(pow(2,3) == 8, "pow 2^3")
   assert(pow(5,2) == 25, "pow 5^2")
   assert(pow(10,0) == 1, "pow 10^0")
   assert(pow(2,10) == 1024, "pow 2^10")
   assert(gcd(12,18) == 6, "gcd")
   assert(lcm(12,18) == 36, "lcm")

   assert(sqrt(16) == 4, "sqrt 16")
   assert(sqrt(25) == 5, "sqrt 25")
   assert(sqrt(1) == 1, "sqrt 1")
   assert(sqrt(0) == 0, "sqrt 0")

   fn near(a, b, eps){
       "Auto-generated docstring: near."
       abs(a - b) <= eps
   }
   assert(near(sin(0.0), 0.0, 0.000001), "sin 0")
   assert(near(sin(PI / 2.0), 1.0, 0.0005), "sin pi/2")
   assert(near(cos(0.0), 1.0, 0.000001), "cos 0")
   assert(near(cos(PI), -1.0, 0.001), "cos pi")
   assert(near(tan(0.0), 0.0, 0.000001), "tan 0")
   assert(near(atan(1.0), PI / 4.0, 0.005), "atan 1")
   assert(near(atan2(1.0, 1.0), PI / 4.0, 0.005), "atan2 1,1")
   assert(near(asin(0.5), PI / 6.0, 0.005), "asin 0.5")
   assert(near(acos(0.5), PI / 3.0, 0.005), "acos 0.5")

   assert(floor(3) == 3, "floor pos")
   assert(floor(-3) == -3, "floor neg")
   assert(ceil(3) == 3, "ceil pos")
   assert(ceil(-3) == -3, "ceil neg")

   print("✓ std.math.mod tests passed")
}
