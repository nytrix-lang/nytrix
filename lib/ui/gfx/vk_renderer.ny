;; Keywords: ui gfx vulkan renderer
;; Vulkan 2D Renderer for Nytrix

module std.ui.gfx.vk_renderer (
   init, shutdown,
   begin_frame, end_frame,
   clear, clear_depth,
   draw_rect, draw_rect_tex, draw_rect_tex_uv, draw_line, draw_glyph,
   draw_rectangle_fast, draw_glyph_fast,
   create_texture, update_texture_rect, bind_texture, texture_size, texture_format, texture_descriptor, destroy_texture, read_framebuffer,
   _draw_triangle_2d, draw_triangle_3d, draw_quad_3d, draw_vertices, draw_lines_raw,
   set_mvp, set_ortho, _pack_color, _flush, _update_default_mvp,
   renderer_config, _get_local_vertex_map, _get_vertex_offset, _advance_vertex_offset,
   __vkr_push_vertex, __vkr_push_rect_tex, __vkr_draw_text, _vkr_glyph_get_off,
   get_render_stats, create_static_buffer, draw_static_buffer,
   _mvp_matrix, VERTEX_STRIDE, set_unlit,
   _vkr_glyph_present, _prof_flush_avg,
   draw_rect_fast, draw_text_batch,
   set_wireframe
)

use std.core *
use std.core.mem *
use std.math *
use std.math.matrix *
use std.ui.gfx.vulkan *
use std.ui.glfw as ui_glfw
use std.os *
use std.os.process as proc
use std.str as text
use std.util.common as common

mut _frame_draw_calls = 0
mut _total_draw_calls = 0
mut _static_vbo_ptr = 0
mut _static_off_ptr = 0
def VERTEX_STRIDE = 36
def _VKR_VERT_STRIDE = 36
def _VKR_OFF_X = 0
def _VKR_OFF_Y = 4
def _VKR_OFF_Z = 8
def _VKR_OFF_U = 12
def _VKR_OFF_V = 16
def _VKR_OFF_C = 20
def _VKR_OFF_NX = 24
def _VKR_OFF_NY = 28
def _VKR_OFF_NZ = 32

def _VKR_GLYPH_STRIDE = 48
def _VKR_G_ADV = 0
def _VKR_G_XOFF = 4
def _VKR_G_YOFF = 8
def _VKR_G_BW = 12
def _VKR_G_BH = 16
def _VKR_G_U1 = 20
def _VKR_G_V1 = 24
def _VKR_G_U2 = 28
def _VKR_G_V2 = 32
def _VKR_G_TEX = 36
def _VKR_G_PRESENT = 40
def _VKR_G_IS_COLOR = 44

fn draw_rect_fast(x, y, w, h, color_u32){
   "Submits a rectangle using pre-packed color and fixed vertex layout."
   bind_texture(_default_texture)
   _check_flush(216)
   __vkr_push_rect_tex_fast(_local_vertex_map + _vertex_offset, x, y, w, h, 0, 0, 0, 0, color_u32)
   _vertex_offset += 216
}

fn __vkr_push_rect_tex_fast(ptr, x, y, w, h, u1, v1, u2, v2, color_u32){
   memcpy(ptr, _quad_template, 216)
   def x2 = float(x) + float(w)
   def y2 = float(y) + float(h)
   mut bv = ptr
   store32_f32(bv, float(x), 0)  store32_f32(bv, float(y), 4)  store32_f32(bv, u1, 12) store32_f32(bv, v1, 16) store32(bv, color_u32, 20)
   bv += 36
   store32_f32(bv, float(x), 0)  store32_f32(bv, y2, 4)        store32_f32(bv, u1, 12) store32_f32(bv, v2, 16) store32(bv, color_u32, 20)
   bv += 36
   store32_f32(bv, x2, 0)        store32_f32(bv, y2, 4)        store32_f32(bv, u2, 12) store32_f32(bv, v2, 16) store32(bv, color_u32, 20)
   bv += 36
   store32_f32(bv, x2, 0)        store32_f32(bv, y2, 4)        store32_f32(bv, u2, 12) store32_f32(bv, v2, 16) store32(bv, color_u32, 20)
   bv += 36
   store32_f32(bv, x2, 0)        store32_f32(bv, float(y), 4)  store32_f32(bv, u2, 12) store32_f32(bv, v1, 16) store32(bv, color_u32, 20)
   bv += 36
   store32_f32(bv, float(x), 0)  store32_f32(bv, float(y), 4)  store32_f32(bv, u1, 12) store32_f32(bv, v1, 16) store32(bv, color_u32, 20)
}

fn draw_text_batch(font_id, lines, x, y, spacing, color_u32){
   "Draws multiple lines of text in a single Nytrix call to minimize interpreter overhead."
   if(!is_list(lines)){ return }
   def f = _font_get(font_id)
   if(!f){ return }
   def gptr = dict_get(f, "fast_glyphs", 0)
   def ascent = dict_get(f, "ascent", 0)
   mut cur_y = float(y)
   def f_size = float(dict_get(f, "size", 16))
   mut i = 0 while(i < len(lines)){
      __vkr_draw_text_fast(get(lines, i), float(x), cur_y + float(ascent), color_u32, gptr, f_size)
      cur_y += float(spacing)
      i += 1
   }
}

fn __vkr_draw_text_fast(text, x, y, color_u32, glyphs_ptr, line_h_f){
   if(!glyphs_ptr || !is_str(text)){ return }
   def n = str_len(text)
   _check_flush(n * 216)
   def page0 = load64(glyphs_ptr, 0)
   mut pen_x = x
   mut i = 0 while(i < n){
      def b0 = load8(text, i) & 255
      mut cp = 0 mut step = 1 mut g_off = 0
      if(b0 < 128){
         cp = b0
         i += 1
         if(page0){
         g_off = page0 + cp * _VKR_GLYPH_STRIDE
         if(load32(g_off, _VKR_G_PRESENT) == 0){ g_off = 0 }
         }
      } else {
         if(((b0 & 224) == 192) && (i + 1 < n)){
         cp = (((b0 & 31) << 6) | ((load8(text, i + 1) & 255) & 63))
         step = 2
         } elif(((b0 & 240) == 224) && (i + 2 < n)){
         cp = (((((b0 & 15) << 12) | (((load8(text, i + 1) & 255) & 63) << 6)) | ((load8(text, i + 2) & 255) & 63)))
         step = 3
         } elif(((b0 & 248) == 240) && (i + 3 < n)){
         cp = (((((((b0 & 7) << 18) | (((load8(text, i + 1) & 255) & 63) << 12)) | (((load8(text, i + 2) & 255) & 63) << 6)) | ((load8(text, i + 3) & 255) & 63))))
         step = 4
         }
         i += step
         def page = load64(glyphs_ptr, ((cp >> 8) & 65535) * 8)
         if(page){
         g_off = page + (cp & 255) * _VKR_GLYPH_STRIDE
         if(load32(g_off, _VKR_G_PRESENT) == 0){ g_off = 0 }
         }
      }
      if(cp == 13 || cp == 10 || cp == 9){ continue }
      if(!g_off){ if(page0){ g_off = page0 + 63 * _VKR_GLYPH_STRIDE } else { continue } }

      def tex_id = load32(g_off, _VKR_G_TEX)
      if(tex_id != _current_texture_id){ bind_texture(tex_id) }

      def bh_raw = load32_f32(g_off, _VKR_G_BH)
      def is_color = load32(g_off, _VKR_G_IS_COLOR) != 0
      mut gs = 1.0
      if(is_color && bh_raw > 0.0){
         gs = (line_h_f * 0.88) / bh_raw
      }
      def bw = load32_f32(g_off, _VKR_G_BW) * gs
      def bh = bh_raw * gs

      if(bw > 0.0 && bh > 0.0){
         def gx = pen_x + load32_f32(g_off, _VKR_G_XOFF) * gs
         mut gy = y - load32_f32(g_off, _VKR_G_YOFF) * gs
         if(is_color){ gy += (line_h_f * 0.06) }
         __vkr_push_rect_tex_fast(_local_vertex_map + _vertex_offset, gx, gy, bw, bh, load32_f32(g_off, _VKR_G_U1), load32_f32(g_off, _VKR_G_V1), load32_f32(g_off, _VKR_G_U2), load32_f32(g_off, _VKR_G_V2), is_color ? 0xFFFFFFFF : color_u32)
         _vertex_offset += 216
      }
      pen_x += load32_f32(g_off, _VKR_G_ADV) * gs
   }
}

fn _vkr_color_u32(c){
   "Internal: normalizes a color value to packed 32-bit form."
   if(is_int(c)){ return c }
   if(is_float(c)){ return __flt_to_int(c) }
   if(!is_list(c)){ return 0xFFFFFFFF }
   __vkr_pack_color(get(c, 0, 1.0), get(c, 1, 1.0), get(c, 2, 1.0), get(c, 3, 1.0))
}

fn __vkr_pack_color(r, g, b, a){
   "Internal: packs RGBA float components into the renderer's native color format."
   def r8 = __flt_to_int(float(r) * 255.0) & 255
   def g8 = __flt_to_int(float(g) * 255.0) & 255
   def b8 = __flt_to_int(float(b) * 255.0) & 255
   def a8 = __flt_to_int(float(a) * 255.0) & 255
   (a8 << 24) | (b8 << 16) | (g8 << 8) | r8
}

fn _vkr_store_vertex(base, idx, x, y, z, u, v, color, nx=0.0, ny=0.0, nz=1.0){
   "Internal: stores vertex `idx` into packed vertex buffer `base`."
   def off = base + idx * _VKR_VERT_STRIDE
   store32_f32(off, float(x), _VKR_OFF_X)
   store32_f32(off, float(y), _VKR_OFF_Y)
   store32_f32(off, float(z), _VKR_OFF_Z)
   store32_f32(off, float(u), _VKR_OFF_U)
   store32_f32(off, float(v), _VKR_OFF_V)
   store32(off, _vkr_color_u32(color), _VKR_OFF_C)
   store32_f32(off, float(nx), _VKR_OFF_NX)
   store32_f32(off, float(ny), _VKR_OFF_NY)
   store32_f32(off, float(nz), _VKR_OFF_NZ)
}

fn __vkr_push_vertex(ptr, x, y, z, u, v, color, nx=0.0, ny=0.0, nz=1.0){
   "Writes one packed vertex to `ptr`."
   if(!ptr){ return }
   _vkr_store_vertex(ptr, 0, x, y, z, u, v, color, nx, ny, nz)
}

fn __vkr_push_rect_tex(ptr, x, y, w, h, u1, v1, u2, v2, color, nz=1.0){
   "Writes a six-vertex textured quad to `ptr` using an optimized template path."
   if(!ptr){ return 0 }

   memcpy(ptr, _quad_template, 216)

   def c = _vkr_color_u32(color)
   def x2 = float(x) + float(w)
   def y2 = float(y) + float(h)

   mut bv = ptr
   store32_f32(bv, float(x), 0)  store32_f32(bv, float(y), 4)  store32_f32(bv, u1, 12) store32_f32(bv, v1, 16) store32(bv, c, 20)
   bv += 36
   store32_f32(bv, float(x), 0)  store32_f32(bv, y2, 4)        store32_f32(bv, u1, 12) store32_f32(bv, v2, 16) store32(bv, c, 20)
   bv += 36
   store32_f32(bv, x2, 0)        store32_f32(bv, y2, 4)        store32_f32(bv, u2, 12) store32_f32(bv, v2, 16) store32(bv, c, 20)
   bv += 36
   store32_f32(bv, x2, 0)        store32_f32(bv, y2, 4)        store32_f32(bv, u2, 12) store32_f32(bv, v2, 16) store32(bv, c, 20)
   bv += 36
   store32_f32(bv, x2, 0)        store32_f32(bv, float(y), 4)  store32_f32(bv, u2, 12) store32_f32(bv, v1, 16) store32(bv, c, 20)
   bv += 36
   store32_f32(bv, float(x), 0)  store32_f32(bv, float(y), 4)  store32_f32(bv, u1, 12) store32_f32(bv, v1, 16) store32(bv, c, 20)
   0
}

fn _init_quad_template(){
   "Pre-fills a 6-vertex quad template with default values (Z=0, Normal=[0,0,1])."
   if(!_quad_template){ return }
   mut i = 0 while(i < 6){
      def off = _quad_template + i * 36
      store32_f32(off, 0.0, 8) ; Z
      store32_f32(off, 0.0, 24) ; NX
      store32_f32(off, 0.0, 28) ; NY
      store32_f32(off, 1.0, 32) ; NZ
      i += 1
   }
}

fn __vkr_push_rect(ptr, x, y, w, h, color){
   "Writes a six-vertex solid-color quad to `ptr`."
   __vkr_push_rect_tex(ptr, x, y, w, h, 0.0, 0.0, 0.0, 0.0, color, 0.0)
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
   _vkr_store_vertex(ptr, 0, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 1, float(x1) - nx, float(y1) - ny, 0.0, 0.0, 0.0, color, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 2, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 3, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 4, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, 0.0, 0.0, 1.0)
   _vkr_store_vertex(ptr, 5, float(x2) + nx, float(y2) + ny, 0.0, 0.0, 0.0, color, 0.0, 0.0, 1.0)
}

fn _vkr_glyph_get_off(glyphs_ptr, cp){
   "Internal: returns the address of glyph metadata for `cp` using a paged table."
   if(cp < 0 || cp >= 1114112){ return 0 }
   def page_idx = cp >> 8
   def page_ptr = load64(glyphs_ptr, page_idx * 8)
   if(!page_ptr){ return 0 }
   page_ptr + (cp & 255) * _VKR_GLYPH_STRIDE
}

fn _vkr_glyph_present(glyphs_ptr, cp){
   "Internal: returns whether codepoint `cp` exists in the paged glyph table."
   def off = _vkr_glyph_get_off(glyphs_ptr, cp)
   if(!off){ return false }
   load32(off, _VKR_G_PRESENT) != 0
}

fn __vkr_draw_text(_unused_vbo, text, x, y, color, glyphs_ptr, ascent, line_h, out_info){
   "Builds packed glyph vertices for `text` directly into the current VBO, handling atlas changes."
   if(!glyphs_ptr || !is_str(text)){ return }
   def n = str_len(text)
   _check_flush(n * 216)
   def pen_x0 = float(x)
   mut pen_x = pen_x0
   mut pen_y = float(y) + float(ascent)
   def line_h_f = float(line_h)
   def c_text = _vkr_color_u32(color)
   mut total_verts = 0
   def page0 = load64(glyphs_ptr, 0)

   mut i = 0
   while(i < n){
      ;; 1. Optimized Decoder & Glyph Lookup
      def b0 = load8(text, i) & 255
      mut cp = 0 mut step = 1
      mut g_off = 0

      if(b0 < 128){
         cp = b0 i += 1
         if(page0){
         g_off = page0 + cp * _VKR_GLYPH_STRIDE
         if(load32(g_off, _VKR_G_PRESENT) == 0){ g_off = 0 }
         }
      } else {
         if(((b0 & 224) == 192) && (i + 1 < n)){ cp = (((b0 & 31) << 6) | ((load8(text, i + 1) & 255) & 63)) step = 2 }
         elif(((b0 & 240) == 224) && (i + 2 < n)){ cp = (((((b0 & 15) << 12) | (((load8(text, i + 1) & 255) & 63) << 6)) | ((load8(text, i + 2) & 255) & 63))) step = 3 }
         elif(((b0 & 248) == 240) && (i + 3 < n)){ cp = (((((((b0 & 7) << 18) | (((load8(text, i + 1) & 255) & 63) << 12)) | (((load8(text, i + 2) & 255) & 63) << 6)) | ((load8(text, i + 3) & 255) & 63)))) step = 4 }
         i += step

         def page = load64(glyphs_ptr, ((cp >> 8) & 65535) * 8)
         if(page){
         g_off = page + (cp & 255) * _VKR_GLYPH_STRIDE
         if(load32(g_off, _VKR_G_PRESENT) == 0){ g_off = 0 }
         }
      }

      if(cp == 13){ continue }
      if(cp == 10){ pen_x = pen_x0 pen_y = pen_y + line_h_f continue }
      if(cp == 9){ pen_x = pen_x + line_h_f * 2.0 continue }

      if(!g_off){
         if(page0){ g_off = page0 + 63 * _VKR_GLYPH_STRIDE } else { continue }
      }

      def tex_id = load32(g_off, _VKR_G_TEX)
      if(tex_id != _current_texture_id){ bind_texture(tex_id) }

      def adv_raw = load32_f32(g_off, _VKR_G_ADV)
      def bh_raw = load32_f32(g_off, _VKR_G_BH)
      def is_color = load32(g_off, _VKR_G_IS_COLOR) != 0

      mut gs = 1.0
      if(is_color && bh_raw > 0.0){ gs = line_h_f / bh_raw }

      def bw = load32_f32(g_off, _VKR_G_BW) * gs
      def bh = bh_raw * gs
      def adv = adv_raw * gs

      if(bw > 0.0 && bh > 0.0){
          def gx = pen_x + load32_f32(g_off, _VKR_G_XOFF) * gs
          mut gy = pen_y - load32_f32(g_off, _VKR_G_YOFF) * gs
          if(is_color){ gy += (line_h_f * 0.06) }
         def u1 = load32_f32(g_off, _VKR_G_U1)
         def v1 = load32_f32(g_off, _VKR_G_V1)
         def u2 = load32_f32(g_off, _VKR_G_U2)
         def v2 = load32_f32(g_off, _VKR_G_V2)
         def c = is_color ? 0xFFFFFFFF : c_text

         ;; 3. Fully Inlined Vertex Generation (6 vertices) - Template Optimized
         mut bv = _local_vertex_map + _vertex_offset
         memcpy(bv, _quad_template, 216)

         def gx2 = gx + bw
         def gy2 = gy + bh

         store32_f32(bv, gx, 0)   store32_f32(bv, gy, 4)   store32_f32(bv, u1, 12) store32_f32(bv, v1, 16) store32(bv, c, 20)
         bv += 36
         store32_f32(bv, gx, 0)   store32_f32(bv, gy2, 4)  store32_f32(bv, u1, 12) store32_f32(bv, v2, 16) store32(bv, c, 20)
         bv += 36
         store32_f32(bv, gx2, 0)  store32_f32(bv, gy2, 4)  store32_f32(bv, u2, 12) store32_f32(bv, v2, 16) store32(bv, c, 20)
         bv += 36
         store32_f32(bv, gx2, 0)  store32_f32(bv, gy2, 4)  store32_f32(bv, u2, 12) store32_f32(bv, v2, 16) store32(bv, c, 20)
         bv += 36
         store32_f32(bv, gx2, 0)  store32_f32(bv, gy, 4)   store32_f32(bv, u2, 12) store32_f32(bv, v1, 16) store32(bv, c, 20)
         bv += 36
         store32_f32(bv, gx, 0)   store32_f32(bv, gy, 4)   store32_f32(bv, u1, 12) store32_f32(bv, v1, 16) store32(bv, c, 20)

         _vertex_offset += 216
         total_verts += 6
      }
      pen_x = pen_x + adv
   }

   if(out_info){
      store64(out_info, total_verts, 0)
      store64(out_info, _current_texture_id, 8)
   }
}

