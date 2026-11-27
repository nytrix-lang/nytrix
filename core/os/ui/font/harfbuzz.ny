;; Keywords: font shaping harfbuzz unicode script direction ligature
;; Optional HarfBuzz shaping backend for the UI font renderer.
;; References:
;; - core.os.ui.font.truetype
module core.os.ui.font.harfbuzz(available, shape)
use core
use core.os.ffi as ffi

mut _hb_lib = 0
mut _hb_symbols = dict(16)
mut _hb_fonts = dict(16)

fn _load() bool {
   if _hb_lib { return true }
   mut lib = 0
   if comptime { __os_name() == "linux" } {
      lib = ffi.dlopen_checked("libharfbuzz.so.0", "hb_shape", ffi.RTLD_NOW())
      if !lib { lib = ffi.dlopen_checked("libharfbuzz.so", "hb_shape", ffi.RTLD_NOW()) }
   } elif comptime { __os_name() == "windows" } {
      lib = ffi.dlopen_checked("libharfbuzz-0.dll", "hb_shape", ffi.RTLD_NOW())
      if !lib { lib = ffi.dlopen_checked("harfbuzz.dll", "hb_shape", ffi.RTLD_NOW()) }
   } else {
      lib = ffi.dlopen_checked("libharfbuzz.0.dylib", "hb_shape", ffi.RTLD_NOW())
      if !lib { lib = ffi.dlopen_checked("libharfbuzz.dylib", "hb_shape", ffi.RTLD_NOW()) }
      if !lib { lib = ffi.dlopen_checked("harfbuzz", "hb_shape", ffi.RTLD_NOW()) }
   }
   if !lib { return false }
   def names = ["hb_blob_create_from_file_or_fail", "hb_blob_destroy", "hb_face_create", "hb_face_destroy",
      "hb_font_create", "hb_font_destroy", "hb_ot_font_set_funcs", "hb_font_set_scale",
      "hb_buffer_create", "hb_buffer_destroy", "hb_buffer_add_utf8", "hb_buffer_guess_segment_properties",
      "hb_buffer_get_length", "hb_buffer_get_glyph_infos", "hb_buffer_get_glyph_positions", "hb_shape"]
   mut symbols = dict(16)
   for name in names {
      def addr = ffi.dlsym(lib, name)
      if !addr { ffi.dlclose(lib) return false }
      symbols[name] = addr
   }
   _hb_lib = lib
   _hb_symbols = symbols
   true
}

fn available() bool {
   "Returns whether a compatible HarfBuzz runtime can be loaded."
   _load()
}

fn _call(str name, list args) any {
   def fn_ptr = _hb_symbols.get(name, 0)
   if !fn_ptr { return 0 }
   ffi.ffi_call(fn_ptr, args)
}

fn _font(str path, int size) any {
   if !_load() || path.len == 0 || size <= 0 { return 0 }
   def key = path + "|" + to_str(size)
   def cached = _hb_fonts.get(key, 0)
   if cached { return cached }
   ;; HarfBuzz objects use foreign allocations that must outlive renderer
   ;; shutdown callbacks across the VM, NYR and LLVM JIT routes. Keep a small,
   ;; process-lifetime cache rather than destroying objects from those routes.
   if _hb_fonts.len >= 128 { return 0 }
   def blob = _call("hb_blob_create_from_file_or_fail", [ffi.cptr(path)])
   if !blob { return 0 }
   def face = _call("hb_face_create", [blob, 0])
   if !face { _call("hb_blob_destroy", [blob]) return 0 }
   def font = _call("hb_font_create", [face])
   if !font {
      _call("hb_face_destroy", [face])
      _call("hb_blob_destroy", [blob])
      return 0
   }
   _call("hb_ot_font_set_funcs", [font])
   _call("hb_font_set_scale", [font, size * 64, size * 64])
   def record = {"font": font, "face": face, "blob": blob}
   _hb_fonts[key] = record
   record
}

fn _i32(any p, int off) int {
   def v = load32(p, off)
   v > 2147483647 ? v - 4294967296 : v
}

fn shape(str path, int size, str text) list {
   "Shapes UTF-8 text and returns [glyph_id, cluster_byte, x_advance, y_advance, x_offset, y_offset] records in pixels; an empty list means unavailable or invalid input."
   if text.len == 0 { return [] }
   def record = _font(path, size)
   if !record { return [] }
   def font = record.get("font", 0)
   def buffer = _call("hb_buffer_create", [])
   if !buffer { return [] }
   def utf8 = ffi.cptr(text)
   _call("hb_buffer_add_utf8", [buffer, utf8, text.len, 0, text.len])
   _call("hb_buffer_guess_segment_properties", [buffer])
   _call("hb_shape", [font, buffer, 0, 0])
   def count_ptr = malloc(4)
   if !count_ptr { _call("hb_buffer_destroy", [buffer]) return [] }
   store32(count_ptr, 0, 0)
   def infos = _call("hb_buffer_get_glyph_infos", [buffer, count_ptr])
   def count = load32(count_ptr, 0)
   store32(count_ptr, 0, 0)
   def positions = _call("hb_buffer_get_glyph_positions", [buffer, count_ptr])
   def position_count = load32(count_ptr, 0)
   free(count_ptr)
   mut shaped = []
   if infos && positions && count > 0 && count == position_count {
      mut i = 0
      while i < count {
         def io = i * 20
         def po = i * 20
         shaped = shaped.append([load32(infos, io), load32(infos, io + 8),
               float(_i32(positions, po)) / 64.0, float(_i32(positions, po + 4)) / 64.0,
               float(_i32(positions, po + 8)) / 64.0, float(_i32(positions, po + 12)) / 64.0])
         i += 1
      }
   }
   _call("hb_buffer_destroy", [buffer])
   shaped
}
