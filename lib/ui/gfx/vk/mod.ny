;; Keywords: ui gfx vulkan renderer
;; Vulkan 2D Renderer for Nytrix (modularized)

module std.ui.gfx.vk (
   init, shutdown,
   begin_frame, end_frame,
   clear, clear_depth,
   draw_rect, draw_rect_tex, draw_rect_tex_uv, draw_line, draw_glyph,
   draw_rectangle_fast,
   create_texture, create_texture_ex, update_texture_rect, bind_texture, bind_default_texture, texture_size, texture_format, texture_descriptor, destroy_texture, read_framebuffer,
   _draw_triangle_2d, draw_triangle_3d, draw_quad_3d, draw_vertices, draw_lines_raw,
   set_mvp, set_ortho, _pack_color, _flush, _update_default_mvp,
   renderer_config, _get_local_vertex_map, _get_vertex_offset, _advance_vertex_offset,
   __vkr_push_vertex, __vkr_push_rect_tex, __vkr_draw_text, _vkr_glyph_get_off,
   create_static_buffer, destroy_static_buffer, draw_static_buffer,
   _mvp_matrix, VERTEX_STRIDE, set_unlit,
   draw_circle_sdf, draw_ring_sdf,
   _vkr_glyph_present, _prof_flush_avg,
   draw_rect_fast, draw_text_batch,
   set_wireframe,
   set_model_matrix, set_perspective, set_clear_color,
   blit_buffer,
   compile_glsl_to_spirv, create_shader_module_from_source, create_pipeline, bind_pipeline, push_constants, _get_default_pipeline, _get_nocull_pipeline,
   notify_window_resize, get_swapchain_size, _get_device
)

use std.ui.gfx.vk.renderer as vk_renderer
use std.ui.gfx.vk.draw as vk_draw
use std.ui.gfx.vk.font as vk_font
use std.ui.gfx.vk.texture as vk_texture
use std.ui.gfx.vk.buffers as vk_buffers
use std.ui.gfx.vk.utils as vk_utils
use std.ui.gfx.vk.pipeline as vk_pipeline
use std.ui.gfx.vk.vulkan (destroy_buffer, free_memory)
use std.ui.gfx.vk.state (VERTEX_STRIDE, _prof_flush_avg, _device)

fn init(win){ "Initializes the Vulkan backend for the given window." vk_renderer.init(win) }
fn shutdown(){ "Shuts down the Vulkan backend and releases all resources." vk_renderer.shutdown() }
fn begin_frame(){ "Begins a new Vulkan rendering frame." vk_renderer.begin_frame() }
fn end_frame(){ "Finalizes and presents the current Vulkan rendering frame." vk_renderer.end_frame() }
fn notify_window_resize(w, h){ "Notifies the renderer of a WM-driven window resize." vk_renderer.notify_window_resize(w, h) }
fn get_swapchain_size(){ "Returns [w, h] of the current swapchain extent." vk_renderer.get_swapchain_size() }
fn clear(r, g, b, a){ "Clears the active framebuffer with the specified color." vk_renderer.clear(r, g, b, a) }
fn _get_device(){ vk_renderer._get_device() }
fn clear_depth(){ "Clears the depth buffer." vk_renderer.clear_depth() }

