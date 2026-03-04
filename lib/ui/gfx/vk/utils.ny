;; Keywords: ui gfx vulkan renderer utils

module std.ui.gfx.vk.utils (
   __vkr_push_rect_tex_fast,
   _vkr_color_u32, __vkr_pack_color, _vkr_store_vertex, __vkr_push_vertex, __vkr_push_rect_tex,
   _init_quad_template, __vkr_push_rect, __vkr_push_line, __vkr_push_rect_sdf,
   _check_debug_env, _dbg_handle,
   _get_vertex_offset, _get_local_vertex_map, _advance_vertex_offset,
   _pack_color, _push_vertex
)

use std.core *
use std.core.mem *
use std.math *
use std.os *
use std.ui.gfx.vk.state *

@jit
fn __vkr_push_rect_tex_fast(ptr, x, y, w, h, u1, v1, u2, v2, color_u32, tex_id=0){
   memcpy(ptr, _quad_template, _VKR_VERT_STRIDE * 6)
   def x2 = float(x) + float(w)
   def y2 = float(y) + float(h)
   mut bv = ptr
   store32_f32(bv, float(x), _VKR_OFF_X)  store32_f32(bv, float(y), _VKR_OFF_Y)  store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, color_u32, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, float(x), _VKR_OFF_X)  store32_f32(bv, y2, _VKR_OFF_Y)        store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, color_u32, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)        store32_f32(bv, y2, _VKR_OFF_Y)        store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, color_u32, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)        store32_f32(bv, y2, _VKR_OFF_Y)        store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, color_u32, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)        store32_f32(bv, float(y), _VKR_OFF_Y)  store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, color_u32, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, float(x), _VKR_OFF_X)  store32_f32(bv, float(y), _VKR_OFF_Y)  store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, color_u32, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
}

fn _vkr_color_u32(c){
   "Internal: normalizes a color value to packed 32-bit form."
   if(is_int(c)){ return c }
   if(is_float(c)){ return __flt_to_int(c) }
   if(!is_list(c)){ return 0xFFFFFFFF }
   __vkr_pack_color(get(c, 0, 1.0), get(c, 1, 1.0), get(c, 2, 1.0), get(c, 3, 1.0))
}

@pure @jit
fn __vkr_pack_color(r, g, b, a){
   "Internal: packs RGBA float components into the renderer's native color format."
   def r8 = __flt_to_int(float(r) * 255.0) & 255
   def g8 = __flt_to_int(float(g) * 255.0) & 255
   def b8 = __flt_to_int(float(b) * 255.0) & 255
   def a8 = __flt_to_int(float(a) * 255.0) & 255
   (a8 << 24) | (b8 << 16) | (g8 << 8) | r8
}

@jit
fn _vkr_store_vertex(base, idx, x, y, z, u, v, color, tex_id=0, nx=0.0, ny=0.0, nz=1.0){
   "Internal: stores vertex `idx` into packed vertex buffer `base`."
   def off = base + idx * _VKR_VERT_STRIDE
   store32_f32(off, float(x), _VKR_OFF_X)
   store32_f32(off, float(y), _VKR_OFF_Y)
   store32_f32(off, float(z), _VKR_OFF_Z)
   store32_f32(off, float(u), _VKR_OFF_U)
   store32_f32(off, float(v), _VKR_OFF_V)
   store32(off, _vkr_color_u32(color), _VKR_OFF_C)
   store32(off, tex_id, _VKR_OFF_TEX)
   store32_f32(off, float(nx), _VKR_OFF_NX)
   store32_f32(off, float(ny), _VKR_OFF_NY)
   store32_f32(off, float(nz), _VKR_OFF_NZ)
}

@jit
fn __vkr_push_vertex(ptr, x, y, z, u, v, color, tex_id=0, nx=0.0, ny=0.0, nz=1.0){
   "Writes one packed vertex to `ptr`."
   if(!ptr){ return }
   _vkr_store_vertex(ptr, 0, x, y, z, u, v, color, tex_id, nx, ny, nz)
}

@jit
fn __vkr_push_rect_tex(ptr, x, y, w, h, u1, v1, u2, v2, color, tex_id=0, nz=1.0){
   "Writes a six-vertex textured quad to `ptr` using an optimized template path."
   if(!ptr){ return 0 }

   memcpy(ptr, _quad_template, _VKR_VERT_STRIDE * 6)

   def c = _vkr_color_u32(color)
   def x2 = float(x) + float(w)
   def y2 = float(y) + float(h)

   mut bv = ptr
   store32_f32(bv, float(x), _VKR_OFF_X)  store32_f32(bv, float(y), _VKR_OFF_Y)  store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, float(x), _VKR_OFF_X)  store32_f32(bv, y2, _VKR_OFF_Y)        store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)        store32_f32(bv, y2, _VKR_OFF_Y)        store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)        store32_f32(bv, y2, _VKR_OFF_Y)        store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)        store32_f32(bv, float(y), _VKR_OFF_Y)  store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, float(x), _VKR_OFF_X)  store32_f32(bv, float(y), _VKR_OFF_Y)  store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, tex_id, _VKR_OFF_TEX)
   0
}