mut _cfg_vsync = false
mut _cfg_filter = 0 ; 0=NEAREST, 1=LINEAR
mut _cfg_vert_spv = ""
mut _cfg_frag_spv = ""

mut _debug_gfx_enabled = false
fn _check_debug_env(){
   "Internal: loads Vulkan debug flags from environment variables."
   def v = env("NYTRIX_DEBUG_GFX")
   if(v && (eq(v, "1") || eq(v, "true"))){ _debug_gfx_enabled = true }
   else {
      def v2 = env("NY_UI_DEBUG")
      if(v2 && (eq(v2, "1") || eq(v2, "true"))){ _debug_gfx_enabled = true }
   }
}
fn _dbg_handle(label, h){
   "Internal: prints a labeled Vulkan handle when debug logging is enabled."
   if(_debug_gfx_enabled){ print(f"Vulkan: {label} h={h}") }
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

fn renderer_config(vsync, filter, vert_spv_path, frag_spv_path, msaa){
   "Configures the renderer. Must be called BEFORE init_window().
   vsync: true/false (default false)
   filter: 0 for NEAREST, 1 for LINEAR (default 0)
   vert_spv_path: path to custom vertex shader .spv or empty for default
   frag_spv_path: path to custom fragment shader .spv or empty for default
   msaa: number of MSAA samples (1, 2, 4, 8) (default 1)"
   if(vsync){ _cfg_vsync = true } else { _cfg_vsync = false }
   if(filter){ _cfg_filter = 1 } else { _cfg_filter = 0 }
   _cfg_vert_spv = vert_spv_path
   _cfg_frag_spv = frag_spv_path
   _cfg_msaa = msaa
}

mut _instance = 0
mut _physical_device = 0
mut _device = 0
mut _graphics_queue = 0
mut _present_queue = 0
mut _graphics_family_index = 0
mut _surface = 0
mut _swapchain = 0
mut _swapchain_image_count = 0
mut _swapchain_images = []
mut _swapchain_image_views = []
mut _swapchain_format = 0
mut _swapchain_extent_w = 0
mut _swapchain_extent_h = 0
mut _render_pass = 0
mut _framebuffers = []

mut _depth_image = 0
mut _depth_memory = 0
mut _depth_view = 0

mut _msaa_color_image = 0
mut _msaa_color_memory = 0
mut _msaa_color_view = 0

mut _command_pool = 0
mut _command_buffers = []

mut _descriptor_set_layout = 0
mut _pipeline_layout = 0
mut _pipeline = 0
mut _unlit_pipeline = 0
mut _line_pipeline = 0
mut _wire_pipeline = 0
mut _vert_module = 0
mut _frag_module = 0
mut _is_wireframe = false

mut _vertex_capacity = 33554432 ; 32MB per frame slice (huge headroom)
mut _current_frame_vertex_offset = 0
mut _vertex_buffer = 0
mut _vertex_memory = 0
mut _vertex_map = 0
mut _local_vertex_map = 0 ;; Faster locally-allocated CPU buffer
mut _vertex_offset = 0
mut _last_flush_offset = 0
mut _vertex_limit_hit = false

mut _staging_buffer = 0
mut _staging_memory = 0
mut _staging_map = 0
mut _staging_capacity = 67108864 ; 64MB staging capacity (can handle 4K RGBA textures)

mut _default_texture = 0
mut _default_sampler = 0
mut _descriptor_pool = 0
mut _textures = [] ; list of { image, view, memory, descriptor_set, width, height }
mut _texture_ds_cache = []
mut _texture_fmt_cache = []

mut _current_texture_id = -1
mut _current_is_unlit = 0
mut _last_is_unlit = 0

mut _image_available_semaphores = []
mut _render_finished_semaphores = []
mut _in_flight_fences = []

mut _current_frame = 0
mut _image_index = 0
mut _total_frames = 0
mut _pc_buffer = 0 ;; Pre-allocated push constant buffer
mut _current_mvp = 0
mut _current_model = 0
mut _frame_open = false
mut _window_ref = 0
mut _upload_cb = 0
mut _upload_alloc = 0
mut _upload_bi = 0
mut _upload_bar1 = 0
mut _upload_bar2 = 0
mut _upload_region = 0
mut _upload_si = 0
mut _upload_cb_arr = 0
mut _upload_cb_ptr = 0
mut _flush_off = 0
mut _flush_buf = 0
mut _last_bound_tex_id = -1
mut _last_bound_ds = 0
mut _target_pipeline = 0
mut _last_bound_pipe = 0
mut _pc_dirty = false
mut _last_is_mask = 0
mut _clear_ca = 0
mut _clear_rect = 0
mut _upload_fence = 0
mut _upload_fence_ptr = 0
mut _quad_template = 0
mut _prof_flush_total = 0.0
mut _prof_flush_count = 0
mut _prof_flush_avg = 0.0

;; Pre-allocated frame pointers
mut _ptr_fence = 0
mut _ptr_img_idx = 0
mut _ptr_bi = 0
mut _ptr_clear = 0
mut _ptr_ri = 0
mut _ptr_vp = 0
mut _ptr_sci = 0
mut _ptr_dsl = 0
mut _ptr_ds = 0
mut _ptr_sub = 0
mut _ptr_wait_sems = 0
mut _ptr_sig_sems = 0
mut _ptr_stages = 0
mut _clear_r = 0.05
mut _clear_g = 0.05
mut _clear_b = 0.1
mut _clear_a = 1.0
def MAX_FRAMES_IN_FLIGHT = 2

mut _fps_last_time = 0.0
mut _fps_count = 0
mut _fps_curr = 0

fn init(win){
   "Initializes the Vulkan renderer for the given window."
   _window_ref = win
   _check_debug_env()

   if(!vk_init()){ return false }
   if(!_create_instance()){ return false }
   if(!_create_surface(win)){ return false }
   if(!_pick_physical_device()){ return false }
   if(!_create_logical_device()){ return false }
   if(!_create_swapchain(win)){ return false }
   _create_swapchain_image_views()
   _create_depth_resources()
   _create_render_pass()
   _create_graphics_pipeline()
   _create_framebuffers()
   if(!_create_sync_objects()){ if(_debug_gfx_enabled){ print("Vulkan: Sync objects failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Sync objects OK") }
   _create_command_pool()
   if(!_create_command_buffers()){ if(_debug_gfx_enabled){ print("Vulkan: Command buffers failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Command buffers OK") }
   if(!_create_vertex_buffer()){ if(_debug_gfx_enabled){ print("Vulkan: Vertex buffer failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Vertex buffer OK") }
   if(!_create_staging_buffer()){ if(_debug_gfx_enabled){ print("Vulkan: Staging buffer failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Staging buffer OK") }
   if(!_create_descriptor_pool()){ if(_debug_gfx_enabled){ print("Vulkan: Descriptor pool failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Descriptor pool OK") }
   if(!_create_default_texture()){ if(_debug_gfx_enabled){ print("Vulkan: Default texture failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Default texture OK") }

   _current_mvp = sys_malloc(64)
   _current_model = sys_malloc(64)
   _pc_buffer   = sys_malloc(160)
   memset(_pc_buffer, 0, 160)

   _ptr_fence = sys_malloc(8)
   _ptr_img_idx = sys_malloc(4)
   _ptr_bi = sys_malloc(64)
   _ptr_clear = sys_malloc(96) ; 3 * 32 bytes max for MSAA clear values
   _ptr_ri = sys_malloc(128)
   _ptr_vp = sys_malloc(32)
   _ptr_sci = sys_malloc(32)
   _ptr_dsl = sys_malloc(8)
   _ptr_ds = sys_malloc(8)
   _ptr_sub = sys_malloc(128)
   _ptr_wait_sems = sys_malloc(32)
   _ptr_sig_sems = sys_malloc(32)
   _ptr_stages = sys_malloc(128)

   _quad_template = sys_malloc(216)
   _init_quad_template()

   ;; Pre-allocate upload buffers for update_texture_rect
   _upload_alloc = sys_malloc(32)
   _upload_bi = sys_malloc(32)
   _upload_bar1 = sys_malloc(72)
   _upload_bar2 = sys_malloc(72)
   _upload_region = sys_malloc(56)
   _upload_si = sys_malloc(72)
   _upload_cb_arr = sys_malloc(8)
   _upload_cb_ptr = sys_malloc(8)
   _flush_off = sys_malloc(8)
   _flush_buf = sys_malloc(8)
   ;; Upload fence for texture rect updates
   mut fence_ci = sys_malloc(16)
   memset(fence_ci, 0, 16)
   store32(fence_ci, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
   _upload_fence_ptr = sys_malloc(8)
   create_fence(_device, fence_ci, 0, _upload_fence_ptr)
   _upload_fence = load64(_upload_fence_ptr, 0)
   sys_free(fence_ci)

   _static_vbo_ptr = sys_malloc(8)
   _static_off_ptr = sys_malloc(8)
   store64_raw(_static_off_ptr, 0, 0)

   _upload_cb_ptr = sys_malloc(8)
   _update_default_mvp(_window_ref)
   true
}

fn _create_staging_buffer(){
   "Creates the GPU staging buffer for data uploads."
   mut ci = sys_malloc(56)
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store32(ci, 0, 16) ; flags
   store64_raw(ci, _staging_capacity, 24) ; size
   store32(ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 32)
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36)
   mut buf_ptr = sys_malloc(8)
   if(create_buffer(_device, ci, 0, buf_ptr) != 0){ return false }
   _staging_buffer = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, _staging_buffer, mem_req)
   def size = load64(mem_req, 0)
   def type_bits = load32(mem_req, 16)

   def mem_type_index = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
   if(mem_type_index == -1){ return false }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)
   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return false }
   _staging_memory = load64(mem_ptr, 0)
   bind_buffer_memory(_device, _staging_buffer, _staging_memory, 0)

   mut map_ptr = sys_malloc(8)
   map_memory(_device, _staging_memory, 0, size, 0, map_ptr)
   _staging_map = load64(map_ptr, 0)

   true
}

fn _create_descriptor_pool(){
   "Initializes the Vulkan descriptor pool for shaders."
   mut pool_size = sys_malloc(8)
   store32(pool_size, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0)
   store32(pool_size, 1000, 4)

   mut pool_ci = sys_malloc(40)
   memset(pool_ci, 0, 40)
   store32(pool_ci, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0)
   store32(pool_ci, 0, 16) ; flags
   store32(pool_ci, 1000, 20) ; maxSets
   store32(pool_ci, 1, 24) ; poolSizeCount
   store64_raw(pool_ci, pool_size, 32)

   mut pool_ptr = sys_malloc(8)
   if(create_descriptor_pool(_device, pool_ci, 0, pool_ptr) != 0){ return false }
   _descriptor_pool = load64(pool_ptr, 0)
   true
}

fn _find_memory_type(type_filter, properties){
   "Heuristic to find the best Vulkan memory type index for given filter/props."
   mut mem_props = sys_malloc(520)
   get_physical_device_memory_properties(_physical_device, mem_props)
   def count = load32(mem_props, 0)
   mut i = 0
   while(i < count){
      if((type_filter & (1 << i)) != 0){
         def flags = load32(mem_props, 4 + i * 8)
         if((flags & properties) == properties){
         sys_free(mem_props)
         return i
         }
      }
      i += 1
   }
   sys_free(mem_props)
   -1
}

fn _copy_buffer(src, dst, size){
   "Internal: performs a synchronous GPU-to-GPU buffer copy using a transient command buffer."
   mut bi = sys_malloc(32) memset(bi, 0, 32)
   store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(bi, 1, 16) ; ONE_TIME_SUBMIT

   mut ai = sys_malloc(32) memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_raw(ai, _command_pool, 16)
   store32(ai, 0, 24) ; PRIMARY
   store32(ai, 1, 28) ; 1
   mut cb_ptr = sys_malloc(8)
   allocate_command_buffers(_device, ai, cb_ptr)
   def cb = load64(cb_ptr, 0)

   begin_command_buffer(cb, bi)
   mut region = sys_malloc(24) memset(region, 0, 24)
   store64_raw(region, 0, 0) ; srcOffset
   store64_raw(region, 0, 8) ; dstOffset
   store64_raw(region, size, 16)
   cmd_copy_buffer(cb, src, dst, 1, region)
   end_command_buffer(cb)

   mut si = sys_malloc(72) memset(si, 0, 72)
   store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(si, 1, 40) ; cb count
   mut cb_arr = sys_malloc(8) store64_raw(cb_arr, cb, 0)
   store64_raw(si, cb_arr, 48)

   queue_submit(_graphics_queue, 1, si, 0)
   queue_wait_idle(_graphics_queue)

   free_command_buffers(_device, _command_pool, 1, cb_ptr)
   sys_free(bi) sys_free(ai) sys_free(cb_ptr) sys_free(region) sys_free(si) sys_free(cb_arr)
}

fn create_static_buffer(ptr, count){
   "Creates a device-local GPU vertex buffer and uploads data to it. Returns a buffer descriptor dict."
   if(!ptr || count <= 0){ return 0 }
   def size = count * 36

   ;; 1. Staging Buffer
   mut s_ci = sys_malloc(56) memset(s_ci, 0, 56)
   store32(s_ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_raw(s_ci, size, 24)
   store32(s_ci, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 32)
   mut s_ptr = sys_malloc(8)
   if(create_buffer(_device, s_ci, 0, s_ptr) != 0){ return 0 }
   def s_buf = load64(s_ptr, 0)

   mut s_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, s_buf, s_req)
   def s_size = load64(s_req, 0)
   def s_type = _find_memory_type(load32(s_req, 16), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

   mut s_alloc = sys_malloc(64) memset(s_alloc, 0, 64)
   store32(s_alloc, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(s_alloc, s_size, 16)
   store32(s_alloc, s_type, 24)
   mut s_mem_ptr = sys_malloc(8)
   allocate_memory(_device, s_alloc, 0, s_mem_ptr)
   def s_mem = load64(s_mem_ptr, 0)
   bind_buffer_memory(_device, s_buf, s_mem, 0)

   mut s_map = sys_malloc(8)
   map_memory(_device, s_mem, 0, size, 0, s_map)
   def s_ptr_map = load64(s_map, 0)
   memcpy(s_ptr_map, ptr, size)
   unmap_memory(_device, s_mem)

   ;; 2. Final Device-Local Buffer
   mut d_ci = sys_malloc(56) memset(d_ci, 0, 56)
   store32(d_ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_raw(d_ci, size, 24)
   store32(d_ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 32)
   mut d_ptr = sys_malloc(8)
   create_buffer(_device, d_ci, 0, d_ptr)
   def d_buf = load64(d_ptr, 0)

   mut d_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, d_buf, d_req)
   def d_size = load64(d_req, 0)
   def d_type = _find_memory_type(load32(d_req, 16), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut d_alloc = sys_malloc(64) memset(d_alloc, 0, 64)
   store32(d_alloc, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(d_alloc, d_size, 16)
   store32(d_alloc, d_type, 24)
   mut d_mem_ptr = sys_malloc(8)
   allocate_memory(_device, d_alloc, 0, d_mem_ptr)
   def d_mem = load64(d_mem_ptr, 0)
   bind_buffer_memory(_device, d_buf, d_mem, 0)

   ;; 3. Transfer
   _copy_buffer(s_buf, d_buf, size)

   ;; 4. Cleanup Staging
   destroy_buffer(_device, s_buf, 0)
   free_memory(_device, s_mem, 0)

   sys_free(s_ci) sys_free(s_ptr) sys_free(s_req) sys_free(s_alloc) sys_free(s_mem_ptr) sys_free(s_map)
   sys_free(d_ci) sys_free(d_ptr) sys_free(d_req) sys_free(d_alloc) sys_free(d_mem_ptr)

   mut m = dict()
   m = dict_set(m, "handle", d_buf)
   m = dict_set(m, "memory", d_mem)
   m = dict_set(m, "count", count)
   m
}

fn draw_static_buffer(sbuf, is_lines=false){
   "Records a draw command for a static GPU buffer. Must be called inside a frame."
   if(!_frame_open || !is_dict(sbuf)){ return false }
   def buf = dict_get(sbuf, "handle", 0)
   def count = dict_get(sbuf, "count", 0)
   if(!buf || count <= 0){ return false }

   _flush() ; Flush pending dynamic geometry

   def cb = get(_command_buffers, _current_frame)

   ; Ensure pipeline is correctly bound for the static mesh
   mut target = _pipeline
   if(is_lines && _line_pipeline != 0){
      target = _line_pipeline
      if(_last_is_mask != 0){ _last_is_mask = 0 _pc_dirty = true }
      if(_last_is_unlit != 1){ _last_is_unlit = 1 _pc_dirty = true }
   } else {
      mut base_pipe = _pipeline
      if(_current_is_unlit != 0 && _unlit_pipeline != 0){ base_pipe = _unlit_pipeline }
      target = _target_pipeline
      if(target == _pipeline){ target = base_pipe }
      if(_is_wireframe && _wire_pipeline != 0){
         if(target == _pipeline || target == _unlit_pipeline){ target = _wire_pipeline }
      }
   }

   if(_last_bound_pipe != target){
       cmd_bind_pipeline(cb, 0, target)
       _last_bound_pipe = target
       _pc_dirty = true
   }

   ; Ensure textures and descriptors are bound
   mut tid = _current_texture_id
   if(tid < 0 || tid >= len(_textures)){ tid = _default_texture }
   def ds = texture_descriptor(tid)
   if(ds && (ds != _last_bound_ds || tid != _last_bound_tex_id)){
      store64_raw(_ptr_ds, ds, 0)
      cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 1, _ptr_ds, 0, 0)
      _last_bound_ds = ds
      _last_bound_tex_id = tid
      mut new_mask = 0
      if(texture_format(tid) == 9){ new_mask = 1 }
      if(new_mask != _last_is_mask){ _last_is_mask = new_mask _pc_dirty = true }
   }

   _sync_pc() ; Sync push constants

   store64_raw(_static_vbo_ptr, buf, 0)

   cmd_bind_vertex_buffers(cb, 0, 1, _static_vbo_ptr, _static_off_ptr)
   cmd_draw(cb, count, 1, 0, 0)

   _total_draw_calls += 1
   _frame_draw_calls += 1

   ; Rebind dynamic VBO back
   store64_raw(_static_off_ptr, _current_frame_vertex_offset, 0)
   store64_raw(_static_vbo_ptr, _vertex_buffer, 0)
   cmd_bind_vertex_buffers(cb, 0, 1, _static_vbo_ptr, _static_off_ptr)
   store64_raw(_static_off_ptr, 0, 0)
   true
}

fn create_texture_ex(width, height, pixels, format=37){
   "Creates a GPU texture. Format 37=RGBA8, 9=R8."
   mut bpp = 4
   if(format == 9){ bpp = 1 }

   ; 1. Create Image
   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16) ; flags
   store32(img_ci, 1, 20) ; imageType = 2D
   store32(img_ci, format, 24)
   store32(img_ci, width, 28)
   store32(img_ci, height, 32)
   store32(img_ci, 1, 36) ; depth
   store32(img_ci, 1, 40) ; mipLevels
   store32(img_ci, 1, 44) ; arrayLayers
   store32(img_ci, 1, 48) ; samples
   store32(img_ci, 0, 52) ; tiling = OPTIMAL
   store32(img_ci, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 56)
   store32(img_ci, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(img_ci, 0, 80) ; initialLayout = UNDEFINED

   mut img_ptr = sys_malloc(8)
   def r1 = create_image(_device, img_ci, 0, img_ptr)
   if(r1 != 0){ return -1 }
   def image = load64(img_ptr, 0)

   ; 2. Allocate Memory
   mut mem_req = sys_malloc(24)
   get_image_memory_requirements(_device, image, mem_req)
   def size = load64(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   common.touch(type_bits)
   def mem_idx = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_idx, 24)

   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return -1 }
   def memory = load64(mem_ptr, 0)
   bind_image_memory(_device, image, memory, 0)

   ; 3. Create ImageView
   mut view_ci = sys_malloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_raw(view_ci, image, 24)
   store32(view_ci, 1, 32) ; 2D
   store32(view_ci, format, 36)
   store32(view_ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)

   mut view_ptr = sys_malloc(8)
   def r3 = create_image_view(_device, view_ci, 0, view_ptr)
   if(r3 != 0){ return -1 }
   def view = load64(view_ptr, 0)

   ; 4. Upload Initial Pixels
   def img_size = width * height * bpp
   if(pixels && _staging_map){
      if(img_size > _staging_capacity){
         if(_debug_gfx_enabled){ print(f"Vulkan: IMAGE TOO LARGE FOR STAGING: {img_size} > {_staging_capacity}") }
         return -1
      }
      memcpy(_staging_map, pixels, img_size)

      mut alloc_cb = sys_malloc(32)
      memset(alloc_cb, 0, 32)
      store32(alloc_cb, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
      store64_raw(alloc_cb, _command_pool, 16)
      store32(alloc_cb, 0, 24)
      store32(alloc_cb, 1, 28)
      mut cb_ptr = sys_malloc(8)
      def rcb = allocate_command_buffers(_device, alloc_cb, cb_ptr)
      if(rcb != 0){ return -1 }
      def cb = load64(cb_ptr, 0)

      mut bi = sys_malloc(32)
      memset(bi, 0, 32)
      store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
      store32(bi, 1, 16)
      begin_command_buffer(cb, bi)

      ; Transition UNDEFINED -> DST
      mut bar1 = sys_malloc(72)
      memset(bar1, 0, 72)
      store32(bar1, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
      store32(bar1, 0, 16)
      store32(bar1, VK_ACCESS_TRANSFER_WRITE_BIT, 20)
      store32(bar1, 0, 24) ; UNDEFINED
      store32(bar1, 7, 28) ; TRANSFER_DST
      store32(bar1, -1, 32) store32(bar1, -1, 36)
      store64_raw(bar1, image, 40)
      store32(bar1, 1, 48) ; COLOR
      store32(bar1, 0, 52) store32(bar1, 1, 56)
      store32(bar1, 0, 60) store32(bar1, 1, 64)
      cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, bar1)

      mut region = sys_malloc(56)
      memset(region, 0, 56)
      store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 16)
      store32(region, 0, 20) ; mip
      store32(region, 0, 24) ; baseLayer
      store32(region, 1, 28) ; layerCount
      store32(region, width, 44)
      store32(region, height, 48)
      store32(region, 1, 52)
      cmd_copy_buffer_to_image(cb, _staging_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, region)

      ; Transition DST -> SHADER_READ
      store32(bar1, VK_ACCESS_TRANSFER_WRITE_BIT, 16)
      store32(bar1, VK_ACCESS_SHADER_READ_BIT, 20)
      store32(bar1, 7, 24) ; DST
      store32(bar1, 5, 28) ; SHADER_READ
      cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, bar1)

      end_command_buffer(cb)

      mut si = sys_malloc(72)
      memset(si, 0, 72)
      store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
      store32(si, 1, 40)
      store64_raw(si, cb_ptr, 48)
      queue_submit(_graphics_queue, 1, si, 0)
      device_wait_idle(_device)

      free_command_buffers(_device, _command_pool, 1, cb_ptr)
      sys_free(alloc_cb) sys_free(cb_ptr) sys_free(bi) sys_free(bar1) sys_free(region) sys_free(si)
   }

   ; 5. Descriptor Set
   mut dsl_ptr = sys_malloc(8)
   store64_raw(dsl_ptr, _descriptor_set_layout, 0)
   mut alloc_ds = sys_malloc(40)
   memset(alloc_ds, 0, 40)
   store32(alloc_ds, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0)
   store64_raw(alloc_ds, _descriptor_pool, 16)
   store32(alloc_ds, 1, 24)
   store64_raw(alloc_ds, dsl_ptr, 32)
   mut ds_ptr = sys_malloc(8)
   def dres = allocate_descriptor_sets(_device, alloc_ds, ds_ptr)
   mut ds = 0
   if(dres == 0){ ds = load64(ds_ptr, 0) }

   if(ds){
      mut im_info = sys_malloc(24)
      store64_raw(im_info, _default_sampler, 0)
      store64_raw(im_info, view, 8)
      store32(im_info, 5, 16) ; SHADER_READ_ONLY_OPTIMAL

      mut write = sys_malloc(64)
      memset(write, 0, 64)
      store32(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
      store64_raw(write, ds, 16)
      store32(write, 0, 24) ; binding
      store32(write, 1, 32) ; count
      store32(write, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36)
      store64_raw(write, im_info, 40)
      update_descriptor_sets(_device, 1, write, 0, 0)
      sys_free(im_info) sys_free(write)
   }

   mut tex = dict(8)
   tex = dict_set(tex, "image", image)
   tex = dict_set(tex, "view", view)
   tex = dict_set(tex, "memory", memory)
   tex = dict_set(tex, "ds", ds)
   tex = dict_set(tex, "width", width)
   tex = dict_set(tex, "height", height)
   tex = dict_set(tex, "format", format)
   tex = dict_set(tex, "bpp", bpp)

   def tex_id = len(_textures)
   _textures = append(_textures, tex)
   _texture_ds_cache = append(_texture_ds_cache, ds)
   _texture_fmt_cache = append(_texture_fmt_cache, format)

   sys_free(img_ci) sys_free(img_ptr) sys_free(mem_req) sys_free(alloc_info) sys_free(mem_ptr)
   sys_free(view_ci) sys_free(view_ptr) sys_free(dsl_ptr) sys_free(alloc_ds) sys_free(ds_ptr)

   tex_id
}

fn update_texture_rect(tex_id, x, y, w, h, pixels){
   "Partially updates a texture's pixel data. Uses pre-allocated buffers."
   if(tex_id < 0 || tex_id >= len(_textures)){ return false }
   def tex_obj = get(_textures, tex_id)
   def image = dict_get(tex_obj, "image")
   def bpp = dict_get(tex_obj, "bpp", 4)
   def img_size = w * h * bpp
   if(img_size > _staging_capacity){ return false }

   memcpy(_staging_map, pixels, img_size)

   memset(_upload_alloc, 0, 32)
   store32(_upload_alloc, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_raw(_upload_alloc, _command_pool, 16)
   store32(_upload_alloc, 0, 24)
   store32(_upload_alloc, 1, 28)
   if(allocate_command_buffers(_device, _upload_alloc, _upload_cb_ptr) != 0){ return false }
   def cb = load64(_upload_cb_ptr, 0)

   memset(_upload_bi, 0, 32)
   store32(_upload_bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(_upload_bi, 1, 16)
   begin_command_buffer(cb, _upload_bi)

   memset(_upload_bar1, 0, 72)
   store32(_upload_bar1, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
   store32(_upload_bar1, VK_ACCESS_SHADER_READ_BIT, 16)
   store32(_upload_bar1, VK_ACCESS_TRANSFER_WRITE_BIT, 20)
   store32(_upload_bar1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 24)
   store32(_upload_bar1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 28)
   store32(_upload_bar1, -1, 32)
   store32(_upload_bar1, -1, 36)
   store64_raw(_upload_bar1, image, 40)
   store32(_upload_bar1, VK_IMAGE_ASPECT_COLOR_BIT, 48)
   store32(_upload_bar1, 0, 52) store32(_upload_bar1, 1, 56)
   store32(_upload_bar1, 0, 60) store32(_upload_bar1, 1, 64)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, _upload_bar1)

   memset(_upload_region, 0, 56)
   store32(_upload_region, VK_IMAGE_ASPECT_COLOR_BIT, 16)
   store32(_upload_region, 0, 20)
   store32(_upload_region, 0, 24)
   store32(_upload_region, 1, 28)
   store32(_upload_region, x, 32)
   store32(_upload_region, y, 36)
   store32(_upload_region, 0, 40)
   store32(_upload_region, w, 44)
   store32(_upload_region, h, 48)
   store32(_upload_region, 1, 52)
   cmd_copy_buffer_to_image(cb, _staging_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, _upload_region)

   memset(_upload_bar2, 0, 72)
   store32(_upload_bar2, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
   store32(_upload_bar2, VK_ACCESS_TRANSFER_WRITE_BIT, 16)
   store32(_upload_bar2, VK_ACCESS_SHADER_READ_BIT, 20)
   store32(_upload_bar2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 24)
   store32(_upload_bar2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 28)
   store32(_upload_bar2, -1, 32)
   store32(_upload_bar2, -1, 36)
   store64_raw(_upload_bar2, image, 40)
   store32(_upload_bar2, VK_IMAGE_ASPECT_COLOR_BIT, 48)
   store32(_upload_bar2, 0, 52) store32(_upload_bar2, 1, 56)
   store32(_upload_bar2, 0, 60) store32(_upload_bar2, 1, 64)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, _upload_bar2)

   end_command_buffer(cb)

   memset(_upload_si, 0, 72)
   store32(_upload_si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(_upload_si, 1, 40)
   store64_raw(_upload_cb_arr, cb, 0)
   store64_raw(_upload_si, _upload_cb_arr, 48)
   reset_fences(_device, 1, _upload_fence_ptr)
   queue_submit(_graphics_queue, 1, _upload_si, _upload_fence)
   wait_for_fences(_device, 1, _upload_fence_ptr, 1, 0xFFFFFFFFFFFFFFFF)
   free_command_buffers(_device, _command_pool, 1, _upload_cb_ptr)
   true
}

fn create_texture(width, height, pixels){
   "Creates a GPU texture from raw pixel data (RGBA8)."
   create_texture_ex(width, height, pixels, 37)
}

fn _create_default_texture(){
   "Creates the default 1x1 white texture for untextured drawing."
   ; Create sampler first
   mut sampler_ci = sys_malloc(80)
   memset(sampler_ci, 0, 80)
   store32(sampler_ci, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0)
   store32(sampler_ci, 0, 16) ; flags
   store32(sampler_ci, _cfg_filter, 20) ; magFilter
   store32(sampler_ci, _cfg_filter, 24) ; minFilter
   store32(sampler_ci, 0, 28) ; mipmapMode
   store32(sampler_ci, 2, 32) ; addressModeU = CLAMP_TO_EDGE
   store32(sampler_ci, 2, 36) ; addressModeV
   store32(sampler_ci, 2, 40) ; addressModeW

   mut sampler_ptr = sys_malloc(8)
   if(create_sampler(_device, sampler_ci, 0, sampler_ptr) != 0){ return false }
   _default_sampler = load64(sampler_ptr, 0)

   ; Create 1x1 white texture
   def pixels = sys_malloc(4)
   store32(pixels, 0xFFFFFFFF, 0)
   def tex_id = create_texture(1, 1, pixels)
   if(tex_id == -1){ return false }
   _default_texture = tex_id
   _current_texture_id = tex_id
   true
}

mut _mvp_dirty = true
mut _model_dirty = true

fn _mvp_matrix(){
   "Returns the current internal MVP matrix, as set by begin_mode_3d."
   mut m = mat4_identity()
   if(_current_mvp){ mat4_from_buffer(m, _current_mvp) }
   return m
}

fn set_model_matrix(mat){
   "Updates the Model matrix for subsequent 3D draw calls."
   if(_current_model && is_list(mat)){
      if(_vertex_offset != _last_flush_offset){ _flush() }
      mat4_to_buffer(mat, _current_model)
      _model_dirty = true
      _pc_dirty = true
   }
}

fn set_mvp(mat){
   "Updates the View-Projection matrix for the renderer."
   if(_current_mvp && is_list(mat)){
      if(_vertex_offset != _last_flush_offset){ _flush() }
      mat4_to_buffer(mat, _current_mvp)
      _mvp_dirty = true
      _pc_dirty = true
   }
}

fn set_ortho(l, r, b, t, n, f){
   "Sets the MVP matrix to an orthographic projection."
   if(b < t){ def tmp = b b = t t = tmp } ; enforce Y-down for UI coords
   def mat = mat4_ortho(l, r, b, t, n, f)
   set_mvp(mat)
}

fn set_perspective(fovy, aspect, near, far){
   "Sets the View-Projection matrix to a perspective projection."
   def mat = mat4_perspective(fovy, aspect, near, far)
   set_mvp(mat)
}

fn bind_texture(tex_id){
   "Binds a texture by ID for subsequent drawing commands."
   if(tex_id == _current_texture_id){ return }
   _flush()
   _current_texture_id = tex_id
}

fn texture_size(tex_id){
   "Returns [width, height] for a texture ID, or 0 if invalid."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_textures)){ return 0 }
   def tex = get(_textures, tex_id, 0)
   if(!tex || !is_dict(tex)){ return 0 }
   [dict_get(tex, "width", 0), dict_get(tex, "height", 0)]
}

fn _texture_meta(tex_id, key, fallback){
   "Internal: reads texture metadata field `key` from texture `tex_id`, or returns `fallback`."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_textures)){ return fallback }
   def tex_obj = get(_textures, tex_id, 0)
   if(!tex_obj || !is_dict(tex_obj)){ return fallback }
   dict_get(tex_obj, key, fallback)
}

fn texture_format(tex_id){
   "Returns the format of a texture."
   _texture_meta(tex_id, "format", 37)
}

fn texture_descriptor(tex_id){
   "Returns the descriptor set for a texture."
   _texture_meta(tex_id, "ds", 0)
}

fn destroy_texture(tex_id){
   "Destroys a texture and frees its GPU resources."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_textures)){ return }
   def tex = get(_textures, tex_id, 0)
   if(!tex || !is_dict(tex)){ return }
   def img = dict_get(tex, "image", 0)
   def view = dict_get(tex, "view", 0)
   def mem = dict_get(tex, "memory", 0)
   if(view){ destroy_image_view(_device, view, 0) }
   if(img){ destroy_image(_device, img, 0) }
   if(mem){ free_memory(_device, mem, 0) }
}

fn read_framebuffer(){
   "Reads the current swapchain image back to CPU memory. Returns {data, width, height, channels} or 0."
   if(!_device || !_swapchain || _image_index < 0){ return 0 }
   def w = _swapchain_extent_w
   def h = _swapchain_extent_h
   if(w <= 0 || h <= 0){ return 0 }
   def size = w * h * 4

   ; Create a host-visible buffer for readback
   mut buf_ci = sys_malloc(56)
   memset(buf_ci, 0, 56)
   store32(buf_ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_raw(buf_ci, size, 24)
   store32(buf_ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 32)
   mut buf_ptr = sys_malloc(8)
   if(create_buffer(_device, buf_ci, 0, buf_ptr) != 0){ return 0 }
   def readback_buf = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, readback_buf, mem_req)
   def mem_idx = _find_memory_type(load32(mem_req, 16), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, load64(mem_req, 0), 16)
   store32(alloc_info, mem_idx, 24)
   mut mem_ptr = sys_malloc(8)
   allocate_memory(_device, alloc_info, 0, mem_ptr)
   def readback_mem = load64(mem_ptr, 0)
   bind_buffer_memory(_device, readback_buf, readback_mem, 0)

   ; Record copy commands
   mut ai = sys_malloc(32)
   memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_raw(ai, _command_pool, 16)
   store32(ai, 1, 28)
   mut cb_p = sys_malloc(8)
   allocate_command_buffers(_device, ai, cb_p)
   def cb = load64(cb_p, 0)

   mut bi = sys_malloc(32)
   memset(bi, 0, 32)
   store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(bi, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 16)
   begin_command_buffer(cb, bi)

   def src_image = get(_swapchain_images, _image_index)
   mut barrier = sys_malloc(72)
   memset(barrier, 0, 72)
   store32(barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
   store32(barrier, 0, 16) ; srcAccess
   store32(barrier, VK_ACCESS_TRANSFER_READ_BIT, 20) ; dstAccess

   mut old_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
   if(!_surface){ old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }
   store32(barrier, old_layout, 24)
   store32(barrier, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 28)
   store64_raw(barrier, src_image, 40)
   store32(barrier, VK_IMAGE_ASPECT_COLOR_BIT, 48)
   store32(barrier, 1, 56)
   store32(barrier, 1, 64)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, barrier)

   mut region = sys_malloc(56)
   memset(region, 0, 56)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 16)
   store32(region, 0, 20)
   store32(region, 0, 24)
   store32(region, 1, 28)
   store32(region, w, 44)
   store32(region, h, 48)
   store32(region, 1, 52)
   cmd_copy_image_to_buffer(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf, 1, region)

   store32(barrier, VK_ACCESS_TRANSFER_READ_BIT, 16)
   store32(barrier, 0, 20)
   store32(barrier, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 24)
   store32(barrier, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 28)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, barrier)
   end_command_buffer(cb)

   mut s_info = sys_malloc(72)
   memset(s_info, 0, 72)
   store32(s_info, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(s_info, 1, 40)
   store64_raw(s_info, cb_p, 48)
   queue_submit(_graphics_queue, 1, s_info, 0)
   device_wait_idle(_device)

   mut map_ptr = sys_malloc(8)
   map_memory(_device, readback_mem, 0, size, 0, map_ptr)
   def mapped_data = load64(map_ptr, 0)

   ; Copy to Nytrix heap so we can free GPU resources
   def pixels = malloc(size)
   memcpy(pixels, mapped_data, size)

   unmap_memory(_device, readback_mem)
   destroy_buffer(_device, readback_buf, 0)
   free_memory(_device, readback_mem, 0)
   free_command_buffers(_device, _command_pool, 1, cb_p)

   ; Handle BGR swap for standard formats (44=BGRA8_UNORM, 50=BGRA8_SRGB, etc.)
   ; Standard BGRA formats: 44, 45, 46, 47, 48, 49, 50, 51, 52
   if(_swapchain_format >= 44 && _swapchain_format <= 52){
      mut b = 0
      while(b < size){
         def blue = load8(pixels, b)
         def red  = load8(pixels, b + 2)
         store8(pixels, red, b)
         store8(pixels, blue, b + 2)
         b += 4
      }
   }
   if(_debug_gfx_enabled){ print(f"Vulkan: Captured framebuffer format={_swapchain_format} size={w}x{h}") }

   mut res = dict(4)
   res = dict_set(res, "data",   pixels)
   res = dict_set(res, "width",  w)
   res = dict_set(res, "height", h)
   res = dict_set(res, "bpp",    4)
   res
}

fn _update_default_mvp(win){
   "Recalculates the default orthographic projection matrix for the window/swapchain."
   mut w = float(_swapchain_extent_w)
   mut h = float(_swapchain_extent_h)
   if(win){
      w = float(dict_get(win, "w", w))
      h = float(dict_get(win, "h", h))
   }
   ; Standard 2D coordinate system: (0,0) is top-left, (w,h) is bottom-right.
   set_ortho(0.0, w, 0.0, h, -1.0, 1.0)
}

fn _create_vertex_buffer(){
   "Creates the GPU vertex buffer for batch rendering."
   mut ci = sys_malloc(56)
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_raw(ci, _vertex_capacity * MAX_FRAMES_IN_FLIGHT, 24) ; 16MB total
   store32(ci, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 32)
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36)
   mut buf_ptr = sys_malloc(8)
   def res = create_buffer(_device, ci, 0, buf_ptr)
   if(res != 0){
      return false
   }
   _vertex_buffer = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, _vertex_buffer, mem_req)
   def size = load64(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   common.touch(type_bits)

   def mem_type_index = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
   if(mem_type_index == -1){ return false }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)

   mut mem_ptr = sys_malloc(8)
   def alloc_res = allocate_memory(_device, alloc_info, 0, mem_ptr)
   if(alloc_res != 0){
      return false
   }
   _vertex_memory = load64(mem_ptr, 0)

   bind_buffer_memory(_device, _vertex_buffer, _vertex_memory, 0)

   mut map_ptr = sys_malloc(8)
   def map_res = map_memory(_device, _vertex_memory, 0, size, 0, map_ptr)
   if(map_res == 0){
       _vertex_map = load64(map_ptr, 0)
   } else {
       _vertex_map = 0
   }

   ; Create CPU-side local buffer with enough room for all slices
   _local_vertex_map = sys_malloc(_vertex_capacity * MAX_FRAMES_IN_FLIGHT)
   true
}

