;; Keywords: render opengl gl buffer vbo ibo static
;; References: std.os.ui.render.gl.state std.os.ui.render.gl.texture
module std.os.ui.render.gl.buffer(base, bytes, cp, create_static_buffer, create_static_index_buffer, create_static_indexed_buffer, destroy_static_buffer, draw_static_buffer, draw_static_buffer_indexed, draw_static_buffer_indexed_raw, draw_static_buffer_raw, h, ib, id, idx_ty, sip, sp, stride, use_u32, vb)
use std.core
use std.math
use std.os.ffi as ffi
use std.os.ui.render.shared as render_shared
use std.os.ui.render.gl.constants as gl_constants
use std.os.ui.render.gl.state as gl_state
use std.os.ui.render.gl.texture as gl_tex

fn create_static_buffer(?ptr src_ptr, int count) any {
   "Uploads a packed vertex buffer to an OpenGL VBO."
   if _soft_enabled() {
      if !src_ptr || count <= 0 { return 0 }
      def bytes = count * _STRIDE
      def cp = malloc(bytes)
      if !cp { return 0 }
      memcpy(cp, src_ptr, bytes)
      return {"backend": "gl", "handle": cp, "offset": 0, "count": count, "soft": true, "soft_ptr": cp}
   }
   if !src_ptr || count <= 0 || !_has("glGenBuffers") { return 0 }
   def id = _gen_name("glGenBuffers")
   if id <= 0 { return 0 }
   _bind_buffer(GL_ARRAY_BUFFER, id)
   _ny_glBufferData(GL_ARRAY_BUFFER, count * _STRIDE, ptr_add(src_ptr, 0), GL_STATIC_DRAW)
   _bind_buffer(GL_ARRAY_BUFFER, 0)
   return {"backend": "gl", "handle": id, "offset": 0, "count": count}
}

fn create_static_index_buffer(?ptr idx_ptr, int idx_count, bool use_u32=false) any {
   "Uploads an index buffer to an OpenGL IBO."
   if _soft_enabled() {
      if !idx_ptr || idx_count <= 0 { return 0 }
      def stride = use_u32 ? 4 : 2
      def bytes = idx_count * stride
      def cp = malloc(bytes)
      if !cp { return 0 }
      memcpy(cp, idx_ptr, bytes)
      return {"backend": "gl", "ibuf": cp, "ioffset": 0, "index_count": idx_count, "index_type_u32": use_u32, "soft": true, "soft_ibuf": cp}
   }
   if !idx_ptr || idx_count <= 0 || !_has("glGenBuffers") { return 0 }
   def id = _gen_name("glGenBuffers")
   if id <= 0 { return 0 }
   def stride = use_u32 ? 4 : 2
   _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, id)
   _ny_glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_count * stride, ptr_add(idx_ptr, 0), GL_STATIC_DRAW)
   _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0)
   return {"backend": "gl", "ibuf": id, "ioffset": 0, "index_count": idx_count, "index_type_u32": use_u32}
}

;; Creates and returns the static indexed buffer.
fn create_static_indexed_buffer(?ptr vert_ptr, int count, ?ptr idx_ptr, int idx_count, any opts=0) any {
   def vb = create_static_buffer(vert_ptr, count)
   if !vb { return 0 }
   def use_u32 = is_dict(opts) && opts.get("index_type_u32", false)
   def ib = create_static_index_buffer(idx_ptr, idx_count, use_u32)
   if !ib { destroy_static_buffer(vb) return 0 }
   vb["backend"] = "gl"
   vb["ibuf"] = ib.get("ibuf", 0)
   vb["ioffset"] = ib.get("ioffset", 0)
   vb["index_count"] = idx_count
   vb["index_type_u32"] = use_u32
   if ib.get("soft", false) {
      vb["soft"] = true
      vb["soft_ibuf"] = ib.get("soft_ibuf", ib.get("ibuf", 0))
   }
   vb
}

