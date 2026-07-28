use std.core
use std.math.complex

def re_bits = __flt_unbox_val(1.0)
def im_bits = __flt_unbox_val(2.0)
def z = __complex_new_bits(re_bits, im_bits)
assert(type(z) == "complex", "__complex_new_bits returns complex")
assert(real(z) == 1.0, "__complex_new_bits sets real component")
assert(imag(z) == 2.0, "__complex_new_bits sets imaginary component")
assert(__complex_re_bits(z) == re_bits, "__complex_re_bits roundtrips real bits")
assert(__complex_im_bits(z) == im_bits, "__complex_im_bits roundtrips imaginary bits")
print("✓ runtime complex tests passed")
