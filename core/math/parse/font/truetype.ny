;; Keywords: font truetype os parse ui
;; FreeType-backed TrueType font loading, metrics, glyph rasterization, and atlas data.
;; References:
;; - core.os.ui.font
;; - core.os
module core.os.ui.font.truetype(load, load_path, unload, shutdown, available, scale_for_pixel_height, scale_for_em, get_vmetrics, get_hmetrics, get_kern, get_glyph_index, get_glyph_bitmap, get_glyph_bitmap_lcd, get_glyph_box)
use core
use core.mem
use core.math.checked (checked_mul)
use core.os (file_exists)
use core.os.ffi (dlopen_checked, dlsym, RTLD_NOW, RTLD_GLOBAL)
use core.os.ffi as calls
use core.os.path as ospath

extern {
   fn _ft_init(ptr lib_p) i32 as "FT_Init_FreeType"
   fn _ft_new_memory_face(ptr lib, ptr data, i64 size, i64 index, ptr face_p) i32 as "FT_New_Memory_Face"
   fn _ft_new_face(ptr lib, ptr path, i64 index, ptr face_p) i32 as "FT_New_Face"
   fn _ft_done_face(ptr face) i32 as "FT_Done_Face"
   fn _ft_done_free_type(ptr lib) i32 as "FT_Done_FreeType"
   fn _ft_select_size(ptr face, i32 strike_index) i32 as "FT_Select_Size"
   fn _ft_set_pixel_sizes(ptr face, u32 pixel_width, u32 pixel_height) i32 as "FT_Set_Pixel_Sizes"
   fn _ft_get_char_index(ptr face, u64 cp) u32 as "FT_Get_Char_Index"
   fn _ft_load_glyph(ptr face, u32 gi, i32 flags) i32 as "FT_Load_Glyph"
   fn _ft_get_kerning(ptr face, u32 g1, u32 g2, u32 mode, ptr vec) i32 as "FT_Get_Kerning"
}

if comptime { __os_name() == "linux" }{
   #link "libfreetype.so"
   #include <freetype2/freetype/freetype.h> as "FT_"
}

if comptime { __os_name() == "windows" }{
   #link "freetype.lib"
   #include <freetype2/freetype/freetype.h> as "FT_"
}

if comptime { __os_name() == "macos" }{
   #link "libfreetype.dylib"
   #include <freetype2/freetype/freetype.h> as "FT_"
}

mut _FT_library  = 0
mut _FT_dyn_lib  = 0
mut _FT_ptr_init = 0
mut _FT_ptr_new_memory_face = 0
mut _FT_ptr_new_face = 0
mut _FT_ptr_done_face = 0
mut _FT_ptr_done_free_type = 0
mut _FT_ptr_select_size = 0
mut _FT_ptr_set_pixel_sizes = 0
mut _FT_ptr_get_char_index = 0
mut _FT_ptr_load_glyph = 0
mut _FT_ptr_get_kerning = 0

fn _raw_ptr(any p) any {
   if !p { return 0 }
   if is_int(p) { return to_int(p) }
   p
}

fn _load_u8_h(any p, int off) int {
   def base = off - (off & 3)
   def shift = (off & 3) * 8
   (load32_h(p, base) >> shift) & 255
}

fn _load_u16_h(any p, int off) int { load32_h(p, off) & 65535 }

fn _load_i16_h(any p, int off) int {
   def int v = _load_u16_h(p, off)
   v - ((v & 32768) * 2)
}

fn _load_u32_h(any p, int off) int { load32_h(p, off) }

fn _load_i32_h(any p, int off) int {
   def v = load32_h(p, off)
   v > 2147483647 ? (v - 4294967296) : v
}

