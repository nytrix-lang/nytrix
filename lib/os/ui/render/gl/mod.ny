;; Keywords: render opengl gl backend gpu texture mesh os ui
;; OpenGL renderer backend. Uses real OpenGL draw calls, buffers, textures, scissor, and swap.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.shared
;; - std.os.ui.window
module std.os.ui.render.gl(
   init, shutdown, capabilities, begin_frame, end_frame, notify_window_resize,
   get_swapchain_width, get_swapchain_height, get_swapchain_image_count,
   set_clear_color, set_next_frame_load_color, clear, clear_depth, set_mvp, set_model_matrix, set_ortho, set_perspective,
   set_scissor_rect, reset_scissor_rect, set_wireframe, set_mesh_raster_state,
   renderer_vertex_offset, frame_stats,
   draw_rect, draw_rect_fast, draw_rect_outline_fast, draw_rects_fast_ptr, draw_lines_2d_fast_ptr, draw_rect_tex, draw_rect_tex_uv,
   draw_rect_tex_uv_rot, draw_line, draw_line_fast, draw_vertices, draw_vertices_indexed_raw,
   draw_lines_raw, draw_points_raw, draw_line_3d, draw_triangle_3d, draw_quad_3d,
   draw_glyph_bitmap_scaled,
   set_unlit, set_vertex_color_mode, set_material, set_material_packed, set_material_from_slab, set_material_from_slab_base, set_ui_material,
   create_texture, create_texture_ex, update_texture_rect, bind_texture, bind_default_texture, destroy_texture,
   texture_size, texture_format, texture_count, last_created_texture_id, read_framebuffer,
   create_static_buffer, create_static_index_buffer, create_static_indexed_buffer,
   destroy_static_buffer, draw_static_buffer, draw_static_buffer_raw,
   draw_static_buffer_indexed, draw_static_buffer_indexed_raw
)

use std.core
use std.core.common as common
use std.math
use std.os.ffi as ffi
use std.os.ui.window as lib_uiw
use std.os.ui.render.matrix
use std.os.ui.render.shared as render_shared

