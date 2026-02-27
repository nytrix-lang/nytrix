;; Keywords: image tga
;; Reference:
;; - https://en.wikipedia.org/wiki/Truevision_TGA

module std.image.format.tga (
   decode, encode
)

use std.core *
use std.core.dict *

fn decode(data){
   "Decodes a TGA image from a byte string."
   if(!is_str(data) || len(data) < 18){ return 0 }
   def id_len = load8(data, 0)
   def image_type = load8(data, 2)
   if(image_type != 2 && image_type != 3 && image_type != 10 && image_type != 11){ return 0 }
   def w = load16(data, 12)
   def h = load16(data, 14)
   def bpp = load8(data, 16)
   def descriptor = load8(data, 17)
   if(w <= 0 || h <= 0){ return 0 }
   def gray = (image_type == 3 || image_type == 11)
   if(gray){
      if(bpp != 8 && bpp != 16){ return 0 }
   } else {
      if(bpp != 24 && bpp != 32){ return 0 }
   }
   def top_down = (descriptor & 32) != 0
   def left_to_right = (descriptor & 16) == 0
   def offset = 18 + id_len
   if(offset > len(data)){ return 0 }
   def pixels = init_str(malloc(w * h * 4 + 1), w * h * 4)
   if(image_type == 2 || image_type == 3){
      def bytes_per_px = bpp / 8
      if(offset + w * h * bytes_per_px > len(data)){ return 0 }
      mut y = 0
      while(y < h){
         def src_y = top_down ? y : (h - 1 - y)
         def dst_y = y
         mut x = 0
         while(x < w){
            def src_x = left_to_right ? x : (w - 1 - x)
            def src_off = offset + (src_y * w + src_x) * bytes_per_px
            def dst_off = (dst_y * w + x) * 4
            if(gray){
               def v = load8(data, src_off)
               def a = (bytes_per_px == 2) ? load8(data, src_off + 1) : 255
               store8(pixels, v, dst_off)
               store8(pixels, v, dst_off + 1)
               store8(pixels, v, dst_off + 2)
               store8(pixels, a, dst_off + 3)
            } elif(bpp == 24){
               store8(pixels, load8(data, src_off + 2), dst_off)
               store8(pixels, load8(data, src_off + 1), dst_off + 1)
               store8(pixels, load8(data, src_off), dst_off + 2)
               store8(pixels, 255, dst_off + 3)
            } else {
               store8(pixels, load8(data, src_off + 2), dst_off)
               store8(pixels, load8(data, src_off + 1), dst_off + 1)
               store8(pixels, load8(data, src_off), dst_off + 2)
               store8(pixels, load8(data, src_off + 3), dst_off + 3)
            }
            x += 1
         }
         y += 1
      }
   } else {
      mut p = offset
      mut count_px = 0
      def total_px = w * h
      def bytes_per_px = bpp / 8
      while(count_px < total_px){
         if(p >= len(data)){ return 0 }
         def head = load8(data, p)
         p += 1
         mut run_len = (head & 127) + 1
         if(head & 128){
            if(p + bytes_per_px > len(data)){ return 0 }
            def b = load8(data, p)
            mut g = 0 mut r = 0 mut a = 255 mut v = 0
            if(gray){
               v = b
               if(bytes_per_px == 2){ a = load8(data, p + 1) }
            } else {
               g = load8(data, p + 1)
               r = load8(data, p + 2)
               if(bytes_per_px == 4){ a = load8(data, p + 3) }
            }
            p += bytes_per_px
            while(run_len > 0){
               if(count_px >= total_px){ break }
               def px_idx = count_px
               def y = px_idx / w
               def x = px_idx % w
               def dst_y = top_down ? y : (h - 1 - y)
               def dst_x = left_to_right ? x : (w - 1 - x)
               def dst_off = (dst_y * w + dst_x) * 4
               if(gray){
                  store8(pixels, v, dst_off)
                  store8(pixels, v, dst_off + 1)
                  store8(pixels, v, dst_off + 2)
               } else {
                  store8(pixels, r, dst_off)
                  store8(pixels, g, dst_off + 1)
                  store8(pixels, b, dst_off + 2)
               }
               store8(pixels, a, dst_off + 3)
               count_px += 1
               run_len -= 1
            }
         } else {
            while(run_len > 0){
               if(count_px >= total_px){ break }
               if(p + bytes_per_px > len(data)){ return 0 }
               def b = load8(data, p)
               mut g = 0 mut r = 0 mut a = 255 mut v = 0
               if(gray){
                  v = b
                  if(bytes_per_px == 2){ a = load8(data, p + 1) }
               } else {
                  g = load8(data, p + 1)
                  r = load8(data, p + 2)
                  if(bytes_per_px == 4){ a = load8(data, p + 3) }
               }
               p += bytes_per_px
               def px_idx = count_px
               def y = px_idx / w
               def x = px_idx % w
               def dst_y = top_down ? y : (h - 1 - y)
               def dst_x = left_to_right ? x : (w - 1 - x)
               def dst_off = (dst_y * w + dst_x) * 4
               if(gray){
                  store8(pixels, v, dst_off)
                  store8(pixels, v, dst_off + 1)
                  store8(pixels, v, dst_off + 2)
               } else {
                  store8(pixels, r, dst_off)
                  store8(pixels, g, dst_off + 1)
                  store8(pixels, b, dst_off + 2)
               }
               store8(pixels, a, dst_off + 3)
               count_px += 1
               run_len -= 1
            }
         }
      }
   }
   mut res = dict(4)
   res = dict_set(res, "data", pixels)
   res = dict_set(res, "width", w)
   res = dict_set(res, "height", h)
   res = dict_set(res, "channels", 4)
   res
}

