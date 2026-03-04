;; Keywords: image png rfc2083
;; Portable Network Graphics (PNG) Image Loader and Encoder for Nytrix
;; Reference:
;; - https://en.wikipedia.org/wiki/PNG
;; - https://www.rfc-editor.org/rfc/rfc2083.html

module std.image.format.png (
   decode, encode
)

use std.core *
use std.core.dict_mod *
use std.core.mem as core_mem
use std.math as math
use std.math.hash as math_hash
use std.enc.zlib as zlib

fn _png_paeth(a, b, c){
   "Internal: implements the Paeth predictor for PNG filtering."
   def p = a + b - c
   def pa = math.abs(p - a)
   def pb = math.abs(p - b)
   def pc = math.abs(p - c)
   if(pa <= pb && pa <= pc){ return a }
   if(pb <= pc){ return b }
   return c
}

fn _s32be(p, v, o){
   "Internal: stores a 32-bit big-endian integer `v` at `p + o`."
   store8(p, (v >> 24) & 255, o)
   store8(p, (v >> 16) & 255, o + 1)
   store8(p, (v >> 8) & 255, o + 2)
   store8(p, v & 255, o + 3)
}

fn _dbg(msg){
   "Internal: prints a debug message to stdout if `NY_PNG_DEBUG` is set."
   if(env("NY_PNG_DEBUG")){ print("PNG: " + msg) }
}

fn _fail(msg){
   "Internal: logs an error message and returns 0 to indicate failure."
   _dbg("fail: " + msg)
   0
}

