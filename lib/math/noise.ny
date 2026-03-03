;; Keywords: math noise perlin
;; Perlin Noise Implementation
;; Reference:
;; - https://github.com/nothings/stb/blob/master/stb_perlin.h

module std.math.noise (
   perlin3, perlin3_seed,
   fbm3, turbulence3
)

use std.core *
use std.math *

def _perm = [
   23, 125, 161, 52, 103, 117, 70, 37, 247, 101, 203, 169, 124, 126, 44, 123,
   152, 238, 145, 45, 171, 114, 253, 10, 192, 136, 4, 157, 249, 30, 35, 72,
   175, 63, 77, 90, 181, 16, 96, 111, 133, 104, 75, 162, 93, 56, 66, 240,
   8, 50, 84, 229, 49, 210, 173, 239, 141, 1, 87, 18, 2, 198, 143, 57,
   225, 160, 58, 217, 168, 206, 245, 204, 199, 6, 73, 60, 20, 230, 211, 233,
   94, 200, 88, 9, 74, 155, 33, 15, 219, 130, 226, 202, 83, 236, 42, 172,
   165, 218, 55, 222, 46, 107, 98, 154, 109, 67, 196, 178, 127, 158, 13, 243,
   65, 79, 166, 248, 25, 224, 115, 80, 68, 51, 184, 128, 232, 208, 151, 122,
   26, 212, 105, 43, 179, 213, 235, 148, 146, 89, 14, 195, 28, 78, 112, 76,
   250, 47, 24, 251, 140, 108, 186, 190, 228, 170, 183, 139, 39, 188, 244, 246,
   132, 48, 119, 144, 180, 138, 134, 193, 82, 182, 120, 121, 86, 220, 209, 3,
   91, 241, 149, 85, 205, 150, 113, 216, 31, 100, 41, 164, 177, 214, 153, 231,
   38, 71, 185, 174, 97, 201, 29, 95, 7, 92, 54, 254, 191, 118, 34, 221,
   131, 11, 163, 99, 234, 81, 227, 147, 156, 176, 17, 142, 69, 12, 110, 62,
   27, 255, 0, 194, 59, 116, 242, 252, 19, 21, 187, 53, 207, 129, 64, 135,
   61, 40, 167, 237, 102, 223, 106, 159, 197, 189, 215, 137, 36, 32, 22, 5
]

def _grad_idx = [
   7, 9, 5, 0, 11, 1, 6, 9, 3, 9, 11, 1, 8, 10, 4, 7,
   8, 6, 1, 5, 3, 10, 9, 10, 0, 8, 4, 1, 5, 2, 7, 8,
   7, 11, 9, 10, 1, 0, 4, 7, 5, 0, 11, 6, 1, 4, 2, 8,
   8, 10, 4, 9, 9, 2, 5, 7, 9, 1, 7, 2, 2, 6, 11, 5,
   5, 4, 6, 9, 0, 1, 1, 0, 7, 6, 9, 8, 4, 10, 3, 1,
   2, 8, 8, 9, 10, 11, 5, 11, 11, 2, 6, 10, 3, 4, 2, 4,
   9, 10, 3, 2, 6, 3, 6, 10, 5, 3, 4, 10, 11, 2, 9, 11,
   1, 11, 10, 4, 9, 4, 11, 0, 4, 11, 4, 0, 0, 0, 7, 6,
   10, 4, 1, 3, 11, 5, 3, 4, 2, 9, 1, 3, 0, 1, 8, 0,
   6, 7, 8, 7, 0, 4, 6, 10, 8, 2, 3, 11, 11, 8, 0, 2,
   4, 8, 3, 0, 0, 10, 6, 1, 2, 2, 4, 5, 6, 0, 1, 3,
   11, 9, 5, 5, 9, 6, 9, 8, 3, 8, 1, 8, 9, 6, 9, 11,
   10, 7, 5, 6, 5, 9, 1, 3, 7, 0, 2, 10, 11, 2, 6, 1,
   3, 11, 7, 7, 2, 1, 7, 3, 0, 8, 1, 1, 5, 0, 6, 10,
   11, 11, 0, 2, 7, 0, 10, 8, 3, 5, 7, 1, 11, 1, 0, 7,
   9, 0, 11, 5, 10, 3, 2, 3, 5, 9, 7, 9, 8, 4, 6, 5
]

def _grads = [
   [1, 1, 0], [-1, 1, 0], [1, -1, 0], [-1, -1, 0],
   [1, 0, 1], [-1, 0, 1], [1, 0, -1], [-1, 0, -1],
   [0, 1, 1], [0, -1, 1], [0, 1, -1], [0, -1, -1]
]

