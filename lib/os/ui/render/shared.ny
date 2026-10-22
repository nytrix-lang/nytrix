;; Keywords: render shared vertex color matrix os ui
;; Backend-neutral render helpers shared by Vulkan, OpenGL, and software paths.
;; References:
;; - core.os.ui.render
;; - core.os.ui.render.vk
module core.os.ui.render.shared(
   VERTEX_STRIDE, OFF_X, OFF_Y, OFF_Z, OFF_U, OFF_V, OFF_C, OFF_NX, OFF_NY, OFF_NZ,
   OFF_TX, OFF_TY, OFF_TZ, OFF_TW, OFF_U2, OFF_V2, OFF_TEX,
   GLYPH_STRIDE, GLYPH_ADV, GLYPH_XOFF, GLYPH_YOFF, GLYPH_BW, GLYPH_BH,
   GLYPH_U1, GLYPH_V1, GLYPH_U2, GLYPH_V2, GLYPH_TEX, GLYPH_PRESENT, GLYPH_IS_COLOR,
   MAX_TEXTURES, SCENE_LIGHT_MAX,
   MATERIAL_U7_MAX, BUILTIN_GLYPH_WIDTH, BUILTIN_GLYPH_HEIGHT,
   BUILTIN_GLYPH_ROW_MASK,
   safe_f32_limit, pack_rgba_u32, color_component, color_u32, store_mat4_cm_raw, store_vertex64, store_vertex64_typed,
   push_vertex64
)

use core
use core.math
use core.math.float as fmath

def VERTEX_STRIDE = 64
def OFF_X = 0
def OFF_Y = 4
def OFF_Z = 8
def OFF_U = 12
def OFF_V = 16
def OFF_C = 20
def OFF_NX = 24
def OFF_NY = 28
def OFF_NZ = 32
def OFF_TX = 36
def OFF_TY = 40
def OFF_TZ = 44
def OFF_TW = 48
def OFF_U2 = 52
def OFF_V2 = 56
def OFF_TEX = 60
def GLYPH_STRIDE = 48
def GLYPH_ADV = 0
def GLYPH_XOFF = 4
def GLYPH_YOFF = 8
def GLYPH_BW = 12
def GLYPH_BH = 16
def GLYPH_U1 = 20
def GLYPH_V1 = 24
def GLYPH_U2 = 28
def GLYPH_V2 = 32
def GLYPH_TEX = 36
def GLYPH_PRESENT = 40
def GLYPH_IS_COLOR = 44
def MAX_TEXTURES = 4096
def SCENE_LIGHT_MAX = 8

; Packed scene material fields reserve seven bits for unit-interval scales.
def MATERIAL_U7_MAX = (1 << 7) - 1

; The fallback bitmap font stores each 5x7 glyph as seven five-bit rows.
def BUILTIN_GLYPH_WIDTH = 5
def BUILTIN_GLYPH_HEIGHT = 7
def BUILTIN_GLYPH_ROW_MASK = (1 << BUILTIN_GLYPH_WIDTH) - 1

;; Returns the result of the `safe_f32_limit` operation.
fn safe_f32_limit(any v, f64 fallback=0.0, f64 limit=1048576.0) f64 {
   def fv = fmath.float(v)
   if fmath.is_nan(fv) || fmath.is_inf(fv) { return fallback }
   if fv > limit { return limit }
   if fv < 0.0 - limit { return 0.0 - limit }
   fv
}

@pure
@jit

;; Returns the result of the `pack_rgba_u32` operation.
fn pack_rgba_u32(any r, any g, any b, any a) int {
   (int(float(r) * 255.0) & 0xFF) |
   ((int(float(g) * 255.0) & 0xFF) << 8) |
   ((int(float(b) * 255.0) & 0xFF) << 16) |
   ((int(float(a) * 255.0) & 0xFF) << 24)
}