fn encode(img, bpp=24){
   "Encodes an image dictionary (width, height, data) into a TGA byte string."
   def w = dict_get(img, "width")
   def h = dict_get(img, "height")
   def pixels = dict_get(img, "data")
   def total_size = 18 + w * h * (bpp / 8)
   mut out = malloc(total_size)
   init_str(out, total_size)
   store8(out, 0, 0)
   store8(out, 0, 1)
   store8(out, 2, 2)
   store16(out, 0, 8) store16(out, 0, 10)
   store16(out, w, 12)
   store16(out, h, 14)
   store8(out, bpp, 16)
   store8(out, (bpp == 32) ? 8 : 0, 17)
   mut y = 0
   while(y < h){
      mut x = 0
      while(x < w){
         def src_off = (y * w + x) * 4
         def dst_off = 18 + (y * w + x) * (bpp / 8)
         def r = load8(pixels, src_off)
         def g = load8(pixels, src_off + 1)
         def b = load8(pixels, src_off + 2)
         store8(out, b, dst_off)
         store8(out, g, dst_off + 1)
         store8(out, r, dst_off + 2)
         if(bpp == 32){
            store8(out, load8(pixels, src_off + 3), dst_off + 3)
         }
         x += 1
      }
      y += 1
   }
   out
}

if(comptime{__main()}){
   use std.core.error *
   
   fn make_test_tga(){
      "Implements `make_test_tga`."
      def data = malloc(21)
      init_str(data, 21)
      store8(data, 0, 0) store8(data, 0, 1) 
      store8(data, 2, 2)
      store16(data, 1, 12) store16(data, 1, 14)
      store8(data, 24, 16) store8(data, 0, 17)
      store8(data, 255, 18) store8(data, 0, 19) store8(data, 0, 20)
      data
   }
   
   def tga_data = make_test_tga()
   def img = decode(tga_data)
   assert(img != 0, "tga decode")
   assert(dict_get(img, "width") == 1, "tga width")
   def p = dict_get(img, "data")
   assert(load8(p, 2) == 255, "tga blue channel")
   
   def enc_data = encode(img)
   assert(load8(enc_data, 2) == 2, "tga encode type")
   assert(load16(enc_data, 12) == 1, "tga encode width")
   
   print("✓ std.image.tga tests passed")
}