fn _create_instance(){
   "Creates the Vulkan instance."
   ; Create all structures with system malloc to avoid any Nytrix metadata issues
   mut app_info = sys_malloc(48)
   memset(app_info, 0, 48)
   store32(app_info, VK_STRUCTURE_TYPE_APPLICATION_INFO, 0)
   store32(app_info, 1, 24)
   store32(app_info, 1, 40)
   store32(app_info, 0x00401000, 44)

   def exts_list = ui_glfw.required_extensions()
   def ext_count = get(exts_list, 0)
   def ext_ptrs = get(exts_list, 1)

   ; Create VkInstanceCreateInfo manually with explicit zeroing
   mut create_info = sys_malloc(64)
   memset(create_info, 0, 64)
   store32(create_info, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0)
   store64_raw(create_info, app_info, 24)
   store32(create_info, ext_count, 48) ; extensions
   store64_raw(create_info, ext_ptrs, 56)
   mut inst_ptr = sys_malloc(8)
   store32(inst_ptr, 0, 0) store32(inst_ptr, 0, 4)

   if(_debug_gfx_enabled){
      print("Vulkan: Creating instance with wrapper...")
      ; print("Vulkan: resolve vk_create_instance = " + to_str(vk_create_instance)) ; Removed debug print
   }

   def res = vk_create_instance(create_info, 0, inst_ptr)

   if(_debug_gfx_enabled){
      print("Vulkan: create_instance returned " + to_str(res))
      ; print("Vulkan: inst_ptr[0] = " + to_str(load64(inst_ptr, 0))) ; Removed debug print
   }

   if(res != 0){
      return false
   }
   _instance = load64(inst_ptr, 0)
   if(_debug_gfx_enabled){
      print("Vulkan: Instance created OK.")
      _dbg_handle("instance", _instance)
   }
   true
}

