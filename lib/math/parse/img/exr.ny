;; Keywords: image exr openexr hdr parse
;; OpenEXR image loader implemented in Ny via dynamic FFI.
;; References:
;; - std.math.parse.img
;; - std.math.parse
module std.math.parse.img.exr(decode, decode_bytes, load_path, available, last_error, backend_name)
use std.core
use std.core.dict_mod
use std.math
use std.os
use std.os.path as ospath
use std.os.ffi (dlopen_checked, dlsym, RTLD_NOW, RTLD_GLOBAL, call0_ptr, call1, call1_ptr, call3, call4, call5, cstr)
use std.core.str as str

def _EXR_PIXEL_SIZE = 8
def _EXR_MAGIC_0 = 118
def _EXR_MAGIC_1 = 47
def _EXR_MAGIC_2 = 49
def _EXR_MAGIC_3 = 1
mut _exr_checked = false
mut _exr_ok = false
mut _exr_last_error = ""
mut _exr_backend_name = ""
mut _exr_lib = 0
mut _half_u8_lut = 0
mut _half_a8_lut = 0
mut _ptr_ImfOpenInputFile = 0
mut _ptr_ImfCloseInputFile = 0
mut _ptr_ImfInputHeader = 0
mut _ptr_ImfHeaderDataWindow = 0
mut _ptr_ImfInputSetFrameBuffer = 0
mut _ptr_ImfInputReadPixels = 0
mut _ptr_ImfErrorMessage = 0

fn _set_error(any msg) int {
   _exr_last_error = to_str(msg)
   0
}

fn last_error() str {
   "Returns the last EXR backend error."
   _exr_last_error
}

fn backend_name() str {
   "Returns the active EXR backend name, or empty string when unavailable."
   _exr_backend_name
}

fn _exr_error_message(str fallback="OpenEXR error") str {
   if _ptr_ImfErrorMessage {
      def msg_ptr = call0_ptr(_ptr_ImfErrorMessage)
      if msg_ptr {
         def msg = str.cstr_to_str(msg_ptr)
         if is_str(msg) && msg.len > 0 { return msg }
      }
   }
   fallback
}

fn _exr_load_dyn() any {
   if _exr_checked { return _exr_ok }
   _exr_checked = true
   _exr_ok = false
   _exr_last_error = ""
   _exr_backend_name = ""
   mut lib = 0
   if comptime { __os_name() == "linux" }{
      lib = dlopen_checked("libOpenEXR-3_4.so.30", "ImfOpenInputFile", RTLD_NOW() | RTLD_GLOBAL())
      if !lib { lib = dlopen_checked("libOpenEXR-3_4.so", "ImfOpenInputFile", RTLD_NOW() | RTLD_GLOBAL()) }
      if !lib { lib = dlopen_checked("OpenEXR-3_4", "ImfOpenInputFile", RTLD_NOW() | RTLD_GLOBAL()) }
      if !lib { lib = dlopen_checked("OpenEXR", "ImfOpenInputFile", RTLD_NOW() | RTLD_GLOBAL()) }
   } elif comptime { __os_name() == "macos" }{
      lib = dlopen_checked("libOpenEXR.dylib", "ImfOpenInputFile", RTLD_NOW() | RTLD_GLOBAL())
      if !lib { lib = dlopen_checked("OpenEXR", "ImfOpenInputFile", RTLD_NOW() | RTLD_GLOBAL()) }
   } else {
      return _set_error("OpenEXR backend unsupported on this OS")
   }
   if !lib { return _set_error("missing OpenEXR runtime library") }
   _ptr_ImfOpenInputFile = dlsym(lib, "ImfOpenInputFile")
   _ptr_ImfCloseInputFile = dlsym(lib, "ImfCloseInputFile")
   _ptr_ImfInputHeader = dlsym(lib, "ImfInputHeader")
   _ptr_ImfHeaderDataWindow = dlsym(lib, "ImfHeaderDataWindow")
   _ptr_ImfInputSetFrameBuffer = dlsym(lib, "ImfInputSetFrameBuffer")
   _ptr_ImfInputReadPixels = dlsym(lib, "ImfInputReadPixels")
   _ptr_ImfErrorMessage = dlsym(lib, "ImfErrorMessage")
   if !_ptr_ImfOpenInputFile || !_ptr_ImfCloseInputFile || !_ptr_ImfInputHeader || !_ptr_ImfHeaderDataWindow || !_ptr_ImfInputSetFrameBuffer || !_ptr_ImfInputReadPixels { return _set_error("OpenEXR runtime missing required symbols") }
   _exr_lib = lib
   _exr_backend_name = "OpenEXR"
   _exr_last_error = ""
   _exr_ok = true
   true
}

