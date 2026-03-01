;; Keywords: image ico icon dib bmp
;; References:
;; - https://en.wikipedia.org/wiki/ICO_(file_format)
;; - https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)

module std.image.format.ico (
   decode, encode
)

use std.core *
use std.core.dict *
use std.image.format.png as png

fn _u16le(s, i){
   "Internal helper for `u16le`."
   load8(s, i) | (load8(s, i + 1) << 8)
}

fn _u32le(s, i){
   "Internal helper for `u32le`."
   load8(s, i) | (load8(s, i + 1) << 8) | (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)
}

fn _mk_zero_list(n){
   "Internal helper for `mk_zero_list`."
   def xs = list(n)
   mut i = 0
   while(i < n){
      append(xs, 0)
      i += 1
   }
   xs
}

fn _list_to_bytes(xs){
   "Internal helper for `list_to_bytes`."
   def n = len(xs)
   def out = init_str(malloc(n + 1), n)
   mut i = 0
   while(i < n){
      store8(out, get(xs, i), i)
      i += 1
   }
   out
}

fn _chan_from_mask(px, mask){
   "Internal helper for `chan_from_mask`."
   if(mask == 0){ return 0 }
   mut m = mask
   mut sh = 0
   while((m & 1) == 0){
      m = m >> 1
      sh += 1
   }
   def raw = (px & mask) >> sh
   if(m <= 0){ return 0 }
   ((raw * 255) + (m / 2)) / m
}

fn _decode_dib(dib){
   "Internal helper for `decode_dib`."
   if(!is_str(dib) || len(dib) < 40){ return 0 }
   def hdr = _u32le(dib, 0)
   if(hdr < 40 || hdr > len(dib)){ return 0 }
   def w = _u32le(dib, 4)
   def h_all = _u32le(dib, 8)
   if(w <= 0 || h_all <= 1){ return 0 }
   def h = h_all / 2
   def planes = _u16le(dib, 12)
   def bpp = _u16le(dib, 14)
   def ico_compression = _u32le(dib, 16)
   def clr_used = _u32le(dib, 32)
   if(planes != 1){ return 0 }
   if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32){ return 0 }
   if(ico_compression != 0){ return 0 }
   mut pal_n = 0
   if(bpp <= 8){
      pal_n = clr_used
      if(pal_n <= 0){ pal_n = 1 << bpp }
   }
   def pal_off = hdr
   def xor_off = pal_off + pal_n * 4
   def xor_stride = ((w * bpp + 31) / 32) * 4
   def and_off = xor_off + xor_stride * h
   def and_stride = ((w + 31) / 32) * 4
   if(xor_off + xor_stride * h > len(dib)){ return 0 }
   if(and_off + and_stride * h > len(dib)){ return 0 }
   if(bpp <= 8 && pal_off + pal_n * 4 > len(dib)){ return 0 }
   def px = init_str(malloc(w * h * 4 + 1), w * h * 4)
   memset(px, 0, w * h * 4)
   mut y = 0
   while(y < h){
      def src_y = h - 1 - y
      def row = xor_off + src_y * xor_stride
      mut x = 0
      while(x < w){
         def dst = (y * w + x) * 4
         if(bpp == 32){
            def s = row + x * 4
            store8(px, load8(dib, s + 2), dst)
            store8(px, load8(dib, s + 1), dst + 1)
            store8(px, load8(dib, s + 0), dst + 2)
            store8(px, load8(dib, s + 3), dst + 3)
         } elif(bpp == 24){
            def s = row + x * 3
            store8(px, load8(dib, s + 2), dst)
            store8(px, load8(dib, s + 1), dst + 1)
            store8(px, load8(dib, s + 0), dst + 2)
            store8(px, 255, dst + 3)
         } elif(bpp == 8){
            mut idx = load8(dib, row + x)
            if(idx >= pal_n){ idx = 0 }
            def p = pal_off + idx * 4
            store8(px, load8(dib, p + 2), dst)
            store8(px, load8(dib, p + 1), dst + 1)
            store8(px, load8(dib, p + 0), dst + 2)
            store8(px, 255, dst + 3)
         } elif(bpp == 4){
            def b = load8(dib, row + (x / 2))
            mut idx = ((x & 1) == 0) ? ((b >> 4) & 15) : (b & 15)
            if(idx >= pal_n){ idx = 0 }
            def p = pal_off + idx * 4
            store8(px, load8(dib, p + 2), dst)
            store8(px, load8(dib, p + 1), dst + 1)
            store8(px, load8(dib, p + 0), dst + 2)
            store8(px, 255, dst + 3)
         } else {
            def b = load8(dib, row + (x / 8))
            mut idx = (b >> (7 - (x & 7))) & 1
            if(idx >= pal_n){ idx = 0 }
            def p = pal_off + idx * 4
            store8(px, load8(dib, p + 2), dst)
            store8(px, load8(dib, p + 1), dst + 1)
            store8(px, load8(dib, p + 0), dst + 2)
            store8(px, 255, dst + 3)
         }
         x += 1
      }
      y += 1
   }
   y = 0
   while(y < h){
      def src_y = h - 1 - y
      def row = and_off + src_y * and_stride
      mut x = 0
      while(x < w){
         def b = load8(dib, row + (x / 8))
         def m = (b >> (7 - (x & 7))) & 1
         if(m){
            store8(px, 0, (y * w + x) * 4 + 3)
         }
         x += 1
      }
      y += 1
   }
   mut out = dict(4)
   out = dict_set(out, "data", px)
   out = dict_set(out, "width", w)
   out = dict_set(out, "height", h)
   out = dict_set(out, "channels", 4)
   out
}

