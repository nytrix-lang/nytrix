;; Keywords: render vulkan gpu utils os ui
;; Vulkan utility routines for handles, memory, layout transitions, and debug formatting.
;; References:
;; - std.os.ui.render.vk
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk.utils(__vkr_push_rect_tex_fast, __vkr_push_quad_xyuv_fast, __vkr_push_rect_outline_fast, _vkr_color_u32, __vkr_pack_color, _vkr_store_vertex, __vkr_push_vertex, __vkr_push_rect_tex, _init_quad_template, __vkr_push_rect, __vkr_push_line, __vkr_push_line_sdf, __vkr_push_rect_sdf, _check_debug_env, _dbg_handle, _get_vertex_offset, _get_local_vertex_map, _advance_vertex_offset, _vkr_bind_dynamic_vertex_buffer, _vkr_pipeline_known, _vkr_bind_pipeline_if_needed, _pack_color, _push_vertex, _vkr_safe_f32_limit, _vkr_bgra_to_rgba_if_needed, store_mat4_cm_raw, pack_emissive_u32, pack_normal_tex_word, pack_rgba_u32, pack_material_scalar_u32, pack_alpha_cutoff_u32, gltf_anim_apply_uv_pointer_override, gltf_anim_apply_material_pointer_overrides, gltf_expand_indexed_vertices, gltf_rewind_triangle_vertices, gltf_sync_drawable_part_from_raw, gltf_sync_drawable_parts_from_raw, pack_bsdf0_u32, pack_bsdf1_u32, pack_bsdf2_u32, pack_bsdf3_u32, pack_bsdf4_u32, pack_bsdf5_u32, pack_bsdf_ext_slab, pack_material_slab)
use std.core
use std.core.mem
use std.math
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.shared as render_shared
use std.os.ui.render.vk.state
use std.os.ui.render.vk.vulkan (cmd_bind_vertex_buffers, cmd_bind_pipeline)
use std.core.common as common
use std.core.str (to_hex)
use std.os.ui.render.utils as render_utils

mut _cached_ubo_env = -1
mut _cached_renderdoc_env = -1

@inline
fn store_mat4_cm_raw(any dst, any mat, bool allow_plain16=false) bool { render_shared.store_mat4_cm_raw(dst, mat, allow_plain16) }

fn pack_normal_tex_word(int normal_tex_id, int normal_uv_set, f64 normal_scale=1.0, bool clearcoat_only=false, bool mirrored_double_sided=false, bool double_sided=false) int { render_utils.pack_normal_tex_word(normal_tex_id, normal_uv_set, normal_scale, clearcoat_only, mirrored_double_sided, double_sided) }

fn pack_bsdf0_u32(dict minfo) int { render_utils.pack_bsdf0_u32(minfo) }

fn pack_bsdf1_u32(dict minfo) int { render_utils.pack_bsdf1_u32(minfo) }

fn pack_bsdf2_u32(dict minfo) int { render_utils.pack_bsdf2_u32(minfo) }

fn pack_bsdf3_u32(dict minfo) int { render_utils.pack_bsdf3_u32(minfo) }

fn pack_bsdf4_u32(dict minfo) int { render_utils.pack_bsdf4_u32(minfo) }

fn pack_bsdf5_u32(dict minfo) int { render_utils.pack_bsdf5_u32(minfo) }

fn pack_bsdf_ext_slab(any minfo) any { render_utils.pack_bsdf_ext_slab(minfo) }

fn pack_emissive_u32(any emissive_factor, f64 emissive_strength=1.0) int { render_utils.pack_emissive_u32(emissive_factor, emissive_strength) }

fn pack_rgba_u32(list v) int { render_utils.pack_rgba_u32(v) }

fn pack_material_scalar_u32(int cur_mat, str kind, any v) int { render_utils.pack_material_scalar_u32(cur_mat, kind, v) }