fn destroy_static_buffer(any sbuf) bool {
   "Deletes OpenGL VBO/IBO resources."
   if !is_dict(sbuf) { return false }
   if sbuf.get("soft", false) {
      def sp = sbuf.get("soft_ptr", 0)
      def sip = sbuf.get("soft_ibuf", 0)
      if sp { free(sp) }
      if sip && sip != sp { free(sip) }
      return true
   }
   def h = int(sbuf.get("handle", 0))
   def ib = int(sbuf.get("ibuf", 0))
   if h > 0 { _delete_name("glDeleteBuffers", h) }
   if ib > 0 { _delete_name("glDeleteBuffers", ib) }
   true
}

;; Returns true when draw static buffer.
fn draw_static_buffer(dict sbuf, bool is_lines=false, f64 width=1.0, any pipe_override=0, bool is_points=false, bool use_material=false) bool {
   if !is_dict(sbuf) { return false }
   draw_static_buffer_raw(sbuf.get("handle", 0), sbuf.get("offset", 0), int(sbuf.get("count", 0)), is_lines, width, pipe_override, is_points, use_material)
}

;; Returns true when draw static buffer raw.
fn draw_static_buffer_raw(any buf, any voff, int count, bool is_lines=false, f64 width=1.0, any _pipe_override=0, bool is_points=false, bool use_material=false) bool {
   _pipe_override
   if !buf || count <= 0 { return false }
   if _soft_enabled() {
      def base = buf + int(voff)
      if is_points { _soft_draw_points(base, count, use_material) }
      elif is_lines { _soft_draw_lines(base, int(count / 2), width, use_material) }
      else { _soft_draw_vertices(base, count, -1, use_material) }
      _record_draw(count, true, false)
      return true
   }
   _enable_draw_state(-2, use_material)
   _call1f("glLineWidth", max(1.0, width))
   _bind_buffer(GL_ARRAY_BUFFER, buf)
   _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0)
   _setup_arrays_vbo(voff, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit)
   _ny_glDrawArrays(_draw_mode(is_lines, is_points), 0, count)
   _record_draw(count, true, false)
   _bind_buffer(GL_ARRAY_BUFFER, 0)
}

;; Returns true when draw static buffer indexed.
fn draw_static_buffer_indexed(dict sbuf, any idx_buf, int index_count, bool is_lines=false, f64 width=1.0, any pipe_override=0, bool is_points=false, bool use_material=false) bool {
   if !is_dict(sbuf) { return false }
   draw_static_buffer_indexed_raw(sbuf.get("handle", 0), sbuf.get("offset", 0), idx_buf, sbuf.get("ioffset", 0), index_count, is_lines, width, pipe_override, sbuf.get("index_type_u32", false) ? 1 : 0, is_points, use_material)
}

;; Returns true when draw static buffer indexed raw.
fn draw_static_buffer_indexed_raw(any buf, any voff, any idx_buf, any ioff, int index_count, bool is_lines=false, f64 width=1.0, any _pipe_override=0, int index_type=0, bool is_points=false, bool use_material=false) bool {
   _pipe_override
   if !buf || !idx_buf || index_count <= 0 { return false }
   if _soft_enabled() {
      _soft_draw_indexed(buf + int(voff), 0, idx_buf + int(ioff), index_count, index_type, -1, is_lines, width, is_points, use_material)
      _record_draw(index_count, true, true)
      return true
   }
   _enable_draw_state(-2, use_material)
   _call1f("glLineWidth", max(1.0, width))
   _bind_buffer(GL_ARRAY_BUFFER, buf)
   _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, idx_buf)
   _setup_arrays_vbo(voff, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit)
   def idx_ty = index_type == 1 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT
   _ny_glDrawElementsOffset(_draw_mode(is_lines, is_points), index_count, idx_ty, int(ioff))
   if _gl_finish_each_draw() { _ny_glFinish() }
   _record_draw(index_count, true, true)
   _bind_buffer(GL_ARRAY_BUFFER, 0)
   _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0)
}