fn _create_surface(win){
   "Creates the native window surface (WSI)."
   def window = dict_get(win, "handle", 0)
   if(!window){
      return false
   }
   mut surf_ptr = sys_malloc(8)
   store32(surf_ptr, 0, 0) ; store32(surf_ptr, 0, 4)
   def res = ui_glfw.create_surface(_instance, window, 0, surf_ptr)
   if(res != 0){
      return false
   }
   _surface = load64(surf_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("surface", _surface) }
   true
}

fn _pick_physical_device(){
   "Selects a suitable physical GPU for rendering."
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   def _res1 = enumerate_physical_devices(_instance, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(count == 0){ return false }
   def _res2 = enumerate_physical_devices(_instance, count_ptr, 0) ; Added _res2 to avoid warning
   mut devices_ptr = sys_malloc(count * 8)
   enumerate_physical_devices(_instance, count_ptr, devices_ptr)
   _physical_device = load64(devices_ptr, 0)

   mut props = sys_malloc(1024)
   memset(props, 0, 1024)
   get_physical_device_properties(_physical_device, props)
   def device_name = text.cstr_to_str(props, 20)
   if(_debug_gfx_enabled){
      print("Vulkan: Selected GPU:", device_name)
      _dbg_handle("physical", _physical_device)
   }
   sys_free(devices_ptr)
   sys_free(props)
   true
}

fn _create_logical_device(){
   "Creates the logical Vulkan device and retrieves queues."
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   get_physical_device_queue_family_properties(_physical_device, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(count == 0){ return false }
   def prop_stride = 24
   mut props = sys_malloc(count * prop_stride)
   get_physical_device_queue_family_properties(_physical_device, count_ptr, props)
   mut graphics_family = -1
   mut i = 0
   while(i < count){
      def flags = load32(props, i * prop_stride)
      if((flags & 1) != 0){ ; VK_QUEUE_GRAPHICS_BIT
         graphics_family = i
         break
      }
      i += 1
   }
   if(graphics_family == -1){
      return false
   }
   _graphics_family_index = graphics_family
   ; Queue priority (1.0f in IEEE-754)
   mut priorities = sys_malloc(4)
   store32(priorities, 0x3f800000, 0)
   mut queue_create_info = sys_malloc(40)
   store32(queue_create_info, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0)
   store32(queue_create_info, 0, 8) store32(queue_create_info, 0, 12) ; pNext
   store32(queue_create_info, 0, 16) ; flags
   store32(queue_create_info, graphics_family, 20) ; queueFamilyIndex
   store32(queue_create_info, 1, 24) ; queueCount
   store64_raw(queue_create_info, priorities, 32) ; pQueuePriorities
   mut ext1 = sys_malloc(32)
   strcpy(ext1, "VK_KHR_swapchain")
   mut ext_ptrs = sys_malloc(8)
   store64_raw(ext_ptrs, ext1, 0)
   mut create_info = sys_malloc(72)
   store32(create_info, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0)
   store32(create_info, 0, 8) store32(create_info, 0, 12) ; pNext
   store32(create_info, 0, 16) ; flags
   store32(create_info, 1, 20) ; queueCreateInfoCount
   store64_raw(create_info, queue_create_info, 24) ; pQueueCreateInfos
   store32(create_info, 0, 32) ; enabledLayerCount
   store32(create_info, 0, 40) store32(create_info, 0, 44) ; ppEnabledLayerNames
   store32(create_info, 1, 48) ; enabledExtensionCount
   store64_raw(create_info, ext_ptrs, 56) ; ppEnabledExtensionNames
   store32(create_info, 0, 64) store32(create_info, 0, 68) ; pEnabledFeatures (set below)
   ; Enable wideLines and fillModeNonSolid
   mut dev_features = sys_malloc(232)
   memset(dev_features, 0, 232)
   store32(dev_features, 1, 52) ; fillModeNonSolid = VK_TRUE
   store32(dev_features, 1, 60) ; wideLines = VK_TRUE
   store64_raw(create_info, dev_features, 64)
   mut dev_ptr = sys_malloc(8)
   store32(dev_ptr, 0, 0) store32(dev_ptr, 0, 4)
   def res = create_device(_physical_device, create_info, 0, dev_ptr)
   if(res != 0){
      if(_debug_gfx_enabled){ print(f"Vulkan: create_device failed with {res}") }
      return false
   }
   _device = load64(dev_ptr, 0)
   if(_debug_gfx_enabled){
      print(f"Vulkan: Logical device created OK")
      _dbg_handle("device", _device)
   }
   mut q_ptr = sys_malloc(8)
   store32(q_ptr, 0, 0) store32(q_ptr, 0, 4)
   get_device_queue(_device, graphics_family, 0, q_ptr)
   _graphics_queue = load64(q_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("queue", _graphics_queue) }
   ; Use same queue for presenting for now (most GPUs support this)
   _present_queue = _graphics_queue
   true
}

fn _choose_composite_alpha(flags){
   "Selects a supported composite alpha mode. Forcing OPAQUE to avoid transparency issues."
   if(band(flags, 1)){ return 1 } ; OPAQUE
   if(band(flags, 2)){ return 2 } ; PRE_MULTIPLIED
   if(band(flags, 4)){ return 4 } ; POST_MULTIPLIED
   if(band(flags, 8)){ return 8 } ; INHERIT
   1
}

fn _choose_present_mode(){
   "Chooses the fastest present mode available (MAILBOX > IMMEDIATE > FIFO)."
   mut count_ptr = sys_malloc(4)
   get_physical_device_surface_present_modes_khr(_physical_device, _surface, count_ptr, 0)
   def count = load32(count_ptr, 0)
   mut modes_ptr = sys_malloc(count * 4)
   get_physical_device_surface_present_modes_khr(_physical_device, _surface, count_ptr, modes_ptr)

   mut mailbox_supported = false
   mut immediate_supported = false
   mut i = 0
   while(i < count){
      def mode = load32(modes_ptr, i * 4)
      if(mode == VK_PRESENT_MODE_MAILBOX_KHR){ mailbox_supported = true }
      if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR){ immediate_supported = true }
      i += 1
   }
   sys_free(count_ptr)
   sys_free(modes_ptr)

   if(_cfg_vsync){
      return VK_PRESENT_MODE_FIFO_KHR
   } else {
      if(immediate_supported){ return VK_PRESENT_MODE_IMMEDIATE_KHR }
      if(mailbox_supported){ return VK_PRESENT_MODE_MAILBOX_KHR }
      return VK_PRESENT_MODE_FIFO_KHR
   }
}

fn _create_headless_image(w, h){
   "Internal: creates an offscreen color image for headless rendering."
   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 1, 20) ; 2D
   store32(img_ci, 37, 24) ; RGBA8
   store32(img_ci, w, 28)
   store32(img_ci, h, 32)
   store32(img_ci, 1, 36) ; depth
   store32(img_ci, 1, 40) ; mip
   store32(img_ci, 1, 44) ; layers
   store32(img_ci, 1, 48) ; samples
   store32(img_ci, 0, 52) ; tiling optimal
   store32(img_ci, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 56)
   store32(img_ci, 0, 60) ; sharing exclusive
   store32(img_ci, 0, 80) ; layout undefined

   mut p = sys_malloc(8)
   create_image(_device, img_ci, 0, p)
   def img = load64(p, 0)

   mut req = sys_malloc(24)
   get_image_memory_requirements(_device, img, req)
   def mem_type = _find_memory_type(load32(req, 16), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut ai = sys_malloc(64)
   memset(ai, 0, 64)
   store32(ai, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(ai, load64(req, 0), 16)
   store32(ai, mem_type, 24)
   allocate_memory(_device, ai, 0, p)
   def mem = load64(p, 0)
   bind_image_memory(_device, img, mem, 0)
   img
}

fn _create_swapchain(win){
   "Initializes the Vulkan swapchain or simulated images for headless mode."
   if(!_surface){
      ; Headless: Create 3 simulated images
      _swapchain_extent_w = 400
      _swapchain_extent_h = 300
      if(win){
         _swapchain_extent_w = dict_get(win, "w", 400)
         _swapchain_extent_h = dict_get(win, "h", 300)
      }
      _swapchain_format = 37 ; RGBA8
      _swapchain_image_count = 3
      _swapchain_images = []
      mut i = 0
      while(i < 3){
         _swapchain_images = push(_swapchain_images, _create_headless_image(_swapchain_extent_w, _swapchain_extent_h))
         i += 1
      }
      return true
   }
   mut caps = sys_malloc(128)
   memset(caps, 0, 128)
   get_physical_device_surface_capabilities_khr(_physical_device, _surface, caps)
   mut req_w = 400
   mut req_h = 300
   if(win){
      req_w = int(dict_get(win, "w", 400))
      req_h = int(dict_get(win, "h", 300))
   }
   def cur_w = load32(caps, 8)
   def cur_h = load32(caps, 12)
   def min_w = load32(caps, 16)
   def min_h = load32(caps, 20)
   def max_w = load32(caps, 24)
   def max_h = load32(caps, 28)
   mut w = req_w
   mut h = req_h
   if(cur_w != -1 && cur_h != -1 && cur_w > 0 && cur_h > 0){
      w = cur_w
      h = cur_h
   } else {
      if(w < min_w){ w = min_w }
      if(h < min_h){ h = min_h }
      if(max_w > 0 && w > max_w){ w = max_w }
      if(max_h > 0 && h > max_h){ h = max_h }
   }
   _swapchain_extent_w = w
   _swapchain_extent_h = h
   mut min_imgs = load32(caps, 0)
   mut max_imgs = load32(caps, 4)
   mut count = min_imgs + 1
   if(max_imgs > 0 && count > max_imgs){ count = max_imgs }
   def pre_transform = load32(caps, 40)
   def composite_alpha = _choose_composite_alpha(load32(caps, 44))

   mut create_info = sys_malloc(128)
   memset(create_info, 0, 128)
   store32(create_info, VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0)
   store64_raw(create_info, _surface, 24)
   store32(create_info, count, 32)
   _swapchain_format = 44 ; VK_FORMAT_B8G8R8A8_UNORM
   store32(create_info, _swapchain_format, 36) ; format
   store32(create_info, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 40) ; colorSpace
   store32(create_info, w, 44) ; width
   store32(create_info, h, 48) ; height
   store32(create_info, 1, 52) ; layers
   store32(create_info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 56)
   store32(create_info, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(create_info, 0, 64) ; queueCount
   store32(create_info, 0, 72)
   store32(create_info, pre_transform, 80)
   store32(create_info, composite_alpha, 84)
   store32(create_info, _choose_present_mode(), 88)
   store32(create_info, 1, 92) ; clipped
   store32(create_info, 0, 96) ; oldSwapchain

   mut sc_ptr = sys_malloc(8)
   store32(sc_ptr, 0, 0)
   store32(sc_ptr, 0, 4)
   def res = create_swapchain_khr(_device, create_info, 0, sc_ptr)
   if(res != 0){
      if(_debug_gfx_enabled){ print(f"Vulkan: create_swapchain_khr failed with {res}") }
      return false
   }
   _swapchain = load64(sc_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("swapchain", _swapchain) }
   _swapchain_format = VK_FORMAT_B8G8R8A8_UNORM
   ; Get images
   mut img_count_ptr = sys_malloc(4)
   get_swapchain_images_khr(_device, _swapchain, img_count_ptr, 0)
   _swapchain_image_count = load32(img_count_ptr, 0)
   mut img_ptrs_raw = sys_malloc(_swapchain_image_count * 8)
   get_swapchain_images_khr(_device, _swapchain, img_count_ptr, img_ptrs_raw)
   _swapchain_images = []
   mut i = 0
   while(i < _swapchain_image_count){
      _swapchain_images = append(_swapchain_images, load64(img_ptrs_raw, i * 8))
      i += 1
   }
   sys_free(img_count_ptr)
   sys_free(img_ptrs_raw)
   true
}

fn _create_swapchain_image_views(){
   "Internal: creates image views for all swapchain images."
   _swapchain_image_views = []
   mut i = 0
   while(i < len(_swapchain_images)){
      mut ci = sys_malloc(80)
      memset(ci, 0, 80)
      store32(ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store64_raw(ci, get(_swapchain_images, i), 24)
      store32(ci, 1, 32) ; 2D
      store32(ci, _swapchain_format, 36)
      store32(ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(ci, 1, 64)
      store32(ci, 1, 72)
      mut view_ptr = sys_malloc(8)
      create_image_view(_device, ci, 0, view_ptr)
      _swapchain_image_views = append(_swapchain_image_views, load64(view_ptr, 0))
      sys_free(ci)
      sys_free(view_ptr)
      i += 1
   }
   true
}

fn _destroy_swapchain_objects(){
   "Releases swapchain-dependent resources (framebuffers, views, etc)."
   if(!_device){ return 0 }
   mut i = 0
   while(i < len(_framebuffers)){
      def fb = get(_framebuffers, i, 0)
      if(fb){ destroy_framebuffer(_device, fb, 0) }
      i += 1
   }
   _framebuffers = []
   i = 0
   while(i < len(_swapchain_image_views)){
      def iv = get(_swapchain_image_views, i, 0)
      if(iv){ destroy_image_view(_device, iv, 0) }
      i += 1
   }
   _swapchain_image_views = []
   if(_swapchain){
      destroy_swapchain_khr(_device, _swapchain, 0)
      _swapchain = 0
   }
   _swapchain_images = []
   _swapchain_image_count = 0
   0
}

fn _recreate_swapchain(){
   "Rebuilds the swapchain after window resize."
   if(!_window_ref || !_device){ return false }
   device_wait_idle(_device)
   _destroy_swapchain_objects()

   ; Clean up old depth + MSAA resources
   if(_depth_image){ destroy_image(_device, _depth_image, 0) _depth_image = 0 }
   if(_depth_view){ destroy_image_view(_device, _depth_view, 0) _depth_view = 0 }
   if(_depth_memory){ free_memory(_device, _depth_memory, 0) _depth_memory = 0 }
   if(_msaa_color_image){ destroy_image(_device, _msaa_color_image, 0) _msaa_color_image = 0 }
   if(_msaa_color_view){ destroy_image_view(_device, _msaa_color_view, 0) _msaa_color_view = 0 }
   if(_msaa_color_memory){ free_memory(_device, _msaa_color_memory, 0) _msaa_color_memory = 0 }

   if(!_create_swapchain(_window_ref)){ return false }
   if(!_create_swapchain_image_views()){ return false }

   ; Fix: Rebuild depth resources to match new swapchain size
   if(!_create_depth_resources()){ return false }

   if(!_create_framebuffers()){ return false }
   true
}

fn _create_image_views(){
   "Initializes Vulkan image views for each swapchain image."
   _swapchain_image_views = []
   mut i = 0
   while(i < _swapchain_image_count){
      def image_handle = get(_swapchain_images, i)
      mut create_info = sys_malloc(80)
      memset(create_info, 0, 80)
      store32(create_info, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store32(create_info, 0, 8) store32(create_info, 0, 12) ; pNext
      store32(create_info, 0, 16) ; flags
      store64_raw(create_info, image_handle, 24)
      store32(create_info, 1, 32) ; viewType (2D = 1)
      store32(create_info, _swapchain_format, 36)
      ; components (all identity=0)
      ; subresourceRange
      store32(create_info, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(create_info, 0, 60) ; baseMipLevel
      store32(create_info, 1, 64) ; levelCount
      store32(create_info, 0, 68) ; baseArrayLayer
      store32(create_info, 1, 72) ; layerCount
      mut view_ptr = sys_malloc(8)
      def iv_res = create_image_view(_device, create_info, 0, view_ptr)
      if(iv_res != 0){
         return false
      }
      def view_h = load64(view_ptr, 0)
      _swapchain_image_views = append(_swapchain_image_views, view_h)
      i += 1
   }
   true
}

fn _create_depth_resources(){
   "Allocates depth buffer and (if MSAA>1) MSAA color buffer for 3D rendering."
   def depth_format = 126 ; VK_FORMAT_D32_SFLOAT
   def samples = _cfg_msaa

   ;; --- Depth image (with MSAA samples) ---
   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16)
   store32(img_ci, 1, 20)
   store32(img_ci, depth_format, 24)
   store32(img_ci, _swapchain_extent_w, 28)
   store32(img_ci, _swapchain_extent_h, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, 1, 40)
   store32(img_ci, 1, 44)
   store32(img_ci, samples, 48) ; MSAA samples
   store32(img_ci, 0, 52)
   store32(img_ci, 32, 56) ; DEPTH_STENCIL_ATTACHMENT
   store32(img_ci, 0, 60)
   store32(img_ci, 0, 64)
   store32(img_ci, 0, 80)
   mut img_ptr = sys_malloc(8)
   if(create_image(_device, img_ci, 0, img_ptr) != 0){ return false }
   _depth_image = load64(img_ptr, 0)
   mut mem_req = sys_malloc(24)
   get_image_memory_requirements(_device, _depth_image, mem_req)
   def d_size = load64(mem_req, 0)
   def d_bits = load32(mem_req, 16)
   def d_mtype = _find_memory_type(d_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, d_size, 16)
   store32(alloc_info, d_mtype, 24)
   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return false }
   _depth_memory = load64(mem_ptr, 0)
   bind_image_memory(_device, _depth_image, _depth_memory, 0)
   mut view_ci = sys_malloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_raw(view_ci, _depth_image, 24)
   store32(view_ci, 1, 32)
   store32(view_ci, depth_format, 36)
   store32(view_ci, 0x00000002, 56)
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)
   mut view_ptr = sys_malloc(8)
   if(create_image_view(_device, view_ci, 0, view_ptr) != 0){ return false }
   _depth_view = load64(view_ptr, 0)

   ;; --- MSAA color image (only when samples > 1) ---
   if(samples > 1){
      mut ci2 = sys_malloc(88)
      memset(ci2, 0, 88)
      store32(ci2, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
      store32(ci2, 0, 16)
      store32(ci2, 1, 20)
      store32(ci2, _swapchain_format, 24) ; same format as swapchain
      store32(ci2, _swapchain_extent_w, 28)
      store32(ci2, _swapchain_extent_h, 32)
      store32(ci2, 1, 36)
      store32(ci2, 1, 40)
      store32(ci2, 1, 44)
      store32(ci2, samples, 48)
      store32(ci2, 0, 52) ; Tiling OPTIMAL
      store32(ci2, 0x00000010, 56) ; COLOR_ATTACHMENT_BIT (not TRANSIENT_ATTACHMENT_BIT for RADV stability)
      store32(ci2, 0, 60)
      store32(ci2, 0, 64)
      store32(ci2, 0, 80)
      mut ip2 = sys_malloc(8)
      if(create_image(_device, ci2, 0, ip2) != 0){ return false }
      _msaa_color_image = load64(ip2, 0)
      mut mr2 = sys_malloc(24)
      get_image_memory_requirements(_device, _msaa_color_image, mr2)
      def c_size = load64(mr2, 0)
      def c_bits = load32(mr2, 16)
      def c_mtype = _find_memory_type(c_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      mut ai2 = sys_malloc(64)
      memset(ai2, 0, 64)
      store32(ai2, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
      store64_raw(ai2, c_size, 16)
      store32(ai2, c_mtype, 24)
      mut mp2 = sys_malloc(8)
      if(allocate_memory(_device, ai2, 0, mp2) != 0){ return false }
      _msaa_color_memory = load64(mp2, 0)
      bind_image_memory(_device, _msaa_color_image, _msaa_color_memory, 0)
      mut vc2 = sys_malloc(80)
      memset(vc2, 0, 80)
      store32(vc2, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store64_raw(vc2, _msaa_color_image, 24)
      store32(vc2, 1, 32)
      store32(vc2, _swapchain_format, 36)
      store32(vc2, 0x00000001, 56) ; ASPECT_COLOR
      store32(vc2, 1, 64)
      store32(vc2, 1, 72)
      mut vp2 = sys_malloc(8)
      if(create_image_view(_device, vc2, 0, vp2) != 0){ return false }
      _msaa_color_view = load64(vp2, 0)
   }
   true
}

fn _create_render_pass(){
   "Defines the Vulkan render pass. Uses 3 attachments (MSAA color + depth + resolve) when MSAA>1, or 2 (color + depth) otherwise."
   def samples = _cfg_msaa
   def msaa = samples > 1

   if(msaa){
      ;; === 3-attachment MSAA render pass ===
      ;; att 0: MSAA color (multisample, DONT_CARE store, COLOR_ATTACHMENT_OPTIMAL final)
      ;; att 1: depth (multisample)
      ;; att 2: resolve (1-sample, STORE, PRESENT_SRC_KHR)
      mut atts = sys_malloc(108) ; 3 * 36 bytes
      memset(atts, 0, 108)
      ; att 0 - MSAA color
      store32(atts, _swapchain_format, 4)
      store32(atts, samples, 8)
      store32(atts, 1, 12) ; loadOp CLEAR
      store32(atts, 2, 16) ; storeOp DONT_CARE (MSAA image doesn't need to be stored)
      store32(atts, 2, 20)
      store32(atts, 2, 24)
      store32(atts, 0, 28) ; initialLayout UNDEFINED
      store32(atts, 2, 32) ; finalLayout COLOR_ATTACHMENT_OPTIMAL
      ; att 1 - depth
      store32(atts, 126, 36+4)
      store32(atts, samples, 36+8)
      store32(atts, 1, 36+12) ; loadOp CLEAR
      store32(atts, 2, 36+16) ; storeOp DONT_CARE
      store32(atts, 2, 36+20)
      store32(atts, 2, 36+24)
      store32(atts, 0, 36+28)
      store32(atts, 3, 36+32) ; DEPTH_STENCIL_ATTACHMENT_OPTIMAL
      ; att 2 - resolve (1 sample, swapchain)
      store32(atts, _swapchain_format, 72+4)
      store32(atts, 1, 72+8)
      store32(atts, 2, 72+12) ; loadOp DONT_CARE
      store32(atts, 0, 72+16) ; storeOp STORE
      store32(atts, 2, 72+20)
      store32(atts, 2, 72+24)
      store32(atts, 0, 72+28) ; UNDEFINED
      store32(atts, 1000001002, 72+32) ; PRESENT_SRC_KHR

      mut car = sys_malloc(8) store32(car, 0, 0) store32(car, 2, 4) ; att0 COLOR_ATTACHMENT_OPTIMAL
      mut dar = sys_malloc(8) store32(dar, 1, 0) store32(dar, 3, 4) ; att1 DEPTH_STENCIL_ATTACHMENT_OPTIMAL
      mut rar = sys_malloc(8) store32(rar, 2, 0) store32(rar, 2, 4) ; att2 COLOR_ATTACHMENT_OPTIMAL (resolve)

      mut sd = sys_malloc(72)
      memset(sd, 0, 72)
      store32(sd, 0, 4) ; pipelineBindPoint = GRAPHICS
      store32(sd, 1, 24) ; colorAttachmentCount = 1
      store64_raw(sd, car, 32) ; pColorAttachments (offset 32)
      store64_raw(sd, rar, 40) ; pResolveAttachments (offset 40) — resolves MSAA to swapchain
      store64_raw(sd, dar, 48) ; pDepthStencilAttachment (offset 48)

      mut dep = sys_malloc(28)
      store32(dep, -1, 0)
      store32(dep, 0, 4)
      store32(dep, 0x00000400, 8)
      store32(dep, 0x00000400, 12)
      store32(dep, 0, 16)
      store32(dep, 0x00000100 | 0x00000010, 20)
      store32(dep, 0, 24)

      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
      store32(create_info, 3, 20) ; 3 attachments
      store64_raw(create_info, atts, 24)
      store32(create_info, 1, 32)
      store64_raw(create_info, sd, 40)
      store32(create_info, 1, 48)
      store64_raw(create_info, dep, 56)

      mut pass_ptr = sys_malloc(8)
      if(create_render_pass(_device, create_info, 0, pass_ptr) != 0){ return false }
      _render_pass = load64(pass_ptr, 0)
   } else {
      ;; === 2-attachment non-MSAA render pass ===
      mut atts = sys_malloc(72)
      memset(atts, 0, 72)
      store32(atts, _swapchain_format, 4)
      store32(atts, 1, 8)
      store32(atts, 1, 12) ; CLEAR
      store32(atts, 0, 16) ; STORE
      store32(atts, 2, 20) store32(atts, 2, 24)
      store32(atts, 0, 28) store32(atts, 1000001002, 32) ; PRESENT_SRC_KHR
      store32(atts, 126, 36+4)
      store32(atts, 1, 36+8)
      store32(atts, 1, 36+12) ; loadOp CLEAR
      store32(atts, 2, 36+16) ; storeOp DONT_CARE
      store32(atts, 2, 36+20) store32(atts, 2, 36+24)
      store32(atts, 0, 36+28) store32(atts, 3, 36+32)

      mut car = sys_malloc(8) store32(car, 0, 0) store32(car, 2, 4)
      mut dar = sys_malloc(8) store32(dar, 1, 0) store32(dar, 3, 4)

      mut sd = sys_malloc(72)
      memset(sd, 0, 72)
      store32(sd, 0, 4)
      store32(sd, 1, 24)
      store64_raw(sd, car, 32)
      store64_raw(sd, dar, 48)

      mut dep = sys_malloc(28)
      store32(dep, -1, 0) store32(dep, 0, 4)
      store32(dep, 0x00000400, 8) store32(dep, 0x00000400, 12)
      store32(dep, 0, 16) store32(dep, 0x00000100 | 0x00000010, 20)
      store32(dep, 0, 24)

      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
      store32(create_info, 2, 20)
      store64_raw(create_info, atts, 24)
      store32(create_info, 1, 32)
      store64_raw(create_info, sd, 40)
      store32(create_info, 1, 48)
      store64_raw(create_info, dep, 56)

      mut pass_ptr = sys_malloc(8)
      if(create_render_pass(_device, create_info, 0, pass_ptr) != 0){ return false }
      _render_pass = load64(pass_ptr, 0)
   }
   true
}

fn _create_framebuffers(){
   "Creates Vulkan framebuffers. When MSAA>1: [msaa_color, depth, resolve(swapchain)]. Otherwise: [swapchain, depth]."
   _framebuffers = []
   def msaa = _cfg_msaa > 1
   mut i = 0
   while(i < _swapchain_image_count){
      mut attach_ptr = 0
      mut att_count = 0
      if(msaa){
         attach_ptr = sys_malloc(24)
         store64_raw(attach_ptr, _msaa_color_view, 0) ; att0 MSAA color
         store64_raw(attach_ptr, _depth_view, 8) ; att1 depth
         store64_raw(attach_ptr, get(_swapchain_image_views, i), 16) ; att2 resolve
         att_count = 3
      } else {
         attach_ptr = sys_malloc(16)
         store64_raw(attach_ptr, get(_swapchain_image_views, i), 0)
         store64_raw(attach_ptr, _depth_view, 8)
         att_count = 2
      }
      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0)
      store64_raw(create_info, _render_pass, 24)
      store32(create_info, att_count, 32)
      store64_raw(create_info, attach_ptr, 40)
      store32(create_info, _swapchain_extent_w, 48)
      store32(create_info, _swapchain_extent_h, 52)
      store32(create_info, 1, 56)
      mut fb_ptr = sys_malloc(8)
      if(_debug_gfx_enabled){ print(f"Vulkan: Creating framebuffer {i}...") }
      if(create_framebuffer(_device, create_info, 0, fb_ptr) != 0){ return false }
      def fb = load64(fb_ptr, 0)
      if(_debug_gfx_enabled){ _dbg_handle(f"framebuffer {i}", fb) }
      _framebuffers = append(_framebuffers, fb)
      i += 1
   }
   if(_debug_gfx_enabled){ print("Vulkan: All framebuffers created.") }
   true
}

fn _create_sync_objects(){
   "Initializes semaphores and fences for frame synchronization."
   _image_available_semaphores = []
   _render_finished_semaphores = []
   _in_flight_fences = []
   mut i = 0
   while(i < MAX_FRAMES_IN_FLIGHT){
      mut si = sys_malloc(24)
      memset(si, 0, 24)
      store32(si, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0)
      store32(si, 0, 8) store32(si, 0, 12) ; pNext
      store32(si, 0, 16) ; flags
      mut sem1 = sys_malloc(8)
      def s1_res = create_semaphore(_device, si, 0, sem1)
      if(s1_res != 0){
         return false
      }
      _image_available_semaphores = append(_image_available_semaphores, load64(sem1, 0))
      mut sem2 = sys_malloc(8)
      def s2_res = create_semaphore(_device, si, 0, sem2)
      if(s2_res != 0){
         return false
      }
      _render_finished_semaphores = append(_render_finished_semaphores, load64(sem2, 0))
      mut fi = sys_malloc(24)
      memset(fi, 0, 24)
      store32(fi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
      store32(fi, 0, 8) store32(fi, 0, 12) ; pNext
      store32(fi, 1, 16) ; flags (1 = SIGNAL_BIT)
      mut fence = sys_malloc(8)
      def f_res = create_fence(_device, fi, 0, fence)
      if(f_res != 0){
         return false
      }
      _in_flight_fences = append(_in_flight_fences, load64(fence, 0))
      i += 1
   }
   true
}

fn _create_command_pool(){
   "Creates the Vulkan command pool for recording draw commands."
   mut create_info = sys_malloc(32)
   memset(create_info, 0, 32)
   store32(create_info, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0)
   store32(create_info, 2, 16) ; flags (2 = RESET_BIT)
   store32(create_info, _graphics_family_index, 20)
   mut pool_ptr = sys_malloc(8)
   def cp_res = create_command_pool(_device, create_info, 0, pool_ptr)
   if(cp_res != 0){
      return false
   }
   _command_pool = load64(pool_ptr, 0)
   true
}

fn _create_command_buffers(){
   "Allocates primary command buffers from the pool."
   mut ai = sys_malloc(32)
   memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_raw(ai, _command_pool, 16)
   store32(ai, 0, 24) ; level (0 = PRIMARY)
   store32(ai, MAX_FRAMES_IN_FLIGHT, 28)
   mut bufs_ptr = sys_malloc(MAX_FRAMES_IN_FLIGHT * 8)
   def cb_res = allocate_command_buffers(_device, ai, bufs_ptr)
   if(cb_res != 0){
      return false
   }
   _command_buffers = []
   mut i = 0
   while(i < MAX_FRAMES_IN_FLIGHT){
      _command_buffers = append(_command_buffers, load64(bufs_ptr, i * 8))
      i += 1
   }
   true
}

fn compile_glsl_to_spirv(source, stage_ext){
   "Compiles GLSL source string to SPIR-V bytes using glslc."
   def tmp_src = f"/build/cache/ny_shader_custom.{stage_ext}"
   def tmp_spv = f"/build/cache/ny_shader_custom.{stage_ext}.spv"
   unwrap(file_write(tmp_src, source))
   if(proc.run("glslc", ["glslc", tmp_src, "-o", tmp_spv]) != 0){ return 0 }
   def res = file_read(tmp_spv)
   if(is_err(res)){ return 0 }
   unwrap(res)
}

fn create_shader_module_from_source(source, stage_ext){
   "Compiles GLSL source and creates a Vulkan shader module."
   def spirv = compile_glsl_to_spirv(source, stage_ext)
   if(!spirv){ return 0 }

   def size = len(spirv)
   mut ci = sys_malloc(128)
   memset(ci, 0, 128)
   store32(ci, 16, 0) ; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store64_raw(ci, size, 24)
   store64_raw(ci, spirv, 32)

   mut mod_ptr = sys_malloc(8)
   if(create_shader_module(_device, ci, 0, mod_ptr) != 0){ return 0 }
   def sm_low = load32(mod_ptr, 0)
   def sm_high = load32(mod_ptr, 4)
   (sm_high * 4294967296) + (sm_low & 0xFFFFFFFF)
}

fn create_pipeline(vert_mod, frag_mod, topology=3, depth_test=1, depth_write=1, cull_mode=0, front_face=0, depth_bias=0, depth_clamp=0){
   "Creates a custom graphics pipeline. topology: 3=TRI_LIST, 1=LINE_LIST."
   mut main_str = sys_malloc(8)
   strcpy(main_str, "main")

   def s1 = VkPipelineShaderStageCreateInfo(1, vert_mod, main_str)
   def s2 = VkPipelineShaderStageCreateInfo(16, frag_mod, main_str)
   mut stages = sys_malloc(96)
   memcpy(stages, s1, 48)
   memcpy(stages + 48, s2, 48)

   ; Use built-in descriptors and layout for now
   ; (Could be extended to accept custom layouts)
   def pipe_layout = _pipeline_layout

   ; Vertex Input (using standard 36-byte stride)
   mut binding_desc = sys_malloc(12)
   store32(binding_desc, 0, 0)
   store32(binding_desc, 36, 4)
   store32(binding_desc, 0, 8)

   mut attr_desc = sys_malloc(64)
   store32(attr_desc, 0, 0) store32(attr_desc, 0, 4) store32(attr_desc, 106, 8) store32(attr_desc, 0, 12)
   store32(attr_desc, 1, 16) store32(attr_desc, 0, 20) store32(attr_desc, 103, 24) store32(attr_desc, 12, 28)
   store32(attr_desc, 2, 32) store32(attr_desc, 0, 36) store32(attr_desc, 37, 40) store32(attr_desc, 20, 44)
   store32(attr_desc, 3, 48) store32(attr_desc, 0, 52) store32(attr_desc, 106, 56) store32(attr_desc, 24, 60)
   def vi = VkPipelineVertexInputStateCreateInfo(1, binding_desc, 4, attr_desc)

   def ia = VkPipelineInputAssemblyStateCreateInfo(topology, 0)
   def viewport_state = VkPipelineViewportStateCreateInfo(1, 0, 1, 0)
   ; 0 is CCW, 1 is CW. We use negative viewport height (Vulkan 1.1+ feature),
   ; which flips the Y coordinate and thus reverses the winding. Use CW (1) to compensate.
   ; 0 is CCW, 1 is CW.
   def rs = VkPipelineRasterizationStateCreateInfo(depth_clamp, 0, 0, cull_mode, front_face, depth_bias, 1.25, 0.0, 1.75, 1.0)
   def ms = VkPipelineMultisampleStateCreateInfo(_cfg_msaa, 0, 0.0, 0, 0, 0)
   def cba = VkPipelineColorBlendAttachmentState(1, 6, 7, 0, 1, 7, 0, 15)
   def cb = VkPipelineColorBlendStateCreateInfo(0, 0, 1, cba, 0)
   def dss = VkPipelineDepthStencilStateCreateInfo(depth_test, depth_write, 3, 0, 0, 0, 0, 0.0, 1.0)

   mut dyn_states = sys_malloc(12)
   store32(dyn_states, 0, 0)
   store32(dyn_states, 1, 4)
   store32(dyn_states, 2, 8) ; line width
   def ds = VkPipelineDynamicStateCreateInfo(3, dyn_states)

   def ci = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs, ms, dss, cb, ds, pipe_layout, _render_pass, 0, 0, -1)
   mut pipe_ptr = sys_malloc(8)
   if(create_graphics_pipelines(_device, 0, 1, ci, 0, pipe_ptr) != 0){ return 0 }
   load64(pipe_ptr, 0)
}

fn bind_pipeline(pipe){
   "Binds a custom graphics pipeline for subsequent draw calls. Pass 0 to restore default."
   if(!_frame_open){ return }
   mut p = pipe
   if(p == 0){ p = _pipeline }
   if(p == _last_bound_pipe){ _target_pipeline = p return }
   _flush()
   def cb = get(_command_buffers, _current_frame)
   cmd_bind_pipeline(cb, 0, p)
   _last_bound_pipe = p
   _target_pipeline = p
   _pc_dirty = true ; force push constants for new pipeline
}

fn push_constants(ptr, size, offset=0){
   "Pushes raw data to the current pipeline's push constants and caches it for flushes."
   if(!_frame_open || !ptr || size <= 0){ return }
   if(offset + size > 160){ return }

   ; Cache in _pc_buffer so automatic _flush doesn't clobber it
   memcpy(_pc_buffer + offset, ptr, size)
   _pc_dirty = true

   def cb = get(_command_buffers, _current_frame)
   cmd_push_constants(cb, _pipeline_layout, 1 | 16, offset, size, ptr)
}

fn _get_default_pipeline(){
   "Internal: returns the default triangle pipeline handle."
   _pipeline
}

fn _create_shader_module(path){
   "Loads a SPIR-V shader file and creates a Vulkan shader module handle."
   def res = file_read(path)
   if(is_err(res)){
      return 0
   }
   def code = unwrap(res)
   def size = len(code)
   mut ci = sys_malloc(128)
   memset(ci, 0, 128)
   store32(ci, 16, 0) ; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store32(ci, 0, 8) store32(ci, 0, 12)
   store32(ci, 0, 16) ; flags
   store64_raw(ci, size, 24) ; codeSize (bytes)
   store64_raw(ci, code, 32) ; pCode
   mut mod_ptr = sys_malloc(8)
   def vk_res = create_shader_module(_device, ci, 0, mod_ptr)
   if(vk_res != 0){
      return 0
   }
   def sm_low = load32(mod_ptr, 0)
   def sm_high = load32(mod_ptr, 4)
   (sm_high * 4294967296) + (sm_low & 0xFFFFFFFF)
}

fn _ensure_shader_binaries(){
   "Internal helper to compile default shader sources via glslc."
   def vert_spv = "/build/cache/ny_shader.vert.spv"
   def frag_spv = "/build/cache/ny_shader.frag.spv"
   if(is_str(_cfg_vert_spv) && file_exists(_cfg_vert_spv)){
      proc.run("cp", ["cp", _cfg_vert_spv, vert_spv])
     } else {
      def vert_src = "#version 450\nlayout(location=0) in vec3 inPos;\nlayout(location=1) in vec2 inUV;\nlayout(location=2) in vec4 inColor;\nlayout(location=3) in vec3 inNormal;\nlayout(push_constant) uniform PC { mat4 vp; mat4 model; int isMask; int isUnlit; } pc;\nlayout(location=0) out vec4 vColor;\nlayout(location=1) out vec2 vUV;\nlayout(location=2) out vec3 vNormal;\nvoid main(){\n  gl_Position = pc.vp * pc.model * vec4(inPos, 1.0);\n  vColor = inColor;\n  vUV = inUV;\n  vNormal = mat3(pc.model) * inNormal;\n}\n"
      unwrap(file_write("/build/cache/ny_shader.vert", vert_src))
      if(proc.run("glslc", ["glslc", "/build/cache/ny_shader.vert", "-o", vert_spv]) != 0){ return false }
     }
   if(is_str(_cfg_frag_spv) && file_exists(_cfg_frag_spv)){
       proc.run("cp", ["cp", _cfg_frag_spv, frag_spv])
     } else {
      def frag_src = "#version 450\n" +
         "layout(location=0) in vec4 vColor;\n" +
         "layout(location=1) in vec2 vUV;\n" +
         "layout(location=2) in vec3 vNormal;\n" +
         "layout(push_constant) uniform PC { mat4 vp; mat4 model; int isMask; int isUnlit; } pc;\n" +
         "layout(binding=0) uniform sampler2D texSampler;\n" +
         "layout(location=0) out vec4 outColor;\n" +
         "void main(){\n" +
         "  vec4 tex = texture(texSampler, vUV);\n" +
         "  if(pc.isMask != 0){ tex = vec4(1.0, 1.0, 1.0, tex.r); }\n" +
         "  if(pc.isUnlit != 0){\n" +
         "     outColor = vColor * tex;\n" +
             "  } else {\n" +
             "     vec3 normal = normalize(vNormal + vec3(0.0, 0.0, 0.0001));\n" +
             "     vec3 l = normalize(vec3(0.5, 1.0, 0.5));\n" +
             "     float diff = max(dot(normal, l), 0.1);\n" +
             "     vec3 skyCol = vec3(0.5, 0.7, 1.0); vec3 groundCol = vec3(0.12, 0.12, 0.15);\n" +
             "     vec3 ambient = mix(groundCol, skyCol, normal.y * 0.5 + 0.5) * 0.4;\n" +
             "     outColor = vColor * tex * vec4(ambient + diff * 0.7, 1.0);\n" +
             "  }\n" +
         "}\n"
       unwrap(file_write("/build/cache/ny_shader.frag", frag_src))
       if(proc.run("glslc", ["glslc", "/build/cache/ny_shader.frag", "-o", frag_spv]) != 0){ return false }
   }
   file_exists(vert_spv) && file_exists(frag_spv)
}

fn _create_graphics_pipeline(){
   "Configures and creates the graphics pipeline (shaders, vertex input, blending)."
   if(!_ensure_shader_binaries()){
      return false
   }
   _vert_module = _create_shader_module("/build/cache/ny_shader.vert.spv")
   _frag_module = _create_shader_module("/build/cache/ny_shader.frag.spv")
   if(!_vert_module || !_frag_module){ return false }

   ; Descriptor Set Layout
   def dsl_binding = VkDescriptorSetLayoutBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0)
   def dsl_ci = VkDescriptorSetLayoutCreateInfo(1, dsl_binding)

   mut dsl_ptr = sys_malloc(8)
   def dsl_res = create_descriptor_set_layout(_device, dsl_ci, 0, dsl_ptr)
   if(dsl_res != 0){
      return false
   }
   _descriptor_set_layout = load64(dsl_ptr, 0)

   ; Pipeline Layout
   mut pc_range = sys_malloc(12)
   store32(pc_range, 1 | 16, 0) ; STAGE_VERTEX | STAGE_FRAGMENT
   store32(pc_range, 0, 4)
   store32(pc_range, 160, 8) ; size 160 (aligned)
   mut dsl_arr = sys_malloc(8)
   store64_raw(dsl_arr, _descriptor_set_layout, 0)

   def layout_ci = VkPipelineLayoutCreateInfo(1, dsl_arr, 1, pc_range)

   mut layout_ptr = sys_malloc(8)
   def pl_res = create_pipeline_layout(_device, layout_ci, 0, layout_ptr)
   if(pl_res != 0){
      return false
   }
   _pipeline_layout = load64(layout_ptr, 0)

   ; Vertex Input State
   mut binding_desc = sys_malloc(12)
   store32(binding_desc, 0, 0) ; binding
   store32(binding_desc, 36, 4) ; stride (36)
   store32(binding_desc, 0, 8) ; inputRate VERTEX

   mut attr_desc = sys_malloc(64) ; 4 attributes
   ; 0: Position (vec3) offset 0
   store32(attr_desc, 0, 0) store32(attr_desc, 0, 4) store32(attr_desc, 106, 8) store32(attr_desc, 0, 12)
   ; 1: UV (vec2) offset 12
   store32(attr_desc, 1, 16) store32(attr_desc, 0, 20) store32(attr_desc, 103, 24) store32(attr_desc, 12, 28)
   ; 2: Color (R8G8B8A8_UNORM) offset 20
   store32(attr_desc, 2, 32) store32(attr_desc, 0, 36) store32(attr_desc, 37, 40) store32(attr_desc, 20, 44)
   ; 3: Normal (vec3) offset 24
   store32(attr_desc, 3, 48) store32(attr_desc, 0, 52) store32(attr_desc, 106, 56) store32(attr_desc, 24, 60)

   def vi = VkPipelineVertexInputStateCreateInfo(1, binding_desc, 4, attr_desc)

   ; Common States
   def viewport_state = VkPipelineViewportStateCreateInfo(1, 0, 1, 0)
   def rs_cull = VkPipelineRasterizationStateCreateInfo(0, 0, 0, 2, 0, 0, 0, 0.0, 0.0, 1.0) ; cull=BACK(2), front=CCW(0)
   def rs_nocull = VkPipelineRasterizationStateCreateInfo(0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 1.0) ; cull=NONE(0), front=CCW(0)
   def ms = VkPipelineMultisampleStateCreateInfo(_cfg_msaa, 0, 0.0, 0, 0, 0)
   def cba = VkPipelineColorBlendAttachmentState(1, 6, 7, 0, 1, 7, 0, 15) ; blend=1, srcC=SRC_ALPHA(6), dstC=ONE_MINUS_SRC_ALPHA(7), srcA=ONE(1), dstA=ONE_MINUS_SRC_ALPHA(7)
   def cb = VkPipelineColorBlendStateCreateInfo(0, 0, 1, cba, 0)

   ; Depth Stencil State (Enabled for 3D)

   mut dyn_states = sys_malloc(8)
   store32(dyn_states, 0, 0)
   store32(dyn_states, 1, 4)
   def ds = VkPipelineDynamicStateCreateInfo(2, dyn_states)

   ; Pipeline
   mut main_str = sys_malloc(8)
   strcpy(main_str, "main")

   def s1 = VkPipelineShaderStageCreateInfo(1, _vert_module, main_str)
   def s2 = VkPipelineShaderStageCreateInfo(16, _frag_module, main_str)
   ; Pack two stage structs contiguously (48 bytes each)
   mut stages = sys_malloc(96)
   memcpy(stages, s1, 48)
   memcpy(stages + 48, s2, 48)

   ; 1. Create Lit Pipeline (with depth test)
   def ia = VkPipelineInputAssemblyStateCreateInfo(3, 0)
   ; Enable robust Depth Testing & Writing for 3D Mesh culling
   def dss = VkPipelineDepthStencilStateCreateInfo(1, 1, 3, 0, 0, 0, 0, 0.0, 1.0)
   common.touch(dss)
   def ci = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_cull, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   mut pipe_ptr = sys_malloc(8)
   store32(pipe_ptr, 0, 0) store32(pipe_ptr, 0, 4)
   if(_debug_gfx_enabled){
      print(f"Vulkan: Creating graphics pipeline with device={_device} layout={_pipeline_layout} pass={_render_pass}")
   }
   def res = create_graphics_pipelines(_device, 0, 1, ci, 0, pipe_ptr)
   if(_debug_gfx_enabled){ print(f"Vulkan: create_graphics_pipelines returned {res}") }
   if(res != 0){ return false }

   if(_debug_gfx_enabled){ print("Vulkan: Loading graphics pipeline handle...") }
   _pipeline = load64(pipe_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("pipeline", _pipeline) }

   ; 2. Create Unlit Pipeline (no depth test)
   def dss_unlit = VkPipelineDepthStencilStateCreateInfo(0, 0, 0, 0, 0, 0, 0, 0.0, 1.0)
     def ci_unlit = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_nocull, ms, dss_unlit, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_unlit, 0, pipe_ptr) == 0){
       _unlit_pipeline = load64(pipe_ptr, 0)
   }

   ; 3. Create Line Pipeline (for robust line rendering)
   def ia_line = VkPipelineInputAssemblyStateCreateInfo(1, 0) ; topology=LINE_LIST
   def ci_line = VkGraphicsPipelineCreateInfo(2, stages, vi, ia_line, 0, viewport_state, rs_nocull, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_line, 0, pipe_ptr) == 0){
       _line_pipeline = load64(pipe_ptr, 0)
   }

   ; 4. Create Wireframe Pipeline (PolygonMode=LINE=1, Cull=NONE=0)
   def rs_wire = VkPipelineRasterizationStateCreateInfo(0, 0, 1, 0, 0, 0, 0, 0.0, 0.0, 1.0)
   def ci_wire = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_wire, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_wire, 0, pipe_ptr) == 0){
       _wire_pipeline = load64(pipe_ptr, 0)
   }

   if(_debug_gfx_enabled){ print("Vulkan: Graphics pipeline initialization complete.") }
   true
}

fn begin_frame(){
   "Prepares the renderer for a new frame (sync, acquire image, begin recording)."
   if(!_device){ return false }

   if(_window_ref){
      mut cur_ww = int(dict_get(_window_ref, "w", _swapchain_extent_w))
      mut cur_wh = int(dict_get(_window_ref, "h", _swapchain_extent_h))

      ;; Handle minimization: spin until window is restored
      if(cur_ww == 0 || cur_wh == 0){
         while(cur_ww == 0 || cur_wh == 0){
         if(!_window_ref){ return false }
         msleep(10) ; yield to OS
         ;; Note: must poll here if not already doing so on main thread, but assume ui.ny does it.
         ;; Re-read size from dict (updated by GLFW callback in std.ui.window)
         cur_ww = int(dict_get(_window_ref, "w", 0))
         cur_wh = int(dict_get(_window_ref, "h", 0))
         }
      }

      if(cur_ww != _swapchain_extent_w || cur_wh != _swapchain_extent_h){
         if(_debug_gfx_enabled){ print(f"Vulkan: Window resized {cur_ww}x{cur_wh}") }
         if(!_recreate_swapchain()){ return false }
      }
   }

   _frame_open = false

   ; Wait for previous frame's fence
   def fence = get(_in_flight_fences, _current_frame)
   store64_raw(_ptr_fence, fence, 0)
   def wf = wait_for_fences(_device, 1, _ptr_fence, 1, 0xFFFFFFFFFFFFFFFF)
   if(wf != 0){ return false }
   ; Reset the fence BEFORE recording starts to avoid driver race
   reset_fences(_device, 1, _ptr_fence)

   ; Capture current image index
   mut acq = 0
   def sem = get(_image_available_semaphores, _current_frame)
   if(_surface){
      acq = acquire_next_image_khr(_device, _swapchain, 0xFFFFFFFFFFFFFFFF, sem, 0, _ptr_img_idx)
      if(acq == 0xC460C464 || acq == -1000001004){
         if(_debug_gfx_enabled){ print("Vulkan: Acquire next image out of date") }
         _recreate_swapchain()
         return false
      }
      if(acq != 0 && acq != 1000001003){ return false }
      _image_index = load32(_ptr_img_idx, 0)
   } else {
      _image_index = (_image_index + 1) % _swapchain_image_count
   }

   ; Reset + begin recording command buffer
   def cb = get(_command_buffers, _current_frame)
   memset(_ptr_bi, 0, 32)
   store32(_ptr_bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   if(begin_command_buffer(cb, _ptr_bi) != 0){ return false }

   ; Begin Render Pass
   ; Set clear values: color + depth (+ resolve slot if MSAA)
   ; VkClearValue is 16 bytes each: clear[0]=color@0, clear[1]=depth@16, clear[2]=resolve@32
   memset(_ptr_clear, 0, 96)
   store32_f32(_ptr_clear, _clear_r, 0) ; clear[0].color.r
   store32_f32(_ptr_clear, _clear_g, 4) ; clear[0].color.g
   store32_f32(_ptr_clear, _clear_b, 8) ; clear[0].color.b
   store32_f32(_ptr_clear, _clear_a, 12) ; clear[0].color.a
   store32_f32(_ptr_clear, 1.0, 16) ; clear[1].depthStencil.depth = 1.0
   store32(_ptr_clear, 0, 20) ; clear[1].depthStencil.stencil = 0

   def clear_count = (_cfg_msaa > 1) ? 3 : 2

   memset(_ptr_ri, 0, 64)
   store32(_ptr_ri, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0)
   store64_raw(_ptr_ri, _render_pass, 16)
   store64_raw(_ptr_ri, get(_framebuffers, _image_index), 24)
   store32(_ptr_ri, _swapchain_extent_w, 40)
   store32(_ptr_ri, _swapchain_extent_h, 44)
   store32(_ptr_ri, clear_count, 48)
   store64_raw(_ptr_ri, _ptr_clear, 56)
   cmd_begin_render_pass(cb, _ptr_ri, 0)

   ; Set dynamic viewport/scissor (pre-allocated)
   ; Vulkan Y-flip via negative viewport height
   store32_f32(_ptr_vp, 0.0, 0)
   store32_f32(_ptr_vp, float(_swapchain_extent_h), 4)
   store32_f32(_ptr_vp, float(_swapchain_extent_w), 8)
   store32_f32(_ptr_vp, -float(_swapchain_extent_h), 12)
   store32_f32(_ptr_vp, 0.0, 16)
   store32_f32(_ptr_vp, 1.0, 20)
   cmd_set_viewport(cb, 0, 1, _ptr_vp)

   store32(_ptr_sci, 0, 0)
   store32(_ptr_sci, 0, 4)
   store32(_ptr_sci, _swapchain_extent_w, 8)
   store32(_ptr_sci, _swapchain_extent_h, 12)
   cmd_set_scissor(cb, 0, 1, _ptr_sci)

   _frame_open = true
   _vertex_offset = 0
   _last_flush_offset = 0
   _total_frames += 1
   _fps_count += 1
   def now_t = get_time()
   if(now_t - _fps_last_time >= 1.0){
      _fps_curr = _fps_count
      _fps_count = 0
      _fps_last_time = now_t
   }

   _update_default_mvp(_window_ref)

   ; Reset per-frame vertex and state tracking
   _vertex_offset = 0
   _last_flush_offset = 0
   _vertex_limit_hit = false
   _current_frame_vertex_offset = _current_frame * _vertex_capacity

   ; MUST reset these so bind_texture and _flush re-issue commands to the NEW command buffer
   _last_bound_ds = 0
   _last_bound_tex_id = -1
   _last_bound_pipe = 0
   _target_pipeline = _pipeline ; Default to main pipeline
   _current_texture_id = -1 ; Force next bind_texture to actually do work
   _mvp_dirty = true
   _pc_dirty = true
   _last_is_mask = -1 ; force is_mask re-check on first draw

   ; Initial pipeline and common state
   cmd_bind_pipeline(cb, 0, _pipeline)
   _last_bound_pipe = _pipeline

   ; Bind vertex buffer ONCE for this frame's slice — not again until draw_lines_raw
   store64_raw(_flush_off, _current_frame_vertex_offset, 0)
   store64_raw(_flush_buf, _vertex_buffer, 0)
   cmd_bind_vertex_buffers(cb, 0, 1, _flush_buf, _flush_off)

   ; Set initial dynamic state
   cmd_set_viewport(cb, 0, 1, _ptr_vp)
   cmd_set_scissor(cb, 0, 1, _ptr_sci)

   memcpy(_pc_buffer, _current_mvp, 64)
   mut ident = mat4_identity()
   mat4_to_buffer(ident, _current_model)
   memcpy(_pc_buffer + 64, _current_model, 64)
   store32(_pc_buffer, 0, 128)
   store32(_pc_buffer, _current_is_unlit, 132)
   _mvp_dirty = false
   _model_dirty = false
   _pc_dirty = true ;; Force push on first draw of frame
   _last_is_mask = 0
   _last_is_unlit = _current_is_unlit
   true
}

fn set_unlit(unlit){
   "Toggles lighting for subsequent draw calls."
   def val = unlit ? 1 : 0
   if(val != _current_is_unlit){
      _flush()
      _current_is_unlit = val
      _pc_dirty = true
   }
}

fn _sync_pc(){
   "Internal: Synchronizes push constants with the GPU if dirty."
   if(_current_is_unlit != _last_is_unlit){ _last_is_unlit = _current_is_unlit _pc_dirty = true }
   if(_mvp_dirty || _model_dirty){ _pc_dirty = true }
   if(!_pc_dirty){ return }
   def cb = get(_command_buffers, _current_frame)
   if(!cb || !_pipeline_layout){ return }

   if(_mvp_dirty){ memcpy(_pc_buffer, _current_mvp, 64) _mvp_dirty = false }
   if(_model_dirty){ memcpy(_pc_buffer + 64, _current_model, 64) _model_dirty = false }

   store32(_pc_buffer, _last_is_mask, 128)
   store32(_pc_buffer, _last_is_unlit, 132)

   ; Stage flags: 1=VERT, 16=FRAG
   cmd_push_constants(cb, _pipeline_layout, 17, 0, 144, _pc_buffer)
   _pc_dirty = false
}

fn _flush(){
   "Records a draw call for current pending triangle batch."
   if(_vertex_offset == _last_flush_offset){ return }
   def t0 = ticks()

   def count = (_vertex_offset - _last_flush_offset) / 36
   def first_vert = _last_flush_offset / 36

   def cb = get(_command_buffers, _current_frame)

      ; Select appropriate triangle pipeline based on unlit state
   mut base_pipe = _pipeline
   if(_current_is_unlit != 0 && _unlit_pipeline != 0){ base_pipe = _unlit_pipeline }

   mut target = _target_pipeline
   if(target == _pipeline){ target = base_pipe } ; if target is default, use our unlit-aware base

   if(_is_wireframe && _wire_pipeline != 0){
      if(target == _pipeline || target == _unlit_pipeline){ target = _wire_pipeline }
   }

   if(_last_bound_pipe != target){
       cmd_bind_pipeline(cb, 0, target)
       _last_bound_pipe = target
       _pc_dirty = true
   }

   ; Bind Texture / Descriptor Set only when changed
   mut tid = _current_texture_id
   if(tid < 0 || tid >= len(_textures)){ tid = _default_texture }

   def ds = texture_descriptor(tid)
   if(ds && (ds != _last_bound_ds || tid != _last_bound_tex_id)){
      store64_raw(_ptr_ds, ds, 0)
      cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 1, _ptr_ds, 0, 0)
      _last_bound_ds = ds
      _last_bound_tex_id = tid
       mut new_mask = 0
       if(texture_format(tid) == 9){ new_mask = 1 }
       if(new_mask != _last_is_mask){ _last_is_mask = new_mask _pc_dirty = true }
   }

   ; Push constants only when matrix, model, mask, or unlit changed
   _sync_pc()

   ; Depth state depends on unlit
   ; Simple way: just clear depth for every flush if unlit? No, too slow.
   ; Real way: we need a separate pipeline for 2D.
   ; For now, UI test calls clear_depth() which is fine.

   ; VBO is already bound in begin_frame — just draw using first_vert index
   if(count > 0){
      cmd_draw(cb, count, 1, first_vert, 0)
      _total_draw_calls += 1
      _frame_draw_calls += 1
   }
   _last_flush_offset = _vertex_offset

   def t1 = ticks()
   _prof_flush_total += float(t1 - t0)
   _prof_flush_count += 1
   _prof_flush_avg = _prof_flush_total / float(_prof_flush_count)
}

fn _check_flush(bytes){
   "Ensures enough space in the current frame buffer slice."
   if(_vertex_limit_hit){ return }
   if(_vertex_offset + bytes > _vertex_capacity){
      _flush()
      if(_vertex_offset + bytes > _vertex_capacity){
          if(_debug_gfx_enabled){ print("Vulkan: VERTEX BUFFER FULL for current frame!") }
          _vertex_limit_hit = true
      }
   }
}

fn _pack_color(r, g, b, a){
   "Packs RGBA floats [0,1] into a uint32 (R8G8B8A8 for UNORM attribute)."
   (int(r * 255.0) & 0xFF) | ((int(g * 255.0) & 0xFF) << 8) | ((int(b * 255.0) & 0xFF) << 16) | ((int(a * 255.0) & 0xFF) << 24)
}

fn _push_vertex(x, y, z, u, v, r, g, b, a){
   "Appends a single vertex (36 bytes) to the current batch."
   def off = _local_vertex_map + _vertex_offset
   ; Ensure we use raw floats to avoid object tagging artifacts in the buffer.
   store32_f32(off, float(x), 0)
   store32_f32(off, float(y), 4)
   store32_f32(off, float(z), 8)
   store32_f32(off, float(u), 12)
   store32_f32(off, float(v), 16)
   store32(off, _pack_color(r, g, b, a), 20)
   store32_f32(off, 0.0, 24) ;; NX
   store32_f32(off, 0.0, 28) ;; NY
   store32_f32(off, 1.0, 32) ;; NZ
   _vertex_offset += 36
}

fn end_frame(){
   "Finalizes rendering and presents the frame to the swapchain image."
   _end_frame_internal(true)
}

fn _end_frame_internal(present){
   "Finalizes command recording and triggers vertex upload."
   if(!_frame_open){ return false }
   _flush()

   ; SINGLE VERTEX UPLOAD: Copy entire frame's vertex data to the GPU.
   if(_vertex_offset > 0){
      def sz = (_vertex_offset < _vertex_capacity) ? _vertex_offset : _vertex_capacity
      memcpy(_vertex_map + _current_frame_vertex_offset, _local_vertex_map, sz)
   }
   def cb = get(_command_buffers, _current_frame)
   cmd_end_render_pass(cb)
   def ecb = end_command_buffer(cb)
   if(ecb != 0){
      return false
   }

   def sem_avail = get(_image_available_semaphores, _current_frame)
   def sem_finish = get(_render_finished_semaphores, _current_frame)

   store64_raw(_ptr_wait_sems, sem_avail, 0)
   store64_raw(_ptr_sig_sems, sem_finish, 0)
   store32(_ptr_stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0)

   memset(_ptr_sub, 0, 128)
   store32(_ptr_sub, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(_ptr_sub, 1, 16) ; waitSemaphoreCount
   store64_raw(_ptr_sub, _ptr_wait_sems, 24)
   store64_raw(_ptr_sub, _ptr_stages, 32)
   store32(_ptr_sub, 1, 40) ; commandBufferCount
   mut cb_ptr = _ptr_sub + 80 ;; Reuse end of buffer for cb array
   store64_raw(cb_ptr, cb, 0)
   store64_raw(_ptr_sub, cb_ptr, 48)
   store32(_ptr_sub, 1, 56) ; signalSemaphoreCount
   store64_raw(_ptr_sub, _ptr_sig_sems, 64)

   def fence = get(_in_flight_fences, _current_frame)
   def sub_res = queue_submit(_graphics_queue, 1, _ptr_sub, fence)
   if(sub_res != 0){
      return false
   }

   if(present){
      def sc = _swapchain
      def img_idx = _image_index

      mut scs = _ptr_ri ;; Reuse ri buffer for swapchain array
      store64_raw(scs, sc, 0)
      mut idxs = scs + 8
      store32(idxs, img_idx, 0)

      mut pi = _ptr_ri + 32 ;; Reuse ri buffer for present info
      memset(pi, 0, 64)
      store32(pi, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0)
      store32(pi, 1, 16) ; waitSemaphoreCount
      store64_raw(pi, _ptr_sig_sems, 24)
      store32(pi, 1, 32) ; swapchainCount
      store64_raw(pi, scs, 40)
      store64_raw(pi, idxs, 48)

      def pr = queue_present_khr(_present_queue, pi)
      if(pr == 0xC460C464 || pr == -1000001004 || pr == 1000001003){ ; OUT_OF_DATE or SUBOPTIMAL
         _frame_open = false
         _recreate_swapchain()
         return false
      }
   }

   _frame_open = false
   _current_frame = (_current_frame + 1) % MAX_FRAMES_IN_FLIGHT
   true
}

fn clear(r, g, b, a){
   "Commands the GPU to clear the current color attachment."
   if(!_frame_open){ return 0 }
   if(!_clear_ca){ _clear_ca = sys_malloc(24) _clear_rect = sys_malloc(24) }
   def cb = get(_command_buffers, _current_frame)
   store32(_clear_ca, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(_clear_ca, 0, 4)
   store32_f32(_clear_ca, r, 8)
   store32_f32(_clear_ca, g, 12)
   store32_f32(_clear_ca, b, 16)
   store32_f32(_clear_ca, a, 20)
   store32(_clear_rect, 0, 0) store32(_clear_rect, 0, 4)
   store32(_clear_rect, _swapchain_extent_w, 8) store32(_clear_rect, _swapchain_extent_h, 12)
   store32(_clear_rect, 0, 16)
   store32(_clear_rect, 1, 20)
   cmd_clear_attachments(cb, 1, _clear_ca, 1, _clear_rect)
}

fn clear_depth(){
   "Clears the depth buffer, ensuring subsequent depth passes render correctly over past layers."
   if(!_frame_open){ return 0 }
   _flush() ; Flush pending vertex geometry to ensure it writes before clear
   if(!_clear_ca){ _clear_ca = sys_malloc(24) _clear_rect = sys_malloc(24) }
   def cb = get(_command_buffers, _current_frame)
   store32(_clear_ca, 2, 0) ; VK_IMAGE_ASPECT_DEPTH_BIT
   store32(_clear_ca, 0, 4) ; colorAttachment ignored
   store32_f32(_clear_ca, 1.0, 8) ; depth
   store32(_clear_ca, 0, 12) ; stencil
   store32(_clear_rect, 0, 0) store32(_clear_rect, 0, 4)
   store32(_clear_rect, _swapchain_extent_w, 8) store32(_clear_rect, _swapchain_extent_h, 12)
   store32(_clear_rect, 0, 16)
   store32(_clear_rect, 1, 20)
   cmd_clear_attachments(cb, 1, _clear_ca, 1, _clear_rect)
}

fn draw_rect(x, y, w, h, r, g, b, a){
   "Batches a colored rectangle (6-vertex CW triangle list) — optimized path."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   _check_flush(216) ;; 6 * 36 bytes
   def c = _pack_color(r, g, b, a)
   _push_rect_packed(x, y, w, h, c)
}

fn draw_rectangle_fast(x, y, w, h, color_packed){
   "Submits a rectangle using a pre-packed color value."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   _check_flush(216)
   _push_rect_packed(x, y, w, h, color_packed)
}

fn _push_rect_packed(x, y, w, h, c){
   "Unrolled 6-vertex quad submission for minimal interpreter overhead."
   def off = _local_vertex_map + _vertex_offset
   __vkr_push_rect(off, x, y, w, h, c)
   _vertex_offset += 216
}

fn _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, c){
   "Internal: batches a textured quad using packed color `c`."
   if(!_frame_open){ return 0 }
   bind_texture(tex_id)
   _check_flush(216)
   __vkr_push_rect_tex(_local_vertex_map + _vertex_offset, x, y, w, h, u1, v1, u2, v2, c)
   _vertex_offset += 216
}

fn draw_rect_tex(x, y, w, h, tex_id, r, g, b, a){
   "Batches a textured rectangle (6-vertex triangle list) — optimized."
   _draw_textured_rect_packed(x, y, w, h, tex_id, 0.0, 0.0, 1.0, 1.0, _pack_color(r, g, b, a))
}

fn draw_glyph(x, y, w, h, u1, v1, u2, v2, tex_id, r, g, b, a){
   "Submits a glyph quad for text rendering."
   _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, _pack_color(r, g, b, a))
}

fn draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a){
   "Batches a textured rectangle with explicit UV coordinates."
   _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, _pack_color(r, g, b, a))
}

