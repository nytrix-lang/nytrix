;; Keywords: image webp
;; WebP Image Loader for Nytrix using libwebp
module std.parse.img.webp(decode)
#include <webp/decode.h>
extern "webp" {
   fn WebPGetInfo(ptr: data, u64: data_size, ptr: width_p, ptr: height_p): i32
   fn WebPDecodeRGBAInto(ptr: data, u64: data_size, ptr: output_buffer, u64: output_buffer_size, i32: output_stride): ptr
}

use std.core
use std.core.dict_mod

fn _raw_ptr(any: p): any {
   if(!p){ return 0 }
   if(is_int(p)){ return to_int(p) }
   p
}

fn decode(str: data): any {
   "Decodes WebP bytes into an image dict with RGBA pixel data, or 0 on failure."
   if(!is_str(data) || data.len < 12){ return 0 }
   def w_p, h_p = zalloc(4), zalloc(4)
   if(!w_p || !h_p){
      if(w_p){ free(w_p) }
      if(h_p){ free(h_p) }
      return 0
   }
   def info_ok = WebPGetInfo(_raw_ptr(data), data.len, w_p, h_p)
   if(int(info_ok) == 0){
      free(w_p, h_p)
      return 0
   }
   def w, h = load32(w_p, 0), load32(h_p, 0)
   free(w_p, h_p)
   if(w <= 0 || h <= 0 || w > 32768 || h > 32768){ return 0 }
   def pix_len = w * h * 4
   def pix_raw = malloc(pix_len + 32)
   if(!pix_raw){ return 0 }
   def dec_ptr = WebPDecodeRGBAInto(_raw_ptr(data), data.len, pix_raw, pix_len, w * 4)
   if(!dec_ptr){
      free(pix_raw)
      return 0
   }
   def pix = init_str(pix_raw, pix_len)
   mut out = dict(4)
   out["data"] = pix
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out
}
