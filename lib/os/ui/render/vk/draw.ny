;; Keywords: render vulkan gpu draw
;; Vulkan draw submission for renderer primitives, batches, meshes, grids, and SDF shapes.
module std.os.ui.render.vk.draw(draw_rect_fast, draw_rect_outline_fast, draw_rects_fast_ptr, draw_line_fast, draw_lines_2d_fast_ptr, draw_rect, draw_rect_tex, draw_rect_tex_uv, draw_line, draw_glyph, _draw_triangle_2d, draw_triangle_3d, draw_quad_3d, draw_rect_lines_2d, draw_chamfer_rect_2d, draw_rounded_rect_2d, draw_fan_2d, draw_ellipse_lines_2d, draw_arc_2d, draw_sector_2d, draw_star_2d, draw_vertices, draw_lines_raw, draw_vertices_indexed_raw, draw_line_3d, draw_grid_3d, draw_axes_3d, draw_cube_3d, draw_line_strip_2d, draw_points_raw, draw_static_buffer, draw_static_buffer_raw, draw_static_buffer_indexed, draw_static_buffer_indexed_raw, draw_circle_sdf, draw_ring_sdf, _ensure_default_triangle_pipeline, mesh_build_axes_3d)
use std.core
use std.core.mem
use std.math
use std.os.ui.render.vk.state
use std.os.ui.render.vk.vulkan (
   cmd_bind_descriptor_sets, cmd_push_constants, cmd_bind_vertex_buffers, cmd_bind_index_buffer,
   cmd_set_line_width, cmd_bind_pipeline, cmd_draw, cmd_draw_indexed,
)

use std.os.ui.profile as ui_profile
use std.os.ui.render.vk.utils
use std.os.ui.render.vk.pipeline (_ensure_circle_pipeline,
   _ensure_ring_pipeline,
   _ensure_line_pipeline,
   _ensure_point_pipeline,
   _get_nocull_pipeline,

_get_unlit_nocull_pipeline)

use std.os.ui.render.vk.renderer (_check_flush, _flush, _sync_pc, set_ui_material)
use std.os.ui.render.vk.texture (bind_texture, bind_default_texture, bindless_sync_texture_slot)

mut _ui_rect_trace_hits = 0

fn color_pack(any: r, any: g, any: b, any: a=1.0): int { _pack_color(r, g, b, a) }

@inline
fn _draw_light_trace_bind_enabled(): bool {
   ui_profile.env_lower_cached("NY_UI_LIGHT_TRACE") == "bind"
}

@inline
fn _draw_gui_trace_enabled(): bool {
   ui_profile.env_enabled_cached("NY_UI_GUI_TRACE")
}

fn _bind_descriptors(any: cb): any {
   def ubo_ds = _current_frame_ubo_ds
   def ds_count = 2
   if(_draw_light_trace_bind_enabled()){ ui_profile.print_text("[vk:bind] bindless_ds=" + to_str(_bindless_ds) + " ubo_ds=" + to_str(ubo_ds) + " frame=" + to_str(_current_frame)) }
   if(_bindless_ds && (_bindless_ds != _last_bound_ds || ubo_ds != _last_bound_ubo_ds)){
      store64_h(_ptr_ds, _bindless_ds, 0)
      store64_h(_ptr_ds, ubo_ds, 8)
      cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, ds_count, _ptr_ds, 0, 0)
      _last_bound_ds = _bindless_ds
      _last_bound_ubo_ds = ubo_ds
      _descriptor_bind_count += 1
   }
   0
}

fn _ensure_static_bind_scratch(): bool {
   if(_static_vbo_ptr && _static_off_ptr){ return _static_vbo_ptr != 0 && _static_off_ptr != 0 }
   def slab = malloc(16)
   if(!slab){ return false }
   memset(slab, 0, 16)
   _static_vbo_ptr = slab
   _static_off_ptr = slab + 8
   true
}

fn _bind_static_vertex_buffer(any: cb, any: buf, any: voff): int {
   if(!_ensure_static_bind_scratch()){ return -1 }
   def can_base_vertex = (voff % _VKR_VERT_STRIDE) == 0
   def vbo_bind_off = can_base_vertex ? 0 : voff
   def first_vertex = can_base_vertex ? int(voff / _VKR_VERT_STRIDE) : 0
   if(_dynamic_vbo_bound || buf != _last_static_vbo || vbo_bind_off != _last_static_vbo_off){
      store64_h(_static_vbo_ptr, buf, 0)
      store64_h(_static_off_ptr, vbo_bind_off, 0)
      cmd_bind_vertex_buffers(cb, 0, 1, _static_vbo_ptr, _static_off_ptr)
      _last_static_vbo = buf
      _last_static_vbo_off = vbo_bind_off
      _dynamic_vbo_bound = false
   }
   first_vertex
}

fn _bind_static_index_buffer(any: cb, any: idx_buf, any: ioff, int: index_type): int {
   def idx_size = (index_type == 1) ? 4 : 2
   def can_base_index = (ioff % idx_size) == 0
   def ibo_bind_off = can_base_index ? 0 : ioff
   def first_index = can_base_index ? int(ioff / idx_size) : 0
   if(idx_buf != _last_static_ibuf || ibo_bind_off != _last_static_ibuf_off || index_type != _last_static_ibuf_type){
      cmd_bind_index_buffer(cb, idx_buf, ibo_bind_off, index_type)
      _last_static_ibuf = idx_buf
      _last_static_ibuf_off = ibo_bind_off
      _last_static_ibuf_type = index_type
   }
   first_index
}

@inline
fn _set_unlit_true_fast(): any {
   if(_current_is_unlit != 1){
      if(_vertex_offset != _last_flush_offset){
         _flush_reason = 2
         _flush()
      }
      _current_is_unlit = 1
      _pc_dirty = true
   }
   0
}

@inline
fn _set_mask_zero_fast(): any {
   if(_last_is_mask != 0){
      if(_vertex_offset != _last_flush_offset){
         _flush_reason = 2
         _flush()
      }
      _last_is_mask = 0
      _pc_dirty = true
   }
   0
}

@inline
fn _ensure_ui_vertex_material_mode_fast(int: vc_mode): any {
   if(_current_base_color_u32 == 0xffffffff && _current_base_tex_id == -1 && _current_vc_mode == vc_mode){ return 0 }
   set_ui_material(-1, 0, vc_mode)
   0
}

@inline
fn _ensure_ui_vertex_material_fast(): any {
   _ensure_ui_vertex_material_mode_fast(12)
}

@inline
fn _ensure_ui_vertex_color_material_fast(): any {
   _ensure_ui_vertex_material_mode_fast(1)
   0
}

@inline
fn _bind_texture_slot_fast(any: tid): any {
   if(_bindless_ds && tid >= 0){
      if(_current_tex_index != tid){
         bindless_sync_texture_slot(tid)
         _current_texture_id = tid
         _current_tex_index = tid
         if(_vertex_offset == _last_flush_offset){
            _batch_texture_id = tid
            _batch_tex_index = tid
         }
      }
      return tid
   }
   bind_texture(tid)
   tid
}

@inline
fn _bind_vertex_texture_fast(any: tex_id): any {
   _set_unlit_true_fast()
   _ensure_ui_vertex_material_fast()
   mut tid = tex_id
   if(!is_int(tid) || tid < 0 || tid >= _textures.len){ tid = _default_texture }
   _bind_texture_slot_fast(tid)
}

@inline
fn _bind_default_texture_fast(): any {
   _set_unlit_true_fast()
   _ensure_ui_vertex_color_material_fast()
   def tid = _default_texture
   if(tid < 0){ return _bind_vertex_texture_fast(tid) }
   _bind_texture_slot_fast(tid)
}

@inline
fn _begin_ui_triangle_stream(any: tex_id): bool {
   if(!_frame_open){ return false }
   _bind_vertex_texture_fast(tex_id)
   _set_mask_zero_fast()
   _use_default_triangle_pipeline()
   true
}

@inline
fn _begin_ui_default_triangle_stream(): bool {
   if(!_frame_open){ return false }
   _bind_default_texture_fast()
   _set_mask_zero_fast()
   _use_default_triangle_pipeline()
   true
}

@inline
fn _begin_default_stream_bytes(int: bytes): bool {
   if(!_begin_ui_default_triangle_stream()){ return false }
   _check_flush(bytes)
}

@inline
fn _begin_default_stream_vertices(int: verts): bool{ _begin_default_stream_bytes(_VKR_VERT_STRIDE * verts) }

