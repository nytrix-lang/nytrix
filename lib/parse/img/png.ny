;; Keywords: image png parse
;; Portable Network Graphics (PNG) Image Loader and Encoder for Nytrix
;; References:
;; - std.parse.img
;; - std.parse
module std.parse.img.png(decode, encode)
#include <png.h>
extern "png16" {
   fn png_image_begin_read_from_memory(ptr image, ptr memory, u64 size) i32
   fn png_image_finish_read(ptr image, ptr background, ptr buffer, i32 row_stride, ptr colormap) i32
   fn png_image_free(ptr image)
   fn _png_image_begin_read_from_memory(ptr image, ptr memory, u64 size) i32 as "png_image_begin_read_from_memory"
   fn _png_image_finish_read(ptr image, ptr background, ptr buffer, i32 row_stride, ptr colormap) i32 as "png_image_finish_read"
   fn _png_image_free(ptr image) as "png_image_free"
}

use std.core
use std.core.dict_mod
use std.core.mem as core_mem
use std.os
use std.os.sys
use std.os.path
use std.math as math
use std.math.crypto.hash as math_hash
use std.parse.data.zlib as zlib
use std.core.common as common

mut _png_disable_libpng_cache = -1
mut _png_stop_early_cache = -1
mut _png_validate_crc_cache = -1
mut _png_no_decompress_cache = -1
mut _png_stage_debug_cache = -1
mut _png_encode_level_cache = -2

fn _png_paeth(int a, int b, int c) int {
   def p = a + b - c
   def pa = p - a
   def pb = p - b
   def pc = p - c
   def apa = (pa < 0) ? (0 - pa) : pa
   def apb = (pb < 0) ? (0 - pb) : pb
   def apc = (pc < 0) ? (0 - pc) : pc
   if apa <= apb && apa <= apc { return a }
   if apb <= apc { return b }
   c
}

fn _s32be(any p, int v, int o) any {
   store8(p, (v >> 24) & 255, o)
   store8(p, (v >> 16) & 255, o + 1)
   store8(p, (v >> 8) & 255, o + 2)
   store8(p, v & 255, o + 3)
}

fn _u16be(any p, int o) int {
   (load8(p, o) << 8) | load8(p, o + 1)
}

fn _u32be(any p, int o) int {
   (load8(p, o) << 24) | (load8(p, o + 1) << 16) | (load8(p, o + 2) << 8) | load8(p, o + 3)
}

fn _png_rgb_opaque32(any p, int o) int {
   load8(p, o) | (load8(p, o + 1) << 8) | (load8(p, o + 2) << 16) | (255 << 24)
}

fn _copy_bytes_safe(any dst, int dst_off, any src, int src_off, int count) any {
   mut i = 0
   while i < count {
      store8(dst, load8(src, src_off + i), dst_off + i)
      i += 1
   }
}

fn _png_chunk_is(any data, int chunk_pos, int a, int b, int c, int d) bool {
   load8(data, chunk_pos + 4) == a &&
   load8(data, chunk_pos + 5) == b &&
   load8(data, chunk_pos + 6) == c &&
   load8(data, chunk_pos + 7) == d
}

fn _png_stop_early_enabled() bool {
   _png_stop_early_cache = common.cached_env_truthy(_png_stop_early_cache, "NY_PNG_STOP_EARLY")
   _png_stop_early_cache == 1
}

fn _png_validate_crc_enabled() bool {
   _png_validate_crc_cache = common.cached_env_truthy(_png_validate_crc_cache, "NY_PNG_VALIDATE_CRC")
   _png_validate_crc_cache == 1
}

fn _png_no_decompress_enabled() bool {
   _png_no_decompress_cache = common.cached_env_truthy(_png_no_decompress_cache, "NY_PNG_NO_DECOMPRESS")
   _png_no_decompress_cache == 1
}

fn _png_stage_debug_enabled() bool {
   _png_stage_debug_cache = common.cached_env_truthy(_png_stage_debug_cache, "NY_PNG_STAGE_DEBUG")
   _png_stage_debug_cache == 1
}

fn _png_encode_level() int {
   if _png_encode_level_cache != -2 { return _png_encode_level_cache }
   mut level = 6
   if common.env_present("NY_PNG_ENCODE_LEVEL") { level = common.env_int_clamped("NY_PNG_ENCODE_LEVEL", level, 0, 9) } elif common.env_truthy("NYTRIX_AUTO_DUMP") || common.env_truthy("NY_UI_BATCH_FAST_ENV") { level = 1 }
   _png_encode_level_cache = level
   level
}

fn _fail(any msg) int {
   print("[png] error: " + msg)
   0
}

fn _png_decode_result(any pixels, int w, int h) dict {
   mut out = dict(8)
   out["data"] = pixels
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out
}

fn _raw_ptr(any p) any {
   if !p { return 0 }
   if is_int(p) { return to_int(p) }
   p
}

fn _png_disable_libpng() bool {
   _png_disable_libpng_cache = common.cached_env_truthy(_png_disable_libpng_cache, "NY_PNG_DISABLE_LIBPNG")
   _png_disable_libpng_cache == 1
}

fn _png_has_sensitive_colorspace_chunks(any data) bool {
   "Returns true when the PNG carries colorspace metadata that the simplified
   libpng path would apply, but glTF sample tests expect raw texture samples."
   if !is_str(data) || data.len < 16 { return false }
   mut p = 8
   while p + 8 <= data.len {
      def length = _u32be(data, p)
      def chunk_data = p + 8
      def next_p = chunk_data + length + 4
      if next_p > data.len { return false }
      if _png_chunk_is(data, p, 105, 67, 67, 80) { return true }
      if _png_chunk_is(data, p, 103, 65, 77, 65) {
         if length < 4 { return true }
         def gamma_100k = _u32be(data, chunk_data)
         if gamma_100k != 45455 { return true }
      }
      if _png_chunk_is(data, p, 73, 69, 78, 68) { break }
      p = next_p
   }
   false
}

fn _png_decode_libpng(any data) any {
   def img = zalloc(128)
   if !img { return 0 }
   store32(img, 1, 8)
   def begin_ok = _png_image_begin_read_from_memory(img, _raw_ptr(data), data.len)
   if int(begin_ok) == 0 {
      _png_image_free(img)
      free(img)
      return 0
   }
   def w, h = load32(img, 12), load32(img, 16)
   if w <= 0 || h <= 0 || w > 32768 || h > 32768 {
      _png_image_free(img)
      free(img)
      return 0
   }
   store32(img, 3, 20)
   def pix_len = w * h * 4
   def pix_raw = malloc(pix_len + 32)
   if !pix_raw {
      _png_image_free(img)
      free(img)
      return 0
   }
   def finish_ok = _png_image_finish_read(img, 0, pix_raw, 0, 0)
   _png_image_free(img)
   free(img)
   if int(finish_ok) == 0 {
      free(pix_raw)
      return 0
   }
   def pix = init_str(pix_raw, pix_len)
   _png_decode_result(pix, w, h)
}