fn available() any {
   "Returns whether the OpenEXR backend is available."
   _exr_load_dyn()
}

fn _ensure_half_luts() bool {
   if _half_u8_lut && _half_a8_lut { return true }
   def n = 65536
   def pu = malloc(n)
   def pa = malloc(n)
   if !pu || !pa {
      if pu { free(pu) }
      if pa { free(pa) }
      return false
   }
   mut i = 0
   while i < n {
      def v = _half_to_float(i)
      store8(pu, _linear_to_u8(v), i)
      store8(pa, _alpha_to_u8(v), i)
      i += 1
   }
   _half_u8_lut, _half_a8_lut = init_str(pu, n), init_str(pa, n)
   true
}

fn _half_to_float(any h) float {
   def bits = int(h) & 65535
   def sign = (bits >> 15) & 1
   def exp = (bits >> 10) & 31
   def frac = bits & 1023
   mut out = 0.0
   if exp == 0 {
      if frac == 0 { out = 0.0 } else { out = float(frac) * pow(2.0, -24.0) }
   } elif exp == 31 {
      if frac == 0 { out = 65504.0 }
      else { out = 0.0 }
   } else {
      out = (1.0 + float(frac) / 1024.0) * pow(2.0, float(exp - 15))
   }
   if sign != 0 { return 0.0 - out }
   out
}

fn _linear_to_u8(any v) int {
   mut x = float(v)
   if !(x >= 0.0) { x = 0.0 }
   if x < 0.0 { x = 0.0 }
   x = x / (1.0 + x)
   x = pow(x, 1.0 / 2.2)
   if x < 0.0 { x = 0.0 }
   if x > 1.0 { x = 1.0 }
   int(x * 255.0 + 0.5)
}

fn _alpha_to_u8(any v) int {
   mut x = float(v)
   if !(x >= 0.0) { x = 0.0 }
   if x < 0.0 { x = 0.0 }
   if x > 1.0 { x = 1.0 }
   int(x * 255.0 + 0.5)
}

fn _cleanup_input(any in_p) int {
   if in_p && _ptr_ImfCloseInputFile { call1(_ptr_ImfCloseInputFile, in_p) }
   0
}

fn _alloc_i32_slot() any {
   def p = zalloc(8)
   p
}

fn _read_data_window(any hdr) any {
   def x_min_p, y_min_p = _alloc_i32_slot(), _alloc_i32_slot()
   def x_max_p, y_max_p = _alloc_i32_slot(), _alloc_i32_slot()
   if !x_min_p || !y_min_p || !x_max_p || !y_max_p {
      if x_min_p { free(x_min_p) }
      if y_min_p { free(y_min_p) }
      if x_max_p { free(x_max_p) }
      if y_max_p { free(y_max_p) }
      return 0
   }
   call5(_ptr_ImfHeaderDataWindow, hdr, x_min_p, y_min_p, x_max_p, y_max_p)
   def x_min, y_min = load32(x_min_p, 0), load32(y_min_p, 0)
   def x_max, y_max = load32(x_max_p, 0), load32(y_max_p, 0)
   free(x_min_p, y_min_p, x_max_p, y_max_p)
   [x_min, y_min, x_max, y_max]
}

