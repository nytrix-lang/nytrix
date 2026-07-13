;; Keywords: render opengl gl state init material scissor raster ffi
;; References: std.os.ui.render.gl.constants std.os.ui.window std.os.ffi
module std.os.ui.render.gl.state(_apply_current_color, _apply_lighting, _apply_matrices, _base_uv_offset, _bind_buffer, _bound_tex, _call0, _call1, _call1f, _call1u, _call2, _call2f, _call3, _call3f, _call4, _call4f, _call7, _call9, _call_trace_enabled, _call_trace_mode, _cfg_gl_finish_each_draw, _cfg_gl_force_base_only, _cfg_gl_lit_textures, _cfg_gl_perf, _cfg_gl_preview_textures, _cfg_gl_vertex_colors, _clear_a, _clear_b, _clear_g, _clear_r, _color_chan, _current_alpha_u32, _current_base_color_u32, _current_base_tex_id, _current_base_uv_xf0, _current_base_uv_xf1, _current_material_u32, _current_unlit, _current_vc_mode, _decode_uv_offset16, _decode_uv_rot8, _decode_uv_scale11, _default_ortho, _default_tex, _delete_name, _depth_mask_enabled, _draw_immediate_vertices, _draw_mode, _dynamic_vbo, _enable_draw_state, _ensure_dynamic_vbo, _ensure_libgl, _ensure_light_buf, _ensure_matrix_buf, _ensure_name_ptr, _ensure_scratch, _filter_value, _format_external, _format_internal, _frame_draw_calls, _frame_dynamic_draw_calls, _frame_indexed_draw_calls, _frame_open, _frame_static_draw_calls, _frame_submitted_vertices, _gen_name, _gl_finish_each_draw, _gl_lit_textures, _gl_preview_textures, _gl_refresh_config, _gl_use_vertex_colors, _h, _has, _last_frame_draw_calls, _last_frame_dynamic_draw_calls, _last_frame_indexed_draw_calls, _last_frame_static_draw_calls, _last_submitted_vertices, _last_tex, _last_tex_env_mode, _last_texture_xf0, _last_texture_xf1, _libgl, _light_buf, _lighting_enabled, _load_base_uv_texture_matrix, _load_mat, _material_uses_vertex_color, _matrix_buf, _mesh_cull_enabled, _mesh_front_face, _model, _mvp, _name_ptr, _next_frame_load_color, _norm_i32, _ny_glBegin, _ny_glBindBuffer, _ny_glBlendFunc, _ny_glBufferData, _ny_glColor4d, _ny_glColor4ub, _ny_glColorMaterial, _ny_glColorPointer, _ny_glColorPointerOffset, _ny_glDepthFunc, _ny_glDisable, _ny_glDisableClientState, _ny_glDrawArrays, _ny_glDrawElements, _ny_glDrawElementsOffset, _ny_glEnable, _ny_glEnableClientState, _ny_glEnd, _ny_glFinish, _ny_glFrontFace, _ny_glLightfv, _ny_glNormal3d, _ny_glNormal3f, _ny_glNormalPointer, _ny_glNormalPointerOffset, _ny_glReadBuffer, _ny_glReadPixels, _ny_glRectd, _ny_glScissor, _ny_glTexCoord2d, _ny_glTexCoord2f, _ny_glTexCoordPointer, _ny_glTexCoordPointerOffset, _ny_glTexEnvi, _ny_glVertex3d, _ny_glVertex3f, _ny_glVertexPointer, _ny_glVertexPointerOffset, _proc, _procs, _publish_frame_stats, _record_draw, _reset_frame_stats, _reset_ui_draw_state, _scratch, _scratch_cap, _set_depth_mask, _set_tex_env_mode, _setup_arrays, _setup_arrays_vbo, _soft_active_tex, _soft_buf, _soft_clear, _soft_color_blend, _soft_color_chan, _soft_color_lerp2, _soft_color_lerp3, _soft_copy_texture_pixels, _soft_draw_indexed, _soft_draw_line_segment, _soft_draw_lines, _soft_draw_points, _soft_draw_tri, _soft_draw_vertices, _soft_enabled, _soft_ensure_surface, _soft_fill_rect_raw, _soft_h, _soft_index, _soft_mode, _soft_mul_color, _soft_pack, _soft_plot_line_sample, _soft_project, _soft_put, _soft_rect, _soft_sample_tex, _soft_scissor, _soft_sh, _soft_sw, _soft_sx, _soft_sy, _soft_tex_meta, _soft_update_texture_pixels, _soft_vertex_color, _soft_w, _store_light_vec, _store_vertex, _tex_formats, _tex_live, _tex_pixels, _upload_dynamic_vertices, _uv_xf_identity, _w, _win, _wireframe, a, active_tex, alpha, area, b, base_tex_id, begin_frame, bind_default_texture, bind_texture, bo, bytes, c, c0, c00, c01, c10, c11, capabilities, clear, clear_depth, col, cp, cr, cw, dr, dx, e0, e1, e2, end_frame, env_mode, face, fb, first_off, frame_stats, fx, fy, get_swapchain_height, get_swapchain_image_count, get_swapchain_width, glColor4d, glColor4ub, glRectd, glRecti, h, i, id, init, inv, ix, k, keys, last_x, m, mat, material_preview, max_x, max_y, meta, min_x, min_y, n, need, notify_window_resize, nx, o0, o1, off, off_x, off_y, old_sc, p, p0, p1, p2, pi, pix, pkeys, pp, pt, px, radius, renderer_vertex_offset, reset_scissor_rect, rot, rot_bits, rough, row_bytes, row_pixels, sa, scl_x, scl_y, set_clear_color, set_material, set_material_from_slab, set_material_from_slab_base, set_material_packed, set_mesh_raster_state, set_model_matrix, set_mvp, set_next_frame_load_color, set_ortho, set_perspective, set_scissor_rect, set_ui_material, set_unlit, set_vertex_color_mode, set_wireframe, shade, shutdown, sr, steps, sx, sz, t, trace_mode, tu, tv, tw, tx, ty, u, u0, u1, u2, uu, uv_off, v, vi, want_cull, x, x0, x1, x2, xx, y, y0, y1, yy)
use std.os.ui.render.gl.constants as gl_constants
use std.core
use std.core.str as core_str
use std.core.common as common
use std.math
use std.os.ffi as ffi
use std.os.ui.window as lib_uiw
use std.os.ui.window.platform as ui_backend
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.matrix as matrix
use std.os.ui.render.shared as render_shared

fn _is_debug() bool { ui_profile.debug_enabled() }

fn _check_gl_error(str where) bool {
   if !_is_debug() { return true }
   def err = int(_ny_glGetError())
   if err != 0 {
      print("[gl:error] " + where + ": " + to_str(err))
      return false
   }
   true
}

