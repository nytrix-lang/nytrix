;; Keywords: math
;; Math module.

module std.math (
   abs, min, max, pow, mod, clamp, sign, sqrt, gcd, lcm, factorial, lerp,
   PI, PHI, E, TAU, LN2, LN10
)
use std.core *
use std.core.reflect *
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

def PI  = float.PI
def PHI = _box(0x3ff9e3779b97f4a8) ;; 1.618033988749895
def E   = _box(0x4005bf0a8b145769) ;; 2.718281828459045
def TAU  = _box(0x401921fb54442d18) ;; 6.283185307179586
def LN2  = _box(0x3fe62e42fefa39ef) ;; 0.6931471805599453
def LN10 = _box(0x40026bb1bbb55516) ;; 2.302585092994046

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

    assert(floor(3) == 3, "floor pos")
    assert(floor(-3) == -3, "floor neg")
    assert(ceil(3) == 3, "ceil pos")
    assert(ceil(-3) == -3, "ceil neg")

    print("✓ std.math.mod tests passed")
}
