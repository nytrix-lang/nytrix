;; Keywords: render vulkan gpu font truetype buffers compute state texture pipeline draw utils os ui
;; Vulkan renderer facade for swapchains, pipelines, textures, buffers, fonts, and draw calls.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk(init, shutdown, wait_idle, begin_frame, end_frame, clear, clear_depth, draw_rect, draw_rect_tex, draw_rect_tex_uv, draw_rect_tex_uv_rot, _vk_draw_shader_rect, draw_line, draw_line_fast, draw_glyph, create_texture, create_texture_ex, create_cubemap, draw_skybox, update_texture_rect, bind_texture, bind_default_texture, texture_size, texture_format, texture_descriptor, destroy_texture, read_framebuffer, set_texture_debug_meta, _draw_triangle_2d, last_created_texture_id, texture_count, draw_triangle_3d, draw_quad_3d, draw_rect_lines_2d, draw_chamfer_rect_2d, draw_rounded_rect_2d, draw_rounded_rect_sdf, draw_fan_2d, draw_ellipse_lines_2d, draw_arc_2d, draw_sector_2d, draw_star_2d, draw_vertices, draw_vertices_indexed_raw, draw_lines_raw, draw_points_raw, set_mvp, set_ortho, _pack_color, _flush, _update_default_mvp, set_frame_time_sec, renderer_config, _get_local_vertex_map, _get_vertex_offset, _advance_vertex_offset, __vkr_push_vertex, __vkr_push_rect_tex, __vkr_draw_text, _vkr_glyph_get_off, set_mask, create_static_buffer, create_static_index_buffer, create_static_indexed_buffer, destroy_static_buffer, draw_static_buffer, draw_static_buffer_raw, draw_static_buffer_indexed, draw_static_buffer_indexed_raw, draw_parts_flat, draw_parts_flat_range, draw_parts_flat_range_no_restore, draw_parts_flat_range_state_no_restore, draw_part_flat_no_restore, draw_part0_flat_no_restore, draw_part0_flat_state_no_restore, _mvp_matrix, _get_vertex_stride, set_unlit, set_double_sided, set_vertex_color_mode, set_material, set_material_packed, set_material_from_slab, set_cam_pos, set_env_tex, set_env_spec_tex, set_scene_lights, set_scene_lights_slab, set_next_frame_load_color, capture_scene_color_resume_pass, clear_scene_color_capture, request_frame_capture, draw_circle_sdf, draw_ring_sdf, _vkr_glyph_present, draw_rect_fast, draw_rect_outline_fast, draw_rects_fast_ptr, draw_lines_2d_fast_ptr, draw_text_batch, draw_text_runs, draw_text_runs_ptr, draw_text_runs_flat_ptr, draw_text_runs_flat_color_ptr, set_scissor_rect, reset_scissor_rect, set_wireframe, set_model_matrix, set_perspective, set_clear_color, blit_buffer, compile_glsl_to_spirv, create_shader_module_from_source, create_pipeline, bind_pipeline, push_constants, _get_default_pipeline, _get_nocull_pipeline, _get_unlit_nocull_pipeline, _get_flip_pipeline, _get_flip_unlit_pipeline, _get_mesh_opaque_pipeline, _get_mesh_opaque_nocull_pipeline, _get_mesh_opaque_nocull_flip_pipeline, _get_mesh_opaque_unlit_pipeline, _get_mesh_opaque_unlit_nocull_pipeline, _get_mesh_opaque_unlit_nocull_flip_pipeline, _get_mesh_fast_opaque_pipeline, _get_mesh_fast_opaque_nocull_pipeline, _get_mesh_fast_opaque_flip_pipeline, _get_mesh_fast_opaque_nocull_flip_pipeline, _get_mesh_fast_env_opaque_pipeline, _get_mesh_fast_env_opaque_nocull_pipeline, _get_mesh_fast_env_opaque_flip_pipeline, _get_mesh_fast_env_opaque_nocull_flip_pipeline, _get_mesh_alpha_pipeline, _get_mesh_alpha_nocull_pipeline, _get_mesh_alpha_nocull_flip_pipeline, _get_mesh_alpha_unlit_pipeline, _get_mesh_alpha_unlit_nocull_pipeline, _get_mesh_alpha_unlit_nocull_flip_pipeline, _get_mesh_alpha_flip_pipeline, _get_mesh_alpha_unlit_flip_pipeline, notify_window_resize, get_swapchain_size, get_swapchain_width, get_swapchain_height, get_swapchain_image_count, _get_device, gltf_pass_plan, gltf_material_layout, gltf_descriptor_plan, gltf_gbuffer_layout, gltf_shader_defines, gltf_pipeline_key, gltf_variant_key, gltf_alpha_sort_key, gltf_unwired_checklist, compute_caps, gltf_material_compute_shader, speculative_gi_probe_shader, ibl_prefilter_shader, brdf_lut_shader, transmission_blur_shader, material_ext_resolve_shader, refraction_resolve_shader, compute_pass_desc, compute_feature_mask, COMPUTE_FEATURE_REFRACTION, COMPUTE_FEATURE_TRANSMISSION, COMPUTE_FEATURE_VOLUME, COMPUTE_FEATURE_DIFFUSE_TRANSMISSION, COMPUTE_FEATURE_SPECULAR_GI, COMPUTE_FEATURE_MESH_INSTANCING, CULL_PART_STRIDE, CULL_PART_VIS_OFF, CULL_PART_AABB_OFF, cull_extract_frustum_planes, cull_aabb_visible, cull_write_visibility_slab, cull_stats, cull_shader, INDIRECT_DRAW_BYTES, INDIRECT_CMD_BYTES, indirect_make_key, indirect_count_groups, indirect_build_groups, indirect_write_draw_cmd, indirect_write_draw_indexed_cmd, indirect_group_command_bytes, indirect_write_group_cmds, indirect_shader_prepare, tex_job_make, tex_job_queue_make, tex_job_queue_pop, tex_job_result_make, tex_job_cache_key, tex_job_worker_plan, tex_job_upload_plan, debug_stage)
use std.os.ui.render.vk.renderer as vk_renderer
use std.os.ui.render.vk.draw as vk_draw
use std.os.ui.render.vk.font as vk_font
use std.os.ui.render.vk.texture as vk_texture
use std.os.ui.render.vk.buffers as vk_buffers
use std.os.ui.render.vk.utils as vk_utils
use std.os.ui.render.vk.pipeline as vk_pipeline
use std.os.ui.render.vk.compute as vk_compute
use std.core (free)
use std.os.ui.render.vk.vulkan (destroy_buffer, free_memory, device_wait_idle)
use std.os.ui.render.vk.state as vk_state

comptime template _vk_passthrough0(ns, name){
   fn ${name}() any { ${ns}.${name}() }
}

comptime template _vk_passthrough0_doc(ns, name, doc){
   fn ${name}() any { ${ns}.${name}() }
}

comptime template _vk_passthrough0_ret(ns, name, ret){
   fn ${name}() ${ret} { ${ns}.${name}() }
}

comptime template _vk_passthrough1(ns, name, t1, a1){
   fn ${name}(${t1} ${a1}) any { ${ns}.${name}(${a1}) }
}

comptime template _vk_passthrough1_doc(ns, name, t1, a1, doc){
   fn ${name}(${t1} ${a1}) any { ${ns}.${name}(${a1}) }
}

comptime template _vk_passthrough1_any(ns, name, _a1){
   fn ${name}(any v) any { ${ns}.${name}(v) }
}

