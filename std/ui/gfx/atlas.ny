;; Keywords: ui gfx atlas texture packing
;; Texture Atlas manager for std.ui.gfx.
;; Efficiently packs many small images (glyphs, icons) into larger textures.

module std.ui.gfx.atlas (
   atlas_create, atlas_destroy, atlas_add, atlas_get, atlas_bind,
   atlas_texture_id, atlas_uv_rect
)

use std.core *
use std.ui.gfx.vk_renderer as vkr
use std.math *

;; Atlas object: { texture_id, width, height, current_x, current_y, max_row_h, items }

fn atlas_create(w=2048, h=2048){
   "Creates a new texture atlas of the specified dimensions."
   def pixels = malloc(w * h * 4)
   memset(pixels, 0, w * h * 4)
   
   def tex_id = vkr.create_texture(w, h, pixels)
   free(pixels)
   
   mut a = dict(8)
   a = dict_set(a, "tex_id", tex_id)
   a = dict_set(a, "width", w)
   a = dict_set(a, "height", h)
   a = dict_set(a, "cx", 2) ;; start with small padding
   a = dict_set(a, "cy", 2)
   a = dict_set(a, "max_row_h", 0)
   a = dict_set(a, "items", dict(256))
   a
}

fn atlas_destroy(a){
   "Destroys the atlas and its underlying texture."
   ;; texture_destroy not implemented in vkr yet? 
   ;; vkr.shutdown handles all textures for now.
}

fn atlas_add(a, key, w, h, pixels){
   "Packs a new image into the atlas. Returns [u1, v1, u2, v2] or 0."
   if(!a || w <= 0 || h <= 0){ return 0 }
   
   mut items = dict_get(a, "items")
   if(dict_has(items, key)){ return dict_get(items, key) }
   
   def aw = dict_get(a, "width")
   def ah = dict_get(a, "height")
   mut cx = dict_get(a, "cx")
   mut cy = dict_get(a, "cy")
   mut mrh = dict_get(a, "max_row_h")
   
   ;; Simple shelf packing with 2px padding
   if(cx + w + 2 > aw){
      cx = 2
      cy = cy + mrh + 2
      mrh = 0
   }
   
   if(cy + h + 2 > ah){
      ;; Atlas full! 
      return 0
   }
   
   ;; Update texture data on GPU
   vkr.update_texture_rect(dict_get(a, "tex_id"), cx, cy, w, h, pixels)
   
   if(h > mrh){ mrh = h }
   
   def fw = float(aw)
   def fh = float(ah)
   def u1 = float(cx) / fw
   def v1 = float(cy) / fh
   def u2 = float(cx + w) / fw
   def v2 = float(cy + h) / fh
   def uv = [u1, v1, u2, v2]
   
   dict_set(items, key, uv)
   dict_set(a, "cx", cx + w + 2)
   dict_set(a, "cy", cy)
   dict_set(a, "max_row_h", mrh)
   
   uv
}

fn atlas_texture_id(a){ dict_get(a, "tex_id", -1) }
