;; Keywords: render shared vertex color matrix os ui
;; Backend-neutral render helpers shared by Vulkan, OpenGL, and software paths.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.vk
module std.os.ui.render.shared(
   VERTEX_STRIDE, OFF_X, OFF_Y, OFF_Z, OFF_U, OFF_V, OFF_C, OFF_NX, OFF_NY, OFF_NZ,
   OFF_TX, OFF_TY, OFF_TZ, OFF_TW, OFF_U2, OFF_V2, OFF_TEX,
   GLYPH_STRIDE, GLYPH_ADV, GLYPH_XOFF, GLYPH_YOFF, GLYPH_BW, GLYPH_BH,
   GLYPH_U1, GLYPH_V1, GLYPH_U2, GLYPH_V2, GLYPH_TEX, GLYPH_PRESENT, GLYPH_IS_COLOR,
   MAX_TEXTURES, SCENE_LIGHT_MAX,
   safe_f32_limit, pack_rgba_u32, color_u32, store_mat4_cm_raw, store_vertex64,
   push_vertex64
)

use std.core
use std.math
use std.math.float as fmath

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

fn safe_f32_limit(any v, f64 fallback=0.0, f64 limit=1048576.0) f64 {
   def fv = fmath.float(v)
   if(fmath.is_nan(fv) || fmath.is_inf(fv)){ return fallback }
   if(fv > limit){ return limit }
   if(fv < 0.0 - limit){ return 0.0 - limit }
   fv
}

@pure
@jit
fn pack_rgba_u32(any r, any g, any b, any a) int {
   (int(float(r) * 255.0) & 0xFF) |
   ((int(float(g) * 255.0) & 0xFF) << 8) |
   ((int(float(b) * 255.0) & 0xFF) << 16) |
   ((int(float(a) * 255.0) & 0xFF) << 24)
}

fn color_u32(any c) int {
   if(is_int(c)){ return c }
   if(is_float(c)){ return __flt_to_int(c) }
   if(!is_list(c)){ return 0xFFFFFFFF }
   pack_rgba_u32(c.get(0, 1.0), c.get(1, 1.0), c.get(2, 1.0), c.get(3, 1.0))
}

fn store_mat4_cm_raw(any dst, any mat, bool allow_plain16=false) bool {
   "Stores tagged column-major mat4, optionally accepting a plain 16-float list."
   if(!dst || !is_list(mat)){ return false }
   def n = mat.len
   if(n == 18){
      if(int(__load_item_fast(mat, 0)) != 4 || int(__load_item_fast(mat, 1)) != 4){ return false }
      mut i = 0
      while(i < 16){
         store32_f32(dst, __load_item_fast(mat, 2 + i), i * 4)
         i += 1
      }
      return true
   }
   if(allow_plain16 && n == 16){
      mut i = 0
      while(i < 16){
         store32_f32(dst, __load_item_fast(mat, i), i * 4)
         i += 1
      }
      return true
   }
   false
}

@jit
fn store_vertex64(any base, int idx, any x, any y, any z, any u, any v, any color, any tex_id=0, any nx=0.0, any ny=0.0, any nz=1.0) any {
   def off = base + idx * VERTEX_STRIDE
   store32_f32(off, safe_f32_limit(x), OFF_X)
   store32_f32(off, safe_f32_limit(y), OFF_Y)
   store32_f32(off, safe_f32_limit(z), OFF_Z)
   store32_f32(off, safe_f32_limit(u), OFF_U)
   store32_f32(off, safe_f32_limit(v), OFF_V)
   store32(off, color_u32(color), OFF_C)
   store32(off, tex_id, OFF_TEX)
   store32_f32(off, safe_f32_limit(nx), OFF_NX)
   store32_f32(off, safe_f32_limit(ny), OFF_NY)
   store32_f32(off, safe_f32_limit(nz, 1.0), OFF_NZ)
   store32_f32(off, 1.0, OFF_TX)
   store32_f32(off, 0.0, OFF_TY)
   store32_f32(off, 0.0, OFF_TZ)
   store32_f32(off, 1.0, OFF_TW)
   store32_f32(off, 0.0, OFF_U2)
   store32_f32(off, 0.0, OFF_V2)
}

@jit
fn push_vertex64(any p, any x, any y, any z, any u, any v, any color, any tex_id=0, any nx=0.0, any ny=0.0, any nz=1.0) any {
   if(!p){ return 0 }
   store_vertex64(p, 0, x, y, z, u, v, color, tex_id, nx, ny, nz)
}