#linux {
   #link "libGL.so"
   #include <GL/gl.h>
   extern "" {
      fn _ny_glReadPixels(i32 x, i32 y, i32 width, i32 height, u32 format, u32 typ, ptr pixels) as "glReadPixels"
      fn _ny_glReadBuffer(u32 mode) as "glReadBuffer"
      fn _ny_glFinish() as "glFinish"
      fn _ny_glClear(u32 mask) as "glClear"
      fn _ny_glClearColor(f32 r, f32 g, f32 b, f32 a) as "glClearColor"
      fn _ny_glViewport(i32 x, i32 y, i32 width, i32 height) as "glViewport"
      fn _ny_glMatrixMode(u32 mode) as "glMatrixMode"
      fn _ny_glLoadMatrixf(ptr m) as "glLoadMatrixf"
      fn _ny_glPolygonMode(u32 face, u32 mode) as "glPolygonMode"
      fn _ny_glDepthMask(u32 flag) as "glDepthMask"
      fn _ny_glActiveTexture(u32 texture) as "glActiveTexture"
      fn _ny_glBindTexture(u32 target, u32 texture) as "glBindTexture"
      fn _ny_glTexParameteri(u32 target, u32 pname, i32 param) as "glTexParameteri"
      fn _ny_glPixelStorei(u32 pname, i32 param) as "glPixelStorei"
      fn _ny_glTexImage2D(u32 target, i32 level, i32 internalformat, i32 width, i32 height, i32 border, u32 format, u32 typ, ptr pixels) as "glTexImage2D"
      fn _ny_glTexSubImage2D(u32 target, i32 level, i32 x, i32 y, i32 width, i32 height, u32 format, u32 typ, ptr pixels) as "glTexSubImage2D"
      fn _ny_glGenerateMipmap(u32 target) as "glGenerateMipmap"
      fn _ny_glEnable(u32 cap) as "glEnable"
      fn _ny_glDisable(u32 cap) as "glDisable"
      fn _ny_glScissor(i32 x, i32 y, i32 width, i32 height) as "glScissor"
      fn _ny_glDepthFunc(u32 func) as "glDepthFunc"
      fn _ny_glFrontFace(u32 mode) as "glFrontFace"
      fn _ny_glBlendFunc(u32 sfactor, u32 dfactor) as "glBlendFunc"
      fn _ny_glBlendFuncSeparate(u32 srcRGB, u32 dstRGB, u32 srcAlpha, u32 dstAlpha) as "glBlendFuncSeparate"
      fn _ny_glBindBuffer(u32 target, u32 buffer) as "glBindBuffer"
      fn _ny_glBufferData(u32 target, i64 size, ptr data, u32 usage) as "glBufferData"
      fn _ny_glColorMaterial(u32 face, u32 mode) as "glColorMaterial"
      fn _ny_glLightfv(u32 light, u32 pname, ptr params) as "glLightfv"
      fn _ny_glTexEnvi(u32 target, u32 pname, i32 param) as "glTexEnvi"
      fn _ny_glEnableClientState(u32 array) as "glEnableClientState"
      fn _ny_glDisableClientState(u32 array) as "glDisableClientState"
      fn _ny_glColorPointer(i32 size, u32 typ, i32 stride, ptr pointer) as "glColorPointer"
      fn _ny_glVertexPointer(i32 size, u32 typ, i32 stride, ptr pointer) as "glVertexPointer"
      fn _ny_glTexCoordPointer(i32 size, u32 typ, i32 stride, ptr pointer) as "glTexCoordPointer"
      fn _ny_glNormalPointer(u32 typ, i32 stride, ptr pointer) as "glNormalPointer"
      fn _ny_glDrawArrays(u32 mode, i32 first, i32 count) as "glDrawArrays"
      fn _ny_glDrawElements(u32 mode, i32 count, u32 typ, ptr indices) as "glDrawElements"
      fn _ny_glColorPointerOffset(i32 size, u32 typ, i32 stride, handle pointer) as "glColorPointer"
      fn _ny_glVertexPointerOffset(i32 size, u32 typ, i32 stride, handle pointer) as "glVertexPointer"
      fn _ny_glTexCoordPointerOffset(i32 size, u32 typ, i32 stride, handle pointer) as "glTexCoordPointer"
      fn _ny_glNormalPointerOffset(u32 typ, i32 stride, handle pointer) as "glNormalPointer"
      fn _ny_glDrawElementsOffset(u32 mode, i32 count, u32 typ, handle indices) as "glDrawElements"
      fn _ny_glBegin(u32 mode) as "glBegin"
      fn _ny_glEnd() as "glEnd"
      fn _ny_glColor4ub(u32 r, u32 g, u32 b, u32 a) as "glColor4ub"
      fn _ny_glTexCoord2f(f32 u, f32 v) as "glTexCoord2f"
      fn _ny_glNormal3f(f32 x, f32 y, f32 z) as "glNormal3f"
      fn _ny_glVertex3f(f32 x, f32 y, f32 z) as "glVertex3f"
      fn _ny_glColor4d(f64 r, f64 g, f64 b, f64 a) as "glColor4d"
      fn _ny_glTexCoord2d(f64 u, f64 v) as "glTexCoord2d"
      fn _ny_glNormal3d(f64 x, f64 y, f64 z) as "glNormal3d"
      fn _ny_glVertex3d(f64 x, f64 y, f64 z) as "glVertex3d"
      fn _ny_glRectd(f64 x1, f64 y1, f64 x2, f64 y2) as "glRectd"
      fn _ny_glGetString(u32 name) ptr as "glGetString"
      fn _ny_glGetError() u32 as "glGetError"
   }
} #else {
   fn _ny_glReadPixels(int _x, int _y, int _width, int _height, int _format, int _type, any _pixels) any { nil }
   fn _ny_glReadBuffer(int _mode) any { nil }
   fn _ny_glFinish() any { nil }
   fn _ny_glClear(int _mask) any { nil }
   fn _ny_glClearColor(f64 _r, f64 _g, f64 _b, f64 _a) any { nil }
   fn _ny_glViewport(int _x, int _y, int _width, int _height) any { nil }
   fn _ny_glMatrixMode(int _mode) any { nil }
   fn _ny_glLoadMatrixf(any _m) any { nil }
   fn _ny_glPolygonMode(int _face, int _mode) any { nil }
   fn _ny_glDepthMask(int _flag) any { nil }
   fn _ny_glActiveTexture(int _texture) any { nil }
   fn _ny_glBindTexture(int _target, int _texture) any { nil }
   fn _ny_glTexParameteri(int _target, int _pname, int _param) any { nil }
   fn _ny_glPixelStorei(int _pname, int _param) any { nil }
   fn _ny_glTexImage2D(int _target, int _level, int _internalformat, int _width, int _height, int _border, int _format, int _type, any _pixels) any { nil }
   fn _ny_glTexSubImage2D(int _target, int _level, int _x, int _y, int _width, int _height, int _format, int _type, any _pixels) any { nil }
   fn _ny_glGenerateMipmap(int _target) any { nil }
   fn _ny_glEnable(int _cap) any { nil }
   fn _ny_glDisable(int _cap) any { nil }
   fn _ny_glScissor(int _x, int _y, int _width, int _height) any { nil }
   fn _ny_glDepthFunc(int _func) any { nil }
   fn _ny_glFrontFace(int _mode) any { nil }
   fn _ny_glBlendFunc(int _sfactor, int _dfactor) any { nil }
   fn _ny_glBlendFuncSeparate(int _srcRGB, int _dstRGB, int _srcAlpha, int _dstAlpha) any { nil }
   fn _ny_glBindBuffer(int _target, int _buffer) any { nil }
   fn _ny_glBufferData(int _target, int _size, any _data, int _usage) any { nil }
   fn _ny_glColorMaterial(int _face, int _mode) any { nil }
   fn _ny_glLightfv(int _light, int _pname, any _params) any { nil }
   fn _ny_glTexEnvi(int _target, int _pname, int _param) any { nil }
   fn _ny_glEnableClientState(int _array) any { nil }
   fn _ny_glDisableClientState(int _array) any { nil }
   fn _ny_glColorPointer(int _size, int _type, int _stride, any _pointer) any { nil }
   fn _ny_glVertexPointer(int _size, int _type, int _stride, any _pointer) any { nil }
   fn _ny_glTexCoordPointer(int _size, int _type, int _stride, any _pointer) any { nil }
   fn _ny_glNormalPointer(int _type, int _stride, any _pointer) any { nil }
   fn _ny_glDrawArrays(int _mode, int _first, int _count) any { nil }
   fn _ny_glDrawElements(int _mode, int _count, int _type, any _indices) any { nil }
   fn _ny_glColorPointerOffset(int _size, int _type, int _stride, any _pointer) any { nil }
   fn _ny_glVertexPointerOffset(int _size, int _type, int _stride, any _pointer) any { nil }
   fn _ny_glTexCoordPointerOffset(int _size, int _type, int _stride, any _pointer) any { nil }
   fn _ny_glNormalPointerOffset(int _type, int _stride, any _pointer) any { nil }
   fn _ny_glDrawElementsOffset(int _mode, int _count, int _type, any _indices) any { nil }
   fn _ny_glBegin(int _mode) any { nil }
   fn _ny_glEnd() any { nil }
   fn _ny_glColor4ub(int _r, int _g, int _b, int _a) any { nil }
   fn _ny_glTexCoord2f(f64 _u, f64 _v) any { nil }
   fn _ny_glNormal3f(f64 _x, f64 _y, f64 _z) any { nil }
   fn _ny_glVertex3f(f64 _x, f64 _y, f64 _z) any { nil }
   fn _ny_glColor4d(f64 _r, f64 _g, f64 _b, f64 _a) any { nil }
   fn _ny_glTexCoord2d(f64 _u, f64 _v) any { nil }
   fn _ny_glNormal3d(f64 _x, f64 _y, f64 _z) any { nil }
   fn _ny_glVertex3d(f64 _x, f64 _y, f64 _z) any { nil }
   fn _ny_glRectd(f64 _x1, f64 _y1, f64 _x2, f64 _y2) any { nil }
   fn _ny_glGetString(int _name) ptr { nil }
   fn _ny_glGetError() int { 0 }
   ;; Returns the result of the `glColor4d` operation.
   fn glColor4d(f64 _r, f64 _g, f64 _b, f64 _a) any { nil }
   ;; Returns the result of the `glColor4ub` operation.
   fn glColor4ub(u32 _r, u32 _g, u32 _b, u32 _a) any { nil }
   ;; Returns the result of the `glRectd` operation.
   fn glRectd(f64 _x1, f64 _y1, f64 _x2, f64 _y2) any { nil }
   ;; Returns the result of the `glRecti` operation.
   fn glRecti(i32 _x1, i32 _y1, i32 _x2, i32 _y2) any { nil }
} #endif
mut _win = 0
mut _w = 0
mut _h = 0
mut _frame_open = false
mut _procs = dict(128)
mut _libgl = 0
mut _matrix_buf = 0
mut _light_buf = 0
mut _scratch = 0
mut _scratch_cap = 0
mut _soft_buf = 0
mut _soft_w = 0
mut _soft_h = 0
mut _soft_mode = -1
mut _soft_scissor = false
mut _soft_sx = 0
mut _soft_sy = 0
mut _soft_sw = 0
mut _soft_sh = 0
mut _dynamic_vbo = 0
mut _name_ptr = 0
mut _mvp = 0
mut _model = 0
mut _default_tex = 0
mut _bound_tex = -999999
mut _last_tex = -1
mut _tex_live = dict(128)
mut _tex_formats = dict(128)
mut _tex_pixels = dict(128)
mut _wireframe = false
mut _lighting_enabled = false
mut _depth_mask_enabled = true
mut _blend_enabled = true
;; Cached GL_TEXTURE_2D / GL_DEPTH_TEST enable state. The previous code issued
;; glEnable/glDisable unconditionally on every draw_vertices call, which on a
;; 2000-cell terminal frame meant ~4000 redundant GL calls just to restate the
;; same enable bits. These mirror the existing _bound_tex / _depth_mask_enabled
;; caching pattern.
mut _texture_2d_enabled = false
mut _depth_test_enabled = false
mut _multisample_enabled = false
mut _clear_r = 0.0
mut _clear_g = 0.0
mut _clear_b = 0.0
mut _clear_a = 1.0
mut _next_frame_load_color = false
mut _current_unlit = true
mut _current_vc_mode = 12
mut _current_base_color_u32 = 0xffffffff
mut _current_material_u32 = 0x0000ff00
mut _current_base_tex_id = -1
mut _current_alpha_u32 = 0
mut _current_bsdf0_u32 = 0
mut _current_bsdf5_u32 = 0
mut _current_base_uv_xf0 = 0
mut _current_base_uv_xf1 = 0
mut _last_texture_xf0 = 0x7fffffff
mut _last_texture_xf1 = 0x7fffffff
mut _frame_draw_calls = 0
mut _frame_dynamic_draw_calls = 0
mut _frame_static_draw_calls = 0
mut _frame_indexed_draw_calls = 0
mut _frame_submitted_vertices = 0
mut _last_frame_draw_calls = 0
mut _last_frame_dynamic_draw_calls = 0
mut _last_frame_static_draw_calls = 0
mut _last_frame_indexed_draw_calls = 0
mut _last_submitted_vertices = 0
mut _call_trace_mode = -1
mut _cfg_gl_force_base_only = false
mut _cfg_gl_preview_textures = true
mut _cfg_gl_lit_textures = false
mut _cfg_gl_vertex_colors = false
mut _cfg_gl_finish_each_draw = false
mut _cfg_gl_perf = false
mut _last_tex_env_mode = -999999
mut _mesh_cull_enabled = false
mut _mesh_front_face = GL_CCW

fn _soft_enabled() bool {
   if _soft_mode < 0 {
      if common.env_truthy("NY_GL_SOFTWARE_DRAW") || common.env_truthy("NY_GL_SOFTWARE_UPLOAD") {
         _soft_mode = 1
      } elif common.env_present("NY_GL_NATIVE_DRAW") {
         _soft_mode = common.env_truthy("NY_GL_NATIVE_DRAW") ? 0 : 1
      } else {
         _soft_mode = 0
      }
   }
   _soft_mode == 1
}

fn _soft_pack(int r, int g, int b, int a=255) int {
   (r & 255) | ((g & 255) << 8) | ((b & 255) << 16) | ((a & 255) << 24)
}

fn _soft_color_chan(int c, int shift) int { (int(c) >> shift) & 255 }

