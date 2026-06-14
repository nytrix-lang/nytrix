;; Keywords: render atlas os ui
;; Texture Atlas manager for std.os.ui.render.
;; Efficiently packs many small images (glyphs, icons) into larger textures.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.atlas(atlas_set_backend, atlas_get_backend, atlas_create, atlas_destroy, atlas_add, atlas_get, atlas_bind, atlas_texture_id, atlas_uv_rect, atlas_ensure_texture, atlas_flush)
use std.core
use std.os.ui.render.vk as vkr
use std.os.ui.render.vk.texture as vk_texture
use std.os.ui.render.gl as glr
use std.math

mut _atlas_backend = "vk"

fn atlas_set_backend(any backend) bool {
   "Sets the backend used by newly-created or deferred atlas textures."
   def b = to_str(backend)
   if b == "gl" || b == "opengl" { _atlas_backend = "gl" }
   elif b == "vk" || b == "vulkan" { _atlas_backend = "vk" }
   elif b == "mock" || b == "cpu" || b == "none" { _atlas_backend = "mock" }
   true
}

fn atlas_get_backend() str { _atlas_backend }

fn _atlas_backend_name(any a=0) str {
   if is_dict(a) {
      def b = to_str(a.get("backend", _atlas_backend))
      if b == "gl" || b == "opengl" { return "gl" }
      if b == "vk" || b == "vulkan" { return "vk" }
      if b == "mock" || b == "cpu" || b == "none" { return "mock" }
   }
   _atlas_backend
}

fn _atlas_use_gl(any a=0) bool { _atlas_backend_name(a) == "gl" }

fn _atlas_stable_tex_id(any candidate, any a=0) int {
   mut tex_id = int(candidate)
   def stable = int(_atlas_use_gl(a) ? glr.last_created_texture_id() : vkr.last_created_texture_id())
   if stable >= 0 && stable < 1024 { return stable }
   def count = int(_atlas_use_gl(a) ? glr.texture_count() : vkr.texture_count())
   if (tex_id < 0 || tex_id >= 1024) && count > 0 {
      def latest = count - 1
      if latest >= 0 && latest < 1024 { return latest }
   }
   tex_id
}

fn _atlas_create_texture(int w, int h, any pixels, int filter, any a=0) int {
   if _atlas_use_gl(a) {
      def id = int(glr.create_texture_ex(w, h, pixels, 37, filter, 33071, 33071))
      return id
   }
   if _atlas_backend_name(a) == "mock" { return -1 }
   _atlas_stable_tex_id(vkr.create_texture_ex(w, h, pixels, 37, filter, 33071, 33071), a)
}

fn _atlas_destroy_texture(int tex_id, any a=0) bool {
   if tex_id < 0 { return false }
   if _atlas_use_gl(a) { return glr.destroy_texture(tex_id) }
   if _atlas_backend_name(a) == "mock" { return false }
   vk_texture.set_texture_protected(tex_id, false)
   vkr.destroy_texture(tex_id)
   true
}

fn _atlas_bind_texture(int tex_id, any a=0) bool {
   if _atlas_use_gl(a) { return glr.bind_texture(tex_id) }
   if _atlas_backend_name(a) == "mock" { return false }
   vkr.bind_texture(tex_id)
   true
}

fn _atlas_update_texture_rect(int tex_id, int x, int y, int w, int h, any pixels, any a=0) bool {
   if _atlas_use_gl(a) { return glr.update_texture_rect(tex_id, x, y, w, h, pixels) }
   if _atlas_backend_name(a) == "mock" { return false }
   vkr.update_texture_rect(tex_id, x, y, w, h, pixels)
}

mut _atlas_scratch = 0
mut _atlas_scratch_cap = 0