fn load_path(any path) any {
   "Loads an EXR image from `path` into a standard RGBA image dict."
   _exr_last_error = ""
   if !is_str(path) || path.len == 0 { return _set_error("empty EXR path") }
   if !file_exists(path) { return _set_error("EXR file not found: " + to_str(path)) }
   if !_exr_load_dyn() { return 0 }
   if !_ensure_half_luts() { return _set_error("out of memory building EXR conversion LUTs") }
   def in_p = call1_ptr(_ptr_ImfOpenInputFile, cstr(path))
   if !in_p { return _set_error(_exr_error_message("failed to open EXR: " + path)) }
   defer { _cleanup_input(in_p) }
   def hdr = call1_ptr(_ptr_ImfInputHeader, in_p)
   if !hdr { return _set_error(_exr_error_message("failed to read EXR header: " + path)) }
   def win = _read_data_window(hdr)
   if !win || win.len < 4 { return _set_error("failed to read EXR data window") }
   def x_min, y_min = int(win.get(0, 0)), int(win.get(1, 0))
   def x_max, y_max = int(win.get(2, -1)), int(win.get(3, -1))
   def w, h = x_max - x_min + 1, y_max - y_min + 1
   if w <= 0 || h <= 0 || w > 32768 || h > 32768 { return _set_error("invalid EXR dimensions: " + to_str(w) + "x" + to_str(h)) }
   def pixel_count = w * h
   def shift_px = y_min * w + x_min
   def lead_px = max(0, shift_px)
   def trail_px = max(0, 0 - shift_px)
   def half_span = (lead_px + pixel_count + trail_px) * _EXR_PIXEL_SIZE
   def half_mem = zalloc(half_span + 64)
   if !half_mem { return _set_error("out of memory reading EXR pixels") }
   defer { free(half_mem) }
   def fb_base = half_mem + lead_px * _EXR_PIXEL_SIZE + 32
   def pix_ptr = fb_base + shift_px * _EXR_PIXEL_SIZE
   def set_ok = call4(_ptr_ImfInputSetFrameBuffer, in_p, fb_base, 1, w)
   if int(set_ok) == 0 { return _set_error(_exr_error_message("failed to set EXR framebuffer")) }
   def read_ok = call3(_ptr_ImfInputReadPixels, in_p, y_min, y_max)
   if int(read_ok) == 0 { return _set_error(_exr_error_message("failed reading EXR pixels")) }
   def rgba_len = pixel_count * 4
   def rgba_ptr = malloc(rgba_len + 32)
   if !rgba_ptr { return _set_error("out of memory converting EXR image") }
   mut i = 0
   while i < pixel_count {
      def src = pix_ptr + i * _EXR_PIXEL_SIZE
      def dst = rgba_ptr + i * 4
      def hr = load16(src, 0) & 65535
      def hg = load16(src, 2) & 65535
      def hb = load16(src, 4) & 65535
      def ha = load16(src, 6) & 65535
      store8(dst, load8(_half_u8_lut, hr), 0)
      store8(dst, load8(_half_u8_lut, hg), 1)
      store8(dst, load8(_half_u8_lut, hb), 2)
      store8(dst, load8(_half_a8_lut, ha), 3)
      i += 1
   }
   def rgba = init_str(rgba_ptr, rgba_len)
   mut out = dict(5)
   out["data"] = rgba
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out["source_format"] = "exr"
   out
}

fn _decode_exr_bytes_impl(any data, any ext="") any {
   if !is_str(data) || data.len < 4 { return 0 }
   if load8(data, 0) != _EXR_MAGIC_0 || load8(data, 1) != _EXR_MAGIC_1 || load8(data, 2) != _EXR_MAGIC_2 || load8(data, 3) != _EXR_MAGIC_3 { return _set_error("not an OpenEXR stream") }
   def td = temp_dir()
   if !is_str(td) || td.len == 0 { return _set_error("temp dir unavailable for EXR decode") }
   def suffix = (is_str(ext) && ext == ".exr") ? ".exr" : ".tmp.exr"
   def path = ospath.join(td, "ny_exr_" + to_str(ticks()) + suffix)
   match file_write(path, data) {
      ok(ignoredok) -> { ignoredok }
      err(ignorederr) -> { ignorederr  return _set_error("failed to stage EXR bytes") }
   }
   defer {
      match file_remove(path) { ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
   }
   def im = load_path(path)
   if im { return im }
   if _exr_last_error.len > 0 { return 0 }
   _set_error("failed to decode staged EXR image")
}

fn decode_bytes(any data, any ext="") any {
   "Decodes EXR bytes by staging them to a temp file and reading them via OpenEXR."
   _decode_exr_bytes_impl(data, ext)
}

fn decode(any data, any ext="") any {
   "Compatibility wrapper for EXR byte decoding."
   return decode_bytes(data, ext)
}