fn decode(data){
   "Decodes a PNG image from a byte string."
   if(!is_str(data) || len(data) < 8){ return _fail("bad input type/size") }
   if(load8(data, 0) != 137 || load8(data, 1) != 80 || load8(data, 2) != 78 || load8(data, 3) != 71){ return _fail("bad signature") }
   mut p = 8
   mut idat_list = list(4)
   mut w = 0
   mut h = 0
   mut bit_depth = 0
   mut color_type = 0
   mut palette = 0
   mut palette_alpha = 0
   mut trns_gray = -1
   mut trns_r = -1
   mut trns_g = -1
   mut trns_b = -1
   while(p + 8 <= len(data)){
      def length = (load8(data, p) << 24) | (load8(data, p + 1) << 16) | (load8(data, p + 2) << 8) | load8(data, p + 3)
      def chunk_type = (load8(data, p + 4) << 24) | (load8(data, p + 5) << 16) | (load8(data, p + 6) << 8) | load8(data, p + 7)
      def chunk_data = p + 8
      def next_p = chunk_data + length + 4
      if(next_p > len(data)){ return _fail("chunk overflow at " + to_str(p) + " len=" + to_str(length)) }
      if(chunk_type == 1229472850){
         if(length < 13){ return _fail("short IHDR") }
         w = (load8(data, chunk_data) << 24) | (load8(data, chunk_data + 1) << 16) | (load8(data, chunk_data + 2) << 8) | load8(data, chunk_data + 3)
         h = (load8(data, chunk_data + 4) << 24) | (load8(data, chunk_data + 5) << 16) | (load8(data, chunk_data + 6) << 8) | load8(data, chunk_data + 7)
         bit_depth = load8(data, chunk_data + 8)
         color_type = load8(data, chunk_data + 9)
         def compression_method = load8(data, chunk_data + 10)
         def filter_method = load8(data, chunk_data + 11)
         def interlace_method = load8(data, chunk_data + 12)
         if(compression_method != 0 || filter_method != 0 || interlace_method != 0){ return _fail("unsupported IHDR methods") }
         if(bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8 && bit_depth != 16){
         return _fail("unsupported bit depth " + to_str(bit_depth))
         }
      } elif(chunk_type == 1347179589){
         palette = init_str(malloc(length + 1 + 16) + 16, length)
         if(length > 0){ __copy_mem(palette, data + chunk_data, length) }
      } elif(chunk_type == 1951551059){
         if(color_type == 3){
         palette_alpha = init_str(malloc(length + 1 + 16) + 16, length)
         if(length > 0){ __copy_mem(palette_alpha, data + chunk_data, length) }
         } elif(color_type == 0 && length >= 2){
         trns_gray = (load8(data, chunk_data) << 8) | load8(data, chunk_data + 1)
         } elif(color_type == 2 && length >= 6){
         trns_r = (load8(data, chunk_data) << 8) | load8(data, chunk_data + 1)
         trns_g = (load8(data, chunk_data + 2) << 8) | load8(data, chunk_data + 3)
         trns_b = (load8(data, chunk_data + 4) << 8) | load8(data, chunk_data + 5)
         }
      } elif(chunk_type == 1229209940){
         mut chunk = init_str(malloc(length + 1 + 16) + 16, length)
         if(length > 0){ __copy_mem(chunk, data + chunk_data, length) }
         idat_list = append(idat_list, chunk)
      } elif(chunk_type == 1229278788){
         break
      }
      p = next_p
   }
   def idat = _join_chunks(idat_list)
   if(len(idat) == 0){ return _fail("missing IDAT") }
   mut channels = 3
   if(color_type == 6){ channels = 4 }
   elif(color_type == 4){ channels = 2 }
   elif(color_type == 2){ channels = 3 }
   elif(color_type == 0){ channels = 1 }
   elif(color_type == 3){ channels = 1 }
   else { return _fail("unsupported color type " + to_str(color_type)) }
   if(bit_depth < 8 && color_type != 0 && color_type != 3){ return _fail("packed depth with unsupported color type") }
   def bytes_per_sample = (bit_depth == 16) ? 2 : 1
   mut bytes_per_pixel = channels * bytes_per_sample
   if(bytes_per_pixel < 1){ bytes_per_pixel = 1 }
   mut stride = 0
   if(bit_depth < 8){
      stride = ((w * channels * bit_depth) + 7) / 8
   } else {
      stride = w * channels * bytes_per_sample
   }
   ;; Raw stream is one filter byte + stride per row.
   def expect_raw = (stride + 1) * h
   if(expect_raw <= 0 || expect_raw > 512 * 1024 * 1024){ return _fail("raw size invalid " + to_str(expect_raw)) }
   def raw = zlib.decompress_zlib_limit(idat, expect_raw)
   if(len(raw) == 0){ return _fail("zlib decompress failed/empty") }
   def pixels = init_str(malloc(w * h * 4 + 1 + 16) + 16, w * h * 4)
   memset(pixels, 0, w * h * 4)
   mut y = 0
   mut raw_p = 0
   while(y < h){
      if(raw_p >= len(raw)){ return _fail("raw underrun before row " + to_str(y)) }
      def filter = load8(raw, raw_p)
      raw_p += 1
      if(raw_p + stride > len(raw)){ return _fail("raw underrun row payload " + to_str(y)) }
      mut x = 0
      while(x < stride){
         def cur = load8(raw, raw_p + x)
         def a = (x >= bytes_per_pixel) ? load8(raw, raw_p + x - bytes_per_pixel) : 0
         def b = (y > 0) ? load8(raw, raw_p + x - (stride + 1)) : 0
         def c = (y > 0 && x >= bytes_per_pixel) ? load8(raw, raw_p + x - (stride + 1) - bytes_per_pixel) : 0
         mut val = 0
         if(filter == 0){ val = cur }
         elif(filter == 1){ val = (cur + a) & 255 }
         elif(filter == 2){ val = (cur + b) & 255 }
         elif(filter == 3){ val = (cur + (a + b) / 2) & 255 }
         elif(filter == 4){ val = (cur + _png_paeth(a, b, c)) & 255 }
         store8(raw, val, raw_p + x)
         x += 1
      }
      mut i = 0
      while(i < w){
         def dst_off = (y * w + i) * 4
         def src_off = raw_p + i * channels * bytes_per_sample
         if(bit_depth < 8){
         if(color_type == 0){
               def packed_off = raw_p + ((i * bit_depth) / 8)
               def packed_shift = 8 - bit_depth - ((i * bit_depth) & 7)
               def packed_mask = (1 << bit_depth) - 1
               def s = (load8(raw, packed_off) >> packed_shift) & packed_mask
               mut v = 0
               if(packed_mask > 0){
                  v = ((s * 255) + (packed_mask / 2)) / packed_mask
               }
               mut a = 255
               if(trns_gray >= 0 && s == (trns_gray & ((1 << bit_depth) - 1))){ a = 0 }
               store8(pixels, v, dst_off)
               store8(pixels, v, dst_off + 1)
               store8(pixels, v, dst_off + 2)
               store8(pixels, a, dst_off + 3)
         } elif(color_type == 3){
               def packed_off = raw_p + ((i * bit_depth) / 8)
               def packed_shift = 8 - bit_depth - ((i * bit_depth) & 7)
               def packed_mask = (1 << bit_depth) - 1
               def idx = (load8(raw, packed_off) >> packed_shift) & packed_mask
               if(palette && idx * 3 + 2 < len(palette)){
                  store8(pixels, load8(palette, idx * 3), dst_off)
                  store8(pixels, load8(palette, idx * 3 + 1), dst_off + 1)
                  store8(pixels, load8(palette, idx * 3 + 2), dst_off + 2)
                  mut a = 255
                  if(palette_alpha && idx < len(palette_alpha)){ a = load8(palette_alpha, idx) }
                  store8(pixels, a, dst_off + 3)
               }
         }
         } elif(color_type == 2){
         mut r = load8(raw, src_off)
         mut g = load8(raw, src_off + bytes_per_sample)
         mut b = load8(raw, src_off + 2 * bytes_per_sample)
         mut a = 255
         if(bit_depth == 16){
               r = load8(raw, src_off)
               g = load8(raw, src_off + 2)
               b = load8(raw, src_off + 4)
               if(trns_r >= 0){
                  def rr = (load8(raw, src_off) << 8) | load8(raw, src_off + 1)
                  def gg = (load8(raw, src_off + 2) << 8) | load8(raw, src_off + 3)
                  def bb = (load8(raw, src_off + 4) << 8) | load8(raw, src_off + 5)
                  if(rr == trns_r && gg == trns_g && bb == trns_b){ a = 0 }
               }
         } elif(trns_r >= 0){
               if(r == (trns_r & 255) && g == (trns_g & 255) && b == (trns_b & 255)){ a = 0 }
         }
         store8(pixels, r, dst_off)
         store8(pixels, g, dst_off + 1)
         store8(pixels, b, dst_off + 2)
         store8(pixels, a, dst_off + 3)
         } elif(color_type == 6){
         store8(pixels, load8(raw, src_off), dst_off)
         store8(pixels, load8(raw, src_off + bytes_per_sample), dst_off + 1)
         store8(pixels, load8(raw, src_off + 2 * bytes_per_sample), dst_off + 2)
         store8(pixels, load8(raw, src_off + 3 * bytes_per_sample), dst_off + 3)
         } elif(color_type == 0){
         mut v = load8(raw, src_off)
         mut a = 255
         if(bit_depth == 16){
               v = load8(raw, src_off)
               if(trns_gray >= 0 && (((load8(raw, src_off) << 8) | load8(raw, src_off + 1)) == trns_gray)){ a = 0 }
         } elif(trns_gray >= 0 && v == (trns_gray & 255)){
               a = 0
         }
         store8(pixels, v, dst_off)
         store8(pixels, v, dst_off + 1)
         store8(pixels, v, dst_off + 2)
         store8(pixels, a, dst_off + 3)
         } elif(color_type == 4){
         def v = load8(raw, src_off)
         def a = load8(raw, src_off + bytes_per_sample)
         store8(pixels, v, dst_off)
         store8(pixels, v, dst_off + 1)
         store8(pixels, v, dst_off + 2)
         store8(pixels, a, dst_off + 3)
         } elif(color_type == 3){
         def idx = load8(raw, src_off)
         if(palette && idx * 3 + 2 < len(palette)){
               store8(pixels, load8(palette, idx * 3), dst_off)
               store8(pixels, load8(palette, idx * 3 + 1), dst_off + 1)
               store8(pixels, load8(palette, idx * 3 + 2), dst_off + 2)
               mut a = 255
               if(palette_alpha && idx < len(palette_alpha)){ a = load8(palette_alpha, idx) }
               store8(pixels, a, dst_off + 3)
         }
         }
         i += 1
      }
      raw_p += stride
      y += 1
   }
   mut res = dict(4)
   res = dict_set(res, "data", pixels)
   res = dict_set(res, "width", w)
   res = dict_set(res, "height", h)
   res = dict_set(res, "channels", 4)
   res
}

