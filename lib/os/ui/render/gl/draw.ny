;; Keywords: render opengl gl draw 2d 3d primitives
;; References: std.os.ui.render.gl.state std.os.ui.render.gl.texture std.os.ui.render.matrix
module std.os.ui.render.gl.draw(_store_line_quad, _store_rect, alpha8, ax, base_b, base_g, base_r, batch, bx, c, co, cx0, done, draw_glyph_bitmap_scaled, draw_line, draw_line_3d, draw_line_fast, draw_lines_2d_fast_ptr, draw_lines_raw, draw_points_raw, draw_quad_3d, draw_rect, draw_rect_fast, draw_rect_outline_fast, draw_rect_tex, draw_rect_tex_uv, draw_rect_tex_uv_rot, draw_rects_fast_ptr, draw_triangle_3d, draw_vertices, draw_vertices_indexed_raw, drew, dx, dxs, hs, hw, i, ibase, idx_ty, ix, j, last_x, len, max_batch, nl, nx, p, p2, px, px_off, pxw, rad, rec, rr, s, samples, si, steps, sum_a, sum_b, sum_g, sum_r, sx, sx0, sx1, sy, sy0, sy1, t, verts, x, x0, x1, xx, y, yy)
use std.core
use std.math
use std.os.ui.render.shared as render_shared
use std.os.ui.render.gl.constants as gl_constants
use std.os.ui.render.gl.state as gl_state
use std.os.ui.render.gl.texture as gl_tex
use std.os.ui.render.matrix

fn draw_vertices(any p, int count, int tex_id=-1, bool use_material=false) bool {
   "Draws packed 64-byte vertices through optimized OpenGL calls."
   if !p || count <= 0 { return false }
   if _soft_enabled() {
      _soft_draw_vertices(p, count, tex_id, use_material)
      _record_draw(count, false, false)
      return true
   }
   _enable_draw_state(tex_id, use_material)
   if _upload_dynamic_vertices(p, count) {
      _setup_arrays_vbo(0, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit)
      _ny_glDrawArrays(GL_TRIANGLES, 0, count)
      ;; Skip defensive unbind: _bind_buffer in state.ny caches the binding,
      ;; so the next draw's _upload_dynamic_vertices will see the same buffer
      ;; already bound and short-circuit. This eliminates 1 glBindBuffer per
      ;; draw call on the hot terminal text path.
   } else {
      if !_draw_immediate_vertices(p, count, GL_TRIANGLES, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit) { return false }
   }
   _record_draw(count, false, false)
}

fn draw_lines_raw(any p, int line_count, f64 width=1.0, bool use_material=false) bool {
   "Draws packed 64-byte line vertices through OpenGL client arrays."
   if !p || line_count <= 0 { return false }
   if _soft_enabled() {
      _soft_draw_lines(p, line_count, width, use_material)
      _record_draw(line_count * 2, false, false)
      return true
   }
   _enable_draw_state(-1, use_material)
   _call1f("glLineWidth", max(1.0, width))
   def verts = line_count * 2
   if !_draw_immediate_vertices(p, verts, GL_LINES, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit) { return false }
   _record_draw(verts, false, false)
}

fn draw_points_raw(any p, int point_count, int tex_id=-1, bool use_material=false) bool {
   "Draws packed 64-byte point vertices through OpenGL client arrays."
   if !p || point_count <= 0 { return false }
   if _soft_enabled() {
      _soft_draw_points(p, point_count, use_material)
      _record_draw(point_count, false, false)
      return true
   }
   _enable_draw_state(tex_id, use_material)
   _call1f("glPointSize", 1.0)
   if !_draw_immediate_vertices(p, point_count, GL_POINTS, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit) { return false }
   _record_draw(point_count, false, false)
}