fn _load_dyn() bool {
   if _FT_dyn_lib { return true }
   mut lib = 0
   if comptime { __os_name() == "linux" }{
      lib = dlopen_checked("libfreetype.so.6", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL())
      if !lib { lib = dlopen_checked("libfreetype.so", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL()) }
   } elif comptime { __os_name() == "windows" }{
      lib = dlopen_checked("freetype.dll", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL())
      if !lib { lib = dlopen_checked("freetype", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL()) }
      if !lib { lib = dlopen_checked("libfreetype-6.dll", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL()) }
      if !lib { lib = dlopen_checked("freetype-6.dll", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL()) }
      if !lib { lib = dlopen_checked("libfreetype.dll", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL()) }
   } else {
      lib = dlopen_checked("libfreetype.dylib", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL())
      if !lib { lib = dlopen_checked("freetype", "FT_Init_FreeType", RTLD_NOW() | RTLD_GLOBAL()) }
   }
   if !lib { return false }
   _FT_dyn_lib = lib
   _FT_ptr_init = dlsym(lib, "FT_Init_FreeType")
   _FT_ptr_new_memory_face = dlsym(lib, "FT_New_Memory_Face")
   _FT_ptr_new_face = dlsym(lib, "FT_New_Face")
   _FT_ptr_done_face = dlsym(lib, "FT_Done_Face")
   _FT_ptr_done_free_type = dlsym(lib, "FT_Done_FreeType")
   _FT_ptr_select_size = dlsym(lib, "FT_Select_Size")
   _FT_ptr_set_pixel_sizes = dlsym(lib, "FT_Set_Pixel_Sizes")
   _FT_ptr_get_char_index = dlsym(lib, "FT_Get_Char_Index")
   _FT_ptr_load_glyph = dlsym(lib, "FT_Load_Glyph")
   _FT_ptr_get_kerning = dlsym(lib, "FT_Get_Kerning")
   _FT_ptr_init && _FT_ptr_new_memory_face && _FT_ptr_new_face && _FT_ptr_done_face &&
   _FT_ptr_select_size && _FT_ptr_set_pixel_sizes && _FT_ptr_get_char_index &&
   _FT_ptr_load_glyph && _FT_ptr_get_kerning
}

fn _init() bool {
   if _FT_library { return true }
   if !_load_dyn() {
      if comptime { __os_name() != "windows" }{ return false }
   }
   def lib_ptr = malloc(8)
   if !lib_ptr { return false }
   store64_h(lib_ptr, 0, 0)
   def rc = _ft_init(lib_ptr)
   if rc != 0 {
      free(lib_ptr)
      return false
   }
   _FT_library = load64_h(lib_ptr, 0)
   free(lib_ptr)
   !!_FT_library
}

fn available() bool {
   "Returns whether the FreeType-backed TrueType loader is available."
   _init()
}

fn load(any data, int index=0) any {
   "Loads a font face from in-memory font bytes."
   if !_init() { return 0 }
   if !is_str(data) || data.len < 4 { return 0 }
   def face_ptr = malloc(8)
   if !face_ptr { return 0 }
   store64_h(face_ptr, 0, 0)
   def int rc = _ft_new_memory_face(_raw_ptr(_FT_library), data, data.len, index, face_ptr)
   if rc != 0 {
      free(face_ptr)
      return 0
   }
   def face = load64_h(face_ptr, 0)
   free(face_ptr)
   if !face { return 0 }
   _finish_load(dict(16), face, data)
}

fn load_path(str path, int index=0) any {
   "Loads a font face from a file path directly."
   if !_init() { return 0 }
   if !is_str(path) || !file_exists(path) { return 0 }
   def face_ptr = malloc(8)
   if !face_ptr { return 0 }
   store64_h(face_ptr, 0, 0)
   def ptr path_c = calls.cstr(path)
   def int rc = _ft_new_face(_raw_ptr(_FT_library), path_c, index, face_ptr)
   if rc != 0 {
      free(face_ptr)
      return 0
   }
   def face = load64_h(face_ptr, 0)
   free(face_ptr)
   if !face { return 0 }
   _finish_load(dict(16), face, 0)
}

fn _finish_load(any info_in, any face, any data) dict {
   mut info = info_in
   if !is_dict(info) { info = dict(16) }
   def face_p = _raw_ptr(face)
   def units_per_em = _load_u16_h(face_p, 136)
   def ascender  = _load_i16_h(face_p, 138)
   def descender = _load_i16_h(face_p, 140)
   def face_height = _load_i16_h(face_p, 142)
   def num_glyphs = _load_i32_h(face_p, 32)
   def flags = _load_u32_h(face_p, 16)
   def is_scalable = (flags & 1) != 0
   info["face"] = face
   info["data"] = data
   info["units_per_em"] = units_per_em
   info["ascender"] = ascender
   info["descender"] = descender
   info["height"] = face_height
   info["num_glyphs"] = num_glyphs
   def ps_ptr = malloc(8)
   if !ps_ptr {
      info["ps_ptr"] = 0
      info["is_scalable"] = is_scalable
      info["is_color"] = (flags & 16384) != 0
      return info
   }
   store32(ps_ptr, 0, 0)
   info["ps_ptr"] = ps_ptr
   info["is_scalable"] = is_scalable
   info["is_color"] = (flags & 16384) != 0
   info
}

