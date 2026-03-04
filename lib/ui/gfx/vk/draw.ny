;; Keywords: ui gfx vulkan renderer draw

module std.ui.gfx.vk.draw (
   draw_rect_fast, draw_rect, draw_rect_tex, draw_rect_tex_uv, draw_line, draw_glyph,
   draw_rectangle_fast,
   _draw_triangle_2d, draw_triangle_3d, draw_quad_3d, draw_vertices, draw_lines_raw,
   draw_line_3d, draw_grid_3d, draw_axes_3d, draw_cube_3d, draw_line_strip_2d,
   draw_static_buffer, draw_circle_sdf, draw_ring_sdf
)

use std.core *
use std.core.mem *
use std.math *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.utils *
use std.ui.gfx.vk.renderer (_check_flush, _flush, _sync_pc)
use std.ui.gfx.vk.texture (bind_texture, texture_descriptor)

fn _bind_descriptors(cb){
   "Binds descriptor sets if changed. Returns ubo_ds for callers that need it."
   def ubo_ds = _current_frame_ubo_ds
   if(_bindless_enabled){
      if(_bindless_ds && (_bindless_ds != _last_bound_ds || ubo_ds != _last_bound_ubo_ds)){
         store64_raw(_ptr_ds, _bindless_ds, 0)
         store64_raw(_ptr_ds, ubo_ds, 8)
         cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 2, _ptr_ds, 0, 0)
         _last_bound_ds = _bindless_ds
         _last_bound_ubo_ds = ubo_ds
      }
   } else {
      mut tid = _current_texture_id
      if(tid < 0 || tid >= len(_textures)){ tid = _default_texture }
      def ds = texture_descriptor(tid)
      if(ds && (ds != _last_bound_ds || tid != _last_bound_tex_id || ubo_ds != _last_bound_ubo_ds)){
         store64_raw(_ptr_ds, ds, 0)
         store64_raw(_ptr_ds, ubo_ds, 8)
         cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 2, _ptr_ds, 0, 0)
         _last_bound_ds = ds _last_bound_tex_id = tid
         _last_bound_ubo_ds = ubo_ds
      }
   }
}