fn pack_alpha_cutoff_u32(int cur_alpha, any v) int { render_utils.pack_alpha_cutoff_u32(cur_alpha, v) }

fn gltf_anim_apply_uv_pointer_override(any out, any mesh, any slab, str slot, str kind, any val) list { render_utils.gltf_anim_apply_uv_pointer_override(out, mesh, slab, slot, kind, val) }

fn gltf_anim_apply_material_pointer_overrides(any part, any ptr_overrides) any { render_utils.gltf_anim_apply_material_pointer_overrides(part, ptr_overrides) }

fn gltf_sync_drawable_part_from_raw(any part, any raw_part, bool update_part_tex=true, bool update_part_material=true) any { render_utils.gltf_sync_drawable_part_from_raw(part, raw_part, update_part_tex, update_part_material) }

fn gltf_sync_drawable_parts_from_raw(any existing_parts, any raw_parts, bool update_part_tex=true, bool update_part_material=true) any { render_utils.gltf_sync_drawable_parts_from_raw(existing_parts, raw_parts, update_part_tex, update_part_material) }

fn gltf_expand_indexed_vertices(?ptr vptr, int vcnt, ?ptr iptr, int icnt, bool idx_u32=false) ?ptr { render_utils.gltf_expand_indexed_vertices(vptr, vcnt, iptr, icnt, idx_u32) }

fn gltf_rewind_triangle_vertices(?ptr vptr, int vcnt) ?ptr { render_utils.gltf_rewind_triangle_vertices(vptr, vcnt) }

fn pack_material_slab(any part) any { render_utils.pack_material_slab(part) }

fn _vkr_safe_f32(any v, f64 fallback=0.0) f64 { render_shared.safe_f32_limit(v, fallback, 1048576.0) }

fn _vkr_safe_f32_limit(any v, f64 fallback=0.0, f64 limit=1048576.0) f64 { render_shared.safe_f32_limit(v, fallback, limit) }

@inline
fn __vkr_push_quad_xyuv_fast(any p, any x0, any y0, any x1, any y1, any u1, any v1, any u2, any v2, any color_u32, any tex_id=0) any {
   if !p { return 0 }
   _vkr_write_quad_xyuv_template(p, x0, y0, x1, y1, u1, v1, u2, v2, _vkr_color_u32(color_u32), tex_id)
   p
}

@inline
fn __vkr_push_rect_tex_fast(any p, any x, any y, any w, any h, any u1, any v1, any u2, any v2, any color_u32, any tex_id=0) any {
   if !p { return 0 }
   def x0, y0 = _vkr_safe_f32(x), _vkr_safe_f32(y)
   def x1, y1 = _vkr_safe_f32(x0 + _vkr_safe_f32(w)), _vkr_safe_f32(y0 + _vkr_safe_f32(h))
   _vkr_write_quad_xyuv_template(p, x0, y0, x1, y1, u1, v1, u2, v2, _vkr_color_u32(color_u32), tex_id)
   p
}

@jit
fn __vkr_push_rect_outline_fast(any p, any x, any y, any w, any h, any color_u32, any tex_id=0) any {
   if !p { return 0 }
   def x0, y0 = _vkr_safe_f32(x), _vkr_safe_f32(y)
   def x1, y1 = _vkr_safe_f32(x0 + _vkr_safe_f32(w)), _vkr_safe_f32(y0 + _vkr_safe_f32(h))
   def c = _vkr_color_u32(color_u32)
   _vkr_write_quad_xyuv_template(p + 0 * _VKR_VERT_STRIDE * 6, x0, y0, x1, _vkr_safe_f32(y0 + 1.0), 0.0, 0.0, 0.0, 0.0, c, tex_id)
   _vkr_write_quad_xyuv_template(p + 1 * _VKR_VERT_STRIDE * 6, x0, _vkr_safe_f32(y1 - 1.0), x1, y1, 0.0, 0.0, 0.0, 0.0, c, tex_id)
   _vkr_write_quad_xyuv_template(p + 2 * _VKR_VERT_STRIDE * 6, x0, y0, _vkr_safe_f32(x0 + 1.0), y1, 0.0, 0.0, 0.0, 0.0, c, tex_id)
   _vkr_write_quad_xyuv_template(p + 3 * _VKR_VERT_STRIDE * 6, _vkr_safe_f32(x1 - 1.0), y0, x1, y1, 0.0, 0.0, 0.0, 0.0, c, tex_id)
   p
}