@inline
fn _bind_default_stream_vertices(int: verts): bool {
   if(!_frame_open){ return false }
   _bind_default_texture_fast()
   _check_flush(_VKR_VERT_STRIDE * verts)
}

@inline
fn _write_ui_rect_vertex(any: v, f64: x, f64: y, f64: u, f64: uv, int: color_u32, int: tex_id): any {
   store32_f32(v, x, _VKR_OFF_X)
   store32_f32(v, y, _VKR_OFF_Y)
   store32_f32(v, 0.0, _VKR_OFF_Z)
   store32_f32(v, u, _VKR_OFF_U)
   store32_f32(v, uv, _VKR_OFF_V)
   store32(v, color_u32, _VKR_OFF_C)
   store32_f32(v, 0.0, _VKR_OFF_NX)
   store32_f32(v, 0.0, _VKR_OFF_NY)
   store32_f32(v, 1.0, _VKR_OFF_NZ)
   store32_f32(v, 1.0, _VKR_OFF_TX)
   store32_f32(v, 0.0, _VKR_OFF_TY)
   store32_f32(v, 0.0, _VKR_OFF_TZ)
   store32_f32(v, 1.0, _VKR_OFF_TW)
   store32_f32(v, 0.0, _VKR_OFF_U2)
   store32_f32(v, 0.0, _VKR_OFF_V2)
   store32(v, tex_id, _VKR_OFF_TEX)
}

fn _push_rect_tex_direct(any: p, f64: x, f64: y, f64: w, f64: h, f64: u1, f64: v1, f64: u2, f64: v2, int: color_u32, int: tex_id): any {
   if(!p){ return 0 }
   def x2 = x + w
   def y2 = y + h
   _write_ui_rect_vertex(p + 0 * _VKR_VERT_STRIDE, x, y, u1, v1, color_u32, tex_id)
   _write_ui_rect_vertex(p + 1 * _VKR_VERT_STRIDE, x, y2, u1, v2, color_u32, tex_id)
   _write_ui_rect_vertex(p + 2 * _VKR_VERT_STRIDE, x2, y2, u2, v2, color_u32, tex_id)
   _write_ui_rect_vertex(p + 3 * _VKR_VERT_STRIDE, x2, y2, u2, v2, color_u32, tex_id)
   _write_ui_rect_vertex(p + 4 * _VKR_VERT_STRIDE, x2, y, u2, v1, color_u32, tex_id)
   _write_ui_rect_vertex(p + 5 * _VKR_VERT_STRIDE, x, y, u1, v1, color_u32, tex_id)
   p
}

fn _select_static_draw_pipeline(any: cb, bool: is_lines, f64: width, any: pipe_override, bool: is_points=false): any {
   mut target = _pipeline
   if(is_points && !_point_pipeline){ _ = _ensure_point_pipeline() }
   if(is_lines && !_line_pipeline){ _ = _ensure_line_pipeline() }
   if(is_points && _point_pipeline != 0){
      target = _point_pipeline
      if(_last_is_mask != 0){ _last_is_mask = 0 _pc_dirty = true }
   } elif(is_lines && _line_pipeline != 0){
      target = _line_pipeline
      if(_last_is_mask != 0){ _last_is_mask = 0 _pc_dirty = true }
      if(width != _last_line_width){
         cmd_set_line_width(cb, width)
         _last_line_width = width
      }
   } else {
      if(pipe_override){ target = pipe_override } else {
         mut base_pipe = _pipeline
         if(_current_is_unlit != 0){
            def up = _get_unlit_nocull_pipeline()
            if(up != 0){ base_pipe = up }
         }
         target = _target_pipeline
         if(!target
            || target == _pipeline
            || target == _circle_pipeline
            || target == _ring_pipeline
            || target == _skybox_pipeline
            || target == _flip_pipeline
            || target == _flip_unlit_pipeline
            || target == _line_pipeline
            || target == _point_pipeline){
            target = base_pipe
         }
         if(_is_wireframe && _wire_pipeline != 0){
            if(target == _pipeline
               || target == _unlit_pipeline
               || target == _nocull_pipeline
               || target == _unlit_nocull_pipeline
               || target == base_pipe){
               target = _wire_pipeline
            }
         }
      }
   }
   _vkr_bind_pipeline_if_needed(cb, target)
   target
}

@inline
fn _activate_pipeline_target(any: target): any {
   if(_target_pipeline != target){
      _flush_reason = 4
      _flush()
      _target_pipeline = target
      _use_custom_pc = 0
      _pc_dirty = true
      return 0
   }
   if(_use_custom_pc != 0){
      _use_custom_pc = 0
      _pc_dirty = true
   }
   0
}

@inline
fn _prepare_sdf_draw(any: pipe): bool {
   if(!_frame_open || !pipe){ return false }
   _activate_pipeline_target(pipe)
   _check_flush(_VKR_VERT_STRIDE * 6)
}

@inline
fn _use_default_triangle_pipeline(): any {
   mut target = _pipeline
   if(_current_is_unlit != 0){
      if(_unlit_nocull_pipeline != 0){ target = _unlit_nocull_pipeline }
      elif(_unlit_pipeline != 0){ target = _unlit_pipeline }
      else {
         def up = _get_unlit_nocull_pipeline()
         if(up != 0){ target = up }
      }
   }
   _activate_pipeline_target(target)
}

fn _ensure_default_triangle_pipeline(): any {
   if(!_frame_open){ return 0 }
   _use_default_triangle_pipeline()
}

fn _begin_static_draw(bool: is_lines, f64: width, any: pipe_override, bool: is_points): any {
   if(_vertex_offset != _last_flush_offset){
      _flush_reason = 3
      _flush()
   }
   def cb = _current_frame_cb
   _select_static_draw_pipeline(cb, is_lines, width, pipe_override, is_points)
   _bind_descriptors(cb)
   _sync_pc()
   cb
}

fn _draw_raw_stream_current_material(any: p, int: vertex_count, any: pipe): bool {
   if(_vertex_offset != _last_flush_offset){
      _flush_reason = 4
      _flush()
   }
   def bytes = vertex_count * _VKR_VERT_STRIDE
   if(!_check_flush(bytes)){ return false }
   def cb = _current_frame_cb
   if(_last_bound_pipe != pipe){
      cmd_bind_pipeline(cb, 0, pipe)
      _last_bound_pipe = pipe
      _pc_dirty = true
   }
   _bind_descriptors(cb)
   _sync_pc()
   def first_vert = _vertex_offset / _VKR_VERT_STRIDE
   __copy_mem(_local_vertex_map + _vertex_offset, p, bytes)
   _vkr_bind_dynamic_vertex_buffer(cb)
   cmd_draw(cb, vertex_count, 1, first_vert, 0)
   _vertex_offset += bytes
   _last_flush_offset = _vertex_offset
   true
}

fn draw_rect_fast(f64: x, f64: y, f64: w, f64: h, int: color_u32): any {
   "Submits a rectangle using pre-packed color and fixed vertex layout."
   if(!_frame_open){
      def gui_trace = _draw_gui_trace_enabled()
      if(gui_trace && _ui_rect_trace_hits < 6){
         ui_profile.print_text("[vk:gui-rect] drop reason=frame_closed")
         _ui_rect_trace_hits += 1
      }
      return 0
   }
   if(!_begin_ui_default_triangle_stream()){ return 0 }
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){
      def gui_trace = _draw_gui_trace_enabled()
      if(gui_trace && _ui_rect_trace_hits < 6){
         ui_profile.print_text("[vk:gui-rect] drop reason=flush_full")
         _ui_rect_trace_hits += 1
      }
      return 0
   }
   def gui_trace = _draw_gui_trace_enabled()
   if(gui_trace && _ui_rect_trace_hits < 6){
      ui_profile.print_text("[vk:gui-rect] queued x=" + to_str(x) + " y=" + to_str(y) + " w=" + to_str(w) + " h=" + to_str(h))
      _ui_rect_trace_hits += 1
   }
   def write_base = _local_vertex_map + _vertex_offset
   _push_rect_tex_direct(write_base, x, y, w, h, 0.0, 0.0, 0.0, 0.0, color_u32, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
   _prim_rect_quads += 1
   0
}