@jit
fn draw_rect_fast(x, y, w, h, color_u32){
   "Submits a rectangle using pre-packed color and fixed vertex layout."
   if(!_frame_open){ return 0 }
   if(_current_texture_id != _default_texture){ bind_texture(_default_texture) }
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   __vkr_push_rect_tex_fast(_local_vertex_map + _vertex_offset, x, y, w, h, 0, 0, 0, 0, color_u32, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

@jit
fn draw_rect(x, y, w, h, r, g, b, a){
   "Batches a colored rectangle (6-vertex CW triangle list) — optimized path."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   def c = _pack_color(r, g, b, a)
   _push_rect_packed(x, y, w, h, c)
}

@jit
fn draw_rectangle_fast(x, y, w, h, color_packed){
   "Submits a rectangle using a pre-packed color value."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   _push_rect_packed(x, y, w, h, color_packed)
}

@jit
fn _push_rect_packed(x, y, w, h, c){
   "Unrolled 6-vertex quad submission for minimal interpreter overhead."
   def off = _local_vertex_map + _vertex_offset
   __vkr_push_rect(off, x, y, w, h, c)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

@jit
fn _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, c){
   "Internal: batches a textured quad using packed color `c`."
   if(!_frame_open){ return 0 }
   def tid = (tex_id < 0 || tex_id >= len(_textures)) ? _default_texture : tex_id
   bind_texture(tid)
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   __vkr_push_rect_tex(_local_vertex_map + _vertex_offset, x, y, w, h, u1, v1, u2, v2, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

@jit
fn draw_rect_tex(x, y, w, h, tex_id, r, g, b, a){
   "Batches a textured rectangle (6-vertex triangle list) — optimized."
   _draw_textured_rect_packed(x, y, w, h, tex_id, 0.0, 0.0, 1.0, 1.0, _pack_color(r, g, b, a))
}

@jit
fn draw_glyph(x, y, w, h, u1, v1, u2, v2, tex_id, r, g, b, a){
   "Submits a glyph quad for text rendering."
   _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, _pack_color(r, g, b, a))
}

@jit
fn draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a){
   "Batches a textured rectangle with explicit UV coordinates."
   _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, _pack_color(r, g, b, a))
}

@jit
fn _push_rect_tex_packed(x, y, w, h, u1, v1, u2, v2, c){
   "Fully unrolled textured 6-vertex quad submission."
   def off = _local_vertex_map + _vertex_offset
   __vkr_push_rect_tex(off, x, y, w, h, u1, v1, u2, v2, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

fn draw_vertices(ptr, count, tex_id){
   "Bulk-uploads raw vertex data (packed vertex stride) to the local mapping."
   if(!_frame_open || count <= 0 || !ptr){ return 0 }
   bind_texture(tex_id)
   def bytes = count * _VKR_VERT_STRIDE
   if(!_check_flush(bytes)){ return 0 }
   memcpy(_local_vertex_map + _vertex_offset, ptr, bytes)
   _vertex_offset += bytes
   true
}

fn draw_lines_raw(ptr, line_count, _line_width){
   "Draws lines using pre-baked raw vertex buffer. thickness controls GPU line thickness."
   if(!_frame_open || line_count <= 0 || !ptr || !_line_pipeline){ return 0 }
   _flush() ; flush pending triangles first

   def cb = _current_frame_cb
   if(!_check_flush(line_count * 2 * _VKR_VERT_STRIDE)){ return 0 }

   ; Switch to line pipeline
   if(_last_bound_pipe != _line_pipeline){
      cmd_bind_pipeline(cb, 0, _line_pipeline)
      _last_bound_pipe = _line_pipeline
      _pc_dirty = true
   }

   _bind_descriptors(cb)

   ; Update constants for lines (always unlit, never mask)
   if(_mvp_dirty){ memcpy(_pc_buffer, _current_mvp, 64) _mvp_dirty = false }
   if(_model_dirty){ memcpy(_pc_buffer + 64, _current_model, 64) _model_dirty = false }
   store32(_pc_buffer, 0, 128)
   store32(_pc_buffer, 1, 132)
   store32(_pc_buffer, 0, 136)
   store32(_pc_buffer, 0, 140)
   if(_ubo_enabled){
      if(_ubo_map){
         def ubo_off = _current_frame * _ubo_stride
         if(!_vk_guard_span(_ubo_map + ubo_off, _UBO_SIZE, _ubo_map, _ubo_map_size, "ubo_map")){ return }
         memcpy(_ubo_map + ubo_off, _pc_buffer, _UBO_SIZE)
      }
      _last_is_mask = 0
      _last_is_unlit = 1
      _pc_dirty = true ; force next draw to refresh UBO for its state
   } else {
      cmd_push_constants(cb, _pipeline_layout, 1 | 16, 0, 160, _pc_buffer)
      _pc_dirty = false
   }

   ; NOTE: vkCmdSetLineWidth requires float in xmm0 (x86-64 ABI) which Nytrix FFI
   ; cannot pass correctly via ffi_call. Line width stays at the static pipeline value (1.0).
   ; The parameter is reserved for future typed-extern support.

   ; Copy vertices
   def bytes = line_count * 2 * _VKR_VERT_STRIDE
   def first_vert = _vertex_offset / _VKR_VERT_STRIDE
   memcpy(_local_vertex_map + _vertex_offset, ptr, bytes)

   ; Draw using firstVertex within the existing slice VBO binding (no rebind needed)
   cmd_draw(cb, line_count * 2, 1, first_vert, 0)

   _vertex_offset += bytes
   _last_flush_offset = _vertex_offset

   ; Rebind VBO back to slice start so subsequent triangle _flush calls work
   store64_raw(_flush_off, _current_frame_vertex_offset, 0)
   cmd_bind_vertex_buffers(cb, 0, 1, _flush_buf, _flush_off)
   true
}

fn _draw_triangle_2d(x1, y1, x2, y2, x3, y3, r, g, b, a){
   "Batches a colored 2D triangle."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 3)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base, 0, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 1, x2, y2, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 2, x3, y3, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 3
}

fn draw_line(x1, y1, x2, y2, thickness, r, g, b, a){
   "Batches a thick line using a 6-vertex triangle quad."
   if(!_frame_open){ return 0 }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   def c = _pack_color(r, g, b, a)
   __vkr_push_line(_local_vertex_map + _vertex_offset, x1, y1, x2, y2, thickness, c)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

fn draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a){
   "Batches a single colored 3D triangle (zero-alloc)."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 3)){ return }
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base, 0, x1, y1, z1, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 1, x2, y2, z2, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 2, x3, y3, z3, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 3
}