fn encode(img){
   "Encodes an image dictionary to PNG. Supports RGB and RGBA."
   if(!img){ return 0 }
   def w = dict_get(img, "width")
   def h = dict_get(img, "height")
   def pixels = dict_get(img, "data")
   if(!w || !h || !pixels){ return 0 }

   def ch = dict_get(img, "channels", dict_get(img, "bpp", 4))
   def color_type = (ch == 4) ? 6 : 2

   def ihdr_p = malloc(13 + 1 + 16) + 16
   _s32be(ihdr_p, w, 0)
   _s32be(ihdr_p, h, 4)
   store8(ihdr_p, 8, 8) ; Bit depth 8
   store8(ihdr_p, color_type, 9)
   store8(ihdr_p, 0, 10) ; Compression (deflate)
   store8(ihdr_p, 0, 11) ; Filter (none)
   store8(ihdr_p, 0, 12) ; Interlace (none)
   def ihdr = init_str(ihdr_p, 13)

   def stride = w * ch
   def raw_size = h * (stride + 1)
   def raw_p = malloc(raw_size + 1 + 16) + 16
   if(!raw_p){ return 0 }
   memset(raw_p, 0, raw_size)
   ;if(true){ ; Debug first pixel
   ;   def r = load8(pixels, 0)
   ;   def g = load8(pixels, 1)
   ;   def b = load8(pixels, 2)
   ;   print(f"PNG: Encoding first pixel R={r} G={g} B={b}")
   ;}

   mut y = 0
   while(y < h){
      def row_start = y * (stride + 1)
      def src_off = y * stride
      store8(raw_p, 0, row_start) ; Filter 0

      mut x = 0
      while(x < w){
         def sx = src_off + x * ch
         def dx = row_start + 1 + x * ch
         store8(raw_p, load8(pixels, sx), dx)
         store8(raw_p, load8(pixels, sx + 1), dx + 1)
         store8(raw_p, load8(pixels, sx + 2), dx + 2)
         if(ch == 4){ store8(raw_p, load8(pixels, sx + 3), dx + 3) }
         x += 1
      }
      y += 1
   }

   def raw = init_str(raw_p, raw_size)
   def compressed = zlib.compress(raw)
   if(!compressed || str_len(compressed) == 0){ return 0 }

   def ihdr_chunk = _make_chunk("IHDR", ihdr)
   def idat_chunk = _make_chunk("IDAT", compressed)
   def iend_chunk = _make_chunk("IEND", "")

   def sig = "\x89PNG\r\n\x1a\n"
   def total_file_len = 8 + str_len(ihdr_chunk) + str_len(idat_chunk) + str_len(iend_chunk)
   def final_p = malloc(total_file_len + 1 + 16) + 16

   mut off = 0
   core_mem.memcpy(to_int(final_p) + off, sig, 8)
   off = off + 8
   core_mem.memcpy(to_int(final_p) + off, ihdr_chunk, str_len(ihdr_chunk))
   off = off + str_len(ihdr_chunk)
   core_mem.memcpy(to_int(final_p) + off, idat_chunk, str_len(idat_chunk))
   off = off + str_len(idat_chunk)
   core_mem.memcpy(to_int(final_p) + off, iend_chunk, str_len(iend_chunk))

   def res = init_str(final_p, total_file_len)
   res
}

