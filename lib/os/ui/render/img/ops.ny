;; Keywords: render image ops
;; Reference:
;; - https://github.com/nothings/stb/blob/master/stb_image_resize2.h
module std.os.ui.render.img.ops(resize, rgba_mip_level_count, rgba_mip_total_bytes, generate_rgba_mips)
use std.core
use std.math

fn rgba_mip_level_count(int: w, int: h): int {
   mut levels = 1
   mut cw = max(1, int(w))
   mut ch = max(1, int(h))
   while(cw > 1 || ch > 1){
      cw, ch = max(1, cw >> 1), max(1, ch >> 1)
      levels += 1
   }
   levels
}

fn rgba_mip_total_bytes(int: w, int: h): int {
   mut total = 0
   mut cw = max(1, int(w))
   mut ch = max(1, int(h))
   while(cw > 0 && ch > 0){
      total += cw * ch * 4
      if(cw == 1 && ch == 1){ break }
      cw, ch = max(1, cw >> 1), max(1, ch >> 1)
   }
   total
}

fn generate_rgba_mips(ptr: src_pixels, int: w, int: h, bool: copy_single=false): any {
   def iw, ih = int(w), int(h)
   if(!src_pixels || iw <= 0 || ih <= 0){ return 0 }
   def levels = rgba_mip_level_count(iw, ih)
   if(levels <= 1){
      if(!copy_single){ return src_pixels }
      def single_bytes = iw * ih * 4
      def copy = malloc(single_bytes)
      if(!copy){ return 0 }
      memcpy(copy, src_pixels, single_bytes)
      return copy
   }
   def total = rgba_mip_total_bytes(iw, ih)
   mut dst = malloc(total)
   if(!dst){ return 0 }
   memcpy(dst, src_pixels, iw * ih * 4)
   mut src_off = 0
   mut dst_off = iw * ih * 4
   mut prev_w = iw
   mut prev_h = ih
   mut i = 1
   while(i < levels){
      mut next_w, next_h = prev_w >> 1, prev_h >> 1
      if(next_w < 1){ next_w = 1 }
      if(next_h < 1){ next_h = 1 }
      mut y = 0
      while(y < next_h){
         mut x = 0
         while(x < next_w){
            mut sx0, sy0 = x << 1, y << 1
            if(sx0 >= prev_w){ sx0 = prev_w - 1 }
            if(sy0 >= prev_h){ sy0 = prev_h - 1 }
            mut sx1, sy1 = sx0 + 1, sy0 + 1
            if(sx1 >= prev_w){ sx1 = prev_w - 1 }
            if(sy1 >= prev_h){ sy1 = prev_h - 1 }
            def p00, p10 = src_off + (sy0 * prev_w + sx0) * 4, src_off + (sy0 * prev_w + sx1) * 4
            def p01, p11 = src_off + (sy1 * prev_w + sx0) * 4, src_off + (sy1 * prev_w + sx1) * 4
            def dp = dst_off + (y * next_w + x) * 4
            mut c = 0
            while(c < 4){
               def sum = int(load8(dst, p00 + c)) + int(load8(dst, p10 + c)) + int(load8(dst, p01 + c)) + int(load8(dst, p11 + c))
               store8(dst, (sum + 2) / 4, dp + c)
               c += 1
            }
            x += 1
         }
         y += 1
      }
      src_off = dst_off
      dst_off += next_w * next_h * 4
      prev_w, prev_h = next_w, next_h
      i += 1
   }
   dst
}

fn resize(dict: img, int: new_w, int: new_h): any {
   "Resizes an image dictionary using bilinear interpolation."
   if(!is_dict(img)){ return 0 }
   def w, h = img.get("width"), img.get("height")
   def pixels = img.get("data")
   def new_pixels = malloc(new_w * new_h * 4)
   if(!new_pixels){ return 0 }
   def x_ratio, y_ratio = float(w - 1) / float(new_w), float(h - 1) / float(new_h)
   mut y = 0
   while(y < new_h){
      mut x = 0
      while(x < new_w){
         def px, py = float(x) * x_ratio, float(y) * y_ratio
         def x_l, x_h = int(floor(px)), int(ceil(px))
         def y_l, y_h = int(floor(py)), int(ceil(py))
         def x_weight, y_weight = px - float(x_l), py - float(y_l)
         mut c = 0
         while(c < 4){
            def a, b = float(load8(pixels, (y_l * w + x_l) * 4 + c)), float(load8(pixels, (y_l * w + x_h) * 4 + c))
            def d, e = float(load8(pixels, (y_h * w + x_l) * 4 + c)), float(load8(pixels, (y_h * w + x_h) * 4 + c))
            def val = a * (1 - x_weight) * (1 - y_weight) +
            b * x_weight * (1 - y_weight) +
            d * y_weight * (1 - x_weight) +
            e * x_weight * y_weight
            store8(new_pixels, int(val), (y * new_w + x) * 4 + c)
            c += 1
         }
         x += 1
      }
      y += 1
   }
   return {"data": new_pixels, "width": new_w, "height": new_h, "channels": 4}
}
