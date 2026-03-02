;; Keywords: image resize bilinear
;; Reference:
;; - https://github.com/nothings/stb/blob/master/stb_image_resize2.h

module std.image.imgops (
   resize
)

use std.core *
use std.math *

fn resize(img, new_w, new_h){
   "Resizes an image dictionary using bilinear interpolation."
   if(!is_dict(img)){ return 0 }
   def w = dict_get(img, "width")
   def h = dict_get(img, "height")
   def pixels = dict_get(img, "data")
   def new_pixels = malloc(new_w * new_h * 4)
   def x_ratio = float(w - 1) / float(new_w)
   def y_ratio = float(h - 1) / float(new_h)
   mut y = 0
   while(y < new_h){
      mut x = 0
      while(x < new_w){
         def px = float(x) * x_ratio
         def py = float(y) * y_ratio
         def x_l = floor(px)
         def x_h = ceil(px)
         def y_l = floor(py)
         def y_h = ceil(py)
         def x_weight = px - float(x_l)
         def y_weight = py - float(y_l)
         mut c = 0
         while(c < 4){
            def a = float(load8(pixels, (y_l * w + x_l) * 4 + c))
            def b = float(load8(pixels, (y_l * w + x_h) * 4 + c))
            def d = float(load8(pixels, (y_h * w + x_l) * 4 + c))
            def e = float(load8(pixels, (y_h * w + x_h) * 4 + c))
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
   mut res = dict(4)
   dict_set(res, "data", new_pixels)
   dict_set(res, "width", new_w)
   dict_set(res, "height", new_h)
   dict_set(res, "channels", 4)
   res
}

if(comptime{__main()}){
   mut pix = malloc(16)
   store32(pix, 0xFF0000FF, 0) store32(pix, 0x00FF00FF, 4)
   store32(pix, 0x0000FFFF, 8) store32(pix, 0xFFFFFFFF, 12)

   mut img = dict()
   dict_set(img, "data", pix)
   dict_set(img, "width", 2)
   dict_set(img, "height", 2)

   def res = resize(img, 4, 4)
   assert(dict_get(res, "width") == 4, "resize width")
   assert(dict_get(res, "height") == 4, "resize height")

   print("✓ std.image.ops tests passed")
}