fn _soft_color_blend(int dst, int src) int {
   def sa = _soft_color_chan(src, 24)
   if sa >= 255 { return src }
   if sa <= 0 { return dst }
   def inv = 255 - sa
   def dr, dg, db, da = _soft_color_chan(dst, 0), _soft_color_chan(dst, 8), _soft_color_chan(dst, 16), _soft_color_chan(dst, 24)
   def sr, sg, sb = _soft_color_chan(src, 0), _soft_color_chan(src, 8), _soft_color_chan(src, 16)
   _soft_pack((sr * sa + dr * inv) / 255, (sg * sa + dg * inv) / 255, (sb * sa + db * inv) / 255, min(255, sa + (da * inv) / 255))
}

fn _soft_put(int x, int y, int c) bool {
   if !_soft_buf || x < 0 || y < 0 || x >= _soft_w || y >= _soft_h { return false }
   if _soft_scissor && (x < _soft_sx || y < _soft_sy || x >= _soft_sx + _soft_sw || y >= _soft_sy + _soft_sh) { return false }
   def off = (y * _soft_w + x) * 4
   store32(_soft_buf, _soft_color_blend(load32(_soft_buf, off), c), off)
   true
}

fn _soft_ensure_surface() bool {
   if !_soft_enabled() { return false }
   if _w <= 0 || _h <= 0 { return false }
   def need = _w * _h * 4
   if _soft_buf && _soft_w == _w && _soft_h == _h { return true }
   if _soft_buf { free(_soft_buf) _soft_buf = 0 }
   _soft_buf = zalloc(need)
   _soft_w = _w
   _soft_h = _h
   _soft_buf != 0
}

fn _soft_fill_rect_raw(int x0, int y0, int x1, int y1, int c) bool {
   if !_soft_ensure_surface() { return false }
   if _soft_scissor {
      x0 = max(x0, _soft_sx)
      y0 = max(y0, _soft_sy)
      x1 = min(x1, _soft_sx + _soft_sw)
      y1 = min(y1, _soft_sy + _soft_sh)
   }
   if x0 < 0 { x0 = 0 }
   if y0 < 0 { y0 = 0 }
   if x1 > _soft_w { x1 = _soft_w }
   if y1 > _soft_h { y1 = _soft_h }
   if x1 <= x0 || y1 <= y0 { return true }
   def alpha = _soft_color_chan(c, 24)
   if alpha >= 255 {
      def row_pixels = x1 - x0
      def row_bytes = row_pixels * 4
      def first_off = (y0 * _soft_w + x0) * 4
      mut i = 0
      mut off = first_off
      while i < row_pixels {
         store32(_soft_buf, c, off)
         off += 4
         i += 1
      }
      mut yy = y0 + 1
      while yy < y1 {
         memcpy(_soft_buf + ((yy * _soft_w + x0) * 4), _soft_buf + first_off, row_bytes)
         yy += 1
      }
      return true
   }
   mut y = y0
   while y < y1 {
      mut x = x0
      while x < x1 {
         _soft_put(x, y, c)
         x += 1
      }
      y += 1
   }
   true
}

fn _soft_plot_line_sample(int x, int y, f64 width, int c) bool {
   if width <= 1.25 {
      return _soft_put(x, y, c)
   }
   def radius = max(0.5, (width - 1.0) * 0.5)
   _soft_fill_rect_raw(
      int(float(x) - radius),
      int(float(y) - radius),
      int(float(x) + radius + 1.0),
      int(float(y) + radius + 1.0),
      c
   )
}

fn _soft_clear(int c) bool {
   if !_soft_ensure_surface() { return false }
   def old_sc = _soft_scissor
   _soft_scissor = false
   _soft_fill_rect_raw(0, 0, _soft_w, _soft_h, c)
   _soft_scissor = old_sc
   true
}

fn _soft_rect(f64 x, f64 y, f64 w, f64 h, int c) bool {
   mut x0, y0 = int(x), int(y)
   mut x1, y1 = int(x + w), int(y + h)
   if float(x1) < x + w { x1 += 1 }
   if float(y1) < y + h { y1 += 1 }
   _soft_fill_rect_raw(x0, y0, x1, y1, c)
}

fn _soft_tex_meta(int tex_id) any { _tex_live.get(tex_id, 0) }

fn _soft_sample_tex(int tex_id, f64 u, f64 v) int {
   def meta = _soft_tex_meta(tex_id)
   if !is_dict(meta) { return 0xffffffff }
   def pix = _tex_pixels.get(tex_id, 0)
   if !pix { return 0xffffffff }
   def tw, th = int(meta.get("width", 0)), int(meta.get("height", 0))
   if tw <= 0 || th <= 0 { return 0xffffffff }
   mut uu, vv = u, v
   if uu < 0.0 { uu = 0.0 }
   if vv < 0.0 { vv = 0.0 }
   if uu > 1.0 { uu = 1.0 }
   if vv > 1.0 { vv = 1.0 }
   if int(meta.get("filter", 1)) == 0 || tw == 1 || th == 1 {
      def tx = min(tw - 1, max(0, int(uu * float(tw - 1) + 0.5)))
      def ty = min(th - 1, max(0, int(vv * float(th - 1) + 0.5)))
      return load32(pix, (ty * tw + tx) * 4)
   }
   def fx = uu * float(tw - 1)
   def fy = vv * float(th - 1)
   def x0 = min(tw - 1, max(0, int(floor(fx))))
   def y0 = min(th - 1, max(0, int(floor(fy))))
   def x1 = min(tw - 1, x0 + 1)
   def y1 = min(th - 1, y0 + 1)
   def tx = fx - float(x0)
   def ty = fy - float(y0)
   def c00 = load32(pix, (y0 * tw + x0) * 4)
   def c10 = load32(pix, (y0 * tw + x1) * 4)
   def c01 = load32(pix, (y1 * tw + x0) * 4)
   def c11 = load32(pix, (y1 * tw + x1) * 4)
   _soft_color_lerp2(_soft_color_lerp2(c00, c10, tx), _soft_color_lerp2(c01, c11, tx), ty)
}

fn _soft_mul_color(int a, int b) int {
   _soft_pack(
      (_soft_color_chan(a, 0) * _soft_color_chan(b, 0)) / 255,
      (_soft_color_chan(a, 8) * _soft_color_chan(b, 8)) / 255,
      (_soft_color_chan(a, 16) * _soft_color_chan(b, 16)) / 255,
   (_soft_color_chan(a, 24) * _soft_color_chan(b, 24)) / 255)
}

fn _soft_project(f64 x, f64 y, f64 z) list {
   def m = mat4_mul(_mvp ? _mvp : _default_ortho(_w, _h), _model ? _model : mat4_identity())
   def v = mat4_mul_vec4(m, [x, y, z, 1.0])
   def cw = float(v.get(3, 1.0))
   if abs(cw) < 0.000001 { return [x, y, z, 1.0] }
   def nx, ny, nz = float(v.get(0, 0.0)) / cw, float(v.get(1, 0.0)) / cw, float(v.get(2, 0.0)) / cw
   [(nx * 0.5 + 0.5) * float(_w), (1.0 - (ny * 0.5 + 0.5)) * float(_h), nz, cw]
}

fn _soft_color_lerp3(int c0, int c1, int c2, f64 w0, f64 w1, f64 w2) int {
   _soft_pack(
      int(float(_soft_color_chan(c0, 0)) * w0 + float(_soft_color_chan(c1, 0)) * w1 + float(_soft_color_chan(c2, 0)) * w2),
      int(float(_soft_color_chan(c0, 8)) * w0 + float(_soft_color_chan(c1, 8)) * w1 + float(_soft_color_chan(c2, 8)) * w2),
      int(float(_soft_color_chan(c0, 16)) * w0 + float(_soft_color_chan(c1, 16)) * w1 + float(_soft_color_chan(c2, 16)) * w2),
   int(float(_soft_color_chan(c0, 24)) * w0 + float(_soft_color_chan(c1, 24)) * w1 + float(_soft_color_chan(c2, 24)) * w2))
}

fn _soft_color_lerp2(int c0, int c1, f64 t) int {
   def u = 1.0 - t
   _soft_pack(
      int(float(_soft_color_chan(c0, 0)) * u + float(_soft_color_chan(c1, 0)) * t),
      int(float(_soft_color_chan(c0, 8)) * u + float(_soft_color_chan(c1, 8)) * t),
      int(float(_soft_color_chan(c0, 16)) * u + float(_soft_color_chan(c1, 16)) * t),
   int(float(_soft_color_chan(c0, 24)) * u + float(_soft_color_chan(c1, 24)) * t))
}

fn _soft_active_tex(int tex_id=-1, bool use_material=false) int {
   if tex_id > 0 { return tex_id }
   if use_material && _current_base_tex_id > 0 { return _current_base_tex_id }
   -1
}

fn _soft_vertex_color(any off, bool use_material=false) int {
   if use_material && !_material_uses_vertex_color() { return _current_base_color_u32 }
   load32(off, _OFF_C)
}

fn _soft_draw_tri(any p, int i0, int i1, int i2, int tex_id=-1, bool use_material=false) bool {
   def o0, o1, o2 = p + i0 * _STRIDE, p + i1 * _STRIDE, p + i2 * _STRIDE
   def p0 = _soft_project(load32_f32(o0, _OFF_X), load32_f32(o0, _OFF_X + 4), load32_f32(o0, _OFF_X + 8))
   def p1 = _soft_project(load32_f32(o1, _OFF_X), load32_f32(o1, _OFF_X + 4), load32_f32(o1, _OFF_X + 8))
   def p2 = _soft_project(load32_f32(o2, _OFF_X), load32_f32(o2, _OFF_X + 4), load32_f32(o2, _OFF_X + 8))
   def x0, y0 = float(p0.get(0, 0.0)), float(p0.get(1, 0.0))
   def x1, y1 = float(p1.get(0, 0.0)), float(p1.get(1, 0.0))
   def x2, y2 = float(p2.get(0, 0.0)), float(p2.get(1, 0.0))
   def area = ((x1 - x0) * (y2 - y0)) - ((y1 - y0) * (x2 - x0))
   if abs(area) < 0.000001 { return false }
   mut min_x = int(min(x0, min(x1, x2)))
   mut max_x = int(max(x0, max(x1, x2)) + 1.0)
   mut min_y = int(min(y0, min(y1, y2)))
   mut max_y = int(max(y0, max(y1, y2)) + 1.0)
   if min_x < 0 { min_x = 0 }
   if min_y < 0 { min_y = 0 }
   if max_x > _soft_w { max_x = _soft_w }
   if max_y > _soft_h { max_y = _soft_h }
   def c0, c1, c2 = _soft_vertex_color(o0, use_material), _soft_vertex_color(o1, use_material), _soft_vertex_color(o2, use_material)
   def uv_off = _base_uv_offset()
   def u0, v0 = load32_f32(o0, uv_off), load32_f32(o0, uv_off + 4)
   def u1, v1 = load32_f32(o1, uv_off), load32_f32(o1, uv_off + 4)
   def u2, v2 = load32_f32(o2, uv_off), load32_f32(o2, uv_off + 4)
   mut y = min_y
   while y < max_y {
      mut x = min_x
      while x < max_x {
         def px, py = float(x) + 0.5, float(y) + 0.5
         def e0 = ((x1 - px) * (y2 - py) - (y1 - py) * (x2 - px)) / area
         def e1 = ((x2 - px) * (y0 - py) - (y2 - py) * (x0 - px)) / area
         def e2 = 1.0 - e0 - e1
         if e0 >= -0.00001 && e1 >= -0.00001 && e2 >= -0.00001 {
            mut col = _soft_color_lerp3(c0, c1, c2, e0, e1, e2)
            if tex_id > 0 {
               def tu = u0 * e0 + u1 * e1 + u2 * e2
               def tv = v0 * e0 + v1 * e1 + v2 * e2
               col = _soft_mul_color(col, _soft_sample_tex(tex_id, tu, tv))
            }
            _soft_put(x, y, col)
         }
         x += 1
      }
      y += 1
   }
   true
}