fn draw_vertices_indexed_raw(
   any p,
   int count,
   any idx_buf,
   any idx_offset,
   int idx_count,
   int index_type=0,
   int tex_id=-1,
   bool is_lines=false,
   f64 width=1.0,
   any _pipe_override=0,
   bool is_points=false,
   bool use_material=false
) bool {
   "Draws packed vertices with an OpenGL index buffer."
   _pipe_override
   if !p || idx_count <= 0 { return false }
   if _soft_enabled() {
      def ibase = idx_buf ? idx_buf + int(idx_offset) : idx_offset
      if !ibase { return false }
      _soft_draw_indexed(p, count, ibase, idx_count, index_type, tex_id, is_lines, width, is_points, use_material)
      _record_draw(idx_count, false, true)
      return true
   }
   _enable_draw_state(tex_id, use_material)
   _call1f("glLineWidth", max(1.0, width))
   if !_upload_dynamic_vertices(p, count) { return false }
   if idx_buf { _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, idx_buf) } else { _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0) }
   _setup_arrays_vbo(0, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit)
   def idx_ty = index_type == 1 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT
   if idx_buf { _ny_glDrawElementsOffset(_draw_mode(is_lines, is_points), idx_count, idx_ty, int(idx_offset)) }
   else { _ny_glDrawElements(_draw_mode(is_lines, is_points), idx_count, idx_ty, idx_offset) }
   ;; Skip defensive unbinds of GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER:
   ;; _bind_buffer in state.ny now caches bindings, so leaving the dynamic VBO
   ;; bound lets the next draw's _upload_dynamic_vertices short-circuit.
   _record_draw(idx_count, false, true)
}

fn _store_rect(ptr p, f64 x, f64 y, f64 w, f64 h, int c, f64 u1=0.0, f64 v1=0.0, f64 u2=1.0, f64 v2=1.0) bool {
   def x2, y2 = x + w, y + h
   _store_vertex(p, 0, x,  y,  0.0, u1, v1, c)
   _store_vertex(p, 1, x,  y2, 0.0, u1, v2, c)
   _store_vertex(p, 2, x2, y2, 0.0, u2, v2, c)
   _store_vertex(p, 3, x2, y2, 0.0, u2, v2, c)
   _store_vertex(p, 4, x2, y,  0.0, u2, v1, c)
   _store_vertex(p, 5, x,  y,  0.0, u1, v1, c)
   true
}

fn _store_line_quad(ptr p, f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, int c) bool {
   def dx, dy = x2 - x1, y2 - y1
   def len = sqrt(dx * dx + dy * dy)
   if len <= 0.000001 {
      def s = max(1.0, thickness)
      return _store_rect(p, x1, y1, s, s, c)
   }
   def px, py = -dy / len * thickness * 0.5, dx / len * thickness * 0.5
   _store_vertex(p, 0, x1 + px, y1 + py, 0.0, 0.0, 0.0, c)
   _store_vertex(p, 1, x1 - px, y1 - py, 0.0, 0.0, 0.0, c)
   _store_vertex(p, 2, x2 - px, y2 - py, 0.0, 0.0, 0.0, c)
   _store_vertex(p, 3, x1 + px, y1 + py, 0.0, 0.0, 0.0, c)
   _store_vertex(p, 4, x2 - px, y2 - py, 0.0, 0.0, 0.0, c)
   _store_vertex(p, 5, x2 + px, y2 + py, 0.0, 0.0, 0.0, c)
   true
}

;; Returns true when draw rect.
fn draw_rect(f64 x, f64 y, f64 w, f64 h, f64 r, f64 g, f64 b, f64 a) bool {
   draw_rect_fast(x, y, w, h, render_shared.pack_rgba_u32(r, g, b, a))
}

fn draw_rect_fast(f64 x, f64 y, f64 w, f64 h, int color_u32) bool {
   "Draws a packed-color rectangle with OpenGL."
   if w == 0.0 || h == 0.0 { return false }
   if _soft_enabled() {
      _soft_rect(x, y, w, h, color_u32)
      _record_draw(6, false, false)
      return true
   }
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * 6)
   if !p { return false }
   _store_rect(p, x, y, w, h, color_u32)
   draw_vertices(p, 6, -1)
}

