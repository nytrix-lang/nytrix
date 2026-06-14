;; Keywords: math scalar integer float bigint rational complex vector matrix ring finite-field polynomial random statistics simd number-theory
;; Math facade: scalar functions, constants, complex numbers, vectors, matrices, rings, modular integers, and polynomials.
;; References:
;; - std
module std.math(backends,
   PI, PHI, E, TAU, LN2, LN10,
   abs, min, max, pow, mod, clamp, clamp01, sign, sqrt, gcd, lcm, factorial, lerp,
   sin, cos, tan, asin, acos, atan, atan2,
   log, log2, log10, exp,
   floor, ceil, round, fmod,
   Complex, complex, c64, c128, is_complex, real, imag, re, im, conj, abs2,
   ring, Zmod, Integers, ZmodElem, zmod_ring, zmod_elem, is_zmod_ring, is_zmod,
PolynomialRing, Poly, poly_ring, poly_elem, is_poly_ring, is_poly_elem)

use std.core
use std.math.float
use std.math.ring
use std.math.backends
use std.math.complex as cmplx

def f64 PI   = 3.14159265358979323846
def f64 PHI  = 1.61803398874989484820
def f64 E    = 2.71828182845904523536
def f64 TAU  = 2.0 * PI
def f64 LN2  = 0.69314718055994530941
def f64 LN10 = 2.30258509299404568402

fn abs(number x) number {
   "Absolute value."
   if is_float(x) {
      if flt(float(x), float(0)) { return fsub(float(0), float(x)) }
      return float(x)
   }
   if x < 0 { return 0 - x }
   return x
}

fn min(number a, number b) number {
   "Minimum of two values."
   if a < b { return a }
   return b
}

fn max(number a, number b) number {
   "Maximum of two values."
   if a > b { return a }
   return b
}

fn pow(number a, number b) number {
   "Power: a^b."
   if is_float(a) || is_float(b) { return __flt_pow(float(a), float(b)) }
   if b == 0 { return 1.0 }
   if b < 0 { return 1.0 / pow(a, -b) }
   mut res = 1.0
   mut i = 0
   while i < b {
      res = res * a
      i += 1
   }
   return res
}

fn mod(number a, number b) number {
   "Modulo: a mod b."
   if is_float(a) || is_float(b) { return __flt_fmod(float(a), float(b)) }
   mut res = a - (a / b) * b
   if res < 0 { res = res + abs(b) }
   return res
}

fn clamp(number x, number lo, number hi) number {
   "Clamp x to [lo, hi]."
   if x < lo { return lo }
   if x > hi { return hi }
   return x
}

fn clamp01(number x) f64 {
   "Clamp x to [0.0, 1.0]."
   def v = float(x)
   if v < 0.0 { return 0.0 }
   if v > 1.0 { return 1.0 }
   v
}

fn sign(number x) int {
   "Sign of x: -1, 0, or 1."
   if x == 0 { return 0 }
   if x < 0 { return -1 }
   return 1
}

fn sqrt(number x) f64 {
   "Square root via Newton's method."
   if x == 0 { return 0.0 }
   __flt_sqrt(float(x))
}

fn gcd(number a, number b) number {
   "Greatest common divisor."
   mut x, y = abs(a), abs(b)
   while y != 0 {
      def t = x % y
      x, y = y, t
   }
   return x
}

fn lcm(number a, number b) number {
   "Least common multiple."
   if a == 0 || b == 0 { return 0 }
   return abs((a / gcd(a, b)) * b)
}

fn factorial(number n) number {
   "Factorial: n!"
   if n <= 1 { return 1 }
   mut res = 1
   mut i = 2
   while i <= n {
      res = res * i
      i += 1
   }
   return res
}

fn _math_lerp(number a, number b, number t) number { return a + (b - a) * t }

fn lerp(number a, number b, number t) number {
   "Linear interpolation: a + (b-a)*t."
   _math_lerp(a, b, t)
}

fn complex(any real_part=0.0, any imag_part=0.0) complex { cmplx.complex(real_part, imag_part) }

fn Complex(any real_part=0.0, any imag_part=0.0) complex { cmplx.Complex(real_part, imag_part) }