#linux {
   #link "libGL.so"
   #include <GL/gl.h>
   extern "" {
      fn _ny_glReadPixels(i32 x, i32 y, i32 width, i32 height, u32 format, u32 typ, ptr pixels) as "glReadPixels"
      fn _ny_glReadBuffer(u32 mode) as "glReadBuffer"
      fn _ny_glFinish() as "glFinish"
      fn _ny_glEnable(u32 cap) as "glEnable"
      fn _ny_glDisable(u32 cap) as "glDisable"
      fn _ny_glScissor(i32 x, i32 y, i32 width, i32 height) as "glScissor"
      fn _ny_glDepthFunc(u32 func) as "glDepthFunc"
      fn _ny_glFrontFace(u32 mode) as "glFrontFace"
      fn _ny_glBlendFunc(u32 sfactor, u32 dfactor) as "glBlendFunc"
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
   }
} #else {
   fn _ny_glReadPixels(int _x, int _y, int _width, int _height, int _format, int _type, any _pixels) any { nil }
   fn _ny_glReadBuffer(int _mode) any { nil }
   fn _ny_glFinish() any { nil }
   fn _ny_glEnable(int _cap) any { nil }
   fn _ny_glDisable(int _cap) any { nil }
   fn _ny_glScissor(int _x, int _y, int _width, int _height) any { nil }
   fn _ny_glDepthFunc(int _func) any { nil }
   fn _ny_glFrontFace(int _mode) any { nil }
   fn _ny_glBlendFunc(int _sfactor, int _dfactor) any { nil }
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
   fn glColor4d(f64 _r, f64 _g, f64 _b, f64 _a) any { nil }
   fn glColor4ub(u32 _r, u32 _g, u32 _b, u32 _a) any { nil }
   fn glRectd(f64 _x1, f64 _y1, f64 _x2, f64 _y2) any { nil }
   fn glRecti(i32 _x1, i32 _y1, i32 _x2, i32 _y2) any { nil }
} #endif
def GL_FALSE = 0
def GL_TRUE = 1
def GL_POINTS = 0x0000
def GL_LINES = 0x0001
def GL_TRIANGLES = 0x0004
def GL_DEPTH_BUFFER_BIT = 0x00000100
def GL_COLOR_BUFFER_BIT = 0x00004000
def GL_FRONT_AND_BACK = 0x0408
def GL_FRONT = 0x0404
def GL_BACK = 0x0405
def GL_CW = 0x0900
def GL_CCW = 0x0901
def GL_CULL_FACE = 0x0B44
def GL_DEPTH_TEST = 0x0B71
def GL_LIGHTING = 0x0B50
def GL_LIGHT0 = 0x4000
def GL_COLOR_MATERIAL = 0x0B57
def GL_NORMALIZE = 0x0BA1
def GL_MULTISAMPLE = 0x809D
def GL_BLEND = 0x0BE2
def GL_SCISSOR_TEST = 0x0C11
def GL_LINE = 0x1B01
def GL_FILL = 0x1B02
def GL_TEXTURE_2D = 0x0DE1
def GL_TEXTURE_ENV = 0x2300
def GL_TEXTURE_ENV_MODE = 0x2200
def GL_MODULATE = 0x2100
def GL_REPLACE = 0x1e01
def GL_UNPACK_ALIGNMENT = 0x0CF5
def GL_PROJECTION = 0x1701
def GL_MODELVIEW = 0x1700
def GL_TEXTURE = 0x1702
def GL_RGBA = 0x1908
def GL_LUMINANCE = 0x1909
def GL_INTENSITY8 = 0x804B
def GL_AMBIENT = 0x1200
def GL_DIFFUSE = 0x1201
def GL_SPECULAR = 0x1202
def GL_POSITION = 0x1203
def GL_AMBIENT_AND_DIFFUSE = 0x1602
def GL_RGBA8 = 0x8058
def GL_SRGB8_ALPHA8 = 0x8C43
def GL_UNSIGNED_BYTE = 0x1401
def GL_UNSIGNED_SHORT = 0x1403
def GL_UNSIGNED_INT = 0x1405
def GL_FLOAT = 0x1406
def GL_NEAREST = 0x2600
def GL_LINEAR = 0x2601
def GL_NEAREST_MIPMAP_NEAREST = 0x2700
def GL_LINEAR_MIPMAP_LINEAR = 0x2703
def GL_REPEAT = 0x2901
def GL_CLAMP_TO_EDGE = 0x812F
def GL_TEXTURE_MAG_FILTER = 0x2800
def GL_TEXTURE_MIN_FILTER = 0x2801
def GL_TEXTURE_WRAP_S = 0x2802
def GL_TEXTURE_WRAP_T = 0x2803
def GL_TEXTURE0 = 0x84C0
def GL_ARRAY_BUFFER = 0x8892
def GL_ELEMENT_ARRAY_BUFFER = 0x8893
def GL_STATIC_DRAW = 0x88E4
def GL_DYNAMIC_DRAW = 0x88E8
def GL_VERTEX_ARRAY = 0x8074
def GL_NORMAL_ARRAY = 0x8075
def GL_COLOR_ARRAY = 0x8076
def GL_TEXTURE_COORD_ARRAY = 0x8078
def GL_SRC_ALPHA = 0x0302
def GL_ONE_MINUS_SRC_ALPHA = 0x0303
def GL_LEQUAL = 0x0203
def _STRIDE = render_shared.VERTEX_STRIDE
def _OFF_X = render_shared.OFF_X
def _OFF_U = render_shared.OFF_U
def _OFF_U2 = render_shared.OFF_U2
def _OFF_C = render_shared.OFF_C
def _OFF_NX = render_shared.OFF_NX
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
   def p = _proc(name)
   if !p { return false }
   ffi.call2_void(p, a, b)
   true
}

fn _call3(str name, any a, any b, any c) bool {
   if _call_trace_enabled() { print("[gl] call3 " + name) }
   def p = _proc(name)
   if !p { return false }
   ffi.call3_void(p, a, b, c)
   true
}