fn draw_quad_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a){
   "Batches a single colored 3D quad (zero-alloc)."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return }
   def c = _pack_color(r, g, b, a)
   def base_idx = _vertex_offset / _VKR_VERT_STRIDE
   _vkr_store_vertex(_local_vertex_map, base_idx + 0, x1, y1, z1, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 1, x2, y2, z2, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 2, x3, y3, z3, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 3, x1, y1, z1, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 4, x3, y3, z3, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 5, x4, y4, z4, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

fn draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, r, g, b, a){
   "Batches a 3D line as a quad (parallel to Y if needed, or billboarded)."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return }
   def dx = float(x2) - float(x1) def dy = float(y2) - float(y1) def dz = float(z2) - float(z1)
   def l = sqrt(dx*dx + dy*dy + dz*dz)
   if(l == 0.0){ return }
   mut nx = -dz / l * (float(thickness) * 0.5)
   mut ny = 0.0
   mut nz =  dx / l * (float(thickness) * 0.5)
   if(abs(dx) < 0.001 && abs(dz) < 0.001){ nx = float(thickness)*0.5 nz = 0.0 }
   def c = _pack_color(r, g, b, a)
   def f1x = float(x1) def f1y = float(y1) def f1z = float(z1)
   def f2x = float(x2) def f2y = float(y2) def f2z = float(z2)
   def base_idx = _vertex_offset / _VKR_VERT_STRIDE
   _vkr_store_vertex(_local_vertex_map, base_idx + 0, f1x+nx, f1y+ny, f1z+nz, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 1, f1x-nx, f1y-ny, f1z-nz, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 2, f2x-nx, f2y-ny, f2z-nz, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 3, f1x+nx, f1y+ny, f1z+nz, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 4, f2x-nx, f2y-ny, f2z-nz, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 5, f2x+nx, f2y+ny, f2z+nz, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
}

fn draw_grid_3d(size, step){
   "Draws an infinite-style 3D grid on the XZ plane."
   def s = float(size)
   mut gx = -s
   while(gx <= s){
      draw_line_3d(gx, 0, -s, gx, 0, s, 0.03, 0.2, 0.2, 0.4, 0.5)
      draw_line_3d(-s, 0, gx, s, 0, gx, 0.03, 0.2, 0.2, 0.4, 0.5)
      gx += float(step)
   }
}

fn draw_axes_3d(size){
   "Draws RGB axis lines with arrowheads."
   def s = float(size)
   draw_line_3d(0,0,0, s,0,0, 0.15, 1,0,0,1)
   draw_line_3d(0,0,0, 0,s,0, 0.15, 0,1,0,1)
   draw_line_3d(0,0,0, 0,0,s, 0.15, 0,0,1,1)

   ;; Basic Arrowheads (Triangles)
   def ts = 0.4
   ;; X
   draw_triangle_3d(s, 0, 0, s-ts, ts, 0, s-ts,-ts, 0, 1, 0, 0, 1)
   ;; Y
   draw_triangle_3d(0, s, 0, ts, s-ts, 0,-ts, s-ts, 0, 0, 1, 0, 1)
   ;; Z
   draw_triangle_3d(0, 0, s, ts, 0, s-ts,-ts, 0, s-ts, 0, 0, 1, 1)
}