fn _png_free_tmp(any s) int { 0 }

fn _png_decode_result_clean(any pixels,
   int w,
   int h,
   any raw=0,
   any idat=0,
   any gray_lut=0,
   any palette_lut=0,
   any palette=0,
   any palette_alpha=0,
   any prev_row=0,
   any cur_row=0) dict {
   _png_free_tmp(raw)
   _png_free_tmp(idat)
   _png_free_tmp(gray_lut)
   _png_free_tmp(palette_lut)
   _png_free_tmp(palette)
   _png_free_tmp(palette_alpha)
   _png_free_tmp(prev_row)
   _png_free_tmp(cur_row)
   _png_decode_result(pixels, w, h)
}

fn _png_decode_fail(any msg,
   any raw=0,
   any idat=0,
   any palette=0,
   any palette_alpha=0,
   any gray_lut=0,
   any palette_lut=0,
   any prev_row=0,
   any cur_row=0,
   any pixels=0) any {
   _fail(msg)
}

fn _png_build_gray_lut(int bit_depth, int trns_gray) any {
   if bit_depth < 1 || bit_depth > 8 { return 0 }
   def count = 1 << bit_depth
   def lut = init_str(malloc(count * 4 + 32), count * 4)
   def maxv = count - 1
   mut i = 0
   while i < count {
      def v = (maxv > 0) ? int((i * 255 + (maxv >> 1)) / maxv) : 0
      def a = (trns_gray >= 0 && i == (trns_gray & maxv)) ? 0 : 255
      store32(lut, v | (v << 8) | (v << 16) | (a << 24), i * 4)
      i += 1
   }
   lut
}

fn _png_build_palette_lut(any palette, any palette_alpha) any {
   if !palette || palette.len == 0 || (palette.len % 3) != 0 { return 0 }
   def count = palette.len / 3
   def lut = init_str(malloc(count * 4 + 32), count * 4)
   mut i = 0
   while i < count {
      def r, g = load8(palette, i * 3), load8(palette, i * 3 + 1)
      def b = load8(palette, i * 3 + 2)
      mut a = 255
      if palette_alpha && i < palette_alpha.len { a = load8(palette_alpha, i) }
      store32(lut, r | (g << 8) | (b << 16) | (a << 24), i * 4)
      i += 1
   }
   lut
}

fn _png_adam7_pass_w(int w, int x0, int xstep) int {
   if w <= x0 { return 0 }
   (w - x0 + xstep - 1) / xstep
}

fn _png_adam7_pass_h(int h, int y0, int ystep) int {
   if h <= y0 { return 0 }
   (h - y0 + ystep - 1) / ystep
}

fn _png_adam7_expected_raw(int w, int h, int channels, int bytes_per_sample) int {
   mut total = 0
   mut pi = 0
   def pass_x0, pass_y0 = [0, 4, 0, 2, 0, 1, 0], [0, 0, 4, 0, 2, 0, 1]
   def pass_xs, pass_ys = [8, 8, 4, 4, 2, 2, 1], [8, 8, 8, 4, 4, 2, 2]
   while pi < 7 {
      def pw = _png_adam7_pass_w(w, pass_x0.get(pi, 0), pass_xs.get(pi, 1))
      def ph = _png_adam7_pass_h(h, pass_y0.get(pi, 0), pass_ys.get(pi, 1))
      if pw > 0 && ph > 0 { total += ph * (1 + pw * channels * bytes_per_sample) }
      pi += 1
   }
   total
}

fn _png_color_channels(int color_type) int {
   case color_type {
      6 -> 4
      4 -> 2
      2 -> 3
      0, 3 -> 1
      _ -> 0
   }
}

fn _png_bit_depth_ok(int color_type, int bit_depth) bool {
   case color_type {
      0 -> bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16
      2 -> bit_depth == 8 || bit_depth == 16
      3 -> bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8
      4, 6 -> bit_depth == 8 || bit_depth == 16
      _ -> false
   }
}

fn _png_bit_depth_error(int color_type, int bit_depth) str {
   case color_type {
      0 -> "unsupported grayscale bit depth " + to_str(bit_depth)
      2 -> "unsupported truecolor bit depth " + to_str(bit_depth)
      3 -> "unsupported palette bit depth " + to_str(bit_depth)
      4, 6 -> "unsupported alpha bit depth " + to_str(bit_depth)
      _ -> "unsupported color type " + to_str(color_type)
   }
}

fn _png_unfilter_row_8(any raw, int src_off, any cur, any prev, int stride, int bpp, int filter) bool {
   if filter == 0 {
      mut ci = 0
      while ci < stride {
         store8(cur, load8(raw, src_off + ci), ci)
         ci += 1
      }
      return true
   }
   mut x = 0
   if filter == 1 {
      while x < bpp {
         store8(cur, load8(raw, src_off + x), x)
         x += 1
      }
      while x < stride {
         store8(cur, (load8(raw, src_off + x) + load8(cur, x - bpp)) & 255, x)
         x += 1
      }
      return true
   }
   if filter == 2 {
      if !prev {
         mut ci = 0
         while ci < stride {
            store8(cur, load8(raw, src_off + ci), ci)
            ci += 1
         }
         return true
      }
      while x + 8 <= stride {
         store8(cur, (load8(raw, src_off + x) + load8(prev, x)) & 255, x)
         store8(cur, (load8(raw, src_off + x + 1) + load8(prev, x + 1)) & 255, x + 1)
         store8(cur, (load8(raw, src_off + x + 2) + load8(prev, x + 2)) & 255, x + 2)
         store8(cur, (load8(raw, src_off + x + 3) + load8(prev, x + 3)) & 255, x + 3)
         store8(cur, (load8(raw, src_off + x + 4) + load8(prev, x + 4)) & 255, x + 4)
         store8(cur, (load8(raw, src_off + x + 5) + load8(prev, x + 5)) & 255, x + 5)
         store8(cur, (load8(raw, src_off + x + 6) + load8(prev, x + 6)) & 255, x + 6)
         store8(cur, (load8(raw, src_off + x + 7) + load8(prev, x + 7)) & 255, x + 7)
         x += 8
      }
      while x < stride {
         store8(cur, (load8(raw, src_off + x) + load8(prev, x)) & 255, x)
         x += 1
      }
      return true
   }
   if filter == 3 {
      while x < stride {
         def left = (x >= bpp) ? load8(cur, x - bpp) : 0
         def above = prev ? load8(prev, x) : 0
         store8(cur, (load8(raw, src_off + x) + ((left + above) >> 1)) & 255, x)
         x += 1
      }
      return true
   }
   if filter == 4 {
      while x < stride {
         def a, b = (x >= bpp) ? load8(cur, x - bpp) : 0, prev ? load8(prev, x) : 0
         def c = (prev && x >= bpp) ? load8(prev, x - bpp) : 0
         store8(cur, (load8(raw, src_off + x) + _png_paeth(a, b, c)) & 255, x)
         x += 1
      }
      return true
   }
   false
}

