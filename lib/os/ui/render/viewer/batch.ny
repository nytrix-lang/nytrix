;; Keywords: viewer batch rect texture primitive os ui render
;; Batched rectangle and texture primitive drawing for fast UI overlays.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.batch(reset, shutdown, queued_count, flush, queue_rect, queue_outline, checker, static_checker, release_static_checker, rgba_mesh, release_mesh, draw_mesh, draw_tex_uv)
use std.core
use std.math as math
use std.os.ui.render as gfx
use std.os.ui.render.matrix as rmat

def RECT_STRIDE = 20
def TEX_STRIDE = 52
def RECT_INITIAL_CAP = 8192
def RECT_SOFT_FLUSH = 8192
def VERTEX_STRIDE = 64
def DEG_TO_RAD = 0.017453292519943295
mut _rects = 0
mut _cap = 0
mut _count = 0
mut _tex = 0
mut _tex_cap = 0
mut _tex_count = 0
mut _mesh_t = rmat.mat4_identity()
mut _mesh_r = rmat.mat4_identity()
mut _mesh_s = rmat.mat4_identity()
mut _mesh_tmp = rmat.mat4_identity()
mut _mesh_model = rmat.mat4_identity()
mut _mesh_ident = rmat.mat4_identity()

fn reset() any {
   "Drops queued rectangles without releasing the backing buffer."
   _count = 0
   _tex_count = 0
}

fn shutdown() any {
   "Flushes and releases the rectangle batch backing buffer."
   flush()
   if(_rects){ free(_rects) }
   if(_tex){ free(_tex) }
   _rects, _cap, _count = 0, 0, 0
   _tex, _tex_cap, _tex_count = 0, 0, 0
}

fn queued_count() int {
   "Returns queued rectangle count."
   _count + _tex_count
}

fn _reserve(int need) bool {
   if(need <= _cap){ return true }
   mut cap = _cap > 0 ? _cap : RECT_INITIAL_CAP
   while(cap < need){ cap *= 2 }
   def bytes = cap * RECT_STRIDE
   def p = _rects ? realloc(_rects, bytes) : malloc(bytes)
   if(!p){ return false }
   _rects, _cap = p, cap
   true
}

fn _reserve_tex(int need) bool {
   if(need <= _tex_cap){ return true }
   mut cap = _tex_cap > 0 ? _tex_cap : RECT_INITIAL_CAP
   while(cap < need){ cap *= 2 }
   def bytes = cap * TEX_STRIDE
   def p = _tex ? realloc(_tex, bytes) : malloc(bytes)
   if(!p){ return false }
   _tex, _tex_cap = p, cap
   true
}

@inline
fn _write(int idx, f64 x, f64 y, f64 w, f64 h, int color) any {
   def rec = _rects + idx * RECT_STRIDE
   store32_f32(rec, float(x), 0)
   store32_f32(rec, float(y), 4)
   store32_f32(rec, float(w), 8)
   store32_f32(rec, float(h), 12)
   store32(rec, int(color), 16)
}

@inline
fn _write_tex(
   int idx, f64 x, f64 y, f64 w, f64 h, int tex_id,
   f64 u1, f64 v1, f64 u2, f64 v2,
   f64 r, f64 g, f64 b, f64 a
) any {
   def rec = _tex + idx * TEX_STRIDE
   store32_f32(rec, float(x), 0)
   store32_f32(rec, float(y), 4)
   store32_f32(rec, float(w), 8)
   store32_f32(rec, float(h), 12)
   store32_f32(rec, float(u1), 16)
   store32_f32(rec, float(v1), 20)
   store32_f32(rec, float(u2), 24)
   store32_f32(rec, float(v2), 28)
   store32_f32(rec, float(r), 32)
   store32_f32(rec, float(g), 36)
   store32_f32(rec, float(b), 40)
   store32_f32(rec, float(a), 44)
   store32(rec, int(tex_id), 48)
}