comptime template _vk_passthrough1_any_doc(ns, name, _a1, doc){
   fn ${name}(any v) any { ${ns}.${name}(v) }
}

comptime template _vk_passthrough1_any_ret(ns, name, _a1, ret){
   fn ${name}(any v) ${ret} { ${ns}.${name}(v) }
}

comptime template _vk_passthrough2_any(ns, name, _a1, _a2){
   fn ${name}(any v1, any v2) any { ${ns}.${name}(v1, v2) }
}

comptime template _vk_renderer_slab_range(name){ fn ${name}(?ptr slab_ptr, int start_idx, int end_idx, int pass_num) int { vk_renderer.${name}(slab_ptr, start_idx, end_idx, pass_num) } }

comptime template _vk_renderer_slab_idx(name){ fn ${name}(?ptr slab_ptr, int idx, int pass_num) int { vk_renderer.${name}(slab_ptr, idx, pass_num) } }

comptime template _vk_renderer_slab_only(name){ fn ${name}(?ptr slab_ptr) int { vk_renderer.${name}(slab_ptr) } }

comptime template _vk_renderer_slab_count(name){ fn ${name}(?ptr slab_ptr, int count) any { vk_renderer.${name}(slab_ptr, count) } }
fn init(any win) bool {
   "Initializes the Vulkan backend for the given window."
   vk_renderer.init(win)
}

fn shutdown() any {
   "Shuts down the Vulkan backend and releases all resources."
   vk_renderer.shutdown()
}

fn wait_idle() bool {
   "Blocks until the Vulkan device has finished in-flight work."
   def dev = vk_state._device
   if !dev { return true }
   device_wait_idle(dev) == 0
}

fn begin_frame() bool {
   "Begins a new Vulkan rendering frame."
   vk_renderer.begin_frame()
}

fn debug_stage() str {
   "Runs the debug stage operation."
   vk_renderer.debug_stage()
}

fn end_frame() any {
   "Finalizes and presents the current Vulkan rendering frame."
   vk_renderer.end_frame()
}

fn notify_window_resize(int w, int h) bool {
   "Notifies the renderer of a WM-driven window resize."
   vk_renderer.notify_window_resize(w, h)
}

fn get_swapchain_size() any {
   "Returns [w, h] of the current swapchain extent."
   vk_renderer.get_swapchain_size()
}

comptime emit _vk_passthrough0_ret(vk_renderer, get_swapchain_width, int)
comptime emit _vk_passthrough0_ret(vk_renderer, get_swapchain_height, int)
comptime emit _vk_passthrough0_ret(vk_renderer, get_swapchain_image_count, int)

fn _get_device() any { vk_state._device }

fn _get_vertex_stride() int { vk_state.VERTEX_STRIDE }
comptime emit _vk_passthrough1_any(vk_renderer, set_env_tex, tex_id)
comptime emit _vk_passthrough1_any(vk_renderer, set_env_spec_tex, tex_id)
comptime emit _vk_passthrough1_any(vk_renderer, set_next_frame_load_color, enabled)
comptime emit _vk_passthrough1_any(vk_renderer, set_frame_time_sec, t)

fn draw_parts_flat(?ptr slab_ptr, int count, int pass_num) int { vk_renderer.draw_parts_flat(slab_ptr, count, pass_num) }
comptime emit _vk_renderer_slab_range(draw_parts_flat_range)
comptime emit _vk_renderer_slab_range(draw_parts_flat_range_no_restore)
comptime emit _vk_renderer_slab_range(draw_parts_flat_range_state_no_restore)
comptime emit _vk_renderer_slab_idx(draw_part_flat_no_restore)
comptime emit _vk_renderer_slab_only(draw_part0_flat_no_restore)
comptime emit _vk_renderer_slab_only(draw_part0_flat_state_no_restore)
comptime emit _vk_passthrough0(vk_renderer, capture_scene_color_resume_pass)
comptime emit _vk_passthrough0(vk_renderer, clear_scene_color_capture)
comptime emit _vk_passthrough0(vk_renderer, request_frame_capture)

fn clear(f64 r, f64 g, f64 b, f64 a) any {
   "Clears the active framebuffer with the specified color."
   vk_renderer.clear(r, g, b, a)
}

fn clear_depth() any {
   "Clears the depth buffer."
   vk_renderer.clear_depth()
}

fn draw_rect(f64 x, f64 y, f64 w, f64 h, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a filled rectangle using the current vk_pipeline."
   vk_draw.draw_rect(x, y, w, h, r, g, b, a)
}

fn draw_rect_tex(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a textured rectangle."
   vk_draw.draw_rect_tex(x, y, w, h, tex_id, r, g, b, a)
}

fn draw_rect_tex_uv(f64 x,
   f64 y,
   f64 w,
   f64 h,
   int tex_id,
   f64 u1,
   f64 v1,
   f64 u2,
   f64 v2,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a textured rectangle with custom UV coordinates."
   vk_draw.draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a)
}

fn draw_rect_tex_uv_rot(f64 cx,
   f64 cy,
   f64 w,
   f64 h,
   f64 rot_deg,
   int tex_id,
   f64 u1,
   f64 v1,
   f64 u2,
   f64 v2,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a rotated textured rectangle around center with custom UV coordinates."
   vk_draw.draw_rect_tex_uv_rot(cx, cy, w, h, rot_deg, tex_id, u1, v1, u2, v2, r, g, b, a)
}

fn draw_line(f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a 2D line."
   vk_draw.draw_line(x1, y1, x2, y2, thickness, r, g, b, a)
}

fn draw_line_fast(f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, int color_u32) any {
   "Draws a packed-color 2D line."
   vk_draw.draw_line_fast(x1, y1, x2, y2, thickness, color_u32)
}

fn draw_glyph(f64 x,
   f64 y,
   f64 w,
   f64 h,
   f64 u1,
   f64 v1,
   f64 u2,
   f64 v2,
   int tex_id,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a single font glyph."
   vk_draw.draw_glyph(x, y, w, h, u1, v1, u2, v2, tex_id, r, g, b, a)
}

fn draw_rect_fast(f64 x, f64 y, f64 w, f64 h, int color_u32) any {
   "Submits a direct-color rectangle."
   vk_draw.draw_rect_fast(x, y, w, h, color_u32)
}

fn draw_rect_outline_fast(f64 x, f64 y, f64 w, f64 h, int color_u32) any {
   "Submits a direct-color rectangle outline."
   vk_draw.draw_rect_outline_fast(x, y, w, h, color_u32)
}

fn draw_rects_fast_ptr(any rects, int count, int stride = 20) int {
   "Submits packed rect records."
   vk_draw.draw_rects_fast_ptr(rects, count, stride)
}

fn draw_lines_2d_fast_ptr(any lines, int count, int stride = 24) int {
   "Submits packed 2D line records."
   vk_draw.draw_lines_2d_fast_ptr(lines, count, stride)
}

fn draw_vertices(any p, int count, int tex_id) bool {
   "Submits raw vertices for drawing."
   vk_draw.draw_vertices(p, count, tex_id)
}

fn draw_vertices_indexed_raw(any p,
   int count,
   any idx_buf,
   any ioff,
   int index_count,
   int index_type = 0,
   int tex_id = -1,
   bool is_lines = false,
   f64 width = 1.0,
   any pipe_override = 0,
   bool is_points = false) bool {
   "Submits dynamic vertices with a GPU index buffer."
   vk_draw.draw_vertices_indexed_raw(p,
      count,
      idx_buf,
      ioff,
      index_count,
      index_type,
      tex_id,
      is_lines,
      width,
      pipe_override,
   is_points)
}