fn _png_unfilter_raw_row_inplace(any raw, int raw_p, int stride, int bpp, int row_idx, int filter) bool {
   case filter {
      0 -> true
      1 -> {
         mut x = 0
         while x < stride {
            def left = (x >= bpp) ? load8(raw, raw_p + x - bpp) : 0
            store8(raw, (load8(raw, raw_p + x) + left) & 255, raw_p + x)
            x += 1
         }
         true
      }
      2 -> {
         if row_idx > 0 {
            def prev_row = raw_p - (stride + 1)
            mut x = 0
            while x + 4 <= stride {
               store8(raw, (load8(raw, raw_p + x) + load8(raw, prev_row + x)) & 255, raw_p + x)
               store8(raw, (load8(raw, raw_p + x + 1) + load8(raw, prev_row + x + 1)) & 255, raw_p + x + 1)
               store8(raw, (load8(raw, raw_p + x + 2) + load8(raw, prev_row + x + 2)) & 255, raw_p + x + 2)
               store8(raw, (load8(raw, raw_p + x + 3) + load8(raw, prev_row + x + 3)) & 255, raw_p + x + 3)
               x += 4
            }
            while x < stride {
               store8(raw, (load8(raw, raw_p + x) + load8(raw, prev_row + x)) & 255, raw_p + x)
               x += 1
            }
         }
         true
      }
      3 -> {
         def prev_row = raw_p - (stride + 1)
         mut x = 0
         while x < stride {
            def cur = load8(raw, raw_p + x)
            def left = (x >= bpp) ? load8(raw, raw_p + x - bpp) : 0
            def above = (row_idx > 0) ? load8(raw, prev_row + x) : 0
            store8(raw, (cur + ((left + above) >> 1)) & 255, raw_p + x)
            x += 1
         }
         true
      }
      4 -> {
         def prev_row = raw_p - (stride + 1)
         mut x = 0
         while x < stride {
            def cur = load8(raw, raw_p + x)
            def a = (x >= bpp) ? load8(raw, raw_p + x - bpp) : 0
            def b = (row_idx > 0) ? load8(raw, prev_row + x) : 0
            def c = (row_idx > 0 && x >= bpp) ? load8(raw, prev_row + x - bpp) : 0
            store8(raw, (cur + _png_paeth(a, b, c)) & 255, raw_p + x)
            x += 1
         }
         true
      }
      _ -> false
   }
}

fn _png_expand_row_rgba8(any dst, int dst_off, any src, int w) any {
   mut i, d = 0, dst_off
   while i < w {
      def s = i * 4
      store8(dst, load8(src, s), d)
      store8(dst, load8(src, s + 1), d + 1)
      store8(dst, load8(src, s + 2), d + 2)
      store8(dst, load8(src, s + 3), d + 3)
      i += 1
      d += 4
   }
}

fn _png_expand_row_rgb8(any dst, int dst_off, any src, int w) any {
   mut i, s = 0, 0
   mut d = dst_off
   while i + 4 <= w {
      store32(dst, _png_rgb_opaque32(src, s), d)
      store32(dst, _png_rgb_opaque32(src, s + 3), d + 4)
      store32(dst, _png_rgb_opaque32(src, s + 6), d + 8)
      store32(dst, _png_rgb_opaque32(src, s + 9), d + 12)
      i += 4
      s += 12
      d += 16
   }
   while i < w {
      store32(dst, _png_rgb_opaque32(src, s), d)
      i += 1
      s += 3
      d += 4
   }
}

fn _png_expand_row_rgb8_trns(any dst, int dst_off, any src, int w, int trns_r, int trns_g, int trns_b) any {
   def tr, tg = trns_r & 255, trns_g & 255
   def tb = trns_b & 255
   mut i, s = 0, 0
   mut d = dst_off
   while i < w {
      def r, g = load8(src, s), load8(src, s + 1)
      def b, a = load8(src, s + 2), (r == tr && g == tg && b == tb) ? 0 : 255
      store32(dst, r | (g << 8) | (b << 16) | (a << 24), d)
      i += 1
      s += 3
      d += 4
   }
}

fn _png_expand_row_gray8_lut(any dst, int dst_off, any src, int w, any gray_lut) any {
   mut i, d = 0, dst_off
   while i + 4 <= w {
      store32(dst, load32(gray_lut, load8(src, i) * 4), d)
      store32(dst, load32(gray_lut, load8(src, i + 1) * 4), d + 4)
      store32(dst, load32(gray_lut, load8(src, i + 2) * 4), d + 8)
      store32(dst, load32(gray_lut, load8(src, i + 3) * 4), d + 12)
      i += 4
      d += 16
   }
   while i < w {
      store32(dst, load32(gray_lut, load8(src, i) * 4), d)
      i += 1
      d += 4
   }
}

fn _png_expand_row_palette8_lut(any dst, int dst_off, any src, int w, any palette_lut) any {
   mut i, d = 0, dst_off
   while i + 4 <= w {
      store32(dst, load32(palette_lut, load8(src, i) * 4), d)
      store32(dst, load32(palette_lut, load8(src, i + 1) * 4), d + 4)
      store32(dst, load32(palette_lut, load8(src, i + 2) * 4), d + 8)
      store32(dst, load32(palette_lut, load8(src, i + 3) * 4), d + 12)
      i += 4
      d += 16
   }
   while i < w {
      store32(dst, load32(palette_lut, load8(src, i) * 4), d)
      i += 1
      d += 4
   }
}

fn _png_expand_row_gray_alpha8(any dst, int dst_off, any src, int w) any {
   mut i, s = 0, 0
   mut d = dst_off
   while i < w {
      def v, a = load8(src, s), load8(src, s + 1)
      store32(dst, v | (v << 8) | (v << 16) | (a << 24), d)
      i += 1
      s += 2
      d += 4
   }
}

