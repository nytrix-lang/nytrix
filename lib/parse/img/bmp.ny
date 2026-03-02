;; Keywords: image bmp rfc7854
;; Reference:
;; - https://en.wikipedia.org/wiki/BMP_file_format
;; - https://www.rfc-editor.org/rfc/rfc7854.html

module std.image.format.bmp (
   decode, encode
)

use std.core *
use std.core.dict *

fn _ctz32(x){
   "Internal helper for `ctz32`."
   if(x == 0){ return 0 }
   mut v = x
   mut n = 0
   while((v & 1) == 0){
      v = v >> 1
      n += 1
   }
   n
}

fn _chan_from_mask(px, mask){
   "Internal helper for `chan_from_mask`."
   if(mask == 0){ return 0 }
   def sh = _ctz32(mask)
   def raw = (px & mask) >> sh
   def m = mask >> sh
   if(m <= 0){ return 0 }
   ((raw * 255) + (m / 2)) / m
}

fn decode(data){
   "Decodes a BMP image from a byte string."
   if(!is_str(data) || len(data) < 54){ return 0 }
   if(load8(data, 0) != 66 || load8(data, 1) != 77){ return 0 }
   def offset = load32(data, 10)
   def hdr_size = load32(data, 14)
   if(hdr_size < 40){ return 0 }
   def w = load32(data, 18)
   def h = load32(data, 22)
   def planes = load16(data, 26)
   def bpp = load16(data, 28)
   def compression = load32(data, 30)
   def colors_used = load32(data, 46)
   if(planes != 1){ return 0 }
   if(w <= 0){ return 0 }
   if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32){ return 0 }
   if(compression != 0 && compression != 3){ return 0 }
   if(compression == 3 && bpp != 16 && bpp != 32){ return 0 }
   mut abs_h = h
   mut top_down = false
   if(h < 0){
      abs_h = -h
      top_down = true
   }
   if(abs_h <= 0){ return 0 }
   if(offset < 14 + hdr_size || offset > len(data)){ return 0 }
   def bpl = ((w * bpp + 31) / 32) * 4
   if(bpl <= 0){ return 0 }
   if(offset + (abs_h - 1) * bpl + bpl > len(data)){ return 0 }
   mut r_mask = 0
   mut g_mask = 0
   mut b_mask = 0
   mut a_mask = 0
   if(compression == 3){
      def mask_off = 14 + 40
      if(hdr_size >= 52 || offset >= mask_off + 12){
         r_mask = load32(data, mask_off)
         g_mask = load32(data, mask_off + 4)
         b_mask = load32(data, mask_off + 8)
      } else {
         return 0
      }
      if(hdr_size >= 56 || offset >= mask_off + 16){
         a_mask = load32(data, mask_off + 12)
      }
      if(r_mask == 0 || g_mask == 0 || b_mask == 0){ return 0 }
   }
   mut palette_entries = 0
   def palette_off = 14 + hdr_size
   if(bpp <= 8){
      if(palette_off >= offset){ return 0 }
      def max_entries = (offset - palette_off) / 4
      if(max_entries <= 0){ return 0 }
      palette_entries = colors_used
      if(palette_entries <= 0 || palette_entries > max_entries){
         palette_entries = max_entries
      }
      if(palette_entries <= 0){ return 0 }
   }
   def pixels = init_str(malloc(w * abs_h * 4 + 16) + 16, w * abs_h * 4)
   memset(pixels, 0, w * abs_h * 4)
   mut y = 0
   while(y < abs_h){
      def src_y = top_down ? y : (abs_h - 1 - y)
      def src_row_offset = offset + (src_y * bpl)
      def dst_row_offset = y * w * 4
      if(src_row_offset + bpl > len(data)){ return 0 }
      mut x = 0
      while(x < w){
         def dst = dst_row_offset + x * 4
         if(bpp <= 8){
            mut idx = 0
            if(bpp == 8){
               idx = load8(data, src_row_offset + x)
            } elif(bpp == 4){
               def v = load8(data, src_row_offset + (x / 2))
               idx = ((x & 1) == 0) ? ((v >> 4) & 15) : (v & 15)
            } else {
               def v = load8(data, src_row_offset + (x / 8))
               idx = (v >> (7 - (x & 7))) & 1
            }
            if(idx < 0 || idx >= palette_entries){ idx = 0 }
            def p = palette_off + idx * 4
            store8(pixels, load8(data, p + 2), dst)
            store8(pixels, load8(data, p + 1), dst + 1)
            store8(pixels, load8(data, p), dst + 2)
            store8(pixels, 255, dst + 3)
         } elif(bpp == 24){
            def s = src_row_offset + x * 3
            store8(pixels, load8(data, s + 2), dst)
            store8(pixels, load8(data, s + 1), dst + 1)
            store8(pixels, load8(data, s), dst + 2)
            store8(pixels, 255, dst + 3)
         } elif(bpp == 32 && compression == 0){
            def s = src_row_offset + x * 4
            store8(pixels, load8(data, s + 2), dst)
            store8(pixels, load8(data, s + 1), dst + 1)
            store8(pixels, load8(data, s), dst + 2)
            store8(pixels, load8(data, s + 3), dst + 3)
         } else {
            mut px = 0
            mut rr = 0 mut gg = 0 mut bb = 0 mut aa = 255
            if(bpp == 16){
               px = load16(data, src_row_offset + x * 2)
               if(compression == 0){
                  rr = _chan_from_mask(px, 0x7C00)
                  gg = _chan_from_mask(px, 0x03E0)
                  bb = _chan_from_mask(px, 0x001F)
               } else {
                  rr = _chan_from_mask(px, r_mask)
                  gg = _chan_from_mask(px, g_mask)
                  bb = _chan_from_mask(px, b_mask)
                  if(a_mask != 0){ aa = _chan_from_mask(px, a_mask) }
               }
            } else {
               px = load32(data, src_row_offset + x * 4)
               rr = _chan_from_mask(px, r_mask)
               gg = _chan_from_mask(px, g_mask)
               bb = _chan_from_mask(px, b_mask)
               if(a_mask != 0){ aa = _chan_from_mask(px, a_mask) }
            }
            store8(pixels, rr, dst)
            store8(pixels, gg, dst + 1)
            store8(pixels, bb, dst + 2)
            store8(pixels, aa, dst + 3)
         }
         x += 1
      }
      y += 1
   }
   mut res = dict(4)
   res = dict_set(res, "data", pixels)
   res = dict_set(res, "width", w)
   res = dict_set(res, "height", abs_h)
   res = dict_set(res, "channels", 4)
   res
}