fn decode(data){
   "Decodes ICO bytes to RGBA image (best entry selected)."
   if(!is_str(data) || len(data) < 22){ return 0 }
   if(_u16le(data, 0) != 0){ return 0 }
   if(_u16le(data, 2) != 1){ return 0 }
   def count = _u16le(data, 4)
   if(count <= 0){ return 0 }
   mut best_i = -1
   mut best_score = -1
   mut i = 0
   while(i < count){
      def e = 6 + i * 16
      if(e + 16 > len(data)){ break }
      mut ew = load8(data, e)
      mut eh = load8(data, e + 1)
      if(ew == 0){ ew = 256 }
      if(eh == 0){ eh = 256 }
      def bpp = _u16le(data, e + 6)
      def sz = _u32le(data, e + 8)
      def off = _u32le(data, e + 12)
      if(sz > 0 && off + sz <= len(data)){
         def score = ew * eh * 512 + bpp
         if(score > best_score){
            best_score = score
            best_i = i
         }
      }
      i += 1
   }
   if(best_i < 0){ return 0 }
   def e = 6 + best_i * 16
   def sz = _u32le(data, e + 8)
   def off = _u32le(data, e + 12)
   def blob = init_str(malloc(sz + 1), sz)
   __copy_mem(blob, data + off, sz)
   if(sz >= 8 && load8(blob, 0) == 137 && load8(blob, 1) == 80 && load8(blob, 2) == 78 && load8(blob, 3) == 71){
      return png.decode(blob)
   }
   _decode_dib(blob)
}

fn _out_u8(out, v){
   "Internal helper for `out_u8`."
   append(out, v & 255)
}
fn _out_u16le(out, v){
   "Internal helper for `out_u16le`."
   _out_u8(out, v) _out_u8(out, v >> 8)
}
fn _out_u32le(out, v){
   "Internal helper for `out_u32le`."
   _out_u8(out, v)
   _out_u8(out, v >> 8)
   _out_u8(out, v >> 16)
   _out_u8(out, v >> 24)
}
fn _out_bytes(out, s){
   "Internal helper for `out_bytes`."
   mut i = 0
   while(i < len(s)){
      _out_u8(out, load8(s, i))
      i += 1
   }
}

fn encode(img){
   "Encodes image dict to ICO (single PNG-backed icon)."
   if(!is_dict(img)){ return 0 }
   def w = dict_get(img, "width", 0)
   def h = dict_get(img, "height", 0)
   if(w <= 0 || h <= 0 || w > 256 || h > 256){ return 0 }
   def png_blob = png.encode(img)
   if(!is_str(png_blob) || len(png_blob) == 0){ return 0 }
   def out = list(64)
   _out_u16le(out, 0)
   _out_u16le(out, 1)
   _out_u16le(out, 1)
   _out_u8(out, (w == 256) ? 0 : w)
   _out_u8(out, (h == 256) ? 0 : h)
   _out_u8(out, 0)
   _out_u8(out, 0)
   _out_u16le(out, 1)
   _out_u16le(out, 32)
   _out_u32le(out, len(png_blob))
   _out_u32le(out, 6 + 16)
   _out_bytes(out, png_blob)
   _list_to_bytes(out)
}

if(comptime{__main()}){
   use std.core.error *
   def w = 16 def h = 16
   def px = init_str(malloc(w * h * 4), w * h * 4)
   mut i = 0
   while(i < w * h){
      def o = i * 4
      store8(px, (i * 13) & 255, o)
      store8(px, (i * 7) & 255, o + 1)
      store8(px, (i * 3) & 255, o + 2)
      store8(px, 255, o + 3)
      i += 1
   }
   mut img = dict(4)
   img = dict_set(img, "width", w)
   img = dict_set(img, "height", h)
   img = dict_set(img, "channels", 4)
   img = dict_set(img, "data", px)
   def enc = encode(img)
   assert(enc && len(enc) > 32, "ico encode")
   def dec = decode(enc)
   assert(dec != 0, "ico decode")
   assert(dict_get(dec, "width") == w, "ico width")
   assert(dict_get(dec, "height") == h, "ico height")
   print("✓ std.image.ico tests passed")
}
