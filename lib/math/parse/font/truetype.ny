;; Keywords: font truetype os parse ui
;; FreeType-backed TrueType font loading, metrics, glyph rasterization, and atlas data.
;; References:
;; - std.os.ui.font
;; - std.os
module std.os.ui.font.truetype(load, load_path, unload, shutdown, available, scale_for_pixel_height, scale_for_em, get_vmetrics, get_hmetrics, get_kern, get_glyph_index, get_glyph_bitmap, get_glyph_box)
use std.core
use std.core.mem
use std.os (file_exists)
use std.os.ffi (dlopen_checked, dlsym, RTLD_NOW, RTLD_GLOBAL, call1, call2, call3, call4, call5)
use std.os.path as ospath

extern "" {
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
   def v = _load_u16_h(p, off)
   v > 32767 ? (v - 65536) : v
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
   def rc = _ft_new_memory_face(_raw_ptr(_FT_library), data, data.len, index, face_ptr)
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
   def path_c = malloc(path.len + 1)
   if !path_c {
      free(face_ptr)
      return 0
   }
   strcpy(path_c, path)
   def rc = _ft_new_face(_raw_ptr(_FT_library), path_c, index, face_ptr)
   free(path_c)
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
      if _FT_ptr_done_face { call1(_FT_ptr_done_face, _raw_ptr(face)) }
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
      if _FT_ptr_done_free_type { call1(_FT_ptr_done_free_type, _raw_ptr(_FT_library)) }
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
   if info.get("is_color", false) { return float(pixels) }
   if !info.get("is_scalable", true) { return 1.0 }
   def asc = int(info.get("ascender", 0))
   def dsc = int(info.get("descender", 0))
   def span = asc - dsc
   if span <= 0 { return 1.0 }
   def upm = int(info.get("units_per_em", 2048))
   if upm <= 0 { return 1.0 }
   return scale_for_em(info, pixels) * float(upm) / float(span)
}

