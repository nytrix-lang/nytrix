;; Keywords: image tga
;; Truevision TGA Image Loader and Encoder for Nytrix
;; Reference:
;; - https://en.wikipedia.org/wiki/Truevision_TGA

module std.image.format.tga (
   decode, encode
)

use std.core *
use std.core.dict_mod *
use std.parse.bin as pbin

fn decode(data){
   "Decodes an uncompressed 24-bit or 32-bit TGA image."
   if(!is_str(data) || len(data) < 18)
   {
      return 0
   }
   def id_len = load8(data, 0)
   def color_map_type = load8(data, 1)
   load8(data, 2) ; image_type
   def w = pbin.u16le(data, 12)
   def h = pbin.u16le(data, 14)
   def bpp = load8(data, 16)
   def desc = load8(data, 17)
   def flip_x = (desc >> 4) & 1
   def flip_y = (desc >> 5) & 1
   mut p = 18 + id_len
   if(color_map_type == 1)
   {
      def map_len = pbin.u16le(data, 5)
      def map_entry_size = load8(data, 7)
      p += map_len * (map_entry_size / 8)
   }
   def tpx = w * h
   def pix = init_str(malloc(tpx * 4 + 1 + 16) + 16, tpx * 4)
   mut y = 0
   while(y < h)
   {
      mut x = 0
      while(x < w)
      {
         def ry = flip_y ? y : (h - 1 - y)
         def rx = flip_x ? (w - 1 - x) : x
         def src_off = p + (y * w + x) * (bpp / 8)
         def dst_off = (ry * w + rx) * 4
         if(bpp == 24)
         {
         store8(pix, load8(data, src_off + 2), dst_off)
         store8(pix, load8(data, src_off + 1), dst_off + 1)
         store8(pix, load8(data, src_off), dst_off + 2)
         store8(pix, 255, dst_off + 3)
         }
         elif(bpp == 32)
         {
         store8(pix, load8(data, src_off + 2), dst_off)
         store8(pix, load8(data, src_off + 1), dst_off + 1)
         store8(pix, load8(data, src_off), dst_off + 2)
         store8(pix, load8(data, src_off + 3), dst_off + 3)
         }
         x += 1
      }
      y += 1
   }
   mut res_d = dict(4)
   dict_set(res_d, "data", pix)
   dict_set(res_d, "width", w)
   dict_set(res_d, "height", h)
   dict_set(res_d, "channels", 4)
   res_d
}

fn encode(img){
   "Encodes an image dictionary as a 32-bit BGRA TGA byte string."
   def w = dict_get(img, "width")
   def h = dict_get(img, "height")
   def d = dict_get(img, "data")
   def ch = dict_get(img, "channels", 4)
   def hdr = init_str(malloc(19 + 16) + 16, 18)
   memset(hdr, 0, 18)
   store8(hdr, 2, 2)
   store8(hdr, w & 255, 12)
   store8(hdr, w >> 8, 13)
   store8(hdr, h & 255, 14)
   store8(hdr, h >> 8, 15)
   store8(hdr, 32, 16) ; 32 bpp
   store8(hdr, 40, 17) ; Origin top-left (bit 5 = 32) + 8 bits alpha
   def out = init_str(malloc(w * h * 4 + 19 + 16) + 16, w * h * 4 + 18)
   __copy_mem(out, hdr, 18)
   mut y = 0
   while(y < h)
   {
      mut x = 0
      while(x < w)
      {
         def src_off = (y * w + x) * ch
         def dst_off = 18 + (y * w + x) * 4
         mut r = load8(d, src_off)
         mut g = r
         mut b = r
         mut a = 255
         if(ch >= 3)
         {
         g = load8(d, src_off + 1)
         b = load8(d, src_off + 2)
         }
         if(ch == 4)
         {
         a = load8(d, src_off + 3)
         }
         store8(out, b, dst_off)
         store8(out, g, dst_off + 1)
         store8(out, r, dst_off + 2)
         store8(out, a, dst_off + 3)
         x += 1
      }
      y += 1
   }
   out
}