fn atlas_create(any w=2048, any h=2048, any filter=-1, bool defer_gpu=false) any {
   "Creates a new texture atlas. Uses a native state buffer for mutable metadata."
   def buf_size = w * h * 4
   def cpu_buf = zalloc(buf_size)
   if !cpu_buf { return 0 }
   def state_ptr = zalloc(64)
   if !state_ptr {
      free(cpu_buf)
      return 0
   }
   store32(state_ptr, 2, 0)
   store32(state_ptr, 2, 4)
   store32(state_ptr, 0, 8)
   store32(state_ptr, 0, 12)
   store32(state_ptr, w, 16)
   store32(state_ptr, h, 20)
   store32(state_ptr, 0, 24)
   store32(state_ptr, 0, 28)
   mut out = {
      "tex_id": -1,
      "width": w,
      "height": h,
      "items": dict(512),
      "cpu_buf": cpu_buf,
      "state_ptr": state_ptr,
      "filter": filter,
      "backend": _atlas_backend
   }
   def tex_id = defer_gpu ? -1 : _atlas_create_texture(w, h, cpu_buf, int(filter), out)
   if tex_id >= 0 && !_atlas_use_gl(out) { vk_texture.set_texture_protected(tex_id, true) }
   out["tex_id"] = tex_id
   return out
}

fn atlas_destroy(any a) int {
   "Frees atlas resources including GPU texture and state buffers."
   if !is_dict(a) { return 0 }
   def tex_id = a.get("tex_id", -1)
   if tex_id >= 0 {
      _atlas_destroy_texture(tex_id, a)
   }
   def cpu_buf = a.get("cpu_buf", 0)
   if cpu_buf { free(cpu_buf) }
   def state_ptr = a.get("state_ptr", 0)
   if state_ptr { free(state_ptr) }
   0
}

fn atlas_ensure_texture(any a) int {
   "Ensures the CPU atlas has a live GPU texture. This allows fonts/icons to be
   loaded before GPU initialization and become valid on first draw."
   if !is_dict(a) { return -1 }
   mut tex_id = int(a.get("tex_id", -1))
   def cur_backend = to_str(a.get("backend", ""))
   if tex_id >= 0 && cur_backend == _atlas_backend { return tex_id }
   def cpu_buf = a.get("cpu_buf", 0)
   def aw = int(a.get("width", 0))
   def ah = int(a.get("height", 0))
   if !cpu_buf || aw <= 0 || ah <= 0 { return -1 }
   a["backend"] = _atlas_backend
   tex_id = _atlas_create_texture(aw, ah, cpu_buf, int(a.get("filter", -1)), a)
   if tex_id >= 0 {
      if !_atlas_use_gl(a) { vk_texture.set_texture_protected(tex_id, true) }
      a["tex_id"] = tex_id
   }
   tex_id
}

fn atlas_add(any a, any key, any w, any h, any pixels) any {
   "Packs an image and returns [u1,v1,u2,v2]. Correctly updates packing state."
   if !is_dict(a) || w <= 0 || h <= 0 { return 0 }
   mut items = a.get("items")
   if items.contains(key) { return items.get(key) }
   def aw, ah = a.get("width"), a.get("height")
   def state_ptr = a.get("state_ptr", 0)
   if !state_ptr { return 0 }
   mut cx, cy = load32(state_ptr, 0), load32(state_ptr, 4)
   mut mrh = load32(state_ptr, 8)
   if cx + w + 2 > aw {
      cx, cy = 2, cy + mrh + 2
      mrh = 0
   }
   if cy + h + 2 > ah { return 0 }
   def cpu_buf = a.get("cpu_buf", 0)
   if cpu_buf && pixels {
      mut row = 0
      while row < h {
         def src_off = row * w * 4
         def dst_off = ((cy + row) * aw + cx) * 4
         mut col = 0
         while col < w {
            def src_px = src_off + col * 4
            def dst_px = dst_off + col * 4
            store8(cpu_buf, load8(pixels, src_px) & 255, dst_px)
            store8(cpu_buf, load8(pixels, src_px + 1) & 255, dst_px + 1)
            store8(cpu_buf, load8(pixels, src_px + 2) & 255, dst_px + 2)
            store8(cpu_buf, load8(pixels, src_px + 3) & 255, dst_px + 3)
            col += 1
         }
         def left_dst = ((cy + row) * aw + (cx - 1)) * 4
         store8(cpu_buf, load8(pixels, src_off) & 255, left_dst)
         store8(cpu_buf, load8(pixels, src_off + 1) & 255, left_dst + 1)
         store8(cpu_buf, load8(pixels, src_off + 2) & 255, left_dst + 2)
         store8(cpu_buf, load8(pixels, src_off + 3) & 255, left_dst + 3)
         def right_src = src_off + ((w - 1) * 4)
         def right_dst = ((cy + row) * aw + (cx + w)) * 4
         store8(cpu_buf, load8(pixels, right_src) & 255, right_dst)
         store8(cpu_buf, load8(pixels, right_src + 1) & 255, right_dst + 1)
         store8(cpu_buf, load8(pixels, right_src + 2) & 255, right_dst + 2)
         store8(cpu_buf, load8(pixels, right_src + 3) & 255, right_dst + 3)
         row += 1
      }
      memcpy(
         cpu_buf + (((cy - 1) * aw + (cx - 1)) * 4),
         cpu_buf + ((cy * aw + (cx - 1)) * 4),
         (w + 2) * 4
      )
      memcpy(
         cpu_buf + (((cy + h) * aw + (cx - 1)) * 4),
         cpu_buf + ((((cy + h) - 1) * aw + (cx - 1)) * 4),
         (w + 2) * 4
      )
      mut dx1, dy1 = load32(state_ptr, 16), load32(state_ptr, 20)
      mut dx2, dy2 = load32(state_ptr, 24), load32(state_ptr, 28)
      if cx - 1 < dx1 { dx1 = cx - 1 }
      if cy - 1 < dy1 { dy1 = cy - 1 }
      if cx + w + 1 > dx2 { dx2 = cx + w + 1 }
      if cy + h + 1 > dy2 { dy2 = cy + h + 1 }
      store32(state_ptr, dx1, 16) store32(state_ptr, dy1, 20)
      store32(state_ptr, dx2, 24) store32(state_ptr, dy2, 28)
      store32(state_ptr, 1, 12)
   }
   if h > mrh { mrh = h }
   store32(state_ptr, cx + w + 2, 0)
   store32(state_ptr, cy, 4)
   store32(state_ptr, mrh, 8)
   def fw, fh = float(aw), float(ah)
   def uv = [
      float(cx) / fw,
      float(cy) / fh,
      float(cx + w) / fw,
      float(cy + h) / fh
   ]
   items[key] = uv
   uv
}