fn _flush_tex() int {
   if(_tex_count <= 0 || !_tex){
      _tex_count = 0
      return 0
   }
   def n = _tex_count
   _tex_count = 0
   mut i = 0
   while(i < n){
      def rec = _tex + i * TEX_STRIDE
      gfx.draw_rect_tex_uv(
         load32_f32(rec, 0), load32_f32(rec, 4),
         load32_f32(rec, 8), load32_f32(rec, 12),
         load32(rec, 48),
         load32_f32(rec, 16), load32_f32(rec, 20),
         load32_f32(rec, 24), load32_f32(rec, 28),
         load32_f32(rec, 32), load32_f32(rec, 36),
         load32_f32(rec, 40), load32_f32(rec, 44)
      )
      i += 1
   }
   n
}

fn flush() int {
   "Submits queued packed rectangles/textures and returns the submitted count."
   mut n = 0
   if(_count <= 0 || !_rects){
      _count = 0
   } else {
      n = _count
      _count = 0
      gfx.draw_rects_fast_ptr(_rects, n, RECT_STRIDE)
   }
   n + _flush_tex()
}

fn queue_rect(f64 x, f64 y, f64 w, f64 h, int color) bool {
   "Queues one packed solid rectangle."
   if(w <= 0.0 || h <= 0.0){ return true }
   if(!_reserve(_count + 1)){
      gfx.draw_rect_fast(x, y, w, h, color)
      return true
   }
   _write(_count, x, y, w, h, color)
   _count += 1
   if(_count >= RECT_SOFT_FLUSH){ flush() }
   true
}

fn queue_outline(f64 x, f64 y, f64 w, f64 h, int color) bool {
   "Queues a 1px outline as four packed solid rectangles."
   if(w <= 0.0 || h <= 0.0){ return true }
   if(!_reserve(_count + 4)){
      flush()
      gfx.draw_rect_outline_fast(x, y, w, h, color)
      return true
   }
   _write(_count, x, y, w, 1.0, color)
   _write(_count + 1, x, y + h - 1.0, w, 1.0, color)
   _write(_count + 2, x, y, 1.0, h, color)
   _write(_count + 3, x + w - 1.0, y, 1.0, h, color)
   _count += 4
   if(_count >= RECT_SOFT_FLUSH){ flush() }
   true
}

@jit
fn checker(f64 world_left, f64 world_top, f64 sw, f64 sh, f64 cell, int color_a, int color_b) int {
   "Draws a camera-aligned checkerboard through the packed rectangle batch."
   if(sw <= 0.0 || sh <= 0.0 || cell <= 0.0){ return 0 }
   flush()
   def start_col, start_row = int(math.floor(world_left / cell)) - 1, int(math.floor(world_top / cell)) - 1
   def end_col = int(math.floor((world_left + sw) / cell)) + 2
   def end_row = int(math.floor((world_top + sh) / cell)) + 2
   def need = int(math.max(1, (end_col - start_col + 1) * (end_row - start_row + 1))) + 1
   if(!_reserve(need)){
      queue_rect(0.0, 0.0, sw, sh, color_b)
      mut row = start_row
      while(row <= end_row){
         def y0, y1 = math.floor(float(row) * cell - world_top), math.floor(float(row + 1) * cell - world_top)
         mut col = start_col
         while(col <= end_col){
            if((row + col) % 2 == 0){
               def x0, x1 = math.floor(float(col) * cell - world_left), math.floor(float(col + 1) * cell - world_left)
               queue_rect(x0, y0, x1 - x0, y1 - y0, color_a)
            }
            col += 1
         }
         row += 1
      }
      return flush()
   }
   _count = 0
   _write(_count, 0.0, 0.0, sw, sh, color_b)
   _count += 1
   mut row = start_row
   while(row <= end_row){
      def y0, y1 = math.floor(float(row) * cell - world_top), math.floor(float(row + 1) * cell - world_top)
      mut col = start_col
      while(col <= end_col){
         if((row + col) % 2 == 0){
            def x0, x1 = math.floor(float(col) * cell - world_left), math.floor(float(col + 1) * cell - world_left)
            _write(_count, x0, y0, x1 - x0, y1 - y0, color_a)
            _count += 1
         }
         col += 1
      }
      row += 1
   }
   flush()
}

@inline
fn _put_vertex(ptr p, int n, f64 x, f64 y, f64 u, f64 v, int color, int tex) int {
   def o = p + n * VERTEX_STRIDE
   store32_f32(o, x, 0)
   store32_f32(o, y, 4)
   store32_f32(o, u, 12)
   store32_f32(o, v, 16)
   store32(o, color, 20)
   store32_f32(o, 1.0, 32)
   store32_f32(o, 1.0, 36)
   store32_f32(o, 1.0, 48)
   store32(o, tex, 60)
   0
}