fn _push_rect_tex_packed(x, y, w, h, u1, v1, u2, v2, c){
   "Fully unrolled textured 6-vertex quad submission."
   def off = _local_vertex_map + _vertex_offset
   __vkr_push_rect_tex(off, x, y, w, h, u1, v1, u2, v2, c)
   _vertex_offset += 216
}

fn draw_vertices(ptr, count, tex_id){
   "Bulk-uploads raw vertex data (36-byte stride) to the local mapping."
   if(!_frame_open || count <= 0 || !ptr){ return 0 }
   bind_texture(tex_id)
   def bytes = count * _VKR_VERT_STRIDE
   _check_flush(bytes)
   memcpy(_local_vertex_map + _vertex_offset, ptr, bytes)
   _vertex_offset += bytes
   true
}

fn draw_lines_raw(ptr, line_count, _line_width){
   "Draws lines using pre-baked raw vertex buffer. thickness controls GPU line thickness."
   if(!_frame_open || line_count <= 0 || !ptr || !_line_pipeline){ return 0 }
   _flush() ; flush pending triangles first

   def cb = get(_command_buffers, _current_frame)
   _check_flush(line_count * 2 * _VKR_VERT_STRIDE)

   ; Switch to line pipeline
   if(_last_bound_pipe != _line_pipeline){
      cmd_bind_pipeline(cb, 0, _line_pipeline)
      _last_bound_pipe = _line_pipeline
      _pc_dirty = true
   }

   ; Ensure descriptor set is bound (crucial for RADV)
   mut tid = _current_texture_id
   if(tid < 0){ tid = _default_texture }
   def ds = texture_descriptor(tid)
   if(ds && (ds != _last_bound_ds || tid != _last_bound_tex_id)){
      store64_raw(_ptr_ds, ds, 0)
      cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 1, _ptr_ds, 0, 0)
      _last_bound_ds = ds _last_bound_tex_id = tid
   }

   ; Push constants if needed
   if(_mvp_dirty){ memcpy(_pc_buffer, _current_mvp, 64) _mvp_dirty = false _pc_dirty = true }
   if(_model_dirty){ memcpy(_pc_buffer + 64, _current_model, 64) _model_dirty = false _pc_dirty = true }
   if(_pc_dirty){
      store32(_pc_buffer, 0, 128) ; lines are never masks
      store32(_pc_buffer, 1, 132) ; lines are always unlit
      cmd_push_constants(cb, _pipeline_layout, 1 | 16, 0, 144, _pc_buffer)
      _pc_dirty = false
   }

   ; NOTE: vkCmdSetLineWidth requires float in xmm0 (x86-64 ABI) which Nytrix FFI
   ; cannot pass correctly via ffi_call. Line width stays at the static pipeline value (1.0).
   ; The parameter is reserved for future typed-extern support.

   ; Copy vertices
   def bytes = line_count * 2 * _VKR_VERT_STRIDE
   def first_vert = _vertex_offset / _VKR_VERT_STRIDE
   memcpy(_local_vertex_map + _vertex_offset, ptr, bytes)

   ; Draw using firstVertex within the existing slice VBO binding (no rebind needed)
   cmd_draw(cb, line_count * 2, 1, first_vert, 0)

   _vertex_offset += bytes
   _last_flush_offset = _vertex_offset

   ; Rebind VBO back to slice start so subsequent triangle _flush calls work
   store64_raw(_flush_off, _current_frame_vertex_offset, 0)
   cmd_bind_vertex_buffers(cb, 0, 1, _flush_buf, _flush_off)
   true
}