fn _soft_draw_vertices(any p, int count, int tex_id=-1, bool use_material=false) bool {
   if !_soft_ensure_surface() || !p || count <= 0 { return false }
   tex_id = _soft_active_tex(tex_id, use_material)
   mut i = 0
   while i + 2 < count {
      _soft_draw_tri(p, i, i + 1, i + 2, tex_id, use_material)
      i += 3
   }
   true
}

fn _soft_draw_line_segment(any o0, any o1, f64 width=1.0, bool use_material=false) bool {
   if !_soft_ensure_surface() || !o0 || !o1 { return false }
   def p0 = _soft_project(load32_f32(o0, _OFF_X), load32_f32(o0, _OFF_X + 4), load32_f32(o0, _OFF_X + 8))
   def p1 = _soft_project(load32_f32(o1, _OFF_X), load32_f32(o1, _OFF_X + 4), load32_f32(o1, _OFF_X + 8))
   def x0, y0 = float(p0.get(0, 0.0)), float(p0.get(1, 0.0))
   def x1, y1 = float(p1.get(0, 0.0)), float(p1.get(1, 0.0))
   def dx, dy = x1 - x0, y1 - y0
   def steps = max(1, int(max(abs(dx), abs(dy))) + 1)
   def c0, c1 = _soft_vertex_color(o0, use_material), _soft_vertex_color(o1, use_material)
   mut last_x, last_y = -999999, -999999
   mut i = 0
   while i <= steps {
      def t = float(i) / float(steps)
      def x = x0 + dx * t
      def y = y0 + dy * t
      def c = _soft_color_lerp2(c0, c1, t)
      def ix, iy = int(x + 0.5), int(y + 0.5)
      if ix != last_x || iy != last_y {
         _soft_plot_line_sample(ix, iy, width, c)
         last_x, last_y = ix, iy
      }
      i += 1
   }
   true
}

fn _soft_draw_lines(any p, int line_count, f64 width=1.0, bool use_material=false) bool {
   if !_soft_ensure_surface() || !p || line_count <= 0 { return false }
   mut i = 0
   while i < line_count {
      def o0 = p + (i * 2) * _STRIDE
      def o1 = p + (i * 2 + 1) * _STRIDE
      _soft_draw_line_segment(o0, o1, width, use_material)
      i += 1
   }
   true
}

fn _soft_draw_points(any p, int point_count, bool use_material=false) bool {
   if !_soft_ensure_surface() || !p || point_count <= 0 { return false }
   mut i = 0
   while i < point_count {
      def off = p + i * _STRIDE
      def pt = _soft_project(load32_f32(off, _OFF_X), load32_f32(off, _OFF_X + 4), load32_f32(off, _OFF_X + 8))
      _soft_rect(float(pt.get(0, 0.0)) - 1.0, float(pt.get(1, 0.0)) - 1.0, 2.0, 2.0, _soft_vertex_color(off, use_material))
      i += 1
   }
   true
}

fn _soft_index(any idx_base, int i, int index_type=0) int {
   if index_type == 1 { return int(load32(idx_base, i * 4)) }
   int(load16(idx_base, i * 2)) & 65535
}

fn _soft_draw_indexed(any p, int count, any idx_base, int idx_count, int index_type=0, int tex_id=-1, bool is_lines=false, f64 width=1.0, bool is_points=false, bool use_material=false) bool {
   if !_soft_ensure_surface() || !p || !idx_base || idx_count <= 0 { return false }
   tex_id = _soft_active_tex(tex_id, use_material)
   if is_points {
      mut i = 0
      while i < idx_count {
         def vi = _soft_index(idx_base, i, index_type)
         if count <= 0 || (vi >= 0 && vi < count) {
            _soft_draw_points(p + vi * _STRIDE, 1, use_material)
         }
         i += 1
      }
      return true
   }
   if is_lines {
      mut i = 0
      while i + 1 < idx_count {
         def a = _soft_index(idx_base, i, index_type)
         def b = _soft_index(idx_base, i + 1, index_type)
         if count <= 0 || (a >= 0 && b >= 0 && a < count && b < count) {
            _soft_draw_line_segment(p + a * _STRIDE, p + b * _STRIDE, width, use_material)
         }
         i += 2
      }
      return true
   }
   mut i = 0
   while i + 2 < idx_count {
      def a = _soft_index(idx_base, i, index_type)
      def b = _soft_index(idx_base, i + 1, index_type)
      def c = _soft_index(idx_base, i + 2, index_type)
      if count <= 0 || (a >= 0 && b >= 0 && c >= 0 && a < count && b < count && c < count) {
         _soft_draw_tri(p, a, b, c, tex_id, use_material)
      }
      i += 3
   }
   true
}

fn _soft_copy_texture_pixels(int width, int height, any pixels, int format=37) ptr {
   def bytes = width * height * 4
   def cp = malloc(bytes)
   if !cp { return 0 }
   if format == 9 {
      def n = width * height
      mut i = 0
      while i < n {
         def a = load8(pixels, i) & 255
         store32(cp, _soft_pack(255, 255, 255, a), i * 4)
         i += 1
      }
   } else {
      memcpy(cp, pixels, bytes)
   }
   cp
}

fn _create_default_texture(any pixels) int {
   if !pixels { return -1 }
   if _soft_enabled() {
      if _last_tex < 0 { _last_tex = 0 }
      _last_tex += 1
      def id = _last_tex
      def cp = _soft_copy_texture_pixels(1, 1, pixels, 37)
      if !cp { return -1 }
      _tex_live[id] = {"width": 1, "height": 1, "format": 37, "filter": 1}
      _tex_formats[id] = 37
      _tex_pixels[id] = cp
      return id
   }
   def id = _gen_name("glGenTextures")
   if id <= 0 { return -1 }
   _bound_tex = -999999
   _call1("glActiveTexture", GL_TEXTURE0)
   _call2("glBindTexture", GL_TEXTURE_2D, id)
   _bound_tex = id
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT)
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT)
   _call2("glPixelStorei", GL_UNPACK_ALIGNMENT, 1)
   _call9("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels)
   _tex_live[id] = {"width": 1, "height": 1, "format": 37, "filter": 1}
   _tex_formats[id] = 37
   _last_tex = id
   id
}

fn _soft_update_texture_pixels(any dst, int tw, int x, int y, int w, int h, any pixels, int format=37) bool {
   if !dst || !pixels { return false }
   mut yy = 0
   while yy < h {
      if format == 9 {
         mut xx = 0
         while xx < w {
            def a = load8(pixels, yy * w + xx) & 255
            store32(dst, _soft_pack(255, 255, 255, a), (((y + yy) * tw + x + xx) * 4))
            xx += 1
         }
      } else {
         memcpy(dst + (((y + yy) * tw + x) * 4), pixels + (yy * w * 4), w * 4)
      }
      yy += 1
   }
   true
}

fn _ensure_libgl() any {
   if _libgl { return _libgl }
   _libgl = ffi.dlopen_any("GL")
   if !_libgl { _libgl = ffi.dlopen_any("libGL.so.1") }
   _libgl
}

fn _proc(str name) any {
   if _procs.contains(name) { return _procs.get(name, 0) }
   mut p = lib_uiw.get_proc_address(name)
   if !p {
      def h = _ensure_libgl()
      if h { p = ffi.dlsym(h, name) }
   }
   _procs[name] = p
   p
}

fn _has(str name) bool { _proc(name) != 0 }

fn _call_trace_enabled() bool {
   if _call_trace_mode < 0 { _call_trace_mode = common.env_truthy("NY_GL_CALL_TRACE") ? 1 : 0 }
   _call_trace_mode == 1
}

fn _call0(str name) bool {
   def p = _proc(name)
   if !p { return false }
   ffi.call0_void(p)
   true
}

fn _call1(str name, any a) bool {
   if _call_trace_enabled() { print("[gl] call1 " + name) }
   if name == "glActiveTexture" { _ny_glActiveTexture(a) return true }
   if name == "glClear" { _ny_glClear(a) return true }
   if name == "glDepthFunc" { _ny_glDepthFunc(a) return true }
   if name == "glDepthMask" { _ny_glDepthMask(a) return true }
   if name == "glDisable" { _ny_glDisable(a) return true }
   if name == "glEnable" { _ny_glEnable(a) return true }
   if name == "glGenerateMipmap" { _ny_glGenerateMipmap(a) return true }
   if name == "glLoadMatrixf" { _ny_glLoadMatrixf(a) return true }
   if name == "glMatrixMode" { _ny_glMatrixMode(a) return true }
   def p = _proc(name)
   if !p { return false }
   ffi.call1_void(p, a)
   true
}

fn _call1u(str name, any a) bool {
   if _call_trace_enabled() { print("[gl] call1u " + name) }
   def p = _proc(name)
   if !p { return false }
   ffi.call1_u32_void(p, a)
   true
}

fn _call2(str name, any a, any b) bool {
   if _call_trace_enabled() { print("[gl] call2 " + name) }
   if name == "glBindTexture" { _ny_glBindTexture(a, b) return true }
   if name == "glBlendFunc" { _ny_glBlendFunc(a, b) return true }
   if name == "glColorMaterial" { _ny_glColorMaterial(a, b) return true }
   if name == "glPixelStorei" { _ny_glPixelStorei(a, b) return true }
   if name == "glPolygonMode" { _ny_glPolygonMode(a, b) return true }
   def p = _proc(name)
   if !p { return false }
   ffi.call2_void(p, a, b)
   true
}

