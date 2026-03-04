;; Keywords: ui gfx atlas texture packing
;; Texture Atlas manager for std.ui.gfx.
;; Efficiently packs many small images (glyphs, icons) into larger textures.
;;
;; BATCHED UPLOAD MODE:
;;   All glyph data is written directly into a CPU-side pixel buffer during atlas_add.
;;   Call atlas_flush(a) to do ONE GPU upload of the entire dirty region.
;;   This reduces font loading from thousands of GPU submissions to 1.

module std.ui.gfx.atlas (
   atlas_create, atlas_destroy, atlas_add, atlas_get, atlas_bind,
   atlas_texture_id, atlas_uv_rect, atlas_flush
)

use std.core *
use std.ui.gfx.vk as vkr
use std.math *

mut _atlas_scratch = 0
mut _atlas_scratch_cap = 0

;; Atlas object fields:
;;   tex_id, width, height, cx, cy, max_row_h, items
;;   cpu_buf   -- CPU-side pixel buffer (always kept; used for batched uploads)
;;   dirty     -- bool: cpu_buf has unpushed changes
;;   dirty_x1/y1/x2/y2 -- bounding box of dirty region for minimal uploads

fn atlas_create(w=2048, h=2048){
   "Creates a new texture atlas of the specified dimensions (RGBA8, 4bpp).
   All glyph writes go to a CPU buffer ; call atlas_flush() to push to GPU."
   def buf_size = w * h * 4
   def cpu_buf = malloc(buf_size)
   memset(cpu_buf, 0, buf_size)

   ;; Create GPU texture with blank data
   def tex_id = vkr.create_texture_ex(w, h, cpu_buf, 37)

   mut a = dict(16)
   a = dict_set(a, "tex_id", tex_id)
   a = dict_set(a, "width", w)
   a = dict_set(a, "height", h)
   a = dict_set(a, "cx", 2)
   a = dict_set(a, "cy", 2)
   a = dict_set(a, "max_row_h", 0)
   a = dict_set(a, "items", dict(256))
   a = dict_set(a, "cpu_buf", cpu_buf)
   a = dict_set(a, "dirty", false)
   a = dict_set(a, "dirty_x1", w)
   a = dict_set(a, "dirty_y1", h)
   a = dict_set(a, "dirty_x2", 0)
   a = dict_set(a, "dirty_y2", 0)
   a
}

fn atlas_destroy(a){
   "Destroys the atlas and frees its CPU buffer."
   if(!a){ return }
   def tex_id = dict_get(a, "tex_id", -1)
   if(tex_id >= 0){ vkr.destroy_texture(tex_id) }
   def cpu_buf = dict_get(a, "cpu_buf", 0)
   if(cpu_buf){ free(cpu_buf) }
}

fn atlas_add(a, key, w, h, pixels){
   "Packs a new image into the atlas CPU buffer. Returns [u1,v1,u2,v2] or 0.
   Does NOT upload to GPU -- call atlas_flush() after priming is done."
   if(!a || w <= 0 || h <= 0){ return 0 }

   mut items = dict_get(a, "items")
   if(dict_has(items, key)){ return dict_get(items, key) }

   def aw = dict_get(a, "width")
   def ah = dict_get(a, "height")
   mut cx = dict_get(a, "cx")
   mut cy = dict_get(a, "cy")
   mut mrh = dict_get(a, "max_row_h")

   ;; Shelf packing with 2px padding
   if(cx + w + 2 > aw){
      cx = 2
      cy = cy + mrh + 2
      mrh = 0
   }
   if(cy + h + 2 > ah){ return 0 } ;; Atlas full

   ;; Write glyph pixels directly into CPU buffer (row by row)
   def cpu_buf = dict_get(a, "cpu_buf", 0)
   if(cpu_buf && pixels){
      mut row = 0
      while(row < h){
         def src_off = row * w * 4
         def dst_off = ((cy + row) * aw + cx) * 4
         memcpy(cpu_buf + dst_off, pixels + src_off, w * 4)
         row += 1
      }
      ;; Expand dirty bounding box
      mut dx1 = dict_get(a, "dirty_x1") mut dy1 = dict_get(a, "dirty_y1")
      mut dx2 = dict_get(a, "dirty_x2") mut dy2 = dict_get(a, "dirty_y2")
      if(cx < dx1){ dx1 = cx }
      if(cy < dy1){ dy1 = cy }
      if(cx + w > dx2){ dx2 = cx + w }
      if(cy + h > dy2){ dy2 = cy + h }
      dict_set(a, "dirty_x1", dx1) dict_set(a, "dirty_y1", dy1)
      dict_set(a, "dirty_x2", dx2) dict_set(a, "dirty_y2", dy2)
      dict_set(a, "dirty", true)
   }

   if(h > mrh){ mrh = h }

   def fw = float(aw) def fh = float(ah)
   def u1 = float(cx) / fw def v1 = float(cy) / fh
   def u2 = float(cx + w) / fw def v2 = float(cy + h) / fh
   def uv = [u1, v1, u2, v2]

   dict_set(items, key, uv)
   dict_set(a, "cx", cx + w + 2)
   dict_set(a, "cy", cy)
   dict_set(a, "max_row_h", mrh)

   uv
}