fn draw_lines_raw(any p, int line_count, f64 _line_width) bool {
   "Submits raw lines for drawing."
   vk_draw.draw_lines_raw(p, line_count, _line_width)
}

fn draw_points_raw(any p, int point_count, int tex_id = -1) bool {
   "Submits raw points for drawing."
   vk_draw.draw_points_raw(p, point_count, tex_id)
}

fn _draw_triangle_2d(f64 x1, f64 y1, f64 x2, f64 y2, f64 x3, f64 y3, f64 r, f64 g, f64 b, f64 a) any { vk_draw._draw_triangle_2d(x1, y1, x2, y2, x3, y3, r, g, b, a) }

fn draw_triangle_3d(f64 x1,
   f64 y1,
   f64 z1,
   f64 x2,
   f64 y2,
   f64 z2,
   f64 x3,
   f64 y3,
   f64 z3,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a 3D triangle."
   vk_draw.draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a)
}

fn draw_quad_3d(f64 x1,
   f64 y1,
   f64 z1,
   f64 x2,
   f64 y2,
   f64 z2,
   f64 x3,
   f64 y3,
   f64 z3,
   f64 x4,
   f64 y4,
   f64 z4,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a 3D quad."
   vk_draw.draw_quad_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a)
}

fn draw_rect_lines_2d(f64 x, f64 y, f64 w, f64 h, f64 thickness, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a rectangle outline."
   vk_draw.draw_rect_lines_2d(x, y, w, h, thickness, r, g, b, a)
}

fn draw_chamfer_rect_2d(f64 x, f64 y, f64 w, f64 h, f64 rad, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a chamfered rectangle."
   vk_draw.draw_chamfer_rect_2d(x, y, w, h, rad, r, g, b, a)
}

fn draw_rounded_rect_2d(f64 x, f64 y, f64 w, f64 h, f64 radius, int segments, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a rounded rectangle."
   vk_draw.draw_rounded_rect_2d(x, y, w, h, radius, segments, r, g, b, a)
}

fn draw_rounded_rect_sdf(f64 x, f64 y, f64 w, f64 h, f64 radius, f64 r, f64 g, f64 b, f64 a) bool {
   "Draws a smooth SDF rounded rectangle."
   vk_draw.draw_rounded_rect_sdf(x, y, w, h, radius, r, g, b, a)
}

fn draw_fan_2d(f64 cx,
   f64 cy,
   f64 rx,
   f64 ry,
   int segments,
   f64 start_rad,
   f64 span_rad,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a filled 2D fan."
   vk_draw.draw_fan_2d(cx, cy, rx, ry, segments, start_rad, span_rad, r, g, b, a)
}

fn draw_ellipse_lines_2d(f64 cx,
   f64 cy,
   f64 rx,
   f64 ry,
   f64 thickness,
   int segments,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws an outlined ellipse."
   vk_draw.draw_ellipse_lines_2d(cx, cy, rx, ry, thickness, segments, r, g, b, a)
}

fn draw_arc_2d(f64 cx,
   f64 cy,
   f64 radius,
   f64 start_rad,
   f64 span_rad,
   f64 thickness,
   int steps,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws an arc outline."
   vk_draw.draw_arc_2d(cx, cy, radius, start_rad, span_rad, thickness, steps, r, g, b, a)
}

fn draw_sector_2d(f64 cx,
   f64 cy,
   f64 inner_radius,
   f64 outer_radius,
   f64 start_rad,
   f64 span_rad,
   int steps,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a filled sector."
   vk_draw.draw_sector_2d(cx, cy, inner_radius, outer_radius, start_rad, span_rad, steps, r, g, b, a)
}

fn draw_star_2d(f64 cx,
   f64 cy,
   f64 inner_radius,
   f64 outer_radius,
   int pts,
   f64 rotation_rad,
   f64 r,
   f64 g,
   f64 b,
   f64 a) any {
   "Draws a filled star."
   vk_draw.draw_star_2d(cx, cy, inner_radius, outer_radius, pts, rotation_rad, r, g, b, a)
}

fn draw_line_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 thickness, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a 3D line."
   vk_draw.draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, r, g, b, a)
}

fn draw_grid_3d(f64 size, f64 step) any {
   "Draws a 3D ground grid."
   vk_draw.draw_grid_3d(size, step)
}

fn draw_axes_3d(f64 gizmo_len, f64 cube_sz = 0.4) any {
   "Draws 3D coordinate axes."
   vk_draw.draw_axes_3d(gizmo_len, cube_sz)
}

fn draw_cube_3d(f64 x, f64 y, f64 z, f64 size, f64 r, f64 g = 1.0, f64 b = 1.0, f64 a = 1.0, int tex_id = -1) any {
   "Draws a 3D cube."
   vk_draw.draw_cube_3d(x, y, z, size, 0.0, 0.0, 0.0, r, g, b, a, tex_id)
}

fn draw_line_strip_2d(f64 x, f64 y, f64 w, f64 h, list history, f64 scale, f64 r, f64 g, f64 b, f64 a) any {
   "Draws a 2D line strip(graph)."
   vk_draw.draw_line_strip_2d(x, y, w, h, history, scale, r, g, b, a)
}

fn draw_static_buffer(any sbuf, bool is_lines = false, f64 width = 1.0, any pipe_override = 0, bool is_points = false) bool {
   "Draws vertices from a static GPU buffer."
   vk_draw.draw_static_buffer(sbuf, is_lines, width, pipe_override, is_points)
}

fn draw_static_buffer_raw(any buf,
   any voff,
   int count,
   bool is_lines = false,
   f64 width = 1.0,
   any pipe_override = 0,
   bool is_points = false) bool {
   "Draws vertices from a static GPU buffer using raw handles."
   vk_draw.draw_static_buffer_raw(buf, voff, count, is_lines, width, pipe_override, is_points)
}

fn draw_static_buffer_indexed(any sbuf,
   any idx_buf,
   int index_count,
   bool is_lines = false,
   f64 width = 1.0,
   any pipe_override = 0,
   bool is_points = false) bool {
   "Draws indexed vertices from a static GPU buffer."
   vk_draw.draw_static_buffer_indexed(sbuf, idx_buf, index_count, is_lines, width, pipe_override, is_points)
}

fn draw_static_buffer_indexed_raw(any buf,
   any voff,
   any idx_buf,
   any ioff,
   int index_count,
   bool is_lines = false,
   f64 width = 1.0,
   any pipe_override = 0,
   int index_type = 0,
   bool is_points = false) bool {
   "Draws indexed vertices from a static GPU buffer using raw handles."
   vk_draw.draw_static_buffer_indexed_raw(buf,
      voff,
      idx_buf,
      ioff,
      index_count,
      is_lines,
      width,
      pipe_override,
      index_type,
   is_points)
}

fn draw_text_batch(int font_id, any lines, f64 x, f64 y, f64 spacing, int color_u32) any {
   "Draws multiple lines of text efficiently."
   vk_font.draw_text_batch(font_id, lines, x, y, spacing, color_u32)
}

fn draw_text_runs(int font_id, any runs, int color_u32) any {
   "Draws arbitrary text runs efficiently."
   vk_font.draw_text_runs(font_id, runs, color_u32)
}