fn unload(any info) any {
   "Releases a loaded font face."
   if !is_dict(info) { return 0 }
   def face = info.get("face", 0)
   if face {
      if _FT_ptr_done_face { calls.any1(_FT_ptr_done_face, _raw_ptr(face)) }
      else { _ft_done_face(_raw_ptr(face)) }
   }
   def ps_ptr = info.get("ps_ptr", 0)
   if ps_ptr { free(ps_ptr) }
   info["face"] = 0
   info["ps_ptr"] = 0
   0
}

fn shutdown() any {
   "Releases the shared FreeType library instance after all faces are unloaded."
   if _FT_library {
      if _FT_ptr_done_free_type { calls.any1(_FT_ptr_done_free_type, _raw_ptr(_FT_library)) }
      else { _ft_done_free_type(_raw_ptr(_FT_library)) }
   }
   _FT_library = 0
   0
}

fn _set_size(dict info, int px) bool {
   def face = info.get("face", 0)
   if !face { return false }
   def ps_ptr = info.get("ps_ptr", 0)
   if !ps_ptr { return false }
   def prev = load32(ps_ptr, 0)
   if prev == px { return true }
   if !info.get("is_scalable", true) {
      def face_p = _raw_ptr(face)
      def num_strikes = _load_i32_h(face_p, 56)
      mut best_idx = -1
      mut best_diff = 999999
      mut si = 0
      while si < num_strikes {
         def sizes_ptr = load64_h(face_p, 64)
         def s_h = _load_u16_h(_raw_ptr(sizes_ptr), si * 24 + 0)
         mut diff = px - s_h
         if diff < 0 { diff = -diff }
         if diff < best_diff { best_diff = diff best_idx = si }
         si += 1
      }
      if best_idx >= 0 { _ft_select_size(_raw_ptr(face), best_idx) }
   } else {
      _ft_set_pixel_sizes(_raw_ptr(face), 0, px)
   }
   store32(ps_ptr, px, 0)
   true
}

fn _px_val(dict info) int {
   def ps_ptr = info.get("ps_ptr", 0)
   if !ps_ptr { return 32 }
   load32(ps_ptr, 0)
}

fn scale_for_pixel_height(dict info, any pixels) float {
   "Returns the font-space scale required to reach `pixels` of height."
   def bool is_color = info.get("is_color", false) == true
   def bool scalable = info.get("is_scalable", true) == true
   def int asc = int(info.get("ascender", 0))
   def int dsc = int(info.get("descender", 0))
   def int metric_span = asc - dsc
   def int upm = int(info.get("units_per_em", 2048))
   def int safe_span = max(1, metric_span)
   mut f64 scale = float(pixels) / float(safe_span)
   if metric_span <= 0 { scale = 1.0 }
   if is_color { scale = float(pixels) }
   scale
}

fn scale_for_em(dict info, any pixels) float {
   "Returns the scale factor that maps the font EM square to `pixels`."
   def px = float(pixels)
   def bool scalable = info.get("is_scalable", true) == true
   def int upm = int(info.get("units_per_em", 2048))
   def int safe_upm = max(1, upm)
   px / float(safe_upm)
}

fn get_vmetrics(dict info) list {
   "Returns vertical font metrics as unscaled `[ascender, descender, height]`."
   def face = info.get("face", 0)
   if !face { return [0, 0, 0] }
   def face_p = _raw_ptr(face)
   [
      float(_load_i16_h(face_p, 138)),
      float(_load_i16_h(face_p, 140)),
      float(_load_i16_h(face_p, 142))
   ]
}

fn get_glyph_index(any info, int cp) int {
   "Returns the glyph index for codepoint `cp`."
   if !is_dict(info) { return 0 }
   def face = info.get("face", 0)
   if !face { return 0 }
   _ft_get_char_index(_raw_ptr(face), cp)
}

fn _load_glyph(dict info, int gi, int flags) bool {
   def face = info.get("face", 0)
   if !face { return false }
   _ft_load_glyph(_raw_ptr(face), gi, flags) == 0
}

