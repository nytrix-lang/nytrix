;; Keywords: render opengl gl texture readback
;; References: std.os.ui.render.gl.state std.os.ui.render.gl.constants
module std.os.ui.render.gl.texture(_read_pixels_buffer, _readback_detail_score, _readback_rgb_score, alt, alt_detail, alt_score, b0, bytes, cp, create_texture, create_texture_ex, destroy_texture, dst, g0, i, id, last_created_texture_id, meta, off, out, pix, pixels, primary_buffer, primary_detail, primary_score, r0, raw, read_framebuffer, row_bytes, score, step, t, texture_count, texture_format, texture_size, tw, update_texture_rect, y)
use std.core
use std.math
use std.os.ffi as ffi
use std.os.ui.render.shared as render_shared
use std.os.ui.render.gl.constants as gl_constants
use std.os.ui.render.gl.state as gl_state

;; Creates and returns the texture.
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

;; Returns true when destroy texture.
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

;; Returns the result of the `texture_size` operation.
fn texture_size(int tex_id) list {
   def t = _tex_live.get(tex_id, 0)
   if is_dict(t) { return [int(t.get("width", 0)), int(t.get("height", 0))] }
   [0, 0]
}

fn texture_format(int tex_id) int { int(_tex_formats.get(tex_id, 0)) }

;; Returns the texture count.
fn texture_count() int { dict_keys(_tex_live).len }

;; Returns the result of the `last_created_texture_id` operation.
fn last_created_texture_id() int { _last_tex }

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