fn draw_rect(x, y, w, h, r, g, b, a){ "Draws a filled rectangle using the current vk_pipeline." vk_draw.draw_rect(x, y, w, h, r, g, b, a) }
fn draw_rect_tex(x, y, w, h, tex_id, r, g, b, a){ "Draws a textured rectangle." vk_draw.draw_rect_tex(x, y, w, h, tex_id, r, g, b, a) }
fn draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a){ "Draws a textured rectangle with custom UV coordinates." vk_draw.draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a) }
fn draw_line(x1, y1, x2, y2, thickness, r, g, b, a){ "Draws a 2D line." vk_draw.draw_line(x1, y1, x2, y2, thickness, r, g, b, a) }
fn draw_glyph(x, y, w, h, u1, v1, u2, v2, tex_id, r, g, b, a){ "Draws a single font glyph." vk_draw.draw_glyph(x, y, w, h, u1, v1, u2, v2, tex_id, r, g, b, a) }
fn draw_rectangle_fast(x, y, w, h, color_packed){ "Fast rectangle drawing path." vk_draw.draw_rectangle_fast(x, y, w, h, color_packed) }
fn draw_rect_fast(x, y, w, h, color_u32){ "Submits a direct-color rectangle." vk_draw.draw_rect_fast(x, y, w, h, color_u32) }
fn draw_vertices(ptr, count, tex_id){ "Submits raw vertices for drawing." vk_draw.draw_vertices(ptr, count, tex_id) }
fn draw_lines_raw(ptr, line_count, _line_width){ "Submits raw lines for drawing." vk_draw.draw_lines_raw(ptr, line_count, _line_width) }
fn _draw_triangle_2d(x1, y1, x2, y2, x3, y3, r, g, b, a){ "Internal: draws a 2D triangle." vk_draw._draw_triangle_2d(x1, y1, x2, y2, x3, y3, r, g, b, a) }
fn draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a){ "Draws a 3D triangle." vk_draw.draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a) }
fn draw_quad_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a){ "Draws a 3D quad." vk_draw.draw_quad_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b, a) }
fn draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, r, g, b, a){ "Draws a 3D line." vk_draw.draw_line_3d(x1, y1, z1, x2, y2, z2, thickness, r, g, b, a) }
fn draw_grid_3d(size, step){ "Draws a 3D ground grid." vk_draw.draw_grid_3d(size, step) }
fn draw_axes_3d(size){ "Draws 3D coordinate axes." vk_draw.draw_axes_3d(size) }
fn draw_cube_3d(x, y, z, size, r, g=1.0, b=1.0, a=1.0, tex_id=-1){ "Draws a 3D cube." vk_draw.draw_cube_3d(x, y, z, size, r, g, b, a, tex_id) }
fn draw_line_strip_2d(x, y, w, h, history, scale, r, g, b, a){ "Draws a 2D line strip (graph)." vk_draw.draw_line_strip_2d(x, y, w, h, history, scale, r, g, b, a) }
fn draw_static_buffer(sbuf, is_lines=false, width=1.0){ "Draws vertices from a static GPU buffer." vk_draw.draw_static_buffer(sbuf, is_lines, width) }
fn draw_text_batch(font_id, lines, x, y, spacing, color_u32){ "Draws multiple lines of text efficiently." vk_font.draw_text_batch(font_id, lines, x, y, spacing, color_u32) }

fn create_texture(width, height, pixels){ "Creates a GPU texture from raw RGBA8 pixels." vk_texture.create_texture(width, height, pixels) }
fn create_texture_ex(width, height, pixels, format=37){ "Creates a GPU texture with a specific format." vk_texture.create_texture_ex(width, height, pixels, format) }
fn update_texture_rect(tex_id, x, y, w, h, pixels){ "Updates a sub-region of an existing GPU vk_texture." vk_texture.update_texture_rect(tex_id, x, y, w, h, pixels) }
fn bind_texture(tex_id){ "Binds a texture for subsequent draw calls." vk_texture.bind_texture(tex_id) }
fn bind_default_texture(){ "Restores the default white texture binding." vk_texture.bind_default_texture() }
fn texture_size(tex_id){ "Returns [width, height] for the given vk_texture." vk_texture.texture_size(tex_id) }
fn texture_format(tex_id){ "Returns the Vulkan format of the vk_texture." vk_texture.texture_format(tex_id) }
fn texture_descriptor(tex_id){ "Returns the descriptor set handle for the vk_texture." vk_texture.texture_descriptor(tex_id) }
fn destroy_texture(tex_id){ "Releases GPU memory associated with a vk_texture." vk_texture.destroy_texture(tex_id) }
fn read_framebuffer(){ "Reads back the current framebuffer into a host-visible image dictionary." vk_texture.read_framebuffer() }
fn blit_buffer(pixels, w, h){ "Blits raw pixels directly to the swapchain (for software rendering)." vk_texture.blit_buffer(pixels, w, h) }

fn renderer_config(vsync, filter, vert_spv_path, frag_spv_path, msaa){ "Standardizes renderer global configuration." vk_renderer.renderer_config(vsync, filter, vert_spv_path, frag_spv_path, msaa) }
fn _get_local_vertex_map(){ "Internal: returns the current vertex pointer." vk_utils._get_local_vertex_map() }
fn _get_vertex_offset(){ "Internal: returns the current byte offset in the vertex buffer." vk_utils._get_vertex_offset() }
fn _advance_vertex_offset(bytes){ "Internal: increments the vertex buffer offset." vk_utils._advance_vertex_offset(bytes) }
fn __vkr_push_vertex(ptr, x, y, z, u, v, color, tex_id=0, nx=0.0, ny=0.0, nz=1.0){ "Internal: appends a single vertex to the command stream." vk_utils.__vkr_push_vertex(ptr, x, y, z, u, v, color, tex_id, nx, ny, nz) }
fn __vkr_push_rect_tex(ptr, x, y, w, h, u1, v1, u2, v2, color, tex_id=0, nz=1.0){ "Internal: appends a textured rectangle (6 vertices) to the command stream." vk_utils.__vkr_push_rect_tex(ptr, x, y, w, h, u1, v1, u2, v2, color, tex_id, nz) }
fn __vkr_draw_text(_unused_vbo, text, x, y, color, glyphs_ptr, ascent, line_h, out_info){ "Internal: low-level text rendering call." vk_font.__vkr_draw_text(_unused_vbo, text, x, y, color, glyphs_ptr, ascent, line_h, out_info) }
fn _vkr_glyph_get_off(glyphs_ptr, cp){ "Internal: returns the glyph entry for a codepoint." vk_font._vkr_glyph_get_off(glyphs_ptr, cp) }
fn _vkr_glyph_present(glyphs_ptr, cp){ "Internal: returns true if a codepoint is loaded in the fast glyph table." vk_font._vkr_glyph_present(glyphs_ptr, cp) }

