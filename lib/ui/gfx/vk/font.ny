;; Keywords: ui gfx vulkan renderer font

module std.ui.gfx.vk.font (
   draw_text_batch, __vkr_draw_text, _vkr_glyph_get_off, _vkr_glyph_present
)

use std.core *
use std.core.mem *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.utils *
use std.ui.gfx.vk.renderer (_check_flush, set_mask)
use std.ui.gfx.vk.texture (bind_texture)

@jit
fn draw_text_batch(font_id, lines, x, y, spacing, color_u32){
   "Draws multiple lines of text in a single Nytrix call to minimize interpreter overhead."
   set_mask(1)
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

@jit
fn __vkr_draw_text_fast(text, x, y, color_u32, glyphs_ptr, line_h_f){
   if(!glyphs_ptr || !is_str(text)){ return }
   def n = str_len(text)
   if(!_check_flush(n * (_VKR_VERT_STRIDE * 6))){ return }
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
      def bw = load32_f32(g_off, _VKR_G_BW) * gs
      def bh = bh_raw * gs

      if(bw > 0.0 && bh > 0.0){
         def gx = pen_x + load32_f32(g_off, _VKR_G_XOFF) * gs
         mut gy = y - load32_f32(g_off, _VKR_G_YOFF) * gs
         __vkr_push_rect_tex_fast(_local_vertex_map + _vertex_offset, gx, gy, bw, bh, load32_f32(g_off, _VKR_G_U1), load32_f32(g_off, _VKR_G_V1), load32_f32(g_off, _VKR_G_U2), load32_f32(g_off, _VKR_G_V2), is_color ? 0xFFFFFFFF : color_u32, _current_tex_index)
         _vertex_offset += _VKR_VERT_STRIDE * 6
      }
      pen_x += load32_f32(g_off, _VKR_G_ADV) * gs
   }
}

@readonly @jit
fn _vkr_glyph_get_off(glyphs_ptr, cp){
   "Internal: returns the address of glyph metadata for `cp` using a paged table."
   if(cp < 0 || cp >= 1114112){ return 0 }
   def page_idx = cp >> 8
   def page_ptr = load64(glyphs_ptr, page_idx * 8)
   if(!page_ptr){ return 0 }
   page_ptr + (cp & 255) * _VKR_GLYPH_STRIDE
}

@readonly @jit
fn _vkr_glyph_present(glyphs_ptr, cp){
   "Internal: returns whether codepoint `cp` exists in the paged glyph table."
   def off = _vkr_glyph_get_off(glyphs_ptr, cp)
   if(!off){ return false }
   load32(off, _VKR_G_PRESENT) != 0
}

@jit
fn __vkr_draw_text(_unused_vbo, text, x, y, color, glyphs_ptr, ascent, line_h, out_info){
   "Builds packed glyph vertices for `text` directly into the current VBO, handling atlas changes."
   set_mask(1)
   if(!glyphs_ptr || !is_str(text)){ return }
   def n = str_len(text)
   if(!_check_flush(n * (_VKR_VERT_STRIDE * 6))){ return }
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

      def bw = load32_f32(g_off, _VKR_G_BW) * gs
      def bh = bh_raw * gs
      def adv = adv_raw * gs

      if(bw > 0.0 && bh > 0.0){
          def gx = pen_x + load32_f32(g_off, _VKR_G_XOFF) * gs
          mut gy = pen_y - load32_f32(g_off, _VKR_G_YOFF) * gs
         def u1 = load32_f32(g_off, _VKR_G_U1)
         def v1 = load32_f32(g_off, _VKR_G_V1)
         def u2 = load32_f32(g_off, _VKR_G_U2)
         def v2 = load32_f32(g_off, _VKR_G_V2)
         def c = is_color ? 0xFFFFFFFF : c_text

         ;; 3. Fully Inlined Vertex Generation (6 vertices) - Template Optimized
         mut bv = _local_vertex_map + _vertex_offset
         def bytes = _VKR_VERT_STRIDE * 6
         if(!_vk_guard_span(bv, bytes, _local_vertex_map, _vertex_capacity, "vertex_map")){ return 0 }
         memcpy(bv, _quad_template, bytes)

         def gx2 = gx + bw
         def gy2 = gy + bh

         store32_f32(bv, gx, _VKR_OFF_X)   store32_f32(bv, gy, _VKR_OFF_Y)   store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, _current_tex_index, _VKR_OFF_TEX)
         bv += _VKR_VERT_STRIDE
         store32_f32(bv, gx, _VKR_OFF_X)   store32_f32(bv, gy2, _VKR_OFF_Y)  store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, _current_tex_index, _VKR_OFF_TEX)
         bv += _VKR_VERT_STRIDE
         store32_f32(bv, gx2, _VKR_OFF_X)  store32_f32(bv, gy2, _VKR_OFF_Y)  store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, _current_tex_index, _VKR_OFF_TEX)
         bv += _VKR_VERT_STRIDE
         store32_f32(bv, gx2, _VKR_OFF_X)  store32_f32(bv, gy2, _VKR_OFF_Y)  store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v2, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, _current_tex_index, _VKR_OFF_TEX)
         bv += _VKR_VERT_STRIDE
         store32_f32(bv, gx2, _VKR_OFF_X)  store32_f32(bv, gy, _VKR_OFF_Y)   store32_f32(bv, u2, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, _current_tex_index, _VKR_OFF_TEX)
         bv += _VKR_VERT_STRIDE
         store32_f32(bv, gx, _VKR_OFF_X)   store32_f32(bv, gy, _VKR_OFF_Y)   store32_f32(bv, u1, _VKR_OFF_U) store32_f32(bv, v1, _VKR_OFF_V) store32(bv, c, _VKR_OFF_C) store32(bv, _current_tex_index, _VKR_OFF_TEX)

         _vertex_offset += _VKR_VERT_STRIDE * 6
         total_verts += 6
      }
      pen_x = pen_x + adv
   }

   if(out_info){
      store64(out_info, total_verts, 0)
      store64(out_info, _current_texture_id, 8)
   }
}