fn draw_cube_3d(x, y, z, size, r, g=1.0, b=1.0, a=1.0, tex_id=-1){
   "Batches a colored 3D cube. frontFace=CW, cullMode=BACK (Vulkan Y-down convention)."
   if(!_frame_open){ return }
   def tid = (tex_id < 0) ? _default_texture : tex_id
   bind_texture(tid)
   if(!_check_flush(_VKR_VERT_STRIDE * 36)){ return }
   def s = float(size) * 0.5
   def fx = float(x) def fy = float(y) def fz = float(z)
   def c = is_int(r) ? r : _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset

   ;; Vulkan Y-down: screen Y increases downward.
   ;; frontFace=CW means a face is front if its verts appear CW on screen.
   ;; For each face, we name verts as seen on screen from outside:
   ;;   v0=top-left, v1=top-right, v2=bot-right, v3=bot-left  (screen coords, Y-down)
   ;; Two CW triangles: (v0,v1,v2) and (v0,v2,v3)

   ;; Front (+Z): looking at face from +Z. Screen: right=+X, down=+Y.
   ;;   TL=(-s,-s,+s) TR=(+s,-s,+s) BR=(+s,+s,+s) BL=(-s,+s,+s)
   _vkr_store_vertex(base,  0, fx-s, fy-s, fz+s, 0,0, c, _current_tex_index,  0, 0, 1)
   _vkr_store_vertex(base,  1, fx+s, fy-s, fz+s, 1,0, c, _current_tex_index,  0, 0, 1)
   _vkr_store_vertex(base,  2, fx+s, fy+s, fz+s, 1,1, c, _current_tex_index,  0, 0, 1)
   _vkr_store_vertex(base,  3, fx-s, fy-s, fz+s, 0,0, c, _current_tex_index,  0, 0, 1)
   _vkr_store_vertex(base,  4, fx+s, fy+s, fz+s, 1,1, c, _current_tex_index,  0, 0, 1)
   _vkr_store_vertex(base,  5, fx-s, fy+s, fz+s, 0,1, c, _current_tex_index,  0, 0, 1)

   ;; Back (-Z): looking at face from -Z. Screen: right=-X, down=+Y.
   ;;   TL=(+s,-s,-s) TR=(-s,-s,-s) BR=(-s,+s,-s) BL=(+s,+s,-s)
   _vkr_store_vertex(base,  6, fx+s, fy-s, fz-s, 0,0, c, _current_tex_index,  0, 0,-1)
   _vkr_store_vertex(base,  7, fx-s, fy-s, fz-s, 1,0, c, _current_tex_index,  0, 0,-1)
   _vkr_store_vertex(base,  8, fx-s, fy+s, fz-s, 1,1, c, _current_tex_index,  0, 0,-1)
   _vkr_store_vertex(base,  9, fx+s, fy-s, fz-s, 0,0, c, _current_tex_index,  0, 0,-1)
   _vkr_store_vertex(base, 10, fx-s, fy+s, fz-s, 1,1, c, _current_tex_index,  0, 0,-1)
   _vkr_store_vertex(base, 11, fx+s, fy+s, fz-s, 0,1, c, _current_tex_index,  0, 0,-1)

   ;; Right (+X): looking from +X. Screen: right=-Z, down=+Y.
   ;;   TL=(+s,-s,-s) TR=(+s,-s,+s) BR=(+s,+s,+s) BL=(+s,+s,-s)
   _vkr_store_vertex(base, 12, fx+s, fy-s, fz-s, 0,0, c, _current_tex_index,  1, 0, 0)
   _vkr_store_vertex(base, 13, fx+s, fy-s, fz+s, 1,0, c, _current_tex_index,  1, 0, 0)
   _vkr_store_vertex(base, 14, fx+s, fy+s, fz+s, 1,1, c, _current_tex_index,  1, 0, 0)
   _vkr_store_vertex(base, 15, fx+s, fy-s, fz-s, 0,0, c, _current_tex_index,  1, 0, 0)
   _vkr_store_vertex(base, 16, fx+s, fy+s, fz+s, 1,1, c, _current_tex_index,  1, 0, 0)
   _vkr_store_vertex(base, 17, fx+s, fy+s, fz-s, 0,1, c, _current_tex_index,  1, 0, 0)

   ;; Left (-X): looking from -X. Screen: right=+Z, down=+Y.
   ;;   TL=(-s,-s,+s) TR=(-s,-s,-s) BR=(-s,+s,-s) BL=(-s,+s,+s)
   _vkr_store_vertex(base, 18, fx-s, fy-s, fz+s, 0,0, c, _current_tex_index, -1, 0, 0)
   _vkr_store_vertex(base, 19, fx-s, fy-s, fz-s, 1,0, c, _current_tex_index, -1, 0, 0)
   _vkr_store_vertex(base, 20, fx-s, fy+s, fz-s, 1,1, c, _current_tex_index, -1, 0, 0)
   _vkr_store_vertex(base, 21, fx-s, fy-s, fz+s, 0,0, c, _current_tex_index, -1, 0, 0)
   _vkr_store_vertex(base, 22, fx-s, fy+s, fz-s, 1,1, c, _current_tex_index, -1, 0, 0)
   _vkr_store_vertex(base, 23, fx-s, fy+s, fz+s, 0,1, c, _current_tex_index, -1, 0, 0)

   ;; Top (-Y, world up): looking from -Y (above). Screen: right=+X, down=+Z.
   ;;   TL=(-s,-s,-s) TR=(+s,-s,-s) BR=(+s,-s,+s) BL=(-s,-s,+s)
   _vkr_store_vertex(base, 24, fx-s, fy-s, fz-s, 0,0, c, _current_tex_index,  0,-1, 0)
   _vkr_store_vertex(base, 25, fx+s, fy-s, fz-s, 1,0, c, _current_tex_index,  0,-1, 0)
   _vkr_store_vertex(base, 26, fx+s, fy-s, fz+s, 1,1, c, _current_tex_index,  0,-1, 0)
   _vkr_store_vertex(base, 27, fx-s, fy-s, fz-s, 0,0, c, _current_tex_index,  0,-1, 0)
   _vkr_store_vertex(base, 28, fx+s, fy-s, fz+s, 1,1, c, _current_tex_index,  0,-1, 0)
   _vkr_store_vertex(base, 29, fx-s, fy-s, fz+s, 0,1, c, _current_tex_index,  0,-1, 0)

   ;; Bottom (+Y, world down): looking from +Y (below). Screen: right=+X, down=-Z.
   ;;   TL=(-s,+s,+s) TR=(+s,+s,+s) BR=(+s,+s,-s) BL=(-s,+s,-s)
   _vkr_store_vertex(base, 30, fx-s, fy+s, fz+s, 0,0, c, _current_tex_index,  0, 1, 0)
   _vkr_store_vertex(base, 31, fx+s, fy+s, fz+s, 1,0, c, _current_tex_index,  0, 1, 0)
   _vkr_store_vertex(base, 32, fx+s, fy+s, fz-s, 1,1, c, _current_tex_index,  0, 1, 0)
   _vkr_store_vertex(base, 33, fx-s, fy+s, fz+s, 0,0, c, _current_tex_index,  0, 1, 0)
   _vkr_store_vertex(base, 34, fx+s, fy+s, fz-s, 1,1, c, _current_tex_index,  0, 1, 0)
   _vkr_store_vertex(base, 35, fx-s, fy+s, fz-s, 0,1, c, _current_tex_index,  0, 1, 0)

   _vertex_offset += _VKR_VERT_STRIDE * 36
}