fn _call4(str name, any a, any b, any c, any d) bool {
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
   if _scratch { free(_scratch) _scratch = 0 _scratch_cap = 0 }
   _scratch = zalloc(bytes)
   if _scratch { _scratch_cap = bytes }
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

fn _bind_buffer(int target, any handle) bool {
   _ny_glBindBuffer(target, int(handle))
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
   _cfg_gl_vertex_colors = common.env_truthy("NY_GL_VERTEX_COLORS")
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
      _ny_glEnable(GL_TEXTURE_2D)
      ;; Default textured path is now a cheap albedo preview: no fixed-function
      ;; lighting or vertex-color multiplication.  Use NY_GL_LIT_TEXTURES=1 to
      ;; compare the legacy modulated path.
      def env_mode = (depth_write && _gl_preview_textures()) ? GL_REPLACE : GL_MODULATE
      _set_tex_env_mode(env_mode)
      bind_texture(active_tex)
      _load_base_uv_texture_matrix(_current_base_uv_xf0, _current_base_uv_xf1)
   } else {
      _call1("glDisable", GL_TEXTURE_2D)
      _bound_tex = -999999
      _load_base_uv_texture_matrix(0, 0)
   }
   ;; UI/text draw paths call with depth_write=false, so keep them out of the
   ;; scene depth buffer completely.  Mesh draw paths pass depth_write=true;
   ;; otherwise GL draws indexed triangles in index order without updating depth,
   ;; so backfaces and darker shell triangles bleed over the albedo texture.
   if depth_write { _call1u("glEnable", GL_DEPTH_TEST) }
   else { _call1u("glDisable", GL_DEPTH_TEST) }
   _set_depth_mask(depth_write)
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

fn set_unlit(any enabled) any {
   _current_unlit = !!enabled
   0
}

fn set_vertex_color_mode(int mode) any {
   _current_vc_mode = int(mode)
   0
}

fn set_material(any base_color, any metallic, any roughness) any {
   metallic
   roughness
   _current_base_color_u32 = render_shared.color_u32(base_color)
   _current_material_u32 = bor(band(int(float(metallic) * 255.0), 255), bshl(band(int(float(roughness) * 255.0), 255), 8))
   _current_base_tex_id = -1
   _current_alpha_u32 = 0
   _current_vc_mode = 0
   _current_base_uv_xf0 = 0
   _current_base_uv_xf1 = 0
   0
}

fn set_ui_material(int base_tex_id=-1, int alpha_u32=0, int vc_mode=12) any {
   _current_base_color_u32 = 0xffffffff
   _current_material_u32 = 0x0000ff00
   _current_unlit = true
   _current_base_tex_id = _norm_i32(base_tex_id)
   if _current_base_tex_id <= 0 { _current_base_tex_id = -1 }
   _current_alpha_u32 = alpha_u32
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
   _current_vc_mode = int(vc_mode)
   _current_base_uv_xf0 = base_uv_xf0
   _current_base_uv_xf1 = base_uv_xf1
   if _current_base_tex_id > 0 { bind_texture(_current_base_tex_id) }
   0
}

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
            _default_tex = create_texture(1, 1, px)
            free(px)
         }
      }
      return true
   }
   if !lib_uiw.make_current(win) { return false }
   if _cfg_gl_perf { lib_uiw.set_window_vsync(false) }
   if common.env_present("NY_GL_SWAP_INTERVAL") { lib_uiw.set_window_vsync(common.env_int_clamped("NY_GL_SWAP_INTERVAL", 0, 0, 1) > 0) }
   if !_has("glClear") || !_has("glDrawArrays") || !_has("glVertexPointer") || !_has("glTexImage2D") {
      shutdown()
      return false
   }
   _mvp = _default_ortho(_w, _h)
   _model = mat4_identity()
   _apply_matrices()
   _call1("glDisable", GL_CULL_FACE)
   _call1("glEnable", GL_BLEND)
   _call2("glBlendFunc", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
   _ny_glEnable(GL_TEXTURE_2D)
   _set_tex_env_mode(GL_MODULATE)
   _call1("glEnable", GL_DEPTH_TEST)
   _call1("glDepthFunc", GL_LEQUAL)
   _call1("glEnable", GL_MULTISAMPLE)
   _call2("glPixelStorei", GL_UNPACK_ALIGNMENT, 1)
   set_ui_material(-1, 0, 12)
   if !_default_tex {
      def px = zalloc(4)
      if px {
         store32(px, 0xffffffff, 0)
         _default_tex = create_texture(1, 1, px)
         free(px)
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

fn capabilities() dict {
   "Returns native OpenGL backend capabilities."
   {
      "opengl": true,
      "gpu_primitives": true,
      "gpu_buffers": _has("glGenBuffers") && _has("glBufferData"),
      "gpu_textures": _has("glGenTextures") && _has("glTexImage2D"),
      "double_buffered": true,
      "software_upload": _soft_enabled(),
      "api_backend": "opengl"
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

fn notify_window_resize(int w, int h) bool {
   if w > 0 { _w = w }
   if h > 0 { _h = h }
   if _soft_enabled() { return true }
   _call4("glViewport", 0, 0, _w, _h)
   true
}

fn get_swapchain_width() int { _w }

fn get_swapchain_height() int { _h }

fn get_swapchain_image_count() int { 2 }

fn renderer_vertex_offset() int { _frame_submitted_vertices * _STRIDE }

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

fn set_mvp(any mat) bool {
   _mvp = mat
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

fn set_model_matrix(any mat) bool {
   _model = mat
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

fn set_ortho(f64 l, f64 r, f64 b, f64 t, f64 n, f64 f) bool {
   _mvp = mat4_ortho(l, r, b, t, n, f)
   if _frame_open && !_soft_enabled() { _apply_matrices() }
   true
}

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

fn create_texture(int width, int height, any pixels) int {
   create_texture_ex(width, height, pixels, 37, 1, GL_REPEAT, GL_REPEAT, false, 0)
}

fn create_texture_ex(
   int width,
   int height,
   any pixels,
   int format=37,
   int filter=1,
   int wrap_s=GL_REPEAT,
   int wrap_t=GL_REPEAT,
   bool use_mipmaps=false,
   int _upload_prebaked_bytes=0
) int {
   "Creates an OpenGL texture from raw pixels."
   _upload_prebaked_bytes
   if width <= 0 || height <= 0 { return -1 }
   if _soft_enabled() {
      if !pixels { return -1 }
      if _last_tex < 0 { _last_tex = 0 }
      _last_tex += 1
      def id = _last_tex
      def cp = _soft_copy_texture_pixels(width, height, pixels, format)
      if !cp { return -1 }
      _tex_live[id] = {"width": width, "height": height, "format": format, "filter": filter}
      _tex_formats[id] = format
      _tex_pixels[id] = cp
      return id
   }
   def id = _gen_name("glGenTextures")
   if id <= 0 { return -1 }
   _bound_tex = -999999
   bind_texture(id)
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, _filter_value(filter, true, use_mipmaps))
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, _filter_value(filter, false, false))
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s)
   _call3("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t)
   _call2("glPixelStorei", GL_UNPACK_ALIGNMENT, 1)
   _call9("glTexImage2D", GL_TEXTURE_2D, 0, _format_internal(format), width, height, 0, _format_external(format), GL_UNSIGNED_BYTE, pixels)
   if use_mipmaps { _call1("glGenerateMipmap", GL_TEXTURE_2D) }
   _tex_live[id] = {"width": width, "height": height, "format": format, "filter": filter}
   _tex_formats[id] = format
   _last_tex = id
   id
}

fn update_texture_rect(int tex_id, int x, int y, int w, int h, any pixels) bool {
   "Updates a sub-rectangle of an existing OpenGL texture."
   if tex_id <= 0 || x < 0 || y < 0 || w <= 0 || h <= 0 || !pixels { return false }
   def meta = _tex_live.get(tex_id, 0)
   if !is_dict(meta) { return false }
   def tw, th = int(meta.get("width", 0)), int(meta.get("height", 0))
   if x + w > tw || y + h > th { return false }
   if _soft_enabled() {
      def dst = _tex_pixels.get(tex_id, 0)
      if !dst { return false }
      return _soft_update_texture_pixels(dst, tw, x, y, w, h, pixels, int(meta.get("format", 37)))
   }
   bind_texture(tex_id)
   _call2("glPixelStorei", GL_UNPACK_ALIGNMENT, 1)
   _call9("glTexSubImage2D", GL_TEXTURE_2D, 0, x, y, w, h, _format_external(int(meta.get("format", 37))), GL_UNSIGNED_BYTE, pixels)
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

fn destroy_texture(int tex_id) bool {
   if tex_id <= 0 { return false }
   if !_soft_enabled() { _delete_name("glDeleteTextures", tex_id) }
   def pix = _tex_pixels.get(tex_id, 0)
   if pix { free(pix) }
   _tex_pixels = _tex_pixels.delete(tex_id)
   _tex_live = _tex_live.delete(tex_id)
   _tex_formats = _tex_formats.delete(tex_id)
   if _bound_tex == tex_id { _bound_tex = -999999 }
   true
}

fn texture_size(int tex_id) list {
   def t = _tex_live.get(tex_id, 0)
   if is_dict(t) { return [int(t.get("width", 0)), int(t.get("height", 0))] }
   [0, 0]
}

fn texture_format(int tex_id) int { int(_tex_formats.get(tex_id, 0)) }

fn texture_count() int { dict_keys(_tex_live).len }

fn last_created_texture_id() int { _last_tex }

fn draw_vertices(any p, int count, int tex_id=-1, bool use_material=false) bool {
   "Draws packed 64-byte vertices through direct OpenGL calls."
   if !p || count <= 0 { return false }
   if _soft_enabled() {
      _soft_draw_vertices(p, count, tex_id, use_material)
      _record_draw(count, false, false)
      return true
   }
   _enable_draw_state(tex_id, use_material)
   if !_draw_immediate_vertices(p, count, GL_TRIANGLES, !use_material || _material_uses_vertex_color(), use_material && !_current_unlit) { return false }
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
   _bind_buffer(GL_ARRAY_BUFFER, 0)
   if idx_buf { _bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0) }
   _record_draw(idx_count, false, true)
}

fn _store_rect(ptr p, f64 x, f64 y, f64 w, f64 h, int c, f64 u1=0.0, f64 v1=0.0, f64 u2=1.0, f64 v2=1.0) bool {
   _store_vertex(p, 0, x,     y,     0.0, u1, v1, c)
   _store_vertex(p, 1, x + w, y + h, 0.0, u2, v2, c)
   _store_vertex(p, 2, x + w, y,     0.0, u2, v1, c)
   _store_vertex(p, 3, x,     y,     0.0, u1, v1, c)
   _store_vertex(p, 4, x,     y + h, 0.0, u1, v2, c)
   _store_vertex(p, 5, x + w, y + h, 0.0, u2, v2, c)
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

fn draw_rect(f64 x, f64 y, f64 w, f64 h, f64 r, f64 g, f64 b, f64 a) bool {
   draw_rect_fast(x, y, w, h, render_shared.pack_rgba_u32(r, g, b, a))
}

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

fn draw_rect_tex(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 r, f64 g, f64 b, f64 a) bool {
   draw_rect_tex_uv(x, y, w, h, tex_id, 0.0, 0.0, 1.0, 1.0, r, g, b, a)
}

fn draw_rect_tex_uv(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 u1, f64 v1, f64 u2, f64 v2, f64 r, f64 g, f64 b, f64 a) bool {
   _reset_ui_draw_state()
   def p = _ensure_scratch(_STRIDE * 6)
   if !p { return false }
   _store_rect(p, x, y, w, h, render_shared.pack_rgba_u32(r, g, b, a), u1, v1, u2, v2)
   draw_vertices(p, 6, tex_id)
}

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

fn draw_static_buffer(dict sbuf, bool is_lines=false, f64 width=1.0, any pipe_override=0, bool is_points=false, bool use_material=false) bool {
   if !is_dict(sbuf) { return false }
   draw_static_buffer_raw(sbuf.get("handle", 0), sbuf.get("offset", 0), int(sbuf.get("count", 0)), is_lines, width, pipe_override, is_points, use_material)
}

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

fn draw_static_buffer_indexed(dict sbuf, any idx_buf, int index_count, bool is_lines=false, f64 width=1.0, any pipe_override=0, bool is_points=false, bool use_material=false) bool {
   if !is_dict(sbuf) { return false }
   draw_static_buffer_indexed_raw(sbuf.get("handle", 0), sbuf.get("offset", 0), idx_buf, sbuf.get("ioffset", 0), index_count, is_lines, width, pipe_override, sbuf.get("index_type_u32", false) ? 1 : 0, is_points, use_material)
}

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

fn _read_pixels_buffer(int buffer, any raw) bool {
   if !raw || _w <= 0 || _h <= 0 { return false }
   _ny_glReadBuffer(buffer)
   _ny_glFinish()
   _ny_glReadPixels(0, 0, _w, _h, GL_RGBA, GL_UNSIGNED_BYTE, raw)
   true
}

fn _readback_rgb_score(any raw, int w, int h) int {
   if !raw || w <= 0 || h <= 0 { return 0 }
   def pixels = w * h
   if pixels <= 0 { return 0 }
   mut step = int(pixels / 4096)
   if step < 1 { step = 1 }
   mut i = 0
   mut score = 0
   while i < pixels {
      def off = i * 4
      if (load8(raw, off) & 255) != 0 || (load8(raw, off + 1) & 255) != 0 || (load8(raw, off + 2) & 255) != 0 {
         score += 1
      }
      i += step
   }
   score
}

fn _readback_detail_score(any raw, int w, int h) int {
   if !raw || w <= 0 || h <= 0 { return 0 }
   def pixels = w * h
   if pixels <= 0 { return 0 }
   def r0 = load8(raw, 0) & 255
   def g0 = load8(raw, 1) & 255
   def b0 = load8(raw, 2) & 255
   mut step = int(pixels / 4096)
   if step < 1 { step = 1 }
   mut i = 0
   mut score = 0
   while i < pixels {
      def off = i * 4
      if (load8(raw, off) & 255) != r0 || (load8(raw, off + 1) & 255) != g0 || (load8(raw, off + 2) & 255) != b0 {
         score += 1
      }
      i += step
   }
   score
}

fn read_framebuffer() any {
   "Reads the visible OpenGL framebuffer as top-left-origin RGBA pixels."
   if _w <= 0 || _h <= 0 { return 0 }
   if _soft_enabled() {
      if !_soft_ensure_surface() { return 0 }
      def bytes = _soft_w * _soft_h * 4
      def out = malloc(bytes)
      if !out { return 0 }
      memcpy(out, _soft_buf, bytes)
      return {"width": _soft_w, "height": _soft_h, "data": out, "channels": 4, "bpp": 4}
   }
   def bytes = _w * _h * 4
   mut raw = malloc(bytes)
   def out = malloc(bytes)
   if !raw || !out {
      if raw { free(raw) }
      if out { free(out) }
      return 0
   }
   def primary_buffer = _frame_open ? GL_BACK : GL_FRONT
   _read_pixels_buffer(primary_buffer, raw)
   def primary_score = _readback_rgb_score(raw, _w, _h)
   def primary_detail = _readback_detail_score(raw, _w, _h)
   if primary_score == 0 || primary_detail == 0 {
      def alt = malloc(bytes)
      if alt {
         _read_pixels_buffer(primary_buffer == GL_BACK ? GL_FRONT : GL_BACK, alt)
         def alt_score = _readback_rgb_score(alt, _w, _h)
         def alt_detail = _readback_detail_score(alt, _w, _h)
         if alt_detail > primary_detail || (primary_score == 0 && alt_score > primary_score) {
            if common.env_truthy("NY_GL_READBACK_TRACE") {
               print("[gl:readback] switched buffer primary_score=" + to_str(primary_score) +
                  " primary_detail=" + to_str(primary_detail) +
                  " alt_score=" + to_str(alt_score) +
               " alt_detail=" + to_str(alt_detail))
            }
            free(raw)
            raw = alt
         } else {
            free(alt)
         }
      }
   }
   def row_bytes = _w * 4
   mut y = 0
   while y < _h {
      __copy_mem(out + y * row_bytes, raw + (_h - 1 - y) * row_bytes, row_bytes)
      y += 1
   }
   free(raw)
   return {"width": _w, "height": _h, "data": out, "channels": 4, "bpp": 4}
}

#main {
   assert(capabilities().get("opengl", false), "gl capability flag")
   assert(texture_size(-1) == [0, 0], "gl missing texture size")
   assert(get_swapchain_image_count() == 2, "gl double buffered")
   print("✓ std.os.ui.render.gl self-test passed")
}