@inline
fn _put_quad(ptr p, int n, f64 x, f64 y, f64 w, f64 h, int color) int {
   _put_vertex(p, n, x, y, 0.0, 0.0, color, 0)
   _put_vertex(p, n + 1, x, y + h, 0.0, 1.0, color, 0)
   _put_vertex(p, n + 2, x + w, y + h, 1.0, 1.0, color, 0)
   _put_vertex(p, n + 3, x + w, y + h, 1.0, 1.0, color, 0)
   _put_vertex(p, n + 4, x + w, y, 1.0, 0.0, color, 0)
   _put_vertex(p, n + 5, x, y, 0.0, 0.0, color, 0)
   n + 6
}

fn release_static_checker(list cache) int {
   "Releases the buffers returned by `static_checker`."
   def sbuf = cache.get(0, 0)
   if(is_dict(sbuf)){ gfx.mesh_destroy(sbuf) }
   if(cache.get(6, 0)){ free(cache.get(6, 0)) }
   0
}

fn _static_checker_dynamic(
   ptr buf,
   int cap,
   int start_col,
   int start_row,
   int end_col,
   int end_row,
   f64 cell,
   int color
) int {
   mut n = 0
   mut row = start_row
   while(row <= end_row && n < cap){
      mut col = start_col
      while(col <= end_col && n < cap){
         if((row + col) % 2 == 0){
            def rec = buf + n * RECT_STRIDE
            store32_f32(rec, float(col) * cell, 0)
            store32_f32(rec, float(row) * cell, 4)
            store32_f32(rec, cell, 8)
            store32_f32(rec, cell, 12)
            store32(rec, color, 16)
            n += 1
         }
         col += 1
      }
      row += 1
   }
   n
}

fn static_checker(list cache, f64 world_left, f64 world_top, f64 sw, f64 sh, f64 cell, int color_a) list {
   "Draws a camera-aligned checkerboard through a cached static GPU mesh."
   if(sw <= 0.0 || sh <= 0.0 || cell <= 0.0){ return cache }
   def start_col, start_row = int(math.floor(world_left / cell)) - 1, int(math.floor(world_top / cell)) - 1
   def end_col = int(math.floor((world_left + sw) / cell)) + 2
   def end_row = int(math.floor((world_top + sh) / cell)) + 2
   def changed = start_col != int(cache.get(1, 0)) ||
      start_row != int(cache.get(2, 0)) ||
      end_col != int(cache.get(3, 0)) ||
      end_row != int(cache.get(4, 0))
   mut sbuf = cache.get(0, 0)
   mut count = int(cache.get(5, 0))
   mut buf = cache.get(6, 0)
   mut cap = int(cache.get(7, 0))
   mut mode = int(cache.get(8, 0))
   def need = int(math.max(2, (end_col - start_col + 1) * (end_row - start_row + 1)))
   if(changed || mode == 0){
      if(is_dict(sbuf)){ gfx.mesh_destroy(sbuf) }
      sbuf = 0
      count = 0
      def verts = malloc(need * 6 * VERTEX_STRIDE)
      if(verts){
         mut row = start_row
         while(row <= end_row){
            mut col = start_col
            while(col <= end_col){
               if((row + col) % 2 == 0){
                  count = _put_quad(verts, count, float(col) * cell, float(row) * cell, cell, cell, color_a)
               }
               col += 1
            }
            row += 1
         }
         mut opts = dict(4)
         opts["unlit"] = true
         opts["vc_mode"] = 1
         sbuf = gfx.mesh_create_static(verts, count, false, opts)
         if(!is_dict(sbuf)){ free(verts) }
      }
      if(is_dict(sbuf)){
         mode = 1
      } else {
         mode = 2
         if(need > cap){
            if(buf){ free(buf) }
            buf = malloc(need * RECT_STRIDE)
            cap = need
         }
         count = buf ? _static_checker_dynamic(buf, cap, start_col, start_row, end_col, end_row, cell, color_a) : 0
      }
   }
   gfx.set_ortho_2d(world_left, world_left + sw, world_top, world_top + sh)
   if(mode == 1 && is_dict(sbuf)){
      gfx.set_unlit(true)
      gfx.draw_mesh(sbuf, false, 1.0)
   } elif(buf && count > 0){
      gfx.draw_rects_fast_ptr(buf, count, RECT_STRIDE)
   }
   [sbuf, start_col, start_row, end_col, end_row, count, buf, cap, mode]
}