fn _call3(str name, any a, any b, any c) bool {
   if _call_trace_enabled() { print("[gl] call3 " + name) }
   if name == "glLightfv" { _ny_glLightfv(a, b, c) return true }
   if name == "glTexEnvi" { _ny_glTexEnvi(a, b, c) return true }
   if name == "glTexParameteri" { _ny_glTexParameteri(a, b, c) return true }
   def p = _proc(name)
   if !p { return false }
   ffi.call3_void(p, a, b, c)
   true
}

fn _call4(str name, any a, any b, any c, any d) bool {
   if name == "glViewport" { _ny_glViewport(a, b, c, d) return true }
   def p = _proc(name)
   if !p { return false }
   ffi.call4_void(p, a, b, c, d)
   true
}

fn _call7(str name, any a, any b, any c, any d, any e, any f, any g) bool {
   def p = _proc(name)
   if !p { return false }
   ffi.call7_void(p, a, b, c, d, e, f, g)
   true
}

fn _call9(str name, any a, any b, any c, any d, any e, any f, any g, any h, any i) bool {
   if name == "glTexImage2D" { _ny_glTexImage2D(a, b, c, d, e, f, g, h, i) return true }
   if name == "glTexSubImage2D" { _ny_glTexSubImage2D(a, b, c, d, e, f, g, h, i) return true }
   def p = _proc(name)
   if !p { return false }
   ffi.call9_void(p, a, b, c, d, e, f, g, h, i)
   true
}

fn _call1f(str name, any a) bool {
   def p = _proc(name)
   if !p { return false }
   ffi.call1_f32_void(p, a)
   true
}

fn _call2f(str name, any a, any b) bool {
   def p = _proc(name)
   if !p { return false }
   ffi.call2_f32_void(p, a, b)
   true
}

fn _call3f(str name, any a, any b, any c) bool {
   def p = _proc(name)
   if !p { return false }
   ffi.call3_f32_void(p, a, b, c)
   true
}

fn _call4f(str name, any a, any b, any c, any d) bool {
   if name == "glClearColor" { _ny_glClearColor(a, b, c, d) return true }
   def p = _proc(name)
   if !p { return false }
   ffi.call4_f32_void(p, a, b, c, d)
   true
}

fn _ensure_name_ptr() ptr {
   if !_name_ptr { _name_ptr = zalloc(4) }
   _name_ptr
}

fn _reset_frame_stats() bool {
   _frame_draw_calls = 0
   _frame_dynamic_draw_calls = 0
   _frame_static_draw_calls = 0
   _frame_indexed_draw_calls = 0
   _frame_submitted_vertices = 0
   true
}

fn _publish_frame_stats() bool {
   _last_frame_draw_calls = _frame_draw_calls
   _last_frame_dynamic_draw_calls = _frame_dynamic_draw_calls
   _last_frame_static_draw_calls = _frame_static_draw_calls
   _last_frame_indexed_draw_calls = _frame_indexed_draw_calls
   _last_submitted_vertices = _frame_submitted_vertices
   true
}

fn _record_draw(int vertices, bool static_draw=false, bool indexed_draw=false) bool {
   _frame_draw_calls += 1
   if static_draw { _frame_static_draw_calls += 1 } else { _frame_dynamic_draw_calls += 1 }
   if indexed_draw { _frame_indexed_draw_calls += 1 }
   _frame_submitted_vertices += int(max(0, vertices))
   true
}

fn _gen_name(str fn_name) int {
   def p = _ensure_name_ptr()
   if !p { return 0 }
   store32(p, 0, 0)
   if !_call2(fn_name, 1, p) { return 0 }
   int(load32(p, 0))
}

fn _delete_name(str fn_name, int id) bool {
   if id <= 0 { return false }
   def p = _ensure_name_ptr()
   if !p { return false }
   store32(p, id, 0)
   _call2(fn_name, 1, p)
}

fn _ensure_matrix_buf() ptr {
   if !_matrix_buf { _matrix_buf = zalloc(64) }
   _matrix_buf
}

fn _ensure_light_buf() ptr {
   if !_light_buf { _light_buf = zalloc(64) }
   _light_buf
}

fn _store_light_vec(int off, f64 x, f64 y, f64 z, f64 w) bool {
   def p = _ensure_light_buf()
   if !p { return false }
   store32_f32(p, x, off + 0)
   store32_f32(p, y, off + 4)
   store32_f32(p, z, off + 8)
   store32_f32(p, w, off + 12)
   true
}

fn _load_mat(int mode, any mat) bool {
   _call1("glMatrixMode", mode)
   def p = _ensure_matrix_buf()
   if p && is_list(mat) && render_shared.store_mat4_cm_raw(p, mat, true) {
      _call1("glLoadMatrixf", p)
      return true
   }
   _call0("glLoadIdentity")
}

fn _apply_matrices() bool {
   _load_mat(GL_PROJECTION, _mvp)
   _load_mat(GL_MODELVIEW, _model)
   true
}

fn _uv_xf_identity(int xf0, int xf1) bool {
   xf0 == 0 && band(xf1, 0x3fffffff) == 0
}

fn _decode_uv_offset16(int q) f64 {
   (float(band(q, 0xffff)) / 65535.0) * 16.0 - 8.0
}

fn _decode_uv_scale11(int q) f64 {
   def v = band(q, 2047)
   v == 0 ? 1.0 : (float(v) / 2047.0) * 64.0 - 32.0
}

fn _decode_uv_rot8(int q) f64 {
   (float(band(q, 255)) / 255.0) * (2.0 * PI) - PI
}

fn _load_base_uv_texture_matrix(int xf0, int xf1) bool {
   if _last_texture_xf0 == xf0 && _last_texture_xf1 == xf1 { return true }
   _last_texture_xf0 = xf0
   _last_texture_xf1 = xf1
   _call1("glMatrixMode", GL_TEXTURE)
   if _uv_xf_identity(xf0, xf1) {
      _call0("glLoadIdentity")
      _call1("glMatrixMode", GL_MODELVIEW)
      return true
   }
   def off_x = _decode_uv_offset16(xf0)
   def off_y = _decode_uv_offset16(bshr(xf0, 16))
   def scl_x = _decode_uv_scale11(xf1)
   def scl_y = _decode_uv_scale11(bshr(xf1, 11))
   def rot_bits = band(bshr(xf1, 22), 255)
   def rot = rot_bits == 128 ? 0.0 : _decode_uv_rot8(rot_bits)
   def cr = cos(rot)
   def sr = sin(rot)
   def p = _ensure_matrix_buf()
   if p {
      memset(p, 0, 64)
      ;; Keep GL fallback consistent with the glTF KHR_texture_transform decode.
      store32_f32(p, cr * scl_x, 0)
      store32_f32(p, -sr * scl_x, 4)
      store32_f32(p, sr * scl_y, 16)
      store32_f32(p, cr * scl_y, 20)
      store32_f32(p, 1.0, 40)
      store32_f32(p, off_x, 48)
      store32_f32(p, off_y, 52)
      store32_f32(p, 1.0, 60)
      _call1("glLoadMatrixf", p)
   } else {
      _call0("glLoadIdentity")
   }
   _call1("glMatrixMode", GL_MODELVIEW)
   true
}

fn _default_ortho(int w, int h) list {
   mat4_ortho(0.0, float(max(1, w)), float(max(1, h)), 0.0, -1.0, 1.0)
}

fn _ensure_scratch(int bytes) ptr {
   if bytes <= 0 { return 0 }
   if _scratch && _scratch_cap >= bytes { return _scratch }
   ;; Grow geometrically with realloc instead of free+zalloc. The previous code
   ;; zero-filled the entire scratch buffer on every growth, even though the
   ;; callers (_store_vertex / _store_rect) overwrite every byte before upload.
   ;; realloc can also extend in-place without copying.
   def new_cap = max(bytes, _scratch_cap * 2)
   if _scratch {
      _scratch = realloc(_scratch, new_cap)
   } else {
      _scratch = malloc(new_cap)
   }
   if _scratch { _scratch_cap = new_cap } else { _scratch_cap = 0 }
   _scratch
}

fn _store_vertex(ptr base, int idx, f64 x, f64 y, f64 z, f64 u, f64 v, int color) bool {
   render_shared.store_vertex64(base, idx, x, y, z, u, v, color, 0)
   true
}

fn _draw_mode(bool lines=false, bool points=false) int {
   points ? GL_POINTS : (lines ? GL_LINES : GL_TRIANGLES)
}

fn _filter_value(int filter, bool min_filter=false, bool mips=false) int {
   if mips && min_filter { return filter == 0 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR }
   filter == 0 ? GL_NEAREST : GL_LINEAR
}

fn _format_internal(int fmt) int {
   if fmt == 43 { return GL_SRGB8_ALPHA8 }
   if fmt == 9 { return GL_INTENSITY8 }
   GL_RGBA8
}

fn _format_external(int fmt) int { fmt == 9 ? GL_LUMINANCE : GL_RGBA }

;; Cached GL buffer bindings. draw_vertices / draw_vertices_indexed_raw end
;; every call by binding buffer 0 (defensive unbind), which forces the next
;; draw to re-bind the dynamic VBO. Without this cache that is 2 glBindBuffer
;; calls per draw that the GL driver has to round-trip. We short-circuit when
;; the requested handle already matches the cached binding for the target.
mut _bound_array_buffer = 0
mut _bound_elem_buffer = 0

fn _bind_buffer(int target, any handle) bool {
   def h = int(handle)
   if target == GL_ARRAY_BUFFER {
      if h == _bound_array_buffer { return true }
      _bound_array_buffer = h
   } elif target == GL_ELEMENT_ARRAY_BUFFER {
      if h == _bound_elem_buffer { return true }
      _bound_elem_buffer = h
   }
   _ny_glBindBuffer(target, h)
   true
}

fn _ensure_dynamic_vbo() int {
   if _dynamic_vbo > 0 { return _dynamic_vbo }
   if !_has("glGenBuffers") { return 0 }
   _dynamic_vbo = _gen_name("glGenBuffers")
   _dynamic_vbo
}

fn _upload_dynamic_vertices(any p, int count) bool {
   if !p || count <= 0 { return false }
   def id = _ensure_dynamic_vbo()
   if id <= 0 { return false }
   _bind_buffer(GL_ARRAY_BUFFER, id)
   _ny_glBufferData(GL_ARRAY_BUFFER, count * _STRIDE, p, GL_DYNAMIC_DRAW)
   true
}