fn draw_line_strip_2d(x, y, w, h, history, scale, r, g, b, a){
   "Batches a UI line strip from a history list."
   if(!_frame_open){ return }
   bind_texture(_default_texture)
   def count = len(history)
   if(count < 2){ return }
   if(!_check_flush((count-1) * (_VKR_VERT_STRIDE * 6))){ return }
   def c = _pack_color(r, g, b, a)
   def dcount = float(count - 1)
   def step = float(w) / dcount
   def fh = float(h) def fx = float(x) def fy = float(y)
   def fs = float(scale)
   mut base_idx = _vertex_offset / _VKR_VERT_STRIDE
   mut i = 0
   while(i < count - 1){
      mut v1 = float(get(history, i, 0)) * fs
      mut v2 = float(get(history, i + 1, 0)) * fs
      if(v1 > 1.0){ v1 = 1.0 } if(v2 > 1.0){ v2 = 1.0 }
      def px1 = fx + float(i) * step
      def py1 = fy + fh * (1.0 - v1)
      def px2 = fx + float(i+1) * step
      def py2 = fy + fh * (1.0 - v2)
      def th = 1.0
      _vkr_store_vertex(_local_vertex_map, base_idx + 0, px1, py1 - th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 1, px1, py1 + th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 2, px2, py2 + th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 3, px1, py1 - th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 4, px2, py2 + th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 5, px2, py2 - th, 0.0, 0.0, 0.0, c, _current_tex_index)
      base_idx += 6
      i += 1
   }
   _vertex_offset = base_idx * _VKR_VERT_STRIDE
}