fn get_hmetrics(dict info, int gi, int px_hint=0) list {
   "Returns horizontal glyph metrics for glyph index `gi` at `px_hint` when provided."
   def face = info.get("face", 0)
   if !face { return [0, 0] }
   mut px = int(px_hint)
   if px <= 0 { px = _px_val(info) }
   if px <= 0 { return [0, 0] }
   _set_size(info, px)
   if !_load_glyph(info, gi, 0) { return [0, 0] }
   def face_p = _raw_ptr(face)
   def gs = load64_h(face_p, 152)
   if !gs { return [0, 0] }
   def gs_p = _raw_ptr(gs)
   [
      float(_load_i32_h(gs_p, 128)) / 64.0,
      float(_load_i32_h(gs_p, 192)) / 64.0
   ]
}

fn get_glyph_box(dict info, int gi) any {
   "Returns glyph bounds for glyph index `gi`."
   def face = info.get("face", 0)
   if !face { return 0 }
   def px = _px_val(info)
   if px <= 0 { return 0 }
   _set_size(info, px)
   if !_load_glyph(info, gi, 0) { return 0 }
   def face_p = _raw_ptr(face)
   def gs = load64_h(face_p, 152)
   if !gs { return 0 }
   def gs_p = _raw_ptr(gs)
   def rows  = _load_i32_h(gs_p, 152)
   def width = _load_i32_h(gs_p, 156)
   if rows <= 0 || width <= 0 { return 0 }
   def bl_s, bt_s = _load_i32_h(gs_p, 192), _load_i32_h(gs_p, 196)
   [float(bl_s), float(bt_s - rows), float(bl_s + width), float(bt_s)]
}

fn get_kern(any info, int g1, int g2, int px=0) float {
   "Returns horizontal kerning between glyphs `g1` and `g2` at pixel size `px`."
   if !is_dict(info) { return 0.0 }
   def face = info.get("face", 0)
   if !face { return 0 }
   if px > 0 { _set_size(info, px) }
   def vec = malloc(16)
   if !vec { return 0.0 }
   store64_h(vec, 0, 0)
   store64_h(vec, 0, 8)
   def rc = _ft_get_kerning(_raw_ptr(face), g1, g2, 0, vec)
   def kx = load32(vec, 0)
   free(vec)
   if rc != 0 { return 0.0 }
   mut kx_s = kx
   if kx_s > 2147483647 { kx_s = -(4294967296 - kx_s) }
   float(kx_s) / 64.0
}