fn _lerp(a, b, t){
   "Internal: performs linear interpolation between `a` and `b` with weight `t`."
   a + (b - a) * t
}
fn _ease(t){
   "Internal: implements the quintic s-curve (6t^5 - 15t^4 + 10t^3) for smooth interpolation."
   t * t * t * (t * (t * 6 - 15) + 10)
}

fn _grad(idx, x, y, z){
   "Internal: computes the dot product of a pre-defined gradient vector with the distance vector (x, y, z)."
   def g = get(_grads, idx)
   get(g, 0) * x + get(g, 1) * y + get(g, 2) * z
}

fn perlin3_seed(x, y, z, seed=0){
   "Computes 3D Perlin noise value at (x, y, z) with a specific `seed`."
   def px = floor(x)
   def py = floor(y)
   def pz = floor(z)
   def x0 = to_int(px) & 255
   def x1 = (x0 + 1) & 255
   def y0 = to_int(py) & 255
   def y1 = (y0 + 1) & 255
   def z0 = to_int(pz) & 255
   def z1 = (z0 + 1) & 255
   def xf = x - px
   def yf = y - py
   def zf = z - pz
   def u = _ease(xf)
   def v = _ease(yf)
   def w = _ease(zf)
   def s = seed & 255
   def r0 = get(_perm, (x0 + s) & 255)
   def r1 = get(_perm, (x1 + s) & 255)
   def r00 = get(_perm, (r0 + y0) & 255)
   def r01 = get(_perm, (r0 + y1) & 255)
   def r10 = get(_perm, (r1 + y0) & 255)
   def r11 = get(_perm, (r1 + y1) & 255)
   def n000 = _grad(get(_grad_idx, (r00 + z0) & 255), xf, yf, zf)
   def n001 = _grad(get(_grad_idx, (r00 + z1) & 255), xf, yf, zf - 1)
   def n010 = _grad(get(_grad_idx, (r01 + z0) & 255), xf, yf - 1, zf)
   def n011 = _grad(get(_grad_idx, (r01 + z1) & 255), xf, yf - 1, zf - 1)
   def n100 = _grad(get(_grad_idx, (r10 + z0) & 255), xf - 1, yf, zf)
   def n101 = _grad(get(_grad_idx, (r10 + z1) & 255), xf - 1, yf, zf - 1)
   def n110 = _grad(get(_grad_idx, (r11 + z0) & 255), xf - 1, yf - 1, zf)
   def n111 = _grad(get(_grad_idx, (r11 + z1) & 255), xf - 1, yf - 1, zf - 1)
   def n00 = _lerp(n000, n001, w)
   def n01 = _lerp(n010, n011, w)
   def n10 = _lerp(n100, n101, w)
   def n11 = _lerp(n110, n111, w)
   def n0 = _lerp(n00, n01, v)
   def n1 = _lerp(n10, n11, v)
   _lerp(n0, n1, u)
}

fn perlin3(x, y, z){
   "Computes 3D Perlin noise value at coordinates (x, y, z). Returns a value approximately in the range [-1.0, 1.0]."
   perlin3_seed(x, y, z, 0)
}

fn fbm3(x, y, z, lacunarity=2.0, gain=0.5, octaves=6){
   "Computes 3D Fractal Brownian Motion noise (sum of octaves of Perlin noise with increasing frequency and decreasing amplitude)."
   mut sum = 0.0
   mut freq = 1.0
   mut amp = 1.0
   mut i = 0
   while(i < octaves){
      sum = sum + perlin3_seed(x * freq, y * freq, z * freq, i) * amp
      freq = freq * lacunarity
      amp = amp * gain
      i += 1
   }
   sum
}

fn turbulence3(x, y, z, lacunarity=2.0, gain=0.5, octaves=6){
   "Computes 3D Turbulence noise (sum of absolute values of octaves of Perlin noise), creating sharper features like ridges."
   mut sum = 0.0
   mut freq = 1.0
   mut amp = 1.0
   mut i = 0
   while(i < octaves){
      sum = sum + abs(perlin3_seed(x * freq, y * freq, z * freq, i)) * amp
      freq = freq * lacunarity
      amp = amp * gain
      i += 1
   }
   sum
}

if(comptime{__main()}){
   def n = perlin3(0.5, 0.5, 0.5)
   print(f"Perlin(0.5, 0.5, 0.5) = {n}")
   assert(n != 0, "perlin value")
   print("✓ std.math.noise tests passed")
}