fn rgba_mesh(any data, int w, int h, int channels=4, f64 scale=1.0, int alpha_min=1) dict {
   "Builds a static colored-quad mesh from raw RGBA/RGB image pixels."
   if(!data || w <= 0 || h <= 0 || channels <= 0 || scale <= 0.0){ return {} }
   mut visible = 0
   mut p = 0
   def pixels = w * h
   while(p < pixels){
      def a = channels > 3 ? load8(data, p * channels + 3) : 255
      if(a >= alpha_min){ visible += 1 }
      p += 1
   }
   if(visible <= 0){ return {} }
   def verts = malloc(visible * 6 * VERTEX_STRIDE)
   if(!verts){ return {} }
   mut n = 0
   mut y = 0
   while(y < h){
      mut x = 0
      while(x < w){
         def si = (y * w + x) * channels
         def a = channels > 3 ? load8(data, si + 3) : 255
         if(a >= alpha_min){
            def r = load8(data, si)
            def g = channels > 1 ? load8(data, si + 1) : r
            def b = channels > 2 ? load8(data, si + 2) : r
            def color = gfx.color_pack(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, float(a) / 255.0)
            n = _put_quad(
               verts,
               n,
               (float(x) - float(w) * 0.5) * scale,
               (float(y) - float(h) * 0.5) * scale,
               scale,
               scale,
               color
            )
         }
         x += 1
      }
      y += 1
   }
   mut opts = dict(4)
   opts["unlit"] = true
   opts["vc_mode"] = 1
   def sbuf = gfx.mesh_create_static(verts, n, false, opts)
   if(!is_dict(sbuf)){ free(verts) }
   is_dict(sbuf) ? {"mesh": sbuf, "count": n, "w": w, "h": h, "scale": scale} : {}
}

fn release_mesh(any mesh) int {
   "Releases a mesh returned by `rgba_mesh`."
   if(is_dict(mesh)){
      def sbuf = mesh.get("mesh", 0)
      if(is_dict(sbuf)){ gfx.mesh_destroy(sbuf) }
   }
   0
}

fn draw_mesh(dict mesh, f64 cx, f64 cy, f64 rot_deg=0.0, bool flip_x=false) bool {
   "Draws a static colored mesh around `cx,cy` with optional rotation and X flip."
   def sbuf = mesh.get("mesh", 0)
   def count = int(mesh.get("count", 0))
   if(!is_dict(sbuf) || count <= 0){ return false }
   rmat.mat4_translate_into(cx, cy, 0.0, _mesh_t)
   rmat.mat4_rotate_z_into(rot_deg * DEG_TO_RAD, _mesh_r)
   rmat.mat4_scale_into(flip_x ? -1.0 : 1.0, 1.0, 1.0, _mesh_s)
   rmat.mat4_mul_into(_mesh_r, _mesh_s, _mesh_tmp)
   rmat.mat4_mul_into(_mesh_t, _mesh_tmp, _mesh_model)
   gfx.set_model_matrix(_mesh_model)
   gfx.set_unlit(true)
   def ok = gfx.draw_mesh(sbuf, false, 1.0)
   gfx.set_model_matrix(_mesh_ident)
   ok
}

fn draw_tex_uv(
   f64 x, f64 y, f64 w, f64 h, int tex_id,
   f64 u1, f64 v1, f64 u2, f64 v2,
   f64 r=1.0, f64 g=1.0, f64 b=1.0, f64 a=1.0
) bool {
   "Queues a textured rectangle. Solids and textures are flushed together before text."
   if(w <= 0.0 || h <= 0.0){ return true }
   if(!_reserve_tex(_tex_count + 1)){
      flush()
      gfx.draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a)
      return true
   }
   _write_tex(_tex_count, x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a)
   _tex_count += 1
   if(_tex_count >= RECT_SOFT_FLUSH){ flush() }
   true
}
