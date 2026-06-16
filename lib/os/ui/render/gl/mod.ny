;; Keywords: render opengl gl backend facade
;; References: std.os.ui.render.gl.state std.os.ui.render.gl.texture std.os.ui.render.gl.draw std.os.ui.render.gl.buffer
module std.os.ui.render.gl(init, shutdown, capabilities, begin_frame, end_frame, notify_window_resize, get_swapchain_width, get_swapchain_height, get_swapchain_image_count, set_clear_color, set_next_frame_load_color, clear, clear_depth, set_mvp, set_model_matrix, set_ortho, set_perspective, set_scissor_rect, reset_scissor_rect, set_wireframe, set_mesh_raster_state, renderer_vertex_offset, frame_stats, draw_rect, draw_rect_fast, draw_rect_outline_fast, draw_rects_fast_ptr, draw_lines_2d_fast_ptr, draw_rect_tex, draw_rect_tex_uv, draw_rect_tex_uv_rot, draw_line, draw_line_fast, draw_fan_2d, draw_ring_2d, draw_rounded_rect_2d, draw_vertices, draw_vertices_indexed_raw, draw_lines_raw, draw_points_raw, draw_line_3d, draw_triangle_3d, draw_quad_3d, draw_glyph_bitmap_scaled, set_unlit, set_vertex_color_mode, set_material, set_material_packed, set_material_from_slab, set_material_from_slab_base, set_ui_material, create_texture, create_texture_ex, update_texture_rect, bind_texture, bind_default_texture, destroy_texture, texture_size, texture_format, texture_count, last_created_texture_id, read_framebuffer, create_static_buffer, create_static_index_buffer, create_static_indexed_buffer, destroy_static_buffer, draw_static_buffer, draw_static_buffer_raw, draw_static_buffer_indexed, draw_static_buffer_indexed_raw)
use std.os.ui.render.gl.constants as c
use std.os.ui.render.gl.state as s
use std.os.ui.render.gl.texture as t
use std.os.ui.render.gl.buffer as b
use std.os.ui.render.gl.draw as d

fn init(any win) bool { s.init(win) }

fn shutdown() bool { s.shutdown() }

fn capabilities() dict { s.capabilities() }

fn begin_frame(any win=0, int w=0, int h=0) bool { s.begin_frame(win, w, h) }

fn end_frame() bool { s.end_frame() }

fn notify_window_resize(int w, int h) bool { s.notify_window_resize(w, h) }

fn get_swapchain_width() int { s.get_swapchain_width() }

fn get_swapchain_height() int { s.get_swapchain_height() }

fn get_swapchain_image_count() int { s.get_swapchain_image_count() }

fn set_clear_color(f64 r, f64 g, f64 b, f64 a=1.0) bool { s.set_clear_color(r, g, b, a) }

fn set_next_frame_load_color(any enabled) bool { s.set_next_frame_load_color(enabled) }

fn clear(f64 r, f64 g, f64 b, f64 a=1.0) bool { s.clear(r, g, b, a) }

fn clear_depth() bool { s.clear_depth() }

fn set_mvp(any mat) bool { s.set_mvp(mat) }

fn set_model_matrix(any mat) bool { s.set_model_matrix(mat) }

fn set_ortho(f64 l, f64 r, f64 b, f64 t, f64 n, f64 f) bool { s.set_ortho(l, r, b, t, n, f) }

fn set_perspective(f64 fovy, f64 aspect, f64 near, f64 far) bool { s.set_perspective(fovy, aspect, near, far) }

fn set_scissor_rect(int x, int y, int w, int h) bool { s.set_scissor_rect(x, y, w, h) }

fn reset_scissor_rect() bool { s.reset_scissor_rect() }

fn set_wireframe(bool enabled) bool { s.set_wireframe(enabled) }

fn set_mesh_raster_state(bool nocull=false, bool flip_winding=false) bool { s.set_mesh_raster_state(nocull, flip_winding) }

fn renderer_vertex_offset() int { s.renderer_vertex_offset() }

fn frame_stats() dict { s.frame_stats() }

fn draw_rect(f64 x, f64 y, f64 w, f64 h, f64 r, f64 g, f64 b, f64 a) bool { d.draw_rect(x, y, w, h, r, g, b, a) }

fn draw_rect_fast(f64 x, f64 y, f64 w, f64 h, int color_u32) bool { d.draw_rect_fast(x, y, w, h, color_u32) }

fn draw_rect_outline_fast(f64 x, f64 y, f64 w, f64 h, int color_u32, f64 thickness=1.0) bool { d.draw_rect_outline_fast(x, y, w, h, color_u32, thickness) }

fn draw_rects_fast_ptr(any rects, int count, int stride=20) int { d.draw_rects_fast_ptr(rects, count, stride) }