fn scale_for_em(dict info, any pixels) float {
   "Returns the scale factor that maps the font EM square to `pixels`."
   def px = float(pixels)
   if !info.get("is_scalable", true) { return 1.0 }
   def upm = info.get("units_per_em", 2048)
   if upm <= 0 { return 1.0 }
   return px / float(upm)
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

fn get_glyph_index(dict info, int cp) int {
   "Returns the glyph index for codepoint `cp`."
   def face = info.get("face", 0)
   if !face { return 0 }
   _ft_get_char_index(_raw_ptr(face), cp)
}

fn _load_glyph(dict info, int gi, int flags) bool {
   def face = info.get("face", 0)
   if !face { return false }
   _ft_load_glyph(_raw_ptr(face), gi, flags) == 0
}

fn get_hmetrics(dict info, int gi) list {
   "Returns horizontal glyph metrics for glyph index `gi`."
   def face = info.get("face", 0)
   if !face { return [0, 0] }
   def px = _px_val(info)
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

fn get_kern(dict info, int g1, int g2, int px=0) float {
   "Returns horizontal kerning between glyphs `g1` and `g2` at pixel size `px`."
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

fn get_glyph_bitmap(dict info, any _scale_x, any scale_y, int gi) any {
   "Rasterizes glyph `gi` at the requested pixel scale and returns bitmap metadata."
   def face = info.get("face", 0)
   if !face { return 0 }
   def is_color_font = info.get("is_color", false)
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
   mut load_flags = 4
   if is_color_font { load_flags = load_flags | 0x100000 } elif !info.get("is_scalable", true) {
   } else {
      load_flags = load_flags | 32
   }
   def rc = _ft_load_glyph(_raw_ptr(face), gi, load_flags)
   if rc != 0 { return 0 }
   def face_p = _raw_ptr(face)
   def gs = load64_h(face_p, 152)
   if !gs { return 0 }
   def gs_p = _raw_ptr(gs)
   def rows  = _load_i32_h(gs_p, 152)
   def width = _load_i32_h(gs_p, 156)
   def pitch = _load_i32_h(gs_p, 160)
   def src   = load64_h(gs_p, 168)
   def src_p = _raw_ptr(src)
   def bl_s = _load_i32_h(gs_p, 192)
   def bt_s = _load_i32_h(gs_p, 196)
   def mode  = _load_u8_h(gs_p, 178)
   def pitch_s = pitch
   if rows <= 0 || width <= 0 {
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
   def bsize = rows * width * 4
   def bmp = malloc(bsize)
   if !bmp {
      mut res = dict(16)
      res["data"] = 0
      res["width"] = 0
      res["height"] = 0
      res["xoff"] = bl_s
      res["yoff"] = bt_s
      return res
   }
   memset(bmp, 0, bsize)
   mut abs_pitch = pitch_s
   if abs_pitch < 0 { abs_pitch = -abs_pitch }
   mut y = 0
   while y < rows {
      mut row_src = 0
      if pitch_s > 0 { row_src = y * pitch_s } else { row_src = (rows - 1 - y) * abs_pitch }
      def row_dst = y * width * 4
      if mode == 7 {
         mut x = 0
         while x + 4 <= width {
            def sx, dx = x * 4, x * 4
            def p0 = _load_u8_h(src_p, row_src + sx+2)|(_load_u8_h(src_p, row_src + sx+1)<<8)|(_load_u8_h(src_p, row_src + sx)<<16)|(_load_u8_h(src_p, row_src + sx+3)<<24)
            def p1 = _load_u8_h(src_p, row_src + sx+6)|(_load_u8_h(src_p, row_src + sx+5)<<8)|(_load_u8_h(src_p, row_src + sx+4)<<16)|(_load_u8_h(src_p, row_src + sx+7)<<24)
            def p2 = _load_u8_h(src_p, row_src + sx+10)|(_load_u8_h(src_p, row_src + sx+9)<<8)|(_load_u8_h(src_p, row_src + sx+8)<<16)|(_load_u8_h(src_p, row_src + sx+11)<<24)
            def p3 = _load_u8_h(src_p, row_src + sx+14)|(_load_u8_h(src_p, row_src + sx+13)<<8)|(_load_u8_h(src_p, row_src + sx+12)<<16)|(_load_u8_h(src_p, row_src + sx+15)<<24)
            store32(bmp, p0, row_dst + dx)
            store32(bmp, p1, row_dst + dx+4)
            store32(bmp, p2, row_dst + dx+8)
            store32(bmp, p3, row_dst + dx+12)
            x += 4
         }
         while x < width {
            def sx, dx = x * 4, x * 4
            store32(bmp, _load_u8_h(src_p, row_src + sx+2)|(_load_u8_h(src_p, row_src + sx+1)<<8)|(_load_u8_h(src_p, row_src + sx)<<16)|(_load_u8_h(src_p, row_src + sx+3)<<24), row_dst + dx)
            x += 1
         }
      } elif mode == 1 {
         mut x = 0
         while x < width {
            def byte_idx = x >> 3
            def bit_mask = 0x80 >> (x & 7)
            def bit_on = (_load_u8_h(src_p, row_src + byte_idx) & bit_mask) != 0
            def a = bit_on ? 255 : 0
            store32(bmp, a | (a << 8) | (a << 16) | (a << 24), row_dst + x * 4)
            x += 1
         }
      } else {
         mut x = 0
         while x + 4 <= width {
            def a0, a1 = _load_u8_h(src_p, row_src + x), _load_u8_h(src_p, row_src + x+1)
            def a2, a3 = _load_u8_h(src_p, row_src + x+2), _load_u8_h(src_p, row_src + x+3)
            def dx = x * 4
            store32(bmp, a0 | (a0 << 8) | (a0 << 16) | (a0 << 24), row_dst + dx)
            store32(bmp, a1 | (a1 << 8) | (a1 << 16) | (a1 << 24), row_dst + dx+4)
            store32(bmp, a2 | (a2 << 8) | (a2 << 16) | (a2 << 24), row_dst + dx+8)
            store32(bmp, a3 | (a3 << 8) | (a3 << 16) | (a3 << 24), row_dst + dx+12)
            x += 4
         }
         while x < width {
            def a = _load_u8_h(src_p, row_src + x)
            store32(bmp, a | (a << 8) | (a << 16) | (a << 24), row_dst + x * 4)
            x += 1
         }
      }
      y += 1
   }
   mut res = dict(16)
   res["data"] = bmp
   res["width"] = width
   res["height"] = rows
   res["xoff"] = bl_s
   res["yoff"] = bt_s
   res["bpp"] = 4
   res["is_color"] = (mode == 7)
   res
}