fn color_component(any color, int idx, f64 fallback=1.0) f64 {
   "Reads RGBA from a list, a vec4 dictionary, or a packed little-endian RGBA word."
   if idx < 0 || idx > 3 { return fallback }
   if is_list(color) { return float(color.get(idx, fallback)) }
   if is_dict(color) {
      def key = case idx { 0 -> "x" 1 -> "y" 2 -> "z" _ -> "w" }
      return float(color.get(key, fallback))
   }
   if is_int(color) {
      def packed = int(color)
      return float((packed >> (idx * 8)) & 255) / 255.0
   }
   fallback
}

;; Returns the result of the `color_u32` operation.
fn color_u32(any c) int {
   if is_int(c) { return c }
   if is_float(c) { return __flt_to_int(c) }
   pack_rgba_u32(color_component(c, 0), color_component(c, 1), color_component(c, 2), color_component(c, 3))
}

@inline
fn store_mat4_cm_raw(any dst, any mat, bool allow_plain16=false) bool {
   "Stores tagged column-major mat4, optionally accepting a plain 16-float list."
   if !dst || !is_list(mat) { return false }
   def n = mat.len
   if n == 18 {
      if int(__load_item_fast(mat, 0)) != 4 || int(__load_item_fast(mat, 1)) != 4 { return false }
      mut i = 0
      while i < 16 {
         store32_f32(dst, __load_item_fast(mat, 2 + i), i * 4)
         i += 1
      }
      return true
   }
   if allow_plain16 && n == 16 {
      mut i = 0
      while i < 16 {
         store32_f32(dst, __load_item_fast(mat, i), i * 4)
         i += 1
      }
      return true
   }
   false
}

@jit

;; Returns the result of the `store_vertex64` operation.
fn store_vertex64(ptr base, int idx, any x, any y, any z, any u, any v, any color, any tex_id=0, any nx=0.0, any ny=0.0, any nz=1.0) any {
   store_vertex64_typed(
      base, idx,
      safe_f32_limit(x), safe_f32_limit(y), safe_f32_limit(z),
      safe_f32_limit(u), safe_f32_limit(v), color_u32(color), int(tex_id),
      safe_f32_limit(nx), safe_f32_limit(ny), safe_f32_limit(nz, 1.0),
   )
}

@jit
fn store_vertex64_typed(ptr base, int idx, f64 x, f64 y, f64 z, f64 u, f64 v, int color, int tex_id=0, f64 nx=0.0, f64 ny=0.0, f64 nz=1.0) any {
   def off = base + idx * VERTEX_STRIDE
   store32_f32(off, _safe_f32_limit_f64(x), OFF_X)
   store32_f32(off, _safe_f32_limit_f64(y), OFF_Y)
   store32_f32(off, _safe_f32_limit_f64(z), OFF_Z)
   store32_f32(off, _safe_f32_limit_f64(u), OFF_U)
   store32_f32(off, _safe_f32_limit_f64(v), OFF_V)
   store32(off, color, OFF_C)
   store32(off, tex_id, OFF_TEX)
   store32_f32(off, _safe_f32_limit_f64(nx), OFF_NX)
   store32_f32(off, _safe_f32_limit_f64(ny), OFF_NY)
   store32_f32(off, _safe_f32_limit_f64(nz, 1.0), OFF_NZ)
   store32_f32(off, 1.0, OFF_TX)
   store32_f32(off, 0.0, OFF_TY)
   store32_f32(off, 0.0, OFF_TZ)
   store32_f32(off, 1.0, OFF_TW)
   store32_f32(off, 0.0, OFF_U2)
   store32_f32(off, 0.0, OFF_V2)
}

fn _safe_f32_limit_f64(f64 v, f64 fallback=0.0, f64 limit=1048576.0) f64 {
   def self_difference = v - v
   if v != v || self_difference != self_difference { return fallback }
   if v > limit { return limit }
   if v < 0.0 - limit { return 0.0 - limit }
   v
}

@jit

;; Writes the vertex64 and returns the result.
fn push_vertex64(ptr p, any x, any y, any z, any u, any v, any color, any tex_id=0, any nx=0.0, any ny=0.0, any nz=1.0) any {
   if !p { return 0 }
   store_vertex64(p, 0, x, y, z, u, v, color, tex_id, nx, ny, nz)
}