fn _get_glyph_bitmap(dict info, any _scale_x, any scale_y, int gi, bool lcd) any {
   "Rasterizes glyph `gi`, optionally preserving FreeType's three LCD coverage samples per pixel."
   def face = info.get("face", 0)
   if !face { return 0 }
   def is_color_font = info.get("is_color", false)
   if lcd && is_color_font { return 0 }
   def is_scalable = info.get("is_scalable", true)
   mut px = 0
   if is_color_font || !is_scalable { px = __flt_to_int(scale_y + 0.5) } else {
      def asc = int(info.get("ascender", 0))
      def dsc = int(info.get("descender", 0))
      def span = max(1, asc - dsc)
      px = __flt_to_int(scale_y * span + 0.5)
   }
   if px < 1 { px = 1 }
   if px > 1024 { px = 1024 }
   if !_set_size(info, px) { return 0 }
   ;; Render at the requested pixel size with the face's native hinting.  Do
   ;; not force auto-hinting here: at editor sizes it visibly distorts some
   ;; mono glyph stems and makes text look stretched/jagged.
   mut load_flags = 4
   if lcd { load_flags = load_flags | 0x30000 }
   if is_color_font { load_flags = load_flags | 0x100000 }
   def rc = _ft_load_glyph(_raw_ptr(face), gi, load_flags)
   if rc != 0 { return 0 }
   def ptr face_p = _raw_ptr(face)
   def gs = load64_h(face_p, 152)
   if !gs { return 0 }
   def ptr gs_p = _raw_ptr(gs)
   def int rows  = _load_i32_h(gs_p, 152)
   def int source_width = _load_i32_h(gs_p, 156)
   def int width = lcd ? source_width / 3 : source_width
   def int pitch = _load_i32_h(gs_p, 160)
   def src   = load64_h(gs_p, 168)
   def ptr src_p = _raw_ptr(src)
   def int bl_s = _load_i32_h(gs_p, 192)
   def int bt_s = _load_i32_h(gs_p, 196)
   def int mode  = _load_u8_h(gs_p, 178)
   def int pitch_s = pitch
   if lcd && mode != 5 { return 0 }
   if rows <= 0 || source_width <= 0 || width <= 0 || (lcd && source_width % 3 != 0) {
      mut res = dict(16)
      res["data"] = 0
      res["width"] = 0
      res["height"] = 0
      res["xoff"] = bl_s
      res["yoff"] = bt_s
      return res
   }
   if !src_p {
      mut res = dict(16)
      res["data"] = 0
      res["width"] = 0
      res["height"] = 0
      res["xoff"] = bl_s
      res["yoff"] = bt_s
      return res
   }
   def pixels_result = checked_mul(rows, width)
   if !is_ok(pixels_result) { return 0 }
   def bsize_result = checked_mul(int(unwrap(pixels_result)), 4)
   if !is_ok(bsize_result) { return 0 }
   def int bsize = int(unwrap(bsize_result))
   mut bitmap_cleanup = 0
   defer { if bitmap_cleanup { free(bitmap_cleanup) } }
   mut bmp = malloc(bsize)
   if !bmp {
      mut res = dict(16)
      res["data"] = 0
      res["width"] = 0
      res["height"] = 0
      res["xoff"] = bl_s
      res["yoff"] = bt_s
      return res
   }
   bitmap_cleanup = bmp
   memset(bmp, 0, bsize)
   mut int abs_pitch = pitch_s
   if abs_pitch < 0 { abs_pitch = -abs_pitch }
   mut int min_source_row = source_width * (mode == 7 ? 4 : 1)
   if mode == 1 { min_source_row = (source_width + 7) / 8 }
   if abs_pitch < min_source_row { return 0 }
   for row_idx in 0..(rows - 1) {
      mut int row_src = 0
      if pitch_s > 0 { row_src = row_idx * pitch_s } else { row_src = (rows - 1 - row_idx) * abs_pitch }
      def int row_dst = row_idx * width * 4
      for col_idx in 0..(width - 1) {
         def int dst = row_dst + col_idx * 4
         if lcd && mode == 5 {
            def int sx = row_src + col_idx * 3
            def int red = _load_u8_h(src_p, sx)
            def int green = _load_u8_h(src_p, sx + 1)
            def int blue = _load_u8_h(src_p, sx + 2)
            def int alpha = (red + green + blue + 1) / 3
            store32(bmp, red | (green << 8) | (blue << 16) | (alpha << 24), dst)
         } elif mode == 7 {
            def int sx = row_src + col_idx * 4
            def int pixel = _load_u8_h(src_p, sx+2)|(_load_u8_h(src_p, sx+1)<<8)|(_load_u8_h(src_p, sx)<<16)|(_load_u8_h(src_p, sx+3)<<24)
            store32(bmp, pixel, dst)
         } elif mode == 1 {
            def int byte_idx = col_idx >> 3
            def int bit_mask = 0x80 >> (col_idx & 7)
            def bool bit_on = (_load_u8_h(src_p, row_src + byte_idx) & bit_mask) != 0
            mut int a = 0
            if bit_on { a = 255 }
            store32(bmp, a | (a << 8) | (a << 16) | (a << 24), dst)
         } else {
            def int a = _load_u8_h(src_p, row_src + col_idx)
            store32(bmp, a | (a << 8) | (a << 16) | (a << 24), dst)
         }
      }
   }
   mut res = dict(16)
   res["data"] = bmp
   res["width"] = width
   res["height"] = rows
   res["xoff"] = bl_s
   res["yoff"] = bt_s
   res["bpp"] = 4
   res["is_color"] = (mode == 7)
   res["is_lcd"] = lcd && mode == 5
   bitmap_cleanup = 0
   res
}

fn get_glyph_bitmap(dict info, any scale_x, any scale_y, int gi) any {
   "Rasterizes a grayscale or color glyph into RGBA bitmap metadata."
   _get_glyph_bitmap(info, scale_x, scale_y, gi, false)
}

fn get_glyph_bitmap_lcd(dict info, any scale_x, any scale_y, int gi) any {
   "Rasterizes an RGB LCD glyph and returns one RGB coverage triplet per output pixel."
   _get_glyph_bitmap(info, scale_x, scale_y, gi, true)
}
