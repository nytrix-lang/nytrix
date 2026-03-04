;; Keywords: ui gfx snapshot
;; Utilty for saving images from raw buffers.

module std.ui.gfx.snapshot (
   save
)

use std.core *
use std.image as img_mod

fn save(filename, buf, w, h, format="auto"){
   "Saves a raw RGBA buffer to `filename`. `format` can be 'auto', 'png', 'jpeg', 'bmp', 'tga', 'gif'."
   mut img = dict(4)
   dict_set(img, "width", w)
   dict_set(img, "height", h)
   dict_set(img, "data", buf)
   dict_set(img, "channels", 4)
   return img_mod.save(img, filename, format)
}
