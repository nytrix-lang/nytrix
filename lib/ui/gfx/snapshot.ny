;; Keywords: ui gfx snapshot tga
;; Utilty for saving TGA images from raw buffers.

module std.ui.gfx.snapshot (
   save_tga
)

use std.core *
use std.image as img_mod

fn save_tga(filename, buf, w, h){
   "Saves a raw RGBA buffer as a TGA file using the standard image library."
   mut img = dict(4)
   dict_set(img, "width", w)
   dict_set(img, "height", h)
   dict_set(img, "data", buf)
   dict_set(img, "channels", 4)
   return img_mod.save(img, filename, "tga")
}