fn _store_fan_step(any p, int vi, f64 cx, f64 cy, f64 x0, f64 y0, f64 x1, f64 y1, int c) bool {
   _store_vertex(p, vi + 0, cx, cy, 0.0, 0.0, 0.0, c)
   _store_vertex(p, vi + 1, x0, y0, 0.0, 0.0, 0.0, c)
   _store_vertex(p, vi + 2, x1, y1, 0.0, 0.0, 0.0, c)
   true
}

fn draw_rounded_rect_2d(f64 x, f64 y, f64 w, f64 h, f64 radius, int segments, f64 r, f64 g, f64 b, f64 a) bool {
   "Batches a filled rounded rectangle with four fan corners."
   if w <= 0.0 || h <= 0.0 { return false }
   if radius <= 0.0 { return draw_rect(x, y, w, h, r, g, b, a) }
   segments = max(2, int(segments))
   def cs_count = max(2, int(segments / 4))
   def vert_count = 18 + cs_count * 12
   if _soft_enabled() {
      _record_draw(vert_count, false, false)
      return true
   }
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * vert_count)
   if !p { return false }
   def c = render_shared.pack_rgba_u32(r, g, b, a)
   def fx, fy = float(x), float(y)
   def fw, fh = float(w), float(h)
   def rr = float(radius)
   _store_rect(p, fx + rr, fy, fw - rr * 2.0, fh, c)
   _store_rect(p + _STRIDE * 6, fx, fy + rr, rr, fh - rr * 2.0, c)
   _store_rect(p + _STRIDE * 12, fx + fw - rr, fy + rr, rr, fh - rr * 2.0, c)
   def step = (PI * 0.5) / float(cs_count)
   def cs = cos(step)
   def sn = sin(step)
   mut vi = 18
   mut cx, cy = fx + rr, fy + rr
   mut ca0, sa0 = -1.0, 0.0
   mut i = 0
   while i < cs_count {
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _store_fan_step(p, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   cx, cy, ca0, sa0, i = fx + fw - rr, fy + rr, 0.0, -1.0, 0
   while i < cs_count {
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _store_fan_step(p, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   cx, cy, ca0, sa0, i = fx + fw - rr, fy + fh - rr, 1.0, 0.0, 0
   while i < cs_count {
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _store_fan_step(p, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   cx, cy, ca0, sa0, i = fx + rr, fy + fh - rr, 0.0, 1.0, 0
   while i < cs_count {
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _store_fan_step(p, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   draw_vertices(p, vert_count, -1)
}

fn draw_fan_2d(f64 cx, f64 cy, f64 rx, f64 ry, int segments, f64 start_rad, f64 span_rad, f64 r, f64 g, f64 b, f64 a) bool {
   "Draws a filled 2D fan sector with OpenGL."
   if segments < 3 { segments = 3 }
   if _soft_enabled() {
      _record_draw(segments * 3, false, false)
      return true
   }
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * segments * 3)
   if !p { return false }
   def c = render_shared.pack_rgba_u32(r, g, b, a)
   def fcx, fcy = float(cx), float(cy)
   def frx, fry = float(rx), float(ry)
   def step = float(span_rad) / float(segments)
   mut i = 0
   while i < segments {
      def a0 = float(start_rad) + float(i) * step
      def a1 = a0 + step
      _store_fan_step(p, i * 3, fcx, fcy, fcx + cos(a0) * frx, fcy + sin(a0) * fry, fcx + cos(a1) * frx, fcy + sin(a1) * fry, c)
      i += 1
   }
   draw_vertices(p, segments * 3, -1)
}

fn draw_ring_2d(f64 cx, f64 cy, f64 inner_r, f64 outer_r, int segments, f64 r, f64 g, f64 b, f64 a) bool {
   "Draws a filled 2D ring with OpenGL."
   if segments < 3 { segments = 3 }
   if _soft_enabled() {
      _record_draw(segments * 6, false, false)
      return true
   }
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * segments * 6)
   if !p { return false }
   def c = render_shared.pack_rgba_u32(r, g, b, a)
   def fcx, fcy = float(cx), float(cy)
   def step = TAU / float(segments)
   mut i = 0
   while i < segments {
      def a0 = float(i) * step
      def a1 = a0 + step
      def c0, s0 = cos(a0), sin(a0)
      def c1, s1 = cos(a1), sin(a1)
      def p0x, p0y = fcx + c0 * float(inner_r), fcy + s0 * float(inner_r)
      def p1x, p1y = fcx + c0 * float(outer_r), fcy + s0 * float(outer_r)
      def p2x, p2y = fcx + c1 * float(outer_r), fcy + s1 * float(outer_r)
      def p3x, p3y = fcx + c1 * float(inner_r), fcy + s1 * float(inner_r)
      _store_vertex(p, i * 6 + 0, p0x, p0y, 0.0, 0.0, 0.0, c)
      _store_vertex(p, i * 6 + 1, p1x, p1y, 0.0, 0.0, 0.0, c)
      _store_vertex(p, i * 6 + 2, p2x, p2y, 0.0, 0.0, 0.0, c)
      _store_vertex(p, i * 6 + 3, p0x, p0y, 0.0, 0.0, 0.0, c)
      _store_vertex(p, i * 6 + 4, p2x, p2y, 0.0, 0.0, 0.0, c)
      _store_vertex(p, i * 6 + 5, p3x, p3y, 0.0, 0.0, 0.0, c)
      i += 1
   }
   draw_vertices(p, segments * 6, -1)
}

;; Returns true when draw rect outline fast.
fn draw_rect_outline_fast(f64 x, f64 y, f64 w, f64 h, int color_u32, f64 thickness=1.0) bool {
   def t = max(1.0, thickness)
   if _soft_enabled() {
      draw_rect_fast(x, y, w, t, color_u32)
      draw_rect_fast(x, y + h - t, w, t, color_u32)
      draw_rect_fast(x, y, t, h, color_u32)
      draw_rect_fast(x + w - t, y, t, h, color_u32)
      return true
   }
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * 24)
   if !p { return false }
   _store_rect(p + _STRIDE * 0, x, y, w, t, color_u32)
   _store_rect(p + _STRIDE * 6, x, y + h - t, w, t, color_u32)
   _store_rect(p + _STRIDE * 12, x, y, t, h, color_u32)
   _store_rect(p + _STRIDE * 18, x + w - t, y, t, h, color_u32)
   draw_vertices(p, 24, -1)
}

fn draw_rects_fast_ptr(any rects, int count, int stride=20) int {
   "Draws packed rect records through a batched OpenGL vertex stream."
   if !rects || count <= 0 { return 0 }
   if stride < 20 { stride = 20 }
   if _soft_enabled() {
      mut si = 0
      while si < count {
         def rec = rects + si * stride
         _soft_rect(load32_f32(rec, 0), load32_f32(rec, 4), load32_f32(rec, 8), load32_f32(rec, 12), load32(rec, 16))
         si += 1
      }
      _record_draw(count * 6, false, false)
      return count
   }
   def max_batch = 2048
   mut done = 0
   while done < count {
      def batch = min(max_batch, count - done)
      _reset_ui_draw_state()
      def p = _ensure_scratch(batch * _STRIDE * 6)
      if !p { return done }
      mut j = 0
      while j < batch {
         def rec = rects + (done + j) * stride
         _store_rect(
            p + j * _STRIDE * 6,
            load32_f32(rec, 0), load32_f32(rec, 4),
            load32_f32(rec, 8), load32_f32(rec, 12),
            load32(rec, 16)
         )
         j += 1
      }
      draw_vertices(p, batch * 6, -1)
      done += batch
   }
   done
}

fn draw_lines_2d_fast_ptr(any lines, int count, int stride=24) int {
   "Draws packed 2D line records through a batched OpenGL vertex stream."
   if !lines || count <= 0 { return 0 }
   if stride < 24 { stride = 24 }
   if _soft_enabled() {
      mut si = 0
      while si < count {
         def rec = lines + si * stride
         def p2 = _ensure_scratch(_STRIDE * 2)
         if p2 {
            _store_vertex(p2, 0, load32_f32(rec, 0), load32_f32(rec, 4), 0.0, 0.0, 0.0, load32(rec, 20))
            _store_vertex(p2, 1, load32_f32(rec, 8), load32_f32(rec, 12), 0.0, 0.0, 0.0, load32(rec, 20))
            _soft_draw_line_segment(p2, p2 + _STRIDE, load32_f32(rec, 16), false)
         }
         si += 1
      }
      _record_draw(count * 6, false, false)
      return count
   }
   def max_batch = 2048
   mut done = 0
   while done < count {
      def batch = min(max_batch, count - done)
      _reset_ui_draw_state()
      def p = _ensure_scratch(batch * _STRIDE * 6)
      if !p { return done }
      mut j = 0
      while j < batch {
         def rec = lines + (done + j) * stride
         _store_line_quad(
            p + j * _STRIDE * 6,
            load32_f32(rec, 0), load32_f32(rec, 4),
            load32_f32(rec, 8), load32_f32(rec, 12),
            max(1.0, load32_f32(rec, 16)),
            load32(rec, 20)
         )
         j += 1
      }
      draw_vertices(p, batch * 6, -1)
      done += batch
   }
   done
}

fn draw_glyph_bitmap_scaled(
   ptr data,
   int src_w,
   int src_h,
   int dst_w,
   int dst_h,
   f64 ox,
   f64 oy,
   f64 r,
   f64 g,
   f64 b,
   f64 a,
   int bpp=4,
   bool is_color=false
) bool {
   "Draws a glyph bitmap directly into the software GL framebuffer."
   if !_soft_enabled() || !data || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || a <= 0.0 { return false }
   if !_soft_ensure_surface() { return false }
   def base_r = int(max(0.0, min(1.0, r)) * 255.0)
   def base_g = int(max(0.0, min(1.0, g)) * 255.0)
   def base_b = int(max(0.0, min(1.0, b)) * 255.0)
   mut drew = false
   mut yy = 0
   while yy < dst_h {
      mut sy0 = int(floor(float(yy) * float(src_h) / float(dst_h)))
      mut sy1 = int(ceil(float(yy + 1) * float(src_h) / float(dst_h)))
      if sy0 < 0 { sy0 = 0 }
      if sy1 <= sy0 { sy1 = sy0 + 1 }
      if sy1 > src_h { sy1 = src_h }
      mut xx = 0
      while xx < dst_w {
         mut sx0 = int(floor(float(xx) * float(src_w) / float(dst_w)))
         mut sx1 = int(ceil(float(xx + 1) * float(src_w) / float(dst_w)))
         if sx0 < 0 { sx0 = 0 }
         if sx1 <= sx0 { sx1 = sx0 + 1 }
         if sx1 > src_w { sx1 = src_w }
         mut sum_a = 0
         mut sum_r = 0
         mut sum_g = 0
         mut sum_b = 0
         mut samples = 0
         mut sy = sy0
         while sy < sy1 {
            mut sx = sx0
            while sx < sx1 {
               def px_off = (sy * src_w + sx) * bpp
               sum_a += load8(data, px_off + (bpp >= 4 ? 3 : 0)) & 255
               if is_color && bpp >= 4 {
                  sum_r += load8(data, px_off + 0) & 255
                  sum_g += load8(data, px_off + 1) & 255
                  sum_b += load8(data, px_off + 2) & 255
               }
               samples += 1
               sx += 1
            }
            sy += 1
         }
         if samples <= 0 { samples = 1 }
         def alpha8 = int(float(sum_a / samples) * max(0.0, min(1.0, a)))
         if alpha8 > 0 {
            mut rr, gg, bb = base_r, base_g, base_b
            if is_color && bpp >= 4 {
               rr = sum_r / samples
               gg = sum_g / samples
               bb = sum_b / samples
            }
            _soft_put(int(ox + xx), int(oy + yy), _soft_pack(rr, gg, bb, alpha8))
            drew = true
         }
         xx += 1
      }
      yy += 1
   }
   drew
}

;; Returns true when draw rect tex.
fn draw_rect_tex(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 r, f64 g, f64 b, f64 a) bool {
   draw_rect_tex_uv(x, y, w, h, tex_id, 0.0, 0.0, 1.0, 1.0, r, g, b, a)
}

;; Returns true when draw rect tex uv.
fn draw_rect_tex_uv(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 u1, f64 v1, f64 u2, f64 v2, f64 r, f64 g, f64 b, f64 a) bool {
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * 6)
   if !p { return false }
   _store_rect(p, x, y, w, h, render_shared.pack_rgba_u32(r, g, b, a), u1, v1, u2, v2)
   draw_vertices(p, 6, tex_id)
}

;; Returns true when draw rect tex uv rot.
fn draw_rect_tex_uv_rot(f64 cx, f64 cy, f64 w, f64 h, f64 rot_deg, int tex_id, f64 u1, f64 v1, f64 u2, f64 v2, f64 r, f64 g, f64 b, f64 a) bool {
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * 6)
   if !p { return false }
   def hw, hh = w * 0.5, h * 0.5
   def rad = rot_deg * PI / 180.0
   def co, si = cos(rad), sin(rad)
   def c = render_shared.pack_rgba_u32(r, g, b, a)
   def x0, y0 = -hw, -hh
   def x1, y1 = hw, hh
   def ax, ay = cx + x0 * co - y0 * si, cy + x0 * si + y0 * co
   def bx, by = cx + x1 * co - y1 * si, cy + x1 * si + y1 * co
   def cx0, cy0 = cx + x1 * co - y0 * si, cy + x1 * si + y0 * co
   def dx, dy = cx + x0 * co - y1 * si, cy + x0 * si + y1 * co
   _store_vertex(p, 0, ax, ay, 0.0, u1, v1, c)
   _store_vertex(p, 1, bx, by, 0.0, u2, v2, c)
   _store_vertex(p, 2, cx0, cy0, 0.0, u2, v1, c)
   _store_vertex(p, 3, ax, ay, 0.0, u1, v1, c)
   _store_vertex(p, 4, dx, dy, 0.0, u1, v2, c)
   _store_vertex(p, 5, bx, by, 0.0, u2, v2, c)
   draw_vertices(p, 6, tex_id)
}

fn draw_line_fast(f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, int color_u32) bool {
   "Draws a packed-color thick 2D line with OpenGL triangles."
   if _soft_enabled() {
      def dxs, dys = x2 - x1, y2 - y1
      def steps = max(1, int(max(abs(dxs), abs(dys))) + 1)
      mut last_x, last_y = -999999, -999999
      mut i = 0
      while i <= steps {
         def t = float(i) / float(steps)
         def x = x1 + dxs * t
         def y = y1 + dys * t
         def ix, iy = int(x + 0.5), int(y + 0.5)
         if ix != last_x || iy != last_y {
            _soft_plot_line_sample(ix, iy, thickness, color_u32)
            last_x, last_y = ix, iy
         }
         i += 1
      }
      _record_draw(2, false, false)
      return true
   }
   def dx, dy = x2 - x1, y2 - y1
   def len = sqrt(dx * dx + dy * dy)
   if len <= 0.000001 { return draw_rect_fast(x1, y1, max(1.0, thickness), max(1.0, thickness), color_u32) }
   def px, py = -dy / len * thickness * 0.5, dx / len * thickness * 0.5
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * 6)
   if !p { return false }
   _store_vertex(p, 0, x1 + px, y1 + py, 0.0, 0.0, 0.0, color_u32)
   _store_vertex(p, 1, x1 - px, y1 - py, 0.0, 0.0, 0.0, color_u32)
   _store_vertex(p, 2, x2 - px, y2 - py, 0.0, 0.0, 0.0, color_u32)
   _store_vertex(p, 3, x1 + px, y1 + py, 0.0, 0.0, 0.0, color_u32)
   _store_vertex(p, 4, x2 - px, y2 - py, 0.0, 0.0, 0.0, color_u32)
   _store_vertex(p, 5, x2 + px, y2 + py, 0.0, 0.0, 0.0, color_u32)
   draw_vertices(p, 6, -1)
}

;; Returns true when draw line.
fn draw_line(f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, f64 r, f64 g, f64 b, f64 a) bool {
   draw_line_fast(x1, y1, x2, y2, thickness, render_shared.pack_rgba_u32(r, g, b, a))
}

fn draw_line_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 thickness, f64 r, f64 g, f64 b, f64 a) bool {
   "Draws a 3D line through the active GL path."
   _set_depth_mask(true)
   if _soft_enabled() {
      if !_soft_ensure_surface() { return false }
      def p = _ensure_scratch(_STRIDE * 2)
      if !p { return false }
      def c = render_shared.pack_rgba_u32(r, g, b, a)
      _store_vertex(p, 0, x1, y1, z1, 0.0, 0.0, c)
      _store_vertex(p, 1, x2, y2, z2, 0.0, 0.0, c)
      def pxw = max(thickness * float(min(_soft_w, _soft_h)) * 0.18, 1.0)
      _soft_draw_line_segment(p, p + _STRIDE, pxw, false)
      _record_draw(2, false, false)
      return true
   }
   def dx, dy, dz = x2 - x1, y2 - y1, z2 - z1
   if sqrt(dx * dx + dy * dy + dz * dz) <= 0.0000001 { return false }
   mut nx, ny, nz = dy, 0.0 - dx, 0.0
   mut nl = sqrt(nx * nx + ny * ny + nz * nz)
   if nl <= 0.0000001 {
      nx, ny, nz = 0.0 - dz, 0.0, dx
      nl = sqrt(nx * nx + ny * ny + nz * nz)
   }
   if nl <= 0.0000001 { return false }
   def hs = thickness / (2.0 * nl)
   nx, ny, nz = nx * hs, ny * hs, nz * hs
   draw_quad_3d(
      x1 + nx, y1 + ny, z1 + nz,
      x1 - nx, y1 - ny, z1 - nz,
      x2 - nx, y2 - ny, z2 - nz,
      x2 + nx, y2 + ny, z2 + nz,
      r, g, b, a
   )
}