fn _gl_refresh_config() bool {
   def trace_mode = common.env_lower("NY_TRACE")
   _cfg_gl_perf = common.env_truthy("NY_TRACE_PERF") || trace_mode == "perf" || trace_mode == "bench" || trace_mode == "fast"
   _cfg_gl_force_base_only = common.env_truthy("NY_GL_FORCE_BASE_ONLY")
   _cfg_gl_lit_textures = !common.env_falsey("NY_GL_LIT_TEXTURES")
   ;; Default to lit textures in GL. The old albedo preview was too flat.
   _cfg_gl_preview_textures = common.env_truthy("NY_GL_PREVIEW_TEXTURES")
   if _cfg_gl_force_base_only {
      _cfg_gl_preview_textures = true
      _cfg_gl_lit_textures = false
   }
   ;; Vertex colors are normal mesh material data, not a debug mode. Default
   ;; them on for glTF parity; NY_GL_VERTEX_COLORS=0 disables them for probes.
   _cfg_gl_vertex_colors = !common.env_falsey("NY_GL_VERTEX_COLORS")
   _cfg_gl_finish_each_draw = common.env_truthy("NY_GL_FINISH_EACH_DRAW")
   true
}

fn _gl_preview_textures() bool { _cfg_gl_preview_textures }

fn _gl_lit_textures() bool { _cfg_gl_lit_textures }

fn _gl_use_vertex_colors() bool { _cfg_gl_vertex_colors }

fn _gl_finish_each_draw() bool { _cfg_gl_finish_each_draw }

fn _base_uv_offset() int {
   band(int(_current_base_uv_xf1), 0x40000000) != 0 ? _OFF_U2 : _OFF_U
}

fn _set_tex_env_mode(int mode) bool {
   if _last_tex_env_mode == mode { return true }
   _ny_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode)
   _last_tex_env_mode = mode
   true
}

fn _set_blend_enabled(bool enabled) bool {
   if enabled == _blend_enabled { return true }
   if enabled { _call1u("glEnable", GL_BLEND) }
   else { _call1u("glDisable", GL_BLEND) }
   _blend_enabled = enabled
   true
}

fn _current_material_needs_blend() bool {
   def alpha_mode = band(int(_current_alpha_u32), 3)
   if alpha_mode == 2 { return true }
   def transmission = band(bshr(int(_current_bsdf0_u32), 16), 255)
   def diffuse_transmission = band(int(_current_bsdf5_u32), 255)
   def refraction = band(bshr(int(_current_bsdf5_u32), 8), 255)
   transmission > 0 || diffuse_transmission > 0 || refraction > 0
}

fn _enable_draw_state(int tex_id=-1, bool depth_write=false) bool {
   ;; tex_id == -2 is the static-buffer sentinel used by mesh draws.  Static
   ;; mesh draws set the current material before calling this function, so the
   ;; active base-color texture must come from _current_base_tex_id.  Without
   ;; this, GL disabled GL_TEXTURE_2D for static meshes and rendered only the
   ;; fixed-function material/lighting color, which made Avocado-style glTFs
   ;; appear as gray bands even when the texture id was valid.
   mut active_tex = tex_id
   if active_tex == -2 && _current_base_tex_id > 0 { active_tex = _current_base_tex_id }
   if active_tex > 0 {
      if !_texture_2d_enabled {
         _ny_glEnable(GL_TEXTURE_2D)
         _texture_2d_enabled = true
      }
      ;; Default textured path is now a cheap albedo preview: no fixed-function
      ;; lighting or vertex-color multiplication.  Use NY_GL_LIT_TEXTURES=1 to
      ;; compare the legacy modulated path.
      def env_mode = (depth_write && _gl_preview_textures()) ? GL_REPLACE : GL_MODULATE
      _set_tex_env_mode(env_mode)
      bind_texture(active_tex)
      _load_base_uv_texture_matrix(_current_base_uv_xf0, _current_base_uv_xf1)
   } else {
      if _texture_2d_enabled {
         _call1("glDisable", GL_TEXTURE_2D)
         _texture_2d_enabled = false
      }
      _bound_tex = -999999
      _load_base_uv_texture_matrix(0, 0)
   }
   ;; UI/text draw paths call with depth_write=false, so keep them out of the
   ;; scene depth buffer completely.  Mesh draw paths pass depth_write=true;
   ;; otherwise GL draws indexed triangles in index order without updating depth,
   ;; so backfaces and darker shell triangles bleed over the albedo texture.
   if depth_write != _depth_test_enabled {
      if depth_write { _call1u("glEnable", GL_DEPTH_TEST) }
      else { _call1u("glDisable", GL_DEPTH_TEST) }
      _depth_test_enabled = depth_write
   }
   ;; UI draws call with depth_write=false and still need normal alpha blending.
   ;; Mesh draws call with depth_write=true; switch GL_BLEND from the active
   ;; material instead of leaving it globally enabled for every opaque object.
   def needs_blend = depth_write ? _current_material_needs_blend() : true
   _set_blend_enabled(needs_blend)
   _set_depth_mask(depth_write && !needs_blend)
   true
}

fn _color_chan(int color_u32, int shift) f64 {
   float((int(color_u32) >> shift) & 255) / 255.0
}

fn _apply_current_color() bool {
   if _current_base_tex_id > 0 && _gl_preview_textures() {
      _call4f("glColor4f", 1.0, 1.0, 1.0, 1.0)
      return true
   }
   def mat = int(_current_material_u32)
   def rough = float((mat >> 8) & 255) / 255.0
   mut shade = 1.0
   if mat != 0x0000ff00 {
      if _current_base_tex_id <= 0 {
         shade = max(0.46, min(1.02, 1.02 - rough * 0.52))
      } else {
         shade = max(0.68, min(1.06, 1.03 - rough * 0.28))
      }
   }
   _call4f("glColor4f",
      _color_chan(_current_base_color_u32, 0) * shade,
      _color_chan(_current_base_color_u32, 8) * shade,
      _color_chan(_current_base_color_u32, 16) * shade,
   _color_chan(_current_base_color_u32, 24))
}

fn _apply_lighting(bool enabled) bool {
   if !enabled {
      _call1u("glDisable", GL_LIGHTING)
      _call1u("glDisable", GL_LIGHT0)
      _call1u("glDisable", GL_COLOR_MATERIAL)
      _lighting_enabled = false
      return true
   }
   def p = _ensure_light_buf()
   if !p { return false }
   _store_light_vec(0, 0.38, 0.38, 0.38, 1.0)
   _store_light_vec(16, 0.65, 0.65, 0.65, 1.0)
   _store_light_vec(32, 0.25, 0.25, 0.25, 1.0)
   _store_light_vec(48, 0.45, 0.75, 1.0, 0.0)
   _call1u("glEnable", GL_NORMALIZE)
   _call1u("glEnable", GL_LIGHTING)
   _call1u("glEnable", GL_LIGHT0)
   _call1u("glEnable", GL_COLOR_MATERIAL)
   _call2("glColorMaterial", GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE)
   _call3("glLightfv", GL_LIGHT0, GL_AMBIENT, p + 0)
   _call3("glLightfv", GL_LIGHT0, GL_DIFFUSE, p + 16)
   _call3("glLightfv", GL_LIGHT0, GL_SPECULAR, p + 32)
   _call3("glLightfv", GL_LIGHT0, GL_POSITION, p + 48)
   _lighting_enabled = true
   true
}

fn _set_depth_mask(bool enabled) bool {
   "Sets glDepthMask to control whether depth buffer writes are enabled."
   if _depth_mask_enabled == enabled { return true }
   _call1("glDepthMask", enabled ? 1 : 0)
   _depth_mask_enabled = enabled
   true
}

fn _material_uses_vertex_color() bool {
   if !_gl_use_vertex_colors() { return false }
   band(int(_current_vc_mode), 1) != 0 || band(int(_current_vc_mode), 4) != 0
}

fn _setup_arrays(any base, bool use_vertex_color=true, bool lit_material=false) bool {
   def material_preview = lit_material && _gl_preview_textures()
   _apply_lighting(lit_material && !material_preview)
   _ny_glEnableClientState(GL_VERTEX_ARRAY)
   if use_vertex_color && !material_preview {
      _ny_glEnableClientState(GL_COLOR_ARRAY)
      _ny_glColorPointer(4, GL_UNSIGNED_BYTE, _STRIDE, base + _OFF_C)
   } else {
      _ny_glDisableClientState(GL_COLOR_ARRAY)
      _apply_current_color()
   }
   _ny_glEnableClientState(GL_TEXTURE_COORD_ARRAY)
   if lit_material && !material_preview {
      _ny_glEnableClientState(GL_NORMAL_ARRAY)
      _ny_glNormalPointer(GL_FLOAT, _STRIDE, base + _OFF_NX)
   } else {
      _ny_glDisableClientState(GL_NORMAL_ARRAY)
   }
   _ny_glVertexPointer(3, GL_FLOAT, _STRIDE, base + _OFF_X)
   _ny_glTexCoordPointer(2, GL_FLOAT, _STRIDE, base + _base_uv_offset())
   true
}

fn _setup_arrays_vbo(any base, bool use_vertex_color=true, bool lit_material=false) bool {
   def bo = int(base)
   def material_preview = lit_material && _gl_preview_textures()
   _apply_lighting(lit_material && !material_preview)
   _ny_glEnableClientState(GL_VERTEX_ARRAY)
   if use_vertex_color && !material_preview {
      _ny_glEnableClientState(GL_COLOR_ARRAY)
      _ny_glColorPointerOffset(4, GL_UNSIGNED_BYTE, _STRIDE, bo + _OFF_C)
   } else {
      _ny_glDisableClientState(GL_COLOR_ARRAY)
      _apply_current_color()
   }
   _ny_glEnableClientState(GL_TEXTURE_COORD_ARRAY)
   if lit_material && !material_preview {
      _ny_glEnableClientState(GL_NORMAL_ARRAY)
      _ny_glNormalPointerOffset(GL_FLOAT, _STRIDE, bo + _OFF_NX)
   } else {
      _ny_glDisableClientState(GL_NORMAL_ARRAY)
   }
   _ny_glVertexPointerOffset(3, GL_FLOAT, _STRIDE, bo + _OFF_X)
   _ny_glTexCoordPointerOffset(2, GL_FLOAT, _STRIDE, bo + _base_uv_offset())
   true
}