fn c64(any real_part=0.0, any imag_part=0.0) c64 { cmplx.c64(real_part, imag_part) }

fn c128(any real_part=0.0, any imag_part=0.0) c128 { cmplx.c128(real_part, imag_part) }

fn is_complex(any z) bool { cmplx.is_complex(z) }

fn real(complex z) f64 { cmplx.real(z) }

fn imag(complex z) f64 { cmplx.imag(z) }

fn re(complex z) f64 { cmplx.re(z) }

fn im(complex z) f64 { cmplx.im(z) }

fn conj(complex z) complex { cmplx.conj(z) }

fn abs2(complex z) f64 { cmplx.abs2(z) }
def f64 _HALF_PI = PI / 2.0

fn sin(number x) f64 {
   "Sine(radians)."
   __flt_sin(float(x))
}

fn cos(number x) f64 {
   "Cosine(radians)."
   __flt_cos(float(x))
}

fn tan(number x) f64 {
   "Tangent(radians)."
   __flt_tan(float(x))
}

fn atan(number x) f64 {
   "Arc tangent(radians)."
   __flt_atan(float(x))
}

fn atan2(number y, number x) f64 {
   "Arc tangent with quadrant handling."
   __flt_atan2(float(y), float(x))
}

fn asin(number x) f64 {
   "Arc sine(radians)."
   __flt_asin(clamp(float(x), -1.0, 1.0))
}

fn acos(number x) f64 {
   "Arc cosine(radians)."
   __flt_acos(clamp(float(x), -1.0, 1.0))
}

fn exp(number x) f64 {
   "Natural exponential: e^x."
   __flt_exp(float(x))
}

fn log(number x) f64 {
   "Natural logarithm."
   __flt_log(float(x))
}

fn log2(number x) f64 {
   "Base-2 logarithm."
   __flt_log2(float(x))
}

fn log10(number x) f64 {
   "Base-10 logarithm."
   __flt_log10(float(x))
}

fn fmod(number a, number b) f64 {
   "Floating point modulo."
   __flt_fmod(float(a), float(b))
}

fn floor(number x) f64 {
   "Floor function."
   __flt_floor(float(x))
}

fn ceil(number x) f64 {
   "Ceiling function."
   __flt_ceil(float(x))
}

fn round(number x) f64 {
   "Round to nearest integer."
   __flt_round(float(x))
}

impl int, f64, f32 {
   @inline
   fn abs(self x) number { abs(x) }
   @inline
   fn min(self x, number y) number { min(x, y) }
   @inline
   fn max(self x, number y) number { max(x, y) }
   @inline
   fn pow(self x, number y) number { pow(x, y) }
   @inline
   fn mod(self x, number y) number { mod(x, y) }
   @inline
   fn clamp(self x, number lo, number hi) number { clamp(x, lo, hi) }
   @inline
   fn clamp01(self x) f64 { clamp01(x) }
   @inline
   fn sign(self x) int { sign(x) }
   @inline
   fn sqrt(self x) f64 { sqrt(x) }
   @inline
   fn lerp(self x, number y, number t) number { _math_lerp(x, y, t) }
   @inline
   fn sin(self x) f64 { sin(x) }
   @inline
   fn cos(self x) f64 { cos(x) }
   @inline
   fn tan(self x) f64 { tan(x) }
   @inline
   fn atan(self x) f64 { atan(x) }
   @inline
   fn asin(self x) f64 { asin(x) }
   @inline
   fn acos(self x) f64 { acos(x) }
   @inline
   fn exp(self x) f64 { exp(x) }
   @inline
   fn log(self x) f64 { log(x) }
   @inline
   fn log2(self x) f64 { log2(x) }
   @inline
   fn log10(self x) f64 { log10(x) }
   @inline
   fn fmod(self x, number y) f64 { fmod(x, y) }
   @inline
   fn floor(self x) f64 { floor(x) }
   @inline
   fn ceil(self x) f64 { ceil(x) }
   @inline
   fn round(self x) f64 { round(x) }
}

impl int {
   @inline
   fn gcd(self x, number y) number { gcd(x, y) }
   @inline
   fn lcm(self x, number y) number { lcm(x, y) }
   @inline
   fn factorial(self x) number { factorial(x) }
}