fn draw_lines_2d_fast_ptr(any lines, int count, int stride=24) int { d.draw_lines_2d_fast_ptr(lines, count, stride) }

fn draw_rect_tex(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 r, f64 g, f64 b, f64 a) bool { d.draw_rect_tex(x, y, w, h, tex_id, r, g, b, a) }

fn draw_rect_tex_uv(f64 x, f64 y, f64 w, f64 h, int tex_id, f64 u1, f64 v1, f64 u2, f64 v2, f64 r, f64 g, f64 b, f64 a) bool { d.draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a) }

fn draw_rect_tex_uv_rot(f64 cx, f64 cy, f64 w, f64 h, f64 rot_deg, int tex_id, f64 u1, f64 v1, f64 u2, f64 v2, f64 r, f64 g, f64 b, f64 a) bool { d.draw_rect_tex_uv_rot(cx, cy, w, h, rot_deg, tex_id, u1, v1, u2, v2, r, g, b, a) }

fn draw_line(f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, f64 r, f64 g, f64 b, f64 a) bool { d.draw_line(x1, y1, x2, y2, thickness, r, g, b, a) }

fn draw_line_fast(f64 x1, f64 y1, f64 x2, f64 y2, f64 thickness, int color_u32) bool { d.draw_line_fast(x1, y1, x2, y2, thickness, color_u32) }

fn draw_fan_2d(f64 cx, f64 cy, f64 rx, f64 ry, int segments, f64 start_rad, f64 span_rad, f64 r, f64 g, f64 b, f64 a) bool { d.draw_fan_2d(cx, cy, rx, ry, segments, start_rad, span_rad, r, g, b, a) }

fn draw_ring_2d(f64 cx, f64 cy, f64 inner_r, f64 outer_r, int segments, f64 r, f64 g, f64 b, f64 a) bool { d.draw_ring_2d(cx, cy, inner_r, outer_r, segments, r, g, b, a) }

fn draw_rounded_rect_2d(f64 x, f64 y, f64 w, f64 h, f64 radius, int segments, f64 r, f64 g, f64 b, f64 a) bool { d.draw_rounded_rect_2d(x, y, w, h, radius, segments, r, g, b, a) }

fn draw_vertices(any p, int count, int tex_id=-1, bool use_material=false) bool { d.draw_vertices(p, count, tex_id, use_material) }

fn draw_vertices_indexed_raw(any p, int count, any idx_buf, any idx_offset, int idx_count, int index_type=0, int tex_id=-1, bool is_lines=false, f64 width=1.0, any _pipe_override=0, bool is_points=false, bool use_material=false) bool { d.draw_vertices_indexed_raw(p, count, idx_buf, idx_offset, idx_count, index_type, tex_id, is_lines, width, _pipe_override, is_points, use_material) }

fn draw_lines_raw(any p, int line_count, f64 width=1.0, bool use_material=false) bool { d.draw_lines_raw(p, line_count, width, use_material) }

fn draw_points_raw(any p, int point_count, int tex_id=-1, bool use_material=false) bool { d.draw_points_raw(p, point_count, tex_id, use_material) }

fn draw_line_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 thickness, f64 r, f64 g, f64 b, f64 a) bool { d.draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, r, g, b, a) }

fn draw_triangle_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 x3, f64 y3, f64 z3, f64 r, f64 g, f64 b, f64 a) bool { d.draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a) }

fn draw_quad_3d(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2, f64 x3, f64 y3, f64 z3, f64 x4, f64 y4, f64 z4, f64 r, f64 g, f64 b, f64 a) bool { d.draw_quad_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a) }

fn draw_glyph_bitmap_scaled(ptr data, int src_w, int src_h, int dst_w, int dst_h, f64 ox, f64 oy, f64 r, f64 g, f64 b, f64 a, int bpp=4, bool is_color=false) bool { d.draw_glyph_bitmap_scaled(data, src_w, src_h, dst_w, dst_h, ox, oy, r, g, b, a, bpp, is_color) }

fn set_unlit(any enabled) any { s.set_unlit(enabled) }

fn set_vertex_color_mode(int mode) any { s.set_vertex_color_mode(mode) }

fn set_material(any base_color, any metallic, any roughness) any { s.set_material(base_color, metallic, roughness) }