fn draw_rect_outline_fast(f64: x, f64: y, f64: w, f64: h, int: color_u32): any {
   "Batches a 1px rectangle outline with one state setup instead of four rect calls."
   if(w <= 0.0 || h <= 0.0){ return 0 }
   def need = _VKR_VERT_STRIDE * 24
   if(!_begin_default_stream_bytes(need)){ return 0 }
   def base = _local_vertex_map + _vertex_offset
   _push_rect_tex_direct(base + 0 * _VKR_VERT_STRIDE * 6, x, y, w, 1.0, 0.0, 0.0, 0.0, 0.0, color_u32, _current_tex_index)
   _push_rect_tex_direct(base + 1 * _VKR_VERT_STRIDE * 6, x, y + h - 1.0, w, 1.0, 0.0, 0.0, 0.0, 0.0, color_u32, _current_tex_index)
   _push_rect_tex_direct(base + 2 * _VKR_VERT_STRIDE * 6, x, y, 1.0, h, 0.0, 0.0, 0.0, 0.0, color_u32, _current_tex_index)
   _push_rect_tex_direct(base + 3 * _VKR_VERT_STRIDE * 6, x + w - 1.0, y, 1.0, h, 0.0, 0.0, 0.0, 0.0, color_u32, _current_tex_index)
   _vertex_offset += need
   _prim_outline_quads += 4
   0
}

fn draw_rects_fast_ptr(any: rects, int: count, int: stride=20): int {
   "Batches packed 2D rect records: f32 x,y,w,h + u32 color."
   if(count <= 0 || !rects){ return 0 }
   if(stride < 20){ stride = 20 }
   if(!_begin_ui_default_triangle_stream()){ return 0 }
   def max_batch = max(1, int(_vertex_capacity / (_VKR_VERT_STRIDE * 6)))
   mut done = 0
   while(done < count){
      def batch = min(max_batch, count - done)
      def bytes = batch * _VKR_VERT_STRIDE * 6
      if(!_check_flush(bytes)){ return done }
      def base = _local_vertex_map + _vertex_offset
      mut j = 0
      while(j < batch){
         def rec = rects + (done + j) * stride
         def dst = base + j * _VKR_VERT_STRIDE * 6
         _push_rect_tex_direct(
            dst,
            load32_f32(rec, 0), load32_f32(rec, 4),
            load32_f32(rec, 8), load32_f32(rec, 12),
            0.0, 0.0, 0.0, 0.0,
            load32(rec, 16), _current_tex_index
         )
         j += 1
      }
      _vertex_offset += bytes
      _prim_rect_quads += batch
      done += batch
   }
   done
}

@jit
fn draw_lines_2d_fast_ptr(any: lines, int: count, int: stride=24): int {
   "Batches packed 2D line records: f32 x1,y1,x2,y2,thickness + u32 color."
   if(count <= 0 || !lines){ return 0 }
   if(stride < 24){ stride = 24 }
   if(!_begin_ui_default_triangle_stream()){ return 0 }
   def max_batch = max(1, int(_vertex_capacity / (_VKR_VERT_STRIDE * 6)))
   mut done = 0
   while(done < count){
      def batch = min(max_batch, count - done)
      def bytes = batch * _VKR_VERT_STRIDE * 6
      if(!_check_flush(bytes)){ return done }
      def base = _local_vertex_map + _vertex_offset
      mut j = 0
      while(j < batch){
         def rec = lines + (done + j) * stride
         __vkr_push_line(
            base + j * _VKR_VERT_STRIDE * 6,
            load32_f32(rec, 0), load32_f32(rec, 4),
            load32_f32(rec, 8), load32_f32(rec, 12),
            load32_f32(rec, 16), load32(rec, 20)
         )
         j += 1
      }
      _vertex_offset += bytes
      _prim_line_quads += batch
      done += batch
   }
   done
}

@jit
fn draw_rect(f64: x, f64: y, f64: w, f64: h, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a colored rectangle(6-vertex CW triangle list) — optimized path."
   draw_rect_fast(x, y, w, h, _pack_color(r, g, b, a))
}