fn _png_decode_fast_rows8(
   any raw, int raw_len, any pixels, int w, int h, int stride,
   int bytes_per_pixel, int color_type, int trns_r, int trns_g, int trns_b,
   any idat, any palette, any palette_alpha, any gray_lut, any palette_lut
) list {
   mut prev_row = init_str(malloc(stride + 32), stride)
   mut cur_row = init_str(malloc(stride + 32), stride)
   if !prev_row || !cur_row { return [true, _png_decode_fail("row buffer alloc failed", raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
   memset(prev_row, 0, stride)
   mut y = 0
   mut raw_p = 0
   while y < h {
      if raw_p >= raw_len { return [true, _png_decode_fail("raw underrun before row " + to_str(y), raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
      def filter = load8(raw, raw_p)
      raw_p += 1
      if raw_p + stride > raw_len { return [true, _png_decode_fail("raw underrun row payload " + to_str(y), raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
      if !_png_unfilter_row_8(raw, raw_p, cur_row, (y > 0) ? prev_row : 0, stride, bytes_per_pixel, filter) { return [true, _png_decode_fail("unsupported filter " + to_str(filter), raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
      def dst_row = y * w * 4
      if color_type == 6 {
         _png_expand_row_rgba8(pixels, dst_row, cur_row, w)
      } elif color_type == 2 {
         if trns_r < 0 { _png_expand_row_rgb8(pixels, dst_row, cur_row, w) }
         else { _png_expand_row_rgb8_trns(pixels, dst_row, cur_row, w, trns_r, trns_g, trns_b) }
      } elif color_type == 0 {
         if !gray_lut { return [true, _png_decode_fail("missing gray_lut", raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
         _png_expand_row_gray8_lut(pixels, dst_row, cur_row, w, gray_lut)
      } elif color_type == 3 {
         if !palette_lut { return [true, _png_decode_fail("missing palette_lut", raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
         _png_expand_row_palette8_lut(pixels, dst_row, cur_row, w, palette_lut)
      } elif color_type == 4 {
         _png_expand_row_gray_alpha8(pixels, dst_row, cur_row, w)
      } else {
         _png_free_tmp(prev_row)
         _png_free_tmp(cur_row)
         return [false, 0]
      }
      raw_p += stride
      def swap_row = prev_row
      prev_row = cur_row
      cur_row = swap_row
      y += 1
   }
   [true, _png_decode_result_clean(pixels, w, h, raw, idat, gray_lut, palette_lut, palette, palette_alpha, prev_row, cur_row)]
}

fn _png_decode_packed_rows(
   any raw, int raw_len, any pixels, int w, int h, int stride,
   int bit_depth, int color_type,
   any idat, any palette, any palette_alpha, any gray_lut, any palette_lut
) list {
   mut prev_row = init_str(malloc(stride + 32), stride)
   mut cur_row = init_str(malloc(stride + 32), stride)
   if !prev_row || !cur_row { return [true, _png_decode_fail("row buffer alloc failed", raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
   memset(prev_row, 0, stride)
   mut y = 0
   mut raw_p = 0
   def packed_mask = (1 << bit_depth) - 1
   while y < h {
      if raw_p >= raw_len { return [true, _png_decode_fail("raw underrun before packed row " + to_str(y), raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
      def filter = load8(raw, raw_p)
      raw_p += 1
      if raw_p + stride > raw_len { return [true, _png_decode_fail("raw underrun packed row payload " + to_str(y), raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
      if !_png_unfilter_row_8(raw, raw_p, cur_row, (y > 0) ? prev_row : 0, stride, 1, filter) { return [true, _png_decode_fail("unsupported filter " + to_str(filter), raw, idat, palette, palette_alpha, gray_lut, palette_lut, prev_row, cur_row, pixels)] }
      mut i = 0
      while i < w {
         def dst_off = (y * w + i) * 4
         def packed_off = (i * bit_depth) / 8
         def packed_shift = 8 - bit_depth - ((i * bit_depth) & 7)
         if color_type == 0 {
            def s = (load8(cur_row, packed_off) >> packed_shift) & packed_mask
            if gray_lut { store32(pixels, load32(gray_lut, s * 4), dst_off) }
         } else {
            def idx = (load8(cur_row, packed_off) >> packed_shift) & packed_mask
            if palette_lut { store32(pixels, load32(palette_lut, idx * 4), dst_off) }
         }
         i += 1
      }
      raw_p += stride
      def swap_row = prev_row
      prev_row = cur_row
      cur_row = swap_row
      y += 1
   }
   [true, _png_decode_result_clean(pixels, w, h, raw, idat, gray_lut, palette_lut, palette, palette_alpha, prev_row, cur_row)]
}

fn _join_chunks(any chunks) any {
   mut total = 0
   mut i = 0
   def n = chunks.len
   while i < n {
      total += len(chunks.get(i))
      i += 1
   }
   if total == 0 { return "" }
   def res_p = malloc(total + 32)
   if !res_p { return "" }
   def res = init_str(res_p, total)
   mut off = 0
   i = 0
   while i < n {
      def ch = chunks.get(i)
      def clen = ch.len
      if clen > 0 {
         __copy_mem(to_int(res) + off, to_int(ch), clen)
         off += clen
      }
      i += 1
   }
   res
}

fn _png_scan_decode_chunks(any data) dict {
   mut p = 8
   mut idat_list = list(4)
   mut w, h, bit_depth, color_type = 0, 0, 0, 0
   mut seen_ihdr = false
   mut palette, palette_alpha = 0, 0
   mut trns_gray, trns_r, trns_g, trns_b = -1, -1, -1, -1
   mut interlace_method = 0
   while p + 8 <= data.len {
      def length = _u32be(data, p)
      def chunk_data = p + 8
      def next_p = chunk_data + length + 4
      if next_p > data.len { return {"ok": false, "error": "chunk overflow at " + to_str(p) + " len=" + to_str(length)} }
      if _png_validate_crc_enabled() {
         def cs = init_str(malloc(length + 4 + 32), length + 4)
         store8(cs, load8(data, p + 4), 0)
         store8(cs, load8(data, p + 5), 1)
         store8(cs, load8(data, p + 6), 2)
         store8(cs, load8(data, p + 7), 3)
         if length > 0 { __copy_mem(to_int(cs) + 4, to_int(data) + chunk_data, length) }
         def want_crc = math_hash.crc32(cs, 0, 0)
         def got_crc = _u32be(data, next_p - 4)
         if want_crc != got_crc { return {"ok": false, "error": "crc mismatch at " + to_str(p)} }
      }
      if _png_chunk_is(data, p, 73, 72, 68, 82) {
         if length < 13 { return {"ok": false, "error": "short IHDR"} }
         seen_ihdr = true
         w = _u32be(data, chunk_data)
         h = _u32be(data, chunk_data + 4)
         bit_depth = load8(data, chunk_data + 8)
         color_type = load8(data, chunk_data + 9)
         def compression_method = load8(data, chunk_data + 10)
         def filter_method = load8(data, chunk_data + 11)
         interlace_method = load8(data, chunk_data + 12)
         if compression_method != 0 || filter_method != 0 || (interlace_method != 0 && interlace_method != 1) { return {"ok": false, "error": "unsupported IHDR methods"} }
         if bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8 && bit_depth != 16 { return {"ok": false, "error": "unsupported bit depth " + to_str(bit_depth)} }
      } elif _png_chunk_is(data, p, 80, 76, 84, 69) {
         palette = init_str(malloc(length + 32), length)
         if length > 0 { __copy_mem(to_int(palette), to_int(data) + chunk_data, length) }
      } elif _png_chunk_is(data, p, 116, 82, 78, 83) {
         if color_type == 3 {
            palette_alpha = init_str(malloc(length + 32), length)
            if length > 0 { __copy_mem(to_int(palette_alpha), to_int(data) + chunk_data, length) }
         } elif color_type == 0 && length >= 2 {
            trns_gray = _u16be(data, chunk_data)
         } elif color_type == 2 && length >= 6 {
            trns_r, trns_g = _u16be(data, chunk_data), _u16be(data, chunk_data + 2)
            trns_b = _u16be(data, chunk_data + 4)
         }
      } elif _png_chunk_is(data, p, 73, 68, 65, 84) {
         def chunk = init_str(malloc(length + 32), length)
         if length > 0 { __copy_mem(to_int(chunk), to_int(data) + chunk_data, length) }
         idat_list = idat_list.append(chunk)
      } elif _png_chunk_is(data, p, 73, 69, 78, 68) {
         break
      }
      p = next_p
   }
   if !seen_ihdr { return {"ok": false, "error": "missing IHDR"} }
   {
      "ok": true, "idat_list": idat_list, "width": w, "height": h,
      "bit_depth": bit_depth, "color_type": color_type,
      "palette": palette, "palette_alpha": palette_alpha,
      "trns_gray": trns_gray, "trns_r": trns_r, "trns_g": trns_g, "trns_b": trns_b,
      "interlace_method": interlace_method
   }
}

fn _png_signature_ok(any data) bool {
   is_str(data) && data.len >= 8
   && load8(data, 0) == 137 && load8(data, 1) == 80 && load8(data, 2) == 78 && load8(data, 3) == 71
   && load8(data, 4) == 13 && load8(data, 5) == 10 && load8(data, 6) == 26 && load8(data, 7) == 10
}

fn _png_join_idat_list(any idat_list) any {
   mut idat = _join_chunks(idat_list)
   def idat_list_n = idat_list.len
   mut ci = 0
   while ci < idat_list_n {
      _png_free_tmp(idat_list.get(ci))
      ci += 1
   }
   idat
}

fn _png_decode_stride(int w, int channels, int bit_depth, int bytes_per_sample) int {
   if bit_depth < 8 { return((w * channels * bit_depth) + 7) / 8 }
   w * channels * bytes_per_sample
}

fn _png_expected_raw(int w, int h, int channels, int bytes_per_sample, int stride, int interlace_method) int {
   if interlace_method == 1 { return _png_adam7_expected_raw(w, h, channels, bytes_per_sample) }
   (stride + 1) * h
}

fn _png_copy_raw(any raw_src, int raw_len) any {
   mut raw = init_str(malloc(raw_len + 32), raw_len)
   if !raw { return 0 }
   mut raw_i = 0
   while raw_i < raw_len {
      store8(raw, load8(raw_src, raw_i), raw_i)
      raw_i += 1
   }
   raw
}

fn _png_decode_raw_payload(any idat, int expect_raw, any palette=0, any palette_alpha=0) any {
   if _png_no_decompress_enabled() { return 0 }
   def raw_src = zlib.decompress_zlib_limit(idat, expect_raw)
   if !raw_src { return _png_decode_fail("zlib decompress failed/empty", raw_src, idat, palette, palette_alpha) }
   def raw_len = raw_src.len
   if raw_len == 0 || len(zlib.error()) > 0 { return _png_decode_fail("zlib decompress failed/empty", raw_src, idat, palette, palette_alpha) }
   if raw_len < expect_raw { return _png_decode_fail("zlib output too short", raw_src, idat, palette, palette_alpha) }
   def raw = _png_copy_raw(raw_src, raw_len)
   if !raw { return _png_decode_fail("raw buffer alloc failed", raw_src, idat, palette, palette_alpha) }
   if _png_stage_debug_enabled() {
      print("[png] stage: after decompress raw_len=" + to_str(raw_len) + " expect=" + to_str(expect_raw))
      print("[png] stage: raw0=" + to_str(load8(raw, 0)) + " raw1=" + to_str(load8(raw, 1)) + " raw2=" + to_str(load8(raw, 2)) + " raw3=" + to_str(load8(raw, 3)))
   }
   raw
}

fn _png_decode_luts(int color_type,
   int bit_depth,
   int trns_gray,
   any palette,
   any palette_alpha,
   any raw,
   any idat,
   any pixels) list {
   def gray_lut = (color_type == 0 && bit_depth <= 8) ? _png_build_gray_lut(bit_depth, trns_gray) : 0
   def palette_lut = (color_type == 3) ? _png_build_palette_lut(palette, palette_alpha) : 0
   if color_type == 3 && !palette_lut { return [false, _png_decode_fail("palette lookup build failed", raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels)] }
   mut keep_palette = palette
   mut keep_palette_alpha = palette_alpha
   if gray_lut || palette_lut {
      _png_free_tmp(keep_palette)
      _png_free_tmp(keep_palette_alpha)
      keep_palette = 0
      keep_palette_alpha = 0
   }
   [true, 0, gray_lut, palette_lut, keep_palette, keep_palette_alpha]
}

fn _png_try_fast_decode_paths(any raw,
   int raw_len,
   any pixels,
   int w,
   int h,
   int stride,
   int bytes_per_pixel,
   int bit_depth,
   int color_type,
   int interlace_method,
   int trns_r,
   int trns_g,
   int trns_b,
   any idat,
   any palette,
   any palette_alpha,
   any gray_lut,
   any palette_lut) list {
   if interlace_method == 0 && bit_depth == 8 {
      if _png_stage_debug_enabled() { print("[png] stage: fast path") }
      def fast8 = _png_decode_fast_rows8(raw, raw_len, pixels, w, h, stride, bytes_per_pixel, color_type, trns_r, trns_g, trns_b, idat, palette, palette_alpha, gray_lut, palette_lut)
      if fast8.get(0, false) { return [true, fast8.get(1, 0)] }
   }
   if interlace_method == 0 && bit_depth < 8 && (color_type == 0 || color_type == 3) {
      if _png_stage_debug_enabled() { print("[png] stage: packed fast path") }
      def packed = _png_decode_packed_rows(raw, raw_len, pixels, w, h, stride, bit_depth, color_type, idat, palette, palette_alpha, gray_lut, palette_lut)
      if packed.get(0, false) { return [true, packed.get(1, 0)] }
   }
   [false, 0]
}

fn _png_store_adam7_pixel(any pixels,
   any raw,
   int src_off,
   int dst_off,
   int bit_depth,
   int color_type,
   int trns_r,
   any gray_lut,
   any palette_lut) any {
   if bit_depth == 8 && color_type == 2 && trns_r < 0 {
      store8(pixels, load8(raw, src_off), dst_off)
      store8(pixels, load8(raw, src_off + 1), dst_off + 1)
      store8(pixels, load8(raw, src_off + 2), dst_off + 2)
      store8(pixels, 255, dst_off + 3)
   } elif bit_depth == 8 && color_type == 6 {
      store32(pixels, load32(raw, src_off), dst_off)
   } elif bit_depth == 8 && color_type == 0 && gray_lut {
      def gv = load8(raw, src_off)
      store32(pixels, load32(gray_lut, gv * 4), dst_off)
   } elif bit_depth == 8 && color_type == 3 && palette_lut {
      def idx = load8(raw, src_off)
      store32(pixels, load32(palette_lut, idx * 4), dst_off)
   } elif bit_depth == 8 && color_type == 4 {
      def v, a = load8(raw, src_off), load8(raw, src_off + 1)
      store8(pixels, v, dst_off)
      store8(pixels, v, dst_off + 1)
      store8(pixels, v, dst_off + 2)
      store8(pixels, a, dst_off + 3)
   }
}

fn _png_decode_adam7_rows(any raw,
   int raw_len,
   any pixels,
   int w,
   int h,
   int channels,
   int bytes_per_sample,
   int bytes_per_pixel,
   int bit_depth,
   int color_type,
   int trns_r,
   any idat,
   any palette,
   any palette_alpha,
   any gray_lut,
   any palette_lut) any {
   if _png_stage_debug_enabled() { print("[png] stage: interlace path") }
   def pass_x0, pass_y0 = [0, 4, 0, 2, 0, 1, 0], [0, 0, 4, 0, 2, 0, 1]
   def pass_xs, pass_ys = [8, 8, 4, 4, 2, 2, 1], [8, 8, 8, 4, 4, 2, 2]
   mut raw_p = 0
   mut pi = 0
   while pi < 7 {
      def x0, y0 = pass_x0.get(pi, 0), pass_y0.get(pi, 0)
      def xstep, ystep = pass_xs.get(pi, 1), pass_ys.get(pi, 1)
      def pw, ph = _png_adam7_pass_w(w, x0, xstep), _png_adam7_pass_h(h, y0, ystep)
      if pw > 0 && ph > 0 {
         def pass_stride = pw * channels * bytes_per_sample
         mut py = 0
         while py < ph {
            if raw_p >= raw_len { return _png_decode_fail("raw underrun before interlace pass " + to_str(pi), raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels) }
            def filter = load8(raw, raw_p)
            raw_p += 1
            if raw_p + pass_stride > raw_len { return _png_decode_fail("raw underrun interlace pass payload " + to_str(pi), raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels) }
            if !_png_unfilter_raw_row_inplace(raw, raw_p, pass_stride, bytes_per_pixel, py, filter) { return _png_decode_fail("unsupported filter " + to_str(filter), raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels) }
            mut px = 0
            while px < pw {
               def src_off = raw_p + px * channels * bytes_per_sample
               def dst_x = x0 + px * xstep
               def dst_y = y0 + py * ystep
               _png_store_adam7_pixel(pixels, raw, src_off, (dst_y * w + dst_x) * 4, bit_depth, color_type, trns_r, gray_lut, palette_lut)
               px += 1
            }
            raw_p += pass_stride
            py += 1
         }
      }
      pi += 1
   }
   if _png_stage_debug_enabled() { print("[png] stage: final return") }
   _png_decode_result(pixels, w, h)
}

fn _png_store_flat_pixel(any pixels,
   any raw,
   int raw_p,
   int w,
   int y,
   int i,
   int channels,
   int bytes_per_sample,
   int bit_depth,
   int color_type,
   int trns_gray,
   int trns_r,
   int trns_g,
   int trns_b,
   any gray_lut,
   any palette_lut) any {
   def dst_off = (y * w + i) * 4
   def src_off = raw_p + i * channels * bytes_per_sample
   if bit_depth < 8 {
      def packed_off = raw_p + ((i * bit_depth) / 8)
      def packed_shift = 8 - bit_depth - ((i * bit_depth) & 7)
      def packed_mask = (1 << bit_depth) - 1
      def idx = (load8(raw, packed_off) >> packed_shift) & packed_mask
      if color_type == 0 && gray_lut { store32(pixels, load32(gray_lut, idx * 4), dst_off) }
      elif color_type == 3 && palette_lut { store32(pixels, load32(palette_lut, idx * 4), dst_off) }
   } elif color_type == 2 {
      mut r, g = load8(raw, src_off), load8(raw, src_off + bytes_per_sample)
      mut b, a = load8(raw, src_off + 2 * bytes_per_sample), 255
      if bit_depth == 16 {
         r, g = load8(raw, src_off), load8(raw, src_off + 2)
         b = load8(raw, src_off + 4)
         if trns_r >= 0 {
            def rr, gg = _u16be(raw, src_off), _u16be(raw, src_off + 2)
            def bb = _u16be(raw, src_off + 4)
            if rr == trns_r && gg == trns_g && bb == trns_b { a = 0 }
         }
      } elif trns_r >= 0 {
         if r == (trns_r & 255) && g == (trns_g & 255) && b == (trns_b & 255) { a = 0 }
      }
      store8(pixels, r, dst_off)
      store8(pixels, g, dst_off + 1)
      store8(pixels, b, dst_off + 2)
      store8(pixels, a, dst_off + 3)
   } elif color_type == 6 {
      store32(pixels, load32(raw, src_off), dst_off)
   } elif color_type == 0 {
      mut v, a = load8(raw, src_off), 255
      if bit_depth == 16 {
         def vv = _u16be(raw, src_off)
         v = (vv > 255) ? 255 : vv
         if trns_gray >= 0 && vv == trns_gray { a = 0 }
      } elif trns_gray >= 0 && v == (trns_gray & 255) {
         a = 0
      }
      store8(pixels, v, dst_off)
      store8(pixels, v, dst_off + 1)
      store8(pixels, v, dst_off + 2)
      store8(pixels, a, dst_off + 3)
   } elif color_type == 4 {
      def v, a = load8(raw, src_off), load8(raw, src_off + bytes_per_sample)
      store8(pixels, v, dst_off)
      store8(pixels, v, dst_off + 1)
      store8(pixels, v, dst_off + 2)
      store8(pixels, a, dst_off + 3)
   } elif color_type == 3 {
      def idx = load8(raw, src_off)
      if palette_lut { store32(pixels, load32(palette_lut, idx * 4), dst_off) }
   }
}

fn _png_decode_flat_rows(any raw,
   int raw_len,
   any pixels,
   int w,
   int h,
   int stride,
   int channels,
   int bytes_per_sample,
   int bytes_per_pixel,
   int bit_depth,
   int color_type,
   int trns_gray,
   int trns_r,
   int trns_g,
   int trns_b,
   any idat,
   any palette,
   any palette_alpha,
   any gray_lut,
   any palette_lut) any {
   mut y = 0
   mut raw_p = 0
   while y < h {
      if raw_p >= raw_len { return _png_decode_fail("raw underrun before row " + to_str(y), raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels) }
      def filter = load8(raw, raw_p)
      if _png_stage_debug_enabled() && bit_depth < 8 && color_type == 3 && y < 8 { print("[png] row=" + to_str(y) + " raw_p=" + to_str(raw_p) + " filter=" + to_str(filter) + " stride=" + to_str(stride)) }
      raw_p += 1
      if raw_p + stride > raw_len { return _png_decode_fail("raw underrun row payload " + to_str(y), raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels) }
      if !_png_unfilter_raw_row_inplace(raw, raw_p, stride, bytes_per_pixel, y, filter) { return _png_decode_fail("unsupported filter " + to_str(filter), raw, idat, palette, palette_alpha, gray_lut, palette_lut, 0, 0, pixels) }
      mut i = 0
      while i < w {
         _png_store_flat_pixel(pixels, raw, raw_p, w, y, i, channels, bytes_per_sample, bit_depth, color_type, trns_gray, trns_r, trns_g, trns_b, gray_lut, palette_lut)
         i += 1
      }
      raw_p += stride
      y += 1
   }
   if _png_stage_debug_enabled() { print("[png] stage: final return") }
   _png_decode_result(pixels, w, h)
}

fn decode(any data) any {
   "Decodes decode."
   if !is_str(data) || data.len < 8 { return _fail("bad input type/size") }
   if !_png_signature_ok(data) { return _fail("bad signature") }
   if _png_stop_early_enabled() { return 0 }
   if !_png_disable_libpng() && !_png_has_sensitive_colorspace_chunks(data) {
      def fast = _png_decode_libpng(data)
      if fast { return fast }
   }
   def scan = _png_scan_decode_chunks(data)
   if !scan.get("ok", false) { return _fail(to_str(scan.get("error", "bad chunks"))) }
   def idat_list = scan.get("idat_list", [])
   def w = int(scan.get("width", 0))
   def h = int(scan.get("height", 0))
   def bit_depth = int(scan.get("bit_depth", 0))
   def color_type = int(scan.get("color_type", 0))
   mut palette = scan.get("palette", 0)
   mut palette_alpha = scan.get("palette_alpha", 0)
   def trns_gray = int(scan.get("trns_gray", -1))
   def trns_r = int(scan.get("trns_r", -1))
   def trns_g = int(scan.get("trns_g", -1))
   def trns_b = int(scan.get("trns_b", -1))
   def interlace_method = int(scan.get("interlace_method", 0))
   if color_type == 3 && (!palette || palette.len == 0) { return _fail("missing PLTE for palette image") }
   if _png_color_channels(color_type) > 0 && !_png_bit_depth_ok(color_type, bit_depth) { return _fail(_png_bit_depth_error(color_type, bit_depth)) }
   mut idat = _png_join_idat_list(idat_list)
   if idat.len == 0 {
      _png_free_tmp(palette)
      _png_free_tmp(palette_alpha)
      return _fail("missing IDAT")
   }
   def channels = _png_color_channels(color_type)
   if channels == 0 { return _png_decode_fail("unsupported color type " + to_str(color_type), 0, idat, palette, palette_alpha) }
   if bit_depth < 8 && color_type != 0 && color_type != 3 { return _png_decode_fail("packed depth with unsupported color type", 0, idat, palette, palette_alpha) }
   def bytes_per_sample = (bit_depth == 16) ? 2 : 1
   mut bytes_per_pixel = channels * bytes_per_sample
   if bytes_per_pixel < 1 { bytes_per_pixel = 1 }
   def stride = _png_decode_stride(w, channels, bit_depth, bytes_per_sample)
   def expect_raw = _png_expected_raw(w, h, channels, bytes_per_sample, stride, interlace_method)
   if expect_raw <= 0 || expect_raw > 512 * 1024 * 1024 { return _png_decode_fail("raw size invalid " + to_str(expect_raw), 0, idat, palette, palette_alpha) }
   def raw = _png_decode_raw_payload(idat, expect_raw, palette, palette_alpha)
   if !raw { return 0 }
   def raw_len = raw.len
   def pixels = init_str(malloc(w * h * 4 + 32), w * h * 4)
   if !pixels { return _png_decode_fail("pixel buffer alloc failed", raw, idat, palette, palette_alpha) }
   memset(pixels, 0, w * h * 4)
   def luts = _png_decode_luts(color_type, bit_depth, trns_gray, palette, palette_alpha, raw, idat, pixels)
   if !luts.get(0, false) { return luts.get(1, 0) }
   def gray_lut = luts.get(2, 0)
   def palette_lut = luts.get(3, 0)
   palette = luts.get(4, 0)
   palette_alpha = luts.get(5, 0)
   def fast_decode = _png_try_fast_decode_paths(raw, raw_len, pixels, w, h, stride, bytes_per_pixel, bit_depth, color_type, interlace_method, trns_r, trns_g, trns_b, idat, palette, palette_alpha, gray_lut, palette_lut)
   if fast_decode.get(0, false) { return fast_decode.get(1, 0) }
   if interlace_method == 1 { return _png_decode_adam7_rows(raw, raw_len, pixels, w, h, channels, bytes_per_sample, bytes_per_pixel, bit_depth, color_type, trns_r, idat, palette, palette_alpha, gray_lut, palette_lut) }
   _png_decode_flat_rows(raw, raw_len, pixels, w, h, stride, channels, bytes_per_sample, bytes_per_pixel, bit_depth, color_type, trns_gray, trns_r, trns_g, trns_b, idat, palette, palette_alpha, gray_lut, palette_lut)
}

fn _make_chunk(any chunk_type, any data) any {
   def length = data.len
   def total = 8 + length + 4
   def res_p = malloc(total + 32)
   if !res_p { return 0 }
   def res = init_str(res_p, total)
   _s32be(res, length, 0)
   store8(res, load8(to_int(chunk_type), 0), 4)
   store8(res, load8(to_int(chunk_type), 1), 5)
   store8(res, load8(to_int(chunk_type), 2), 6)
   store8(res, load8(to_int(chunk_type), 3), 7)
   if length > 0 { _copy_bytes_safe(res, 8, data, 0, length) }
   def cs = init_str(malloc(length + 4 + 17+15), length + 4)
   store8(cs, load8(to_int(chunk_type), 0), 0)
   store8(cs, load8(to_int(chunk_type), 1), 1)
   store8(cs, load8(to_int(chunk_type), 2), 2)
   store8(cs, load8(to_int(chunk_type), 3), 3)
   if length > 0 { _copy_bytes_safe(cs, 4, data, 0, length) }
   def crc = math_hash.crc32(cs, 0, 0)
   _png_free_tmp(cs)
   _s32be(res, crc, 8 + length)
   res
}

fn encode(any img) any {
   "Encodes encode."
   if !img { return 0 }
   def w, h = img.get("width"), img.get("height")
   def pixels = img.get("data")
   if !w || !h || !pixels { return 0 }
   def ch = img.get("channels", img.get("bpp", 4))
   if ch != 3 && ch != 4 { return 0 }
   def color_type = (ch == 4) ? 6 : 2
   def ihdr_p = malloc(13 + 32)
   if !ihdr_p { return 0 }
   def ihdr = init_str(ihdr_p, 13)
   _s32be(ihdr, w, 0)
   _s32be(ihdr, h, 4)
   store8(ihdr, 8, 8)
   store8(ihdr, color_type, 9)
   store8(ihdr, 0, 10)
   store8(ihdr, 0, 11)
   store8(ihdr, 0, 12)
   def stride = w * ch
   def raw_size = h * (stride + 1)
   def raw_p = malloc(raw_size + 32)
   if !raw_p {
      _png_free_tmp(ihdr)
      return 0
   }
   def raw = init_str(raw_p, raw_size)
   mut y = 0
   while y < h {
      def row_start = y * (stride + 1)
      def src_off = y * stride
      store8(raw, 0, row_start)
      _copy_bytes_safe(raw, row_start + 1, pixels, src_off, stride)
      y += 1
   }
   def compressed = zlib.compress_zlib(raw, _png_encode_level())
   if !compressed || compressed.len == 0 {
      _png_free_tmp(ihdr)
      _png_free_tmp(raw)
      return 0
   }
   def ihdr_chunk = _make_chunk("IHDR", ihdr)
   def idat_chunk = _make_chunk("IDAT", compressed)
   def iend_chunk = _make_chunk("IEND", "")
   if !ihdr_chunk || !idat_chunk || !iend_chunk {
      _png_free_tmp(ihdr)
      _png_free_tmp(raw)
      _png_free_tmp(ihdr_chunk)
      _png_free_tmp(idat_chunk)
      _png_free_tmp(iend_chunk)
      return 0
   }
   def total_file_len = 8 + ihdr_chunk.len + idat_chunk.len + iend_chunk.len
   def final_p = malloc(total_file_len + 32)
   if !final_p {
      _png_free_tmp(ihdr)
      _png_free_tmp(raw)
      _png_free_tmp(ihdr_chunk)
      _png_free_tmp(idat_chunk)
      _png_free_tmp(iend_chunk)
      return 0
   }
   def res = init_str(final_p, total_file_len)
   _copy_bytes_safe(res, 0, "\x89PNG\r\n\x1a\n", 0, 8)
   mut off = 8
   _copy_bytes_safe(res, off, ihdr_chunk, 0, ihdr_chunk.len)
   off += ihdr_chunk.len
   _copy_bytes_safe(res, off, idat_chunk, 0, idat_chunk.len)
   off += idat_chunk.len
   _copy_bytes_safe(res, off, iend_chunk, 0, iend_chunk.len)
   _png_free_tmp(ihdr)
   _png_free_tmp(raw)
   _png_free_tmp(ihdr_chunk)
   _png_free_tmp(idat_chunk)
   _png_free_tmp(iend_chunk)
   res
}

#main {
   fn byte_str(list bytes) any {
      def n = bytes.len
      def out = malloc(n + 1)
      assert(out != 0, "png byte string allocation")
      init_str(out, n)
      mut i = 0
      while i < n {
         store8(out, bytes.get(i), i)
         i += 1
      }
      store8(out, 0, n)
      out
   }
   def rgba = byte_str([
         255, 0, 0, 255,
         0, 255, 0, 255,
         0, 0, 255, 255,
         255, 255, 0, 128
   ])
   def encoded = encode({"width": 2, "height": 2, "channels": 4, "data": rgba})
   assert(is_str(encoded), "png encoded string")
   assert(load8(encoded, 0) == 137 && load8(encoded, 1) == 80 && load8(encoded, 2) == 78, "png signature")
   def decoded = decode(encoded)
   assert(is_dict(decoded), "png decoded dict")
   assert(decoded.get("width") == 2 && decoded.get("height") == 2 && decoded.get("channels") == 4, "png decoded shape")
   def data = decoded.get("data")
   assert(is_str(data) && data.len == 16, "png data size")
   assert(load8(data, 0) == 255 && load8(data, 1) == 0 && load8(data, 2) == 0 && load8(data, 3) == 255, "png first pixel")
   assert(load8(data, 12) == 255 && load8(data, 13) == 255 && load8(data, 14) == 0 && load8(data, 15) == 128, "png alpha pixel")
   print("✓ std.parse.img.png self-test passed")
}