@jit
fn _vkr_push_quad_vertex_fast(any v, any x, any y, any u, any uv, int color_u32, any tex_id) any {
   store32_f32(v, _vkr_safe_f32(x), _VKR_OFF_X)
   store32_f32(v, _vkr_safe_f32(y), _VKR_OFF_Y)
   store32_f32(v, 0.0, _VKR_OFF_Z)
   store32_f32(v, _vkr_safe_f32(u), _VKR_OFF_U)
   store32_f32(v, _vkr_safe_f32(uv), _VKR_OFF_V)
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

@jit
fn _vkr_write_quad_xyuv_fast(any p, any x0, any y0, any x1, any y1, any u1, any v1, any u2, any v2, int color_u32, any tex_id) any {
   _vkr_push_quad_vertex_fast(p + 0 * _VKR_VERT_STRIDE, x0, y0, u1, v1, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 1 * _VKR_VERT_STRIDE, x0, y1, u1, v2, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 2 * _VKR_VERT_STRIDE, x1, y1, u2, v2, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 3 * _VKR_VERT_STRIDE, x1, y1, u2, v2, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 4 * _VKR_VERT_STRIDE, x1, y0, u2, v1, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 5 * _VKR_VERT_STRIDE, x0, y0, u1, v1, color_u32, tex_id)
}

@inline
fn _vkr_patch_quad_vertex_template(any v, any x, any y, any u, any uv, int color_u32, any tex_id) any {
   store32_f32(v, _vkr_safe_f32(x), _VKR_OFF_X)
   store32_f32(v, _vkr_safe_f32(y), _VKR_OFF_Y)
   store32_f32(v, _vkr_safe_f32(u), _VKR_OFF_U)
   store32_f32(v, _vkr_safe_f32(uv), _VKR_OFF_V)
   store32(v, color_u32, _VKR_OFF_C)
   store32(v, tex_id, _VKR_OFF_TEX)
}

@inline
@jit
fn _vkr_write_quad_xyuv_template(any p, any x0, any y0, any x1, any y1, any u1, any v1, any u2, any v2, int color_u32, any tex_id) any {
   if _quad_template {
      __copy_mem(p, _quad_template, _VKR_VERT_STRIDE * 6)
      _vkr_patch_quad_vertex_template(p + 0 * _VKR_VERT_STRIDE, x0, y0, u1, v1, color_u32, tex_id)
      _vkr_patch_quad_vertex_template(p + 1 * _VKR_VERT_STRIDE, x0, y1, u1, v2, color_u32, tex_id)
      _vkr_patch_quad_vertex_template(p + 2 * _VKR_VERT_STRIDE, x1, y1, u2, v2, color_u32, tex_id)
      _vkr_patch_quad_vertex_template(p + 3 * _VKR_VERT_STRIDE, x1, y1, u2, v2, color_u32, tex_id)
      _vkr_patch_quad_vertex_template(p + 4 * _VKR_VERT_STRIDE, x1, y0, u2, v1, color_u32, tex_id)
      _vkr_patch_quad_vertex_template(p + 5 * _VKR_VERT_STRIDE, x0, y0, u1, v1, color_u32, tex_id)
      return p
   }
   _vkr_write_quad_xyuv_fast(p, x0, y0, x1, y1, u1, v1, u2, v2, color_u32, tex_id)
   p
}

fn _vkr_color_u32(any c) int {
   if is_int(c) { return c }
   if is_float(c) { return __flt_to_int(c) }
   if !is_list(c) { return 0xFFFFFFFF }
   _pack_color(c.get(0, 1.0), c.get(1, 1.0), c.get(2, 1.0), c.get(3, 1.0))
}

@pure
@jit
fn __vkr_pack_color(any r, any g, any b, any a) int {
   def r8, g8 = __flt_to_int(float(r) * 255.0) & 255, __flt_to_int(float(g) * 255.0) & 255
   def b8, a8 = __flt_to_int(float(b) * 255.0) & 255, __flt_to_int(float(a) * 255.0) & 255
   (a8 << 24) | (r8 << 16) | (g8 << 8) | b8
}

@jit
fn _vkr_store_vertex(any base, int idx, any x, any y, any z, any u, any v, any color, any tex_id=0, any nx=0.0, any ny=0.0, any nz=1.0) any {
   render_shared.store_vertex64(base, idx, x, y, z, u, v, color, tex_id, nx, ny, nz)
}

@jit
fn __vkr_push_vertex(any p, any x, any y, any z, any u, any v, any color, any tex_id=0, any nx=0.0, any ny=0.0, any nz=1.0) any {
   if !p { return 0 }
   _vkr_store_vertex(p, 0, x, y, z, u, v, color, tex_id, nx, ny, nz)
}

@jit
fn __vkr_push_rect_tex(any p, any x, any y, any w, any h, any u1, any v1, any u2, any v2, any color, any tex_id=0, any nz=1.0) any {
   if !p { return 0 }
   def c = _vkr_color_u32(color)
   x, y = _vkr_safe_f32(x), _vkr_safe_f32(y)
   u1, v1 = _vkr_safe_f32(u1), _vkr_safe_f32(v1)
   u2, v2 = _vkr_safe_f32(u2), _vkr_safe_f32(v2)
   def x2, y2 = _vkr_safe_f32(x + _vkr_safe_f32(w)), _vkr_safe_f32(y + _vkr_safe_f32(h))
   _vkr_write_quad_xyuv_template(p, x, y, x2, y2, u1, v1, u2, v2, c, tex_id)
   0
}

fn _init_quad_template() any {
   if !_quad_template { return 0 }
   mut i = 0 while i < 6 {
      def off = _quad_template + i * _VKR_VERT_STRIDE
      store32_f32(off, 0.0, _VKR_OFF_Z)
      store32(off, 0, _VKR_OFF_TEX)
      store32_f32(off, 0.0, _VKR_OFF_NX)
      store32_f32(off, 0.0, _VKR_OFF_NY)
      store32_f32(off, 1.0, _VKR_OFF_NZ)
      store32_f32(off, 1.0, _VKR_OFF_TX)
      store32_f32(off, 0.0, _VKR_OFF_TY)
      store32_f32(off, 0.0, _VKR_OFF_TZ)
      store32_f32(off, 1.0, _VKR_OFF_TW)
      store32_f32(off, 0.0, _VKR_OFF_U2)
      store32_f32(off, 0.0, _VKR_OFF_V2)
      i += 1
   }
}

@jit
fn __vkr_push_rect(any p, any x, any y, any w, any h, any color) any {
   if !p { return 0 }
   def c = _vkr_color_u32(color)
   def x0, y0 = _vkr_safe_f32(x), _vkr_safe_f32(y)
   def x1, y1 = _vkr_safe_f32(x0 + _vkr_safe_f32(w)), _vkr_safe_f32(y0 + _vkr_safe_f32(h))
   if _quad_template {
      _vkr_write_quad_xyuv_fast(p, x0, y0, x1, y1, 0.0, 0.0, 0.0, 0.0, c, _current_tex_index)
      return p
   }
   _vkr_store_vertex(p, 0, x0, y0, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 1, x0, y1, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 2, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 3, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 4, x1, y0, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 5, x0, y0, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
}

fn __vkr_push_line(any p, any x1, any y1, any x2, any y2, any thickness, any color) any {
   if !p { return 0 }
   def dx, dy = float(x2) - float(x1), float(y2) - float(y1)
   def l = sqrt(dx*dx + dy*dy)
   if l == 0.0 { return 0 }
   def th = float(thickness) * 0.5
   def nx = -dy / l * th
   def ny =  dx / l * th
   _vkr_store_vertex(p, 0, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 1, float(x1) - nx, float(y1) - ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 2, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 3, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 4, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 5, float(x2) + nx, float(y2) + ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
}

@inline
fn _vkr_store_line_sdf_vertex(any p, int i, f64 cx, f64 cy, f64 ux, f64 uy, f64 nx, f64 ny, f64 lx, f64 ly, any color, f64 half_len, f64 radius) any {
   _vkr_store_vertex(p, i, cx + ux * lx + nx * ly, cy + uy * lx + ny * ly, 0.0, lx, ly, color, _current_tex_index, half_len, radius, 0.0)
}

@jit
fn __vkr_push_line_sdf(any p, any x1, any y1, any x2, any y2, any thickness, any color) any {
   if !p { return 0 }
   def ax, ay = float(x1), float(y1)
   def bx, by = float(x2), float(y2)
   def dx, dy = bx - ax, by - ay
   def len = sqrt(dx*dx + dy*dy)
   mut ux, uy = 1.0, 0.0
   if len > 0.000001 {
      ux = dx / len
      uy = dy / len
   }
   def nx, ny = -uy, ux
   def half_len = len * 0.5
   def radius = max(0.5, float(thickness) * 0.5)
   def pad = max(1.5, min(4.0, radius * 0.25))
   def lx0, lx1 = -half_len - radius - pad, half_len + radius + pad
   def ly0, ly1 = -radius - pad, radius + pad
   def cx, cy = (ax + bx) * 0.5, (ay + by) * 0.5
   _vkr_store_line_sdf_vertex(p, 0, cx, cy, ux, uy, nx, ny, lx0, ly0, color, half_len, radius)
   _vkr_store_line_sdf_vertex(p, 1, cx, cy, ux, uy, nx, ny, lx0, ly1, color, half_len, radius)
   _vkr_store_line_sdf_vertex(p, 2, cx, cy, ux, uy, nx, ny, lx1, ly1, color, half_len, radius)
   _vkr_store_line_sdf_vertex(p, 3, cx, cy, ux, uy, nx, ny, lx1, ly1, color, half_len, radius)
   _vkr_store_line_sdf_vertex(p, 4, cx, cy, ux, uy, nx, ny, lx1, ly0, color, half_len, radius)
   _vkr_store_line_sdf_vertex(p, 5, cx, cy, ux, uy, nx, ny, lx0, ly0, color, half_len, radius)
}

@jit
fn __vkr_push_rect_sdf(any p, any x, any y, any w, any h, any c, any nx, any ny, any nz) any {
   if !p { return 0 }
   __copy_mem(p, _quad_template, _VKR_VERT_STRIDE * 6)
   def x2, y2 = float(x) + float(w), float(y) + float(h)
   mut bv = p
   store32_f32(bv, float(x), _VKR_OFF_X)
   store32_f32(bv, float(y), _VKR_OFF_Y)
   store32_f32(bv, 0.0, _VKR_OFF_U)
   store32_f32(bv, 0.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, float(x), _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, 0.0, _VKR_OFF_U)
   store32_f32(bv, 1.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, 1.0, _VKR_OFF_U)
   store32_f32(bv, 1.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, 1.0, _VKR_OFF_U)
   store32_f32(bv, 1.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, float(y), _VKR_OFF_Y)
   store32_f32(bv, 1.0, _VKR_OFF_U)
   store32_f32(bv, 0.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, float(x), _VKR_OFF_X)
   store32_f32(bv, float(y), _VKR_OFF_Y)
   store32_f32(bv, 0.0, _VKR_OFF_U)
   store32_f32(bv, 0.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv,
      nz,
   _VKR_OFF_NZ)
}

fn _check_debug_env() any {
   if _cached_ubo_env < 0 {
      case ui_profile.env_lower_cached("NYTRIX_UBO"){
         "1", "true", "on", "yes" -> { _cached_ubo_env = 1 }
         "0", "false", "off", "no" -> { _cached_ubo_env = 2 }
         _ -> { _cached_ubo_env = 0 }
      }
   }
   if _cached_renderdoc_env < 0 { _cached_renderdoc_env = (common.env_present("RENDERDOC") || common.env_present("RENDERDOC_CAPTUREOPTS") || common.env_present("RENDERDOC_CMD")) ? 1 : 0 }
   if ui_profile.debug_enabled() { _debug_gfx_enabled = true }
   if _cached_ubo_env == 1 {
      if ui_profile.env_truthy_cached("NYTRIX_UBO_FORCE") { _ubo_enabled = true } else {
         _ubo_enabled = false
         if _debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] UBO requested but disabled(use NYTRIX_UBO_FORCE=1 to force)") }
      }
   }
   if _cached_ubo_env == 2 { _ubo_enabled = false }
   if _cached_renderdoc_env == 1 { if _debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] RenderDoc detected; bindless remains enabled by design.") } }
}

fn _dbg_handle(any label, any h) int {
   if _debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] " + label + " h=0x" + to_hex(h)) }
   0
}

mut _vkr_pipe_diag_counter = 0

fn _get_vertex_offset() int { _vertex_offset }

fn _get_local_vertex_map() any { _local_vertex_map }

fn _advance_vertex_offset(any bytes) any { _vertex_offset += bytes }

@inline
fn _vkr_bind_dynamic_vertex_buffer(any cb) any {
   if !cb { return 0 }
   if !_dynamic_vbo_bound {
      store64_h(_flush_off, _current_frame_vertex_offset, 0)
      if _vertex_buffer_raw { __copy_mem(_flush_buf, _vertex_buffer_raw, 8) }
      else { store64_h(_flush_buf, _vertex_buffer, 0) }
      cmd_bind_vertex_buffers(cb, 0, 1, _flush_buf, _flush_off)
      _dynamic_vbo_bound = true
   }
}

fn _vkr_pipe_eq(any a, any b) bool { a && b && to_int(a) == to_int(b) }

fn _vkr_pipe_diag(str msg) any {
   if !(ui_profile.env_truthy_cached("NY_GLTF_FORCE_GROUP_DIAG") || ui_profile.env_truthy_cached("NY_VK_PIPE_TRACE")) { return 0 }
   if _vkr_pipe_diag_counter < 24 { ui_profile.print_text("[vk:pipe:diag] " + msg) }
   _vkr_pipe_diag_counter += 1
   0
}

fn _vkr_pipeline_known(any p) bool {
   p && (
      _vkr_pipe_eq(p, _pipeline) ||
      _vkr_pipe_eq(p, _nocull_pipeline) ||
      _vkr_pipe_eq(p, _unlit_pipeline) ||
      _vkr_pipe_eq(p, _unlit_nocull_pipeline) ||
      _vkr_pipe_eq(p, _flip_pipeline) ||
      _vkr_pipe_eq(p, _flip_unlit_pipeline) ||
      _vkr_pipe_eq(p, _line_pipeline) ||
      _vkr_pipe_eq(p, _sdf_line_pipeline) ||
      _vkr_pipe_eq(p, _point_pipeline) ||
      _vkr_pipe_eq(p, _wire_pipeline) ||
      _vkr_pipe_eq(p, _circle_pipeline) ||
      _vkr_pipe_eq(p, _ring_pipeline) ||
      _vkr_pipe_eq(p, _rounded_rect_pipeline) ||
      _vkr_pipe_eq(p, _skybox_pipeline) ||
      _vkr_pipe_eq(p, _mesh_opaque_pipeline) ||
      _vkr_pipe_eq(p, _mesh_opaque_nocull_pipeline) ||
      _vkr_pipe_eq(p, _mesh_opaque_nocull_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_opaque_unlit_pipeline) ||
      _vkr_pipe_eq(p, _mesh_opaque_unlit_nocull_pipeline) ||
      _vkr_pipe_eq(p, _mesh_opaque_unlit_nocull_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_opaque_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_opaque_nocull_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_opaque_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_opaque_nocull_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_env_opaque_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_env_opaque_nocull_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_env_opaque_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_fast_env_opaque_nocull_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_nocull_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_nocull_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_unlit_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_unlit_nocull_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_unlit_nocull_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_flip_pipeline) ||
      _vkr_pipe_eq(p, _mesh_alpha_unlit_flip_pipeline)
   )
}

@inline
fn _vkr_bind_pipeline_if_needed(any cb, any target) bool {
   if !cb || !target { return false }
   _vkr_pipe_diag("before known")
   if !_vkr_pipeline_known(target) {
      ;; Custom pipeline: not in the built-in list but still a valid handle.
      ;; Bind it directly so custom shaders (user pipelines) work natively.
      if _last_bound_pipe != target {
         _vkr_pipe_diag("before cmd bind (custom)")
         cmd_bind_pipeline(cb, 0, target)
         _vkr_pipe_diag("after cmd bind (custom)")
         _last_bound_pipe = target
         _pipeline_bind_count += 1
      }
      return true
   }
   _vkr_pipe_diag("after known")
   if _last_bound_pipe != target {
      _vkr_pipe_diag("before cmd bind")
      cmd_bind_pipeline(cb, 0, target)
      _vkr_pipe_diag("after cmd bind")
      _last_bound_pipe = target
      _pipeline_bind_count += 1
   }
   true
}

fn _vkr_bgra_to_rgba_if_needed(any pixels, int size, int format) any {
   if !pixels || size <= 0 { return 0 }
   if format < 44 || format > 52 { return 0 }
   mut b = 0
   while b < size {
      def blue = load8(pixels, b)
      def red  = load8(pixels, b + 2)
      store8(pixels, red, b)
      store8(pixels, blue, b + 2)
      b += 4
   }
}

@pure
@jit
fn _pack_color(any r, any g, any b, any a) int { render_shared.pack_rgba_u32(r, g, b, a) }

@jit
fn _push_vertex(any x, any y, any z, any u, any v, any r, any g, any b, any a, any tex_id=0) any {
   def off = _local_vertex_map + _vertex_offset
   store32_f32(off, _vkr_safe_f32(x), _VKR_OFF_X)
   store32_f32(off, _vkr_safe_f32(y), _VKR_OFF_Y)
   store32_f32(off, _vkr_safe_f32(z), _VKR_OFF_Z)
   store32_f32(off, _vkr_safe_f32(u), _VKR_OFF_U)
   store32_f32(off, _vkr_safe_f32(v), _VKR_OFF_V)
   store32(off, _pack_color(r, g, b, a), _VKR_OFF_C)
   store32(off, tex_id, _VKR_OFF_TEX)
   store32_f32(off, 0.0, _VKR_OFF_NX)
   store32_f32(off, 0.0, _VKR_OFF_NY)
   store32_f32(off, 1.0, _VKR_OFF_NZ)
   store32_f32(off, 0.0, _VKR_OFF_U2)
   store32_f32(off, 0.0, _VKR_OFF_V2)
   _vertex_offset += _VKR_VERT_STRIDE
}