fn _join_chunks(chunks){
   "Internal: concatenates a list of IDAT byte chunks into a single string."
   mut total = 0
   mut i = 0
   def n = len(chunks)
   while(i < n){
      total = total + len(get(chunks, i))
      i += 1
   }
   if(total == 0){ return "" }

   def res_p = malloc(total + 1 + 16) + 16
   mut p = 0
   i = 0
   while(i < n){
      def ch = get(chunks, i)
      def clen = len(ch)
      core_mem.memcpy(res_p + p, ch, clen)
      p = p + clen
      i += 1
   }
   init_str(res_p, total)
}

fn _make_chunk(type, data){
   "Internal: wraps `data` into a PNG chunk of `type` with length and CRC32."
   def length = str_len(data)
   def total = 8 + length + 4
   def res_p = malloc(total + 1 + 16) + 16
   if(!res_p){ return 0 }
   _s32be(res_p, length, 0)
   store8(res_p, load8(to_int(type), 0), 4)
   store8(res_p, load8(to_int(type), 1), 5)
   store8(res_p, load8(to_int(type), 2), 6)
   store8(res_p, load8(to_int(type), 3), 7)

   if(length > 0){
      core_mem.memcpy(to_int(res_p) + 8, to_int(data), length)
   }

   def cs = init_str(malloc(length + 4 + 1 + 16) + 16, length + 4)
   store8(cs, load8(to_int(type), 0), 0)
   store8(cs, load8(to_int(type), 1), 1)
   store8(cs, load8(to_int(type), 2), 2)
   store8(cs, load8(to_int(type), 3), 3)
   if(length > 0){
      core_mem.memcpy(to_int(cs) + 4, to_int(data), length)
   }
   def crc = math_hash.crc32(cs)
   free(to_int(cs) - 16)

   _s32be(res_p, crc, 8 + length)
   init_str(res_p, total)
}