fn draw_text_runs_ptr(int font_id, any runs, int color_u32, any glyphs_ptr, f64 ascent, f64 line_h) any {
   "Draws text runs with a pre-resolved glyph table."
   vk_font.draw_text_runs_ptr(font_id, runs, color_u32, glyphs_ptr, ascent, line_h)
}

fn draw_text_runs_flat_ptr(int font_id, any runs, int color_u32, any glyphs_ptr, f64 ascent, f64 line_h) any {
   "Draws flat text runs with a pre-resolved glyph table."
   vk_font.draw_text_runs_flat_ptr(font_id, runs, color_u32, glyphs_ptr, ascent, line_h)
}

fn draw_text_runs_flat_color_ptr(int font_id, any runs, any glyphs_ptr, f64 ascent, f64 line_h) any {
   "Draws flat per-run-color text runs with a pre-resolved glyph table."
   vk_font.draw_text_runs_flat_color_ptr(font_id, runs, glyphs_ptr, ascent, line_h)
}

fn set_scissor_rect(int x, int y, int w, int h) any {
   "Applies a Vulkan scissor rectangle for subsequent draws."
   vk_renderer.set_scissor_rect(x, y, w, h)
}

fn reset_scissor_rect() any {
   "Restores the full-frame Vulkan scissor rectangle."
   vk_renderer.reset_scissor_rect()
}

fn create_texture(int width, int height, any pixels) int {
   "Creates a GPU texture from raw RGBA8 pixels."
   if !_device { return -1 }
   def raw = vk_texture.create_texture(width, height, pixels)
   if raw >= 0 && raw < vk_state.MAX_TEXTURES { return raw }
   -1
}

fn create_texture_ex(int width, int height, any pixels, int format = 37, any filter = -1, any wrap_s = 10497, any wrap_t = 10497, bool use_mipmaps = false, int prebaked_mip_bytes = 0) int {
   "Creates a GPU texture with a specific format and optional sampler filter override."
   if !_device { return -1 }
   def raw = vk_texture.create_texture_ex(width, height, pixels, format, filter, wrap_s, wrap_t, use_mipmaps, prebaked_mip_bytes)
   if raw >= 0 && raw < vk_state.MAX_TEXTURES { return raw }
   -1
}

fn last_created_texture_id() int { return vk_texture.last_created_texture_id() }

fn texture_count() int { return vk_texture.texture_count() }

fn create_cubemap(int face_size, list face_pixels_list) int {
   "Creates a cubemap texture from 6 RGBA8 faces."
   _device ? vk_texture.create_cubemap(face_size, face_pixels_list) : -1
}

fn draw_skybox(int tex_id) bool {
   "Draws the current environment skybox."
   _device ? vk_texture.draw_skybox(tex_id) : false
}

fn update_texture_rect(int tex_id, int x, int y, int w, int h, any pixels) bool {
   "Updates a sub-region of an existing GPU vk_texture."
   _device && tex_id >= 0 ? vk_texture.update_texture_rect(tex_id, x, y, w, h, pixels) : false
}

fn bind_texture(int tex_id) any {
   "Binds a texture for subsequent draw calls."
   if _device && tex_id >= 0 { return vk_texture.bind_texture(tex_id) }
   0
}

fn bind_default_texture() any {
   "Restores the default white texture binding."
   vk_texture.bind_default_texture()
}

fn texture_size(int tex_id) any {
   "Returns [width, height] for the given vk_texture."
   _device ? vk_texture.texture_size(tex_id) : 0
}

fn texture_format(int tex_id) int {
   "Returns the Vulkan format of the vk_texture."
   _device ? vk_texture.texture_format(tex_id) : 0
}

fn texture_descriptor(int tex_id) any {
   "Returns the descriptor set handle for the vk_texture."
   _device ? vk_texture.texture_descriptor(tex_id) : 0
}

fn destroy_texture(int tex_id) any {
   "Releases GPU memory associated with a vk_texture."
   if _device && tex_id >= 0 { return vk_texture.destroy_texture(tex_id) }
   0
}

fn read_framebuffer() any {
   "Reads back the current framebuffer into a host-visible image dictionary."
   _device ? vk_texture.read_framebuffer() : 0
}

fn set_texture_debug_meta(int tex_id, str path = "", str cache_key = "") bool {
   "Attaches debug source metadata to a live texture slot."
   _device ? vk_texture.set_texture_debug_meta(tex_id, path, cache_key) : false
}

fn blit_buffer(any pixels, int w, int h) any {
   "Blits raw pixels directly to the swapchain(for software rendering)."
   if _device { return vk_texture.blit_buffer(pixels, w, h) }
   0
}

fn renderer_config(any vsync, any filter, any vert_spv_path, any frag_spv_path, any msaa) any {
   "Standardizes renderer global configuration."
   vk_renderer.renderer_config(vsync, filter, vert_spv_path, frag_spv_path, msaa)
}

fn _get_local_vertex_map() any { vk_state._local_vertex_map }

fn _get_vertex_offset() int { vk_state._vertex_offset }

fn _advance_vertex_offset(int n) int { vk_state._advance_vertex_offset(n) }

fn __vkr_push_vertex(any p,
   f64 x,
   f64 y,
   f64 z,
   f64 u,
   f64 v,
   any color,
   int tex_id = 0,
   f64 nx = 0.0,
   f64 ny = 0.0,
   f64 nz = 1.0) any {
   "Internal: appends a single vertex to the command stream."
   vk_utils.__vkr_push_vertex(p, x, y, z, u, v, color, tex_id, nx, ny, nz)
}

fn __vkr_push_rect_tex(any p,
   f64 x,
   f64 y,
   f64 w,
   f64 h,
   f64 u1,
   f64 v1,
   f64 u2,
   f64 v2,
   any color,
   int tex_id = 0,
   f64 nz = 1.0) any {
   "Internal: appends a textured rectangle(6 vertices) to the command stream."
   vk_utils.__vkr_push_rect_tex(p, x, y, w, h, u1, v1, u2, v2, color, tex_id, nz)
}

fn __vkr_draw_text_glyph(any g_ptr, any v, any x, any y, any cp, any color, any tid) any { vk_font.__vkr_draw_text_glyph(g_ptr, v, x, y, cp, color, tid) }

fn __vkr_draw_text(any font_id, any text, any x, any y, any color, any gptr, any ascent, any lh, any info) any { vk_font.__vkr_draw_text(font_id, text, x, y, color, gptr, ascent, lh, info) }

fn _vkr_glyph_get_off(any glyphs_ptr, any cp) any { vk_font._vkr_glyph_get_off(glyphs_ptr, cp) }

fn _vkr_glyph_present(any glyphs_ptr, any cp) any { vk_font._vkr_glyph_present(glyphs_ptr, cp) }

fn create_static_buffer(any p, any count) any {
   "Creates an immutable GPU buffer from host memory."
   _device ? vk_buffers.create_static_buffer(p, count) : 0
}

fn create_static_index_buffer(any idx_ptr, any idx_count, bool use_u32=false) any {
   "Creates an immutable GPU index buffer from host memory."
   _device ? vk_buffers.create_static_index_buffer(idx_ptr, idx_count, use_u32) : 0
}

fn create_static_indexed_buffer(any p, any count, any idx_ptr, any idx_count, any opts=0) any {
   "Creates an immutable indexed GPU buffer from host memory."
   _device ? vk_buffers.create_static_indexed_buffer(p, count, idx_ptr, idx_count, opts) : 0
}