fn draw_static_buffer(sbuf, is_lines=false, width=1.0){
   "Records a draw command for a static GPU buffer. Must be called inside a frame."
   if(!_frame_open || !is_dict(sbuf)){ return false }
   def buf = dict_get(sbuf, "handle", 0)
   def count = dict_get(sbuf, "count", 0)
   if(!buf || count <= 0){ return false }

   _flush() ; Flush pending dynamic geometry

   def cb = _current_frame_cb

   ; Ensure pipeline is correctly bound for the static mesh
   mut target = _pipeline
   if(is_lines && _line_pipeline != 0){
      target = _line_pipeline
      if(_last_is_mask != 0){ _last_is_mask = 0 _pc_dirty = true }
      if(_last_is_unlit != 1){ _last_is_unlit = 1 _pc_dirty = true }
      cmd_set_line_width(cb, width)
   } else {
      mut base_pipe = _pipeline
      if(_current_is_unlit != 0 && _unlit_pipeline != 0){ base_pipe = _unlit_pipeline }
      target = _target_pipeline
      if(target == _pipeline){ target = base_pipe }
      if(_is_wireframe && _wire_pipeline != 0){
         if(target == _pipeline || target == _unlit_pipeline){ target = _wire_pipeline }
      }
   }

   if(_last_bound_pipe != target){
       cmd_bind_pipeline(cb, 0, target)
       _last_bound_pipe = target
       _pc_dirty = true
   }

   _bind_descriptors(cb)
   _sync_pc()

   store64_raw(_static_vbo_ptr, buf, 0)

   cmd_bind_vertex_buffers(cb, 0, 1, _static_vbo_ptr, _static_off_ptr)
   cmd_draw(cb, count, 1, 0, 0)

   _total_draw_calls += 1
   _frame_draw_calls += 1

   ; Rebind dynamic VBO back
   store64_raw(_static_off_ptr, _current_frame_vertex_offset, 0)
   store64_raw(_static_vbo_ptr, _vertex_buffer, 0)
   cmd_bind_vertex_buffers(cb, 0, 1, _static_vbo_ptr, _static_off_ptr)
   store64_raw(_static_off_ptr, 0, 0)
   true
}

fn draw_circle_sdf(x, y, radius, r, g, b, a){
   "Draws a fast, smooth circle using an SDF fragment shader. Isolated and flushed to prevent state pollution."
   if(!_frame_open || !_circle_pipeline){ return false }
   _flush() ; flush pending regular primitives
   mut prev_target = _target_pipeline
   _target_pipeline = _circle_pipeline
   def c = _pack_color(r, g, b, a)
   def bytes = _VKR_VERT_STRIDE * 6
   if(!_check_flush(bytes)){ return false }
   __vkr_push_rect_sdf(_local_vertex_map + _vertex_offset, x - radius, y - radius, radius * 2.0, radius * 2.0, c, 0, 0, 1.0)
   _vertex_offset += bytes
   _flush() ; execute circle immediately
   _target_pipeline = prev_target ; restore
   true
}

fn draw_ring_sdf(x, y, inner_radius, outer_radius, r, g, b, a){
   "Draws a fast, smooth ring using an SDF fragment shader. Isolated and flushed to prevent state pollution."
   if(!_frame_open || !_ring_pipeline){ return false }
   if(outer_radius <= inner_radius){ return false }
   _flush()
   mut prev_target = _target_pipeline
   _target_pipeline = _ring_pipeline
   def c = _pack_color(r, g, b, a)
   def bytes = _VKR_VERT_STRIDE * 6
   if(!_check_flush(bytes)){ return false }
   def ratio = inner_radius / outer_radius
   __vkr_push_rect_sdf(_local_vertex_map + _vertex_offset, x - outer_radius, y - outer_radius, outer_radius * 2.0, outer_radius * 2.0, c, ratio, 0, 1.0)
   _vertex_offset += bytes
   _flush()
   _target_pipeline = prev_target
   true
}