fn encode(img){
   "Encodes an image dictionary (width, height, data) into a BMP byte string."
   def w = dict_get(img, "width")
   def h = dict_get(img, "height")
   def pixels = dict_get(img, "data")
   def bpp = 24
   def bpl = ((w * bpp + 31) / 32) * 4
   def data_size = bpl * h
   def total_size = 54 + data_size
   mut out = malloc(total_size)
   init_str(out, total_size)
   store8(out, 66, 0) store8(out, 77, 1)
   store32(out, total_size, 2)
   store32(out, 0, 6)
   store32(out, 54, 10)
   store32(out, 40, 14)
   store32(out, w, 18)
   store32(out, h, 22)
   store16(out, 1, 26)
   store16(out, bpp, 28)
   store32(out, 0, 30)
   store32(out, data_size, 34)
   store32(out, 0, 38) store32(out, 0, 42)
   store32(out, 0, 46) store32(out, 0, 50)
   mut y = 0
   while(y < h){
      def src_row_offset = (h - 1 - y) * w * 4
      def dst_row_offset = 54 + (y * bpl)
      mut x = 0
      while(x < w){
         def r = load8(pixels, src_row_offset + x * 4)
         def g = load8(pixels, src_row_offset + x * 4 + 1)
         def b = load8(pixels, src_row_offset + x * 4 + 2)
         store8(out, b, dst_row_offset + x * 3)
         store8(out, g, dst_row_offset + x * 3 + 1)
         store8(out, r, dst_row_offset + x * 3 + 2)
         x += 1
      }
      y += 1
   }
   out
}

if(comptime{__main()}){
   use std.core.error *

   fn make_test_bmp(){
      "Implements `make_test_bmp`."
      def data = malloc(58)
      init_str(data, 58)
      store8(data, 66, 0) store8(data, 77, 1)
      store32(data, 58, 2) store32(data, 54, 10)
      store32(data, 40, 14)
      store32(data, 1, 18) store32(data, 1, 22)
      store16(data, 1, 26) store16(data, 24, 28)
      store8(data, 255, 54) store8(data, 0, 55) store8(data, 0, 56) store8(data, 0, 57)
      data
   }

   def bmp_data = make_test_bmp()
   def img = decode(bmp_data)
   assert(img != 0, "bmp decode")
   assert(dict_get(img, "width") == 1, "bmp width")
   assert(dict_get(img, "height") == 1, "bmp height")
   def p = dict_get(img, "data")
   assert(load8(p, 2) == 255, "bmp red channel")

   def enc_data = encode(img)
   assert(load8(enc_data, 0) == 66, "bmp encode signature")
   assert(load8(enc_data, 54) == 255, "bmp encode pixel")

   print("✓ std.image.bmp tests passed (synthetic)")

   match file_read("etc/assets/images/test.bmp"){
      ok(data) -> {
         def img2 = decode(data)
         assert(img2 != 0, "bmp test.bmp decode")
         print("✓ std.image.bmp tests passed (test.bmp)")
      }
      err(_) -> { print("! test.bmp skipped (not found)") }
   }
}