fn destroy_static_buffer(any sbuf) any {
   "Releases a static GPU vertex buffer created by `create_static_buffer`."
   if !is_dict(sbuf) { return 0 }
   if sbuf.get("shared", false) { return 0 }
   def buf = sbuf.get("handle", 0)
   def mem = sbuf.get("memory", 0)
   if buf { destroy_buffer(_device, buf, 0) }
   if mem { free_memory(_device, mem, 0) }
   def ibuf = sbuf.get("ibuf", 0)
   def imem = sbuf.get("imemory", 0)
   if ibuf { destroy_buffer(_device, ibuf, 0) }
   if imem { free_memory(_device, imem, 0) }
   def cpu = sbuf.get("cpu_ptr", 0)
   def cpu_idx = sbuf.get("cpu_idx_ptr", 0)
   if cpu { free(cpu) }
   if cpu_idx { free(cpu_idx) }
   0
}

fn _mvp_matrix() any { vk_renderer._mvp_matrix() }
comptime emit _vk_passthrough1_any(vk_renderer, set_scene_lights, lights)
comptime emit _vk_renderer_slab_count(set_scene_lights_slab)

fn set_unlit(any unlit) any {
   "Enables or disables simple unlit rendering mode."
   vk_renderer.set_unlit(unlit)
}

fn set_double_sided(bool enabled) any {
   "Marks subsequent draws as true double-sided lighting, not just no-cull."
   vk_renderer.set_double_sided(enabled)
}

fn set_vertex_color_mode(int mode) any {
   "Sets vertex-color usage mode for subsequent draws."
   vk_renderer.set_vertex_color_mode(mode)
}

fn set_mask(int m) any {
   "Marks mask mode for subsequent draws without forcing an immediate flush."
   vk_renderer.set_mask(m)
}

fn set_material(any base_color, f64 metallic, f64 roughness) any {
   "Sets PBR material parameters."
   vk_renderer.set_material(base_color, metallic, roughness)
}

fn set_material_packed(int base_color_u32, int material_u32, int emissive_u32 = 0,
   int emissive_tex_id = -1, int emissive_uv_set = 0, int base_tex_id = -1,
   int alpha_u32 = 0, int occlusion_tex_id = -1, int occlusion_uv_set = 0,
   int bsdf0_u32 = 0, int bsdf1_u32 = 0, int bsdf2_u32 = 0,
   int bsdf3_u32 = 0, int bsdf4_u32 = 0, int bsdf5_u32 = 0,
   int base_uv_xf0 = 0, int base_uv_xf1 = 0, int normal_uv_xf0 = 0,
   int normal_uv_xf1 = 0, int mr_uv_xf0 = 0, int mr_uv_xf1 = 0,
   int occlusion_uv_xf0 = 0, int occlusion_uv_xf1 = 0, int emissive_uv_xf0 = 0,
   int emissive_uv_xf1 = 0, int normal_tex_id = -1, int ext2_tex_word = 0x80000000,
   int vc_mode = 0) any {
   "Sets packed base-color/material words."
   vk_renderer.set_material_packed(base_color_u32,
      material_u32,
      emissive_u32,
      emissive_tex_id,
      emissive_uv_set,
      base_tex_id,
      alpha_u32,
      occlusion_tex_id,
      occlusion_uv_set,
      bsdf0_u32,
      bsdf1_u32,
      bsdf2_u32,
      bsdf3_u32,
      bsdf4_u32,
      bsdf5_u32,
      base_uv_xf0,
      base_uv_xf1,
      normal_uv_xf0,
      normal_uv_xf1,
      mr_uv_xf0,
      mr_uv_xf1,
      occlusion_uv_xf0,
      occlusion_uv_xf1,
      emissive_uv_xf0,
      emissive_uv_xf1,
      normal_tex_id,
      ext2_tex_word,
   vc_mode)
}

fn set_material_from_slab(?ptr p, int vc_mode = 0) any {
   "Sets packed material state from a native material slab."
   vk_renderer.set_material_from_slab(p, vc_mode)
}

fn set_cam_pos(f64 x, f64 y, f64 z) any {
   "Sets camera world-space position for PBR specular."
   vk_renderer.set_cam_pos(x, y, z)
}

fn set_wireframe(bool enabled) any {
   "Enables or disables wireframe rasterization."
   vk_renderer.set_wireframe(enabled)
}

fn set_model_matrix(any mat) any {
   "Sets the global 4x4 Model matrix."
   vk_renderer.set_model_matrix(mat)
}

fn set_mvp(any mat) any {
   "Sets the global 4x4 View-Projection matrix."
   vk_renderer.set_mvp(mat)
}

fn set_ortho(f64 l, f64 r, f64 b, f64 t, f64 n, f64 f) any {
   "Applies an orthographic projection matrix."
   vk_renderer.set_ortho(l, r, b, t, n, f)
}

fn set_perspective(f64 fovy, f64 aspect, f64 near, f64 far) any {
   "Applies a perspective projection matrix."
   vk_renderer.set_perspective(fovy, aspect, near, far)
}

fn set_clear_color(f64 r, f64 g, f64 b, f64 a=1.0) any {
   "Sets the background clear color."
   vk_renderer.set_clear_color(r, g, b, a)
}

fn _pack_color(f64 r, f64 g, f64 b, f64 a) int { vk_utils._pack_color(r, g, b, a) }

fn _flush() any { vk_renderer._flush() }

fn _update_default_mvp(any win) any { vk_renderer._update_default_mvp(win) }

fn compile_glsl_to_spirv(any source, any stage_ext) any {
   "Compiles GLSL source string to SPIR-V."
   vk_pipeline.compile_glsl_to_spirv(source, stage_ext)
}

fn create_shader_module_from_source(any source, any stage_ext) any {
   "Compiles GLSL and creates a Vulkan shader module."
   vk_pipeline.create_shader_module_from_source(source, stage_ext)
}

fn create_pipeline(any vert_mod,
   any frag_mod,
   any topology=3,
   any depth_test=1,
   any depth_write=1,
   any cull_mode=0,
   any front_face=0,
   any depth_bias=0,
   any depth_clamp=0) any {
   "Creates a customizable graphics pipeline object."
   vk_pipeline.create_pipeline(vert_mod,
      frag_mod,
      topology,
      depth_test,
      depth_write,
      cull_mode,
      front_face,
      depth_bias,
   depth_clamp)
}

fn bind_pipeline(any pipe) any {
   "Activates a graphics pipeline for subsequent draw calls."
   vk_pipeline.bind_pipeline(pipe)
}

fn push_constants(any p, any size, any offset=0) any {
   "Updates push constant data on the current command buffer."
   vk_pipeline.push_constants(p, size, offset)
}

fn use_custom_push_constants(any enabled) any {
   "Enables/disables custom push constant mode for custom pipelines."
   vk_pipeline.use_custom_push_constants(enabled)
}

fn set_custom_push_constants(any p, any size, any offset=0) any {
   "Sets custom push constant data(call after bind_pipeline with custom pipeline)."
   vk_pipeline.set_custom_push_constants(p, size, offset)
}

comptime template _vk_pipeline_getter_passthrough(name){
   fn ${name}() any { vk_pipeline.${name}() }
}

comptime template _vk_pipeline_getter_passthrough_doc(name, doc){
   fn ${name}() any { vk_pipeline.${name}() }
}