fn _draw_textured_rect_packed(f64: x, f64: y, f64: w, f64: h, int: tex_id, f64: u1, f64: v1, f64: u2, f64: v2, int: c): any {
   if(!_begin_ui_triangle_stream(tex_id)){ return 0 }
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   _push_rect_tex_direct(_local_vertex_map + _vertex_offset, x, y, w, h, u1, v1, u2, v2, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
   _prim_rect_quads += 1
   0
}

fn draw_rect_tex(f64: x, f64: y, f64: w, f64: h, int: tex_id, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a textured rectangle as a 6-vertex triangle list."
   _draw_textured_rect_packed(x, y, w, h, tex_id, 0.0, 0.0, 1.0, 1.0, _pack_color(r, g, b, a))
}

fn draw_glyph(f64: x,
   f64: y,
   f64: w,
   f64: h,
   f64: u1,
   f64: v1,
   f64: u2,
   f64: v2,
   int: tex_id,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Submits a glyph quad for text rendering."
   if(!_begin_ui_triangle_stream(tex_id)){ return 0 }
   if(!_check_flush(_VKR_VERT_STRIDE * 6)){ return 0 }
   _push_rect_tex_direct(_local_vertex_map + _vertex_offset, x, y, w, h, u1, v1, u2, v2, _pack_color(r, g, b, a), _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
   _prim_rect_quads += 1
   0
}

fn draw_rect_tex_uv(f64: x,
   f64: y,
   f64: w,
   f64: h,
   int: tex_id,
   f64: u1,
   f64: v1,
   f64: u2,
   f64: v2,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches a textured rectangle with explicit UV coordinates."
   _draw_textured_rect_packed(x, y, w, h, tex_id, u1, v1, u2, v2, _pack_color(r, g, b, a))
}

fn draw_vertices(any: p, int: count, int: tex_id): bool {
   "Bulk-uploads raw vertex data(packed vertex stride) to the local mapping."
   if(!_frame_open || count <= 0 || !p){ return false }
   bind_texture(tex_id)
   def bytes = count * _VKR_VERT_STRIDE
   if(!_check_flush(bytes)){ return false }
   __copy_mem(_local_vertex_map + _vertex_offset, p, bytes)
   _vertex_offset += bytes
   true
}

fn draw_vertices_indexed_raw(any: p,
   int: count,
   any: idx_buf,
   int: ioff,
   int: index_count,
   int: index_type=0,
   int: tex_id=-1,
   bool: is_lines=false,
   f64: width=1.0,
   any: pipe_override=0,
   bool: is_points=false): bool {
   "Streams dynamic vertices but draws them with an existing GPU index buffer."
   if(!_frame_open || count <= 0 || !p || !idx_buf || index_count <= 0){ return false }
   if(_vertex_offset != _last_flush_offset){
      _flush_reason = 3
      _flush()
   }
   def bytes = count * _VKR_VERT_STRIDE
   if(!_check_flush(bytes)){ return false }
   if(tex_id >= 0){ bind_texture(tex_id) }
   def cb = _current_frame_cb
   _select_static_draw_pipeline(cb, is_lines, width, pipe_override, is_points)
   _bind_descriptors(cb)
   _sync_pc()
   def first_vert = _vertex_offset / _VKR_VERT_STRIDE
   __copy_mem(_local_vertex_map + _vertex_offset, p, bytes)
   _vkr_bind_dynamic_vertex_buffer(cb)
   if(idx_buf != _last_static_ibuf || ioff != _last_static_ibuf_off || index_type != _last_static_ibuf_type){
      cmd_bind_index_buffer(cb, idx_buf, ioff, index_type)
      _last_static_ibuf = idx_buf
      _last_static_ibuf_off = ioff
      _last_static_ibuf_type = index_type
   }
   cmd_draw_indexed(cb, index_count, 1, 0, first_vert, 0)
   _vertex_offset += bytes
   _last_flush_offset = _vertex_offset
   _total_draw_calls += 1
   _frame_draw_calls += 1
   _frame_dynamic_draw_calls += 1
   _frame_indexed_draw_calls += 1
   true
}

fn draw_lines_raw(any: p, int: line_count, f64: _line_width): bool {
   "Draws lines using pre-baked raw vertex buffer. Immediate line draw path."
   if(!_line_pipeline){ _ = _ensure_line_pipeline() }
   if(!_frame_open || line_count <= 0 || !p || !_line_pipeline){ return false }
   if(!_draw_raw_stream_current_material(p, line_count * 2, _line_pipeline)){ return false }
   _prim_raw_lines += line_count
   true
}

fn draw_points_raw(any: p, int: point_count, int: tex_id=-1): bool {
   "Draws points using pre-baked raw vertex data. Immediate point-list path."
   if(!_point_pipeline){ _ = _ensure_point_pipeline() }
   if(!_frame_open || point_count <= 0 || !p || !_point_pipeline){ return false }
   if(tex_id >= 0){ bind_texture(tex_id) } else { bind_default_texture() }
   if(!_draw_raw_stream_current_material(p, point_count, _point_pipeline)){ return false }
   _prim_raw_points += point_count
   true
}

fn _draw_triangle_2d(f64: x1, f64: y1, f64: x2, f64: y2, f64: x3, f64: y3, f64: r, f64: g, f64: b, f64: a): any {
   if(!_begin_default_stream_vertices(3)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base, 0, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 1, x2, y2, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 2, x3, y3, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 3
   0
}

@jit
fn draw_line_fast(f64: x1, f64: y1, f64: x2, f64: y2, f64: thickness, int: color_u32): any {
   "Batches a thick packed-color line using a 6-vertex triangle quad."
   if(!_begin_default_stream_vertices(6)){ return 0 }
   __vkr_push_line(_local_vertex_map + _vertex_offset, x1, y1, x2, y2, thickness, color_u32)
   _vertex_offset += _VKR_VERT_STRIDE * 6
   _prim_line_quads += 1
   0
}

fn draw_line(f64: x1, f64: y1, f64: x2, f64: y2, f64: thickness, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a thick line using a 6-vertex triangle quad."
   draw_line_fast(x1, y1, x2, y2, thickness, _pack_color(r, g, b, a))
}

fn draw_triangle_3d(f64: x1,
   f64: y1,
   f64: z1,
   f64: x2,
   f64: y2,
   f64: z2,
   f64: x3,
   f64: y3,
   f64: z3,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches a single colored 3D triangle(zero-alloc)."
   if(!_bind_default_stream_vertices(3)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base, 0, x1, y1, z1, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 1, x2, y2, z2, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, 2, x3, y3, z3, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 3
   0
}

fn draw_quad_3d(f64: x1,
   f64: y1,
   f64: z1,
   f64: x2,
   f64: y2,
   f64: z2,
   f64: x3,
   f64: y3,
   f64: z3,
   f64: x4,
   f64: y4,
   f64: z4,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches a single colored 3D quad(zero-alloc)."
   if(!_bind_default_stream_vertices(6)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def base_idx = _vertex_offset / _VKR_VERT_STRIDE
   _vkr_store_vertex(_local_vertex_map, base_idx + 0, x1, y1, z1, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 1, x2, y2, z2, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 2, x3, y3, z3, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 3, x1, y1, z1, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 4, x3, y3, z3, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(_local_vertex_map, base_idx + 5, x4, y4, z4, 0.0, 0.0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 6
   0
}

fn draw_rect_lines_2d(f64: x, f64: y, f64: w, f64: h, f64: thickness, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a 2D rectangle outline as four thick line quads."
   if(w <= 0.0 || h <= 0.0){ return 0 }
   if(!_bind_default_stream_vertices(24)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def x0 = float(x)
   def y0 = float(y)
   def x1 = x0 + float(w)
   def y1 = y0 + float(h)
   def base = _local_vertex_map + _vertex_offset
   __vkr_push_line(base, x0, y0, x1, y0, thickness, c)
   __vkr_push_line(base + _VKR_VERT_STRIDE * 6, x1, y0, x1, y1, thickness, c)
   __vkr_push_line(base + _VKR_VERT_STRIDE * 12, x1, y1, x0, y1, thickness, c)
   __vkr_push_line(base + _VKR_VERT_STRIDE * 18, x0, y1, x0, y0, thickness, c)
   _vertex_offset += _VKR_VERT_STRIDE * 24
   _prim_line_quads += 4
   0
}

fn draw_chamfer_rect_2d(f64: x, f64: y, f64: w, f64: h, f64: rad, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a 45-degree chamfered rectangle as one convex fan."
   if(w <= 0.0 || h <= 0.0){ return 0 }
   if(rad <= 0.0){ draw_rect(x, y, w, h, r, g, b, a) return 0 }
   if(!_bind_default_stream_vertices(24)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def x0 = float(x)
   def y0 = float(y)
   def x1 = x0 + float(w)
   def y1 = y0 + float(h)
   def rr = float(rad)
   def cx = x0 + float(w) * 0.5
   def cy = y0 + float(h) * 0.5
   def p0x = x0 + rr def p0y = y0
   def p1x = x1 - rr def p1y = y0
   def p2x = x1 def p2y = y0 + rr
   def p3x = x1 def p3y = y1 - rr
   def p4x = x1 - rr def p4y = y1
   def p5x = x0 + rr def p5y = y1
   def p6x = x0 def p6y = y1 - rr
   def p7x = x0 def p7y = y0 + rr
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_fan_step(base,  0, cx, cy, p0x, p0y, p1x, p1y, c)
   _vkr_store_fan_step(base,  3, cx, cy, p1x, p1y, p2x, p2y, c)
   _vkr_store_fan_step(base,  6, cx, cy, p2x, p2y, p3x, p3y, c)
   _vkr_store_fan_step(base,  9, cx, cy, p3x, p3y, p4x, p4y, c)
   _vkr_store_fan_step(base, 12, cx, cy, p4x, p4y, p5x, p5y, c)
   _vkr_store_fan_step(base, 15, cx, cy, p5x, p5y, p6x, p6y, c)
   _vkr_store_fan_step(base, 18, cx, cy, p6x, p6y, p7x, p7y, c)
   _vkr_store_fan_step(base, 21, cx, cy, p7x, p7y, p0x, p0y, c)
   _vertex_offset += _VKR_VERT_STRIDE * 24
   0
}

@inline
@jit
fn _vkr_store_fan_step(any: base, int: vi, f64: cx, f64: cy, f64: x0, f64: y0, f64: x1, f64: y1, int: c): any {
   _vkr_store_vertex(base, vi + 0, cx, cy, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, vi + 1, x0, y0, 0.0, 0.0, 0.0, c, _current_tex_index)
   _vkr_store_vertex(base, vi + 2, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index)
   0
}

fn draw_rounded_rect_2d(f64: x, f64: y, f64: w, f64: h, f64: radius, int: segments, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a filled rounded rectangle with four fan corners."
   if(!_frame_open){ return 0 }
   if(w <= 0.0 || h <= 0.0){ return 0 }
   if(radius <= 0.0){ draw_rect(x, y, w, h, r, g, b, a) return 0 }
   segments = max(2, int(segments))
   def cs_count = max(2, int(segments / 4))
   def vert_count = 18 + cs_count * 12
   _bind_default_texture_fast()
   if(!_check_flush(_VKR_VERT_STRIDE * vert_count)){ return 0 }
   def c = _pack_color(r, g, b, a)
   def fx = float(x)
   def fy = float(y)
   def fw = float(w)
   def fh = float(h)
   def rr = float(radius)
   def base = _local_vertex_map + _vertex_offset
   _push_rect_tex_direct(base + 0 * _VKR_VERT_STRIDE,
      fx + rr,
      fy,
      fw - rr * 2.0,
      fh,
      0.0,
      0.0,
      0.0,
      0.0,
      c,
   _current_tex_index)
   _push_rect_tex_direct(base + 6 * _VKR_VERT_STRIDE,
      fx,
      fy + rr,
      rr,
      fh - rr * 2.0,
      0.0,
      0.0,
      0.0,
      0.0,
      c,
   _current_tex_index)
   _push_rect_tex_direct(base + 12 * _VKR_VERT_STRIDE,
      fx + fw - rr,
      fy + rr,
      rr,
      fh - rr * 2.0,
      0.0,
      0.0,
      0.0,
      0.0,
      c,
   _current_tex_index)
   def step = (PI * 0.5) / float(cs_count)
   def cs = cos(step)
   def sn = sin(step)
   mut vi = 18
   mut cx, cy = fx + rr, fy + rr
   mut ca0, sa0 = -1.0, 0.0
   mut i = 0
   while(i < cs_count){
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _vkr_store_fan_step(base, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   cx, cy, ca0, sa0, i = fx + fw - rr, fy + rr, 0.0, -1.0, 0
   while(i < cs_count){
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _vkr_store_fan_step(base, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   cx, cy, ca0, sa0, i = fx + fw - rr, fy + fh - rr, 1.0, 0.0, 0
   while(i < cs_count){
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _vkr_store_fan_step(base, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   cx, cy, ca0, sa0, i = fx + rr, fy + fh - rr, 0.0, 1.0, 0
   while(i < cs_count){
      def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
      _vkr_store_fan_step(base, vi, cx, cy, cx + ca0 * rr, cy + sa0 * rr, cx + ca1 * rr, cy + sa1 * rr, c)
      ca0, sa0 = ca1, sa1 vi += 3 i += 1
   }
   _vertex_offset += _VKR_VERT_STRIDE * vert_count
   0
}

@jit
fn draw_fan_2d(f64: cx,
   f64: cy,
   f64: rx,
   f64: ry,
   int: segments,
   f64: start_rad,
   f64: span_rad,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches a filled 2D triangle fan as raw vertices."
   if(!_frame_open){ return 0 }
   segments = int(segments)
   if(segments < 1 || rx <= 0.0 || ry <= 0.0 || span_rad == 0.0){ return 0 }
   _bind_default_texture_fast()
   def c = _pack_color(r, g, b, a)
   def max_batch = max(1, int(_vertex_capacity / (_VKR_VERT_STRIDE * 3)))
   def step = float(span_rad) / float(segments)
   def cs = cos(step)
   def sn = sin(step)
   mut ca0, sa0 = cos(start_rad), sin(start_rad)
   mut done = 0
   while(done < segments){
      def batch = min(max_batch, segments - done)
      def bytes = _VKR_VERT_STRIDE * 3 * batch
      if(!_check_flush(bytes)){ return 0 }
      def base = _local_vertex_map + _vertex_offset
      mut vi = 0
      mut j = 0
      while(j < batch){
         def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
         _vkr_store_vertex(base, vi + 0, cx, cy, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 1, cx + ca0 * rx, cy + sa0 * ry, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 2, cx + ca1 * rx, cy + sa1 * ry, 0.0, 0.0, 0.0, c, _current_tex_index)
         ca0, sa0 = ca1, sa1
         vi += 3
         j += 1
      }
      _vertex_offset += bytes
      done += batch
   }
   0
}

@jit
fn _draw_curve_lines_rot(f64: cx,
   f64: cy,
   f64: rx,
   f64: ry,
   f64: start_rad,
   f64: span_rad,
   int: steps,
   f64: thickness,
   int: c): any {
   def max_batch = max(1, int(_vertex_capacity / (_VKR_VERT_STRIDE * 6)))
   def step = float(span_rad) / float(steps)
   def cs = cos(step)
   def sn = sin(step)
   mut ca0, sa0 = cos(start_rad), sin(start_rad)
   mut px, py = cx + ca0 * rx, cy + sa0 * ry
   mut done = 0
   while(done < steps){
      def batch = min(max_batch, steps - done)
      def bytes = _VKR_VERT_STRIDE * 6 * batch
      if(!_check_flush(bytes)){ return 0 }
      def base = _local_vertex_map + _vertex_offset
      mut vi = 0
      mut j = 0
      while(j < batch){
         def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
         def nx, ny = cx + ca1 * rx, cy + sa1 * ry
         __vkr_push_line(base + vi * _VKR_VERT_STRIDE, px, py, nx, ny, thickness, c)
         ca0, sa0 = ca1, sa1
         px, py = nx, ny
         vi += 6
         j += 1
      }
      _vertex_offset += bytes
      done += batch
   }
   0
}

@jit
fn draw_ellipse_lines_2d(f64: cx,
   f64: cy,
   f64: rx,
   f64: ry,
   f64: thickness,
   int: segments,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches an outlined ellipse as thick 2D line quads."
   if(!_frame_open){ return 0 }
   segments = int(segments)
   if(segments < 2 || rx <= 0.0 || ry <= 0.0){ return 0 }
   _bind_default_texture_fast()
   _draw_curve_lines_rot(cx, cy, rx, ry, 0.0, TAU, segments, thickness, _pack_color(r, g, b, a))
}

@jit
fn draw_arc_2d(f64: cx,
   f64: cy,
   f64: radius,
   f64: start_rad,
   f64: span_rad,
   f64: thickness,
   int: steps,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches an arc as thick 2D line quads."
   if(!_frame_open){ return 0 }
   steps = int(steps)
   if(steps < 1 || radius <= 0.0 || span_rad == 0.0){ return 0 }
   _bind_default_texture_fast()
   _draw_curve_lines_rot(cx, cy, radius, radius, start_rad, span_rad, steps, thickness, _pack_color(r, g, b, a))
}

@jit
fn draw_sector_2d(f64: cx,
   f64: cy,
   f64: inner_radius,
   f64: outer_radius,
   f64: start_rad,
   f64: span_rad,
   int: steps,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches a filled ring sector as raw vertices."
   if(!_frame_open){ return 0 }
   steps = int(steps)
   if(steps < 1 || outer_radius <= inner_radius || span_rad == 0.0){ return 0 }
   _bind_default_texture_fast()
   def c = _pack_color(r, g, b, a)
   def max_batch = max(1, int(_vertex_capacity / (_VKR_VERT_STRIDE * 6)))
   def step = float(span_rad) / float(steps)
   def cs = cos(step)
   def sn = sin(step)
   mut ca0, sa0 = cos(start_rad), sin(start_rad)
   mut ix0, iy0 = cx + ca0 * inner_radius, cy + sa0 * inner_radius
   mut ox0, oy0 = cx + ca0 * outer_radius, cy + sa0 * outer_radius
   mut done = 0
   while(done < steps){
      def batch = min(max_batch, steps - done)
      def bytes = _VKR_VERT_STRIDE * 6 * batch
      if(!_check_flush(bytes)){ return 0 }
      def base = _local_vertex_map + _vertex_offset
      mut vi = 0
      mut j = 0
      while(j < batch){
         def ca1, sa1 = ca0 * cs - sa0 * sn, sa0 * cs + ca0 * sn
         def ix1 = cx + ca1 * inner_radius def iy1 = cy + sa1 * inner_radius
         def ox1 = cx + ca1 * outer_radius def oy1 = cy + sa1 * outer_radius
         _vkr_store_vertex(base, vi + 0, ox0, oy0, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 1, ix0, iy0, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 2, ox1, oy1, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 3, ix0, iy0, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 4, ix1, iy1, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 5, ox1, oy1, 0.0, 0.0, 0.0, c, _current_tex_index)
         ca0, sa0 = ca1, sa1
         ix0, iy0 = ix1, iy1
         ox0, oy0 = ox1, oy1
         vi += 6
         j += 1
      }
      _vertex_offset += bytes
      done += batch
   }
   0
}

@jit
fn draw_star_2d(f64: cx,
   f64: cy,
   f64: inner_radius,
   f64: outer_radius,
   int: pts,
   f64: rotation_rad,
   f64: r,
   f64: g,
   f64: b,
   f64: a): any {
   "Batches a filled star as raw vertices."
   if(!_frame_open){ return 0 }
   pts = int(pts)
   if(pts < 2 || outer_radius <= 0.0){ return 0 }
   if(inner_radius <= 0.0){ inner_radius = outer_radius * 0.5 }
   def total = pts * 2
   _bind_default_texture_fast()
   def c = _pack_color(r, g, b, a)
   def max_batch = max(1, int(_vertex_capacity / (_VKR_VERT_STRIDE * 3)))
   def step = TAU / float(total)
   def cs = cos(step)
   def sn = sin(step)
   mut ca0, sa0 = cos(rotation_rad), sin(rotation_rad)
   mut done = 0
   while(done < total){
      def batch = min(max_batch, total - done)
      def bytes = _VKR_VERT_STRIDE * 3 * batch
      if(!_check_flush(bytes)){ return 0 }
      def base = _local_vertex_map + _vertex_offset
      mut vi = 0
      mut j = 0
      while(j < batch){
         def i = done + j
         def ca1 = ca0 * cs - sa0 * sn
         def sa1 = sa0 * cs + ca0 * sn
         def r0 = (i % 2 == 0) ? outer_radius : inner_radius
         def r1 = (i % 2 == 0) ? inner_radius : outer_radius
         _vkr_store_vertex(base, vi + 0, cx, cy, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 1, cx + ca0 * r0, cy + sa0 * r0, 0.0, 0.0, 0.0, c, _current_tex_index)
         _vkr_store_vertex(base, vi + 2, cx + ca1 * r1, cy + sa1 * r1, 0.0, 0.0, 0.0, c, _current_tex_index)
         ca0, sa0 = ca1, sa1
         vi += 3
         j += 1
      }
      _vertex_offset += bytes
      done += batch
   }
   0
}

fn draw_line_3d(f64: x1, f64: y1, f64: z1, f64: x2, f64: y2, f64: z2, f64: thickness, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a solid 3D extruded box(rectangular prism) along the line segment."
   if(!_frame_open){ return 0 }
   _bind_default_texture_fast()
   def dx, dy = float(x2) - float(x1), float(y2) - float(y1)
   def dz = float(z2) - float(z1)
   def len = sqrt(dx*dx + dy*dy + dz*dz)
   if(len < 0.0001){ return 0 }
   if(!_check_flush(_VKR_VERT_STRIDE * 36)){ return 0 }
   def nx = dx / len def ny = dy / len def nz = dz / len
   mut ux, uy, uz = 0.0, 0.0, 0.0
   def abs_ny = abs(ny)
   if(abs_ny > 0.9){ ux = 1.0 uy = 0.0 uz = 0.0 } elif(abs(nx) > abs(nz)){
      ux, uy = -nz, 0.0
      uz = nx
   } else {
      ux, uy = nz, 0.0
      uz = -nx
   }
   def ul = sqrt(ux*ux + uy*uy + uz*uz)
   if(ul > 0.0001){
      ux, uy = ux / ul, uy / ul
      uz = uz / ul
   }
   def vx, vy = ny * uz - nz * uy, nz * ux - nx * uz
   def vz = nx * uy - ny * ux
   def hs = float(thickness) * 0.5
   def fx1 = float(x1) def fy1 = float(y1) def fz1 = float(z1)
   def fx2 = float(x2) def fy2 = float(y2) def fz2 = float(z2)
   def c = _pack_color(r, g, b, a)
   def u1x = -ux*hs def u1y = -uy*hs def u1z = -uz*hs
   def u2x =  ux*hs def u2y =  uy*hs def u2z =  uz*hs
   def v1x = -vx*hs def v1y = -vy*hs def v1z = -vz*hs
   def v2x =  vx*hs def v2y =  vy*hs def v2z =  vz*hs
   def a1x = fx1+u1x+v1x def a1y = fy1+u1y+v1y def a1z = fz1+u1z+v1z
   def a2x = fx1+u2x+v1x def a2y = fy1+u2y+v1y def a2z = fz1+u2z+v1z
   def a3x = fx1+u2x+v2x def a3y = fy1+u2y+v2y def a3z = fz1+u2z+v2z
   def a4x = fx1+u1x+v2x def a4y = fy1+u1y+v2y def a4z = fz1+u1z+v2z
   def b1x = fx2+u1x+v1x def b1y = fy2+u1y+v1y def b1z = fz2+u1z+v1z
   def b2x = fx2+u2x+v1x def b2y = fy2+u2y+v1y def b2z = fz2+u2z+v1z
   def b3x = fx2+u2x+v2x def b3y = fy2+u2y+v2y def b3z = fz2+u2z+v2z
   def b4x = fx2+u1x+v2x def b4y = fy2+u1y+v2y def b4z = fz2+u1z+v2z
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base,  0, a1x,a1y,a1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  1, a2x,a2y,a2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  2, a3x,a3y,a3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  3, a1x,a1y,a1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  4, a3x,a3y,a3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  5, a4x,a4y,a4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  6, b1x,b1y,b1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  7, b3x,b3y,b3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  8, b2x,b2y,b2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  9, b1x,b1y,b1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 10, b4x,b4y,b4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 11, b3x,b3y,b3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 12, a1x,a1y,a1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 13, b1x,b1y,b1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 14, b2x,b2y,b2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 15, a1x,a1y,a1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 16, b2x,b2y,b2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 17, a2x,a2y,a2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 18, a2x,a2y,a2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 19, b2x,b2y,b2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 20, b3x,b3y,b3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 21, a2x,a2y,a2z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 22, b3x,b3y,b3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 23, a3x,a3y,a3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 24, a3x,a3y,a3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 25, b3x,b3y,b3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 26, b4x,b4y,b4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 27, a3x,a3y,a3z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 28, b4x,b4y,b4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 29, a4x,a4y,a4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 30, a4x,a4y,a4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 31, b4x,b4y,b4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 32, b1x,b1y,b1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 33, a4x,a4y,a4z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 34, b1x,b1y,b1z, 0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 35, a1x,a1y,a1z, 0,0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 36
   0
}

@inline
fn _draw_xz_line_3d_fast(f64: x1, f64: z1, f64: x2, f64: z2, f64: thick, int: c): any {
   def dx, dz = float(x2) - float(x1), float(z2) - float(z1)
   def len = sqrt(dx*dx + dz*dz)
   if(len < 0.0001){ return 0 }
   def nx = dx / len def nz = dz / len
   def hs = float(thick) * 0.5
   def uy = hs
   def vx = -nz * hs def vz = nx * hs
   def fx1 = float(x1) def fz1 = float(z1)
   def fx2 = float(x2) def fz2 = float(z2)
   if(!_check_flush(_VKR_VERT_STRIDE * 36)){ return 0 }
   def base = _local_vertex_map + _vertex_offset
   _vkr_store_vertex(base,  0,  fx1+vx-uy, -hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  1,  fx1-vx-uy, -hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  2,  fx1-vx+uy,  hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  3,  fx1+vx-uy, -hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  4,  fx1-vx+uy,  hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  5,  fx1+vx+uy,  hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  6,  fx2+vx-uy, -hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  7,  fx2-vx+uy,  hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  8,  fx2-vx-uy, -hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base,  9,  fx2+vx-uy, -hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 10,  fx2+vx+uy,  hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 11,  fx2-vx+uy,  hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 12,  fx1+vx-uy, -hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 13,  fx2+vx-uy, -hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 14,  fx2-vx-uy, -hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 15,  fx1+vx-uy, -hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 16,  fx2-vx-uy, -hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 17,  fx1-vx-uy, -hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 18,  fx1+vx+uy,  hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 19,  fx2+vx+uy,  hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 20,  fx2-vx+uy,  hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 21,  fx1+vx+uy,  hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 22,  fx2-vx+uy,  hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 23,  fx1-vx+uy,  hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 24,  fx1-vx-uy, -hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 25,  fx2-vx-uy, -hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 26,  fx2-vx+uy,  hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 27,  fx1-vx-uy, -hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 28,  fx2-vx+uy,  hs,   fz2-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 29,  fx1-vx+uy,  hs,   fz1-vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 30,  fx1+vx-uy, -hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 31,  fx2+vx+uy,  hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 32,  fx2+vx-uy, -hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 33,  fx1+vx-uy, -hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 34,  fx1+vx+uy,  hs,   fz1+vz,    0,0, c, _current_tex_index)
   _vkr_store_vertex(base, 35,  fx2+vx+uy,  hs,   fz2+vz,    0,0, c, _current_tex_index)
   _vertex_offset += _VKR_VERT_STRIDE * 36
   0
}

fn draw_grid_3d(f64: size, f64: step): any {
   "Draws an infinite-style 3D grid on the XZ plane."
   if(!_frame_open){ return 0 }
   _bind_default_texture_fast()
   def s, c = float(size), _pack_color(0.2, 0.2, 0.4, 0.5)
   mut gx = -s
   while(gx <= s){
      _draw_xz_line_3d_fast(gx, -s, gx, s,  0.03, c)
      _draw_xz_line_3d_fast(-s,  gx, s, gx, 0.03, c)
      gx += float(step)
   }
   0
}

@inline
fn _cube_face_emit_store(any: base, int: vi,
   f64: x0, f64: y0, f64: z0, f64: x1, f64: y1, f64: z1,
   f64: x2, f64: y2, f64: z2, f64: x3, f64: y3, f64: z3,
   int: c, int: tex_id, f64: nx, f64: ny, f64: nz): any {
   _vkr_store_vertex(base, vi + 0, x0, y0, z0, 0, 0, c, tex_id, nx, ny, nz)
   _vkr_store_vertex(base, vi + 1, x1, y1, z1, 1, 1, c, tex_id, nx, ny, nz)
   _vkr_store_vertex(base, vi + 2, x2, y2, z2, 1, 0, c, tex_id, nx, ny, nz)
   _vkr_store_vertex(base, vi + 3, x0, y0, z0, 0, 0, c, tex_id, nx, ny, nz)
   _vkr_store_vertex(base, vi + 4, x3, y3, z3, 0, 1, c, tex_id, nx, ny, nz)
   _vkr_store_vertex(base, vi + 5, x1, y1, z1, 1, 1, c, tex_id, nx, ny, nz)
   0
}

@inline
fn _mesh_axes_emit_box(any: base, f64: sx, f64: sy, f64: sz, int: c): any {
   _cube_face_emit_store(base, 0, -0.5 * sx, -0.5 * sy, 0.5 * sz, 0.5 * sx, 0.5 * sy, 0.5 * sz, 0.5 * sx, -0.5 * sy, 0.5 * sz, -0.5 * sx, 0.5 * sy, 0.5 * sz, c, 0, 0, 0, 1)
   _cube_face_emit_store(base, 6, 0.5 * sx, -0.5 * sy, -0.5 * sz, -0.5 * sx, 0.5 * sy, -0.5 * sz, -0.5 * sx, -0.5 * sy, -0.5 * sz, 0.5 * sx, 0.5 * sy, -0.5 * sz, c, 0, 0, 0, -1)
   _cube_face_emit_store(base, 12, 0.5 * sx, -0.5 * sy, 0.5 * sz, 0.5 * sx, 0.5 * sy, -0.5 * sz, 0.5 * sx, -0.5 * sy, -0.5 * sz, 0.5 * sx, 0.5 * sy, 0.5 * sz, c, 0, 1, 0, 0)
   _cube_face_emit_store(base, 18, -0.5 * sx, -0.5 * sy, -0.5 * sz, -0.5 * sx, 0.5 * sy, 0.5 * sz, -0.5 * sx, -0.5 * sy, 0.5 * sz, -0.5 * sx, 0.5 * sy, -0.5 * sz, c, 0, -1, 0, 0)
   _cube_face_emit_store(base, 24, -0.5 * sx, 0.5 * sy, -0.5 * sz, 0.5 * sx, 0.5 * sy, 0.5 * sz, -0.5 * sx, 0.5 * sy, 0.5 * sz, 0.5 * sx, 0.5 * sy, -0.5 * sz, c, 0, 0, 1, 0)
   _cube_face_emit_store(base, 30, -0.5 * sx, -0.5 * sy, 0.5 * sz, 0.5 * sx, -0.5 * sy, -0.5 * sz, -0.5 * sx, -0.5 * sy, -0.5 * sz, 0.5 * sx, -0.5 * sy, 0.5 * sz, c, 0, 0, -1, 0)
   0
}

fn mesh_build_axes_3d(f64: gizmo_len, f64: cube_sz): any {
   "Builds one combined 3-bar axis mesh centered on origin. Returns {ptr, cnt}."
   def L, T = float(gizmo_len), float(cube_sz)
   def total = 36 * 3
   def buf = malloc(total * VERTEX_STRIDE)
   if(!buf){ return 0 }
   def base0, base1 = buf, buf + 36 * VERTEX_STRIDE
   def base2 = buf + 72 * VERTEX_STRIDE
   _mesh_axes_emit_box(base0, L * 2.0, T, T, color_pack(1.0, 0.15, 0.05, 1.0))
   _mesh_axes_emit_box(base1, T, L * 2.0, T, color_pack(0.15, 1.0, 0.05, 1.0))
   _mesh_axes_emit_box(base2, T, T, L * 2.0, color_pack(0.05, 0.15, 1.0, 1.0))
   return {"ptr": buf, "cnt": total}
}

fn draw_cube_3d(f64: x,
   f64: y,
   f64: z,
   f64: size,
   f64: rx=0.0,
   f64: ry=0.0,
   f64: rz=0.0,
   any: r=1.0,
   f64: g=1.0,
   f64: b=1.0,
   f64: a=1.0,
   int: tex_id=-1): any {
   "Batches a colored 3D cube with explicit XYZ rotation."
   if(!_frame_open){ return 0 }
   def nc_pipe = _get_nocull_pipeline()
   mut want_pipe = _target_pipeline
   if(nc_pipe != 0){ want_pipe = nc_pipe }
   if(_current_is_unlit == 0 || _target_pipeline != want_pipe){
      _flush_reason = 4
      _flush()
      _current_is_unlit = 1
      _last_is_unlit = 1
      _pc_dirty = true
      _target_pipeline = want_pipe
      _use_custom_pc = 0
   } elif(_use_custom_pc != 0){
      _use_custom_pc = 0
      _pc_dirty = true
   }
   def tid = (tex_id < 0) ? _default_texture : tex_id
   bind_texture(tid)
   if(!_check_flush(_VKR_VERT_STRIDE * 36)){ return 0 }
   def c = is_int(r) ? r : _pack_color(r, g, b, a)
   def base = _local_vertex_map + _vertex_offset
   def s = float(size) * 0.5
   def fx = float(x) def fy = float(y) def fz = float(z)
   if(rx == 0.0 && ry == 0.0 && rz == 0.0){
      def x0 = fx - s def x1 = fx + s
      def y0 = fy - s def y1 = fy + s
      def z0 = fz - s def z1 = fz + s
      _cube_face_emit_store(base, 0, x0, y0, z1, x1, y1, z1, x1, y0, z1, x0, y1, z1, c, _current_tex_index,  0,  0,  1)
      _cube_face_emit_store(base, 6, x1, y0, z0, x0, y1, z0, x0, y0, z0, x1, y1, z0, c, _current_tex_index,  0,  0, -1)
      _cube_face_emit_store(base, 12, x1, y0, z1, x1, y1, z0, x1, y0, z0, x1, y1, z1, c, _current_tex_index,  1,  0,  0)
      _cube_face_emit_store(base, 18, x0, y0, z0, x0, y1, z1, x0, y0, z1, x0, y1, z0, c, _current_tex_index, -1,  0,  0)
      _cube_face_emit_store(base, 24, x0, y1, z0, x1, y1, z1, x0, y1, z1, x1, y1, z0, c, _current_tex_index,  0,  1,  0)
      _cube_face_emit_store(base, 30, x0, y0, z1, x1, y0, z0, x0, y0, z0, x1, y0, z1, c, _current_tex_index,  0, -1,  0)
      _vertex_offset += _VKR_VERT_STRIDE * 36
      return 0
   }
   def cx = cos(rx) def sx = sin(rx)
   def cy = cos(ry) def sy = sin(ry)
   def cz = cos(rz) def sz = sin(rz)
   def rot = fn(f64: px, f64: py, f64: pz): any {
      def x1, y1 = px, py * cx - pz * sx
      def z1 = py * sx + pz * cx
      def x2 = x1 * cy + z1 * sy
      def y2 = y1
      def z2 = -x1 * sy + z1 * cy
      def x3 = x2 * cz - y2 * sz
      def y3 = x2 * sz + y2 * cz
      def z3 = z2
      [x3, y3, z3]
   }
   def v = fn(int: i, f64: px, f64: py, f64: pz, f64: u, f64: tv, f64: nx, f64: ny, f64: nz): any {
      def p, n = rot(px, py, pz), rot(nx, ny, nz)
      _vkr_store_vertex(
         base, i,
         fx + p.get(0,0.0), fy + p.get(1,0.0), fz + p.get(2,0.0),
         u, tv, c, _current_tex_index,
         n.get(0,0.0), n.get(1,0.0), n.get(2,1.0)
      )
      0
   }
   v( 0,-s,-s, s,0,0, 0,0, 1)  v( 1, s, s, s,1,1, 0,0, 1)  v( 2, s,-s, s,1,0, 0,0, 1)
   v( 3,-s,-s, s,0,0, 0,0, 1)  v( 4,-s, s, s,0,1, 0,0, 1)  v( 5, s, s, s,1,1, 0,0, 1)
   v( 6, s,-s,-s,0,0, 0,0,-1)  v( 7,-s, s,-s,1,1, 0,0,-1)  v( 8,-s,-s,-s,1,0, 0,0,-1)
   v( 9, s,-s,-s,0,0, 0,0,-1)  v(10, s, s,-s,0,1, 0,0,-1)  v(11,-s, s,-s,1,1, 0,0,-1)
   v(12, s,-s, s,0,0, 1,0,0)   v(13, s, s,-s,1,1, 1,0,0)   v(14, s,-s,-s,1,0, 1,0,0)
   v(15, s,-s, s,0,0, 1,0,0)   v(16, s, s, s,0,1, 1,0,0)   v(17, s, s,-s,1,1, 1,0,0)
   v(18,-s,-s,-s,0,0,-1,0,0)   v(19,-s, s, s,1,1,-1,0,0)   v(20,-s,-s, s,1,0,-1,0,0)
   v(21,-s,-s,-s,0,0,-1,0,0)   v(22,-s, s,-s,0,1,-1,0,0)   v(23,-s, s, s,1,1,-1,0,0)
   v(24,-s, s,-s,0,0, 0,1,0)   v(25, s, s, s,1,1, 0,1,0)   v(26,-s, s, s,0,1, 0,1,0)
   v(27,-s, s,-s,0,0, 0,1,0)   v(28, s, s,-s,1,0, 0,1,0)   v(29, s, s, s,1,1, 0,1,0)
   v(30,-s,-s, s,0,0, 0,-1,0)  v(31, s,-s,-s,1,1, 0,-1,0)  v(32,-s,-s,-s,0,1, 0,-1,0)
   v(33,-s,-s, s,0,0, 0,-1,0)  v(34, s,-s, s,1,0, 0,-1,0)  v(35, s,-s,-s,1,1, 0,-1,0)
   _vertex_offset += _VKR_VERT_STRIDE * 36
   0
}

fn draw_line_strip_2d(f64: x, f64: y, f64: w, f64: h, list: history, f64: scale, f64: r, f64: g, f64: b, f64: a): any {
   "Batches a UI line strip from a history list."
   if(!_frame_open){ return 0 }
   _bind_default_texture_fast()
   def count = history.len
   if(count < 2){ return 0 }
   if(!_check_flush((count-1) * (_VKR_VERT_STRIDE * 6))){ return 0 }
   def c = _pack_color(r, g, b, a)
   def dcount = float(count - 1)
   def step = float(w) / dcount
   def fh = float(h) def fx = float(x) def fy = float(y)
   def fs = float(scale)
   def th = 1.0
   mut base_idx = _vertex_offset / _VKR_VERT_STRIDE
   mut i = 0
   mut v1 = float(history.get(0, 0)) * fs
   if(v1 > 1.0){ v1 = 1.0 }
   while(i < count - 1){
      mut v2 = float(history.get(i + 1, 0)) * fs
      if(v2 > 1.0){ v2 = 1.0 }
      def px1, py1 = fx + float(i) * step, fy + fh * (1.0 - v1)
      def px2, py2 = fx + float(i+1) * step, fy + fh * (1.0 - v2)
      _vkr_store_vertex(_local_vertex_map, base_idx + 0, px1, py1 - th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 1, px1, py1 + th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 2, px2, py2 + th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 3, px1, py1 - th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 4, px2, py2 + th, 0.0, 0.0, 0.0, c, _current_tex_index)
      _vkr_store_vertex(_local_vertex_map, base_idx + 5, px2, py2 - th, 0.0, 0.0, 0.0, c, _current_tex_index)
      base_idx += 6
      v1 = v2
      i += 1
   }
   _vertex_offset = base_idx * _VKR_VERT_STRIDE
   0
}

fn draw_static_buffer(dict: sbuf, bool: is_lines=false, f64: width=1.0, any: pipe_override=0, bool: is_points=false): bool {
   "Records a draw command for a static GPU buffer. Must be called inside a frame."
   if(!_frame_open || !is_dict(sbuf)){ return false }
   def buf = sbuf.get("handle", 0)
   def voff = sbuf.get("offset", 0)
   def count = sbuf.get("count", 0)
   if(!buf || count <= 0){ return false }
   draw_static_buffer_raw(buf, voff, count, is_lines, width, pipe_override, is_points)
}

fn draw_static_buffer_raw(any: buf,
   any: voff,
   int: count,
   bool: is_lines=false,
   f64: width=1.0,
   any: pipe_override=0,
   bool: is_points=false): bool {
   "Records a draw command for a static GPU vertex buffer using raw Vulkan handles."
   if(!_frame_open || !buf || count <= 0){ return false }
   def cb = _begin_static_draw(is_lines, width, pipe_override, is_points)
   def first_vertex = _bind_static_vertex_buffer(cb, buf, voff)
   if(first_vertex < 0){ return false }
   cmd_draw(cb, count, 1, first_vertex, 0)
   _total_draw_calls += 1
   _frame_draw_calls += 1
   _frame_static_draw_calls += 1
   true
}

fn draw_static_buffer_indexed(dict: sbuf,
   any: idx_buf,
   int: index_count,
   bool: is_lines=false,
   f64: width=1.0,
   any: pipe_override=0,
   bool: is_points=false): bool {
   "Records a draw command for a static GPU buffer with indices."
   if(!_frame_open || !is_dict(sbuf)){ return false }
   def buf = sbuf.get("handle", 0)
   def voff = sbuf.get("offset", 0)
   def ioff = sbuf.get("ioffset", 0)
   if(!buf || !idx_buf || index_count <= 0){ return false }
   draw_static_buffer_indexed_raw(buf, voff, idx_buf, ioff, index_count, is_lines, width, pipe_override, 0, is_points)
}

fn draw_static_buffer_indexed_raw(any: buf,
   any: voff,
   any: idx_buf,
   any: ioff,
   int: index_count,
   bool: is_lines=false,
   f64: width=1.0,
   any: pipe_override=0,
   int: index_type=0,
   bool: is_points=false): bool {
   "Records a draw command for a static indexed GPU buffer using raw Vulkan handles."
   if(!_frame_open || !buf || !idx_buf || index_count <= 0){ return false }
   def cb = _begin_static_draw(is_lines, width, pipe_override, is_points)
   def first_vertex = _bind_static_vertex_buffer(cb, buf, voff)
   if(first_vertex < 0){ return false }
   def first_index = _bind_static_index_buffer(cb, idx_buf, ioff, index_type)
   cmd_draw_indexed(cb, index_count, 1, first_index, first_vertex, 0)
   _total_draw_calls += 1
   _frame_draw_calls += 1
   _frame_static_draw_calls += 1
   _frame_indexed_draw_calls += 1
   true
}

fn draw_circle_sdf(f64: x, f64: y, f64: radius, f64: r, f64: g, f64: b, f64: a): bool {
   "Draws a smooth circle using the SDF pipeline, but keeps batching active."
   if(!_circle_pipeline && !_ensure_circle_pipeline()){ return false }
   if(!_prepare_sdf_draw(_circle_pipeline)){ return false }
   def c = _pack_color(r, g, b, a)
   __vkr_push_rect_sdf(
      _local_vertex_map + _vertex_offset,
      x - radius, y - radius,
      radius * 2.0, radius * 2.0,
      c, 0, 0, 1.0
   )
   _vertex_offset += _VKR_VERT_STRIDE * 6
   true
}

fn draw_ring_sdf(f64: x, f64: y, f64: inner_radius, f64: outer_radius, f64: r, f64: g, f64: b, f64: a): bool {
   "Draws a smooth ring using the SDF pipeline, but keeps batching active."
   if(!_ring_pipeline && !_ensure_ring_pipeline()){ return false }
   if(outer_radius <= inner_radius){ return false }
   if(!_prepare_sdf_draw(_ring_pipeline)){ return false }
   def c = _pack_color(r, g, b, a)
   def ratio = inner_radius / outer_radius
   __vkr_push_rect_sdf(
      _local_vertex_map + _vertex_offset,
      x - outer_radius, y - outer_radius,
      outer_radius * 2.0, outer_radius * 2.0,
      c, ratio, 0, 1.0
   )
   _vertex_offset += _VKR_VERT_STRIDE * 6
   true
}

fn draw_axes_3d(f64: gizmo_len, f64: cube_sz=0.4): any {
   "Draws 3D coordinate axes(X=red, Y=green, Z=blue)."
   def len = float(gizmo_len)
   def sz = float(cube_sz)
   draw_line_3d(0, 0, 0, len, 0, 0, 1.0, 1.0, 0.0, 0.0, 1.0)
   draw_line_3d(0, 0, 0, 0, len, 0, 1.0, 0.0, 1.0, 0.0, 1.0)
   draw_line_3d(0, 0, 0, 0, 0, len, 1.0, 0.0, 0.0, 1.0, 1.0)
   def cube_x, cube_y = len - sz * 0.5, len - sz * 0.5
   def cube_z = len - sz * 0.5
   draw_cube_3d(cube_x, 0, 0, sz, 1.0, 0.0, 0.0, 1.0)
   draw_cube_3d(0, cube_y, 0, sz, 0.0, 1.0, 0.0, 1.0)
   draw_cube_3d(0, 0, cube_z, sz, 0.0, 0.0, 1.0, 1.0)
   0
}