fn atlas_flush(a){
   "Uploads the dirty region of the CPU atlas buffer to the GPU in one call.
   Call this once after all atlas_add() calls are done during font priming."
   if(!a){ return }
   if(!dict_get(a, "dirty", false)){ return }

   def cpu_buf = dict_get(a, "cpu_buf", 0)
   def tex_id  = dict_get(a, "tex_id", -1)
   if(!cpu_buf || tex_id < 0){ return }

   def aw = dict_get(a, "width")
   def dx1 = dict_get(a, "dirty_x1") def dy1 = dict_get(a, "dirty_y1")
   def dx2 = dict_get(a, "dirty_x2") def dy2 = dict_get(a, "dirty_y2")
   if(dx2 <= dx1 || dy2 <= dy1){ return }

   ;; Upload only the dirty sub-rect. Avoid extra copies when possible.
   def rw = dx2 - dx1 def rh = dy2 - dy1
   def full_w = (dx1 == 0 && dx2 == aw)
   def full_h = (dy1 == 0 && dy2 == dict_get(a, "height"))
   def full_area = full_w && full_h
   def dirty_area = rw * rh
   def atlas_area = aw * dict_get(a, "height")

   if(full_area){
      vkr.update_texture_rect(tex_id, 0, 0, aw, dict_get(a, "height"), cpu_buf)
   } elif(full_w){
      ;; Dirty span is full width: contiguous in memory, no scratch needed.
      def src = cpu_buf + (dy1 * aw * 4)
      vkr.update_texture_rect(tex_id, 0, dy1, aw, rh, src)
   } elif(dirty_area * 10 >= atlas_area * 6){
      ;; If most of the atlas changed, a full upload is faster than re-packing.
      vkr.update_texture_rect(tex_id, 0, 0, aw, dict_get(a, "height"), cpu_buf)
   } else {
      def row_bytes = rw * 4
      def need = rw * rh * 4
      if(_atlas_scratch_cap < need){
         if(_atlas_scratch){ free(_atlas_scratch) }
         _atlas_scratch = malloc(need)
         _atlas_scratch_cap = need
      }
      mut row = 0
      while(row < rh){
         def src = cpu_buf + ((dy1 + row) * aw + dx1) * 4
         memcpy(_atlas_scratch + row * row_bytes, src, row_bytes)
         row += 1
      }
      vkr.update_texture_rect(tex_id, dx1, dy1, rw, rh, _atlas_scratch)
   }

   ;; Reset dirty state
   dict_set(a, "dirty", false)
   dict_set(a, "dirty_x1", aw)
   dict_set(a, "dirty_y1", dict_get(a, "height"))
   dict_set(a, "dirty_x2", 0)
   dict_set(a, "dirty_y2", 0)
}

fn atlas_get(a, key){
   "Retrieves UV coordinates for a packed item."
   def items = dict_get(a, "items")
   dict_get(items, key, 0)
}

fn atlas_texture_id(a){
   "Returns the texture id stored in atlas `a`."
   dict_get(a, "tex_id", -1)
}

fn atlas_uv_rect(a, key){
   "Alias for atlas_get."
   atlas_get(a, key)
}

fn atlas_bind(a){
   "Binds the atlas texture for drawing."
   vkr.bind_texture(atlas_texture_id(a))
}