comptime template _vk_emit_pipeline_getter_triplet(a, b, c){
   comptime emit _vk_pipeline_getter_passthrough(a)
   comptime emit _vk_pipeline_getter_passthrough(b)
   comptime emit _vk_pipeline_getter_passthrough(c)
}

comptime template _vk_emit_pipeline_getter_single(name){
   comptime emit _vk_pipeline_getter_passthrough(name)
}

comptime emit _vk_pipeline_getter_passthrough_doc(_get_default_pipeline, "Internal: returns the standard 2D renderer vk_pipeline.")
comptime emit _vk_pipeline_getter_passthrough_doc(_get_nocull_pipeline, "Internal: returns the built-in lit no-cull pipeline.")
comptime emit _vk_pipeline_getter_passthrough_doc(_get_unlit_nocull_pipeline, "Internal: returns the built-in unlit no-cull pipeline.")
comptime emit _vk_pipeline_getter_passthrough_doc(_get_flip_pipeline, "Internal: returns the built-in lit flipped-winding pipeline.")
comptime emit _vk_pipeline_getter_passthrough_doc(_get_flip_unlit_pipeline, "Internal: returns the built-in unlit flipped-winding pipeline.")
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_opaque_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_opaque_nocull_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_opaque_nocull_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_opaque_unlit_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_opaque_unlit_nocull_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_opaque_unlit_nocull_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_opaque_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_opaque_nocull_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_opaque_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_opaque_nocull_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_env_opaque_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_env_opaque_nocull_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_env_opaque_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_fast_env_opaque_nocull_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_nocull_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_nocull_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_unlit_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_unlit_nocull_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_unlit_nocull_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_flip_pipeline)
comptime emit _vk_pipeline_getter_passthrough(_get_mesh_alpha_unlit_flip_pipeline)

fn draw_circle_sdf(f64 x, f64 y, f64 radius, f64 r, f64 g, f64 b, f64 a) bool { vk_draw.draw_circle_sdf(x, y, radius, r, g, b, a) }

fn draw_ring_sdf(f64 x, f64 y, f64 inner_radius, f64 outer_radius, f64 r, f64 g, f64 b, f64 a) bool { vk_draw.draw_ring_sdf(x, y, inner_radius, outer_radius, r, g, b, a) }
def GLTF_PASS_OPAQUE = 0
def GLTF_PASS_ALPHA_MASK = 1
def GLTF_PASS_TRANSMISSIVE = 2
def GLTF_PASS_REFRACTIVE = 3
def GLTF_PASS_TRANSPARENT = 4
def GLTF_PASS_COMPUTE_GI = 5
def GLTF_BINDING_SCENE_COLOR = 0
def GLTF_BINDING_SCENE_DEPTH = 1
def GLTF_BINDING_NORMAL_ROUGHNESS = 2
def GLTF_BINDING_MATERIAL_EXT = 3
def COMPUTE_FEATURE_REFRACTION = 1
def COMPUTE_FEATURE_TRANSMISSION = 2
def COMPUTE_FEATURE_VOLUME = 4
def COMPUTE_FEATURE_DIFFUSE_TRANSMISSION = 8
def COMPUTE_FEATURE_SPECULAR_GI = 16
def COMPUTE_FEATURE_MESH_INSTANCING = 32
def CULL_PART_STRIDE = 256
def CULL_PART_VIS_OFF = 228
def CULL_PART_AABB_OFF = 232
def INDIRECT_DRAW_BYTES = 16
def INDIRECT_CMD_BYTES = 20

fn gltf_pass_plan(int feature_mask, str alpha_mode="OPAQUE") list {
   "Runs the pass plan operation."
   mut passes = list(0)
   def am = to_str(alpha_mode)
   if am == "BLEND" {
      if (feature_mask & 128) != 0 || (feature_mask & 8192) != 0 { passes = passes.append(GLTF_PASS_TRANSMISSIVE) }
      if (feature_mask & 32768) != 0 { passes = passes.append(GLTF_PASS_REFRACTIVE) }
      passes = passes.append(GLTF_PASS_TRANSPARENT)
      return passes
   }
   if am == "MASK" { passes = passes.append(GLTF_PASS_ALPHA_MASK) }
   else { passes = passes.append(GLTF_PASS_OPAQUE) }
   if (feature_mask & COMPUTE_FEATURE_SPECULAR_GI) != 0 || (feature_mask & COMPUTE_FEATURE_TRANSMISSION) != 0 || (feature_mask & 32768) != 0 { passes = passes.append(GLTF_PASS_COMPUTE_GI) }
   passes
}

fn gltf_material_layout() dict {
   "Runs the material layout operation."
   {
      "base_material_bytes": 152,
      "ext_sidecar_bytes": 256,
      "off_feature_mask": 0,
      "off_bsdf0": 4,
      "off_bsdf1": 8,
      "off_bsdf2": 12,
      "off_bsdf3": 16,
      "off_bsdf4": 20,
      "off_bsdf5": 24,
      "off_ior": 28,
      "off_alpha_cutoff": 32,
      "off_clearcoat": 36,
      "off_anisotropy": 44,
      "off_transmission": 52,
      "off_volume": 64,
      "off_iridescence": 88,
      "off_specular": 112,
      "off_sheen": 136,
      "off_diffuse_transmission": 160,
      "off_refraction": 184,
      "off_subsurface": 208
   }
}

fn gltf_gbuffer_layout() list {
   "Runs the gbuffer layout operation."
   [
      {"name": "scene_color", "format": "rgba16f", "binding": GLTF_BINDING_SCENE_COLOR},
      {"name": "normal_roughness", "format": "rgba16f", "binding": GLTF_BINDING_NORMAL_ROUGHNESS},
      {"name": "material_ext", "format": "rgba16f", "binding": GLTF_BINDING_MATERIAL_EXT},
      {"name": "depth", "format": "d32", "binding": GLTF_BINDING_SCENE_DEPTH}
   ]
}

fn gltf_descriptor_plan() dict {
   "Runs the descriptor plan operation."
   {
      "set_scene": 0,
      "set_materials": 1,
      "set_textures": 2,
      "set_compute": 3,
      "material_ssbo_binding": 0,
      "texture_array_binding": 1,
      "sampler_array_binding": 2,
      "instance_ssbo_binding": 3,
      "indirect_ssbo_binding": 4
   }
}

fn gltf_shader_defines(int feature_mask) str {
   "Runs the shader defines operation."
   mut s = "#define NY_GLTF_FULL_PBR 1\n"
   if (feature_mask & 64) != 0 { s = s + "#define NY_GLTF_CLEARCOAT 1\n" }
   if (feature_mask & COMPUTE_FEATURE_TRANSMISSION) != 0 { s = s + "#define NY_GLTF_TRANSMISSION 1\n" }
   if (feature_mask & COMPUTE_FEATURE_VOLUME) != 0 { s = s + "#define NY_GLTF_VOLUME 1\n" }
   if (feature_mask & 1024) != 0 { s = s + "#define NY_GLTF_IRIDESCENCE 1\n" }
   if (feature_mask & 2048) != 0 { s = s + "#define NY_GLTF_ANISOTROPY 1\n" }
   if (feature_mask & COMPUTE_FEATURE_DIFFUSE_TRANSMISSION) != 0 { s = s + "#define NY_GLTF_DIFFUSE_TRANSMISSION 1\n" }
   if (feature_mask & 32768) != 0 { s = s + "#define NY_GLTF_REFRACTION 1\n" }
   if (feature_mask & 65536) != 0 { s = s + "#define NY_GLTF_SUBSURFACE 1\n" }
   s
}