fn _draw_triangle_2d(x1, y1, x2, y2, x3, y3, r, g, b, a){
   "Batches a colored 2D triangle."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   _check_flush(108)
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base, 0, x1, y1, 0.0, 0.0, 0.0, c)
   _vkr_store_vertex(base, 1, x2, y2, 0.0, 0.0, 0.0, c)
   _vkr_store_vertex(base, 2, x3, y3, 0.0, 0.0, 0.0, c)
   _vertex_offset += 108
}

fn draw_line(x1, y1, x2, y2, thickness, r, g, b, a){
   "Batches a thick line using a 6-vertex triangle quad."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   _check_flush(216)
   def c = _pack_color(r, g, b, a)
   __vkr_push_line(_local_vertex_map + _vertex_offset, x1, y1, x2, y2, thickness, c)
   _vertex_offset += 216
}

fn draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a){
   "Batches a single colored 3D triangle (zero-alloc)."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   _check_flush(108)
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base, 0, x1, y1, z1, 0.0, 0.0, c)
   _vkr_store_vertex(base, 1, x2, y2, z2, 0.0, 0.0, c)
   _vkr_store_vertex(base, 2, x3, y3, z3, 0.0, 0.0, c)
   _vertex_offset += 108
}

fn draw_quad_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a){
   "Batches a single colored 3D quad (zero-alloc)."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   _check_flush(216)
   def c = _pack_color(r, g, b, a)
   def base_idx = _vertex_offset / 36
   _vkr_store_vertex(_local_vertex_map, base_idx + 0, x1, y1, z1, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 1, x2, y2, z2, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 2, x3, y3, z3, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 3, x1, y1, z1, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 4, x3, y3, z3, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 5, x4, y4, z4, 0.0, 0.0, c)
   _vertex_offset += 216
}