;; Returns true when draw triangle 3d.
fn draw_triangle_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 x3, f64 y3, f64 z3, f64 r, f64 g, f64 b, f64 a) bool {
   _set_depth_mask(true)
   def p = _ensure_scratch(_STRIDE * 3)
   if !p { return false }
   def c = render_shared.pack_rgba_u32(r, g, b, a)
   _store_vertex(p, 0, x1, y1, z1, 0.0, 0.0, c)
   _store_vertex(p, 1, x2, y2, z2, 0.0, 0.0, c)
   _store_vertex(p, 2, x3, y3, z3, 0.0, 0.0, c)
   draw_vertices(p, 3, -1)
}

;; Returns true when draw quad 3d.
fn draw_quad_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 x3, f64 y3, f64 z3, f64 x4, f64 y4, f64 z4, f64 r, f64 g, f64 b, f64 a) bool {
   _set_depth_mask(true)
   def p = _ensure_scratch(_STRIDE * 6)
   if !p { return false }
   def c = render_shared.pack_rgba_u32(r, g, b, a)
   _store_vertex(p, 0, x1, y1, z1, 0.0, 0.0, c)
   _store_vertex(p, 1, x2, y2, z2, 0.0, 0.0, c)
   _store_vertex(p, 2, x3, y3, z3, 0.0, 0.0, c)
   _store_vertex(p, 3, x1, y1, z1, 0.0, 0.0, c)
   _store_vertex(p, 4, x3, y3, z3, 0.0, 0.0, c)
   _store_vertex(p, 5, x4, y4, z4, 0.0, 0.0, c)
   draw_vertices(p, 6, -1)
}