fn gltf_pipeline_key(str alpha_mode, bool double_sided=false, bool unlit=false, int feature_mask=0) str {
   "Runs the pipeline key operation."
   to_str(alpha_mode) + "|ds=" + to_str(double_sided ? 1 : 0) + "|unlit=" + to_str(unlit ? 1 : 0) + "|f=" + to_str(int(feature_mask))
}

fn gltf_variant_key(int mesh_idx, int prim_idx, int variant_idx) str {
   "Runs the variant key operation."
   to_str(int(mesh_idx)) + ":" + to_str(int(prim_idx)) + ":" + to_str(int(variant_idx))
}

fn gltf_alpha_sort_key(f64 depth, int material_idx, int part_idx) dict {
   "Runs the alpha sort key operation."
   {"depth": float(depth), "material": int(material_idx), "part": int(part_idx)}
}

fn gltf_unwired_checklist() list {
   "Runs the unwired checklist operation."
   [
      "Bind material extension sidecar SSBO after pack_material_slab.",
      "Emit normal/roughness/materialExt G-buffer when transmission/refraction/volume bits are present.",
      "Run refraction/transmission compute after opaque scene color, before transparent sorted pass.",
      "Resolve KHR_materials_variants into active material index before GPU part upload.",
      "Expand EXT_mesh_gpu_instancing into instance SSBO and indirect draw records.",
      "Apply KHR_animation_pointer to material/texture transform fields before per-frame material upload.",
      "Run frustum culling into GPU part visibility byte before indirect preparation."
   ]
}

comptime emit _vk_passthrough0(vk_compute, compute_caps)
comptime emit _vk_passthrough1_any_ret(vk_compute, compute_feature_mask, mat_info, int)

fn compute_pass_desc(str name, str shader, int local_x=8, int local_y=8, int local_z=1) dict { vk_compute.compute_pass_desc(name, shader, local_x, local_y, local_z) }
comptime emit _vk_passthrough0_ret(vk_compute, gltf_material_compute_shader, str)
comptime emit _vk_passthrough0_ret(vk_compute, refraction_resolve_shader, str)
comptime emit _vk_passthrough0_ret(vk_compute, speculative_gi_probe_shader, str)
comptime emit _vk_passthrough0_ret(vk_compute, ibl_prefilter_shader, str)
comptime emit _vk_passthrough0_ret(vk_compute, brdf_lut_shader, str)
comptime emit _vk_passthrough0_ret(vk_compute, transmission_blur_shader, str)
comptime emit _vk_passthrough0_ret(vk_compute, material_ext_resolve_shader, str)

fn indirect_make_key(any index_buf, int index_off, any vertex_buf, int vertex_off, int material, any pipeline, int index_count) str {
   "Runs the indirect make key operation."
   to_str(index_buf) + "|" + to_str(index_off) + "|" + to_str(vertex_buf) + "|" + to_str(vertex_off) + "|" + to_str(material) + "|" + to_str(pipeline) + "|" + to_str(index_count)
}

fn indirect_write_draw_indexed_cmd(?ptr p, int off, int index_count, int instance_count, int first_index, int vertex_offset, int first_instance) bool {
   "Runs the indirect write draw indexed cmd operation."
   if !p { return false }
   store32(p, int(index_count), off + 0)
   store32(p, int(instance_count), off + 4)
   store32(p, int(first_index), off + 8)
   store32(p, int(vertex_offset), off + 12)
   store32(p, int(first_instance), off + 16)
   true
}

fn indirect_write_draw_cmd(?ptr p, int off, int vertex_count, int instance_count, int first_vertex, int first_instance) bool {
   "Runs the indirect write draw cmd operation."
   if !p { return false }
   store32(p, int(vertex_count), off + 0)
   store32(p, int(instance_count), off + 4)
   store32(p, int(first_vertex), off + 8)
   store32(p, int(first_instance), off + 12)
   true
}

fn indirect_group_command_bytes(bool indexed=true) int {
   "Runs the indirect group command bytes operation."
   indexed ? INDIRECT_CMD_BYTES : INDIRECT_DRAW_BYTES
}

fn indirect_write_group_cmds(?ptr out, any groups, bool indexed=true) int {
   "Runs the indirect write group cmds operation."
   if !out || !is_list(groups) { return 0 }
   def stride = indirect_group_command_bytes(indexed)
   mut i = 0
   while i < groups.len {
      def g = groups.get(i, 0)
      if is_dict(g) {
         if indexed {
            indirect_write_draw_indexed_cmd(out, i * stride,
               int(g.get("index_count", 0)),
               int(g.get("instance_count", 0)),
               int(g.get("first_index", 0)),
               int(g.get("vertex_offset", 0)),
            int(g.get("first_instance", 0)))
         } else {
            indirect_write_draw_cmd(out, i * stride,
               int(g.get("vertex_count", g.get("index_count", 0))),
               int(g.get("instance_count", 0)),
               int(g.get("first_vertex", g.get("vertex_offset", 0))),
            int(g.get("first_instance", 0)))
         }
      }
      i += 1
   }
   groups.len
}

fn indirect_count_groups(list parts) int {
   "Runs the indirect count groups operation."
   if !is_list(parts) { return 0 }
   mut keys = dict(128)
   mut n = 0
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def p = parts.get(i, 0)
      if is_dict(p) {
         def k = indirect_make_key(p.get("idx_buf",0), p.get("idx_off",0), p.get("vbuf",0), p.get("voff",0), p.get("material",-1), p.get("pipeline",0), p.get("index_count",0))
         if !keys.contains(k) {
            keys = keys.set(k, true)
            n += 1
         }
      }
      i += 1
   }
   n
}

fn indirect_build_groups(list parts) list {
   "Runs the indirect build groups operation."
   if !is_list(parts) { return [] }
   mut map = dict(128)
   mut groups = list(0)
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def p = parts.get(i, 0)
      if is_dict(p) {
         def k = indirect_make_key(p.get("idx_buf",0), p.get("idx_off",0), p.get("vbuf",0), p.get("voff",0), p.get("material",-1), p.get("pipeline",0), p.get("index_count",0))
         mut gi = int(map.get(k, -1))
         if gi < 0 {
            gi = groups.len
            map = map.set(k, gi)
            def g = {
               "key": k,
               "first_part": i,
               "instance_count": 0,
               "index_count": int(p.get("index_count",0)),
               "vertex_count": int(p.get("vertex_count", p.get("count", p.get("index_count",0)))),
               "first_index": int(p.get("first_index",0)),
               "first_vertex": int(p.get("first_vertex", p.get("vertex_offset",0))),
               "vertex_offset": int(p.get("vertex_offset",0)),
               "first_instance": 0,
               "material": int(p.get("material",-1))
            }
            groups = groups.append(g)
         }
         mut old = groups.get(gi, 0)
         if is_dict(old) {
            old = old.set("instance_count", int(old.get("instance_count",0)) + 1)
            groups.set(gi, old)
         }
      }
      i += 1
   }
   mut first_instance = 0
   mut j = 0
   while j < groups.len {
      mut g = groups.get(j, 0)
      if is_dict(g) {
         g = g.set("first_instance", first_instance)
         first_instance += int(g.get("instance_count", 0))
         groups.set(j, g)
      }
      j += 1
   }
   groups
}