fn draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, r, g, b, a){
   "Batches a 3D line as a quad (parallel to Y if needed, or billboarded)."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   _check_flush(216)
   def dx = float(x2) - float(x1) def dy = float(y2) - float(y1) def dz = float(z2) - float(z1)
   def l = sqrt(dx*dx + dy*dy + dz*dz)
   if(l == 0.0){ return }
   mut nx = -dz / l * (float(thickness) * 0.5)
   mut ny = 0.0
   mut nz =  dx / l * (float(thickness) * 0.5)
   if(abs(dx) < 0.001 && abs(dz) < 0.001){ nx = float(thickness)*0.5 nz = 0.0 }
   def c = _pack_color(r, g, b, a)
   def f1x = float(x1) def f1y = float(y1) def f1z = float(z1)
   def f2x = float(x2) def f2y = float(y2) def f2z = float(z2)
   def base_idx = _vertex_offset / 36
   _vkr_store_vertex(_local_vertex_map, base_idx + 0, f1x+nx, f1y+ny, f1z+nz, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 1, f1x-nx, f1y-ny, f1z-nz, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 2, f2x-nx, f2y-ny, f2z-nz, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 3, f1x+nx, f1y+ny, f1z+nz, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 4, f2x-nx, f2y-ny, f2z-nz, 0.0, 0.0, c)
   _vkr_store_vertex(_local_vertex_map, base_idx + 5, f2x+nx, f2y+ny, f2z+nz, 0.0, 0.0, c)
   _vertex_offset += 216
}