fn store32_be(s, v, i){
   "Utility: stores 32-bit integer `v` into string `s` at index `i` in Big-Endian format."
   store8(s, (v >> 24) & 255, i)
   store8(s, (v >> 16) & 255, i + 1)
   store8(s, (v >> 8) & 255, i + 2)
   store8(s, v & 255, i + 3)
}

if(comptime{__main()}){
   use std.core.error *
   use std.parse.bin as pbin

   def png_list = [137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,0,0,0,0,58,126,1,19,0,0,0,10,73,68,65,84,120,156,99,96,0,0,0,2,0,1,226,33,188,51,0,0,0,0,73,69,78,68,174,66,96,130]
   def png_data = pbin.from_list(png_list)

   if(!zlib.available()){
      print("✓ std.image.png tests skipped (zlib missing)")
      return
   }

   def img = decode(png_data)
   assert(img != 0, "png decode")
   assert(dict_get(img, "width") == 1, "png width")
   assert(dict_get(img, "height") == 1, "png height")

   def enc = encode(img)
   assert(len(enc) > 8, "png encode length")
   assert(load8(enc, 1) == 80, "png encode signature P")

   print("✓ std.image.png tests passed (synthetic)")

   match file_read("etc/assets/images/test.png"){
      ok(data) -> {
         def img2 = decode(data)
         assert(img2 != 0, "png test.png decode")
         print("✓ std.image.png tests passed (test.png)")
      }
      err(_) -> { print("! test.png skipped (not found)") }
   }
}