fn indirect_shader_prepare() str {
   "
   #version 450
   layout(local_size_x=64) in ;
   struct DrawIndexedIndirect { uint indexCount ; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
   layout(std430,binding=0) readonly buffer PartWords { uint partWords[] ; };
   layout(std430,binding=1) buffer Draws { DrawIndexedIndirect draws[] ; };
   layout(std430,binding=2) buffer InstanceIds { uint instanceIds[] ; };
   layout(push_constant) uniform PC { uint partCount ; uint drawCount; uint strideWords; uint visWord; uint groupWord; uint mode; } pc;
   void main(){
   uint i=gl_GlobalInvocationID.x ;
   if pc.mode == 0u {
   if i < pc.drawCount { draws[i].instanceCount = 0u ; }
   return ;
   }
   if i >= pc.partCount || pc.strideWords == 0u { return ; }
   uint base = i * pc.strideWords ;
   if partWords[base + pc.visWord] == 0u { return ; }
   uint group = partWords[base + pc.groupWord] ;
   if group >= pc.drawCount { return ; }
   uint slot = atomicAdd(draws[group].instanceCount, 1u) ;
   instanceIds[draws[group].firstInstance + slot] = i ;
   }
   "
}

fn cull_extract_frustum_planes(?ptr mvp_slab) list {
   "Runs the cull extract frustum planes operation."
   if !mvp_slab { return [] }
   mut p = list(0)
   def m00, m01 = load32_f32(mvp_slab, 0), load32_f32(mvp_slab, 4)
   def m02, m03 = load32_f32(mvp_slab, 8), load32_f32(mvp_slab, 12)
   def m10, m11 = load32_f32(mvp_slab, 16), load32_f32(mvp_slab, 20)
   def m12, m13 = load32_f32(mvp_slab, 24), load32_f32(mvp_slab, 28)
   def m20, m21 = load32_f32(mvp_slab, 32), load32_f32(mvp_slab, 36)
   def m22, m23 = load32_f32(mvp_slab, 40), load32_f32(mvp_slab, 44)
   def m30, m31 = load32_f32(mvp_slab, 48), load32_f32(mvp_slab, 52)
   def m32, m33 = load32_f32(mvp_slab, 56), load32_f32(mvp_slab, 60)
   p = p.append([m30 + m00, m31 + m01, m32 + m02, m33 + m03])
   p = p.append([m30 - m00, m31 - m01, m32 - m02, m33 - m03])
   p = p.append([m30 + m10, m31 + m11, m32 + m12, m33 + m13])
   p = p.append([m30 - m10, m31 - m11, m32 - m12, m33 - m13])
   p = p.append([m30 + m20, m31 + m21, m32 + m22, m33 + m23])
   p = p.append([m30 - m20, m31 - m21, m32 - m22, m33 - m23])
   p
}

fn cull_aabb_visible(any planes, f64 minx, f64 miny, f64 minz, f64 maxx, f64 maxy, f64 maxz) bool {
   "Returns whether cull aabb visible."
   mut i = 0
   while is_list(planes) && i < planes.len {
      def pl = planes.get(i, 0)
      def a = float(pl.get(0, 0.0))
      def b = float(pl.get(1, 0.0))
      def c = float(pl.get(2, 0.0))
      def d = float(pl.get(3, 0.0))
      def px = a >= 0.0 ? maxx : minx
      def py = b >= 0.0 ? maxy : miny
      def pz = c >= 0.0 ? maxz : minz
      if a * px + b * py + c * pz + d < 0.0 { return false }
      i += 1
   }
   true
}

fn cull_write_visibility_slab(?ptr parts_slab, int count, any planes, int stride = CULL_PART_STRIDE, int vis_off = CULL_PART_VIS_OFF, int aabb_off = CULL_PART_AABB_OFF) int {
   "Runs the cull write visibility slab operation."
   if !parts_slab || count <= 0 { return 0 }
   mut visible = 0
   mut i = 0
   while i < count {
      def base = parts_slab + i * stride
      def minx = load32_f32(base, aabb_off + 0)
      def miny = load32_f32(base, aabb_off + 4)
      def minz = load32_f32(base, aabb_off + 8)
      def maxx = load32_f32(base, aabb_off + 12)
      def maxy = load32_f32(base, aabb_off + 16)
      def maxz = load32_f32(base, aabb_off + 20)
      def ok = cull_aabb_visible(planes, minx, miny, minz, maxx, maxy, maxz)
      store32(base, ok ? 1 : 0, vis_off)
      if ok { visible += 1 }
      i += 1
   }
   visible
}

fn cull_stats(int total, int visible) dict {
   "Runs the cull stats operation."
   {"total": total, "visible": visible, "culled": total - visible}
}

fn cull_shader() str {
   "#version 450
   layout(local_size_x=64) in ;
   layout(std430,binding=0) buffer PartWords { uint partWords[] ; };
   layout(std430,binding=1) readonly buffer Planes { vec4 planes[6] ; };
   layout(push_constant) uniform PC { uint count ; uint strideWords; uint visWord; uint aabbWord; } pc;
   bool visibleAabb(vec3 mn, vec3 mx){
   for int i=0 ;i<6;i++{
   vec4 p=planes[i] ; vec3 v=vec3(p.x>=0?mx.x:mn.x,p.y>=0?mx.y:mn.y,p.z>=0?mx.z:mn.z); if dot(p.xyz,v)+p.w<0 return false;
   }
   return true ;
   }
   float f32(uint word){ return uintBitsToFloat(word) ; }
   void main(){
   uint i=gl_GlobalInvocationID.x ;
   if i>=pc.count || pc.strideWords == 0u return ;
   uint b=i*pc.strideWords ;
   vec3 mn=vec3(f32(partWords[b+pc.aabbWord+0u]), f32(partWords[b+pc.aabbWord+1u]), f32(partWords[b+pc.aabbWord+2u])) ;
   vec3 mx=vec3(f32(partWords[b+pc.aabbWord+3u]), f32(partWords[b+pc.aabbWord+4u]), f32(partWords[b+pc.aabbWord+5u])) ;
   bool v=visibleAabb(mn,mx) ;
   partWords[b+pc.visWord]=v?1u:0u ;
   }
   "
}

fn tex_job_make(int index, any uri, any mime = "", any sampler = 0, int material = -1, any slot = "") dict { vk_texture.tex_job_make(index, uri, mime, sampler, material, slot) }
comptime emit _vk_passthrough0_ret(vk_texture, tex_job_queue_make, dict)
comptime emit _vk_passthrough2_any(vk_texture, tex_job_queue_push, q, job)

fn tex_job_queue_pop(any q) any { vk_texture.tex_job_queue_pop(q) }

fn tex_job_result_make(any job, int width, int height, any rgba_or_mips, bool ok = true, any err = "") dict { vk_texture.tex_job_result_make(job, width, height, rgba_or_mips, ok, err) }

fn tex_job_cache_key(str uri, str mime = "", int flags = 0) str { vk_texture.tex_job_cache_key(uri, mime, flags) }

fn tex_job_worker_plan(int worker_count = 4) dict { vk_texture.tex_job_worker_plan(worker_count) }
comptime emit _vk_passthrough1_any(vk_texture, tex_job_upload_plan, results)

fn _vk_draw_shader_rect(x, y, w, h, pipe, pc, pcs, pco) any { vk_draw._vk_draw_shader_rect(x, y, w, h, pipe, pc, pcs, pco) }
