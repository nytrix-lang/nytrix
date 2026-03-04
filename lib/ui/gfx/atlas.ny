;; Keywords: ui gfx atlas texture packing
;; Texture Atlas manager for std.ui.gfx.
;; Efficiently packs many small images (glyphs, icons) into larger textures.

module std.ui.gfx.atlas (
   atlas_create, atlas_destroy, atlas_add, atlas_get, atlas_bind,
   atlas_texture_id, atlas_uv_rect, atlas_flush
)

use std.core *
use std.ui.gfx.vk as vkr
use std.math *

mut _atlas_scratch = 0
mut _atlas_scratch_cap = 0

;; Atlas object fields (dictionary):
;;   tex_id, width, height, items, cpu_buf, state_ptr
;; state_ptr structure (i32):
;;   0: cx, 4: cy, 8: max_row_h, 12: dirty, 16: dx1, 20: dy1, 24: dx2, 28: dy2

fn atlas_create(w=2048, h=2048){
   "Creates a new texture atlas. Uses a native state buffer for mutable metadata."
   def buf_size = w * h * 4
   def cpu_buf = malloc(buf_size)
   memset(cpu_buf, 0, buf_size)

   def state_ptr = malloc(64)
   memset(state_ptr, 0, 64)
   store32(state_ptr, 2, 0)  ; cx
   store32(state_ptr, 2, 4)  ; cy
   store32(state_ptr, 0, 8)  ; mrh
   store32(state_ptr, 0, 12) ; dirty
   store32(state_ptr, w, 16) ; dx1
   store32(state_ptr, h, 20) ; dy1
   store32(state_ptr, 0, 24) ; dx2
   store32(state_ptr, 0, 28) ; dy2

   def tex_id = vkr.create_texture_ex(w, h, cpu_buf, 37)

   mut a = dict(8)
   a = dict_set(a, "tex_id", tex_id)
   a = dict_set(a, "width", w)
   a = dict_set(a, "height", h)
   a = dict_set(a, "items", dict(512))
   a = dict_set(a, "cpu_buf", cpu_buf)
   a = dict_set(a, "state_ptr", state_ptr)
   a
}

fn atlas_destroy(a){
   "Frees atlas resources including GPU texture and state buffers."
   if(!a){ return }
   def tex_id = dict_get(a, "tex_id", -1)
   if(tex_id >= 0){ vkr.destroy_texture(tex_id) }
   def cpu_buf = dict_get(a, "cpu_buf", 0)
   if(cpu_buf){ free(cpu_buf) }
   def state_ptr = dict_get(a, "state_ptr", 0)
   if(state_ptr){ free(state_ptr) }
}

fn atlas_add(a, key, w, h, pixels){
   "Packs an image and returns [u1,v1,u2,v2]. Correctly updates packing state."
   if(!a || w <= 0 || h <= 0){ return 0 }

   mut items = dict_get(a, "items")
   if(dict_has(items, key)){ return dict_get(items, key) }

   def aw = dict_get(a, "width")
   def ah = dict_get(a, "height")
   def state_ptr = dict_get(a, "state_ptr", 0)
   if(!state_ptr){ return 0 }

   mut cx = load32(state_ptr, 0)
   mut cy = load32(state_ptr, 4)
   mut mrh = load32(state_ptr, 8)

   ;; Shelf packing
   if(cx + w + 2 > aw){
      cx = 2
      cy = cy + mrh + 2
      mrh = 0
   }
   if(cy + h + 2 > ah){ return 0 }

   def cpu_buf = dict_get(a, "cpu_buf", 0)
   if(cpu_buf && pixels){
      mut row = 0
      while(row < h){
         def src_off = row * w * 4
         def dst_off = ((cy + row) * aw + cx) * 4
         memcpy(cpu_buf + dst_off, pixels + src_off, w * 4)
         row += 1
      }
      ;; Update dirty box
      mut dx1 = load32(state_ptr, 16) mut dy1 = load32(state_ptr, 20)
      mut dx2 = load32(state_ptr, 24) mut dy2 = load32(state_ptr, 28)
      if(cx < dx1){ dx1 = cx }
      if(cy < dy1){ dy1 = cy }
      if(cx + w > dx2){ dx2 = cx + w }
      if(cy + h > dy2){ dy2 = cy + h }
      store32(state_ptr, dx1, 16) store32(state_ptr, dy1, 20)
      store32(state_ptr, dx2, 24) store32(state_ptr, dy2, 28)
      store32(state_ptr, 1, 12) ; dirty = true
   }

   if(h > mrh){ mrh = h }
   
   ;; Commit new packing cursor
   store32(state_ptr, cx + w + 2, 0)
   store32(state_ptr, cy, 4)
   store32(state_ptr, mrh, 8)

   def fw = float(aw) def fh = float(ah)
   def uv = [float(cx) / fw, float(cy) / fh, float(cx + w) / fw, float(cy + h) / fh]
   dict_set(items, key, uv)
   uv
}

fn atlas_flush(a){
   "Uploads dirty CPU pixels to GPU. Uses efficient sub-rectangle updates."
   if(!a){ return }
   def state_ptr = dict_get(a, "state_ptr", 0)
   if(!state_ptr || load32(state_ptr, 12) == 0){ return }

   def cpu_buf = dict_get(a, "cpu_buf", 0)
   def tex_id  = dict_get(a, "tex_id", -1)
   def aw = dict_get(a, "width")
   def ah = dict_get(a, "height")
   
   def dx1 = load32(state_ptr, 16) def dy1 = load32(state_ptr, 20)
   def dx2 = load32(state_ptr, 24) def dy2 = load32(state_ptr, 28)
   if(dx2 <= dx1 || dy2 <= dy1){ return }

   def rw = dx2 - dx1 def rh = dy2 - dy1
   if(rw == aw && rh == ah){
      vkr.update_texture_rect(tex_id, 0, 0, aw, ah, cpu_buf)
   } elif(rw == aw){
      vkr.update_texture_rect(tex_id, 0, dy1, aw, rh, cpu_buf + (dy1 * aw * 4))
   } else {
      ;; Rect upload
      def need = rw * rh * 4
      if(_atlas_scratch_cap < need){
         if(_atlas_scratch){ free(_atlas_scratch) }
         _atlas_scratch = malloc(need)
         _atlas_scratch_cap = need
      }
      mut r = 0 while(r < rh){
         memcpy(_atlas_scratch + r * rw * 4, cpu_buf + ((dy1 + r) * aw + dx1) * 4, rw * 4)
         r += 1
      }
      vkr.update_texture_rect(tex_id, dx1, dy1, rw, rh, _atlas_scratch)
   }

   ;; Reset dirty state
   store32(state_ptr, 0, 12) ; dirty = false
   store32(state_ptr, aw, 16) store32(state_ptr, ah, 20)
   store32(state_ptr, 0, 24) store32(state_ptr, 0, 28)
}

fn atlas_texture_id(a){ dict_get(a, "tex_id", -1) }
fn atlas_get(a, key){ dict_get(dict_get(a, "items"), key, 0) }
fn atlas_uv_rect(a, key){ atlas_get(a, key) }
fn atlas_bind(a){ vkr.bind_texture(atlas_texture_id(a)) }