fn draw_grid_3d(size, step){
   "Draws an infinite-style 3D grid on the XZ plane."
   def s = float(size)
   mut gx = -s
   while(gx <= s){
      draw_line_3d(gx, 0, -s, gx, 0, s, 0.03, 0.2, 0.2, 0.4, 0.5)
      draw_line_3d(-s, 0, gx, s, 0, gx, 0.03, 0.2, 0.2, 0.4, 0.5)
      gx += float(step)
   }
}

fn draw_axes_3d(size){
   "Draws RGB axis lines with arrowheads."
   def s = float(size)
   draw_line_3d(0,0,0, s,0,0, 0.15, 1,0,0,1)
   draw_line_3d(0,0,0, 0,s,0, 0.15, 0,1,0,1)
   draw_line_3d(0,0,0, 0,0,s, 0.15, 0,0,1,1)

   ;; Basic Arrowheads (Triangles)
   def ts = 0.4
   ;; X
   draw_triangle_3d(s, 0, 0, s-ts, ts, 0, s-ts,-ts, 0, 1, 0, 0, 1)
   ;; Y
   draw_triangle_3d(0, s, 0, ts, s-ts, 0,-ts, s-ts, 0, 0, 1, 0, 1)
   ;; Z
   draw_triangle_3d(0, 0, s, ts, 0, s-ts,-ts, 0, s-ts, 0, 0, 1, 1)
}

fn draw_cube_3d(x, y, z, size, r, g, b, a, tex_id){
   "Batches a colored 3D cube. frontFace=CW, cullMode=BACK (Vulkan Y-down convention)."
   if(!_frame_open){ return }
   bind_texture(tex_id)
   _check_flush(36 * 36)
   def s = float(size) * 0.5
   def fx = float(x) def fy = float(y) def fz = float(z)
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset

   ;; Vulkan Y-down: screen Y increases downward.
   ;; frontFace=CW means a face is front if its verts appear CW on screen.
   ;; For each face, we name verts as seen on screen from outside:
   ;;   v0=top-left, v1=top-right, v2=bot-right, v3=bot-left  (screen coords, Y-down)
   ;; Two CW triangles: (v0,v1,v2) and (v0,v2,v3)

   ;; Front (+Z): looking at face from +Z. Screen: right=+X, down=+Y.
   ;;   TL=(-s,-s,+s) TR=(+s,-s,+s) BR=(+s,+s,+s) BL=(-s,+s,+s)
   _vkr_store_vertex(base,  0, fx-s, fy-s, fz+s, 0,0, c,  0, 0, 1)
   _vkr_store_vertex(base,  1, fx+s, fy-s, fz+s, 1,0, c,  0, 0, 1)
   _vkr_store_vertex(base,  2, fx+s, fy+s, fz+s, 1,1, c,  0, 0, 1)
   _vkr_store_vertex(base,  3, fx-s, fy-s, fz+s, 0,0, c,  0, 0, 1)
   _vkr_store_vertex(base,  4, fx+s, fy+s, fz+s, 1,1, c,  0, 0, 1)
   _vkr_store_vertex(base,  5, fx-s, fy+s, fz+s, 0,1, c,  0, 0, 1)

   ;; Back (-Z): looking at face from -Z. Screen: right=-X, down=+Y.
   ;;   TL=(+s,-s,-s) TR=(-s,-s,-s) BR=(-s,+s,-s) BL=(+s,+s,-s)
   _vkr_store_vertex(base,  6, fx+s, fy-s, fz-s, 0,0, c,  0, 0,-1)
   _vkr_store_vertex(base,  7, fx-s, fy-s, fz-s, 1,0, c,  0, 0,-1)
   _vkr_store_vertex(base,  8, fx-s, fy+s, fz-s, 1,1, c,  0, 0,-1)
   _vkr_store_vertex(base,  9, fx+s, fy-s, fz-s, 0,0, c,  0, 0,-1)
   _vkr_store_vertex(base, 10, fx-s, fy+s, fz-s, 1,1, c,  0, 0,-1)
   _vkr_store_vertex(base, 11, fx+s, fy+s, fz-s, 0,1, c,  0, 0,-1)

   ;; Right (+X): looking from +X. Screen: right=-Z, down=+Y.
   ;;   TL=(+s,-s,-s) TR=(+s,-s,+s) BR=(+s,+s,+s) BL=(+s,+s,-s)
   _vkr_store_vertex(base, 12, fx+s, fy-s, fz-s, 0,0, c,  1, 0, 0)
   _vkr_store_vertex(base, 13, fx+s, fy-s, fz+s, 1,0, c,  1, 0, 0)
   _vkr_store_vertex(base, 14, fx+s, fy+s, fz+s, 1,1, c,  1, 0, 0)
   _vkr_store_vertex(base, 15, fx+s, fy-s, fz-s, 0,0, c,  1, 0, 0)
   _vkr_store_vertex(base, 16, fx+s, fy+s, fz+s, 1,1, c,  1, 0, 0)
   _vkr_store_vertex(base, 17, fx+s, fy+s, fz-s, 0,1, c,  1, 0, 0)

   ;; Left (-X): looking from -X. Screen: right=+Z, down=+Y.
   ;;   TL=(-s,-s,+s) TR=(-s,-s,-s) BR=(-s,+s,-s) BL=(-s,+s,+s)
   _vkr_store_vertex(base, 18, fx-s, fy-s, fz+s, 0,0, c, -1, 0, 0)
   _vkr_store_vertex(base, 19, fx-s, fy-s, fz-s, 1,0, c, -1, 0, 0)
   _vkr_store_vertex(base, 20, fx-s, fy+s, fz-s, 1,1, c, -1, 0, 0)
   _vkr_store_vertex(base, 21, fx-s, fy-s, fz+s, 0,0, c, -1, 0, 0)
   _vkr_store_vertex(base, 22, fx-s, fy+s, fz-s, 1,1, c, -1, 0, 0)
   _vkr_store_vertex(base, 23, fx-s, fy+s, fz+s, 0,1, c, -1, 0, 0)

   ;; Top (-Y, world up): looking from -Y (above). Screen: right=+X, down=+Z.
   ;;   TL=(-s,-s,-s) TR=(+s,-s,-s) BR=(+s,-s,+s) BL=(-s,-s,+s)
   _vkr_store_vertex(base, 24, fx-s, fy-s, fz-s, 0,0, c,  0,-1, 0)
   _vkr_store_vertex(base, 25, fx+s, fy-s, fz-s, 1,0, c,  0,-1, 0)
   _vkr_store_vertex(base, 26, fx+s, fy-s, fz+s, 1,1, c,  0,-1, 0)
   _vkr_store_vertex(base, 27, fx-s, fy-s, fz-s, 0,0, c,  0,-1, 0)
   _vkr_store_vertex(base, 28, fx+s, fy-s, fz+s, 1,1, c,  0,-1, 0)
   _vkr_store_vertex(base, 29, fx-s, fy-s, fz+s, 0,1, c,  0,-1, 0)

   ;; Bottom (+Y, world down): looking from +Y (below). Screen: right=+X, down=-Z.
   ;;   TL=(-s,+s,+s) TR=(+s,+s,+s) BR=(+s,+s,-s) BL=(-s,+s,-s)
   _vkr_store_vertex(base, 30, fx-s, fy+s, fz+s, 0,0, c,  0, 1, 0)
   _vkr_store_vertex(base, 31, fx+s, fy+s, fz+s, 1,0, c,  0, 1, 0)
   _vkr_store_vertex(base, 32, fx+s, fy+s, fz-s, 1,1, c,  0, 1, 0)
   _vkr_store_vertex(base, 33, fx-s, fy+s, fz+s, 0,0, c,  0, 1, 0)
   _vkr_store_vertex(base, 34, fx+s, fy+s, fz-s, 1,1, c,  0, 1, 0)
   _vkr_store_vertex(base, 35, fx-s, fy+s, fz-s, 0,1, c,  0, 1, 0)

   _vertex_offset += 36 * 36
}

fn draw_line_strip_2d(x, y, w, h, history, scale, r, g, b, a){
   "Batches a UI line strip from a history list."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   def count = len(history)
   if(count < 2){ return }
   _check_flush((count-1) * 144)
   def c = _pack_color(r, g, b, a)
   def dcount = float(count - 1)
   def step = float(w) / dcount
   def fh = float(h) def fx = float(x) def fy = float(y)
   def fs = float(scale)
   mut off = _local_vertex_map + _vertex_offset
   mut i = 0
   while(i < count - 1){
      mut v1 = float(get(history, i, 0)) * fs
      mut v2 = float(get(history, i + 1, 0)) * fs
      if(v1 > 1.0){ v1 = 1.0 } if(v2 > 1.0){ v2 = 1.0 }
      def px1 = fx + float(i) * step
      def py1 = fy + fh * (1.0 - v1)
      def px2 = fx + float(i+1) * step
      def py2 = fy + fh * (1.0 - v2)
      def th = 1.0
      store32_f32(off, px1, 0) store32_f32(off, py1-th, 4) store32_f32(off, 0, 8) store32_f32(off, 0, 12) store32_f32(off, 0, 16) store32(off, c, 20) off += 24
      store32_f32(off, px1, 0) store32_f32(off, py1+th, 4) store32_f32(off, 0, 8) store32_f32(off, 0, 12) store32_f32(off, 0, 16) store32(off, c, 20) off += 24
      store32_f32(off, px2, 0) store32_f32(off, py2+th, 4) store32_f32(off, 0, 8) store32_f32(off, 0, 12) store32_f32(off, 0, 16) store32(off, c, 20) off += 24
      store32_f32(off, px1, 0) store32_f32(off, py1-th, 4) store32_f32(off, 0, 8) store32_f32(off, 0, 12) store32_f32(off, 0, 16) store32(off, c, 20) off += 24
      store32_f32(off, px2, 0) store32_f32(off, py2+th, 4) store32_f32(off, 0, 8) store32_f32(off, 0, 12) store32_f32(off, 0, 16) store32(off, c, 20) off += 24
      store32_f32(off, px2, 0) store32_f32(off, py2-th, 4) store32_f32(off, 0, 8) store32_f32(off, 0, 12) store32_f32(off, 0, 16) store32(off, c, 20) off += 24
      i += 1
   }
   _vertex_offset = off - _local_vertex_map
}

mut _blit_tex_id = -1

fn blit_buffer(pixels, w, h){
   "Blits a raw RGBA8 pixel buffer to the full window."
   if(!_frame_open){ return }
   if(_blit_tex_id == -1 || texture_size(_blit_tex_id)[0] != w || texture_size(_blit_tex_id)[1] != h){
      if(_blit_tex_id != -1){ destroy_texture(_blit_tex_id) }
      _blit_tex_id = create_texture(w, h, pixels)
   } else {
      update_texture_rect(_blit_tex_id, 0, 0, w, h, pixels)
   }

   ; Draw full-screen quad unlit
   def last_unlit = _current_is_unlit
   set_unlit(true)
   def ws_w = float(_swapchain_extent_w)
   def ws_h = float(_swapchain_extent_h)

   ; Save MVP and set to identity for screen-space draw
   mut old_mvp = mat4_identity()
   mat4_from_buffer(old_mvp, _current_mvp)
   set_ortho(0.0, ws_w, 0.0, ws_h, -1.0, 1.0)

   draw_rect_tex(0.0, 0.0, ws_w, ws_h, _blit_tex_id, 1.0, 1.0, 1.0, 1.0)
   _flush()

   ; Restore state
   set_mvp(old_mvp)
   set_unlit(last_unlit != 0)
}

fn set_clear_color(r, g, b, a=1.0){
   "Sets the clear color for the next begin_frame."
   _clear_r = float(r) _clear_g = float(g) _clear_b = float(b) _clear_a = float(a)
}

fn shutdown(){
   "Shuts down the Vulkan renderer and releases all associated resources."
   if(!_device){
      if(_surface){ destroy_surface_khr(_instance, _surface, 0) }
      if(_instance){ destroy_instance(_instance, 0) }
      return
   }
   device_wait_idle(_device)
   if(_vertex_buffer){ destroy_buffer(_device, _vertex_buffer, 0) }
   if(_depth_image){ destroy_image(_device, _depth_image, 0) }
   if(_depth_view){ destroy_image_view(_device, _depth_view, 0) }
   if(_vertex_memory){ free_memory(_device, _vertex_memory, 0) }
   if(_staging_buffer){ destroy_buffer(_device, _staging_buffer, 0) }
   if(_staging_memory){ free_memory(_device, _staging_memory, 0) }
   if(_default_sampler){ destroy_sampler(_device, _default_sampler, 0) }
   if(_descriptor_pool){ destroy_descriptor_pool(_device, _descriptor_pool, 0) }

   mut i = 0
   while(i < len(_textures)){
      def tex = get(_textures, i)
      def view = dict_get(tex, "view", 0)
      def img = dict_get(tex, "image", 0)
      def mem = dict_get(tex, "memory", 0)
      if(view){ destroy_image_view(_device, view, 0) }
      if(img){ destroy_image(_device, img, 0) }
      if(mem){ free_memory(_device, mem, 0) }
      i += 1
   }
   _textures = []

   _destroy_swapchain_objects()
   if(_device){ destroy_device(_device, 0) }
   if(_surface){ destroy_surface_khr(_instance, _surface, 0) }
   if(_instance){ destroy_instance(_instance, 0) }
}

fn set_wireframe(enabled){
   "Enables or disables wireframe rendering globally."
   _is_wireframe = !!enabled
   if(_vertex_offset != _last_flush_offset){ _flush() }
}