fn _draw_immediate_vertices(any p, int count, int mode, bool use_vertex_color=true, bool lit_material=false) bool {
   if !p || count <= 0 { return false }
   _apply_lighting(lit_material)
   _ny_glBegin(mode)
   mut i = 0
   while i < count {
      def off = p + i * _STRIDE
      if use_vertex_color {
         def c = load32(off, _OFF_C)
         _ny_glColor4d(
            float(c & 255) / 255.0,
            float((c >> 8) & 255) / 255.0,
            float((c >> 16) & 255) / 255.0,
         float((c >> 24) & 255) / 255.0)
      } else {
         _apply_current_color()
      }
      def uv_off = _base_uv_offset()
      _ny_glTexCoord2d(float(load32_f32(off, uv_off)), float(load32_f32(off, uv_off + 4)))
      _ny_glNormal3d(float(load32_f32(off, _OFF_NX)), float(load32_f32(off, _OFF_NX + 4)), float(load32_f32(off, _OFF_NX + 8)))
      _ny_glVertex3d(float(load32_f32(off, _OFF_X)), float(load32_f32(off, _OFF_X + 4)), float(load32_f32(off, _OFF_X + 8)))
      i += 1
   }
   _ny_glEnd()
   true
}

fn _norm_i32(int v) int {
   if v > 2147483647 { return v - 4294967296 }
   v
}

;; Updates the unlit and returns the resulting state.
fn set_unlit(any enabled) any {
   _current_unlit = !!enabled
   0
}

;; Updates the vertex color mode and returns the resulting state.
fn set_vertex_color_mode(int mode) any {
   _current_vc_mode = int(mode)
   0
}

;; Updates the material and returns the resulting state.
fn set_material(any base_color, any metallic, any roughness) any {
   metallic
   roughness
   _current_base_color_u32 = render_shared.color_u32(base_color)
   _current_material_u32 = bor(band(int(float(metallic) * 255.0), 255), bshl(band(int(float(roughness) * 255.0), 255), 8))
   _current_base_tex_id = -1
   _current_alpha_u32 = 0
   _current_bsdf0_u32 = 0
   _current_bsdf5_u32 = 0
   _current_vc_mode = 0
   _current_base_uv_xf0 = 0
   _current_base_uv_xf1 = 0
   0
}

;; Updates the ui material and returns the resulting state.
fn set_ui_material(int base_tex_id=-1, int alpha_u32=0, int vc_mode=12) any {
   _current_base_color_u32 = 0xffffffff
   _current_material_u32 = 0x0000ff00
   _current_unlit = true
   _current_base_tex_id = _norm_i32(base_tex_id)
   if _current_base_tex_id <= 0 { _current_base_tex_id = -1 }
   _current_alpha_u32 = alpha_u32
   _current_bsdf0_u32 = 0
   _current_bsdf5_u32 = 0
   _current_vc_mode = int(vc_mode)
   _current_base_uv_xf0 = 0
   _current_base_uv_xf1 = 0
   if _current_base_tex_id > 0 { bind_texture(_current_base_tex_id) }
   0
}

fn _reset_ui_draw_state() bool {
   set_ui_material(-1, 0, 12)
   true
}

;; Updates the material packed and returns the resulting state.
fn set_material_packed(
   int base_color_u32, int material_u32, int emissive_u32 = 0, int emissive_tex_id = -1,
   int emissive_uv_set = 0, int base_tex_id = -1, int alpha_u32 = 0, int occlusion_tex_id = -1,
   int occlusion_uv_set = 0, int bsdf0_u32 = 0, int bsdf1_u32 = 0, int bsdf2_u32 = 0,
   int bsdf3_u32 = 0, int bsdf4_u32 = 0, int bsdf5_u32 = 0, int base_uv_xf0 = 0,
   int base_uv_xf1 = 0, int normal_uv_xf0 = 0, int normal_uv_xf1 = 0, int mr_uv_xf0 = 0,
   int mr_uv_xf1 = 0, int occlusion_uv_xf0 = 0, int occlusion_uv_xf1 = 0,
   int emissive_uv_xf0 = 0, int emissive_uv_xf1 = 0, int normal_tex_id = -1,
   int ext2_tex_word = 0x80000000, int vc_mode = 0
) any {
   material_u32
   emissive_u32
   emissive_tex_id
   emissive_uv_set
   occlusion_tex_id
   occlusion_uv_set
   bsdf0_u32
   bsdf1_u32
   bsdf2_u32
   bsdf3_u32
   bsdf4_u32
   bsdf5_u32
   normal_uv_xf0
   normal_uv_xf1
   mr_uv_xf0
   mr_uv_xf1
   occlusion_uv_xf0
   occlusion_uv_xf1
   emissive_uv_xf0
   emissive_uv_xf1
   normal_tex_id
   ext2_tex_word
   _current_base_color_u32 = base_color_u32
   _current_material_u32 = material_u32
   _current_base_tex_id = _norm_i32(base_tex_id)
   if _current_base_tex_id <= 0 { _current_base_tex_id = -1 }
   _current_alpha_u32 = alpha_u32
   _current_bsdf0_u32 = bsdf0_u32
   _current_bsdf5_u32 = bsdf5_u32
   _current_vc_mode = int(vc_mode)
   _current_base_uv_xf0 = base_uv_xf0
   _current_base_uv_xf1 = base_uv_xf1
   if _current_base_tex_id > 0 { bind_texture(_current_base_tex_id) }
   0
}

;; Updates the material from slab and returns the resulting state.
fn set_material_from_slab(?ptr p, int vc_mode=0) any {
   if !p { return 0 }
   set_material_packed(
      load32(p, 0),
      load32(p, 4),
      load32(p, 8),
      load32(p, 12),
      load32(p, 16),
      load32(p, 20),
      load32(p, 24),
      load32(p, 28),
      load32(p, 32),
      load32(p, 36),
      load32(p, 40),
      load32(p, 44),
      load32(p, 48),
      ;; render.utils.VkrMaterialSlab pack(4) layout:
      ;; mesh ptr at 104, model ptr at 112, flags at 120/128/140,
      ;; bsdf4/bsdf5/ext2 at 144/148/152.  Do not use the scene GPU-part
      ;; slab offsets here; this function receives a standalone material slab.
      load32(p, 144),
      load32(p, 148),
      load32(p, 52),
      load32(p, 56),
      load32(p, 60),
      load32(p, 64),
      load32(p, 68),
      load32(p, 72),
      load32(p, 76),
      load32(p, 80),
      load32(p, 84),
      load32(p, 88),
      load32(p, 100),
      load32(p, 152),
      vc_mode
   )
}

;; Updates the material from slab base and returns the resulting state.
fn set_material_from_slab_base(?ptr p, int fallback_base_tex_id=-1, int vc_mode=0) any {
   if !p { return 0 }
   ;; Some mesh records keep the correct base texture on the mesh/vertex path
   ;; while an older material slab can carry -1/0.  Preserve every material word
   ;; from the slab, but allow the caller to override only the base-color texture.
   mut base_tex_id = _norm_i32(load32(p, 20))
   def fb = _norm_i32(fallback_base_tex_id)
   if base_tex_id <= 0 && fb > 0 { base_tex_id = fb }
   set_material_packed(
      load32(p, 0),
      load32(p, 4),
      load32(p, 8),
      load32(p, 12),
      load32(p, 16),
      base_tex_id,
      load32(p, 24),
      load32(p, 28),
      load32(p, 32),
      load32(p, 36),
      load32(p, 40),
      load32(p, 44),
      load32(p, 48),
      load32(p, 144),
      load32(p, 148),
      load32(p, 52),
      load32(p, 56),
      load32(p, 60),
      load32(p, 64),
      load32(p, 68),
      load32(p, 72),
      load32(p, 76),
      load32(p, 80),
      load32(p, 84),
      load32(p, 88),
      load32(p, 100),
      load32(p, 152),
      vc_mode
   )
}