fn create_static_buffer(ptr, count){ "Creates an immutable GPU buffer from host memory." vk_buffers.create_static_buffer(ptr, count) }
fn destroy_static_buffer(sbuf){
   "Releases a static GPU vertex buffer created by `create_static_buffer`."
   if(!is_dict(sbuf)){ return }
   def buf = dict_get(sbuf, "handle", 0)
   def mem = dict_get(sbuf, "memory", 0)
   if(buf){ destroy_buffer(_device, buf, 0) }
   if(mem){ free_memory(_device, mem, 0) }
}

fn _mvp_matrix(){ "Returns the current internal projection matrix." vk_renderer._mvp_matrix() }
fn set_unlit(unlit){ "Enables or disables simple unlit rendering mode." vk_renderer.set_unlit(unlit) }
fn set_wireframe(enabled){ "Enables or disables wireframe rasterization." vk_renderer.set_wireframe(enabled) }
fn set_model_matrix(mat){ "Sets the global 4x4 Model matrix." vk_renderer.set_model_matrix(mat) }
fn set_mvp(mat){ "Sets the global 4x4 View-Projection matrix." vk_renderer.set_mvp(mat) }
fn set_ortho(l, r, b, t, n, f){ "Applies an orthographic projection matrix." vk_renderer.set_ortho(l, r, b, t, n, f) }
fn set_perspective(fovy, aspect, near, far){ "Applies a perspective projection matrix." vk_renderer.set_perspective(fovy, aspect, near, far) }
fn set_clear_color(r, g, b, a=1.0){ "Sets the background clear color." vk_renderer.set_clear_color(r, g, b, a) }
fn _pack_color(r, g, b, a){ "Packs float RGBA into 32-bit integer color." vk_utils._pack_color(r, g, b, a) }
fn _flush(){ "Immediately submits pending geometry to the GPU." vk_renderer._flush() }
fn _update_default_mvp(win){ "Internal: updates the default 2D projection based on window size." vk_renderer._update_default_mvp(win) }

fn compile_glsl_to_spirv(source, stage_ext){ "Compiles GLSL source string to SPIR-V." vk_pipeline.compile_glsl_to_spirv(source, stage_ext) }
fn create_shader_module_from_source(source, stage_ext){ "Compiles GLSL and creates a Vulkan shader module." vk_pipeline.create_shader_module_from_source(source, stage_ext) }
fn create_pipeline(vert_mod, frag_mod, topology=3, depth_test=1, depth_write=1, cull_mode=0, front_face=0, depth_bias=0, depth_clamp=0){ "Creates a customizable graphics pipeline object." vk_pipeline.create_pipeline(vert_mod, frag_mod, topology, depth_test, depth_write, cull_mode, front_face, depth_bias, depth_clamp) }
fn bind_pipeline(pipe){ "Activates a graphics pipeline for subsequent draw calls." vk_pipeline.bind_pipeline(pipe) }
fn push_constants(ptr, size, offset=0){ "Updates push constant data on the current command buffer." vk_pipeline.push_constants(ptr, size, offset) }
fn use_custom_push_constants(enabled){ "Enables/disables custom push constant mode for custom pipelines." vk_pipeline.use_custom_push_constants(enabled) }
fn set_custom_push_constants(ptr, size, offset=0){ "Sets custom push constant data (call after bind_pipeline with custom pipeline)." vk_pipeline.set_custom_push_constants(ptr, size, offset) }
fn _get_default_pipeline(){ "Internal: returns the standard 2D renderer vk_pipeline." vk_pipeline._get_default_pipeline() }
fn _get_nocull_pipeline(){ "Internal: returns the built-in lit no-cull pipeline." vk_pipeline._get_nocull_pipeline() }
fn draw_circle_sdf(x, y, radius, r, g, b, a){ vk_draw.draw_circle_sdf(x, y, radius, r, g, b, a) }
fn draw_ring_sdf(x, y, inner_radius, outer_radius, r, g, b, a){ vk_draw.draw_ring_sdf(x, y, inner_radius, outer_radius, r, g, b, a) }