fn _init_quad_template(){
   "Pre-fills a 6-vertex quad template with default values (Z=0, Normal=[0,0,1])."
   if(!_quad_template){ return }
   mut i = 0 while(i < 6){
      def off = _quad_template + i * _VKR_VERT_STRIDE
      store32_f32(off, 0.0, _VKR_OFF_Z) ; Z
      store32(off, 0, _VKR_OFF_TEX) ; Tex index
      store32_f32(off, 0.0, _VKR_OFF_NX) ; NX
      store32_f32(off, 0.0, _VKR_OFF_NY) ; NY
      store32_f32(off, 1.0, _VKR_OFF_NZ) ; NZ
      i += 1
   }
}

@jit
fn __vkr_push_rect(ptr, x, y, w, h, color){
   "Writes a six-vertex solid-color quad to `ptr`."
   __vkr_push_rect_tex(ptr, x, y, w, h, 0.0, 0.0, 0.0, 0.0, color, 0, 0.0)
}

fn __vkr_push_line(ptr, x1, y1, x2, y2, thickness, color){
   "Writes a six-vertex thick 2D line quad to `ptr`."
   if(!ptr){ return }
   def dx = float(x2) - float(x1)
   def dy = float(y2) - float(y1)
   def l = sqrt(dx*dx + dy*dy)
   if(l == 0.0){ return }
   def th = float(thickness) * 0.5
   def nx = -dy / l * th
   def ny =  dx / l * th
   _vkr_store_vertex(ptr, 0, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, 0, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 1, float(x1) - nx, float(y1) - ny, 0.0, 0.0, 0.0, color, 0, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 2, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, 0, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 3, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, 0, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 4, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, 0, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 5, float(x2) + nx, float(y2) + ny, 0.0, 0.0, 0.0, color, 0, 0.0, 0.0, 1.0)
}

