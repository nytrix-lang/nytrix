;; Keywords: render snapshot
;; Image snapshot writer for raw buffers.
module std.os.ui.render.snapshot(save)
use std.core
use std.parse.img as img_mod

fn save(str: filename, any: buf, int: w, int: h, str: format="auto"): any {
   "Saves a raw RGBA buffer to `filename`. `format` can be 'auto', 'png', 'jpeg', 'bmp', 'tga', 'gif'."
   return img_mod.save({"width": w, "height": h, "data": buf, "channels": 4}, filename, format)
}