fn init(any win) bool {
   "Initializes a real OpenGL renderer for a window."
   if !win { return false }
   _gl_refresh_config()
   _win = win
   _w = int(win.get("w", 0))
   _h = int(win.get("h", 0))
   if _w <= 0 || _h <= 0 {
      def fb = lib_uiw.get_framebuffer_size(win)
      _w, _h = int(fb.get(0, 0)), int(fb.get(1, 0))
   }
   if _soft_enabled() {
      _mvp = _default_ortho(_w, _h)
      _model = mat4_identity()
      set_ui_material(-1, 0, 12)
      _soft_ensure_surface()
      if !_default_tex {
         def px = zalloc(4)
         if px {
            store32(px, 0xffffffff, 0)
            _default_tex = _create_default_texture(px)
            free(px)
         }
      }
      return true
   }
   if !lib_uiw.make_current(win) { return false }
   if _cfg_gl_perf {
      lib_uiw.set_window_vsync(false)
      ui_backend.swap_interval(0)
   }
   if common.env_present("NY_GL_SWAP_INTERVAL") {
      def interval = common.env_int_clamped("NY_GL_SWAP_INTERVAL", 0, 0, 4)
      lib_uiw.set_window_vsync(interval > 0)
      ui_backend.swap_interval(interval)
   }
   if !_has("glClear") || !_has("glDrawArrays") || !_has("glVertexPointer") || !_has("glTexImage2D") {
      shutdown()
      return false
   }
   _mvp = _default_ortho(_w, _h)
   _model = mat4_identity()
   _apply_matrices()
   _call1("glDisable", GL_CULL_FACE)
   _call1("glEnable", GL_BLEND)
   _blend_enabled = true
   if _has("glBlendFuncSeparate") {
      _call4("glBlendFuncSeparate", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
   } else {
      _call2("glBlendFunc", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
   }
   _ny_glEnable(GL_TEXTURE_2D)
   _set_tex_env_mode(GL_MODULATE)
   _call1("glEnable", GL_DEPTH_TEST)
   _call1("glDepthFunc", GL_LEQUAL)
   _call1("glEnable", GL_MULTISAMPLE)
   _multisample_enabled = true
   _call2("glPixelStorei", GL_UNPACK_ALIGNMENT, 1)
   _check_gl_error("init.state")
   set_ui_material(-1, 0, 12)
   if !_default_tex {
      def px = zalloc(4)
      if px {
         store32(px, 0xffffffff, 0)
         _default_tex = _create_default_texture(px)
         free(px)
         _check_gl_error("init.default_tex")
      }
   }
   true
}

fn shutdown() bool {
   "Shuts down OpenGL backend state held by Nytrix."
   if _win && !_soft_enabled() { lib_uiw.make_current(_win) }
   def keys = dict_keys(_tex_live)
   mut i = 0
   while i < keys.len {
      def k = int(keys[i])
      if !_soft_enabled() && k > 0 { _delete_name("glDeleteTextures", k) }
      i += 1
   }
   _tex_live = dict(128)
   _tex_formats = dict(128)
   _default_tex = 0
   _bound_tex = -999999
   _lighting_enabled = false
   _multisample_enabled = false
   _last_tex_env_mode = -999999
   if _dynamic_vbo > 0 { _delete_name("glDeleteBuffers", _dynamic_vbo) _dynamic_vbo = 0 }
   if _scratch { free(_scratch) _scratch = 0 _scratch_cap = 0 }
   if _soft_buf { free(_soft_buf) _soft_buf = 0 _soft_w = 0 _soft_h = 0 }
   def pkeys = dict_keys(_tex_pixels)
   mut pi = 0
   while pi < pkeys.len {
      def pp = _tex_pixels.get(pkeys[pi], 0)
      if pp { free(pp) }
      pi += 1
   }
   _tex_pixels = dict(128)
   if _matrix_buf { free(_matrix_buf) _matrix_buf = 0 }
   if _light_buf { free(_light_buf) _light_buf = 0 }
   if _name_ptr { free(_name_ptr) _name_ptr = 0 }
   if _libgl { ffi.dlclose(_libgl) _libgl = 0 }
   _procs = dict(128)
   _frame_open = false
   _win = 0
   if !_soft_enabled() { lib_uiw.make_current(0) }
   true
}

fn _gl_string(int name) str {
   def s = _ny_glGetString(name)
   s ? core_str.cstr_to_str(s) : ""
}

fn capabilities() dict {
   "Returns native OpenGL backend capabilities."
   {
      "opengl": true,
      "gpu_primitives": true,
      "gpu_buffers": _has("glGenBuffers") && _has("glBufferData"),
      "gpu_textures": _has("glGenTextures") && _has("glTexImage2D"),
      "double_buffered": true,
      "software_upload": _soft_enabled(),
      "api_backend": "opengl",
      "renderer": _gl_string(0x1F01),
      "vendor": _gl_string(0x1F00),
      "version": _gl_string(0x1F02)
   }
}

fn begin_frame(any win=0, int w=0, int h=0) bool {
   "Begins an OpenGL frame."
   if win { _win = win }
   if !_win { return false }
   if !_soft_enabled() && !lib_uiw.make_current(_win) { return false }
   if w <= 0 || h <= 0 {
      def sz = _soft_enabled() ? lib_uiw.size(_win) : lib_uiw.get_framebuffer_size(_win)
      w, h = int(sz.get(0, _w)), int(sz.get(1, _h))
   }
   if w > 0 { _w = w }
   if h > 0 { _h = h }
   _reset_frame_stats()
   if _soft_enabled() {
      if !_mvp { _mvp = _default_ortho(_w, _h) }
      if !_model { _model = mat4_identity() }
      _soft_ensure_surface()
      if !_next_frame_load_color {
         _soft_clear(_soft_pack(int(_clear_r * 255.0), int(_clear_g * 255.0), int(_clear_b * 255.0), int(_clear_a * 255.0)))
      }
      _next_frame_load_color = false
      _frame_open = true
      return true
   }
   _call4("glViewport", 0, 0, _w, _h)
   _call1("glEnable", GL_MULTISAMPLE)
   _multisample_enabled = true
   if !_mvp { _mvp = _default_ortho(_w, _h) }
   if !_model { _model = mat4_identity() }
   _apply_matrices()
   if _wireframe { _call2("glPolygonMode", GL_FRONT_AND_BACK, GL_LINE) }
   else { _call2("glPolygonMode", GL_FRONT_AND_BACK, GL_FILL) }
   _set_depth_mask(true)
   if _next_frame_load_color {
      _call1("glClear", GL_DEPTH_BUFFER_BIT)
   } else {
      _call4f("glClearColor", _clear_r, _clear_g, _clear_b, _clear_a)
      _call1("glClear", GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
   }
   _next_frame_load_color = false
   _frame_open = true
   true
}

fn end_frame() bool {
   "Presents the OpenGL backbuffer."
   if !_win { return false }
   _publish_frame_stats()
   if _soft_enabled() {
      if _soft_buf { lib_uiw.blit_software(_win, _soft_buf, _soft_w, _soft_h) }
      _frame_open = false
      return true
   }
   lib_uiw.swap_buffers(_win)
   _frame_open = false
   true
}

;; Returns true when notify window resize.
fn notify_window_resize(int w, int h) bool {
   if w > 0 { _w = w }
   if h > 0 { _h = h }
   if _soft_enabled() { return true }
   _call4("glViewport", 0, 0, _w, _h)
   true
}

;; Returns the swapchain width.
fn get_swapchain_width() int { _w }

;; Returns the swapchain height.
fn get_swapchain_height() int { _h }

;; Returns the swapchain image count.
fn get_swapchain_image_count() int { 2 }

;; Returns the result of the `renderer_vertex_offset` operation.
fn renderer_vertex_offset() int { _frame_submitted_vertices * _STRIDE }

;; Returns the result of the `frame_stats` operation.
fn frame_stats() dict {
   {
      "draws": _last_frame_draw_calls,
      "dynamic_draws": _last_frame_dynamic_draw_calls,
      "static_draws": _last_frame_static_draw_calls,
      "indexed_draws": _last_frame_indexed_draw_calls,
      "current_draws": _frame_draw_calls,
      "current_dynamic_draws": _frame_dynamic_draw_calls,
      "current_static_draws": _frame_static_draw_calls,
      "current_indexed_draws": _frame_indexed_draw_calls,
      "flushes": 0,
      "pipeline_binds": 0,
      "descriptor_binds": 0,
      "submitted_vertices": _last_submitted_vertices,
      "begin_ms": 0.0,
      "syncpc_ms": 0.0,
      "flush_ms": 0.0,
      "end_ms": 0.0,
      "cpu_ms": 0.0,
      "prim_rect_quads": 0,
      "prim_outline_quads": 0,
      "prim_line_quads": 0,
      "prim_raw_lines": 0,
      "prim_raw_points": 0,
      "prim_text_calls": 0,
      "prim_text_glyphs": 0,
   }
}

fn set_clear_color(f64 r, f64 g, f64 b, f64 a=1.0) bool {
   "Sets the OpenGL clear color."
   _clear_r, _clear_g = float(r), float(g)
   _clear_b, _clear_a = float(b), float(a)
   if _soft_enabled() { return true }
   _call4f("glClearColor", _clear_r, _clear_g, _clear_b, _clear_a)
}

fn set_next_frame_load_color(any enabled) bool {
   "Requests that begin_frame preserves the current color buffer once."
   _next_frame_load_color = !!enabled
   true
}

fn clear(f64 r, f64 g, f64 b, f64 a=1.0) bool {
   "Clears the active OpenGL framebuffer with color and depth."
   set_clear_color(r, g, b, a)
   if _soft_enabled() { return _soft_clear(_soft_pack(int(float(r) * 255.0), int(float(g) * 255.0), int(float(b) * 255.0), int(float(a) * 255.0))) }
   _set_depth_mask(true)
   _call1("glClear", GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
}

fn clear_depth() bool {
   "Clears the OpenGL depth buffer."
   if _soft_enabled() { return true }
   _set_depth_mask(true)
   _call1("glClear", GL_DEPTH_BUFFER_BIT)
}

;; Returns true when set mvp.
fn set_mvp(any mat) bool {
   _mvp = mat
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

;; Returns true when set model matrix.
fn set_model_matrix(any mat) bool {
   _model = mat
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

;; Returns true when set ortho.
fn set_ortho(f64 l, f64 r, f64 b, f64 t, f64 n, f64 f) bool {
   _mvp = mat4_ortho(l, r, b, t, n, f)
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

;; Returns true when set perspective.
fn set_perspective(f64 fovy, f64 aspect, f64 near, f64 far) bool {
   _mvp = mat4_perspective(fovy, aspect, near, far)
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

fn set_scissor_rect(int x, int y, int w, int h) bool {
   "Applies a top-left-origin scissor rectangle."
   mut sx, sy, sw, sh = x, y, w, h
   if sx < 0 { sw += sx sx = 0 }
   if sy < 0 { sh += sy sy = 0 }
   if sw < 0 { sw = 0 }
   if sh < 0 { sh = 0 }
   if sx + sw > _w { sw = max(0, _w - sx) }
   if sy + sh > _h { sh = max(0, _h - sy) }
   if _soft_enabled() {
      _soft_scissor = true
      _soft_sx, _soft_sy, _soft_sw, _soft_sh = sx, sy, sw, sh
      return true
   }
   _ny_glEnable(GL_SCISSOR_TEST)
   _ny_glScissor(sx, max(0, _h - sy - sh), sw, sh)
}

fn reset_scissor_rect() bool {
   "Disables the OpenGL scissor test."
   if _soft_enabled() {
      _soft_scissor = false
      return true
   }
   _ny_glDisable(GL_SCISSOR_TEST)
}

;; Returns true when set wireframe.
fn set_wireframe(bool enabled) bool {
   _wireframe = !!enabled
   if _soft_enabled() { return true }
   if _wireframe { _call2("glPolygonMode", GL_FRONT_AND_BACK, GL_LINE) }
   else { _call2("glPolygonMode", GL_FRONT_AND_BACK, GL_FILL) }
   true
}

fn set_mesh_raster_state(bool nocull=false, bool flip_winding=false) bool {
   "Applies mesh culling and front-face state for GL mesh draws."
   if _soft_enabled() { return true }
   def want_cull = !nocull
   if _mesh_cull_enabled != want_cull {
      if want_cull { _ny_glEnable(GL_CULL_FACE) }
      else { _ny_glDisable(GL_CULL_FACE) }
      _mesh_cull_enabled = want_cull
   }
   def face = flip_winding ? GL_CW : GL_CCW
   if _mesh_front_face != face {
      _ny_glFrontFace(face)
      _mesh_front_face = face
   }
   true
}

fn bind_texture(int tex_id) bool {
   "Binds an OpenGL texture id."
   if _soft_enabled() {
      _bound_tex = tex_id
      return true
   }
   if tex_id <= 0 { return bind_default_texture() }
   if _bound_tex == tex_id { return true }
   _call1("glActiveTexture", GL_TEXTURE0)
   _call2("glBindTexture", GL_TEXTURE_2D, tex_id)
   _bound_tex = tex_id
   true
}

;; Returns true when bind default texture.
fn bind_default_texture() bool {
   if _soft_enabled() {
      _bound_tex = _default_tex
      return true
   }
   if _default_tex > 0 { return bind_texture(_default_tex) }
   _call2("glBindTexture", GL_TEXTURE_2D, 0)
   _bound_tex = 0
   true
}