@jit
fn __vkr_push_rect_sdf(ptr, x, y, w, h, c, nx, ny, nz){
   "Writes a six-vertex quad with UVs [0..1] and custom parameters."
   if(!ptr){ return 0 }
   memcpy(ptr, _quad_template, _VKR_VERT_STRIDE * 6)
   def x2 = float(x) + float(w)
   def y2 = float(y) + float(h)
   mut bv = ptr
   ; Vert 1
   store32_f32(bv, float(x), _VKR_OFF_X) store32_f32(bv, float(y), _VKR_OFF_Y) store32_f32(bv, 0.0, _VKR_OFF_U) store32_f32(bv, 0.0, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32_f32(bv, nx, _VKR_OFF_NX) store32_f32(bv, ny, _VKR_OFF_NY) store32_f32(bv, nz, _VKR_OFF_NZ) bv += _VKR_VERT_STRIDE
   ; Vert 2
   store32_f32(bv, float(x), _VKR_OFF_X) store32_f32(bv, y2, _VKR_OFF_Y) store32_f32(bv, 0.0, _VKR_OFF_U) store32_f32(bv, 1.0, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32_f32(bv, nx, _VKR_OFF_NX) store32_f32(bv, ny, _VKR_OFF_NY) store32_f32(bv, nz, _VKR_OFF_NZ) bv += _VKR_VERT_STRIDE
   ; Vert 3
   store32_f32(bv, x2, _VKR_OFF_X) store32_f32(bv, y2, _VKR_OFF_Y) store32_f32(bv, 1.0, _VKR_OFF_U) store32_f32(bv, 1.0, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32_f32(bv, nx, _VKR_OFF_NX) store32_f32(bv, ny, _VKR_OFF_NY) store32_f32(bv, nz, _VKR_OFF_NZ) bv += _VKR_VERT_STRIDE
   ; Vert 4
   store32_f32(bv, x2, _VKR_OFF_X) store32_f32(bv, y2, _VKR_OFF_Y) store32_f32(bv, 1.0, _VKR_OFF_U) store32_f32(bv, 1.0, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32_f32(bv, nx, _VKR_OFF_NX) store32_f32(bv, ny, _VKR_OFF_NY) store32_f32(bv, nz, _VKR_OFF_NZ) bv += _VKR_VERT_STRIDE
   ; Vert 5
   store32_f32(bv, x2, _VKR_OFF_X) store32_f32(bv, float(y), _VKR_OFF_Y) store32_f32(bv, 1.0, _VKR_OFF_U) store32_f32(bv, 0.0, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32_f32(bv, nx, _VKR_OFF_NX) store32_f32(bv, ny, _VKR_OFF_NY) store32_f32(bv, nz, _VKR_OFF_NZ) bv += _VKR_VERT_STRIDE
   ; Vert 6
   store32_f32(bv, float(x), _VKR_OFF_X) store32_f32(bv, float(y), _VKR_OFF_Y) store32_f32(bv, 0.0, _VKR_OFF_U) store32_f32(bv, 0.0, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32_f32(bv, nx, _VKR_OFF_NX) store32_f32(bv, ny, _VKR_OFF_NY) store32_f32(bv, nz, _VKR_OFF_NZ)
}

fn _check_debug_env(){
   "Internal: enables Vulkan/GFX debug from NY_UI_DEBUG."
   def v = env("NY_UI_DEBUG")
   if(v && (eq(v, "1") || eq(v, "true"))){ _debug_gfx_enabled = true }
   def b = env("NYTRIX_BINDLESS")
   if(b && (eq(b, "1") || eq(b, "true"))){ _bindless_enabled = true }
   if(b && (eq(b, "0") || eq(b, "false"))){ _bindless_enabled = false }
   def fast = env("NYTRIX_FAST")
   if(fast && (eq(fast, "1") || eq(fast, "true"))){
      if(!b){ _bindless_enabled = true }
   }
   def u = env("NYTRIX_UBO")
   def uf = env("NYTRIX_UBO_FORCE")
   def force_ubo = uf && (eq(uf, "1") || eq(uf, "true"))
   if(u && (eq(u, "1") || eq(u, "true"))){
      if(force_ubo){
         _ubo_enabled = true
      } else {
         _ubo_enabled = false
         if(_debug_gfx_enabled){ print("[gfx:vulkan] UBO requested but disabled (use NYTRIX_UBO_FORCE=1 to force)") }
      }
   }
   if(u && (eq(u, "0") || eq(u, "false"))){ _ubo_enabled = false }
   def rdoc = env("RENDERDOC") || env("RENDERDOC_CAPTUREOPTS") || env("RENDERDOC_CMD")
   if(rdoc){
      def b_env = env("NYTRIX_BINDLESS")
      mut force_bindless = (b_env && (eq(b_env, "1") || eq(b_env, "true")))
      if(force_bindless){
         if(_debug_gfx_enabled){ print("[gfx:vulkan] Bindless FORCED ON under RenderDoc") }
         _bindless_enabled = true
      } else {
         _bindless_enabled = false
         if(_debug_gfx_enabled){ print("[gfx:vulkan] Bindless auto-disabled for RenderDoc compatibility. (Use NYTRIX_BINDLESS=1 to force)") }
      }
   }
}
fn _dbg_handle(label, h){
   "Internal: prints a labeled Vulkan handle when debug logging is enabled."
   if(_debug_gfx_enabled){ print(f"[gfx:vulkan] {label} h={h}") }
   0
}

mut _cfg_msaa = 1
fn _get_vertex_offset(){
   "Returns the current packed-vertex write offset in bytes."
   _vertex_offset
}
fn _get_local_vertex_map(){
   "Returns the current CPU-visible vertex buffer mapping."
   _local_vertex_map
}
fn _advance_vertex_offset(bytes){
   "Advances the packed-vertex write offset by `bytes` and returns the new value."
   _vertex_offset += bytes
}

@pure @jit
fn _pack_color(r, g, b, a){
   "Packs RGBA floats [0,1] into a uint32 (R8G8B8A8 for UNORM attribute)."
   (int(r * 255.0) & 0xFF) | ((int(g * 255.0) & 0xFF) << 8) | ((int(b * 255.0) & 0xFF) << 16) | ((int(a * 255.0) & 0xFF) << 24)
}

@jit
fn _push_vertex(x, y, z, u, v, r, g, b, a, tex_id=0){
   "Appends a single vertex (packed stride) to the current batch."
   def off = _local_vertex_map + _vertex_offset
   ; Ensure we use raw floats to avoid object tagging artifacts in the buffer.
   store32_f32(off, float(x), _VKR_OFF_X)
   store32_f32(off, float(y), _VKR_OFF_Y)
   store32_f32(off, float(z), _VKR_OFF_Z)
   store32_f32(off, float(u), _VKR_OFF_U)
   store32_f32(off, float(v), _VKR_OFF_V)
   store32(off, _pack_color(r, g, b, a), _VKR_OFF_C)
   store32(off, tex_id, _VKR_OFF_TEX)
   store32_f32(off, 0.0, _VKR_OFF_NX)
   store32_f32(off, 0.0, _VKR_OFF_NY)
   store32_f32(off, 1.0, _VKR_OFF_NZ)
   _vertex_offset += _VKR_VERT_STRIDE
}