fn set_material_packed(int base_color_u32, int material_u32, int emissive_u32 = 0, int emissive_tex_id = -1, int emissive_uv_set = 0, int base_tex_id = -1, int alpha_u32 = 0, int occlusion_tex_id = -1, int occlusion_uv_set = 0, int bsdf0_u32 = 0, int bsdf1_u32 = 0, int bsdf2_u32 = 0, int bsdf3_u32 = 0, int bsdf4_u32 = 0, int bsdf5_u32 = 0, int base_uv_xf0 = 0, int base_uv_xf1 = 0, int normal_uv_xf0 = 0, int normal_uv_xf1 = 0, int mr_uv_xf0 = 0, int mr_uv_xf1 = 0, int occlusion_uv_xf0 = 0, int occlusion_uv_xf1 = 0, int emissive_uv_xf0 = 0, int emissive_uv_xf1 = 0, int normal_tex_id = -1, int ext2_tex_word = 0x80000000, int vc_mode = 0) any { s.set_material_packed(base_color_u32, material_u32, emissive_u32, emissive_tex_id, emissive_uv_set, base_tex_id, alpha_u32, occlusion_tex_id, occlusion_uv_set, bsdf0_u32, bsdf1_u32, bsdf2_u32, bsdf3_u32, bsdf4_u32, bsdf5_u32, base_uv_xf0, base_uv_xf1, normal_uv_xf0, normal_uv_xf1, mr_uv_xf0, mr_uv_xf1, occlusion_uv_xf0, occlusion_uv_xf1, emissive_uv_xf0, emissive_uv_xf1, normal_tex_id, ext2_tex_word, vc_mode) }

fn set_material_from_slab(?ptr p, int vc_mode=0) any { s.set_material_from_slab(p, vc_mode) }

fn set_material_from_slab_base(?ptr p, int fallback_base_tex_id=-1, int vc_mode=0) any { s.set_material_from_slab_base(p, fallback_base_tex_id, vc_mode) }

fn set_ui_material(int base_tex_id=-1, int alpha_u32=0, int vc_mode=12) any { s.set_ui_material(base_tex_id, alpha_u32, vc_mode) }

fn create_texture(int width, int height, any pixels) int { t.create_texture(width, height, pixels) }

fn create_texture_ex(int width, int height, any pixels, int format=37, int filter=1, int wrap_s=GL_REPEAT, int wrap_t=GL_REPEAT, bool use_mipmaps=false, int _upload_prebaked_bytes=0) int { t.create_texture_ex(width, height, pixels, format, filter, wrap_s, wrap_t, use_mipmaps, _upload_prebaked_bytes) }

fn update_texture_rect(int tex_id, int x, int y, int w, int h, any pixels) bool { t.update_texture_rect(tex_id, x, y, w, h, pixels) }

fn bind_texture(int tex_id) bool { s.bind_texture(tex_id) }

fn bind_default_texture() bool { s.bind_default_texture() }

fn destroy_texture(int tex_id) bool { t.destroy_texture(tex_id) }

fn texture_size(int tex_id) list { t.texture_size(tex_id) }

fn texture_format(int tex_id) int { t.texture_format(tex_id) }

fn texture_count() int { t.texture_count() }

fn last_created_texture_id() int { t.last_created_texture_id() }

fn read_framebuffer() any { t.read_framebuffer() }

fn create_static_buffer(?ptr src_ptr, int count) any { b.create_static_buffer(src_ptr, count) }

fn create_static_index_buffer(?ptr idx_ptr, int idx_count, bool use_u32=false) any { b.create_static_index_buffer(idx_ptr, idx_count, use_u32) }

fn create_static_indexed_buffer(?ptr vert_ptr, int count, ?ptr idx_ptr, int idx_count, any opts=0) any { b.create_static_indexed_buffer(vert_ptr, count, idx_ptr, idx_count, opts) }

fn destroy_static_buffer(any sbuf) bool { b.destroy_static_buffer(sbuf) }

fn draw_static_buffer(dict sbuf, bool is_lines=false, f64 width=1.0, any pipe_override=0, bool is_points=false, bool use_material=false) bool { b.draw_static_buffer(sbuf, is_lines, width, pipe_override, is_points, use_material) }

fn draw_static_buffer_raw(any buf, any voff, int count, bool is_lines=false, f64 width=1.0, any _pipe_override=0, bool is_points=false, bool use_material=false) bool { b.draw_static_buffer_raw(buf, voff, count, is_lines, width, _pipe_override, is_points, use_material) }

fn draw_static_buffer_indexed(dict sbuf, any idx_buf, int index_count, bool is_lines=false, f64 width=1.0, any pipe_override=0, bool is_points=false, bool use_material=false) bool { b.draw_static_buffer_indexed(sbuf, idx_buf, index_count, is_lines, width, pipe_override, is_points, use_material) }

fn draw_static_buffer_indexed_raw(any buf, any voff, any idx_buf, any ioff, int index_count, bool is_lines=false, f64 width=1.0, any _pipe_override=0, int index_type=0, bool is_points=false, bool use_material=false) bool { b.draw_static_buffer_indexed_raw(buf, voff, idx_buf, ioff, index_count, is_lines, width, _pipe_override, index_type, is_points, use_material) }
