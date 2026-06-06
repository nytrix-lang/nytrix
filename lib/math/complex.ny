;; Keywords: complex complex-numbers math
;; Native complex number facade for Nytrix.
;; References:
;; - std.math
module std.math.complex(complex, Complex, c64, c128, is_complex, real, imag, re, im, conj, abs2, add, sub, mul, div, same, different)
use std.core

fn complex(any real_part=0.0, any imag_part=0.0) complex {
   "Creates a boxed native complex value backed by the compiler's complex ABI type."
   __complex_new(real_part, imag_part)
}

fn Complex(any real_part=0.0, any imag_part=0.0) complex {
   "Alias for complex(real, imag)."
   complex(real_part, imag_part)
}

fn c128(any real_part=0.0, any imag_part=0.0) c128 {
   "Creates a double-precision complex value."
   __complex_new(real_part, imag_part)
}

fn c64(any real_part=0.0, any imag_part=0.0) c64 {
   "Creates a single-precision complex value; ABI boundaries narrow it."
   __complex_new(real_part, imag_part)
}

fn is_complex(any z) bool {
   "Returns true for native complex values."
   __is_complex_obj(z)
}

fn real(complex z) f64 {
   "Returns the real component."
   __complex_real(z)
}

fn imag(complex z) f64 {
   "Returns the imaginary component."
   __complex_imag(z)
}

fn re(complex z) f64 { real(z) }

fn im(complex z) f64 { imag(z) }

fn conj(complex z) complex {
   "Returns the complex conjugate."
   __complex_conj(z)
}

fn abs2(complex z) f64 {
   "Returns squared magnitude, real(z)^2 + imag(z)^2."
   __complex_abs2(z)
}

fn add(complex a, complex b) complex { __complex_add(a, b) }

fn sub(complex a, complex b) complex { __complex_sub(a, b) }

fn mul(complex a, complex b) complex { __complex_mul(a, b) }

fn div(complex a, complex b) complex { __complex_div(a, b) }

fn same(complex a, complex b) bool { __complex_eq(a, b) }

fn different(complex a, complex b) bool {
   "Returns true when complex numbers differ."
   !same(a, b)
}

impl complex {
   fn real(complex z) f64 { __complex_real(z) }
   fn imag(complex z) f64 { __complex_imag(z) }
   fn re(complex z) f64 { __complex_real(z) }
   fn im(complex z) f64 { __complex_imag(z) }
   fn conj(complex z) complex { __complex_conj(z) }
   fn abs2(complex z) f64 { __complex_abs2(z) }
   fn add(complex a, complex b) complex { __complex_add(a, b) }
   fn sub(complex a, complex b) complex { __complex_sub(a, b) }
   fn mul(complex a, complex b) complex { __complex_mul(a, b) }
   fn div(complex a, complex b) complex { __complex_div(a, b) }
   fn same(complex a, complex b) bool { __complex_eq(a, b) }
   fn different(complex a, complex b) bool { !__complex_eq(a, b) }
   operator + complex: complex = add
   operator - complex: complex = sub
   operator * complex: complex = mul
   operator / complex: complex = div
   operator == complex: bool = same
   operator != complex: bool = different
}