fn atlas_flush(any a) int {
   "Uploads dirty CPU pixels to the existing GPU atlas texture."
   if !is_dict(a) { return 0 }
   def state_ptr = a.get("state_ptr", 0)
   if !state_ptr || load32(state_ptr, 12) == 0 { return 0 }
   def cpu_buf = a.get("cpu_buf", 0)
   def aw = a.get("width")
   def ah = a.get("height")
   if !cpu_buf || aw <= 0 || ah <= 0 { return 0 }
   def tex_id = atlas_ensure_texture(a)
   if tex_id < 0 { return 0 }
   mut dx1, dy1 = load32(state_ptr, 16), load32(state_ptr, 20)
   mut dx2, dy2 = load32(state_ptr, 24), load32(state_ptr, 28)
   if dx1 < 0 { dx1 = 0 }
   if dy1 < 0 { dy1 = 0 }
   if dx2 > aw { dx2 = aw }
   if dy2 > ah { dy2 = ah }
   def rw, rh = dx2 - dx1, dy2 - dy1
   if rw <= 0 || rh <= 0 { return 0 }
   def bytes = rw * rh * 4
   if bytes > _atlas_scratch_cap {
      if _atlas_scratch { free(_atlas_scratch) }
      _atlas_scratch = malloc(bytes)
      _atlas_scratch_cap = _atlas_scratch ? bytes : 0
   }
   if !_atlas_scratch { return 0 }
   mut row = 0
   while row < rh {
      memcpy(
         _atlas_scratch + row * rw * 4,
         cpu_buf + (((dy1 + row) * aw + dx1) * 4),
         rw * 4
      )
      row += 1
   }
   _atlas_update_texture_rect(tex_id, dx1, dy1, rw, rh, _atlas_scratch, a)
   store32(state_ptr, 0, 12)
   store32(state_ptr, aw, 16) store32(state_ptr, ah, 20)
   store32(state_ptr, 0, 24) store32(state_ptr, 0, 28)
   0
}

fn atlas_texture_id(any a) int {
   "Runs the atlas texture id operation."
   is_dict(a) ? a.get("tex_id", -1) : -1
}

fn atlas_get(any a, any key) any {
   "Runs the atlas get operation."
   is_dict(a) ? a.get("items").get(key, 0) : 0
}

fn atlas_uv_rect(any a, any key) any { atlas_get(a, key) }

fn atlas_bind(any a) int {
   "Runs the atlas bind operation."
   if is_dict(a) { _atlas_bind_texture(atlas_texture_id(a), a) }
   0
}
