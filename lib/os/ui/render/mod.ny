;; Keywords: render graphics atlas camera view term terminal ansi tty shader matrix linear-algebra snapshot vterm
;; Rendering facade: frame loop, colors, 2D primitives, text, textures, cameras, meshes, capture, and renderer state.
module std.os.ui.render(
   BACKEND_NONE, BACKEND_GL, BACKEND_VK, BACKEND_MOCK, BACKEND_TTY, BACKEND_AUTO,
   WHITE, BLACK, RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, ORANGE, PURPLE, GRAY, CLEAR,
   color_rgba, color_rgb, color_gray, color_hex, color_lerp, color_alpha,
   backend_capabilities, get_active_backend_name, renderer_frame_stats,
   init_window, init_mock_surface, close_window, set_active_window, render_init,
   window_should_close, get_active_window, begin_frame, begin_frame_clear, end_frame,
   begin_drawing, end_drawing, begin_mode_3d, end_mode_3d, clear_background,
   draw_triangles, draw_polyline, draw_triangle, draw_quad, draw_rect,
   draw_rectangle_lines, draw_rect_rounded, draw_rect_sharp, draw_line, draw_line_2d,
   draw_line_fast, draw_circle, draw_circle_lines, draw_ring, draw_polygon,
   draw_ellipse, draw_ellipse_lines, draw_arc, draw_sector, draw_rounded_rectangle,
   draw_star, draw_grid, draw_grid_pair, draw_grid_pair_axes, draw_axes, draw_cube,
   draw_rect_tex, draw_rect_tex_uv, get_frame_time, get_time, load_shader,
   load_shader_agnostic, shader_transpile, get_active_backend, compile_shader,
   create_pipeline, bind_pipeline, push_constants, store_mat4_cm, reset_pipeline,
   blit_buffer, set_clear_color, draw_vertices, draw_lines_raw, color_pack, set_unlit,
   set_material, set_cam_pos, set_env_tex, set_env_spec_tex, push_vertex, set_view_proj,
   set_ortho, set_ortho_2d, set_perspective, VERTEX_STRIDE, mat4_identity, mat4_mul,
   mat4_ortho, mat4_perspective, mat4_look_at, mat4_look_at_xyz, mat4_rotate_x,
   mat4_rotate_y, mat4_rotate_z, mat4_translate, mat4_scale, init, shutdown,
   get_delta_time, camera_init, camera_update, camera_set_look_at, set_model_matrix,
   set_view, set_projection, set_camera, set_projection_mode, set_ortho_zoom,
   set_win_size, __cam_compute_vectors, collision_rect_rect, collision_rect_circle,
   collision_circle_circle, random_seed, random_value, open_url, easing, log, snapshot,
   clear_depth, request_frame_capture, set_backend_type, get_backend_type, scissor_push,
   scissor_pop, texture_load, texture_load_ex, texture_load_gltf,
   texture_try_load_cached_ex, texture_upload_image_ex, texture_destroy, texture_bind,
   draw_texture, create_cubemap, FONT_FILTER_DEFAULT, FONT_FILTER_NEAREST,
   FONT_FILTER_LINEAR, FONT_FILTER_BILINEAR, font_load, font_load_first, font_destroy,
   font_allow_color_fallback, _font_get, draw_text, draw_text_3d, measure_text,
   font_line_height, font_ascent, mesh_load, mesh_create, mesh_create_cpu,
   mesh_create_static, mesh_create_ex, mesh_create_indexed, mesh_create_cpu_part_from_raw,
   mesh_set_bounds, mesh_fit_world, mesh_fit_perspective, mesh_fit_camera, mesh_destroy,
   mesh_group_destroy, mesh_retire, mesh_collect_retired, mesh_build_grid,
   mesh_build_axes, draw_mesh, draw_mesh_group, set_wireframe, get_pixel,
   get_framebuffer_size, framebuffer_size_f64, layout_fit, begin_frame_layout,
   layout_x, layout_y, layout_size, layout_rect, get_swapchain_image_count,
   renderer_config, set_window_pos, draw_rect_fast,
   draw_rect_outline_fast, draw_rects_fast_ptr, draw_lines_2d_fast_ptr, draw_text_batch,
   draw_text_runs, draw_text_runs_flat, draw_text_runs_flat_colors, measure_text_fast
)

use std.core
use std.core.mem
use std.core.error
use std.os.ui.consts as ui_consts
use std.os.ui.profile as ui_profile
use std.os.ui.window
use std.os.ui.window as lib_uiw
use std.os.ui.render.vk as lib_vkr
use std.os.ui.render.vk.texture as vk_texture
use std.os.ui.render.vk.utils as vk_utils
use std.parse.3d.gltf as gltf
use std.os.ui.render.vk.state (
   _scratch_model_saved_a,
   _VKR_OFF_X, _VKR_OFF_Y, _VKR_OFF_Z,
   _snapshot_model_matrix, _model_matrix, _scratch_ident, _last_frame_draw_calls,
   _debug_gfx_enabled,
   _last_frame_dynamic_draw_calls, _last_frame_static_draw_calls, _last_frame_indexed_draw_calls,
   _last_flush_total, _last_pipeline_bind_count, _last_descriptor_bind_count, _last_submitted_vertices,
   _last_frame_begin_cpu_us, _last_frame_end_cpu_us, _last_frame_flush_cpu_us, _last_frame_sync_pc_cpu_us,
   _last_prim_rect_quads, _last_prim_outline_quads, _last_prim_line_quads, _last_prim_raw_lines,
   _last_prim_raw_points, _last_prim_text_calls, _last_prim_text_glyphs,
   MAX_TEXTURES
)

use std.os.ui.render.vk.vulkan
use std.os.ui.render.shader as lib_shader
use std.os.ui.render.atlas as lib_atlas
use std.os.ui.render.img.ops as img_ops
use std.os.ui.font.truetype as lib_ttf
use std.core.str as lib_str
use std.core.str (to_hex)
use std.math.crypto.encoding.base as str_base
use std.math.crypto.encoding.bytes
use std.os as std_os
use std.os.sys as os_sys
use std.os.fs as lib_fs
use std.os.path as lib_path
use std.os.thread
use std.os.time as ostime
use std.math
use std.os.ui.render.matrix
use std.os.ui.render.matrix as render_matrix
use std.math.vector
use std.math.crypto.hash as lib_hash
use std.parse.img as lib_img
use std.parse.data.zlib as lib_zlib
use std.parse.3d.obj as lib_obj
use std.core.cache as cache

def BACKEND_NONE = 0
def BACKEND_GL   = 1
def BACKEND_VK   = 2
def BACKEND_MOCK = 3
def BACKEND_TTY  = 4
def BACKEND_AUTO = 5
def WHITE = [1.0, 1.0, 1.0, 1.0]
def BLACK = [0.0, 0.0, 0.0, 1.0]
def RED   = [1.0, 0.0, 0.0, 1.0]
def GREEN = [0.0, 1.0, 0.0, 1.0]
def BLUE  = [0.0, 0.0, 1.0, 1.0]
def YELLOW= [1.0, 1.0, 0.0, 1.0]
def CYAN  = [0.0, 1.0, 1.0, 1.0]
def MAGENTA=[1.0, 0.0, 1.0, 1.0]
def ORANGE =[1.0, 0.5, 0.0, 1.0]
def PURPLE =[0.5, 0.0, 0.5, 1.0]
def GRAY  = [0.5, 0.5, 0.5, 1.0]
def CLEAR = [0.0, 0.0, 0.0, 0.0]
def FONT_FILTER_DEFAULT = -1
def FONT_FILTER_NEAREST = 0
def FONT_FILTER_LINEAR = 1
def FONT_FILTER_BILINEAR = 1

layout MeshGpuDrawSlab pack(8){
   ptr: sbuf_handle,
   ptr: sbuf_offset,
   ptr: ibuf,
   ptr: ibuf_offset,
   i32: draw_count,
   i32: idx_count,
   i32: flags,
   i32: idx_u32
}

fn _font_resolve_filter(int: filter, any: info=0): int {
   if(is_int(filter) && filter >= 0){ return filter ? FONT_FILTER_LINEAR : FONT_FILTER_NEAREST }
   if(info && !info.get("is_scalable", true) && !info.get("is_color", false)){ return FONT_FILTER_NEAREST }
   FONT_FILTER_LINEAR
}

fn _slab_i32(int: slab, int: off): int {
   if(!slab){ return -1 }
   def n = int(load32(slab, off))
   if(n > 2147483647){ return n - 4294967296 }
   n
}

fn color_alpha(any: c, f64: a): vec4 {
   "Returns color `c` with the same RGB and replaced alpha."
   [_color_at(c, 0, 1.0), _color_at(c, 1, 1.0), _color_at(c, 2, 1.0), a]
}

mut _active_win = nil
mut _active_scene_group = 0
mut _active_scene_gpu_slab = 0
mut _active_scene_render_parts = 0
mut _active_scene_gpu_parts = 0
mut _active_scene_count = 0
mut _active_scene_optical_start = 0
mut _active_scene_blend_start = 0
mut _active_scene_has_optical = false
mut _active_scene_has_blend = true
mut _active_scene_model_baked = false
mut _active_scene_parts_baked = false
mut _active_scene_light_slab = 0
mut _active_scene_light_count = 0
mut _active_scene_have_lights = false
mut _active_scene_gpu_ready = false
mut _active_scene_force_cpu_anim = false
mut _active_scene_cpu_optical_start = -1
mut _anim_frame_trace_hits = 0

fn _anim_frame_trace_enabled(): bool {
   ui_profile.env_truthy_cached("NY_UI_ANIM_FRAME_TRACE")
}

mut _vk_cam_pos_valid = false
mut _vk_cam_pos_x = 0.0
mut _vk_cam_pos_y = 0.0
mut _vk_cam_pos_z = 0.0
mut _start_time = 0
mut _start_time_sec = 0.0
mut _last_frame_time = 0
mut _current_frame_time = 0
mut _frame_time_accum = 0.0
mut _backend = BACKEND_VK
mut _cpu_buf = 0
mut _cpu_w = 0
mut _cpu_h = 0
mut _cpu_clear_color = 0xFF000000
mut _fonts = dict(32)
mut _next_font_id = 1
mut _default_font_id = 0
mut _default_font_fail_sizes = dict(8)
mut _retired_meshes = []
mut _shape_scratch = 0
mut _shape_scratch_cap = 0
mut _shape_scratch_used = 0
mut _proj_matrix = 0
mut _view_matrix = 0
mut _mvp_matrix = 0
mut _raw_mvp = 0
mut _3d_proj = 0
mut _3d_view = 0
mut _3d_mvp = 0
mut _3d_last_aspect = 0.0
mut _3d_last_fovy = 0.0
mut _scene_proj_mode = 0 ;; 0=PERSPECTIVE, 1=ORTHO
mut _proj_fov  = 120.0
mut _proj_zoom = 40.0
mut _font_cache_by_key = dict(32) ;; "path:size" -> font_id, for instant reload on resize
mut _font_info_cache = dict(32) ;; "path" -> parsed font info, reused across sizes
mut _font_priming = false ;; true during _font_prime_fast_data: suppresses per-glyph atlas flush
mut _font_dirty = dict(8) ;; font_id -> true when atlas has unflushed updates
mut _font_gpu_ready = dict(8) ;; font_id -> atlases already have GPU texture slots
mut _font_fast_text_enabled = -1
mut _font_vk_legacy_fallback_enabled = -1
mut _font_defer_flush_enabled = -1
mut _auto_show_pending = false
mut _text_batch_info = 0 ;; lazy-allocated 16-byte scratch buffer for _draw_text_ttf_fast
mut _grid_cache = dict(16)
mut _axes_cache = dict(8)
mut _grid_axes_cache_keys = []
mut _grid_axes_cache_vals = []
mut _grid_axes_last_key = ""
mut _grid_axes_last_cache = 0
mut _grid_axes_last_qrdx = -2147483648
mut _grid_axes_last_qrdz = -2147483648
mut _grid_axes_last_qadx = -2147483648
mut _grid_axes_last_qadz = -2147483648
mut _fallback_paths_cached = false
mut _fallback_paths = []
mut _fallback_info_cache = dict(32)
mut _fallback_meta_cache = dict(32)
mut _fallback_scan_out = []
mut _fallback_scan_seen = dict(8)
mut _fallback_scan_active = false
mut _fallback_scan_started = false
mut _fallback_cache_path = ""

fn _ensure_grid_axes_cache_lists(){
   if(!is_list(_grid_axes_cache_keys)){ _grid_axes_cache_keys = [] }
   if(!is_list(_grid_axes_cache_vals)){ _grid_axes_cache_vals = [] }
}

def _MESH_GPU_LINES = 1
def _MESH_GPU_UNLIT = 2
def _MESH_GPU_NOCULL = 4
def _MESH_GPU_INDEXED = 8
def _MESH_GPU_POINTS = 16
mut _gltf_safe_visible = -1
mut _tex_mips_cache = -1
mut _font_fallback_mode_cache = -1
mut _font_prime_mode_cache = -1
mut _deep_log_mesh_counter = 0

fn _ensure_font_info_cache(bool: fallback): dict {
   if(fallback){
      if(!is_dict(_fallback_info_cache)){ _fallback_info_cache = dict(32) }
      return _fallback_info_cache
   }
   if(!is_dict(_font_info_cache)){ _font_info_cache = dict(32) }
   _font_info_cache
}

fn _ensure_fallback_meta_cache(): dict {
   if(!is_dict(_fallback_meta_cache)){ _fallback_meta_cache = dict(32) }
   _fallback_meta_cache
}

fn _gltf_force_cpu_blend_enabled(): bool {
   ui_profile.env_truthy_cached("NY_GLTF_FORCE_CPU_BLEND")
}

fn _gltf_model_debug_enabled(): bool {
   ui_profile.env_truthy_cached("NY_GLTF_MODEL_DEBUG")
}

fn _gltf_frustum_cull_enabled(): bool {
   ui_profile.env_toggle_cached("NY_GLTF_FRUSTUM_CULL", true)
}

fn _light_trace_bind_enabled(): bool {
   ui_profile.env_lower_cached("NY_UI_LIGHT_TRACE") == "bind"
}

fn _env_full_none_mode(str: name): int {
   case ui_profile.env_lower_cached(name){
      "full", "1", "true", "yes", "on" -> 1
      "none", "0", "false", "no", "off" -> 2
      _ -> 0
   }
}

fn _font_fallback_mode(): int {
   if(_font_fallback_mode_cache != -1){ return _font_fallback_mode_cache }
   _font_fallback_mode_cache = _env_full_none_mode("NY_FONT_FALLBACK")
   _font_fallback_mode_cache
}

fn _font_prime_mode(): int {
   if(_font_prime_mode_cache != -1){ return _font_prime_mode_cache }
   _font_prime_mode_cache = _env_full_none_mode("NY_FONT_PRIME")
   _font_prime_mode_cache
}

fn _deep_should_log_mesh(): bool {
   if(!ui_profile.debug_deep_enabled()){ return false }
   _deep_log_mesh_counter += 1
   (_deep_log_mesh_counter % 600) == 1
}

fn _set_material_packed_defaults(
   int: material_u32 = 0,
   int: base_tex_id = -1,
   int: alpha_u32 = 0,
   int: normal_tex_id = -1,
   int: ext2_tex_word = 0x80000000,
   int: vc_mode = 0
): any {
   lib_vkr.set_material_packed(
      0xffffffff,
      material_u32,
      0, -1, 0, base_tex_id, alpha_u32, -1,
      0, 0, 0, 0,
      0, 0, 0, 0,
      0, 0, 0, 0,
      0, 0, 0, 0,
      0, normal_tex_id, ext2_tex_word, vc_mode
   )
}

fn _reset_material_state(): any {
   if(_backend == BACKEND_VK){
      _set_material_packed_defaults()
   }
}

fn mesh_create_cpu_part_from_raw(any: part, any: opts=0): list {
   "Creates a CPU mesh from a raw glTF part and returns [part, mesh]."
   if(!is_dict(part)){ return [part, 0] }
   def vptr = part.get("vptr", 0)
   def vcnt = int(part.get("vcnt", 0))
   if(!vptr || vcnt <= 0){ return [part, 0] }
   def iptr = part.get("iptr", 0)
   def icnt = int(part.get("icnt", 0))
   mut part_opts = is_dict(opts) ? opts : (is_dict(part.get("opts", 0)) ? part.get("opts", 0) : dict(4))
   def is_lines = part_opts.get("is_lines", false)
   part_opts["storage"] = "cpu"
   mut mesh = 0
   if(iptr && icnt > 0){ mesh = mesh_create_indexed(vptr, vcnt, iptr, icnt, part_opts, is_lines) } else { mesh = mesh_create_cpu(vptr, vcnt, is_lines, part_opts) }
   if(mesh != 0){ part["mesh"] = mesh }
   [part, mesh]
}

fn _rebuild_drawable_parts_from_raw(any: parts): list {
   mut out = []
   if(!is_list(parts)){ return out }
   def parts_n = parts.len
   mut i = 0
   while(i < parts_n){
      mut part = parts[i]
      if(is_dict(part)){
         mut opts = is_dict(part.get("opts", 0)) ? part.get("opts", 0) : dict(4)
         def built = mesh_create_cpu_part_from_raw(part, opts)
         part = built.get(0, part)
         mut mesh = built.get(1, 0)
         if(mesh != 0){
            mesh["tex_id"] = int(part.get("tex_id", -1))
            mesh["vc_mode"] = int(part.get("vc_mode", 0))
            def part_mat_slab = vk_utils.pack_material_slab(part)
            if(part_mat_slab){
               mesh["material_slab"] = part_mat_slab
               part["material_slab"] = part_mat_slab
            }
            part["mesh"] = mesh
         }
      }
      out = out.append(part)
      i += 1
   }
   out
}

fn _anim_apply_part_overrides(
   dict: part,
   int: node_idx,
   int: skin_idx,
   dict: anim_mats,
   bool: use_vis_map,
   any: vis_map,
   bool: fast_numeric_anim,
   bool: have_ptr_overrides,
   list: ptr_overrides
): list {
   "Applies non-skin animation overrides to a single drawable part.
   Returns [part, changed]."
   mut part_changed = false
   if(node_idx >= 0){
      if(anim_mats.contains(node_idx) && skin_idx < 0){
         def next_model = anim_mats.get(node_idx, part.get("model", 0))
         if(to_int(next_model) != to_int(part.get("model", 0))){
            part["model"] = next_model
            part_changed = true
         }
      }
      if(use_vis_map){
         def next_visible = vis_map.get(node_idx, true) ? true : false
         if((part.get("visible", true) ? true : false) != next_visible){
            part["visible"] = next_visible
            part_changed = true
         }
      }
   }
   if(skin_idx >= 0 && part.contains("skin_mesh_bind_world")){
      def bind_model = part.get("skin_mesh_bind_world", part.get("model", 0))
      if(to_int(bind_model) != to_int(part.get("model", 0))){
         part["model"] = bind_model
         part_changed = true
      }
   }
   if(!fast_numeric_anim && have_ptr_overrides){
      def mat_part = vk_utils.gltf_anim_apply_material_pointer_overrides(part, ptr_overrides)
      if(to_int(mat_part) != to_int(part)){
         part = mat_part
         part_changed = true
      }
   }
   [part, part_changed]
}

fn _anim_wrapped_time(dict: group, f64: frame_time): f64 {
   def anim_duration = float(group.get("anim_duration", 0.0))
   def use_anim_time = group.get("anim_time_override", false)
   mut t = use_anim_time ? float(group.get("anim_time", frame_time)) : float(frame_time)
   if(anim_duration > 0.0){
      while(t >= anim_duration){ t -= anim_duration }
      while(t < 0.0){ t += anim_duration }
   }
   t
}

fn _anim_sample_overrides(dict: gltf_data, int: anim_count, int: anim_idx, f64: t, bool: anim_trace): dict {
   mut overrides = dict(0)
   def sample_t0 = anim_trace ? ticks() : 0
   if(anim_count > 0){
      if(anim_idx < 0){ overrides = gltf.gltf_sample_animation_merged(gltf_data, t) } else { overrides = gltf.gltf_sample_animation(gltf_data, anim_idx, t) }
   }
   if(anim_trace){ ui_profile.print_text("[anim:apply] sample_ms=" + to_str(ui_profile.elapsed_ms(sample_t0))) }
   overrides
}

fn _anim_apply_morph_rebuild(dict: group, dict: gltf_data, dict: overrides, any: mat_records, any: parts): list {
   def morph_apply = gltf.gltf_apply_morph_weights(gltf_data, overrides)
   mut next_gltf = morph_apply.get(0, gltf_data)
   def morph_changed = morph_apply.get(1, false) ? true : false
   if(is_dict(next_gltf) && to_int(next_gltf) != to_int(gltf_data)){ group["gltf_data"] = next_gltf } else { next_gltf = gltf_data }
   if(!morph_changed){ return [group, next_gltf, parts, false] }
   def rebuilt = gltf.gltf_to_mesh_group_indexed(next_gltf, 0, mat_records)
   if(!(is_dict(rebuilt) && is_list(rebuilt.get("parts", 0)))){ return [group, next_gltf, parts, false] }
   def raw_parts = rebuilt.get("parts", parts)
   mut next_parts = parts
   if(is_list(parts) && parts.len == raw_parts.len && parts.len > 0){ next_parts = vk_utils.gltf_sync_drawable_parts_from_raw(parts, raw_parts, true, true) } else { next_parts = _rebuild_drawable_parts_from_raw(raw_parts) }
   group["min"] = rebuilt.get("min", group.get("min", 0))
   group["max"] = rebuilt.get("max", group.get("max", 0))
   def gpu_state = group.get("gpu_draw_state", 0)
   if(is_list(gpu_state) && gpu_state.len >= 7){
      gpu_state[4] = 0
      group["gpu_draw_state"] = gpu_state
   }
   [group, next_gltf, next_parts, true]
}

fn _anim_apply_parts(
   any: parts,
   any: gltf_data,
   any: anim_mats,
   bool: use_vis_map,
   any: vis_map,
   bool: fast_numeric_anim,
   bool: have_ptr_overrides,
   list: ptr_overrides,
   any: skin_mats_cache,
   bool: anim_trace
): list {
   def parts_n = parts.len
   mut i = 0
   mut parts_changed = false
   mut part_skin_ms = 0.0
   mut part_other_ms = 0.0
   while(i < parts_n){
      mut part = parts[i]
      if(is_dict(part)){
         def part_other_t0 = anim_trace ? ticks() : 0
         def node_idx = int(part.get("node_idx", -1))
         def skin_idx = int(part.get("skin_idx", -1))
         def part_state = _anim_apply_part_overrides(part,
            node_idx,
            skin_idx,
            anim_mats,
            use_vis_map,
            vis_map,
            fast_numeric_anim,
            have_ptr_overrides,
         ptr_overrides)
         part = part_state.get(0, part)
         mut part_changed = part_state.get(1, false) ? true : false
         if(anim_trace){ part_other_ms += ui_profile.elapsed_ms(part_other_t0) }
         if(skin_idx >= 0){
            def skin_t0 = anim_trace ? ticks() : 0
            def skin_part = gltf.gltf_apply_skinning(part, gltf_data, anim_mats, skin_mats_cache)
            if(anim_trace){ part_skin_ms += ui_profile.elapsed_ms(skin_t0) }
            if(to_int(skin_part) != to_int(part)){
               part = skin_part
               part_changed = true
            }
         }
         if(part_changed){
            parts[i] = part
            parts_changed = true
         }
      }
      i += 1
   }
   if(anim_trace){ ui_profile.print_text("[anim:apply] part_other_ms=" + to_str(part_other_ms) + " skin_ms=" + to_str(part_skin_ms)) }
   [parts, parts_changed]
}

fn _apply_group_gltf_animation(dict: group): dict {
   if(!is_dict(group)){ return group }
   mut gltf_data = group.get("gltf_data", 0)
   if(!is_dict(gltf_data)){ return group }
   def anim_count = int(group.get("anim_count", 0))
   def skin_count = int(group.get("skin_count", 0))
   def morph_count_total = int(group.get("morph_target_count", 0))
   if(anim_count <= 0 && skin_count <= 0 && morph_count_total <= 0){ return group }
   def t = _anim_wrapped_time(group, float(_frame_time_accum))
   def anim_idx = int(group.get("anim_idx", 0))
   def anim_trace = _anim_frame_trace_enabled() && _anim_frame_trace_hits < 16
   def overrides = _anim_sample_overrides(gltf_data, anim_count, anim_idx, t, anim_trace)
   if(!is_dict(overrides)){ return group }
   def rebuild_t0 = anim_trace ? ticks() : 0
   def anim_mats = gltf.gltf_rebuild_animated_mats(gltf_data, overrides)
   if(anim_trace){ ui_profile.print_text("[anim:apply] rebuild_mats_ms=" + to_str(ui_profile.elapsed_ms(rebuild_t0))) }
   def fast_numeric_anim = overrides.get("__fast_numeric", false) ? true : false
   def vis_t0 = anim_trace ? ticks() : 0
   def use_vis_map = fast_numeric_anim ? false : gltf.gltf_has_node_visibility(gltf_data, overrides)
   def vis_map = use_vis_map ? gltf.gltf_resolve_node_visibility(gltf_data, overrides) : 0
   if(anim_trace){ ui_profile.print_text("[anim:apply] visibility_ms=" + to_str(ui_profile.elapsed_ms(vis_t0))) }
   def ptr_overrides = overrides.get("__pointers", [])
   def have_ptr_overrides = is_list(ptr_overrides) && ptr_overrides.len > 0
   def mat_records = group.get("mat_records", [])
   mut parts = group.get("parts", 0)
   mut parts_changed = false
   def skin_mats_cache = (skin_count > 0 && !fast_numeric_anim) ? dict(max(4, skin_count * 2)) : 0
   if(morph_count_total > 0){
      def morph_state = _anim_apply_morph_rebuild(group, gltf_data, overrides, mat_records, parts)
      group, gltf_data, parts = morph_state.get(0, group), morph_state.get(1, gltf_data), morph_state.get(2, parts)
      parts_changed = morph_state.get(3, false) ? true : false
   }
   if(!is_list(parts)){ return group }
   def part_state = _anim_apply_parts(parts, gltf_data, anim_mats, use_vis_map, vis_map, fast_numeric_anim, have_ptr_overrides, ptr_overrides, skin_mats_cache, anim_trace)
   parts = part_state.get(0, parts)
   parts_changed = parts_changed || (part_state.get(1, false) ? true : false)
   gltf.gltf_free_skin_mats_cache(skin_mats_cache)
   if(parts_changed){ group["parts"] = parts }
   group
}

fn fallback_scan_cb(any: p): int {
   "Internal: Callback for font directory scanning. Adds discovered fonts to the fallback list."
   if(!_fallback_scan_active){ return 0 }
   if(!p || !is_str(p)){ return 0 }
   def lp = lib_str.lower(p)
   if(!(lib_str.endswith(lp, ".ttf") || lib_str.endswith(lp, ".otf") || lib_str.endswith(lp, ".ttc") || lib_str.endswith(lp, ".otc"))){ return 0 }
   if(_fallback_scan_seen.get(p, 0)){ return 0 }
   _fallback_scan_seen[p] = 1
   _fallback_scan_out = _fallback_scan_out.append(p)
   0
}

fn _fallback_cache_file(): str {
   if(_fallback_cache_path == ""){ _fallback_cache_path = lib_path.join(cache_dir(), "nytrix_fallback_fonts.txt") }
   _fallback_cache_path
}

fn _fallback_cache_load(): list {
   def path = _fallback_cache_file()
   if(!std_os.file_exists(path)){ return [] }
   match std_os.file_read(path){
      ok(s) -> {
         if(!is_str(s) || s.len == 0){ return [] }
         def parts = lib_str.split(s, "\n")
         mut out = []
         mut i = 0
         def parts_n = parts.len
         while(i < parts_n){
            def p = lib_str.strip(parts[i])
            if(p.len > 0 && lib_fs.is_file(p)){ out = out.append(p) }
            i += 1
         }
         out
      }
      err(ignorederr) -> { ignorederr  [] }
   }
}

fn _fallback_cache_write(list: paths): int {
   if(!is_list(paths) || paths.len == 0){ return 0 }
   mut s, i = lib_str.Builder(paths.len * 32 + 8), 0
   def paths_n = paths.len
   while(i < paths_n){
      def p = paths[i]
      if(is_str(p) && p.len > 0){
         s = lib_str.builder_append(s, p)
         s = lib_str.builder_append(s, "\n")
      }
      i += 1
   }
   def text = lib_str.builder_to_str(s)
   lib_str.builder_free(s)
   if(text.len > 0){ _ = std_os.file_write(_fallback_cache_file(), text) }
   0
}

; Scratch matrices and cached window state.
mut _scratch_view = render_matrix.mat4_identity()
mut _scratch_proj = render_matrix.mat4_identity()
mut _scratch_mvp  = render_matrix.mat4_identity()
mut _cull_mvp     = render_matrix.mat4_identity() ; scratch for culling matrix compute
mut _scratch_model_set = render_matrix.mat4_identity() ; stable copy of the active model matrix
mut _scratch_model_mul = render_matrix.mat4_identity() ; reusable model composition scratch (avoids per-part alloc)
mut _scratch_model_conv = render_matrix.mat4_identity() ; reusable foreign->renderer model conversion scratch
mut _scratch_model_saved_b = render_matrix.mat4_identity() ; stable snapshot for grouped CPU pass
mut _scratch_marker_model = render_matrix.mat4_identity() ; stable snapshot for CPU point/line marker fallback
mut _model_debug = -1
mut _model_debug_seen = dict(256)
mut _last_win_w = 1280.0
mut _last_win_h = 720.0
mut _scissor_stack = []
mut _last_ortho2d_l = 0.0
mut _last_ortho2d_r = 0.0
mut _last_ortho2d_b = 0.0
mut _last_ortho2d_t = 0.0
mut _last_ortho2d_valid = false
mut _backend_pref = BACKEND_VK
mut _font_parity_seen = dict(2048)
mut _font_parity_log_count = 0

fn _is_debug(): int {
   ui_profile.debug_enabled() ? 1 : 0
}

fn _is_frame_debug(): int {
   ui_profile.gfx_frame_trace_enabled() ? 1 : 0
}

fn _ensure_scissor_stack(): any {
   if(!is_list(_scissor_stack)){ _scissor_stack = [] }
}

fn _font_parity_trace_enabled(): bool {
   ui_profile.env_truthy_cached("NY_FONT_PARITY_TRACE")
}

fn _font_parity_trace_once(any: tag, any: key, any: msg): bool {
   if(!_font_parity_trace_enabled()){ return false }
   if(_font_parity_log_count >= 64){ return false }
   def k = to_str(tag) + "|" + to_str(key)
   if(_font_parity_seen.get(k, false)){ return false }
   _font_parity_seen[k] = true
   _font_parity_log_count += 1
   ui_profile.print_text("[parity:font] " + msg)
   true
}

fn _cpu_pack_color(f64: r, f64: g, f64: b, f64: a): int {
   def rr = int(clamp01(r) * 255.0 + 0.5)
   def gg = int(clamp01(g) * 255.0 + 0.5)
   def bb = int(clamp01(b) * 255.0 + 0.5)
   def a8 = int(clamp01(a) * 255.0 + 0.5)
   return(a8 << 24) | (rr << 16) | (gg << 8) | bb
}

fn _cpu_ensure_surface(): bool {
   if(!_active_win){ return false }
   def w, h = int(_active_win.get("w", 0)), int(_active_win.get("h", 0))
   if(w <= 0 || h <= 0){ return false }
   if(_cpu_buf != 0 && _cpu_w == w && _cpu_h == h){ return true }
   if(_cpu_buf != 0){ free(_cpu_buf) }
   _cpu_w, _cpu_h = w, h
   def bytes = _cpu_w * _cpu_h * 4
   _cpu_buf = malloc(bytes)
   if(!_cpu_buf){
      _cpu_w, _cpu_h = 0, 0
      return false
   }
   memset(_cpu_buf, 0, bytes)
   true
}

fn _cpu_put(int: x, int: y, int: c): bool {
   if(x < 0 || y < 0 || x >= _cpu_w || y >= _cpu_h){ return false }
   def off = ((y * _cpu_w) + x) * 4
   store32(_cpu_buf, c, off)
   true
}

fn _cpu_blend(int: x, int: y, f64: r, f64: g, f64: b, f64: a): bool {
   if(x < 0 || y < 0 || x >= _cpu_w || y >= _cpu_h || a <= 0.0){ return false }
   def off = ((y * _cpu_w) + x) * 4
   def dst = load32(_cpu_buf, off)
   def da = float((dst >> 24) & 255) / 255.0
   def dr = float((dst >> 16) & 255) / 255.0
   def dg = float((dst >> 8) & 255) / 255.0
   def db = float(dst & 255) / 255.0
   def oa = a + da * (1.0 - a)
   mut or, og = r * a + dr * (1.0 - a), g * a + dg * (1.0 - a)
   mut ob = b * a + db * (1.0 - a)
   if(oa > 0.000001){
      or, og = or / oa, og / oa
      ob = ob / oa
   }
   store32(_cpu_buf, _cpu_pack_color(or, og, ob, oa), off)
   true
}

fn _cpu_clear(int: c): bool {
   if(!_cpu_ensure_surface()){ return false }
   def pixels = _cpu_w * _cpu_h
   mut i = 0
   while(i < pixels){
      store32(_cpu_buf, c, i * 4)
      i += 1
   }
   true
}

fn _cpu_draw_triangle(f64: x0, f64: y0, f64: x1, f64: y1, f64: x2, f64: y2, int: c): bool {
   if(!_cpu_ensure_surface()){ return false }
   mut min_x = int(min(x0, min(x1, x2)))
   mut max_x = int(max(x0, max(x1, x2)) + 1.0)
   mut min_y = int(min(y0, min(y1, y2)))
   mut max_y = int(max(y0, max(y1, y2)) + 1.0)
   if(min_x < 0){ min_x = 0 }
   if(min_y < 0){ min_y = 0 }
   if(max_x > _cpu_w){ max_x = _cpu_w }
   if(max_y > _cpu_h){ max_y = _cpu_h }
   def area = ((x1 - x0) * (y2 - y0)) - ((y1 - y0) * (x2 - x0))
   if(abs(area) < 0.000001){ return false }
   mut y = min_y
   while(y < max_y){
      mut x = min_x
      while(x < max_x){
         def px, py = float(x) + 0.5, float(y) + 0.5
         def w0 = ((x1 - x0) * (py - y0)) - ((y1 - y0) * (px - x0))
         def w1 = ((x2 - x1) * (py - y1)) - ((y2 - y1) * (px - x1))
         def w2 = ((x0 - x2) * (py - y2)) - ((y0 - y2) * (px - x2))
         if((w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0) || (w0 <= 0.0 && w1 <= 0.0 && w2 <= 0.0)){ _cpu_put(x, y, c) }
         x += 1
      }
      y += 1
   }
   true
}

fn _cpu_color_from_vk_packed(int: c): int {
   def a, b = float((c >> 24) & 255) / 255.0, float((c >> 16) & 255) / 255.0
   def g, r = float((c >> 8) & 255) / 255.0, float(c & 255) / 255.0
   _cpu_pack_color(r, g, b, a)
}

fn _cpu_pack_color_from(any: color, f64: r=1.0, f64: g=1.0, f64: b=1.0, f64: a=1.0): int {
   if(is_int(color)){ return _cpu_color_from_vk_packed(color) }
   _cpu_pack_color(_color_at(color, 0, r), _color_at(color, 1, g), _color_at(color, 2, b), _color_at(color, 3, a))
}

fn _cpu_project_vertex(f64: x, f64: y, f64: z, any: mvp): any {
   if(!mvp || !_cpu_ensure_surface()){ return 0 }
   def cx, cy = horizontal_mul_mvp(x, y, z, 0, mvp), horizontal_mul_mvp(x, y, z, 1, mvp)
   def cz, cw = horizontal_mul_mvp(x, y, z, 2, mvp), horizontal_mul_mvp(x, y, z, 3, mvp)
   if(abs(cw) < 0.000001 || cw <= 0.0){ return 0 }
   def inv_w = 1.0 / cw
   def nx = cx * inv_w
   def ny = cy * inv_w
   def nz = cz * inv_w
   def sx = (nx * 0.5 + 0.5) * float(_cpu_w)
   def sy = (1.0 - (ny * 0.5 + 0.5)) * float(_cpu_h)
   [sx, sy, nz]
}

fn _cpu_mul_active_mvp_into(any: out): bool {
   def mvp = _mvp_matrix
   if(!is_list(mvp)){ return false }
   mat4_mul_into(mvp, _model_matrix, out)
   true
}

fn _cpu_line_world(list: start, list: finish, int: color, f64: thickness): bool {
   if(!_cpu_mul_active_mvp_into(_cull_mvp)){ return false }
   def a = _cpu_project_vertex(start.get(0,0.0), start.get(1,0.0), start.get(2,0.0), _cull_mvp)
   def b = _cpu_project_vertex(finish.get(0,0.0), finish.get(1,0.0), finish.get(2,0.0), _cull_mvp)
   if(!a || !b){ return false }
   def ax, ay = a.get(0, 0.0), a.get(1, 0.0)
   def bx, by = b.get(0, 0.0), b.get(1, 0.0)
   def dx, dy = bx - ax, by - ay
   def len = sqrt(dx*dx + dy*dy)
   if(len <= 0.000001){ return false }
   def px, py = -dy / len * (thickness * 0.5), dx / len * (thickness * 0.5)
   _cpu_draw_triangle(ax + px, ay + py, ax - px, ay - py, bx - px, by - py, color)
   _cpu_draw_triangle(ax + px, ay + py, bx - px, by - py, bx + px, by + py, color)
   true
}

fn get_pixel(int: x, int: y): any {
   "Returns the pixel color at(x,y) as [r,g,b,a] floats, or 0 if out of bounds."
   if(!_cpu_buf || x < 0 || y < 0 || x >= _cpu_w || y >= _cpu_h){ return 0 }
   def off = ((y * _cpu_w) + x) * 4
   def c = load32(_cpu_buf, off)
   def a8 = (c >> 24) & 255
   def r8 = (c >> 16) & 255
   def g8 = (c >> 8) & 255
   def b8 = c & 255
   [float(r8) / 255.0, float(g8) / 255.0, float(b8) / 255.0, float(a8) / 255.0]
}

fn get_framebuffer_size(): list {
   "Returns [width, height] of the active framebuffer."
   if(_backend == BACKEND_VK){ return lib_vkr.get_swapchain_size() }
   [_cpu_w, _cpu_h]
}

fn framebuffer_size_f64(f64: fallback_w=960.0, f64: fallback_h=540.0): list {
   "Returns sanitized [width, height] of the active framebuffer as floats."
   def fb = get_framebuffer_size()
   mut w, h = float(fb.get(0, fallback_w)), float(fb.get(1, fallback_h))
   if(w <= 0.0){ w = fallback_w }
   if(h <= 0.0){ h = fallback_h }
   if(w <= 0.0){ w = 1.0 }
   if(h <= 0.0){ h = 1.0 }
   [w, h]
}

fn layout_fit(f64: base_w=960.0, f64: base_h=540.0, f64: min_scale=0.0, f64: max_scale=0.0): dict {
   "Syncs a Y-down 2D projection to the framebuffer and returns an aspect-fit layout."
   mut bw, bh = base_w, base_h
   if(bw <= 0.0){ bw = 1.0 }
   if(bh <= 0.0){ bh = 1.0 }
   def fb = framebuffer_size_f64(bw, bh)
   def w, h = float(fb.get(0, bw)), float(fb.get(1, bh))
   def sx, sy = w / bw, h / bh
   mut scale = sx
   if(sy < scale){ scale = sy }
   if(min_scale > 0.0 && scale < min_scale){ scale = min_scale }
   if(max_scale > 0.0 && scale > max_scale){ scale = max_scale }
   def content_w, content_h = bw * scale, bh * scale
   def view_w, view_h = w / scale, h / scale
   def view_x, view_y = (bw - view_w) * 0.5, (bh - view_h) * 0.5
   set_ortho_2d(view_x, view_x + view_w, view_y, view_y + view_h)
   return {
      "w": w,
      "h": h,
      "base_w": bw,
      "base_h": bh,
      "scale": scale,
      "sx": sx,
      "sy": sy,
      "x": (w - content_w) * 0.5,
      "y": (h - content_h) * 0.5,
      "content_w": content_w,
      "content_h": content_h,
      "view_x": view_x,
      "view_y": view_y,
      "view_w": view_w,
      "view_h": view_h
   }
}

fn begin_frame_layout(any: color=BLACK, f64: base_w=960.0, f64: base_h=540.0, f64: min_scale=0.0, f64: max_scale=0.0): any {
   "Begins a cleared 2D frame and returns layout_fit(); returns 0 if the frame cannot begin."
   if(!begin_frame_clear(color)){ return 0 }
   layout_fit(base_w, base_h, min_scale, max_scale)
}

fn layout_x(any: fit, f64: x): f64 {
   "Maps a design-space x coordinate to framebuffer pixels using a layout_fit result."
   if(!is_dict(fit)){ return x }
   float(fit.get("x", 0.0)) + x * float(fit.get("scale", 1.0))
}

fn layout_y(any: fit, f64: y): f64 {
   "Maps a design-space y coordinate to framebuffer pixels using a layout_fit result."
   if(!is_dict(fit)){ return y }
   float(fit.get("y", 0.0)) + y * float(fit.get("scale", 1.0))
}

fn layout_size(any: fit, f64: value): f64 {
   "Scales a design-space size to framebuffer pixels using a layout_fit result."
   if(!is_dict(fit)){ return value }
   value * float(fit.get("scale", 1.0))
}

fn layout_rect(any: fit, f64: x, f64: y, f64: w, f64: h): list {
   "Maps a design-space rectangle to framebuffer pixels using a layout_fit result."
   [layout_x(fit, x), layout_y(fit, y), layout_size(fit, w), layout_size(fit, h)]
}

fn get_swapchain_image_count(): int {
   "Returns the active Vulkan swapchain image count, 0 before Vulkan init, or 1 for non-Vulkan renderers."
   if(_backend == BACKEND_VK){ return lib_vkr.get_swapchain_image_count() }
   1
}

fn _ensure_fonts(): dict {
   if(!is_dict(_fonts)){ _fonts = dict(32) }
   _fonts
}

fn _font_register(dict: font_obj): int {
   def id = _next_font_id
   _next_font_id += 1
   _ensure_fonts()[id] = font_obj
   id
}

fn _font_get(int: font_id): any {
   if(!is_int(font_id) || font_id <= 0){ return 0 }
   _ensure_fonts().get(font_id, 0)
}

fn _font_set(int: font_id, dict: font_obj): int {
   if(!is_int(font_id) || font_id <= 0){ return 0 }
   _ensure_fonts()[font_id] = font_obj
   0
}

fn _font_line_height(any: info, f64: scale, f64: size): f64 {
   def vm = lib_ttf.get_vmetrics(info)
   def line_h = vm.get(2, 0) * scale
   def size_f = float(size)
   mut lh = line_h
   if(lh < size_f * 0.95){ lh = size_f }
   def sane_max = size_f * 1.60
   if(lh > sane_max){ lh = size_f * 1.25 }
   lh
}

fn _fallback_path_priority(any: path): int {
   if(!path || !is_str(path)){ return 9 }
   def lp = lib_str.lower(path)
   if(lib_str.find(lp, "coloremoji") >= 0
      || lib_str.find(lp, "emoji") >= 0
      || lib_str.find(lp, "seguiemj") >= 0
      || lib_str.find(lp, "apple color emoji") >= 0
      || lib_str.find(lp, "twemoji") >= 0){
      return 0
   }
   if(lib_str.find(lp, "cjk") >= 0
      || lib_str.find(lp, "hiragino") >= 0
      || lib_str.find(lp, "meiryo") >= 0
      || lib_str.find(lp, "msgothic") >= 0
      || lib_str.find(lp, "yugoth") >= 0
      || lib_str.find(lp, "notosansjp") >= 0
      || lib_str.find(lp, "notosanskr") >= 0
      || lib_str.find(lp, "notosanssc") >= 0
      || lib_str.find(lp, "notosanstc") >= 0
      || lib_str.find(lp, "wenquanyi") >= 0){
      return 1
   }
   if(lib_str.find(lp, "dejavu") >= 0 || lib_str.find(lp, "liberation") >= 0 || lib_str.find(lp, "arial unicode") >= 0 ||
      lib_str.find(lp, "seguisym") >= 0 || lib_str.find(lp, "symbola") >= 0){
      return 2
   }
   if(lib_str.find(lp, "nerd") >= 0 || lib_str.find(lp, "powerline") >= 0){ return 3 }
   4
}

fn _fallback_merge_ranked(list: base, list: extra): list {
   mut out = []
   mut seen = dict(16)
   mut i = 0
   def base_n = base.len
   while(i < base_n){
      def p = base[i]
      if(is_str(p) && p.len > 0 && !seen.get(p, 0)){
         out = out.append(p)
         seen[p] = 1
      }
      i += 1
   }
   mut b0, b1 = [], []
   mut b2, b3 = [], []
   mut b4 = []
   i = 0
   def extra_n = extra.len
   while(i < extra_n){
      def raw = extra[i]
      def p = is_str(raw) ? lib_path.resolve_repo_asset(raw) : raw
      if(is_str(p) && p.len > 0 && !seen.get(p, 0) && std_os.file_exists(p)){
         seen[p] = 1
         def pri = _fallback_path_priority(p)
         case pri {
            0 -> { b0 = b0.append(p) }
            1 -> { b1 = b1.append(p) }
            2 -> { b2 = b2.append(p) }
            3 -> { b3 = b3.append(p) }
            _ -> { b4 = b4.append(p) }
         }
      }
      i += 1
   }
   def buckets = [b0, b1, b2, b3, b4]
   i = 0
   def buckets_n = buckets.len
   while(i < buckets_n){
      def bucket = buckets[i]
      mut j = 0
      def bucket_n = bucket.len
      while(j < bucket_n){
         out = out.append(bucket[j])
         j += 1
      }
      i += 1
   }
   out
}

fn _get_fallback_paths(): list {
   if(_fallback_paths_cached){ return _fallback_paths }
   def full_scan = ui_profile.env_truthy_cached("NY_TERM_FULL_FONTS")
   mut out = [
      "/usr/share/fonts/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/noto/NotoEmoji-Regular.ttf",
      "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto/NotoSansMonoCJK-Regular.ttc",
      "/usr/share/fonts/noto/NotoSansJP-Regular.otf",
      "/usr/share/fonts/noto/NotoSansKR-Regular.otf",
      "/usr/share/fonts/noto/NotoSansSC-Regular.otf",
      "/usr/share/fonts/noto/NotoSansTC-Regular.otf",
      "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansMonoCJK-Regular.ttc",
      "/usr/share/fonts/opentype/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/liberation2/LiberationMono-Regular.ttf",
      "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
      "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
      "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
      "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
      "C:/Windows/Fonts/seguiemj.ttf",
      "C:/Windows/Fonts/seguisym.ttf",
      "C:/Windows/Fonts/meiryo.ttc",
      "C:/Windows/Fonts/msgothic.ttc",
      "C:/Windows/Fonts/YuGothM.ttc",
      "/System/Library/Fonts/Apple Color Emoji.ttc",
      "/System/Library/Fonts/Hiragino Sans GB.ttc",
      "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
   ]
   mut filtered = []
   mut i = 0
   def out_n = out.len
   while(i < out_n){
      def p = out[i]
      if(std_os.file_exists(p)){ filtered = filtered.append(p) }
      i += 1
   }
   out = filtered
   out = _fallback_merge_ranked(out, _fallback_cache_load())
   _fallback_paths = out
   _fallback_paths_cached = true
   if(full_scan && !_fallback_scan_started){
      _fallback_scan_started = true
      thread_spawn(fn(){
            def dirs = [
               "/usr/share/fonts",
               "/usr/local/share/fonts",
               "/usr/share/fonts/TTF",
               "/usr/share/fonts/OTF",
               "/usr/share/fonts/noto",
               "/usr/share/fonts/noto-cjk",
               "/usr/share/fonts/truetype",
               "/usr/share/fonts/opentype",
               "C:/Windows/Fonts",
               "/System/Library/Fonts",
               "/Library/Fonts",
               lib_path.join(home_dir(), ".fonts"),
               lib_path.join(home_dir(), ".local/share/fonts")
            ]
            _fallback_scan_out = []
            _fallback_scan_seen = dict(8)
            _fallback_scan_active = true
            mut di = 0
            def dirs_n = dirs.len
            while(di < dirs_n){
               def d = dirs.get(di)
               if(std_os.file_exists(d)){ lib_fs.walk(d, fallback_scan_cb) }
               di += 1
            }
            _fallback_scan_active = false
            def merged = _fallback_merge_ranked(_fallback_paths, _fallback_scan_out)
            if(merged.len > 0){
               _fallback_paths = merged
               _fallback_cache_write(merged)
            }
      })
   }
   _fallback_paths
}

fn _cached_ttf_info(str: path, bool: fallback): any {
   mut cache = _ensure_font_info_cache(fallback)
   def cached = cache.get(path, 0)
   if(cached){ return cached }
   if(ui_profile.env_truthy_cached("NY_FONT_LOAD_TRACE")){
      ui_profile.eprint_text("[font:info] load fallback=" + to_str(fallback) + " path=" + path)
   }
   def info = lib_ttf.load_path(path, 0)
   if(info){
      if(fallback){ _fallback_info_cache[path] = info }
      else { _font_info_cache[path] = info }
   }
   info
}

fn _fallback_info(str: path): any { _cached_ttf_info(path, true) }

fn _font_info(str: path): any { _cached_ttf_info(path, false) }

fn _fallback_meta(str: path): dict {
   def cached = _ensure_fallback_meta_cache().get(path, 0)
   if(cached){ return cached }
   def lp = lib_str.lower(path)
   def has_emoji = (lib_str.find(lp, "coloremoji") >= 0)
   || (lib_str.find(lp, "emoji") >= 0)
   || (lib_str.find(lp, "seguiemj") >= 0)
   || (lib_str.find(lp, "apple color emoji") >= 0)
   || (lib_str.find(lp, "twemoji") >= 0)
   def has_cjk = (lib_str.find(lp, "cjk") >= 0)
   || (lib_str.find(lp, "hiragino") >= 0)
   || (lib_str.find(lp, "meiryo") >= 0)
   || (lib_str.find(lp, "msgothic") >= 0)
   || (lib_str.find(lp, "yugoth") >= 0)
   || (lib_str.find(lp, "notosansjp") >= 0)
   || (lib_str.find(lp, "notosanskr") >= 0)
   || (lib_str.find(lp, "notosanssc") >= 0)
   || (lib_str.find(lp, "notosanstc") >= 0)
   || (lib_str.find(lp, "wenquanyi") >= 0)
   def has_nerd = (lib_str.find(lp, "nerd") >= 0) || (lib_str.find(lp, "powerline") >= 0)
   def has_dv = (lib_str.find(lp, "dejavu") >= 0)
   || (lib_str.find(lp, "liberation") >= 0)
   || (lib_str.find(lp, "arial unicode") >= 0)
   || (lib_str.find(lp, "seguisym") >= 0)
   || (lib_str.find(lp, "symbola") >= 0)
   mut atlsz = 1024
   if(has_emoji || has_cjk){ atlsz = 4096 }
   elif(has_nerd || has_dv){ atlsz = 2048 }
   mut m = {"emoji": has_emoji, "emoji2": has_emoji, "cjk": has_cjk, "nerd": has_nerd, "dv": has_dv, "atlas": atlsz}
   _fallback_meta_cache = cache.cache_put_reset(_ensure_fallback_meta_cache(), path, m, 2048, 32)
   m
}

fn _font_build_fallback_chain(int: font_id): bool {
   mut mf = _font_get(font_id)
   if(!is_dict(mf)){ return false }
   if(mf.get("fallback_built", false)){ return true }
   def fallback_paths = mf.get("fallback_paths", [])
   if(!fallback_paths || !is_list(fallback_paths) || fallback_paths.len == 0){
      mf["fallback_built"] = true
      _font_set(font_id, mf)
      return true
   }
   ; Fallback chain — ordered by priority:
   ;   1. NotoColorEmoji — color emoji (SMP U+1F300+)
   ;   2. NotoSansCJK — CJK unified ideographs, Hangul, Hiragana, Katakana
   ;   3. DejaVu — broad Latin, Greek, Cyrillic, Hebrew, Arabic, math, box-drawing, braille
   ;   4. Liberation / FreeMono — final Latin/mono safety net
   ;   5. Nerd Font monospaced (Nerd symbols, powerline, icons) — LAST to avoid icons
   mut ei = 0
   def fallback_paths_n = fallback_paths.len
   while(ei < fallback_paths_n){
      def ep = fallback_paths.get(ei)
      def meta = _fallback_meta(ep)
      if(!mf.get("allow_color_fallback", true) &&
         meta.get("emoji", false)){
         ei += 1
         continue
      }
      def einfo_raw = _fallback_info(ep)
      def efilter = mf.get("filter", _font_resolve_filter(FONT_FILTER_DEFAULT, einfo_raw))
      def einfo = einfo_raw
      if(einfo){
         def esize = float(mf.get("size", 16))
         mut escale = lib_ttf.scale_for_pixel_height(einfo, esize)
         def evm = lib_ttf.get_vmetrics(einfo)
         mut eascent = evm.get(0, 0) * escale
         mut edescent = evm.get(1, 0) * escale
         mut eline_h = _font_line_height(einfo, escale, mf.get("size", 16))
         if(einfo.get("is_color", false) || !einfo.get("is_scalable", true)){
            escale = esize
            eascent = esize * 0.8
            edescent = 0.0 - esize * 0.2
            eline_h = esize
         }
         def ep_has_emoji = meta.get("emoji", false)
         def ep_has_emoji2 = meta.get("emoji2", false)
         def ea = lib_atlas.atlas_create(meta.get("atlas", 1024), meta.get("atlas", 1024), efilter, true)
         mut efont = {
            "_kind": "ttf", "path": ep, "size": mf.get("size", 16),
            "info": einfo, "scale": escale, "ascent": eascent, "descent": edescent,
            "line_height": eline_h, "glyphs": dict(256), "filter": efilter,
            "atlas": ea, "atlas_chain": [ea], "atlas_size": meta.get("atlas", 1024)
         }
         def eid = _font_register(efont)
         mf = _font_get(font_id)
         if(!is_dict(mf)){ return false }
         mut chain = mf.get("fallback_chain", [])
         chain = chain.append(eid)
         mf["fallback_chain"] = chain
         if(!mf.get("emoji_font_id", 0) && (ep_has_emoji || ep_has_emoji2)){ mf["emoji_font_id"] = eid }
         _font_set(font_id, mf)
      }
      ei += 1
   }
   mf = _font_get(font_id)
   if(!is_dict(mf)){ return false }
   mf["fallback_built"] = true
   _font_set(font_id, mf)
   true
}

fn _font_load_impl(str: path, int: size, int: filter=FONT_FILTER_DEFAULT): int {
   if(!is_str(path) || path.len == 0){ return 0 }
   if(size < 4){ size = 4 }
   def cache_key = path + ":" + to_str(size)
   def cached_id = _font_cache_by_key.get(cache_key, 0)
   if(cached_id && _font_get(cached_id)){ return cached_id }
   mut f_info = _font_info(path)
   mut f_data = 0
   if(!f_info){
      def rd = std_os.file_read(path)
      if(is_err(rd)){ return 0 }
      f_data = unwrap(rd)
      f_info = lib_ttf.load(f_data, 0)
   }
   if(!f_info){ return 0 }
   def resolved_filter = _font_resolve_filter(filter, f_info)
   mut scale = lib_ttf.scale_for_pixel_height(f_info, float(size))
   def vm = lib_ttf.get_vmetrics(f_info)
   mut ascent = vm.get(0, 0) * scale
   mut descent = vm.get(1, 0) * scale
   mut line_h = _font_line_height(f_info, scale, size)
   if(f_info.get("is_color", false) || !f_info.get("is_scalable", true)){
      scale = float(size)
      ascent = float(size) * 0.8
      descent = 0.0 - float(size) * 0.2
      line_h = float(size)
   }
   def a = lib_atlas.atlas_create(2048, 2048, resolved_filter, true)
   mut font_obj = {
      "_kind": "ttf", "path": path, "size": size, "data": f_data, "info": f_info,
      "scale": scale, "ascent": ascent, "descent": descent, "line_height": line_h,
      "glyphs": dict(256), "filter": resolved_filter,
      "atlas": a, "atlas_chain": [a], "atlas_size": 2048
   }
   def id = _font_register(font_obj)
   if(ui_profile.env_truthy_cached("NY_FONT_LOAD_TRACE")){ ui_profile.print_text("[font:load] id=" + to_str(id) + " path=" + path + " size=" + to_str(size) + " scale=" + to_str(scale) + " filter=" + to_str(resolved_filter)) }
   ; Fallback chain build mode: "full", "lazy" (default), or "none"
   def fb_mode = _font_fallback_mode()
   def fb_full = fb_mode == 1
   def fb_none = fb_mode == 2
   if(!fb_none){
      def fallback_paths = _get_fallback_paths()
      font_obj["fallback_paths"] = fallback_paths
      font_obj["fallback_built"] = false
      _font_set(id, font_obj)
      if(fb_full){ _font_build_fallback_chain(id) }
   } else {
      font_obj["fallback_paths"] = []
      font_obj["fallback_built"] = true
      font_obj["fallback_chain"] = []
      _font_set(id, font_obj)
   }
   ; Prime AFTER fallback chain is built (if full); lazy mode primes base font only.
   _font_prime_fast_data(id)
   if(!is_dict(_font_cache_by_key)){ _font_cache_by_key = dict(32) }
   _font_cache_by_key[cache_key] = id
   id
}

@inline
fn _font_prime_range(int: font_id, int: cp_start, int: cp_end): bool {
   mut cp = cp_start
   while(cp <= cp_end){
      _font_resolve_glyph(font_id, cp)
      _font_sync_fast_glyph(font_id, cp)
      cp += 1
   }
   true
}

fn _font_prime_fast_data(int: font_id): any {
   "Allocates fast_glyphs table and eagerly primes ASCII + common terminal codepoints.
   All glyph pixels accumulate in atlas CPU buffers during resolve, then ONE
   atlas_flush() per atlas uploads everything in a single GPU call."
   mut f = _font_get(font_id)
   if(!is_dict(f)){ return 0 }
   def bytes = 4352 * 8
   def ptr = malloc(bytes)
   if(!ptr){ return 0 }
   memset(ptr, 0, bytes)
   f["fast_glyphs"] = ptr
   _font_set(font_id, f)
   _font_priming = true
   def mode = _font_prime_mode()
   def prime_full = mode == 1
   def prime_none = mode == 2
   if(prime_none){
      _font_priming = false
      return ptr
   }
   _font_prime_range(font_id, 32, 126)
   _font_prime_range(font_id, 128, 255)
   def prime_terminal = (mode != 0) && is_str(mode) && (mode == "terminal" || mode == "std" || mode == "2")
   if(prime_terminal || prime_full){
      _font_prime_range(font_id, 0x2500, 0x259F)
      _font_prime_range(font_id, 0x2000, 0x206F)
      _font_prime_range(font_id, 0x2700, 0x27BF)
   }
   if(prime_full){
      _font_prime_range(font_id, 0x2600, 0x27BF)
      _font_prime_range(font_id, 0x2800, 0x28FF)
      _font_prime_range(font_id, 0x2190, 0x22FF)
      _font_prime_range(font_id, 0xE000, 0xF8FF)
      _font_prime_range(font_id, 0xEE00, 0xEEFF)
   }
   _font_priming = false
   _font_flush_atlases(font_id)
   ptr
}

fn _font_flush_atlases(int: font_id): bool {
   "Flushes all dirty atlas CPU buffers for a font and its fallback chain to the GPU.
   Called once after priming — replaces thousands of per-glyph GPU uploads with one per lib_atlas."
   mut f = _font_get(font_id)
   if(!f){ return false }
   _font_flush_atlas_chain(f)
   _font_apply_fallback_chain(f, false)
   true
}

fn _font_apply_atlas_op(any: a, bool: ensure_gpu=false, bool: destroy=false): bool {
   if(!is_dict(a)){ return false }
   if(destroy){ lib_atlas.atlas_destroy(a) return false }
   if(!ensure_gpu){ lib_atlas.atlas_flush(a) return false }
   def old_id = int(lib_atlas.atlas_texture_id(a))
   def new_id = int(lib_atlas.atlas_ensure_texture(a))
   if(new_id >= 0){ lib_atlas.atlas_flush(a) }
   old_id < 0 && new_id >= 0
}

fn _font_apply_atlas_chain(any: font_obj, bool: ensure_gpu=false, bool: destroy=false): bool {
   if(!is_dict(font_obj)){ return false }
   mut changed = false
   def chain = font_obj.get("atlas_chain", 0)
   if(is_list(chain) && chain.len > 0){
      mut i = 0
      def chain_n = chain.len
      while(i < chain_n){
         if(_font_apply_atlas_op(chain[i], ensure_gpu, destroy)){ changed = true }
         i += 1
      }
   } else {
      if(_font_apply_atlas_op(font_obj.get("atlas", 0), ensure_gpu, destroy)){ changed = true }
   }
   changed
}

fn _font_flush_atlas_chain(any: font_obj): bool{ _font_apply_atlas_chain(font_obj, false, false) }

fn _font_apply_fallback_chain(any: font_obj, bool: ensure_gpu=false): bool {
   def fchain_ids = font_obj.get("fallback_chain", [])
   mut changed = false
   mut i = 0
   def fchain_n = fchain_ids.len
   while(i < fchain_n){
      def fid = fchain_ids[i]
      if(fid){
         def fb = _font_get(fid)
         if(fb){
            if(ensure_gpu){ if(_font_ensure_atlas_chain_gpu(fb)){ changed = true } } else { _font_flush_atlas_chain(fb) }
         }
      }
      i += 1
   }
   changed
}

fn _font_ensure_atlas_chain_gpu(any: font_obj): bool{ _font_apply_atlas_chain(font_obj, true, false) }

fn _font_atlas_chain_ready(any: font_obj): bool {
   if(!is_dict(font_obj)){ return false }
   def chain = font_obj.get("atlas_chain", 0)
   if(is_list(chain) && chain.len > 0){
      mut i = 0
      def chain_n = chain.len
      while(i < chain_n){
         def a = chain[i]
         if(!is_dict(a) || int(lib_atlas.atlas_texture_id(a)) < 0){ return false }
         i += 1
      }
      return true
   }
   def a0 = font_obj.get("atlas", 0)
   is_dict(a0) && int(lib_atlas.atlas_texture_id(a0)) >= 0
}

fn _font_resync_cached_glyphs(int: font_id): bool {
   mut f = _font_get(font_id)
   if(!f){ return false }
   def glyphs = f.get("glyphs", 0)
   if(!glyphs || !is_dict(glyphs)){ return false }
   def items = dict_items(glyphs)
   mut i = 0
   def items_n = items.len
   while(i < items_n){
      def kv = items[i]
      if(is_list(kv) && kv.len >= 1){ _font_sync_fast_glyph(font_id, int(kv.get(0, 0))) }
      i += 1
   }
   true
}

fn _font_ensure_gpu_atlases(int: font_id): bool {
   def cached_ready = _font_gpu_ready.get(font_id, false)
   if(cached_ready && !_font_dirty.get(font_id, false)){ return false }
   def f = _font_get(font_id)
   if(!f){ return false }
   mut changed = _font_ensure_atlas_chain_gpu(f)
   if(_font_apply_fallback_chain(f, true)){ changed = true }
   def ready_now = _font_atlas_chain_ready(f)
   if(changed || (!cached_ready && ready_now)){ _font_resync_cached_glyphs(font_id) }
   if(ready_now){ _font_gpu_ready[font_id] = true } else { _font_gpu_ready = _font_gpu_ready.delete(font_id) }
   changed
}

fn _fit_color_glyph_metrics(dict: font_obj, f64: advance, f64: xoff, f64: yoff, f64: bw, f64: bh): list {
   def target_h0 = min(float(font_obj.get("size", 16.0)) * 0.78, float(font_obj.get("line_height", 16.0)) * 0.74)
   if(bh <= 0.0 || target_h0 <= 0.0){ return [advance, xoff, yoff, bw, bh] }
   def line_h = float(font_obj.get("line_height", target_h0))
   def ascent = float(font_obj.get("ascent", target_h0 * 0.8))
   mut sf = target_h0 / bh
   mut out_w = bw * sf
   mut out_h = bh * sf
   mut out_adv = advance * sf
   mut out_xoff = xoff * sf
   mut out_yoff = yoff * sf
   def max_w = target_h0 * 1.05
   if(out_w > max_w && out_w > 0.0){
      def clamp_sf = max_w / out_w
      out_w, out_h = out_w * clamp_sf, out_h * clamp_sf
      out_adv = out_adv * clamp_sf
      out_xoff = out_xoff * clamp_sf
   }
   def top_pad = max(0.0, (line_h - out_h) * 0.5)
   out_yoff = ascent - top_pad
   [out_adv, out_xoff, out_yoff, out_w, out_h]
}

fn _font_bitmap_is_color_like(any: font_obj, any: bm): bool {
   if(!bm){ return false }
   mut color_like = bm.get("is_color", 0) ? true : false
   if(!font_obj){ return color_like }
   def info = font_obj.get("info", 0)
   color_like = color_like || (info && info.get("is_color", false))
   if(color_like){ return color_like }
   def path = font_obj.get("path", "")
   if(!is_str(path)){ return false }
   def lp = lib_str.lower(path)
   (lib_str.find(lp, "coloremoji") >= 0) || (lib_str.find(lp, "emoji") >= 0) || (lib_str.find(lp, "seguiemj") >= 0) || (lib_str.find(lp, "apple color emoji") >= 0) || (lib_str.find(lp, "twemoji") >= 0)
}

fn _font_raster_oversample(dict: font_obj): f64 {
   def info = font_obj.get("info", 0)
   if(info && (!info.get("is_scalable", true) || info.get("is_color", false))){ return 1.0 }
   if(!font_obj.get("is_scalable", true) || font_obj.get("is_color", false)){ return 1.0 }
   if(int(font_obj.get("filter", FONT_FILTER_LINEAR)) != FONT_FILTER_LINEAR){ return 1.0 }
   def sz = float(font_obj.get("size", 16.0))
   if(sz <= 0.0 || sz > 36.0){ return 1.0 }
   2.0
}

fn _font_bitmap_atlas_ref(any: owner_font, any: bm): any {
   mut atlas_ref = bm.get("atlas", 0)
   if(is_dict(atlas_ref)){ return atlas_ref }
   def page_idx = int(bm.get("atlas_page", 0))
   def owner_chain = owner_font.get("atlas_chain", 0)
   if(is_list(owner_chain) && page_idx >= 0 && page_idx < owner_chain.len){
      def page_ref = owner_chain.get(page_idx, 0)
      if(is_dict(page_ref)){ return page_ref }
   }
   def primary_atlas = owner_font.get("atlas", 0)
   is_dict(primary_atlas) ? primary_atlas : atlas_ref
}

fn _font_sync_bitmap_tex_id(any: atlas_ref, any: bm, any: glyph, int: font_id, int: cp, any: font_obj): int {
   mut tex_id = int(bm.get("tex_id", -1))
   if(!is_dict(atlas_ref) || tex_id >= 0){ return tex_id }
   def live_tex = lib_atlas.atlas_ensure_texture(atlas_ref)
   if(live_tex < 0){ return tex_id }
   lib_atlas.atlas_flush(atlas_ref)
   tex_id = live_tex
   bm = bm.set("tex_id", live_tex)
   glyph = glyph.set("bitmap", bm)
   mut glyphs = font_obj.get("glyphs", dict(256))
   glyphs[cp] = glyph
   font_obj["glyphs"] = glyphs
   _font_set(font_id, font_obj)
   tex_id
}

fn _font_bitmap_uv(any: atlas_ref, any: bm, int: cp): any {
   mut uv = bm.get("uv", [0.0, 0.0, 0.0, 0.0])
   if(!is_dict(atlas_ref)){ return uv }
   def u0, v0 = is_list(uv) ? _list_num_safe(uv, 0, 0.0) : 0.0, is_list(uv) ? _list_num_safe(uv, 1, 0.0) : 0.0
   def u1, v1 = is_list(uv) ? _list_num_safe(uv, 2, 0.0) : 0.0, is_list(uv) ? _list_num_safe(uv, 3, 0.0) : 0.0
   if(is_list(uv) && uv.len >= 4 && u1 > u0 && v1 > v0){ return uv }
   def atlas_uv = lib_atlas.atlas_get(atlas_ref, cp)
   (is_list(atlas_uv) && atlas_uv.len >= 4) ? atlas_uv : uv
}

fn _font_trace_sync_fast_glyph(int: font_id, int: cp, int: tex_id, any: uv, any: bm, any: atlas_ref): bool {
   if(!ui_profile.env_truthy_cached("NY_FONT_SYNC_TRACE") || !(cp == 65 || cp == 71 || cp == 101)){ return false }
   ui_profile.print_text("[font:sync] font=" + to_str(font_id) +
      " cp=" + to_str(cp) +
      " tex=" + to_str(tex_id) +
      " uv_list=" + to_str(is_list(uv)) +
      " uv_len=" + to_str(is_list(uv) ? uv.len : 0) +
      " uv0=" + to_str(is_list(uv) ? _list_num_safe(uv, 0, -1.0) : -1.0) +
      " uv2=" + to_str(is_list(uv) ? _list_num_safe(uv, 2, -1.0) : -1.0) +
      " page=" + to_str(int(bm.get("atlas_page", -1))) +
      " atlas_tex=" + to_str(is_dict(atlas_ref) ? lib_atlas.atlas_texture_id(atlas_ref) : -99)
   )
   true
}

fn _font_store_fast_bitmap_metrics(any: off, any: font_obj, any: owner_font, any: glyph, any: bm, any: uv, int: tex_id, int: is_color): bool {
   mut xoff, yoff = float(bm.get("xoff", 0)), float(bm.get("yoff", 0))
   mut bw, bh = float(bm.get("width", 0)), float(bm.get("height", 0))
   def raster_os = (is_color == 0) ? _font_raster_oversample(owner_font) : 1.0
   if(raster_os > 1.0){
      xoff, yoff = xoff / raster_os, yoff / raster_os
      bw, bh = bw / raster_os, bh / raster_os
   }
   mut eff_adv = float(glyph.get("advance", 0.0))
   mut eff_xoff, eff_yoff = xoff, yoff
   mut eff_bw, eff_bh = bw, bh
   if(is_color && bh > 0.0){
      def fit = _fit_color_glyph_metrics(font_obj, eff_adv, eff_xoff, eff_yoff, eff_bw, eff_bh)
      eff_adv = _list_num_safe(fit, 0, eff_adv)
      eff_xoff = _list_num_safe(fit, 1, eff_xoff)
      eff_yoff = _list_num_safe(fit, 2, eff_yoff)
      eff_bw = _list_num_safe(fit, 3, eff_bw)
      eff_bh = _list_num_safe(fit, 4, eff_bh)
   }
   store32_f32(off, eff_adv, 0)
   store32_f32(off, eff_xoff, 4)
   store32_f32(off, eff_yoff, 8)
   store32_f32(off, eff_bw,  12)
   store32_f32(off, eff_bh,  16)
   store32_f32(off, _list_num_safe(uv, 0, 0.0), 20)
   store32_f32(off, _list_num_safe(uv, 1, 0.0), 24)
   store32_f32(off, _list_num_safe(uv, 2, 0.0), 28)
   store32_f32(off, _list_num_safe(uv, 3, 0.0), 32)
   store32(off, tex_id, 36)
   store32(off, 1, 40)
   store32(off, is_color ? 1 : 0, 44)
   true
}

fn _font_sync_fast_glyph(int: font_id, int: cp): bool {
   if(cp < 0 || cp >= 1114112){ return false }
   mut f = _font_get(font_id)
   if(!f){ return false }
   def root_ptr = f.get("fast_glyphs", 0)
   if(!root_ptr){ return false }
   def page_idx = cp >> 8
   mut page_ptr = load64(root_ptr, page_idx * 8)
   if(!page_ptr){
      page_ptr = malloc(256 * 48)
      memset(page_ptr, 0, 256 * 48)
      store64_h(root_ptr, page_ptr, page_idx * 8)
   }
   mut glyph = _font_resolve_glyph(font_id, cp)
   if(!glyph){ return false }
   def off = ptr_add(page_ptr, (cp & 255) * 48)
   def owner_id = int(glyph.get("_font_id", font_id))
   mut owner_font = _font_get(owner_id)
   if(!owner_font){ owner_font = f }
   def advance = glyph.get("advance", 0.0)
   mut bm = glyph.get("bitmap", 0)
   if(bm){
      def atlas_ref = _font_bitmap_atlas_ref(owner_font, bm)
      def resolved_tex_id = _font_sync_bitmap_tex_id(atlas_ref, bm, glyph, font_id, cp, f)
      def is_color = _font_bitmap_is_color_like(owner_font, bm) ? 1 : 0
      def uv = _font_bitmap_uv(atlas_ref, bm, cp)
      _font_trace_sync_fast_glyph(font_id, cp, resolved_tex_id, uv, bm, atlas_ref)
      _font_store_fast_bitmap_metrics(off, f, owner_font, glyph, bm, uv, resolved_tex_id, is_color)
   } else {
      store32_f32(off, float(advance), 0)
      store32(off, 1, 40)
      store32(off, 0, 44)
   }
   true
}

fn _default_font_candidates(): list {
   #windows {
      return [
         "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/seguiemj.ttf", "C:/Windows/Fonts/seguisym.ttf",
         "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc", "C:/Windows/Fonts/YuGothM.ttc",
         "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/calibri.ttf"
      ]
   }
   #elif macos {
      return [
         "/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Apple Color Emoji.ttc",
         "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
         "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/System/Library/Fonts/Supplemental/Helvetica.ttf"
      ]
   }
   #endif
   return [
      "etc/assets/fonts/jetbrains.ttf", "etc/assets/fonts/monocraft.ttf",
      "/usr/share/fonts/noto/NotoSansMonoCJK-Regular.ttc", "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/noto/NotoSansMonoCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/liberation2/LiberationMono-Regular.ttf", "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
      "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf", "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf"
   ]
}

fn _ensure_default_font(int: size=16): int {
   if(size < 4){ size = 4 }
   if(_default_font_id){
      def f = _font_get(_default_font_id)
      if(f && f.get("size", 0) == size){ return _default_font_id }
   }
   if(_default_font_fail_sizes.get(size, false)){ return 0 }
   def paths = _default_font_candidates()
   mut i = 0
   def paths_n = paths.len
   while(i < paths_n){
      def id = _font_load_impl(paths[i], size)
      if(id){
         _default_font_fail_sizes = _default_font_fail_sizes.delete(size)
         _default_font_id = id
         return id
      }
      i += 1
   }
   _default_font_fail_sizes[size] = true
   0
}

fn _font_try_resolve_fallback_chain(
   int: font_id,
   int: cp,
   dict: font_obj,
   dict: glyphs,
   list: fallbacks,
   int: emoji_fid
): list {
   "Resolves glyph from fallback chain/emoji fallback and caches primary hit.
   Returns [glyph_or_zero, font_obj, glyphs]."
   mut fi = 0
   def flen = fallbacks.len
   while(fi < flen){
      def fid = fallbacks[fi]
      if(fid){
         def glyph = _font_resolve_glyph(fid, cp, false)
         if(glyph && _dict_int(glyph, "gi", 0) != 0){
            mut result = glyph
            result["_font_id"] = fid
            glyphs[cp] = result
            font_obj["glyphs"] = glyphs
            _font_set(font_id, font_obj)
            _font_dirty[font_id] = true
            return [result, font_obj, glyphs]
         }
      }
      fi += 1
   }
   if(emoji_fid && cp > 127){
      def glyph = _font_resolve_glyph(emoji_fid, cp, false)
      if(glyph && _dict_int(glyph, "gi", 0) != 0){
         mut result = glyph
         result["_font_id"] = emoji_fid
         return [result, font_obj, glyphs]
      }
   }
   [0, font_obj, glyphs]
}

fn _font_prepare_bitmap_for_atlas(
   int: font_id,
   dict: font_obj,
   any: bm,
   int: cp,
   f64: raster_os,
   int: atlas_size,
   int: atlas_filter
): list {
   "Normalizes glyph bitmap metrics and packs bitmap into atlas pages.
   Returns [bitmap, font_obj]."
   if(!bm){ return [bm, font_obj] }
   mut out_bm = bm
   def bdata = out_bm.get("data", 0)
   def bw = _dict_int(out_bm, "width", 0)
   def bh = _dict_int(out_bm, "height", 0)
   def is_color = _font_bitmap_is_color_like(font_obj, bm)
   out_bm = out_bm.set("is_color", is_color ? 1 : 0)
   if(raster_os > 1.0 && !is_color){
      out_bm = out_bm.set("draw_width", float(bw) / raster_os)
      out_bm = out_bm.set("draw_height", float(bh) / raster_os)
      out_bm = out_bm.set("draw_xoff", _dict_num(out_bm, "xoff", 0.0) / raster_os)
      out_bm = out_bm.set("draw_yoff", _dict_num(out_bm, "yoff", 0.0) / raster_os)
      out_bm = out_bm.set("oversample", raster_os)
   }
   if(!(bdata && bw > 0 && bh > 0)){ return [out_bm, font_obj] }
   mut chain = font_obj.get("atlas_chain", 0)
   if(!is_list(chain) || chain.len == 0){
      def a0 = font_obj.get("atlas", 0)
      if(is_dict(a0)){
         chain = [a0]
         font_obj["atlas_chain"] = chain
         _font_set(font_id, font_obj)
      }
   }
   if(!is_list(chain) || chain.len <= 0){ return [bm, font_obj] }
   mut page_idx = chain.len - 1
   mut a = chain.get(page_idx)
   if(!is_dict(a)){ return [out_bm, font_obj] }
   mut uv = lib_atlas.atlas_add(a, cp, bw, bh, bdata)
   if(!is_list(uv) || uv.len < 4){
      def na = lib_atlas.atlas_create(atlas_size, atlas_size, atlas_filter, true)
      chain = chain.append(na)
      page_idx = chain.len - 1
      font_obj["atlas_chain"] = chain
      _font_set(font_id, font_obj)
      a = na
      uv = lib_atlas.atlas_add(a, cp, bw, bh, bdata)
   }
   if(!is_list(uv) || uv.len < 4){ return [out_bm, font_obj] }
   out_bm = out_bm.set("atlas", a)
   out_bm = out_bm.set("atlas_page", page_idx)
   out_bm = out_bm.set("tex_id", lib_atlas.atlas_texture_id(a))
   out_bm = out_bm.set("uv", uv)
   if(!_font_priming){
      if(_font_defer_flush_mode()){ _font_dirty[font_id] = true } else { lib_atlas.atlas_flush(a) }
   }
   [out_bm, font_obj]
}

fn _font_resolve_glyph(int: font_id, int: cp, bool: allow_default=true): any {
   mut font_obj = _font_get(font_id)
   if(!is_dict(font_obj)){ return 0 }
   def font_size = _dict_int(font_obj, "size", 16)
   _font_parity_trace_once("resolve_font", to_str(font_id), "font=" + to_str(font_id) + " size=" + to_str(font_size))
   mut glyphs = font_obj.get("glyphs", dict(256))
   def cached = glyphs.get(cp, 0)
   if(cached && !_dict_bool(cached, "_provisional", false)){ return cached }
   def info = font_obj.get("info", 0)
   if(!info){ return 0 }
   def scale = _dict_num(font_obj, "scale", 1.0)
   def emoji_fid = _dict_int(font_obj, "emoji_font_id", 0)
   mut fallbacks = font_obj.get("fallback_chain", [])
   def atlas_size = _dict_int(font_obj, "atlas_size", 2048)
   def atlas_filter = _dict_int(font_obj, "filter", FONT_FILTER_DEFAULT)
   mut gi = lib_ttf.get_glyph_index(info, cp)
   mut _used_q_fallback = false
   mut _skipped_fallback_build = false
   if(gi == 0 && cp != 32){
      ; Walk fallback chain (CJK, Emoji, etc.)
      if(!fallbacks || !is_list(fallbacks)){ fallbacks = [] }
      if(fallbacks.len == 0 && !_dict_bool(font_obj, "fallback_built", false)){
         _font_build_fallback_chain(font_id)
         font_obj = _font_get(font_id)
         if(!is_dict(font_obj)){ return 0 }
         fallbacks, glyphs = font_obj.get("fallback_chain", []), font_obj.get("glyphs", glyphs)
      }
      def fallback = _font_try_resolve_fallback_chain(font_id, cp, font_obj, glyphs, fallbacks, emoji_fid)
      def fallback_hit = fallback.get(0, 0)
      font_obj, glyphs = fallback.get(1, font_obj), fallback.get(2, glyphs)
      if(fallback_hit){ return fallback_hit }
      if(allow_default){ gi = lib_ttf.get_glyph_index(info, 63) _used_q_fallback = _skipped_fallback_build && (cp != 63) }
   }
   if(gi == 0){
      def empty = {"gi": 0, "bitmap": 0, "advance": float(font_size) * 0.5}
      glyphs[cp] = empty
      font_obj["glyphs"] = glyphs
      _font_set(font_id, font_obj)
      return empty
   }
   ; Get bitmap FIRST to prime pixel_size in info, then get metrics at correct size.
   def raster_os = _font_raster_oversample(font_obj)
   mut bm, hm = lib_ttf.get_glyph_bitmap(info, scale * raster_os, scale * raster_os, gi), lib_ttf.get_hmetrics(info, gi)
   if(raster_os > 1.0){ hm = [float(hm.get(0, 0.0)) / raster_os, float(hm.get(1, 0.0)) / raster_os] }
   mut glyph = {"gi": gi}
   if(bm){
      def bm_state = _font_prepare_bitmap_for_atlas(font_id, font_obj, bm, cp, raster_os, atlas_size, atlas_filter)
      bm, font_obj = bm_state.get(0, bm), bm_state.get(1, font_obj)
   }
   glyph["bitmap"] = bm
   ; get_hmetrics returns pixels (FT already scaled via FT_Set_Pixel_Sizes) — do NOT multiply by scale
   glyph["advance"] = _list_num_safe(hm, 0, 0.0)
   glyph["_font_id"] = font_id
   if(_used_q_fallback){ glyph["_provisional"] = true }
   glyphs[cp] = glyph
   font_obj["glyphs"] = glyphs
   _font_set(font_id, font_obj)
   def bm_trace = glyph.get("bitmap", 0)
   def bm_color = (bm_trace && int(bm_trace.get("is_color", 0)) != 0) ? 1 : 0
   _font_parity_trace_once(
      "glyph_first",
      to_str(font_id) + ":" + to_str(cp),
      "font=" + to_str(font_id) +
      " cp=" + to_str(cp) +
      " gi=" + to_str(glyph.get("gi", 0)) +
      " adv=" + to_str(glyph.get("advance", 0.0)) +
      " tex=" + to_str(bm_trace ? bm_trace.get("tex_id", -1) : -1) +
      " uv=" + to_str((bm_trace && bm_trace.get("uv", 0)) ? 1 : 0) +
      " color=" + to_str(bm_color) +
      " bw=" + to_str(bm_trace ? bm_trace.get("width", 0) : 0) +
      " bh=" + to_str(bm_trace ? bm_trace.get("height", 0) : 0)
   )
   if(bm_color == 1){
      _font_parity_trace_once(
         "glyph_color_first",
         to_str(font_id),
         "font=" + to_str(font_id) +
         " color_cp=" + to_str(cp) +
         " tex=" + to_str(bm_trace ? bm_trace.get("tex_id", -1) : -1)
      )
   }
   glyph
}

fn _utf8_next_cp(str: s, int: i, int: n): list {
   if(i >= n){ return [0, i + 1] }
   def w = lib_str._utf8_seq_len(s, i, n)
   if(w <= 0){
      ; Invalid UTF-8 byte: consume one byte and render replacement '?'
      return [63, i + 1]
   }
   [lib_str._utf8_decode_at(s, i, w), i + w]
}

fn _draw_glyph_bitmap_runs(
   ptr: data,
   int: bw,
   int: bh,
   f64: ox,
   f64: oy,
   f64: r,
   f64: g,
   f64: b,
   f64: a,
   int: bpp=4,
   bool: is_color=false
) : bool {
   "Internal helper to draw a glyph into the CPU framebuffer."
   if(!data || bw <= 0 || bh <= 0 || a <= 0.0 || !_cpu_ensure_surface()){ return false }
   mut drew = false
   mut yy = 0
   while(yy < bh){
      def row_off = yy * bw * bpp
      mut xx = 0
      while(xx < bw){
         def px_off = row_off + xx * bpp
         mut sr, sg, sb = r, g, b
         mut alpha8 = load8(data, px_off + (bpp >= 4 ? 3 : 0)) & 255
         if(alpha8 > 0){
            if(is_color && bpp >= 4){
               sr, sg = float(load8(data, px_off + 0) & 255) / 255.0, float(load8(data, px_off + 1) & 255) / 255.0
               sb = float(load8(data, px_off + 2) & 255) / 255.0
            }
            _cpu_blend(int(ox + xx), int(oy + yy), sr, sg, sb, a * (float(alpha8) / 255.0))
            drew = true
         }
         xx += 1
      }
      yy += 1
   }
   drew
}

fn _ttf_color_rgba(any: color): list {
   if(is_int(color)){
      return [
         float((color >> 0) & 255) / 255.0,
         float((color >> 8) & 255) / 255.0,
         float((color >> 16) & 255) / 255.0,
         float((color >> 24) & 255) / 255.0,
      ]
   }
   [
      _color_at(color, 0, 1.0),
      _color_at(color, 1, 1.0),
      _color_at(color, 2, 1.0),
      _color_at(color, 3, 1.0),
   ]
}

fn _ttf_apply_control_cp(
   int: font_id,
   int: cp,
   f64: line_h,
   f64: base_x,
   f64: pen_x,
   f64: pen_y,
   int: prev_gi,
   f64: space_adv
): list {
   if(cp == 13){ return [true, pen_x, pen_y, prev_gi, space_adv] }
   if(cp == 10){ return [true, base_x, pen_y + line_h, -1, space_adv] }
   if(cp == 9){
      if(space_adv < 0.0){
         def space_g = _font_resolve_glyph(font_id, 32)
         space_adv = space_g.get("advance", 8.0)
      }
      return [true, pen_x + space_adv * 4.0, pen_y, -1, space_adv]
   }
   [false, pen_x, pen_y, prev_gi, space_adv]
}

fn _ttf_draw_bitmap_glyph(
   dict: font_obj,
   dict: bm,
   f64: pen_x,
   f64: pen_y,
   f64: g_adv,
   f64: cr,
   f64: cg,
   f64: cb,
   f64: ca
): list {
   def tex_id = bm.get("tex_id", -1)
   mut bw, bh = float(bm.get("draw_width", bm.get("width", 0))), float(bm.get("draw_height", bm.get("height", 0)))
   def uv = bm.get("uv", 0)
   def is_color = bm.get("is_color", 0)
   def xoff = float(bm.get("draw_xoff", bm.get("xoff", 0)))
   def yoff = float(bm.get("draw_yoff", bm.get("yoff", 0)))
   def bpp = int(bm.get("bpp", 4))
   def bdata = bm.get("data", 0)
   mut drew = false
   if(bw <= 0.0 || bh <= 0.0){ return [g_adv, drew] }
   mut gs = 1.0
   mut gcr, gcg, gcb = cr, cg, cb
   if(is_color){
      gcr, gcg, gcb = 1.0, 1.0, 1.0
      def fit = _fit_color_glyph_metrics(font_obj, g_adv, xoff, yoff, bw, bh)
      g_adv = _list_num_safe(fit, 0, g_adv)
      def gx, gy = pen_x + _list_num_safe(fit, 1, 0.0), pen_y - _list_num_safe(fit, 2, 0.0)
      bw, bh = _list_num_safe(fit, 3, bw), _list_num_safe(fit, 4, bh)
      if(_backend == BACKEND_VK && is_list(uv) && uv.len >= 4 && tex_id >= 0){
         lib_vkr.draw_glyph(
            gx, gy, bw, bh,
            _list_num_safe(uv, 0, 0.0), _list_num_safe(uv, 1, 0.0),
            _list_num_safe(uv, 2, 0.0), _list_num_safe(uv, 3, 0.0),
            tex_id, gcr, gcg, gcb, ca
         )
         drew = true
      } elif(_backend == BACKEND_MOCK){
         if(bdata){
            drew = _draw_glyph_bitmap_runs(
               bdata, int(float(bw)), int(float(bh)), gx, gy, gcr, gcg, gcb, ca, bpp, !!is_color
            ) || drew
         }
      }
      return [g_adv, drew]
   }
   def gx, gy = pen_x + xoff * gs, pen_y - yoff * gs
   if(_backend == BACKEND_VK && is_list(uv) && uv.len >= 4 && tex_id >= 0){
      lib_vkr.draw_glyph(
         gx, gy, bw * gs, bh * gs,
         _list_num_safe(uv, 0, 0.0), _list_num_safe(uv, 1, 0.0),
         _list_num_safe(uv, 2, 0.0), _list_num_safe(uv, 3, 0.0),
         tex_id, gcr, gcg, gcb, ca
      )
      drew = true
   } elif(_backend == BACKEND_MOCK){
      if(bdata){
         drew = _draw_glyph_bitmap_runs(
            bdata, int(bw), int(bh), gx, gy, gcr, gcg, gcb, ca, bpp, !!is_color
         ) || drew
      }
   }
   g_adv = g_adv * gs
   [g_adv, drew]
}

fn _draw_text_ttf(int: font_id, str: text, f64: x, f64: y, any: color): bool {
   def font_obj = _font_get(font_id)
   if(!font_obj || !is_str(text)){ return false }
   def info = font_obj.get("info", 0)
   if(!info){ return false }
   def font_size = int(font_obj.get("size", 16))
   _font_parity_trace_once(
      "draw_call",
      to_str(font_id),
      "draw font=" + to_str(font_id) +
      " size=" + to_str(font_size) +
      " len=" + to_str(text.len) +
      " x=" + to_str(int(x)) +
      " y=" + to_str(int(y))
   )
   def ascent = font_obj.get("ascent", 0.0)
   def line_h = font_obj.get("line_height", float(font_obj.get("size", 16)))
   def rgba = _ttf_color_rgba(color)
   def cr = _list_num_safe(rgba, 0, 1.0)
   def cg = _list_num_safe(rgba, 1, 1.0)
   def cb = _list_num_safe(rgba, 2, 1.0)
   def ca = _list_num_safe(rgba, 3, 1.0)
   mut pen_x, pen_y = x, y + ascent
   mut prev_gi = -1
   mut drew = false
   mut i = 0
   def n = text.len
   mut space_adv = -1.0
   while(i < n){
      def nxt = _utf8_next_cp(text, i, n)
      mut cp = nxt.get(0, 63)
      i = nxt.get(1, i + 1)
      if(cp < 0){ cp = 63 }
      def ctrl = _ttf_apply_control_cp(font_id, cp, line_h, x, pen_x, pen_y, prev_gi, space_adv)
      if(_list_any_safe(ctrl, 0, false)){
         pen_x, pen_y = _list_num_safe(ctrl, 1, pen_x), _list_num_safe(ctrl, 2, pen_y)
         prev_gi = int(_list_any_safe(ctrl, 3, prev_gi))
         space_adv = _list_num_safe(ctrl, 4, space_adv)
         continue
      }
      def glyph = _font_resolve_glyph(font_id, cp)
      if(!glyph){ continue }
      def gi = glyph.get("gi", 0)
      if(prev_gi >= 0 && gi > 0){ pen_x += float(lib_ttf.get_kern(info, prev_gi, gi, font_size)) }
      mut g_adv = glyph.get("advance", 0.0)
      def bm = glyph.get("bitmap", 0)
      if(bm){
         def draw_res = _ttf_draw_bitmap_glyph(font_obj, bm, pen_x, pen_y, g_adv, cr, cg, cb, ca)
         g_adv = _list_num_safe(draw_res, 0, g_adv)
         drew = _list_any_safe(draw_res, 1, false) || drew
      }
      pen_x += g_adv
      prev_gi = gi
   }
   drew
}

comptime table BuiltinGlyphBits {
   48 -> 15623448110 49 -> 4701950094 50 -> 15603929375 51 -> 32247317566
   52 -> 2359917634 53 -> 33854359086 54 -> 6728664622 55 -> 33321787656
   56 -> 15621113390 57 -> 15621129292 65 -> 15621670449 66 -> 32801506878
   67 -> 15620129326 68 -> 32801080894 69 -> 33840644639 70 -> 33840644624
   71 -> 15620359727 72 -> 18842895921 73 -> 15170932878 74 -> 7585483340
   75 -> 18879369809 76 -> 17734058527 77 -> 19182306865 78 -> 19115132465
   79 -> 15621211694 80 -> 32801505808 81 -> 15621215821 82 -> 32801509969
   83 -> 16660235326 84 -> 33424543876 85 -> 18842437166 86 -> 18842429764
   87 -> 18842572458 88 -> 18834663985 89 -> 18834657412 90 -> 33321787935
   45 -> 1015808 46 -> 396 47 -> 1109533200 58 -> 415248768
   95 -> 31 43 -> 139432064 35 -> 11788451136 63 -> 15603929092
   91 -> 15309480206 93 -> 15101659214 40 -> 2290360450 41 -> 8726317192
   62 -> 17452568848 60 -> 1145311297 124 -> 4433514628 42 -> 720353952
}

fn _builtin_glyph_bits(int: c): int {
   if(c >= 97 && c <= 122){ c = c - 32 }
   comptime match BuiltinGlyphBits(c, 33874822719)
}

fn _draw_text_builtin(str: text, f64: x, f64: y, any: color): bool {
   mut cx, cy = x, y
   def px = 1.45
   def cw = px * 6.0
   def ch = px * 8.0
   def packed = _pack_color_from(color, 1.0, 1.0, 1.0, 1.0, true)
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      i += 1
      if(c == 13){ continue }
      if(c == 10){
         cx = x
         cy += ch + 2.0
         continue
      }
      if(c == 32){
         cx += cw
         continue
      }
      def glyph_bits = _builtin_glyph_bits(c)
      mut gy = 0
      while(gy < 7){
         def bits = int((glyph_bits >> ((6 - gy) * 5)) & 31)
         mut gx = 0
         while(gx < 5){
            if((bits & (1 << (4 - gx))) != 0){ draw_rect_fast(cx + float(gx) * px, cy + float(gy) * px, px, px, packed) }
            gx += 1
         }
         gy += 1
      }
      cx += cw
   }
   true
}

fn color_rgba(f64: r, f64: g, f64: b, f64: a=1.0): vec4 {
   "Creates a vec4 color from floating point RGBA components(0.0 to 1.0)."
   return [r, g, b, a]
}

fn color_rgb(f64: r, f64: g, f64: b): vec4 {
   "Creates an opaque vec4 color from RGB components."
   color_rgba(r, g, b, 1.0)
}

fn color_gray(f64: v, f64: a=1.0): vec4 {
   "Creates a grayscale vec4 color."
   color_rgba(v, v, v, a)
}

comptime table HexNibble {
   48..57 -> raw - 48
   65..70 -> raw - 55
   97..102 -> raw - 87
}

fn _hex_nibble(int: ch): int { comptime match HexNibble(ch, 0) }

fn _hex_byte(str: hex, int: idx): int {
   if(hex.len <= idx + 1){ return 0 }
   (_hex_nibble(load8(hex, idx)) << 4) | _hex_nibble(load8(hex, idx + 1))
}

fn color_hex(any: hex): vec4 {
   "Creates a color from a hex string(e.g. \"#ff0000\" or \"ff0000\")."
   if(!is_str(hex) || hex.len < 6){ return BLACK }
   mut start = 0
   if(hex.len > 0 && load8(hex, 0) == 35){ start = 1 } ; '#'
   if(hex.len < start + 6){ return BLACK }
   def r, g = float(_hex_byte(hex, start + 0)) / 255.0, float(_hex_byte(hex, start + 2)) / 255.0
   def b = float(_hex_byte(hex, start + 4)) / 255.0
   mut a = 1.0
   if(hex.len >= start + 8){ a = float(_hex_byte(hex, start + 6)) / 255.0 }
   color_rgba(r, g, b, a)
}

fn color_lerp(vec4: a, vec4: b, f64: t): vec4 {
   "Linearly interpolates between two colors."
   def ar, ag, ab, aa = a.get(0), a.get(1), a.get(2), a.get(3)
   def br, bg, bb, ba = b.get(0), b.get(1), b.get(2), b.get(3)
   def r, g = ar + (br - ar) * t, ag + (bg - ag) * t
   def bl, al = ab + (bb - ab) * t, aa + (ba - aa) * t
   vec4(r, g, bl, al)
}

fn get_active_backend(): int {
   "Returns the active graphics backend constant."
   _backend
}

fn get_active_window(): any {
   "Returns the currently active window object."
   _active_win
}

fn get_active_backend_name(): str {
   "Returns a human-readable name for the active backend."
   if(_backend == BACKEND_VK){ return "vulkan" }
   return "none"
}

fn vk_available(): bool {
   "Returns true when the Vulkan renderer facade is selected or preferred."
   _backend == BACKEND_VK || _backend_pref == BACKEND_VK
}

fn backend_capabilities(): dict {
   "Returns a dictionary of supported features for the active backend."
   {"vulkan": vk_available(), "software": false, "active": get_active_backend_name()}
}

fn load_shader_agnostic(any: _defs): int {
   "Loads a shader with architecture-agnostic source definitions."
   0
}

fn load_shader(str: combined_src): int {
   "Loads a shader from a combined source string(automated transpilation)."
   load_shader_agnostic(lib_shader.transpile_shader_source(combined_src))
}

fn shader_transpile(str: combined_src): any {
   "Transpiles an agnostic shader source into backend-specific code."
   lib_shader.transpile_shader_source(combined_src)
}

fn _wait_window_ready(any: win): bool {
   if(!win){ return false }
   ; X11/GL contexts can race window realization in headless setups (e.g. xvfb).
   def b = win.get(13, 0) ; _W_BACKEND in std.os.ui.window
   if(b != 1){ return false } ; x11
   mut i = 0
   while(i < 2){
      lib_uiw.check_event(win)
      i += 1
   }
   true
}

fn _init_with_window(any: win, bool: prefer_vulkan=true): bool {
   if(!win){ return false }
   _wait_window_ready(win)
   _active_win = win
   def live_sz = lib_uiw.size(win)
   def live_w = int(live_sz.get(0, 0))
   def live_h = int(live_sz.get(1, 0))
   if(live_w > 0 && live_h > 0){ set_win_size(live_w, live_h) }
   if(!prefer_vulkan){
      if(_is_debug()){ ui_profile.print_text("[gfx] Non-Vulkan init requested, but renderer is Vulkan-only") }
      _backend = BACKEND_NONE
      _active_win = nil
      return false
   }
   if(!lib_vkr.init(win)){
      if(_is_debug()){
         ui_profile.print_text("[gfx] Vulkan init failed")
         ui_profile.print_text("[gfx] Renderer is Vulkan-only; no software fallback is available")
      }
      _backend = BACKEND_NONE
      _active_win = nil
      return false
   }
   _backend = BACKEND_VK
   _vk_cam_pos_valid = false
   if(_is_debug()){ ui_profile.print_text("[gfx] Using Native Vulkan Backend") }
   if(_start_time == 0){
      _start_time = ticks()
      _last_frame_time = _start_time
      _start_time_sec = ostime.time()
   }
   true
}

fn init_mock_surface(int: width=1280, int: height=720): any {
   "Initializes the CPU/mock backend with an offscreen framebuffer and no real window."
   if(_backend == BACKEND_VK){ lib_vkr.shutdown() }
   _vk_cam_pos_valid = false
   if(_cpu_buf != 0){ free(_cpu_buf) _cpu_buf = 0 _cpu_w = 0 _cpu_h = 0 }
   _auto_show_pending = false
   _backend = BACKEND_MOCK
   def win = {"_mock": true, "w": int(max(1, width)), "h": int(max(1, height))}
   _active_win = win
   set_win_size(int(max(1, width)), int(max(1, height)))
   if(!_cpu_ensure_surface()){
      _backend = BACKEND_NONE
      _active_win = nil
      return false
   }
   if(_start_time == 0){
      _start_time = ticks()
      _last_frame_time = _start_time
      _start_time_sec = ostime.time()
   }
   win
}

fn renderer_config(bool: vsync, bool: filter, str: vert="", str: frag="", int: msaa=1): bool {
   "Configures the graphics renderer settings(Vulkan only)."
   lib_vkr.renderer_config(vsync, filter, vert, frag, msaa)
   true
}

fn init_window(int: width, int: height, str: title, int: flags=0, bool: vsync=false, bool: filter=false, int: msaa=1): any {
   "Initializes a window with renderer config and graphics context. Returns window dict or false."
   renderer_config(vsync, filter, "", "", msaa)
   mut int: f = flags
   mut int: x = 100
   mut int: y = 100
   if(f == 0){ f = 0 }
   def explicit_hide = (f & 0x0200) != 0
   f = f | 0x80000
   window_hint(CLIENT_API, NO_API)
   def headless = ui_profile.env_truthy_cached("NY_UI_HEADLESS")
   if(headless){
      f = f | 0x0002
      f = f | 0x0200
      if(_is_debug()){ ui_profile.print_text("[gfx] Applying headless mode(HIDDEN | NO_RESIZE)") }
   }
   def auto_hide_boot = !headless && !explicit_hide
   if(auto_hide_boot){ f = f | 0x0200 }
   def win = lib_uiw.open_window(title, x, y, width, height, f)
   if(!win){ return false }
   if(!_init_with_window(win, true)){
      lib_uiw.close(win)
      return false
   }
   _auto_show_pending = auto_hide_boot
   win
}

fn set_active_window(any: win): bool {
   "Changes the target window for subsequent drawing operations."
   _init_with_window(win, true)
}

fn render_init(any: win=0): bool {
   "Ensures the graphics renderer is initialized for the specified window."
   if(_backend_pref == BACKEND_MOCK){
      if(_active_win && is_dict(_active_win) && _active_win.get("_mock", false)){ return true }
      return !!init_mock_surface(int(_last_win_w > 0.0 ? _last_win_w : 1280.0), int(_last_win_h > 0.0 ? _last_win_h : 720.0))
   }
   if(win){ return _init_with_window(win, true) }
   if(_active_win){ return _init_with_window(_active_win, true) }
   def last = lib_uiw.last()
   if(last){ return _init_with_window(last, true) }
   false
}

fn set_window_pos(any: win, int: x, int: y): bool {
   "Moves a window to the specified screen position."
   lib_uiw.move(win, x, y)
   true
}

fn close_window(): bool {
   "Shuts down graphics services and closes the active window."
   if(_backend == BACKEND_VK){ lib_vkr.shutdown() }
   _vk_cam_pos_valid = false
   if(_active_win && !_active_win.get("_mock", false)){ lib_uiw.close(_active_win) }
   _active_win = nil
   if(_cpu_buf != 0){ free(_cpu_buf) _cpu_buf = 0 _cpu_w = 0 _cpu_h = 0 }
   true
}

fn window_should_close(any: win=0): bool {
   "Returns true if the window has requested closure. Polls events automatically."
   lib_uiw.poll_events()
   if(win){ return lib_uiw.should_close(win) }
   if(!_active_win){ return true }
   lib_uiw.should_close(_active_win)
}

fn begin_drawing(): bool {
   "Begins a new drawing frame, clearing states."
   if(!_active_win){ return false }
   _shape_scratch_used = 0
   _ensure_scissor_stack()
   if(_scissor_stack.len > 0){ _scissor_stack = [] }
   if(_backend != BACKEND_MOCK && _auto_show_pending){
      if(_is_frame_debug()){ ui_profile.print_text("[gfx] begin_drawing auto_show start") }
      lib_uiw.show(_active_win)
      lib_uiw.poll_events()
      def shown = lib_uiw.get_win(_active_win)
      if(shown){ _active_win = shown }
      _auto_show_pending = false
      if(_is_frame_debug()){ ui_profile.print_text("[gfx] begin_drawing auto_show done") }
   }
   ; Only close on ESC when explicitly enabled. This prevents ESC from
   ; stealing input from TUI/terminal applications.
   if(_backend != BACKEND_MOCK && ui_profile.env_present_cached("NY_GFX_ESC_CLOSE")){ if(lib_uiw.key_down(_active_win, ui_consts.KEY_ESCAPE) || lib_uiw.key_down(_active_win, 0xFF1B)){ lib_uiw.set_should_close(_active_win, true) } }
   def now = ticks()
   _current_frame_time = (now - _last_frame_time) / 1000000000.0
   _last_frame_time = now
   _frame_time_accum += _current_frame_time
   if(_backend == BACKEND_VK){
      if(_is_frame_debug()){ ui_profile.print_text("[gfx] begin_drawing vk begin_frame enter") }
      def headless = ui_profile.env_truthy_cached("NY_UI_HEADLESS")
      if(!headless){
         def fresh = lib_uiw.get_win(_active_win)
         if(fresh){ _active_win = fresh }
      }
      ; print("GFX: begin_drawing _backend=VK device=", lib_vkr._get_device()) __print_flush()
      lib_vkr.set_frame_time_sec(_frame_time_accum)
      mut vk_begin_ok = false
      try {
         vk_begin_ok = lib_vkr.begin_frame()
      } catch err {
         ui_profile.eprint_text("[gfx:vulkan:panic] stage=" + lib_vkr.debug_stage() + " err=" + repr(err))
         panic(err)
      }
      if(!vk_begin_ok){ return false }
      if(_is_frame_debug()){ ui_profile.print_text("[gfx] begin_drawing vk begin_frame ok") }
      lib_vkr.set_unlit(true)
      if(headless && ui_profile.env_truthy_cached("NYTRIX_VK_ALLOW_HEADLESS")){ return true }
      ; Sync _active_win size to actual swapchain extent (WM may have resized us)
      def scw, sch = int(lib_vkr.get_swapchain_width()), int(lib_vkr.get_swapchain_height())
      if(scw > 0 && sch > 0){
         if(scw != int(_active_win.get("w", 0)) || sch != int(_active_win.get("h", 0))){
            _active_win["w"] = scw
            _active_win["h"] = sch
            lib_uiw._save_win(_active_win)
            set_win_size(scw, sch)
         } else {
            _last_win_w, _last_win_h = float(scw), float(sch)
         }
      }
      return true
   }
   true
}

fn begin_frame(): bool {
   "Begins a new drawing frame."
   return begin_drawing()
}

fn begin_frame_clear(any: color=BLACK): bool {
   "Begins a new frame and clears the background with the specified color."
   if(begin_drawing()){
      clear_background(color)
      return true
   }
   false
}

fn end_frame(): bool {
   "Finalizes and presents the current frame."
   return end_drawing()
}

fn end_drawing(): bool {
   "Finalizes a drawing frame and swaps buffers."
   if(!_active_win){ return false }
   if(_backend == BACKEND_VK){
      if(_is_frame_debug()){ ui_profile.print_text("[gfx] end_drawing vk enter") }
      def ok = lib_vkr.end_frame()
      if(_is_frame_debug()){ ui_profile.print_text("[gfx] end_drawing vk exit ok=" + to_str(ok)) }
      return ok
   }
   true
}

fn begin_mode_3d(any: camera): bool {
   "Enters 3D rendering mode using the specified camera."
   if(!_active_win){ return false }
   if(_backend == BACKEND_VK){
      lib_vkr.set_unlit(false)
      lib_vkr.set_mask(0)
   }
   set_camera(camera)
   true
}

fn set_projection(any: mat): bool {
   "Sets the active projection matrix."
   _proj_matrix = mat
   _sync_combined_vp()
   true
}

fn set_view(any: mat): bool {
   "Sets the active view matrix."
   _view_matrix = mat
   _sync_combined_vp()
   true
}

fn _send_vk_cam_pos(f64: x, f64: y, f64: z): bool {
   if(_backend != BACKEND_VK){ return false }
   def nx, ny = float(x), float(y)
   def nz = float(z)
   if(_vk_cam_pos_valid && nx == _vk_cam_pos_x && ny == _vk_cam_pos_y && nz == _vk_cam_pos_z){ return false }
   _vk_cam_pos_x, _vk_cam_pos_y = nx, ny
   _vk_cam_pos_z = nz
   _vk_cam_pos_valid = true
   lib_vkr.set_cam_pos(nx, ny, nz)
   true
}

fn set_camera(any: cam): bool {
   "Sets both view and projection from a camera object."
   if(!cam){ return false }
   if(_active_win){
      mut live_w, live_h = 0, 0
      if(_backend == BACKEND_VK){
         live_w, live_h = int(lib_vkr.get_swapchain_width()), int(lib_vkr.get_swapchain_height())
      } else {
         def live_sz = get_framebuffer_size()
         live_w, live_h = int(live_sz.get(0, 0)), int(live_sz.get(1, 0))
      }
      if(live_w > 0 && live_h > 0){ set_win_size(live_w, live_h) }
   }
   def pos = cam.get(0)
   _send_vk_cam_pos(pos.get(0, 0), pos.get(1, 0), pos.get(2, 0))
   def target = cam.get(1)
   def up = cam.get(2)
   def fovy = cam.get(3, _proj_fov)
   mut aspect = 1.0
   if(_last_win_h > 0.0){ aspect = _last_win_w / _last_win_h }
   mat4_look_at_into(pos, target, up, _scratch_view)
   _view_matrix = _scratch_view
   if(_scene_proj_mode == 1){ ;; SCENE ORTHO
      def z = _proj_zoom
      mat4_ortho_into(-z * aspect, z * aspect, -z, z, -1000.0, 1000.0, _scratch_proj)
   } else { ;; PERSPECTIVE
      mat4_perspective_into(fovy * PI / 180.0, aspect, 0.1, 1000.0, _scratch_proj)
   }
   _proj_matrix = _scratch_proj
   _sync_combined_vp()
   true
}

fn set_win_size(int: w, int: h): bool {
   "Updates cached window size used for projection calculations."
   if(w > 0){ _last_win_w = float(w) }
   if(h > 0){ _last_win_h = float(h) }
   if(_backend == BACKEND_VK && w > 0 && h > 0){ if(int(lib_vkr.get_swapchain_width()) != int(w) || int(lib_vkr.get_swapchain_height()) != int(h)){ lib_vkr.notify_window_resize(int(w), int(h)) } }
   true
}

fn set_projection_mode(int: mode): bool {
   "Sets the active 3D scene projection mode: 0 for Perspective, 1 for Orthographic."
   _scene_proj_mode = mode
   true
}

fn set_ortho_zoom(f64: zoom): bool {
   "Sets the zoom level for orthographic projection."
   _proj_zoom = float(zoom)
   true
}

fn _sync_combined_vp(): bool {
   if(!_proj_matrix){ _proj_matrix = _scratch_proj }
   if(!_view_matrix){ _view_matrix = _scratch_view }
   mat4_mul_into(_proj_matrix, _view_matrix, _scratch_mvp)
   _mvp_matrix = _scratch_mvp
   if(_backend == BACKEND_VK){ lib_vkr.set_mvp(_scratch_mvp) }
   true
}

fn end_mode_3d(): bool {
   "Exits 3D rendering mode, restoring the 2D default projection."
   _proj_matrix = nil
   _view_matrix = nil
   _mvp_matrix = nil
   if(_backend == BACKEND_VK){
      lib_vkr._update_default_mvp(_active_win)
      _reset_material_state()
      lib_vkr.set_unlit(true)
   }
   true
}

fn clear_background(any: color=BLACK): bool {
   "Fills the background with the specified color."
   if(_backend == BACKEND_VK){
      lib_vkr.clear(_color_at(color, 0, 0.0), _color_at(color, 1, 0.0),
      _color_at(color, 2, 0.0), _color_at(color, 3, 1.0))
   } elif(_backend == BACKEND_MOCK){
      _cpu_clear(_cpu_pack_color(
            _color_at(color, 0, 0.0),
            _color_at(color, 1, 0.0),
            _color_at(color, 2, 0.0),
            _color_at(color, 3, 1.0)
      ))
   }
   true
}

fn get_frame_time(): f64 {
   "Returns the duration of the last frame in seconds."
   _current_frame_time
}

fn get_delta_time(): f64 {
   "Returns the duration of the last frame in seconds."
   _current_frame_time
}

fn get_time(): f64 {
   "Returns the total elapsed time in seconds since library initialization."
   if(_start_time_sec == 0.0){ _start_time_sec = ostime.time() }
   ostime.time() - _start_time_sec
}

fn _renderer_frame_stats_pack(
   dict: stats,
   int: draws=0,
   int: dynamic_draws=0,
   int: static_draws=0,
   int: indexed_draws=0,
   int: flushes=0,
   int: pipeline_binds=0,
   int: descriptor_binds=0,
   int: submitted_vertices=0,
   int: begin_us=0,
   int: syncpc_us=0,
   int: flush_us=0,
   int: end_us=0
) : dict {
   dict_merge(stats, {
         "draws": draws,
         "dynamic_draws": dynamic_draws,
         "static_draws": static_draws,
         "indexed_draws": indexed_draws,
         "flushes": flushes,
         "pipeline_binds": pipeline_binds,
         "descriptor_binds": descriptor_binds,
         "submitted_vertices": submitted_vertices,
         "begin_ms": float(begin_us) / 1000.0,
         "syncpc_ms": float(syncpc_us) / 1000.0,
         "flush_ms": float(flush_us) / 1000.0,
         "end_ms": float(end_us) / 1000.0,
         "cpu_ms": float(begin_us + end_us) / 1000.0,
   })
}

fn renderer_frame_stats(): dict {
   "Returns the most recent renderer counters for the active backend."
   mut stats = {"backend": get_active_backend_name()}
   if(_backend == BACKEND_VK){
      stats = _renderer_frame_stats_pack(
         stats,
         _last_frame_draw_calls,
         _last_frame_dynamic_draw_calls,
         _last_frame_static_draw_calls,
         _last_frame_indexed_draw_calls,
         _last_flush_total,
         _last_pipeline_bind_count,
         _last_descriptor_bind_count,
         _last_submitted_vertices,
         _last_frame_begin_cpu_us,
         _last_frame_sync_pc_cpu_us,
         _last_frame_flush_cpu_us,
         _last_frame_end_cpu_us
      )
      stats = dict_merge(stats, {
            "prim_rect_quads": _last_prim_rect_quads,
            "prim_outline_quads": _last_prim_outline_quads,
            "prim_line_quads": _last_prim_line_quads,
            "prim_raw_lines": _last_prim_raw_lines,
            "prim_raw_points": _last_prim_raw_points,
            "prim_text_calls": _last_prim_text_calls,
            "prim_text_glyphs": _last_prim_text_glyphs,
      })
      return stats
   }
   _renderer_frame_stats_pack(stats)
}

comptime template _vk_only_void0(name, doc, call_fn){
   fn ${name}(){
      doc
      if(_backend == BACKEND_VK){ call_fn() }
   }
}

comptime template _vk_only_void1(name, doc, call_fn){
   fn ${name}(arg0){
      doc
      if(_backend == BACKEND_VK){ call_fn(arg0) }
   }
}

comptime template _vk_only_void3(name, doc, call_fn){
   fn ${name}(arg0, arg1, arg2){
      doc
      if(_backend == BACKEND_VK){ call_fn(arg0, arg1, arg2) }
   }
}

fn draw_vertices(ptr: p, int: count, int: tex_id=-1): bool {
   "Fast-path: submits a raw vertex buffer for drawing."
   if(_backend == BACKEND_VK){ lib_vkr.draw_vertices(p, count, tex_id) return true }
   false
}

fn draw_lines_raw(ptr: p, int: count, f64: width=1.0): bool {
   "Fast-path: submits a raw vertex buffer for line drawing."
   if(_backend == BACKEND_VK){ lib_vkr.draw_lines_raw(p, count, width) return true }
   false
}

fn color_pack(f64: r, f64: g, f64: b, f64: a=1.0): int {
   "Packs RGBA floats into a native integer format for fast drawing."
   lib_vkr._pack_color(r, g, b, a)
}

fn _pack_color_from(any: color, f64: r=1.0, f64: g=1.0, f64: b=1.0, f64: a=1.0, bool: preserve_int=false): int {
   if(preserve_int && is_int(color)){ return color }
   lib_vkr._pack_color(_color_at(color, 0, r), _color_at(color, 1, g), _color_at(color, 2, b), _color_at(color, 3, a))
}

comptime emit _vk_only_void1(set_unlit, "Toggles shading for subsequent draw calls.", lib_vkr.set_unlit)
comptime emit _vk_only_void3(set_material, "Sets PBR material parameters for subsequent draw calls.", lib_vkr.set_material)

fn set_cam_pos(f64: x, f64: y, f64: z): bool {
   "Sets camera world-space position for PBR specular calculation."
   _send_vk_cam_pos(x, y, z)
}

comptime emit _vk_only_void1(set_env_tex, "Sets the active environment/sky texture for lit shading.", lib_vkr.set_env_tex)
comptime emit _vk_only_void1(set_env_spec_tex, "Sets the active specular IBL texture for lit shading.", lib_vkr.set_env_spec_tex)

fn push_vertex(
   ptr: p,
   f64: x,
   f64: y,
   f64: z,
   f64: u,
   f64: v,
   any: color,
   int: tex_id=0,
   f64: nx=0.0,
   f64: ny=0.0,
   f64: nz=1.0
): bool {
   "Internal helper to store a vertex into a raw buffer."
   lib_vkr.__vkr_push_vertex(p, x, y, z, u, v, color, tex_id, nx, ny, nz)
   true
}

comptime emit _vk_only_void1(set_view_proj, "Sets the View-Projection matrix for 3D rendering.", lib_vkr.set_mvp)

fn set_ortho(f64: l, f64: r, f64: b, f64: t, f64: n, f64: f): bool {
   "Sets the active projection to a standard 3D orthographic projection."
   _proj_matrix = mat4_ortho(l, r, b, t, n, f)
   _sync_combined_vp()
   true
}

fn set_ortho_2d(f64: l, f64: r, f64: b, f64: t): bool {
   "Sets the active projection to a Y-down orthographic projection for 2D(resets view/model)."
   mut bb, tt = b, t
   if(bb < tt){ def tmp = bb bb = tt tt = tmp } ; UI coords: Y-down
   if(!_last_ortho2d_valid
      || l != _last_ortho2d_l
      || r != _last_ortho2d_r
      || bb != _last_ortho2d_b
      || tt != _last_ortho2d_t){
      _scratch_proj = mat4_ortho(l, r, bb, tt, -1.0, 1.0)
      _last_ortho2d_l = l
      _last_ortho2d_r = r
      _last_ortho2d_b = bb
      _last_ortho2d_t = tt
      _last_ortho2d_valid = true
   }
   _proj_matrix = _scratch_proj
   _view_matrix = _scratch_ident
   set_model_matrix(_scratch_ident)
   set_unlit(true)
   if(_backend == BACKEND_VK){ lib_vkr.set_ortho(l, r, bb, tt, -1.0, 1.0) } else { _sync_combined_vp() }
   true
}

fn set_perspective(f64: fovy_deg, f64: aspect, f64: near, f64: far): bool {
   "Sets the active projection to 3D perspective(fovy in degrees)."
   _scratch_proj = mat4_perspective(fovy_deg * PI / 180.0, aspect, near, far)
   _proj_matrix = _scratch_proj
   _sync_combined_vp()
   true
}

def VERTEX_STRIDE = 64

fn compile_shader(str: source, str: stage_ext): any {
   "Compiles GLSL source string and creates a backend shader module."
   if(_debug_gfx_enabled){ ui_profile.print_text(f"[gfx:shader] compile stage='{stage_ext}'") }
   if(_backend == BACKEND_VK){ return lib_vkr.create_shader_module_from_source(source, stage_ext) }
   0
}

fn create_pipeline(
   any: vert_mod,
   any: frag_mod,
   int: topology=3,
   int: depth_test=1,
   int: depth_write=1,
   int: cull_mode=0,
   int: front_face=0,
   int: depth_bias=0,
   int: depth_clamp=0
): any {
   "Creates a custom graphics pipeline from shader modules."
   if(_backend == BACKEND_VK){ return lib_vkr.create_pipeline(vert_mod, frag_mod, topology, depth_test, depth_write, cull_mode, front_face, depth_bias, depth_clamp) }
   0
}

comptime emit _vk_only_void1(bind_pipeline, "Binds a custom graphics pipeline for subsequent draw calls.", lib_vkr.bind_pipeline)

fn reset_pipeline(): bool {
   "Restores the engine's default graphics pipeline."
   if(_backend == BACKEND_VK){ lib_vkr.bind_pipeline(lib_vkr._get_default_pipeline()) }
   _backend == BACKEND_VK
}

fn push_constants(ptr: p, int: size, int: offset=0): bool {
   "Pushes custom data to the current pipeline's push constants."
   if(_backend == BACKEND_VK){ lib_vkr.push_constants(p, size, offset) return true }
   false
}

fn store_mat4_cm(ptr: p, any: mat): bool {
   "Stores a 16-float or tagged 18-float column-major mat4 into raw memory."
   vk_utils.store_mat4_cm_raw(p, mat, false)
   true
}

comptime emit _vk_only_void3(blit_buffer, "Blits a raw pixel buffer to the screen.", lib_vkr.blit_buffer)
comptime emit _vk_only_void0(clear_depth, "Explicitly clears the depth buffer. Useful for drawing on top of existing scene data.", lib_vkr.clear_depth)

fn set_clear_color(any: color): bool {
   "Sets the clear color for the next frame."
   if(_backend == BACKEND_VK){
      lib_vkr.set_clear_color(_color_at(color, 0, 0.0), _color_at(color, 1, 0.0),
      _color_at(color, 2, 0.0), _color_at(color, 3, 1.0))
   } elif(_backend == BACKEND_MOCK){
      def r, g = int(_color_at(color, 0, 0.0) * 255.0), int(_color_at(color, 1, 0.0) * 255.0)
      def b, a = int(_color_at(color, 2, 0.0) * 255.0), int(_color_at(color, 3, 1.0) * 255.0)
      _cpu_clear_color = bor(bor(bor(bshl(a, 24), bshl(r, 16)), bshl(g, 8)), b)
   }
   true
}

@inline
fn _color_at(any: color, int: idx, f64: fallback): f64 {
   if(is_list(color) && color.len > idx){ return float(color.get(idx, fallback)) }
   if(is_int(color)){
      if(idx == 0){ return float(color & 255) / 255.0 }
      if(idx == 1){ return float((color >> 8) & 255) / 255.0 }
      if(idx == 2){ return float((color >> 16) & 255) / 255.0 }
      if(idx == 3){ return float((color >> 24) & 255) / 255.0 }
   }
   fallback
}

fn _shape_sdf_color(any: color): vec4 {
   mut r, g, b, a = 1.0, 1.0, 1.0, 1.0
   if(is_int(color)){
      a, r = float((color >> 24) & 255) / 255.0, float((color >> 16) & 255) / 255.0
      g, b = float((color >> 8)  & 255) / 255.0, float(color & 255) / 255.0
   } else {
      r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   }
   [r, g, b, a]
}

fn _store_v3_c4(ptr: buf, int: vi, f64: x, f64: y, f64: z, f64: r, f64: g, f64: b, f64: a): bool {
   def off = vi * 28
   ; Vertex pack is 7x f32; write IEEE float payloads explicitly.
   store32_f32(buf, x, off)
   store32_f32(buf, y, off + 4)
   store32_f32(buf, z, off + 8)
   store32_f32(buf, r, off + 12)
   store32_f32(buf, g, off + 16)
   store32_f32(buf, b, off + 20)
   store32_f32(buf, a, off + 24)
   true
}

fn _store_fan_tri_c4(
   ptr: buf,
   int: vi,
   f64: cx,
   f64: cy,
   f64: x0,
   f64: y0,
   f64: x1,
   f64: y1,
   f64: r,
   f64: g,
   f64: b,
   f64: a
) : int {
   _store_v3_c4(buf, vi, cx, cy, 0.0, r, g, b, a) _store_v3_c4(buf, vi + 1, x0, y0, 0.0, r, g, b, a)
   _store_v3_c4(buf, vi + 2, x1, y1, 0.0, r, g, b, a)
   vi + 3
}

fn _grid_cache_key(int: slices, f64: spacing, f64: thickness, any: minor_col, any: major_col): str {
   def minor = _pack_color_from(minor_col, 0.3, 0.3, 0.3, 0.55)
   def major = _pack_color_from(major_col, 0.58, 0.62, 0.7, 0.95)
   f"{slices}:{spacing}:{thickness}:{minor}:{major}"
}

fn _grid_write_thick_line(
   ptr: p,
   int: idx,
   f64: x1,
   f64: y1,
   f64: z1,
   f64: x2,
   f64: y2,
   f64: z2,
   f64: thickness,
   int: color_u32
) : int {
   "Internal: Writes a thick 3D line quad into a vertex buffer."
   def dx, dy, dz = float(x2) - float(x1), float(y2) - float(y1), float(z2) - float(z1)
   def l = sqrt(dx*dx + dy*dy + dz*dz)
   if(l == 0.0){ return idx }
   mut nx, ny = -dz / l * (float(thickness) * 0.5), 0.0
   mut nz =  dx / l * (float(thickness) * 0.5)
   if(abs(dx) < 0.001 && abs(dz) < 0.001){ nx, nz = float(thickness)*0.5, 0.0 }
   def f1x, f1y, f1z = float(x1), float(y1), float(z1)
   def f2x, f2y, f2z = float(x2), float(y2), float(z2)
   lib_vkr.__vkr_push_vertex(p + (idx + 0) * VERTEX_STRIDE, f1x+nx, f1y+ny, f1z+nz, 0.0, 0.0, color_u32, 0)
   lib_vkr.__vkr_push_vertex(p + (idx + 1) * VERTEX_STRIDE, f1x-nx, f1y-ny, f1z-nz, 0.0, 0.0, color_u32, 0)
   lib_vkr.__vkr_push_vertex(p + (idx + 2) * VERTEX_STRIDE, f2x-nx, f2y-ny, f2z-nz, 0.0, 0.0, color_u32, 0)
   lib_vkr.__vkr_push_vertex(p + (idx + 3) * VERTEX_STRIDE, f1x+nx, f1y+ny, f1z+nz, 0.0, 0.0, color_u32, 0)
   lib_vkr.__vkr_push_vertex(p + (idx + 4) * VERTEX_STRIDE, f2x-nx, f2y-ny, f2z-nz, 0.0, 0.0, color_u32, 0)
   lib_vkr.__vkr_push_vertex(p + (idx + 5) * VERTEX_STRIDE, f2x+nx, f2y+ny, f2z+nz, 0.0, 0.0, color_u32, 0)
   idx + 6
}

fn _prepare_draw(): bool {
   if(_backend == BACKEND_VK || _backend == BACKEND_MOCK){ return true }
   false
}

fn _shape_scratch_alloc(int: bytes): ptr {
   if(bytes <= 0){ return 0 }
   if(_shape_scratch == 0 || (_shape_scratch_used + bytes) > _shape_scratch_cap){
      mut new_cap = max(65536, _shape_scratch_cap)
      while(new_cap < (_shape_scratch_used + bytes)){ new_cap *= 2 }
      def next = _shape_scratch ? realloc(_shape_scratch, new_cap) : malloc(new_cap)
      if(!next){ return 0 }
      _shape_scratch = next
      _shape_scratch_cap = new_cap
   }
   def ptr = _shape_scratch + _shape_scratch_used
   _shape_scratch_used += bytes
   ptr
}

fn _draw_triangles_impl(ptr: verts, int: tri_count, bool: owns=true): bool {
   if(verts == 0 || tri_count <= 0){ return false }
   if(!_prepare_draw()){ if(owns){ free(verts) } return false }
   def draw_vk = _backend == BACKEND_VK
   def draw_mock = _backend == BACKEND_MOCK
   if(draw_vk || draw_mock){
      if(draw_mock){ _cpu_mul_active_mvp_into(_cull_mvp) }
      mut i = 0
      while(i < tri_count){
         def off = i * 3 * 28
         def x0, y0 = load32_f32(verts, off + 0), load32_f32(verts, off + 4)
         def x1, y1 = load32_f32(verts, off + 28), load32_f32(verts, off + 32)
         def x2, y2 = load32_f32(verts, off + 56), load32_f32(verts, off + 60)
         def z0, z1 = load32_f32(verts, off + 8), load32_f32(verts, off + 36)
         def z2 = load32_f32(verts, off + 64)
         def cr, cg = load32_f32(verts, off + 12), load32_f32(verts, off + 16)
         def cb, ca = load32_f32(verts, off + 20), load32_f32(verts, off + 24)
         if(draw_vk){
            lib_vkr.draw_triangle_3d(x0, y0, z0, x1, y1, z1, x2, y2, z2, cr, cg, cb, ca)
            i += 1
            continue
         }
         mut sx0, sy0 = x0, y0
         mut sx1, sy1 = x1, y1
         mut sx2, sy2 = x2, y2
         if(_mvp_matrix){
            def p0, p1 = _cpu_project_vertex(x0, y0, z0, _cull_mvp), _cpu_project_vertex(x1, y1, z1, _cull_mvp)
            def p2 = _cpu_project_vertex(x2, y2, z2, _cull_mvp)
            if(!p0 || !p1 || !p2){ i += 1 continue }
            sx0, sy0 = p0.get(0, 0.0), p0.get(1, 0.0)
            sx1, sy1 = p1.get(0, 0.0), p1.get(1, 0.0)
            sx2, sy2 = p2.get(0, 0.0), p2.get(1, 0.0)
         }
         _cpu_draw_triangle(sx0, sy0, sx1, sy1, sx2, sy2, _cpu_pack_color(cr, cg, cb, ca))
         i += 1
      }
   }
   if(owns){ free(verts) }
   true
}

fn _to_v3(any: v): list {
   if(is_list(v)){ return [v.get(0, 0.0), v.get(1, 0.0), v.get(2, 0.0)] }
   return [0.0, 0.0, 0.0]
}

fn draw_triangles(list: vertices, any: color): bool {
   "Draws a list of triangles with a single color."
   if(!is_list(vertices)){ return false }
   def tri_count = int(vertices.len / 3)
   if(tri_count < 1){ return false }
   def vertex_count = tri_count * 3
   def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   if(_backend == BACKEND_VK){
      mut i = 0
      while(i < tri_count){
         def j = i * 3
         def v0, v1, v2 = vertices[j], vertices[j + 1], vertices[j + 2]
         lib_vkr.draw_triangle_3d(
            v0.get(0,0.0), v0.get(1,0.0), v0.get(2,0.0),
            v1.get(0,0.0), v1.get(1,0.0), v1.get(2,0.0),
         v2.get(0,0.0), v2.get(1,0.0), v2.get(2,0.0), r, g, b, a)
         i += 1
      }
      return true
   }
   def verts = malloc(vertex_count * 28)
   if(!verts){ return false }
   mut i = 0
   while(i < vertex_count){
      def v = _to_v3(vertices[i])
      _store_v3_c4(verts, i, v.get(0, 0.0), v.get(1, 0.0), v.get(2, 0.0), r, g, b, a)
      i += 1
   }
   _draw_triangles_impl(verts, tri_count)
}

fn draw_polyline(list: points, any: color, f64: thickness=0.02, bool: closed=false): bool {
   "Draws a sequence of connected lines."
   if(!is_list(points) || points.len < 2){ return false }
   def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   if(_backend == BACKEND_VK){
      mut i = 0
      while(i + 1 < points.len){
         def p0, p1 = points[i], points[i + 1]
         lib_vkr.draw_line_3d(p0.get(0,0.0), p0.get(1,0.0), p0.get(2,0.0),
         p1.get(0,0.0), p1.get(1,0.0), p1.get(2,0.0), thickness, r, g, b, a)
         i += 1
      }
      if(closed){
         def p0, p1 = points[points.len - 1], points[0]
         lib_vkr.draw_line_3d(p0.get(0,0.0), p0.get(1,0.0), p0.get(2,0.0),
         p1.get(0,0.0), p1.get(1,0.0), p1.get(2,0.0), thickness, r, g, b, a)
      }
      return true
   }
   mut i = 0
   while(i + 1 < points.len){
      _draw_line_impl(points[i], points[i + 1], color, thickness)
      i += 1
   }
   if(closed){ _draw_line_impl(points[points.len - 1], points[0], color, thickness) }
   true
}

fn _draw_tri_quad_vk(list: v1, list: v2, list: v3, any: v4, any: color): bool {
   def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   if(v4){
      lib_vkr.draw_quad_3d(
         v1.get(0,0.0), v1.get(1,0.0), v1.get(2,0.0),
         v2.get(0,0.0), v2.get(1,0.0), v2.get(2,0.0),
         v3.get(0,0.0), v3.get(1,0.0), v3.get(2,0.0),
      v4.get(0,0.0), v4.get(1,0.0), v4.get(2,0.0), r, g, b, a)
   } else {
      lib_vkr.draw_triangle_3d(
         v1.get(0,0.0), v1.get(1,0.0), v1.get(2,0.0),
         v2.get(0,0.0), v2.get(1,0.0), v2.get(2,0.0),
      v3.get(0,0.0), v3.get(1,0.0), v3.get(2,0.0), r, g, b, a)
   }
   true
}

fn draw_triangle(list: v1, list: v2, list: v3, any: color): bool {
   "Draws a single filled triangle."
   if(_backend == BACKEND_VK){ return _draw_tri_quad_vk(v1, v2, v3, 0, color) }
   draw_triangles([v1, v2, v3], color)
}

fn draw_quad(list: v1, list: v2, list: v3, list: v4, any: color): bool {
   "Draws a single filled quadrilateral."
   if(_backend == BACKEND_VK){ return _draw_tri_quad_vk(v1, v2, v3, v4, color) }
   draw_triangles([v1, v2, v3, v1, v3, v4], color)
}

fn _draw_line_impl(list: start, list: finish, any: color, f64: thickness): bool {
   if(thickness <= 0){ thickness = 0.02 }
   if(_backend == BACKEND_VK){
      def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
      lib_vkr.draw_line_3d(
         start.get(0,0.0), start.get(1,0.0), start.get(2,0.0),
         finish.get(0,0.0), finish.get(1,0.0), finish.get(2,0.0),
      thickness, r, g, b, a)
      return true
   } elif(_backend == BACKEND_MOCK && _mvp_matrix){
      _cpu_line_world(_to_v3(start), _to_v3(finish), _cpu_pack_color_from(color), max(thickness * float(min(_cpu_w, _cpu_h)) * 0.5, 1.0))
      return true
   }
   def ax, bx = _to_v3(start), _to_v3(finish)
   def dir = sub(bx, ax)
   if(magnitude(dir) <= 0.0000001){ return false }
   mut n = cross3(dir, vec3(0.0, 0.0, 1.0))
   if(magnitude(n) <= 0.0000001){ n = cross3(dir, vec3(0.0, 1.0, 0.0)) }
   n = scale(normalize(n), thickness / 2.0)
   def v1, v2 = add(ax, n), sub(ax, n)
   def v3, v4 = add(bx, n), sub(bx, n)
   draw_quad(v1, v2, v4, v3, color)
}

fn draw_line(list: start, list: finish, any: color, f64: thickness=0.02): bool {
   "Draws a thick line between two 3D positions."
   _draw_line_impl(start, finish, color, thickness)
}

fn draw_line_fast(f64: x1, f64: y1, f64: x2, f64: y2, f64: thickness, int: c_u32): bool {
   "Draws a packed-color 2D line without color conversion."
   if(_backend == BACKEND_VK){
      lib_vkr.draw_line_fast(x1, y1, x2, y2, thickness, c_u32)
      return true
   }
   draw_line_2d(x1, y1, x2, y2, c_u32, thickness)
}

fn draw_line_2d(f64: x1, f64: y1, f64: x2, f64: y2, any: color, f64: thickness=0.02): bool {
   "Draws a thick line between two 2D screen positions."
   if(_backend == BACKEND_VK){
      if(is_int(color)){ lib_vkr.draw_line_fast(x1, y1, x2, y2, thickness, color) } else {
         lib_vkr.draw_line(x1, y1, x2, y2, thickness, _color_at(color, 0, 1.0), _color_at(color, 1, 1.0),
         _color_at(color, 2, 1.0), _color_at(color, 3, 1.0))
      }
      return true
   }
   _draw_line_impl([x1, y1, 0.0], [x2, y2, 0.0], color, thickness)
}

fn draw_rect(f64: x, f64: y, f64: w, f64: h, any: color): bool {
   "Draws a filled rectangle on screen. Automatically uses the fastest path available."
   if(_backend == BACKEND_VK){
      lib_vkr.draw_rect_fast(x, y, w, h, _pack_color_from(color, 1.0, 1.0, 1.0, 1.0, true))
      return true
   }
   draw_quad([x, y, 0.0], [x+w, y, 0.0], [x+w, y+h, 0.0], [x, y+h, 0.0], color)
}

fn draw_rect_sharp(f64: x, f64: y, f64: w, f64: h, any: color, f64: r=4.0): bool {
   "Draws a rectangle with 45-degree chamfered corners."
   if(r <= 0.0){ return draw_rect(x, y, w, h, color) }
   def rad = (r > min(w, h) / 2.0) ? min(w, h) / 2.0 : r
   if(_backend == BACKEND_VK){
      def cr, cg = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      def cb, ca = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
      lib_vkr.draw_chamfer_rect_2d(x, y, w, h, rad, cr, cg, cb, ca)
      return true
   }
   draw_rect(x + rad, y, w - rad * 2.0, h, color)
   draw_rect(x, y + rad, rad, h - rad * 2.0, color)
   draw_rect(x + w - rad, y + rad, rad, h - rad * 2.0, color)
   draw_triangle([x, y + rad, 0.0], [x + rad, y + rad, 0.0], [x + rad, y, 0.0], color)
   draw_triangle([x + w - rad, y, 0.0], [x + w - rad, y + rad, 0.0], [x + w, y + rad, 0.0], color)
   draw_triangle([x + w, y + h - rad, 0.0], [x + w - rad, y + h - rad, 0.0], [x + w - rad, y + h, 0.0], color)
   draw_triangle([x + rad, y + h, 0.0], [x + rad, y + h - rad, 0.0], [x, y + h - rad, 0.0], color)
   true
}

fn draw_rect_rounded(f64: x, f64: y, f64: w, f64: h, f64: r, any: color, int: segments=256): bool {
   "Draws a rounded rectangle using proper quarter-corner sectors."
   draw_rounded_rectangle(x, y, w, h, r, color, segments)
}

fn draw_rect_tex(f64: x, f64: y, f64: w, f64: h, int: tex_id, f64: r, f64: g, f64: b, f64: a): bool {
   "Draws a textured rectangle on screen."
   if(_backend == BACKEND_VK){ lib_vkr.draw_rect_tex(x, y, w, h, tex_id, r, g, b, a) return true }
   false
}

fn draw_rect_tex_uv(
   f64: x,
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
   f64: a
): bool {
   "Draws a textured rectangle with custom UV coordinates."
   if(_backend == BACKEND_VK){ lib_vkr.draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a) return true }
   false
}

fn draw_rectangle_lines(f64: x, f64: y, f64: w, f64: h, any: color, f64: thickness=0.02): bool {
   "Draws the outline of a rectangle."
   if(_backend == BACKEND_VK){
      if(float(thickness) == 1.0){
         lib_vkr.draw_rect_outline_fast(x, y, w, h, _pack_color_from(color, 1.0, 1.0, 1.0, 1.0, true))
         return true
      }
      def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
      lib_vkr.draw_rect_lines_2d(x, y, w, h, thickness, r, g, b, a)
      return true
   }
   draw_line_2d(x, y, x + w, y, color, thickness)
   draw_line_2d(x + w, y, x + w, y + h, color, thickness)
   draw_line_2d(x + w, y + h, x, y + h, color, thickness)
   draw_line_2d(x, y + h, x, y, color, thickness)
   true
}

fn _draw_filled_fan_fallback(
   f64: cx,
   f64: cy,
   f64: rx,
   f64: ry,
   int: segments,
   f64: rot,
   f64: r,
   f64: g,
   f64: b,
   f64: alpha
) : bool {
   segments = max(3, int(segments))
   def verts = _shape_scratch_alloc(segments * 84)
   if(!verts){ return false }
   mut vi, i = 0, 0
   while(i < segments){
      def a0, a1 = rot + (float(i) / float(segments)) * TAU, rot + (float(i + 1) / float(segments)) * TAU
      vi = _store_fan_tri_c4(verts, vi, cx, cy,
         cx + cos(a0) * rx, cy + sin(a0) * ry,
         cx + cos(a1) * rx, cy + sin(a1) * ry,
      r, g, b, alpha)
      i += 1
   }
   _draw_triangles_impl(verts, segments, false)
   true
}

fn draw_circle(f64: cx, f64: cy, f64: radius, any: color, int: segments=256): bool {
   "Draws a filled circle. Reuses edge points to minimize trig calls."
   if(radius <= 0){ return false }
   def rgba = _shape_sdf_color(color)
   def r, g = float(rgba.get(0, 1.0)), float(rgba.get(1, 1.0))
   def b, a = float(rgba.get(2, 1.0)), float(rgba.get(3, 1.0))
   if(_backend == BACKEND_VK){
      lib_vkr.draw_circle_sdf(cx, cy, radius, r, g, b, a)
      return true
   }
   _draw_filled_fan_fallback(cx, cy, radius, radius, segments, 0.0, r, g, b, a)
}

fn _draw_ring_sector_fallback(
   f64: cx,
   f64: cy,
   f64: inner_radius,
   f64: outer_radius,
   f64: start_rad,
   f64: span_rad,
   int: steps,
   f64: r,
   f64: g,
   f64: b,
   f64: a
): bool {
   steps = max(1, int(steps))
   def tri_count = steps * 2
   def verts = _shape_scratch_alloc(tri_count * 84)
   if(!verts){ return false }
   mut vi, i = 0, 0
   while(i < steps){
      def a0 = start_rad + (span_rad * float(i) / float(steps))
      def a1 = start_rad + (span_rad * float(i + 1) / float(steps))
      def ix0, iy0 = cx + cos(a0) * inner_radius, cy + sin(a0) * inner_radius
      def ix1, iy1 = cx + cos(a1) * inner_radius, cy + sin(a1) * inner_radius
      def ox0, oy0 = cx + cos(a0) * outer_radius, cy + sin(a0) * outer_radius
      def ox1, oy1 = cx + cos(a1) * outer_radius, cy + sin(a1) * outer_radius
      _store_v3_c4(verts, vi, ox0, oy0, 0.0, r, g, b, a) _store_v3_c4(verts, vi + 1, ix0, iy0, 0.0, r, g, b, a)
      _store_v3_c4(verts, vi + 2, ox1, oy1, 0.0, r, g, b, a) _store_v3_c4(verts, vi + 3, ix0, iy0, 0.0, r, g, b, a)
      _store_v3_c4(verts, vi + 4, ix1, iy1, 0.0, r, g, b, a) _store_v3_c4(verts, vi + 5, ox1, oy1, 0.0, r, g, b, a)
      vi += 6
      i += 1
   }
   _draw_triangles_impl(verts, tri_count, false)
   true
}

fn draw_ring(f64: cx, f64: cy, f64: inner_radius, f64: outer_radius, any: color, int: segments=256): bool {
   "Draws a filled ring/annulus using SDF for Vulkan or triangle segments for fallback."
   if(inner_radius < 0){ inner_radius = 0 }
   if(outer_radius <= inner_radius){ return false }
   def rgba = _shape_sdf_color(color)
   def r, g = float(rgba.get(0, 1.0)), float(rgba.get(1, 1.0))
   def b, a = float(rgba.get(2, 1.0)), float(rgba.get(3, 1.0))
   if(_backend == BACKEND_VK){
      lib_vkr.draw_ring_sdf(cx, cy, inner_radius, outer_radius, r, g, b, a)
      return true
   }
   segments = max(3, int(segments))
   _draw_ring_sector_fallback(cx, cy, inner_radius, outer_radius, 0.0, TAU, segments, r, g, b, a)
}

fn draw_circle_lines(f64: cx, f64: cy, f64: radius, any: color, f64: thickness=0.02, int: segments=256): bool {
   "Draws the outline of a circle."
   draw_ring(cx, cy, max(0.0, radius - thickness / 2.0), radius + thickness / 2.0, color, segments)
}

fn _draw_filled_fan_2d(f64: cx, f64: cy, f64: rx, f64: ry, int: segments, any: color, f64: rot=0.0): bool {
   if(rx <= 0 || ry <= 0){ return false }
   segments = max(3, int(segments))
   def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def b, alpha = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   if(_backend == BACKEND_VK){
      lib_vkr.draw_fan_2d(cx, cy, rx, ry, segments, rot, TAU, r, g, b, alpha)
      return true
   }
   _draw_filled_fan_fallback(cx, cy, rx, ry, segments, rot, r, g, b, alpha)
}

fn draw_polygon(f64: cx, f64: cy, int: sides, f64: radius, any: color, f64: rotation_deg=0.0): bool {
   "Draws a regular filled polygon."
   _draw_filled_fan_2d(cx, cy, radius, radius, max(3, int(sides)), color, rotation_deg * PI / 180.0)
}

fn draw_ellipse(f64: cx, f64: cy, f64: rx, f64: ry, any: color, int: segments=256): bool {
   "Draws a filled ellipse."
   _draw_filled_fan_2d(cx, cy, rx, ry, segments, color, 0.0)
}

fn _draw_radial_polyline_fallback(
   f64: cx,
   f64: cy,
   f64: rx,
   f64: ry,
   f64: start_rad,
   f64: span_rad,
   int: steps,
   any: color,
   f64: thickness,
   bool: closed=false
): bool {
   steps = max(1, int(steps))
   def limit = closed ? steps : steps + 1
   mut points, i = [], 0
   while(i < limit){
      def angle = start_rad + (span_rad * float(i) / float(steps))
      points = points.append([cx + cos(angle) * rx, cy + sin(angle) * ry, 0.0])
      i += 1
   }
   draw_polyline(points, color, thickness, closed)
}

fn draw_ellipse_lines(f64: cx, f64: cy, f64: rx, f64: ry, any: color, f64: thickness=0.02, int: segments=72): bool {
   "Draws the outline of an ellipse."
   if(rx <= 0 || ry <= 0){ return false }
   segments = max(8, int(segments))
   if(_backend == BACKEND_VK){
      def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
      lib_vkr.draw_ellipse_lines_2d(cx, cy, rx, ry, thickness, segments, r, g, b, a)
      return true
   }
   _draw_radial_polyline_fallback(cx, cy, rx, ry, 0.0, TAU, segments, color, thickness, true)
}

fn draw_arc(f64: cx, f64: cy, f64: radius, f64: start_deg, f64: end_deg, any: color, f64: thickness=0.02, int: segments=32): bool {
   "Draws an arc segment outline."
   if(radius <= 0){ return false }
   if(end_deg < start_deg){ def t = start_deg start_deg = end_deg end_deg = t }
   def span = end_deg - start_deg
   if(span <= 0){ return false }
   mut steps = max(2, int((float(segments) * span) / 360.0))
   def start = start_deg * PI / 180.0
   def span_rad = span * PI / 180.0
   if(_backend == BACKEND_VK){
      def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
      lib_vkr.draw_arc_2d(cx, cy, radius, start, span_rad, thickness, steps, r, g, b, a)
      return true
   }
   _draw_radial_polyline_fallback(cx, cy, radius, radius, start, span_rad, steps, color, thickness, false)
}

fn draw_sector(
   f64: cx,
   f64: cy,
   f64: inner_radius,
   f64: outer_radius,
   f64: start_deg,
   f64: end_deg,
   any: color,
   int: segments=32
): bool {
   "Draws a filled sector(pie slice/ring segment)."
   if(inner_radius < 0){ inner_radius = 0 }
   if(outer_radius <= inner_radius){ return false }
   if(end_deg < start_deg){ def t = start_deg start_deg = end_deg end_deg = t }
   def span = end_deg - start_deg
   if(span <= 0){ return false }
   mut steps = max(1, int((float(segments) * span) / 360.0))
   def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   def start = start_deg * PI / 180.0
   def span_rad = span * PI / 180.0
   if(_backend == BACKEND_VK){
      lib_vkr.draw_sector_2d(cx, cy, inner_radius, outer_radius, start, span_rad, steps, r, g, b, a)
      return true
   }
   _draw_ring_sector_fallback(cx, cy, inner_radius, outer_radius, start, span_rad, steps, r, g, b, a)
}

fn draw_rounded_rectangle(f64: x, f64: y, f64: w, f64: h, f64: radius, any: color, int: segments=24): bool {
   "Draws a filled rectangle with rounded corners."
   if(w <= 0 || h <= 0){ return false }
   if(radius <= 0){ return draw_rect(x, y, w, h, color) }
   def max_r = min(w, h) / 2.0
   if(radius > max_r){ radius = max_r }
   if(_backend == BACKEND_VK){
      def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
      def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
      lib_vkr.draw_rounded_rect_2d(x, y, w, h, radius, segments, r, g, b, a)
      return true
   }
   draw_rect(x + radius, y, w - (radius * 2.0), h, color)
   draw_rect(x, y + radius, radius, h - (radius * 2.0), color)
   draw_rect(x + w - radius, y + radius, radius, h - (radius * 2.0), color)
   mut cs = max(2, int(segments / 4))
   draw_sector(x+radius, y+radius, 0.0, radius, 180.0, 270.0, color, cs)
   draw_sector(x + w - radius, y+radius, 0.0, radius, 270.0, 360.0, color, cs)
   draw_sector(x + w - radius, y + h - radius, 0.0, radius, 0.0, 90.0, color, cs)
   draw_sector(x+radius, y + h - radius, 0.0, radius, 90.0, 180.0, color, cs)
   true
}

fn draw_star(f64: cx, f64: cy, f64: inner_radius, f64: outer_radius, int: pts, any: color, f64: rotation_deg=0.0): bool {
   "Draws a filled star shape."
   pts = max(2, int(pts))
   if(inner_radius <= 0){ inner_radius = outer_radius * 0.5 }
   def total, rot = pts * 2, rotation_deg * PI / 180.0
   def r, g = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def b, a = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   if(_backend == BACKEND_VK){
      lib_vkr.draw_star_2d(cx, cy, inner_radius, outer_radius, pts, rot, r, g, b, a)
      return true
   }
   def verts = _shape_scratch_alloc(total * 84)
   if(!verts){ return false }
   mut vi, i = 0, 0
   while(i < total){
      def a0, a1 = rot + (float(i) / float(total)) * TAU, rot + (float(i + 1) / float(total)) * TAU
      mut r0, r1 = (i % 2 == 0) ? outer_radius : inner_radius, (i % 2 == 0) ? inner_radius : outer_radius
      vi = _store_fan_tri_c4(verts, vi, cx, cy,
         cx+cos(a0)*r0, cy+sin(a0)*r0,
         cx+cos(a1)*r1, cy+sin(a1)*r1,
      r, g, b, a)
      i += 1
   }
   _draw_triangles_impl(verts, total, false)
}

fn _draw_grid_static_buffer(any: cached): bool {
   if(!cached){ return false }
   lib_vkr.bind_default_texture()
   lib_vkr.set_vertex_color_mode(1)
   lib_vkr.draw_static_buffer(cached)
   lib_vkr.set_vertex_color_mode(0)
   true
}

fn _draw_grid_lines(int: slices, f64: spacing, any: minor_col, any: major_col, f64: thickness, f64: dx=0.0, f64: dz=0.0): bool {
   def extent = float(slices) * spacing
   mut i = 0 - slices
   while(i <= slices){
      def d = float(i) * spacing
      def col = (i == 0) ? major_col : minor_col
      _draw_line_impl([dx - extent, 0.0, dz + d], [dx + extent, 0.0, dz + d], col, thickness)
      _draw_line_impl([dx + d, 0.0, dz - extent], [dx + d, 0.0, dz + extent], col, thickness)
      i += 1
   }
   true
}

fn _grid_write_xz_static_lines(
   ptr: buf,
   int: idx,
   int: slices,
   f64: spacing,
   f64: extent,
   f64: dx,
   f64: dz,
   f64: thickness,
   int: minor_col,
   int: major_col
): int {
   mut i = 0 - slices
   while(i <= slices){
      def d = float(i) * spacing
      def col = (i == 0) ? major_col : minor_col
      idx = _grid_write_thick_line(buf, idx, dx - extent, 0.0, dz + d, dx + extent, 0.0, dz + d, thickness, col)
      idx = _grid_write_thick_line(buf, idx, dx + d, 0.0, dz - extent, dx + d, 0.0, dz + extent, thickness, col)
      i += 1
   }
   idx
}

fn draw_grid(
   int: slices=10,
   f64: spacing=1.0,
   any: minor_col=[0.3, 0.34, 0.4, 0.55],
   any: major_col=[0.58, 0.62, 0.7, 0.95],
   f64: thickness=0.01
): bool {
   "Draws a grid on the ground plane(XZ)."
   slices = max(1, int(slices))
   def extent = float(slices) * spacing
   if(_backend == BACKEND_VK){
      def key = _grid_cache_key(slices, spacing, thickness, minor_col, major_col)
      mut cached = _grid_cache.get(key, 0)
      if(!cached){
         def minor = _pack_color_from(minor_col, 0.3, 0.3, 0.3, 0.55)
         def major = _pack_color_from(major_col, 0.58, 0.62, 0.7, 0.95)
         def line_count = (slices * 2 + 1) * 2
         def vert_count = line_count * 6
         def buf = malloc(vert_count * VERTEX_STRIDE)
         if(!buf){ return false }
         def idx = _grid_write_xz_static_lines(buf, 0, slices, spacing, extent, 0.0, 0.0, thickness, minor, major)
         cached = lib_vkr.create_static_buffer(buf, idx)
         free(buf)
         _grid_cache[key] = cached
      }
      _draw_grid_static_buffer(cached)
      return true
   }
   _draw_grid_lines(slices, spacing, minor_col, major_col, thickness)
}

fn draw_grid_pair(
   int: fine_slices, f64: fine_spacing, any: fine_minor_col, any: fine_major_col, f64: fine_thickness,
   int: coarse_slices, f64: coarse_spacing, any: coarse_minor_col, any: coarse_major_col, f64: coarse_thickness,
   f64: coarse_dx=0.0, f64: coarse_dz=0.0
): bool {
   "Draws fine+coarse XZ grids as one cached static buffer."
   fine_slices = max(1, int(fine_slices))
   coarse_slices = max(1, int(coarse_slices))
   if(_backend == BACKEND_VK){
      def fine_minor = _pack_color_from(fine_minor_col, 0.3, 0.3, 0.3, 0.55)
      def fine_major = _pack_color_from(fine_major_col, 0.58, 0.62, 0.7, 0.95)
      def coarse_minor = _pack_color_from(coarse_minor_col, 0.3, 0.3, 0.3, 0.55)
      def coarse_major = _pack_color_from(coarse_major_col, 0.58, 0.62, 0.7, 0.95)
      def rdx = float(int(float(coarse_dx) * 1000.0)) / 1000.0
      def rdz = float(int(float(coarse_dz) * 1000.0)) / 1000.0
      def key = "pair:" + to_str(fine_slices)
      + ":" + to_str(fine_spacing)
      + ":" + to_str(fine_thickness)
      + ":" + to_str(fine_minor)
      + ":" + to_str(fine_major)
      + ":" + to_str(coarse_slices)
      + ":" + to_str(coarse_spacing)
      + ":" + to_str(coarse_thickness)
      + ":" + to_str(coarse_minor)
      + ":" + to_str(coarse_major)
      + ":" + to_str(rdx)
      + ":" + to_str(rdz)
      mut cached = _grid_cache.get(key, 0)
      if(!cached){
         def fine_extent = float(fine_slices) * float(fine_spacing)
         def coarse_extent = float(coarse_slices) * float(coarse_spacing)
         def fine_lines = (fine_slices * 2 + 1) * 2
         def coarse_lines = (coarse_slices * 2 + 1) * 2
         def vert_count = (fine_lines + coarse_lines) * 6
         def buf = malloc(vert_count * VERTEX_STRIDE)
         if(!buf){ return false }
         mut idx = _grid_write_xz_static_lines(buf, 0, fine_slices, fine_spacing, fine_extent, 0.0, 0.0, fine_thickness, fine_minor, fine_major)
         idx = _grid_write_xz_static_lines(buf, idx, coarse_slices, coarse_spacing, coarse_extent, rdx, rdz, coarse_thickness, coarse_minor, coarse_major)
         cached = lib_vkr.create_static_buffer(buf, idx)
         free(buf)
         _grid_cache[key] = cached
      }
      _draw_grid_static_buffer(cached)
      return true
   }
   _draw_grid_lines(fine_slices, fine_spacing, fine_minor_col, fine_major_col, fine_thickness)
   _draw_grid_lines(coarse_slices, coarse_spacing, coarse_minor_col, coarse_major_col, coarse_thickness, coarse_dx, coarse_dz)
   true
}

fn draw_grid_pair_axes(f64: coarse_dx, f64: coarse_dz, f64: axes_dx, f64: axes_dz): bool {
   "Draws the editor fine+coarse XZ grid plus centered RGB axes as one cached static buffer."
   if(_backend != BACKEND_VK){ return false }
   _ensure_grid_axes_cache_lists()
   def fine_slices = 72
   def fine_spacing = 1.0
   def fine_thickness = 0.008
   def coarse_slices = 24
   def coarse_spacing = 10.0
   def coarse_thickness = 0.014
   def qrdx = int(float(coarse_dx) * 1000.0)
   def qrdz = int(float(coarse_dz) * 1000.0)
   def qadx = int(float(axes_dx) * 1000.0)
   def qadz = int(float(axes_dz) * 1000.0)
   mut cached = 0
   if(qrdx == _grid_axes_last_qrdx
      && qrdz == _grid_axes_last_qrdz
      && qadx == _grid_axes_last_qadx
      && qadz == _grid_axes_last_qadz){
      cached = _grid_axes_last_cache
   }
   def rdx, rdz = float(qrdx) / 1000.0, float(qrdz) / 1000.0
   def adx, adz = float(qadx) / 1000.0, float(qadz) / 1000.0
   mut key = ""
   if(!cached){
      key = "editor_grid_axes:" + to_str(rdx) + ":" + to_str(rdz) + ":" + to_str(adx) + ":" + to_str(adz)
      cached = (key == _grid_axes_last_key) ? _grid_axes_last_cache : 0
   }
   if(!cached){
      mut ci = 0
      while(ci < _grid_axes_cache_keys.len){
         if(_grid_axes_cache_keys.get(ci, "") == key){
            cached = _grid_axes_cache_vals.get(ci, 0)
            _grid_axes_last_key = key
            _grid_axes_last_cache = cached
            _grid_axes_last_qrdx = qrdx
            _grid_axes_last_qrdz = qrdz
            _grid_axes_last_qadx = qadx
            _grid_axes_last_qadz = qadz
            break
         }
         ci += 1
      }
   }
   if(!cached){
      def fine_minor = color_pack(0.11, 0.12, 0.14, 0.28)
      def fine_major = color_pack(0.18, 0.20, 0.24, 0.58)
      def coarse_minor = color_pack(0.20, 0.24, 0.30, 0.18)
      def coarse_major = color_pack(0.36, 0.42, 0.52, 0.72)
      def red = color_pack(1.0, 0.15, 0.05, 1.0)
      def green = color_pack(0.15, 1.0, 0.05, 1.0)
      def blue = color_pack(0.05, 0.15, 1.0, 1.0)
      def fine_extent = float(fine_slices) * fine_spacing
      def coarse_extent = float(coarse_slices) * coarse_spacing
      def fine_lines = (fine_slices * 2 + 1) * 2
      def coarse_lines = (coarse_slices * 2 + 1) * 2
      def vert_count = (fine_lines + coarse_lines + 3) * 6
      def buf = malloc(vert_count * VERTEX_STRIDE)
      if(!buf){ return false }
      mut idx = _grid_write_xz_static_lines(buf, 0, fine_slices, fine_spacing, fine_extent, 0.0, 0.0, fine_thickness, fine_minor, fine_major)
      idx = _grid_write_xz_static_lines(buf, idx, coarse_slices, coarse_spacing, coarse_extent, rdx, rdz, coarse_thickness, coarse_minor, coarse_major)
      idx = _grid_write_thick_line(buf, idx, adx - 18.0, 0.0, adz, adx + 18.0, 0.0, adz, 0.08, red)
      idx = _grid_write_thick_line(buf, idx, adx, -18.0, adz, adx, 18.0, adz, 0.08, green)
      idx = _grid_write_thick_line(buf, idx, adx, 0.0, adz - 18.0, adx, 0.0, adz + 18.0, 0.08, blue)
      cached = lib_vkr.create_static_buffer(buf, idx)
      free(buf)
      if(cached){
         _grid_axes_cache_keys = _grid_axes_cache_keys.append(key)
         _grid_axes_cache_vals = _grid_axes_cache_vals.append(cached)
         if(_grid_axes_cache_vals.len > 16){
            def old = _grid_axes_cache_vals.get(0, 0)
            if(old){ lib_vkr.destroy_static_buffer(old) }
            _grid_axes_cache_keys = slice(_grid_axes_cache_keys, 1, _grid_axes_cache_keys.len, 1)
            _grid_axes_cache_vals = slice(_grid_axes_cache_vals, 1, _grid_axes_cache_vals.len, 1)
         }
         _grid_axes_last_key = key
         _grid_axes_last_cache = cached
         _grid_axes_last_qrdx = qrdx
         _grid_axes_last_qrdz = qrdz
         _grid_axes_last_qadx = qadx
         _grid_axes_last_qadz = qadz
      }
   }
   if(!cached){ return false }
   lib_vkr.bind_default_texture()
   lib_vkr.set_vertex_color_mode(1)
   lib_vkr.draw_static_buffer(cached)
   lib_vkr.set_vertex_color_mode(0)
   true
}

fn draw_axes(f64: length=1.0, f64: thickness=0.02): bool {
   "Draws RGB colored axes(XYZ)."
   if(_backend == BACKEND_VK){
      def key = f"{float(length)}:{float(thickness)}"
      mut cached = _axes_cache.get(key, 0)
      if(!cached){
         def built = mesh_build_axes(length, thickness)
         if(is_dict(built)){
            cached = mesh_create_ex(built.get("ptr", 0), built.get("cnt", 0), {"unlit": true, "vc_mode": 1}, false)
            if(cached){ _axes_cache[key] = cached }
            def bptr = built.get("ptr", 0)
            if(bptr){ free(bptr) }
         }
      }
      if(cached){ draw_mesh(cached) }
      return bool(cached)
   }
   _draw_line_impl([-length, 0.0, 0.0], [length, 0.0, 0.0], RED, thickness)
   _draw_line_impl([0.0, -length, 0.0], [0.0, length, 0.0], GREEN, thickness)
   _draw_line_impl([0.0, 0.0, -length], [0.0, 0.0, length], BLUE, thickness)
   true
}

fn draw_cube(list: pos, f64: size, any: color): bool {
   "Draws a colored 3D cube."
   def x, y, z = pos.get(0, 0.0), pos.get(1, 0.0), pos.get(2, 0.0)
   if(_backend == BACKEND_VK){
      def col = _pack_color_from(color)
      lib_vkr.draw_cube_3d(x, y, z, size, 0.0, 0.0, 0.0, col)
      return true
   }
   def s = size / 2.0
   def p000, p001, p010, p011 = [x-s, y-s, z-s], [x-s, y-s, z+s], [x-s, y+s, z-s], [x-s, y+s, z+s]
   def p100, p101, p110, p111 = [x+s, y-s, z-s], [x+s, y-s, z+s], [x+s, y+s, z-s], [x+s, y+s, z+s]
   draw_quad(p001, p101, p111, p011, color) draw_quad(p000, p010, p110, p100, color)
   draw_quad(p010, p011, p111, p110, color) draw_quad(p000, p100, p101, p001, color)
   draw_quad(p000, p001, p011, p010, color) draw_quad(p100, p110, p111, p101, color)
   true
}

fn init(): bool {
   "Initializes the graphics library with default settings."
   lib_uiw.set_blit_handler(blit_buffer)
   if(_backend_pref == BACKEND_MOCK){ init_mock_surface(800, 600) } else {
      ; Defaults to VK if not specified via environment or prefs
      init_window(800, 600, "Nytrix GFX")
   }
   true
}

fn shutdown(): bool {
   "Shuts down the graphics library and releases all resources."
   _texture_cache_writer_flush()
   close_window()
   true
}

fn camera_init(list: pos, f64: yaw, f64: pitch, f64: aspect=1.33): list {
   "Initializes a 3D camera object with position and orientation."
   ; Returns a camera object [pos, target, up, fovy, yaw, pitch, aspect]
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   def front = vec3(sin(ryaw) * cos(rpitch), sin(rpitch), -cos(ryaw) * cos(rpitch))
   def target = add(pos, front)
   mut cam = list()
   cam = cam.append(pos)
   cam = cam.append(target)
   cam = cam.append(vec3(0.0, 1.0, 0.0))
   cam = cam.append(120.0)
   cam = cam.append(yaw)
   cam = cam.append(pitch)
   cam = cam.append(aspect)
   cam
}

fn camera_update(list: camera): list {
   "Updates the camera's target position based on its orientation."
   def yaw = camera.get(4)
   def pitch = camera.get(5)
   def pos = camera.get(0)
   def ryaw = yaw * PI / 180.0
   def rpitch = pitch * PI / 180.0
   mut target = camera.get(1)
   def fx, fy = sin(ryaw) * cos(rpitch), sin(rpitch)
   def fz = -cos(ryaw) * cos(rpitch)
   if(is_list(target)){
      target[0] = pos.get(0) + fx
      target[1] = pos.get(1) + fy
      target[2] = pos.get(2) + fz
   } else {
      target = vec3(pos.get(0) + fx, pos.get(1) + fy, pos.get(2) + fz)
      camera[1] = target
   }
   camera
}

fn __cam_compute_vectors(f64: yaw, f64: pitch, any: out): any {
   def y, p = float(yaw), float(pitch)
   def cp = cos(p)
   def fx = sin(y) * cp
   def fy = sin(p)
   def fz = -cos(y) * cp
   if(is_list(out) || is_dict(out)){
      out[2] = fx
      out[3] = fy
      out[4] = fz
   }
   out
}

fn camera_set_look_at(any: camera, list: target): any {
   "Explicitly sets the camera's look-at target."
   camera[1] = target
   camera
}

fn set_model_matrix(any: mat): bool {
   "Sets the transformation matrix for subsequent 3D draw calls."
   if(!_mat4_valid(_scratch_model_set)){
      _scratch_model_set = render_matrix.mat4_identity()
   }
   def off = _mat4_data_off(mat)
   if(off >= 0){
      mut i = 0
      while(i < 16){ _scratch_model_set[2 + i] = mat.get(off + i, 0.0) i += 1 }
   } elif(!_mat4_valid(_scratch_ident)){
      render_matrix.mat4_identity_into(_scratch_model_set)
   }
   _model_matrix = (off >= 0 || !_mat4_valid(_scratch_ident)) ? _scratch_model_set : _scratch_ident
   if(_backend == BACKEND_VK){ lib_vkr.set_model_matrix(_model_matrix) }
   true
}

fn collision_rect_rect(f64: x1, f64: y1, f64: w1, f64: h1, f64: x2, f64: y2, f64: w2, f64: h2): bool {
   "Checks for intersection between two axis-aligned rectangles."
   if(x1 + w1 < x2){ return false }
   if(x1 > x2 + w2){ return false }
   if(y1 + h1 < y2){ return false }
   if(y1 > y2 + h2){ return false }
   return true
}

fn collision_rect_circle(f64: rx, f64: ry, f64: rw, f64: rh, f64: cx, f64: cy, f64: cr): bool {
   "Checks for intersection between a rectangle and a circle."
   mut tx, ty = cx, cy
   if(cx < rx){ tx = rx } elif(cx > rx + rw){ tx = rx + rw }
   if(cy < ry){ ty = ry } elif(cy > ry + rh){ ty = ry + rh }
   def dx, dy = cx - tx, cy - ty
   return(dx * dx + dy * dy) <= (cr * cr)
}

fn collision_circle_circle(f64: x1, f64: y1, f64: r1, f64: x2, f64: y2, f64: r2): bool {
   "Checks for intersection between two circles."
   def dx, dy = x1 - x2, y1 - y2
   def dsum = r1 + r2
   return(dx * dx + dy * dy) <= (dsum * dsum)
}

mut _seed = 0

fn random_seed(int: seed): bool {
   "Sets the seed for the internal pseudo-random number generator."
   _seed = int(seed)
   true
}

fn random_value(int: min, int: max): int {
   "Returns a random integer within the specified range [min, max]."
   _seed = (_seed * 1103515245 + 12345) & 0x7FFFFFFF
   min + (_seed % (max - min + 1))
}

fn open_url(str: url): bool {
   "Opens the given URL in the default system browser."
   #linux {
      std_os.shell("xdg-open '" + url + "' > /dev/null 2>&1", false, false)
   }
   #elif macos {
      std_os.shell("open '" + url + "'", false, false)
   }
   #elif windows {
      std_os.shell("start " + url, false, false)
   }
   #endif
   true
}

fn easing(f64: t, str: type): f64 {
   "Calculates an easing value for normalized time t."
   if(eq(type, "SineIn")){ return 1.0 - cos(t * PI / 2.0) }
   if(eq(type, "SineOut")){ return sin(t * PI / 2.0) }
   if(eq(type, "CubicIn")){ return t * t * t }
   if(eq(type, "CubicOut")){ def p = t - 1.0 return p * p * p + 1.0 }
   t ; Linear fallback
}

fn log(any: msg): bool {
   "Prints a formatted GFX log message."
   ui_profile.print_text("[GFX] " + str(msg))
   true
}

fn _snapshot_fill_alpha(any: data, int: w, int: h): bool {
   if(!data || w <= 0 || h <= 0){ return false }
   mut ai = 3
   def aend = w * h * 4
   while(ai < aend){
      store8(data, 255, ai)
      ai += 4
   }
   true
}

fn snapshot(str: filename): bool {
   "Saves the current framebuffer to an image file. Supports TGA, PNG, BMP formats based on extension."
   if(_backend == BACKEND_MOCK && _cpu_buf != 0 && _cpu_w > 0 && _cpu_h > 0){
      _snapshot_fill_alpha(_cpu_buf, _cpu_w, _cpu_h)
      def fb = {"width": _cpu_w, "height": _cpu_h, "data": _cpu_buf, "channels": 4}
      match lib_img.save(fb, filename){ ok(ignoredok) -> { ignoredok  return true } err(ignorederr) -> { ignorederr  return false } }
   }
   if(_backend == BACKEND_VK){
      def fb = lib_vkr.read_framebuffer()
      if(!fb || !is_dict(fb)){ return false }
      def data = fb.get("data", 0)
      def w = int(fb.get("width", 0))
      def h = int(fb.get("height", 0))
      _snapshot_fill_alpha(data, w, h)
      mut saved_ok = false
      match lib_img.save(fb, filename){ ok(ignoredok) -> { ignoredok  saved_ok = true } err(ignorederr) -> { ignorederr  saved_ok = false } }
      if(data){ free(data) }
      return saved_ok
   }
   false
}

fn request_frame_capture(): bool {
   "Requests that the next Vulkan end_frame embed a framebuffer readback for snapshot()."
   if(_backend == BACKEND_VK){ lib_vkr.request_frame_capture() return true }
   false
}

fn set_backend_type(int: type): bool {
   "Configures the preferred graphics backend(must be called before init)."
   def t = int(type)
   if(t == BACKEND_MOCK){ _backend_pref = BACKEND_MOCK } elif(t == BACKEND_VK || t == BACKEND_AUTO){ _backend_pref = BACKEND_VK } else { _backend_pref = BACKEND_VK }
   true
}

fn get_backend_type(): int {
   "Returns the requested backend type."
   _backend_pref
}

fn scissor_push(f64: x, f64: y, f64: w, f64: h): bool {
   "Enables scissor testing for the specified rectangular area."
   _ensure_scissor_stack()
   mut rx, ry = float(x), float(y)
   mut rw, rh = max(0.0, float(w)), max(0.0, float(h))
   if(rx < 0.0){
      rw += rx
      rx = 0.0
   }
   if(ry < 0.0){
      rh += ry
      ry = 0.0
   }
   if(_last_win_w > 0.0 && rx + rw > _last_win_w){ rw = max(0.0, _last_win_w - rx) }
   if(_last_win_h > 0.0 && ry + rh > _last_win_h){ rh = max(0.0, _last_win_h - ry) }
   if(_scissor_stack.len > 0){
      def prev = _scissor_stack.get(_scissor_stack.len - 1, [rx, ry, rw, rh])
      def ax0 = float(prev.get(0, 0.0))
      def ay0 = float(prev.get(1, 0.0))
      def ax1 = ax0 + float(prev.get(2, 0.0))
      def ay1 = ay0 + float(prev.get(3, 0.0))
      def bx0 = rx
      def by0 = ry
      def bx1 = rx + rw
      def by1 = ry + rh
      def cx0 = max(ax0, bx0)
      def cy0 = max(ay0, by0)
      def cx1 = min(ax1, bx1)
      def cy1 = min(ay1, by1)
      rx, ry = cx0, cy0
      rw, rh = max(0.0, cx1 - cx0), max(0.0, cy1 - cy0)
   }
   def rect = [rx, ry, rw, rh]
   _scissor_stack = _scissor_stack.append(rect)
   if(_backend == BACKEND_VK){ lib_vkr.set_scissor_rect(int(rx), int(ry), int(rw), int(rh)) }
   elif(_backend == BACKEND_GL){
      ; gl.scissor(x, _active_win_h - y - h, w, h)
   }
   true
}

fn scissor_pop(): bool {
   "Disables scissor testing."
   _ensure_scissor_stack()
   if(_scissor_stack.len > 0){ _scissor_stack = slice(_scissor_stack, 0, _scissor_stack.len - 1) }
   if(_backend == BACKEND_VK){
      if(_scissor_stack.len > 0){
         def rect = _scissor_stack.get(_scissor_stack.len - 1, [0.0, 0.0, _last_win_w, _last_win_h])
         lib_vkr.set_scissor_rect(int(rect.get(0, 0.0)), int(rect.get(1, 0.0)), int(rect.get(2, 0.0)), int(rect.get(3, 0.0)))
      } else {
         lib_vkr.reset_scissor_rect()
      }
   } elif(_backend == BACKEND_GL){
      ; gl.disable(GL_SCISSOR_TEST)
   }
   true
}

mut _texture_cache = dict(8)
mut _texture_disk_path_cache = dict(64)
mut _texture_source_sig_cache = dict(256)
def _TEXTURE_CACHE_ABI = "v10"
def _TEXTURE_CACHE_PATH_LIMIT = 2048
def _TEXTURE_SOURCE_SIG_LIMIT = 4096
mut _texture_decode_mu = 0
mut _texture_cache_writer_mu = 0
mut _texture_cache_writer_jobs = []
mut _texture_cache_writer_thread = 0
mut _texture_cache_writer_stop = false
mut _tex_cache_enabled = -1
mut _tex_disable_write_cache = -1

fn _ensure_texture_caches(): any {
   if(!is_dict(_texture_cache)){ _texture_cache = dict(8) }
   if(!is_dict(_texture_disk_path_cache)){ _texture_disk_path_cache = dict(64) }
   if(!is_dict(_texture_source_sig_cache)){ _texture_source_sig_cache = dict(256) }
   if(!is_list(_texture_cache_writer_jobs)){ _texture_cache_writer_jobs = [] }
}

fn _tex_cache_init(): bool {
   _ensure_texture_caches()
   if(_tex_cache_enabled >= 0){ return false }
   _tex_cache_enabled = ui_profile.env_toggle_cached("NY_TEX_CACHE", true) ? 1 : 0
   true
}

fn _texture_disable_disk_cache_writes(): bool {
   if(_tex_disable_write_cache != -1){ return _tex_disable_write_cache == 1 }
   if(ui_profile.env_truthy_cached("NY_TEX_DISABLE_WRITE")){
      _tex_disable_write_cache = 1
      return true
   }
   def batch_list = ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_LIST")
   _tex_disable_write_cache = batch_list.len > 0 ? 1 : 0
   _tex_disable_write_cache == 1
}

fn _texture_async_disk_cache_writes(): bool {
   ui_profile.env_truthy_cached("NY_TEX_ASYNC_WRITE") ||
   ui_profile.env_truthy_cached("NY_TEX_ASYNC_WRITES")
}

fn _texture_source_sig(any: path): str {
   _ensure_texture_caches()
   if(!is_str(path) || path.len == 0 || lib_str.startswith(path, "data:")){ return "" }
   def norm = lib_path.normalize(path)
   if(_texture_source_sig_cache.contains(norm)){ return to_str(_texture_source_sig_cache.get(norm, "")) }
   if(!lib_fs.is_file(norm)){
      _texture_source_sig_cache[norm] = ""
      return ""
   }
   match os_sys.sys_open(norm, 0, 0){
      ok(fd) -> {
         def probe_cap = 128
         def buf = malloc(probe_cap)
         if(!buf){
            _ = os_sys.sys_close(fd)
            _texture_source_sig_cache[norm] = ""
            return ""
         }
         def rr = os_sys.sys_read(fd, buf, probe_cap)
         _ = os_sys.sys_close(fd)
         if(is_err(rr)){
            free(buf)
            _texture_source_sig_cache = cache.cache_put_reset(_texture_source_sig_cache,
               norm,
               "",
               _TEXTURE_SOURCE_SIG_LIMIT,
            256)
            return ""
         }
         def rn = int(unwrap(rr))
         if(rn <= 0){
            free(buf)
            _texture_source_sig_cache = cache.cache_put_reset(_texture_source_sig_cache,
               norm,
               "",
               _TEXTURE_SOURCE_SIG_LIMIT,
            256)
            return ""
         }
         def prefix = init_str(buf, rn)
         def sig = to_hex(lib_hash.xxh32("src0|" + to_str(rn) + "|" + prefix)) + "_" + to_hex(lib_hash.xxh32("src1|" + norm + "|" + prefix))
         _texture_source_sig_cache = cache.cache_put_reset(_texture_source_sig_cache,
            norm,
            sig,
            _TEXTURE_SOURCE_SIG_LIMIT,
         256)
         return sig
      }
      err(ignorederr) -> { ignorederr
         _texture_source_sig_cache = cache.cache_put_reset(_texture_source_sig_cache,
            norm,
            "",
            _TEXTURE_SOURCE_SIG_LIMIT,
         256)
         ""
      }
   }
}

fn _texture_cache_key_for(any: path, any: format, bool: use_mipmaps=false, int: filter=-1, int: wrap_s=10497, int: wrap_t=10497): str {
   def norm = (is_str(path) && path.len > 0 && !lib_str.startswith(path, "data:")) ? lib_path.normalize(path) : to_str(path)
   mut key = norm + ":" + to_str(format) + ":" + to_str(use_mipmaps ? 1 : 0) + ":" + to_str(int(filter)) + ":" + to_str(int(wrap_s)) + ":" + to_str(int(wrap_t))
   def sig = _texture_source_sig(norm)
   if(sig.len > 0){ key = key + ":src=" + sig }
   key
}

fn _texture_disk_cache_path_for(any: path, any: format, bool: use_mipmaps=false, int: filter=-1, int: wrap_s=10497, int: wrap_t=10497): str {
   _ensure_texture_caches()
   if(!is_str(path) || path.len == 0 || lib_str.startswith(path, "data:")){ return "" }
   def key = _texture_cache_key_for(path, format, use_mipmaps, filter, wrap_s, wrap_t)
   if(_texture_disk_path_cache.contains(key)){ return to_str(_texture_disk_path_cache.get(key, "")) }
   def h0, h1 = to_hex(lib_hash.xxh32("tex|" + key)), to_hex(lib_hash.xxh32("ntex|" + key))
   def out = lib_path.join(cache_dir(), "ny_tex_" + _TEXTURE_CACHE_ABI + "_" + h0 + "_" + h1 + ".ntex")
   _texture_disk_path_cache = cache.cache_put_reset(_texture_disk_path_cache, key, out, _TEXTURE_CACHE_PATH_LIMIT, 64)
   out
}

fn _texture_cache_writer_mutex(): any {
   if(!_texture_cache_writer_mu){ _texture_cache_writer_mu = mutex_new() }
   _texture_cache_writer_mu
}

fn _texture_decode_mutex(): any {
   if(!_texture_decode_mu){ _texture_decode_mu = mutex_new() }
   _texture_decode_mu
}

fn _texture_decode_serial_enabled(): bool {
   ui_profile.env_truthy_cached("NY_IMAGE_SERIAL")
}

fn _texture_load_image_native(any: path): any {
   "Decode a texture source through the repo image loaders.
   This replaces the old implicit native hook so non-predecoded one-texture
   glTF materials do not fall through to a missing dynamic symbol."
   if(!is_str(path) || path.len == 0){ return 0 }
   if(lib_str.find(path, "data:") == 0){
      def comma = lib_str.find(path, ",")
      if(comma < 0){ return 0 }
      def payload = lib_str.str_slice(path, comma + 1, path.len)
      def bytes = (lib_str.find(path, ";base64,") >= 0) ? str_base.decode64(payload) : payload
      if(!bytes){ return 0 }
      mut ext = ""
      if(lib_str.find(path, "image/png") >= 0){ ext = ".png" }
      elif(lib_str.find(path, "image/jpeg") >= 0 || lib_str.find(path, "image/jpg") >= 0){ ext = ".jpg" }
      elif(lib_str.find(path, "image/webp") >= 0){ ext = ".webp" }
      elif(lib_str.find(path, "image/bmp") >= 0){ ext = ".bmp" }
      elif(lib_str.find(path, "image/gif") >= 0){ ext = ".gif" }
      return lib_img.decode(bytes, ext)
   }
   lib_img.load(path)
}

fn _texture_load_image_locked(any: path): any {
   if(!_texture_decode_serial_enabled()){ return _texture_load_image_native(path) }
   def mu = _texture_decode_mutex()
   if(mu){ mutex_lock(mu) }
   def img = _texture_load_image_native(path)
   if(mu){ mutex_unlock(mu) }
   img
}

fn _texture_cache_write_job(any: path, int: w, int: h, int: format, bool: use_mipmaps, any: pixels_ptr): bool {
   if(!path || path == "" || !pixels_ptr || w <= 0 || h <= 0){
      if(pixels_ptr){ free(pixels_ptr) }
      return false
   }
   if(file_exists(path)){
      free(pixels_ptr)
      return true
   }
   if(use_mipmaps && format != 9){
      mut levels = 1
      mut tw = max(1, int(w))
      mut th = max(1, int(h))
      while(tw > 1 || th > 1){
         tw, th = max(1, tw >> 1), max(1, th >> 1)
         levels += 1
      }
      def payload_bytes = img_ops.rgba_mip_total_bytes(w, h)
      def mip_pixels = img_ops.generate_rgba_mips(pixels_ptr, w, h, true)
      free(pixels_ptr)
      if(!mip_pixels){ return false }
      def ok = _texture_cache_write_ntex(path, w, h, format, levels, mip_pixels, payload_bytes)
      free(mip_pixels)
      return ok
   }
   def ok = _texture_cache_write_ntex(path, w, h, format, 1, pixels_ptr, w * h * 4)
   free(pixels_ptr)
   ok
}

fn _texture_cache_writer_loop(): int {
   _ensure_texture_caches()
   while(true){
      mut job = 0
      def mu = _texture_cache_writer_mutex()
      if(mu){ mutex_lock(mu) }
      if(is_list(_texture_cache_writer_jobs) && _texture_cache_writer_jobs.len > 0){
         job = _texture_cache_writer_jobs.get(0, 0)
         _texture_cache_writer_jobs = slice(_texture_cache_writer_jobs, 1, _texture_cache_writer_jobs.len, 1)
      }
      def should_stop = _texture_cache_writer_stop && (!is_list(_texture_cache_writer_jobs) || _texture_cache_writer_jobs.len == 0)
      if(mu){ mutex_unlock(mu) }
      if(is_dict(job)){
         _ = _texture_cache_write_job(
            to_str(job.get("path", "")),
            int(job.get("w", 0)),
            int(job.get("h", 0)),
            int(job.get("format", 37)),
            bool(job.get("use_mipmaps", false)),
         job.get("pixels", 0))
         continue
      }
      if(should_stop){ break }
      msleep(2)
   }
   0
}

fn _texture_cache_writer_ensure(): any {
   if(_texture_disable_disk_cache_writes()){ return 0 }
   if(!_texture_async_disk_cache_writes()){ return 0 }
   if(_texture_cache_writer_thread){ return _texture_cache_writer_thread }
   _texture_cache_writer_stop = false
   _texture_cache_writer_thread = thread_spawn(fn(){
         _texture_cache_writer_loop()
   })
   _texture_cache_writer_thread
}

fn _texture_cache_enqueue_write(any: path, int: w, int: h, int: format, bool: use_mipmaps, any: pixels): bool {
   _ensure_texture_caches()
   if(!path || path == "" || !pixels || w <= 0 || h <= 0){ return false }
   if(_texture_disable_disk_cache_writes()){ return false }
   def bytes = w * h * 4
   if(bytes <= 0){ return false }
   def dup = malloc(bytes)
   if(!dup){ return false }
   memcpy(dup, pixels, bytes)
   def th = _texture_cache_writer_ensure()
   if(!th){ return _texture_cache_write_job(path, w, h, format, use_mipmaps, dup) }
   def job = {"path": path, "w": int(w), "h": int(h), "format": int(format), "use_mipmaps": use_mipmaps, "pixels": dup}
   def mu = _texture_cache_writer_mutex()
   if(mu){ mutex_lock(mu) }
   _texture_cache_writer_jobs = _texture_cache_writer_jobs.append(job)
   if(mu){ mutex_unlock(mu) }
   true
}

fn _texture_cache_writer_flush(): bool {
   if(!_texture_cache_writer_thread){ return false }
   _texture_cache_writer_stop = true
   thread_join(_texture_cache_writer_thread)
   _texture_cache_writer_thread = 0
   _texture_cache_writer_stop = false
   true
}

fn _texture_cache_write_ntex(
   str: path,
   int: w,
   int: h,
   int: format,
   int: mip_levels,
   ptr: pixels,
   int: payload_bytes
) : bool {
   if(!path || path == "" || !pixels || payload_bytes <= 0){ return false }
   mut blob = bytes(32 + payload_bytes)
   if(!blob){ return false }
   store8(blob, 78, 0) ;; N
   store8(blob, 84, 1) ;; T
   store8(blob, 69, 2) ;; E
   store8(blob, 88, 3) ;; X
   store32(blob, 1, 4)
   store32(blob, int(w), 8)
   store32(blob, int(h), 12)
   store32(blob, int(format), 16)
   store32(blob, int(mip_levels), 20)
   store32(blob, int(payload_bytes), 24)
   store32(blob, 0, 28)
   memcpy(blob + 32, pixels, payload_bytes)
   match std_os.file_write(path, blob){ ok(ignoredok) -> { ignoredok  true } err(ignorederr) -> { ignorederr  false } }
}

fn _texture_cache_try_upload_ntex(
   str: cache_path,
   int: format,
   int: filter,
   int: wrap_s,
   int: wrap_t,
   str: cache_key,
   bool: use_cache,
   bool: trace_on=false,
   bool: deep_on=false,
   int: t0=0
) : int {
   if(!_texture_gpu_ready()){ return -2 }
   if(!cache_path || cache_path == "" || !file_exists(cache_path)){ return -2 }
   def rd = std_os.file_read(cache_path)
   if(!is_ok(rd)){ return -2 }
   def data = unwrap(rd)
   if(!data || data.len < 32){ return -2 }
   if(load8(data, 0) != 78 || load8(data, 1) != 84 || load8(data, 2) != 69 || load8(data, 3) != 88){ return -2 }
   if(load32(data, 4) != 1){ return -2 }
   def w, h = load32(data, 8), load32(data, 12)
   def fmt = load32(data, 16)
   def levels = load32(data, 20)
   def payload = load32(data, 24)
   if(w <= 0 || h <= 0 || payload <= 0 || fmt != format || data.len < 32 + payload){ return -2 }
   def gt0 = deep_on ? ticks() : 0
   mut tex_id = lib_vkr.create_texture_ex(w, h, data + 32, format, filter, wrap_s, wrap_t, levels > 1, payload)
   tex_id = _texture_recover_upload_id(tex_id, w, h)
   def upload_ms = deep_on ? (ui_profile.elapsed_ms(gt0)) : 0.0
   tex_id = _texture_trace_upload_result(cache_path, "ntex", trace_on, deep_on, tex_id, w, h, format, filter, 4, 0.0, 0.0, upload_ms, t0)
   if(tex_id < 0){ return -1 }
   if(tex_id >= 0){
      if(use_cache && cache_key != ""){ _texture_cache[cache_key] = tex_id }
      if(trace_on){ ui_profile.print_text("[tex] total path=" + cache_path + " cache=ntex ms=" + to_str(ui_profile.elapsed_ms(t0))) }
   }
   return tex_id
}

fn _texture_trace_enabled(): bool {
   ui_profile.env_truthy_cached("NY_TEX_TRACE")
}

fn _texture_deep_trace_enabled(): bool {
   ui_profile.debug_deep_enabled()
}

fn _texture_skip_enabled(): bool {
   ui_profile.env_truthy_cached("NY_TEX_SKIP")
}

fn _texture_stable_upload_id(any: candidate): int {
   mut tex_id = int(candidate)
   if(tex_id >= 0 && tex_id < MAX_TEXTURES){ return tex_id }
   if(tex_id < 0){ return -1 }
   def stable_tex_id = int(vk_texture.last_created_texture_id())
   def tex_count = int(vk_texture.texture_count())
   mut latest_tex_id = -1
   if(tex_count > 0){ latest_tex_id = tex_count - 1 }
   mut out = tex_id
   if(stable_tex_id >= 0){ if(stable_tex_id < MAX_TEXTURES){ out = stable_tex_id } }
   if(out < 0 || out >= MAX_TEXTURES){ if(latest_tex_id >= 0){ if(latest_tex_id < MAX_TEXTURES){ out = latest_tex_id } } }
   return out
}

fn _texture_cached_id_valid(int: tex_id): bool {
   "Reject the default 1x1 white fallback, but allow slot 0 if a backend legitimately reused it for a real upload."
   if(tex_id < 0 || tex_id >= MAX_TEXTURES){ return false }
   if(tex_id == 0){
      def sz = lib_vkr.texture_size(tex_id)
      if(is_list(sz)&& sz.len >= 2){ return int(sz.get(0, 0)) > 1 || int(sz.get(1, 0)) > 1 }
      return false
   }
   return true
}

fn _texture_uploaded_id_matches(int: tex_id, int: w, int: h): bool {
   if(tex_id < 0 || tex_id >= MAX_TEXTURES){ return false }
   if(w <= 0 || h <= 0){ return _texture_cached_id_valid(tex_id) }
   def sz = lib_vkr.texture_size(tex_id)
   if(is_list(sz) && sz.len >= 2){
      def tw, th = int(sz.get(0, 0)), int(sz.get(1, 0))
      if(tw > 0 && th > 0){ return tw == w && th == h }
   }
   if(w > 1 || h > 1){ return tex_id > 0 }
   return tex_id >= 0
}

fn _texture_recover_upload_id(any: candidate, int: w, int: h): int {
   mut tex_id = _texture_stable_upload_id(candidate)
   if(_texture_uploaded_id_matches(tex_id, w, h)){ return tex_id }
   def stable_tex_id = int(vk_texture.last_created_texture_id())
   if(_texture_uploaded_id_matches(stable_tex_id, w, h)){ return stable_tex_id }
   def tex_count = int(vk_texture.texture_count())
   if(tex_count > 0){
      def latest_tex_id = tex_count - 1
      if(_texture_uploaded_id_matches(latest_tex_id, w, h)){ return latest_tex_id }
   }
   return tex_id
}

fn _texture_load_gltf_attempt(str: path, int: format, bool: use_mips, bool: allow_disk_cache, int: filter, int: wrap_s, int: wrap_t): int {
   mut tex_id = texture_load_ex(path, format, use_mips, allow_disk_cache, filter, wrap_s, wrap_t, use_mips)
   if(_texture_cached_id_valid(tex_id)){ return tex_id }
   if(!use_mips){
      tex_id = texture_load_ex(path, format, true, allow_disk_cache, filter, wrap_s, wrap_t, true)
      if(_texture_cached_id_valid(tex_id)){ return tex_id }
   } else {
      tex_id = texture_load_ex(path, format, false, false, filter, wrap_s, wrap_t, false)
      if(_texture_cached_id_valid(tex_id)){ return tex_id }
   }
   return -1
}

fn _texture_live_cache_key(
   str: path,
   int: format,
   bool: use_mipmaps,
   int: filter,
   int: wrap_s,
   int: wrap_t,
   str: cache_key=""
) : str {
   def key = to_str(cache_key)
   key == "" ? _texture_cache_key_for(path, format, use_mipmaps, filter, wrap_s, wrap_t) : key
}

fn _texture_disk_cache_path_maybe(
   str: path,
   int: format,
   bool: use_mipmaps,
   int: filter,
   int: wrap_s,
   int: wrap_t,
   bool: allow_disk_cache=true
) : str {
   if(!allow_disk_cache || !is_str(path) || path.len == 0 || lib_str.startswith(path, "data:")){ return "" }
   _texture_disk_cache_path_for(path, format, use_mipmaps, filter, wrap_s, wrap_t)
}

fn texture_load(str: path): int {
   "Loads an image file as RGBA8_UNORM. Use texture_load_ex for explicit control."
   return texture_load_ex(path, 37) ; 37 = RGBA8_UNORM
}

fn _texture_gpu_ready(): bool{ _backend == BACKEND_VK && lib_vkr._get_device() != 0 }

fn texture_try_load_cached_ex(
   str: path,
   int: format=37,
   bool: use_mipmaps=false,
   int: filter=-1,
   int: wrap_s=10497,
   int: wrap_t=10497
) : int {
   "Returns a texture id from memory/disk cache without decoding the source image; returns -2 on cache miss."
   _ensure_texture_caches()
   if(!is_str(path) || path.len == 0 || lib_str.startswith(path, "data:")){ return -2 }
   if(_texture_skip_enabled()){ return -2 }
   _tex_cache_init()
   def use_cache = _tex_cache_enabled == 1
   def cache_key = _texture_live_cache_key(path, format, use_mipmaps, filter, wrap_s, wrap_t)
   if(use_cache){
      def cached = _texture_cache.get(cache_key, -2)
      if(cached != -2){
         if(_texture_cached_id_valid(cached)){ return cached }
         _texture_cache = _texture_cache.delete(cache_key)
      }
   }
   def disk_cache_path = _texture_disk_cache_path_maybe(path, format, use_mipmaps, filter, wrap_s, wrap_t)
   if(disk_cache_path != "" && file_exists(disk_cache_path)){
      return _texture_cache_try_upload_ntex(disk_cache_path,
         format,
         filter,
         wrap_s,
         wrap_t,
         cache_key,
         use_cache,
         false,
         false,
      0)
   }
   return -2
}

fn texture_load_ex(
   str: path,
   int: format=37,
   bool: use_mipmaps=false,
   bool: allow_disk_cache=true,
   int: filter=-1,
   int: wrap_s=10497,
   int: wrap_t=10497,
   bool: use_live_cache=true
) : int {
   "Loads an image file with explicit format(e.g. 37=UNORM, 43=SRGB) and mipmap option."
   _ensure_texture_caches()
   if(!_texture_gpu_ready()){ return -1 }
   def trace_on = false
   def deep_on = _texture_deep_trace_enabled()
   def t0 = deep_on ? ticks() : 0
   if(_texture_skip_enabled()){ return 0 }
   _tex_cache_init()
   def use_cache = _tex_cache_enabled == 1 && use_live_cache
   def cache_key = _texture_live_cache_key(path, format, use_mipmaps, filter, wrap_s, wrap_t)
   def disk_cache_path = _texture_disk_cache_path_maybe(path,
      format,
      use_mipmaps,
      filter,
      wrap_s,
      wrap_t,
   allow_disk_cache)
   if(use_cache){
      def cached = _texture_cache.get(cache_key, -2)
      if(cached != -2){
         if(_texture_cached_id_valid(cached)){
            if(deep_on){ ui_profile.print_text("[tex:deep] cache_hit path=" + path + " tex=" + to_str(cached) + " fmt=" + to_str(format) + " filter=" + to_str(int(filter))) }
            return cached
         }
         _texture_cache = _texture_cache.delete(cache_key)
         if(deep_on){ ui_profile.print_text("[tex:deep] cache_drop_invalid path=" + path + " tex=" + to_str(cached)) }
      }
   }
   if(disk_cache_path != "" && file_exists(disk_cache_path)){
      def cached_tex = _texture_cache_try_upload_ntex(disk_cache_path,
         format,
         filter,
         wrap_s,
         wrap_t,
         cache_key,
         use_cache,
         trace_on,
         deep_on,
      t0)
      if(cached_tex >= 0){ return cached_tex }
   }
   mut img = 0
   mut loaded_from_disk_cache = false
   def dt0 = deep_on ? ticks() : 0
   if(!img || !is_dict(img)){ img = _texture_load_image_locked(path) }
   def decode_ms = deep_on ? (ui_profile.elapsed_ms(dt0)) : 0.0
   if(!img || !is_dict(img)){ ui_profile.print_text("[tex] FAIL decode src path=" + path) }
   if(img && is_dict(img) && loaded_from_disk_cache && format != 9){
      def cached_channels = int(img.get("channels", img.get("bpp", 4)))
      if(cached_channels == 1){
         if(_texture_trace_enabled()){ ui_profile.print_text("[tex] cache reload src path=" + path + " reason=unexpected_grayscale") }
         img = _texture_load_image_locked(path)
         loaded_from_disk_cache = false
      }
   }
   if(!img || !is_dict(img)){ return -1 }
   def tex_id = _texture_upload_image_ex(img,
      path,
      format,
      use_mipmaps,
      allow_disk_cache,
      filter,
      wrap_s,
      wrap_t,
      cache_key,
      disk_cache_path,
      use_cache,
      loaded_from_disk_cache,
      decode_ms,
      trace_on,
      deep_on,
   t0)
   return tex_id
}

fn texture_upload_image_ex(
   dict: img,
   str: path,
   int: format=37,
   bool: use_mipmaps=false,
   bool: allow_disk_cache=true,
   int: filter=-1,
   int: wrap_s=10497,
   int: wrap_t=10497,
   str: cache_key="",
   bool: take_ownership=true
) : int {
   "Uploads a decoded image dict directly to a GPU texture, reusing the normal cache/upload path."
   _ensure_texture_caches()
   if(!img || !is_dict(img)){ return -1 }
   if(!_texture_gpu_ready()){
      if(take_ownership){ lib_img.free(img) }
      return -1
   }
   def trace_on = false
   def deep_on = _texture_deep_trace_enabled()
   def t0 = deep_on ? ticks() : 0
   if(_texture_skip_enabled()){
      if(take_ownership){ lib_img.free(img) }
      return 0
   }
   _tex_cache_init()
   def use_cache = _tex_cache_enabled == 1
   def cache_key_live = _texture_live_cache_key(path, format, use_mipmaps, filter, wrap_s, wrap_t, cache_key)
   def disk_cache_path = _texture_disk_cache_path_maybe(path,
      format,
      use_mipmaps,
      filter,
      wrap_s,
      wrap_t,
   allow_disk_cache)
   if(use_cache && cache_key_live != ""){
      def cached = _texture_cache.get(cache_key_live, -2)
      if(cached != -2){
         def img_w, img_h = _dict_int(img, "width", 0), _dict_int(img, "height", 0)
         if(_texture_uploaded_id_matches(cached, img_w, img_h)){
            if(take_ownership){ lib_img.free(img) }
            if(deep_on){ ui_profile.print_text("[tex:deep] cache_hit_upload path=" + path + " tex=" + to_str(cached) + " fmt=" + to_str(format) + " filter=" + to_str(int(filter))) }
            return cached
         }
         _texture_cache = _texture_cache.delete(cache_key_live)
         if(deep_on){ ui_profile.print_text("[tex:deep] cache_drop_invalid_upload path=" + path + " tex=" + to_str(cached)) }
      }
   }
   return _texture_upload_image_ex(img,
      path,
      format,
      use_mipmaps,
      allow_disk_cache,
      filter,
      wrap_s,
      wrap_t,
      cache_key_live,
      disk_cache_path,
      use_cache,
      false,
      0.0,
      trace_on,
      deep_on,
      t0,
   take_ownership)
}

fn _texture_temp_alloc(any: img, bool: free_img, int: bytes): ptr {
   def ptr = malloc(bytes)
   if(!ptr && free_img){ lib_img.free(img) }
   ptr
}

fn _texture_prepare_upload_pixels(
   dict: img,
   ?ptr: pixels,
   int: w,
   int: h,
   int: channels,
   int: format,
   bool: free_img
): list {
   "Returns [upload_pixels, temp_pixels, channels] with on-demand format conversion."
   mut upload_pixels = pixels
   mut temp_pixels = 0
   if(format == 9){
      if(channels != 1){
         temp_pixels = _texture_temp_alloc(img, free_img, w * h)
         if(!temp_pixels){ return [0, 0, -1] }
         mut pi = 0
         while(pi < w * h){
            def src = pixels + pi * channels
            def r = load8(src, 0)
            def g = channels > 1 ? load8(src, 1) : r
            def b = channels > 2 ? load8(src, 2) : r
            store8(temp_pixels, int((int(r) + int(g) + int(b)) / 3) & 255, pi)
            pi += 1
         }
         upload_pixels = temp_pixels
         channels = 1
      }
   } elif(channels != 4){
      temp_pixels = _texture_temp_alloc(img, free_img, w * h * 4)
      if(!temp_pixels){ return [0, 0, -1] }
      mut pi = 0
      while(pi < w * h){
         def src = pixels + pi * channels
         def dst = temp_pixels + pi * 4
         if(channels == 3){
            store8(dst, load8(src, 0), 0)
            store8(dst, load8(src, 1), 1)
            store8(dst, load8(src, 2), 2)
            store8(dst, 255, 3)
         } elif(channels == 2){
            def g = load8(src, 0)
            store8(dst, g, 0)
            store8(dst, g, 1)
            store8(dst, g, 2)
            store8(dst, load8(src, 1), 3)
         } elif(channels == 1){
            def g = load8(src, 0)
            store8(dst, g, 0)
            store8(dst, g, 1)
            store8(dst, g, 2)
            store8(dst, 255, 3)
         } else {
            store8(dst, 255, 0)
            store8(dst, 255, 1)
            store8(dst, 255, 2)
            store8(dst, 255, 3)
         }
         pi += 1
      }
      upload_pixels = temp_pixels
      channels = 4
   }
   return [upload_pixels, temp_pixels, channels]
}

fn _texture_cache_src(bool: loaded_from_disk_cache, str: cache_label=""): str {
   cache_label.len > 0 ? cache_label : (loaded_from_disk_cache ? "disk" : "src")
}

fn _texture_trace_upload_result(str: path, str: cache_src, bool: trace_on, bool: deep_on, int: tex_id, int: w, int: h, int: format, int: filter, int: channels, f64: decode_ms, f64: convert_ms, f64: upload_ms, int: t0): int {
   if(!_texture_uploaded_id_matches(tex_id, w, h)){
      if(trace_on || deep_on){
         ui_profile.print_text(
            "[tex] gpu upload invalid path=" + path
            + " cache=" + cache_src
            + " tex=" + to_str(tex_id)
            + " expected=" + to_str(w) + "x" + to_str(h)
            + " fmt=" + to_str(format)
         )
      }
      return -1
   }
   if(trace_on){
      ui_profile.print_text(
         "[tex] gpu upload path=" + path
         + " cache=" + cache_src
         + " " + to_str(w) + "x" + to_str(h)
         + " fmt=" + to_str(format)
         + " filter=" + to_str(int(filter))
         + " ms=" + to_str(upload_ms)
      )
   }
   if(deep_on){
      ui_profile.print_text(
         "[tex:deep] path=" + path +
         " w=" + to_str(w) +
         " h=" + to_str(h) +
         " channels=" + to_str(channels) +
         " decode_ms=" + to_str(decode_ms) +
         " convert_ms=" + to_str(convert_ms) +
         " upload_ms=" + to_str(upload_ms) +
         " total_ms=" + to_str(ui_profile.elapsed_ms(t0)) +
         " cache=" + cache_src
      )
   }
   tex_id
}

fn _texture_upload_image_ex(
   dict: img, str: path, int: format=37,
   bool: use_mipmaps=false, bool: allow_disk_cache=true, int: filter=-1,
   int: wrap_s=10497, int: wrap_t=10497,
   str: cache_key="", str: disk_cache_path="",
   bool: use_cache=true, bool: loaded_from_disk_cache=false,
   f64: decode_ms=0.0, bool: trace_on=false, bool: deep_on=false,
   int: t0=0, bool: free_img=true
): int {
   "Uploads a decoded image dict to a GPU texture, handling format conversion and cache bookkeeping."
   _ensure_texture_caches()
   if(!img || !is_dict(img)){ return -1 }
   if(!_texture_gpu_ready()){
      if(free_img){ lib_img.free(img) }
      return -1
   }
   def w, h = _dict_int(img, "width", 0), _dict_int(img, "height", 0)
   def pixels = img.get("data", 0)
   if(w <= 0 || h <= 0){
      if(trace_on || deep_on){ ui_profile.print_text("[tex] skip upload invalid size path=" + path + " w=" + to_str(w) + " h=" + to_str(h)) }
      if(free_img){ lib_img.free(img) }
      return -1
   }
   mut channels = _dict_int(img, "channels", 0)
   if(channels <= 0 || channels > 4){
      def raw_bpp = _dict_int(img, "bpp", 0)
      if(raw_bpp > 4 && raw_bpp % 8 == 0){ channels = raw_bpp / 8 } elif(raw_bpp > 0 && raw_bpp <= 4){ channels = raw_bpp }
   }
   if(channels <= 0 || channels > 4){ channels = 4 }
   def ct0 = deep_on ? ticks() : 0
   def prep = _texture_prepare_upload_pixels(img, pixels, w, h, channels, format, free_img)
   def upload_pixels = prep.get(0, 0)
   def temp_pixels = prep.get(1, 0)
   channels = int(prep.get(2, channels))
   if(channels < 0){ return -1 }
   def upload_src = upload_pixels
   def upload_prebaked_bytes = 0
   def convert_ms = deep_on ? (ui_profile.elapsed_ms(ct0)) : 0.0
   def gt0 = deep_on ? ticks() : 0
   mut tex_id = lib_vkr.create_texture_ex(
      w,
      h,
      upload_src,
      format,
      filter,
      wrap_s,
      wrap_t,
      use_mipmaps,
      upload_prebaked_bytes
   )
   tex_id = _texture_recover_upload_id(tex_id, w, h)
   def upload_ms = deep_on ? (ui_profile.elapsed_ms(gt0)) : 0.0
   def cache_src = _texture_cache_src(loaded_from_disk_cache)
   tex_id = _texture_trace_upload_result(path, cache_src, trace_on, deep_on, tex_id, w, h, format, filter, channels, decode_ms, convert_ms, upload_ms, t0)
   if(trace_on && tex_id < 0){
      ui_profile.print_text(
         "[tex] gpu upload failed path=" + path
         + " " + to_str(w) + "x" + to_str(h)
         + " fmt=" + to_str(format)
         + " filter=" + to_str(int(filter))
         + " channels=" + to_str(channels)
         + " cache=" + cache_src
      )
   }
   if(allow_disk_cache
      && !loaded_from_disk_cache
      && disk_cache_path != ""
      && !file_exists(disk_cache_path)
      && upload_pixels
      && channels == 4
      && format != 9
      && !_texture_disable_disk_cache_writes()){
      _ = _texture_cache_enqueue_write(disk_cache_path, w, h, format, use_mipmaps, upload_pixels)
   }
   if(free_img){ lib_img.free(img) }
   if(temp_pixels){ free(temp_pixels) }
   if(tex_id < 0){ return -1 }
   lib_vkr.set_texture_debug_meta(tex_id, path, cache_key)
   if(use_cache && cache_key != ""){ _texture_cache[cache_key] = tex_id }
   if(trace_on){ ui_profile.print_text("[tex] total path=" + path + " cache=" + cache_src + " ms=" + to_str(ui_profile.elapsed_ms(t0))) }
   return tex_id
}

fn texture_load_gltf(str: uri, str: base_path="", str: usage="color", int: filter=-1, any: sampler_info=0): int {
   "Loads a glTF texture URI, choosing sRGB(43) for 'color'/'emissive' and UNORM(37) for others."
   _ensure_texture_caches()
   if(ui_profile.env_present_cached("NY_RENDER_DEBUG")){ ui_profile.print_text("[render] loading texture: " + to_str(uri)) }
   if(!is_str(uri) || uri.len == 0){ return -1 }
   def is_color = (usage == "color" || usage == "emissive")
   def allow_disk_cache = usage != "emissive"
   def format = is_color ? 43 : 37 ; 43 = SRGB, 37 = UNORM
   def batch_dumping = ui_profile.env_present_cached("NY_UI_BATCH_DUMP_LIST") || ui_profile.env_present_cached("NY_UI_BATCH_DUMP_FILE") || ui_profile.env_truthy_cached("NY_UI_BATCH_DUMP_ALL")
   def sampler_uses_mips = is_dict(sampler_info) && bool(sampler_info.get("min_uses_mips", false))
   if(batch_dumping && !ui_profile.env_present_cached("NY_TEX_MIPS")&& !ui_profile.env_truthy_cached("NY_UI_BATCH_KEEP_MIPS") && !sampler_uses_mips){ _tex_mips_cache = 0 }
   if(_tex_mips_cache < 0){
      if(ui_profile.env_present_cached("NY_TEX_MIPS")){
         _tex_mips_cache = ui_profile.env_toggle_cached("NY_TEX_MIPS", true) ? 1 : 0
      } elif(batch_dumping && !ui_profile.env_truthy_cached("NY_UI_BATCH_KEEP_MIPS") && !sampler_uses_mips){
         _tex_mips_cache = 0
      } else {
         _tex_mips_cache = ui_profile.env_toggle_cached("NY_TEX_MIPS", false) ? 1 : 0
      }
   }
   def use_mips = _tex_mips_cache == 1 || sampler_uses_mips
   def wrap_s = is_dict(sampler_info) ? int(sampler_info.get("wrap_s", 10497)) : 10497
   def wrap_t = is_dict(sampler_info) ? int(sampler_info.get("wrap_t", 10497)) : 10497
   mut tex_filter = filter
   if(tex_filter < 0){
      if(is_dict(sampler_info) && (sampler_info.get("mag_linear", false) || sampler_info.get("min_linear", false) || sampler_info.get("min_uses_mips", false))){ tex_filter = 1 }
      else if(is_dict(sampler_info) && (sampler_info.get("mag_nearest", false) || sampler_info.get("min_nearest", false))){ tex_filter = 0 }
      else { tex_filter = 1 }
   }
   if(lib_str.find(uri, "data:") == 0){ return _texture_load_gltf_attempt(uri, format, use_mips, false, tex_filter, wrap_s, wrap_t) }
   mut norm_uri = uri
   if(lib_str.startswith(norm_uri, "./")){ norm_uri = lib_str.str_slice(norm_uri, 2, norm_uri.len) }
   norm_uri = lib_path.normalize(norm_uri)
   mut tex_id = _texture_load_gltf_attempt(norm_uri, format, use_mips, allow_disk_cache, tex_filter, wrap_s, wrap_t)
   if(tex_id > 0){ return tex_id }
   def _uri_is_already_resolved = lib_path.is_abs(norm_uri) || lib_str.startswith(norm_uri, base_path)
   if(base_path.len > 0 && !_uri_is_already_resolved){
      def full = lib_path.normalize(lib_path.join(base_path, norm_uri))
      tex_id = _texture_load_gltf_attempt(full, format, use_mips, allow_disk_cache, tex_filter, wrap_s, wrap_t)
      if(tex_id > 0){ return tex_id }
   }
   if(lib_str.startswith(uri, "./") && base_path.len > 0 && !_uri_is_already_resolved){
      def clean = lib_str.str_slice(uri, 2, uri.len)
      def full = lib_path.normalize(lib_path.join(base_path, clean))
      tex_id = _texture_load_gltf_attempt(full, format, use_mips, allow_disk_cache, tex_filter, wrap_s, wrap_t)
      if(tex_id > 0){ return tex_id }
   }
   return -1
}

fn texture_destroy(int: tex): bool {
   "Releases a texture and its GPU memory."
   _ensure_texture_caches()
   if(!is_int(tex) || tex <= 0){ return false }
   def cache_keys = dict_keys(_texture_cache)
   mut i = 0
   def cache_keys_n = cache_keys.len
   while(i < cache_keys_n){
      def k = cache_keys[i]
      if(_texture_cache.get(k, 0) == tex){ _texture_cache = _texture_cache.delete(k) }
      i += 1
   }
   lib_vkr.destroy_texture(tex)
   true
}

fn create_cubemap(int: face_size, list: face_pixels_list): any {
   "Creates a cubemap texture from 6 RGBA8 face buffers."
   lib_vkr.create_cubemap(face_size, face_pixels_list)
}

fn texture_bind(int: tex, int: slot=0): bool {
   "Binds a texture to the specified shader sampler slot."
   if(_backend != BACKEND_VK || !is_int(tex) || tex <= 0){ return false }
   lib_vkr.bind_texture(tex)
   true
}

fn draw_texture(int: tex, f64: x, f64: y, f64: scale=1.0, any: color=WHITE): bool {
   "Draws a texture at the given position with optional scale."
   if(_backend != BACKEND_VK || !is_int(tex) || tex <= 0){ return false }
   def sz = lib_vkr.texture_size(tex)
   if(!sz){ return false }
   def w, h = sz.get(0, 0), sz.get(1, 0)
   if(w <= 0 || h <= 0){ return false }
   def cr, cg = _color_at(color, 0, 1.0), _color_at(color, 1, 1.0)
   def cb, ca = _color_at(color, 2, 1.0), _color_at(color, 3, 1.0)
   lib_vkr.draw_rect_tex(x, y, int(w * scale), int(h * scale), tex, cr, cg, cb, ca)
   true
}

fn font_load(str: path, int: size, any: filter=FONT_FILTER_DEFAULT): int {
   "Loads a TrueType font at the specified pixel size and optional texture filter."
   def id = _font_load_impl(path, size, int(filter))
   if(id){ _default_font_fail_sizes = _default_font_fail_sizes.delete(max(4, size)) }
   if(!id && _is_debug()){ ui_profile.print_text("[GFX] Font load failed: " + to_str(path)) }
   id
}

fn font_load_first(list: paths, int: size, any: filter=FONT_FILTER_DEFAULT): int {
   "Loads the first available font from `paths`, resolving repo-relative paths when needed."
   if(!is_list(paths) || paths.len == 0){ return 0 }
   mut i = 0
   def paths_n = paths.len
   while(i < paths_n){
      def raw = paths[i]
      if(raw && is_str(raw) && raw.len > 0){
         mut resolved = lib_path.resolve_repo_asset(raw)
         if(!is_str(resolved)){ resolved = raw }
         if(resolved.len == 0){ resolved = raw }
         def fid = font_load(resolved, size, filter)
         if(fid){ return fid }
      }
      i += 1
   }
   0
}

fn font_allow_color_fallback(int: font, bool: enabled=true): bool {
   "Enables or disables color-emoji fallback for a loaded font."
   def font_id = _resolve_text_font(font)
   mut f = _font_get(font_id)
   if(!is_dict(f)){ return false }
   f["allow_color_fallback"] = !!enabled
   _font_set(font_id, f)
   true
}

fn font_destroy(int: font): bool {
   "Unloads a font and releases its memory and glyph caches."
   if(!is_int(font) || font <= 0){ return false }
   def font_obj = _font_get(font)
   if(font_obj){
      def glyphs = font_obj.get("glyphs", 0)
      if(glyphs && is_dict(glyphs)){
         def items = dict_items(glyphs)
         mut i = 0
         def items_n = items.len
         while(i < items_n){
            def kv = items[i]
            def glyph = kv.get(1, 0)
            if(glyph && is_dict(glyph)){
               def bm = glyph.get("bitmap", 0)
               if(bm && is_dict(bm)){
                  def bdata = bm.get("data", 0)
                  if(bdata){ free(bdata) }
               }
            }
            i += 1
         }
      }
      _font_apply_atlas_chain(font_obj, false, true)
   }
   _fonts = _fonts.delete(font)
   _font_dirty = _font_dirty.delete(font)
   _font_gpu_ready = _font_gpu_ready.delete(font)
   if(_default_font_id == font){
      _default_font_id = 0
      _default_font_fail_sizes = dict(8)
   }
   true
}

@jit
fn draw_text(int: font, any: text, f64: x, f64: y, any: color=WHITE): bool {
   "Draws text at the given screen coordinates. Automatically uses the fastest path."
   _draw_text_impl(font, text, x, y, color)
}

fn measure_text(int: font, any: text): list {
   "Returns [width, height] of the string `text` when rendered with `font`."
   if(!is_str(text)){ text = to_str(text) }
   def font_id = _resolve_text_font(font)
   _font_prime_string(font_id, text)
   def f = _font_get(font_id)
   if(!f){ return [0.0, 0.0] }
   def glyphs_ptr = f.get("fast_glyphs", 0)
   def info = f.get("info", 0)
   def size = f.get("size", 16.0)
   def line_h = f.get("line_height", 16.0)
   if(!glyphs_ptr){ return [float(text.len) * line_h * 0.5, line_h] }
   mut pen_x = 0.0
   mut max_x = 0.0
   mut lines = 1
   mut prev_gi = -1
   def n = text.len
   mut i = 0
   while(i < n){
      def nxt = _utf8_next_cp(text, i, n)
      def cp = nxt.get(0, 0)
      i = nxt.get(1, i + 1)
      if(cp == 10){
         if(pen_x > max_x){ max_x = pen_x }
         pen_x = 0.0
         lines += 1
         prev_gi = -1
         continue
      }
      if(cp == 13){ prev_gi = -1 continue }
      if(cp == 9){
         def sp_tab = lib_vkr._vkr_glyph_get_off(glyphs_ptr, 32)
         pen_x += (sp_tab && load32(sp_tab, 40) != 0) ? load32_f32(sp_tab, 0) * 4.0 : line_h * 2.0
         prev_gi = -1
         continue
      }
      mut gi = 0
      if(info){
         gi = lib_ttf.get_glyph_index(info, cp)
         if(gi == 0 && cp != 63){ gi = lib_ttf.get_glyph_index(info, 63) }
         if(prev_gi >= 0 && gi > 0){ pen_x += float(lib_ttf.get_kern(info, prev_gi, gi, int(size))) }
      }
      def g_off = lib_vkr._vkr_glyph_get_off(glyphs_ptr, cp)
      if(g_off && load32(g_off, 40) != 0){
         def adv = load32_f32(g_off, 0)
         pen_x += adv
      } else {
         def sp_off = lib_vkr._vkr_glyph_get_off(glyphs_ptr, 32)
         if(sp_off && load32(sp_off, 40) != 0){ pen_x += load32_f32(sp_off, 0) * (cp > 255 ? 2.0 : 1.0) } else { pen_x += line_h * 0.5 * (cp > 255 ? 2.0 : 1.0) }
      }
      prev_gi = gi
   }
   if(pen_x > max_x){ max_x = pen_x }
   [max_x, float(lines) * line_h]
}

mut _mt_cache = dict(256)

fn _ensure_mt_cache(): dict {
   if(!is_dict(_mt_cache)){ _mt_cache = dict(256) }
   _mt_cache
}

@jit
fn measure_text_fast(int: font, any: text): list {
   "Cached version of measure_text for repeated short strings."
   if(!is_str(text)){ return measure_text(font, text) }
   if(text.len > 256){ return measure_text(font, text) }
   def key = to_str(font) + ":" + text
   def mtc = _ensure_mt_cache()
   def res = mtc.get(key, 0)
   if(res){ return res }
   def sz = measure_text(font, text)
   _mt_cache = cache.cache_put_reset(mtc, key, sz, 4096, 256)
   sz
}

fn _font_metric_line_height(dict: f): f64 {
   def size = _dict_num(f, "size", 16.0)
   mut h = _dict_num(f, "line_height", size)
   if(h <= 0.0){ h = size }
   h
}

fn _font_metric_source(int: font): any { _font_get(font) }

fn font_line_height(int: font): f64 {
   "Returns the configured line height for a font."
   def f = _font_metric_source(font)
   if(!f){ return 0.0 }
   _font_metric_line_height(f)
}

fn font_ascent(int: font): f64 {
   "Returns the configured ascent for a font."
   def f = _font_metric_source(font)
   if(!f){ return 0.0 }
   mut a = _dict_num(f, "ascent", 0.0)
   if(a <= 0.0){ a = _font_metric_line_height(f) * 0.8 }
   a
}

@jit
fn _font_fast_text_mode(): int {
   if(_font_fast_text_enabled != -1){ return _font_fast_text_enabled }
   _font_fast_text_enabled = ui_profile.env_toggle_cached("NY_FONT_FAST_TEXT", true) ? 1 : 0
   _font_fast_text_enabled
}

fn _font_vk_legacy_fallback_mode(): bool {
   if(_font_vk_legacy_fallback_enabled != -1){ return _font_vk_legacy_fallback_enabled == 1 }
   _font_vk_legacy_fallback_enabled = ui_profile.env_toggle_cached("NY_FONT_VK_LEGACY_FALLBACK", false) ? 1 : 0
   _font_vk_legacy_fallback_enabled == 1
}

fn _font_defer_flush_mode(): bool {
   if(_font_defer_flush_enabled != -1){ return _font_defer_flush_enabled == 1 }
   _font_defer_flush_enabled = ui_profile.env_toggle_cached("NY_FONT_DEFER_FLUSH", true) ? 1 : 0
   _font_defer_flush_enabled == 1
}

fn draw_rect_fast(f64: x, f64: y, f64: w, f64: h, int: c_u32): bool {
   "Draws a rectangle using a pre-packed color."
   if(_backend == BACKEND_VK){
      lib_vkr.draw_rect_fast(x, y, w, h, c_u32)
      return true
   }
   def a, b = float((c_u32 >> 24) & 255) / 255.0, float((c_u32 >> 16) & 255) / 255.0
   def g, r = float((c_u32 >> 8) & 255) / 255.0, float(c_u32 & 255) / 255.0
   draw_rect(x, y, w, h, [r, g, b, a])
}

fn draw_rect_outline_fast(f64: x, f64: y, f64: w, f64: h, int: c_u32): bool {
   "Draws a 1px rectangle outline using a pre-packed color."
   if(_backend == BACKEND_VK){
      lib_vkr.draw_rect_outline_fast(x, y, w, h, c_u32)
      return true
   }
   draw_rect_fast(x, y, w, 1.0, c_u32)
   draw_rect_fast(x, y + h - 1.0, w, 1.0, c_u32)
   draw_rect_fast(x, y, 1.0, h, c_u32)
   draw_rect_fast(x + w - 1.0, y, 1.0, h, c_u32)
   true
}

@jit
fn draw_rects_fast_ptr(any: rects, int: count, int: stride=20): int{
   "Draws packed rect records: f32 x,y,w,h + u32 color."
   if(count <= 0 || !rects){ return 0 }
   if(_backend == BACKEND_VK){ return lib_vkr.draw_rects_fast_ptr(rects, count, stride) }
   if(stride < 20){ stride = 20 }
   mut i = 0
   while(i < count){
      def rec = rects + i * stride
      draw_rect_fast(
         load32_f32(rec, 0), load32_f32(rec, 4),
         load32_f32(rec, 8), load32_f32(rec, 12),
         load32(rec, 16)
      )
      i += 1
   }
   count
}

@jit
fn draw_lines_2d_fast_ptr(any: lines, int: count, int: stride=24): int{
   "Draws packed 2D line records: f32 x1,y1,x2,y2,thickness + u32 color."
   if(count <= 0 || !lines){ return 0 }
   if(_backend == BACKEND_VK){ return lib_vkr.draw_lines_2d_fast_ptr(lines, count, stride) }
   if(stride < 24){ stride = 24 }
   mut i = 0
   while(i < count){
      def rec = lines + i * stride
      draw_line_2d(
         load32_f32(rec, 0), load32_f32(rec, 4),
         load32_f32(rec, 8), load32_f32(rec, 12),
         load32(rec, 20), load32_f32(rec, 16)
      )
      i += 1
   }
   count
}

@jit
fn _font_can_use_fast_vk_text(): bool{ _backend == BACKEND_VK && _font_fast_text_mode() == 1 }

@jit
fn _font_sync_and_ensure_gpu(int: font_id): bool {
   if(_font_dirty.get(font_id, false)){
      _font_flush_atlases(font_id)
      _font_dirty = _font_dirty.delete(font_id)
   }
   _font_ensure_gpu_atlases(font_id)
   true
}

@jit
fn _draw_text_runs_list(int: font_id, list: runs, any: color): bool {
   mut i = 0
   def n = runs.len
   while(i < n){
      def run = runs[i]
      if(is_list(run)&& run.len >= 3){ draw_text(font_id, run.get(0, ""), float(run.get(1, 0.0)), float(run.get(2, 0.0)), color) }
      i += 1
   }
   true
}

@jit
fn _draw_text_builtin_runs_list(list: runs, any: color): bool {
   mut i = 0
   def n = runs.len
   while(i < n){
      def run = runs[i]
      if(is_list(run)&& run.len >= 3){ _draw_text_builtin(run.get(0, ""), float(run.get(1, 0.0)), float(run.get(2, 0.0)), color) }
      i += 1
   }
   true
}

@jit
fn _prime_text_runs_list(int: font_id, list: runs): bool {
   mut i = 0
   def n = runs.len
   while(i < n){
      def run = runs[i]
      if(is_list(run) && run.len >= 1){ _font_maybe_prime_string(font_id, run.get(0, "")) }
      i += 1
   }
   true
}

@jit
fn _draw_text_runs_flat_list(int: font_id, list: runs, any: color): bool {
   mut i = 0
   def n = runs.len
   while(i + 2 < n){
      draw_text(font_id, runs[i], float(runs[i + 1]), float(runs[i + 2]), color)
      i += 3
   }
   true
}

@jit
fn _draw_text_runs_flat_color_list(int: font_id, list: runs): bool {
   mut i = 0
   def n = runs.len
   while(i + 3 < n){
      draw_text(font_id, runs[i], float(runs[i + 1]), float(runs[i + 2]), int(runs[i + 3]))
      i += 4
   }
   true
}

@jit
fn _draw_text_builtin_runs_flat_list(list: runs, any: color): bool {
   mut i = 0
   def n = runs.len
   while(i + 2 < n){
      _draw_text_builtin(runs[i], float(runs[i + 1]), float(runs[i + 2]), color)
      i += 3
   }
   true
}

@jit
fn _prime_text_runs_flat_list(int: font_id, list: runs): bool {
   mut i = 0
   def n = runs.len
   while(i + 2 < n){
      _font_maybe_prime_string(font_id, runs[i])
      i += 3
   }
   true
}

@jit
fn _prime_text_runs_flat_color_list(int: font_id, list: runs): bool {
   mut i = 0
   def n = runs.len
   while(i + 3 < n){
      _font_maybe_prime_string(font_id, runs[i])
      i += 4
   }
   true
}

@jit
fn draw_text_batch(int: font, list: lines, f64: x, f64: y, f64: spacing, int: color_u32): bool {
   "Draws multiple lines of text with minimal interpreter overhead."
   def font_id = _resolve_text_font(font)
   def n_lines = is_list(lines) ? lines.len : 0
   if(_backend != BACKEND_VK){
      mut i = 0
      while(i < n_lines){
         draw_text(font_id, lines[i], x, y + float(i) * float(spacing), color_u32)
         i += 1
      }
      return true
   }
   def use_fast_text = _font_can_use_fast_vk_text()
   mut i = 0 while(i < n_lines){
      _font_maybe_prime_string(font_id, lines[i])
      i += 1
   }
   _font_sync_and_ensure_gpu(font_id)
   def col = _pack_color_from(color_u32, 1.0, 1.0, 1.0, 1.0, true)
   if(!use_fast_text){
      mut li = 0
      while(li < n_lines){
         draw_text(font_id, lines.get(li), x, y + float(li) * float(spacing), col)
         li += 1
      }
      return true
   }
   mut fi = 0
   while(fi < n_lines){
      def line = lines[fi]
      def ly = y + float(fi) * float(spacing)
      if(!_draw_text_ttf_fast(font_id, line, x, ly, col)){
         if(_font_vk_legacy_fallback_mode()){
            if(!_draw_text_ttf(font_id, line, x, ly, col)){ _draw_text_builtin(line, x, ly, col) }
         } else {
            _draw_text_builtin(line, x, ly, col)
         }
      }
      fi += 1
   }
   true
}

@jit
fn draw_text_runs(int: font, list: runs, any: color=WHITE): bool {
   "Draws arbitrary [text, x, y] runs with shared font/color using the fastest backend path."
   def font_id = _resolve_text_font(font)
   def n_runs = is_list(runs) ? runs.len : 0
   if(n_runs <= 0){ return false }
   if(!_font_can_use_fast_vk_text()){
      _draw_text_runs_list(font_id, runs, color)
      return true
   }
   _prime_text_runs_list(font_id, runs)
   _font_sync_and_ensure_gpu(font_id)
   def col = _pack_color_from(color, 1.0, 1.0, 1.0, 1.0, true)
   def f = _font_get(font_id)
   def glyphs_ptr = f ? f.get("fast_glyphs", 0) : 0
   if(glyphs_ptr){
      lib_vkr.draw_text_runs_ptr(font_id, runs, col, glyphs_ptr, f.get("ascent", 0.0), f.get("line_height", f.get("size", 16.0)))
      return true
   }
   if(_font_vk_legacy_fallback_mode()){ _draw_text_runs_list(font_id, runs, col) }
   else { _draw_text_builtin_runs_list(runs, col) }
   true
}

@jit
fn draw_text_runs_flat(int: font, list: runs, any: color=WHITE): bool {
   "Draws flat [text, x, y, ...] runs with shared font/color using the fastest backend path."
   def font_id = _resolve_text_font(font)
   def n = is_list(runs) ? runs.len : 0
   if(n < 3){ return false }
   if(!_font_can_use_fast_vk_text()){
      _draw_text_runs_flat_list(font_id, runs, color)
      return true
   }
   _prime_text_runs_flat_list(font_id, runs)
   _font_sync_and_ensure_gpu(font_id)
   def f = _font_get(font_id)
   def glyphs_ptr = f ? f.get("fast_glyphs", 0) : 0
   def col = _pack_color_from(color, 1.0, 1.0, 1.0, 1.0, true)
   if(glyphs_ptr){
      lib_vkr.draw_text_runs_flat_ptr(font_id, runs, col, glyphs_ptr, f.get("ascent", 0.0), f.get("line_height", f.get("size", 16.0)))
      return true
   }
   if(_font_vk_legacy_fallback_mode()){ _draw_text_runs_flat_list(font_id, runs, col) }
   else { _draw_text_builtin_runs_flat_list(runs, col) }
   true
}

@jit
fn draw_text_runs_flat_colors(int: font, list: runs): bool {
   "Draws flat [text, x, y, color, ...] runs with shared font and per-run packed colors."
   def font_id = _resolve_text_font(font)
   def n = is_list(runs) ? runs.len : 0
   if(n < 4){ return false }
   if(!_font_can_use_fast_vk_text()){
      _draw_text_runs_flat_color_list(font_id, runs)
      return true
   }
   _prime_text_runs_flat_color_list(font_id, runs)
   _font_sync_and_ensure_gpu(font_id)
   def f = _font_get(font_id)
   def glyphs_ptr = f ? f.get("fast_glyphs", 0) : 0
   if(glyphs_ptr){
      lib_vkr.draw_text_runs_flat_color_ptr(font_id, runs, glyphs_ptr, f.get("ascent", 0.0), f.get("line_height", f.get("size", 16.0)))
      return true
   }
   _draw_text_runs_flat_color_list(font_id, runs)
   true
}

@jit
fn _font_ascii_preprimed(str: text): bool {
   if(_font_prime_mode() == 2){ return false }
   mut i = 0
   def n = text.len
   while(i < n){
      if((load8(text, i) & 255) >= 128){ return false }
      i += 1
   }
   true
}

@jit
fn _font_maybe_prime_string(int: font_id, any: text): bool {
   if(!is_str(text)){ return false }
   if(_font_ascii_preprimed(text)){ return true }
   _font_prime_string(font_id, text)
}

@jit
fn _font_prime_string(int: font_id, any: text): bool {
   if(!is_str(text)){ return false }
   def f = _font_get(font_id)
   if(!f){ return false }
   def root_ptr = f.get("fast_glyphs", 0)
   if(!root_ptr){ return false }
   def n = text.len
   def page0 = load64(root_ptr, 0)
   mut i = 0 while(i < n){
      def b0 = load8(text, i) & 255
      mut cp = 0
      mut page_ptr = 0
      mut present = 0
      if(b0 < 128){
         cp = b0
         i += 1
         page_ptr = page0
         if(page_ptr){ present = load32(ptr_add(page_ptr, cp * 48), 40) }
      } else {
         def nxt = _utf8_next_cp(text, i, n)
         cp, i = nxt.get(0, 0), nxt.get(1, i + 1)
         if(cp > 0){
            page_ptr = load64(root_ptr, (cp >> 8) * 8)
            if(page_ptr){ present = load32(ptr_add(page_ptr, (cp & 255) * 48), 40) }
         }
      }
      if(cp <= 0){ continue }
      if(!page_ptr || present == 0){ _font_sync_fast_glyph(font_id, cp) }
   }
   true
}

@jit
fn _resolve_text_font(int: font): int {
   mut font_id = 0
   if(is_int(font) && font > 0){ font_id = font }
   if(font_id == 0){ font_id = _ensure_default_font(16) }
   font_id
}

@jit
fn _draw_text_impl(int: font, any: text, f64: x, f64: y, any: color=WHITE): bool {
   if(!is_str(text)){ text = to_str(text) }
   def font_id = _resolve_text_font(font)
   if(_backend == BACKEND_VK && font_id > 0){
      def packed = _pack_color_from(color, 1.0, 1.0, 1.0, 1.0, true)
      if(_font_can_use_fast_vk_text()){
         if(_draw_text_ttf_fast(font_id, text, x, y, packed)){ return true }
         if(!_font_vk_legacy_fallback_mode()){
            _draw_text_builtin(text, x, y, packed)
            return true
         }
      }
      if(_draw_text_ttf(font_id, text, x, y, color)){ return true }
      _draw_text_builtin(text, x, y, color)
      return true
   }
   if(font_id > 0 && _draw_text_ttf(font_id, text, x, y, color)){ return true }
   _draw_text_builtin(text, x, y, color)
   true
}

@jit
fn _draw_text_ttf_fast(int: font_id, str: text, f64: x, f64: y, int: packed_color): bool {
   def f = _font_get(font_id)
   if(!f){ return false }
   def glyphs_ptr = f.get("fast_glyphs", 0)
   if(!glyphs_ptr){ return false }
   _font_maybe_prime_string(font_id, text)
   _font_sync_and_ensure_gpu(font_id)
   def ascent = f.get("ascent", 0.0)
   def f_size = float(f.get("size", 16.0))
   def line_h = f.get("line_height", f_size)
   ; Do NOT pre-bind the atlas here — the C renderer binds per-glyph tex_id itself
   ; Defer vertex writing and offsetting to the backend to handle batch changes
   lib_vkr.__vkr_draw_text(font_id, text, x, y, packed_color, glyphs_ptr, ascent, line_h, 0)
   true
}

fn draw_text_3d(int: font, any: text, list: pos, f64: size, any: color=WHITE): bool {
   "Draws text in 3D space."
   if(!is_list(pos)){ return false }
   mut font_id = font
   if((!is_int(font_id) || font_id <= 0) && size > 0){ font_id = _ensure_default_font(int(size)) }
   draw_text(font_id, text, pos.get(0, 0.0), pos.get(1, 0.0), color)
}

fn mesh_build_grid(int: radius=200, f64: spacing=5.0, f64: thickness=0.04, any: color=[0.12, 0.15, 0.18, 1.0]): any {
   "Builds XZ grid mesh data as solid quads and returns `{ptr, cnt}`."
   def size = float(radius) * spacing
   def buf = malloc((radius * 2 + 1) * 12 * VERTEX_STRIDE)
   if(!buf){ return 0 }
   def c = color_pack(color.get(0), color.get(1), color.get(2), color.get(3))
   def thick = float(thickness) * 2.0
   mut idx = 0
   mut gi = 0 - radius
   while(gi <= radius){
      def v = float(gi) * spacing
      idx = _grid_write_thick_line(buf, idx, -size, 0.0, v, size, 0.0, v, thick, c)
      idx = _grid_write_thick_line(buf, idx, v, 0.0, -size, v, 0.0, size, thick, c)
      gi += 1
   }
   {"ptr": buf, "cnt": idx}
}

fn mesh_build_axes(f64: length=1.0, f64: thickness=0.12): any {
   "Builds centered RGB axis sticks as a static triangle mesh."
   def vert_count = 36 * 3
   def buf = malloc(vert_count * VERTEX_STRIDE)
   if(!buf){ return 0 }
   def red = color_pack(1.0, 0.15, 0.05, 1.0)
   def green = color_pack(0.15, 1.0, 0.05, 1.0)
   def blue = color_pack(0.05, 0.15, 1.0, 1.0)
   mut idx = 0
   idx = _grid_write_thick_line(buf, idx, -length, 0.0, 0.0, length, 0.0, 0.0, thickness, red)
   idx = _grid_write_thick_line(buf, idx, 0.0, -length, 0.0, 0.0, length, 0.0, thickness, green)
   idx = _grid_write_thick_line(buf, idx, 0.0, 0.0, -length, 0.0, 0.0, length, thickness, blue)
   {"ptr": buf, "cnt": idx}
}

fn mesh_create(?ptr: p, int: count, bool: is_lines=false): any {
   "Creates a static GPU mesh from raw vertex data(count vertices, packed stride)."
   mesh_create_ex(p, count, 0, is_lines)
}

fn mesh_create_static(?ptr: p, int: count, bool: is_lines=false, any: opts=0): any {
   "Creates a mesh backed by a static GPU buffer."
   mut mopts = is_dict(opts) ? opts : dict(4)
   mopts["storage"] = "static"
   mesh_create_ex(p, count, mopts, is_lines)
}

fn mesh_create_cpu(?ptr: p, int: count, bool: is_lines=false, any: opts=0): any {
   "Creates a mesh backed by host memory only."
   mut mopts = is_dict(opts) ? opts : dict(4)
   mopts["storage"] = "cpu"
   mesh_create_ex(p, count, mopts, is_lines)
}

fn _mesh_options(any: opts, bool: is_lines, bool: indexed=false): list {
   def has_opts = is_dict(opts)
   def use_points = has_opts && opts.get("is_points", false)
   if(use_points){ is_lines = false }
   elif(!is_lines && has_opts && opts.get("is_lines", false)){ is_lines = true }
   [
      use_points,
      is_lines,
      has_opts ? opts.get("storage", "static") : "static",
      has_opts && opts.get("unlit", false),
      has_opts && opts.get("no_cull", false),
      has_opts ? opts.get("vc_mode", false) : false,
      indexed && has_opts && opts.get("index_type_u32", false)
   ]
}

fn _mesh_render_flags(bool: use_points, bool: is_lines, bool: use_unlit, bool: use_nocull, bool: indexed=false): int {
   mut flags = use_points ? _MESH_GPU_POINTS : (is_lines ? _MESH_GPU_LINES : 0)
   if(use_unlit){ flags = flags | _MESH_GPU_UNLIT }
   if(use_nocull){ flags = flags | _MESH_GPU_NOCULL }
   if(indexed){ flags = flags | _MESH_GPU_INDEXED }
   flags
}

fn _mesh_base_result(int: count, int: idx_count, list: opt): dict {
   def use_points = opt.get(0, false)
   def is_lines = opt.get(1, false)
   def use_unlit = opt.get(3, false)
   def use_nocull = opt.get(4, false)
   mut res = {
      "count": count, "is_lines": is_lines, "is_points": use_points,
      "draw_count": count, "draw_lines": is_lines, "draw_points": use_points,
      "draw_unlit": use_unlit, "draw_nocull": use_nocull
   }
   if(idx_count > 0){
      res["index_count"] = idx_count
      res["draw_index_count"] = idx_count
   } else {
      res["vc_mode"] = opt.get(5, false)
   }
   if(use_unlit){ res["unlit"] = true }
   if(use_nocull){ res["no_cull"] = true }
   res
}

fn _mesh_cpu_result(dict: res, ?ptr: p, int: flags, ?ptr: idx_ptr=nil, bool: idx_u32=false): dict {
   res["ptr"] = p
   if(idx_ptr){
      res["idx_ptr"] = idx_ptr
      res["index_type_u32"] = idx_u32
   }
   res["render_flags"] = flags
   res
}

fn _mesh_static_result(
   dict: res,
   ?ptr: p,
   dict: sbuf,
   int: flags,
   int: count,
   ?ptr: idx_ptr=nil,
   int: idx_count=0
) : dict {
   def indexed = idx_ptr && idx_count > 0
   def final_idx_count = indexed ? sbuf.get("index_count", idx_count) : 0
   def idx_u32 = indexed ? sbuf.get("index_type_u32", false) : false
   def gh = sbuf.get("handle", 0)
   def go = sbuf.get("offset", 0)
   def ih = indexed ? sbuf.get("ibuf", 0) : 0
   def io = indexed ? sbuf.get("ioffset", 0) : 0
   res["ptr"] = p
   if(indexed){
      res["idx_ptr"] = idx_ptr
      res["ibuf"] = ih
      res["ibuf_offset"] = io
      res["index_type_u32"] = idx_u32
      res["index_count"] = final_idx_count
      res["draw_index_count"] = final_idx_count
   }
   res["sbuf"] = sbuf
   res["sbuf_handle"] = gh
   res["sbuf_offset"] = go
   res["render_flags"] = flags
   res["gpu_draw"] = indexed ? [gh, go, ih, io, count, final_idx_count, flags, idx_u32] : [gh, go, 0, 0, count, 0, flags]
   res["gpu_draw_slab"] = _mesh_gpu_draw_slab(gh, go, ih, io, count, final_idx_count, flags, idx_u32)
   res
}

fn mesh_create_ex(?ptr: p, int: count, any: opts=0, bool: is_lines=false): any {
   "Creates a mesh from raw vertex data. `opts.storage` may be `static` or `cpu`."
   if(!p || count <= 0){ return 0 }
   def opt = _mesh_options(opts, is_lines, false)
   def use_points = opt.get(0, false)
   is_lines = opt.get(1, false)
   def storage = opt.get(2, "static")
   def use_unlit = opt.get(3, false)
   def use_nocull = opt.get(4, false)
   mut res = _mesh_base_result(count, 0, opt)
   def flags = _mesh_render_flags(use_points, is_lines, use_unlit, use_nocull, false)
   if(storage == "cpu" || _backend == BACKEND_MOCK){ return _mesh_cpu_result(res, p, flags) }
   def sbuf = lib_vkr.create_static_buffer(p, count)
   if(!sbuf){ return 0 }
   _mesh_static_result(res, p, sbuf, flags, count)
}

fn mesh_create_indexed(?ptr: p, int: count, ?ptr: idx_ptr, int: idx_count, any: opts=0, bool: is_lines=false): any {
   "Creates an indexed mesh from raw vertex and index data."
   if(!p || count <= 0 || !idx_ptr || idx_count <= 0){ return 0 }
   def opt = _mesh_options(opts, is_lines, true)
   def use_points = opt.get(0, false)
   is_lines = opt.get(1, false)
   def storage = opt.get(2, "static")
   def use_unlit = opt.get(3, false)
   def use_nocull = opt.get(4, false)
   def use_u32 = opt.get(6, false)
   mut res = _mesh_base_result(count, idx_count, opt)
   def flags = _mesh_render_flags(use_points, is_lines, use_unlit, use_nocull, true)
   if(storage == "cpu" || _backend == BACKEND_MOCK){ return _mesh_cpu_result(res, p, flags, idx_ptr, use_u32) }
   mut buf_opts = 0
   if(use_u32){
      buf_opts = dict(4)
      buf_opts["index_type_u32"] = true
   }
   def sbuf = lib_vkr.create_static_indexed_buffer(p, count, idx_ptr, idx_count, buf_opts)
   if(!sbuf){ return 0 }
   _mesh_static_result(res, p, sbuf, flags, count, idx_ptr, idx_count)
}

fn _mesh_gpu_draw_slab(
   any: sbuf_handle,
   any: sbuf_offset,
   any: ibuf,
   any: ibuf_offset,
   int: draw_count,
   int: idx_count,
   int: flags,
   bool: idx_u32=false
) : ptr {
   "Packs immutable GPU draw state into a raw slab."
   mut slab = malloc(__layout_size("MeshGpuDrawSlab"))
   if(!slab){ return 0 }
   memset(slab, 0, __layout_size("MeshGpuDrawSlab"))
   store_layout(
      slab, "MeshGpuDrawSlab",
      sbuf_handle, sbuf_offset, ibuf, ibuf_offset,
      int(draw_count), int(idx_count), int(flags), idx_u32 ? 1 : 0
   )
   slab
}

fn mesh_set_bounds(dict: m, any: min, any: max): dict {
   "Attaches world-space AABB bounds to a mesh for culling."
   if(!is_dict(m)){ return m }
   m["min"] = min
   m["max"] = max
   m
}

fn _mesh_bounds_info(dict: m): list {
   def bmin = m.get("min", [-1.0, -1.0, -1.0])
   def bmax = m.get("max", [ 1.0,  1.0,  1.0])
   def min_x, min_y, min_z =
   _list_num_safe(bmin, 0, -1.0), _list_num_safe(bmin, 1, -1.0), _list_num_safe(bmin, 2, -1.0)
   def max_x, max_y, max_z =
   _list_num_safe(bmax, 0,  1.0), _list_num_safe(bmax, 1,  1.0), _list_num_safe(bmax, 2,  1.0)
   [min_x,
      min_y,
      min_z,
      max_x,
      max_y,
      max_z,
      max_x - min_x,
      max_y - min_y,
      max_z - min_z,
      (min_x + max_x) * 0.5,
      (min_y + max_y) * 0.5,
   (min_z + max_z) * 0.5]
}

fn _mesh_store_fit(dict: m, f64: scale, f64: cx, f64: min_y, f64: cz, f64: target_z): dict {
   m["fit_scale"] = scale
   m["fit_tx"] = 0.0 - cx * scale
   m["fit_ty"] = 0.0 - min_y * scale
   m["fit_tz"] = target_z - cz * scale
   m
}

fn mesh_fit_world(any: m, f64: target_longest=16.0, f64: z_offset=0.0): any {
   "Computes a stable world fit from cached mesh bounds and stores it on the mesh."
   if(!is_dict(m)){ return m }
   def bi = _mesh_bounds_info(m)
   def sx, sy, sz = bi.get(6, 0.0), bi.get(7, 0.0), bi.get(8, 0.0)
   def longest = max(sx, max(sy, sz))
   def scale = (longest > 0.0001) ? (target_longest / longest) : 1.0
   _mesh_store_fit(m, scale, bi.get(9, 0.0), bi.get(1, 0.0), bi.get(11, 0.0), z_offset)
}

fn mesh_fit_perspective(any: m, f64: fovy_deg, f64: aspect=16.0 / 9.0, f64: distance=25.0, f64: fill=0.9, f64: z_center=0.0): any {
   "Fits a bounded mesh into a perspective view frustum at a chosen distance."
   if(!is_dict(m)){ return m }
   def bi = _mesh_bounds_info(m)
   def sx, sy, sz = bi.get(6, 0.0), bi.get(7, 0.0), bi.get(8, 0.0)
   def longest = max(sx, max(sy, sz))
   mut safe_aspect = aspect
   mut safe_dist = distance
   mut safe_fill = fill
   if(safe_aspect < 0.1){ safe_aspect = 0.1 }
   if(safe_dist < 0.01){ safe_dist = 0.01 }
   if(safe_fill < 0.05){ safe_fill = 0.05 }
   if(safe_fill > 1.0){ safe_fill = 1.0 }
   def fovy = max(1.0, min(170.0, fovy_deg)) * PI / 180.0
   def view_h = max(0.001, 2.0 * tan(fovy * 0.5) * safe_dist * safe_fill)
   def view_w = max(0.001, view_h * safe_aspect)
   mut sx_fit, sy_fit = 1e9, 1e9
   if(sx > 0.0001){ sx_fit = view_w / sx }
   if(sy > 0.0001){ sy_fit = view_h / sy }
   mut scale = min(sx_fit, sy_fit)
   if(scale <= 0.0 || scale > 1e8){ scale = (longest > 0.0001) ? (min(view_w, view_h) / longest) : 1.0 }
   _mesh_store_fit(m, scale, bi.get(9, 0.0), bi.get(1, 0.0), bi.get(11, 0.0), z_center)
}

fn mesh_fit_camera(any: m, f64: fovy_deg, f64: aspect=16.0 / 9.0, f64: fill=0.9, f64: target_y_bias=0.35): any {
   "Fits a bounded mesh and stores a suggested camera pose that frames it."
   if(!is_dict(m)){ return m }
   mut fm = m
   def bi = _mesh_bounds_info(fm)
   def min_x, min_y, min_z = bi.get(0, -1.0), bi.get(1, -1.0), bi.get(2, -1.0)
   def max_x, max_y, max_z = bi.get(3,  1.0), bi.get(4,  1.0), bi.get(5,  1.0)
   def sx, sy, sz = bi.get(6, 2.0), bi.get(7, 2.0), bi.get(8, 2.0)
   def cx, cy_mid, cz = bi.get(9, 0.0), bi.get(10, 0.0), bi.get(11, 0.0)
   mut cy = min_y + sy * target_y_bias
   if(cy < min_y){ cy = min_y }
   if(cy > max_y){ cy = max_y }
   mut safe_aspect = aspect
   mut safe_fill = fill
   if(safe_aspect < 0.1){ safe_aspect = 0.1 }
   if(safe_fill < 0.05){ safe_fill = 0.05 }
   if(safe_fill > 0.98){ safe_fill = 0.98 }
   def fovy, fovx = max(1.0, min(170.0, fovy_deg)) * PI / 180.0, 2.0 * atan(tan(fovy * 0.5) * safe_aspect)
   def half_v, half_h = max(0.05, fovy * 0.5 * safe_fill), max(0.05, fovx * 0.5 * safe_fill)
   def dist_y = (sy > 0.0001) ? ((sy * 0.5) / tan(half_v)) : 1.0
   def dist_x = (sx > 0.0001) ? ((sx * 0.5) / tan(half_h)) : 1.0
   def dist_z = sz * 0.75 + 2.0
   def dist = max(dist_z, max(dist_x, dist_y))
   def cam_y = cy_mid + max(sy * 0.18, 2.0)
   def cam_z = max_z + dist
   def pitch = atan2(cy - cam_y, max(0.001, cam_z - cz)) * 180.0 / PI
   fm["fit_scale"] = 1.0
   fm["fit_tx"] = 0.0 - cx
   fm["fit_ty"] = 0.0 - min_y
   fm["fit_tz"] = 0.0 - cz
   fm["fit_world_min"] = [min_x, min_y, min_z]
   fm["fit_world_max"] = [max_x, max_y, max_z]
   fm["fit_cam_x"] = 0.0
   fm["fit_cam_y"] = cam_y - min_y
   fm["fit_cam_z"] = cam_z - cz
   fm["fit_cam_yaw"] = 0.0
   fm["fit_cam_pitch"] = pitch
   fm["fit_target_x"] = 0.0
   fm["fit_target_y"] = cy - min_y
   fm["fit_target_z"] = 0.0
   fm
}

fn mesh_load(str: path, any: color=WHITE): any {
   "Loads an OBJ file and returns a native vertex buffer {ptr, count}."
   def obj = lib_obj.load_obj(path)
   if(!obj){ return 0 }
   def list = lib_obj.mesh_from_obj(obj)
   def count = list.len
   if(count == 0){ return 0 }
   def buf = malloc(count * VERTEX_STRIDE)
   if(!buf){ return 0 }
   def packed = _pack_color_from(color)
   mut min_x, min_y, min_z = 1e9, 1e9, 1e9
   mut max_x, max_y, max_z = -1e9, -1e9, -1e9
   mut i = 0
   while(i < count){
      def entry = list[i]
      def p = entry.get(0)
      def uv = entry.get(1)
      def n = entry.get(2)
      def px = _list_num_safe(p, 0, 0.0)
      def py = _list_num_safe(p, 1, 0.0)
      def pz = _list_num_safe(p, 2, 0.0)
      if(px < min_x){ min_x = px } if(px > max_x){ max_x = px }
      if(py < min_y){ min_y = py } if(py > max_y){ max_y = py }
      if(pz < min_z){ min_z = pz } if(pz > max_z){ max_z = pz }
      def off = buf + i * VERTEX_STRIDE
      store32_f32(off, px, 0)
      store32_f32(off, py, 4)
      store32_f32(off, pz, 8)
      store32_f32(off, _list_num_safe(uv, 0, 0.0), 12)
      store32_f32(off, _list_num_safe(uv, 1, 0.0), 16)
      store32(off, packed, 20)
      store32_f32(off, _list_num_safe(n, 0, 0.0), 24)
      store32_f32(off, _list_num_safe(n, 1, 0.0), 28)
      store32_f32(off, _list_num_safe(n, 2, 0.0), 32)
      store32_f32(off, 0.0, 36)
      store32_f32(off, 0.0, 40)
      store32(off, 0, 44) ; tex index
      i += 1
   }
   def sbuf = lib_vkr.create_static_buffer(buf, count)
   return {"ptr": buf, "count": count, "sbuf": sbuf, "min": [min_x, min_y, min_z], "max": [max_x, max_y, max_z]}
}

fn _ptr_key(any: p): str{ __ptr_key(p) }

fn _mark_ptr_once(any: p, dict: seen): dict{
   if(p == 0){ return seen }
   def key = _ptr_key(p)
   if(key == "" || seen.get(key, 0)){ return seen }
   seen[key] = 1
   seen
}

fn _free_ptr_once(any: p, dict: seen): dict{
   if(p == 0){ return seen }
   def key = _ptr_key(p)
   if(key == "" || seen.get(key, 0)){ return seen }
   seen[key] = 1
   free(p)
   seen
}

fn _free_ptr_list_once(list: ptrs, dict: seen): dict {
   mut out = seen
   mut i = 0
   def ptrs_n = ptrs.len
   while(i < ptrs_n){
      out = _free_ptr_once(ptrs.get(i, 0), out)
      i += 1
   }
   out
}

fn mesh_destroy(any: m): bool {
   "Unloads a mesh and releases its resources."
   if(is_dict(m)){
      def sbuf = m.get("sbuf", 0)
      if(sbuf){
         lib_vkr.destroy_static_buffer(sbuf)
         m["sbuf"] = 0
         m["ibuf"] = 0
      }
      mut seen_ptrs = dict(2)
      seen_ptrs = _free_ptr_list_once([m.get("ptr", 0), m.get("idx_ptr", 0)], seen_ptrs)
      m["ptr"] = 0
      m["idx_ptr"] = 0
   }
   true
}

fn _clear_active_scene_cache(): bool {
   _active_scene_group = 0
   _active_scene_gpu_slab = 0
   _active_scene_render_parts = 0
   _active_scene_gpu_parts = 0
   _active_scene_count = 0
   _active_scene_blend_start = 0
   _active_scene_has_blend = true
   _active_scene_model_baked = false
   _active_scene_parts_baked = false
   _active_scene_light_slab = 0
   _active_scene_light_count = 0
   _active_scene_have_lights = false
   _active_scene_gpu_ready = false
   _active_scene_force_cpu_anim = false
   _active_scene_cpu_optical_start = -1
   if(_backend == BACKEND_VK){
      lib_vkr.set_scene_lights_slab(0, 0)
      _reset_material_state()
   }
   true
}

fn _mesh_gpu_handles(any: m): list {
   if(!is_dict(m)){ return [] }
   def sbuf = m.get("sbuf", 0)
   if(!is_dict(sbuf)){ return [] }
   [sbuf.get("handle", 0), sbuf.get("memory", 0), sbuf.get("ibuf", 0), sbuf.get("imemory", 0)]
}

fn _mark_mesh_gpu_handles_for_destroy(any: m, dict: freed): dict {
   mut out = freed
   def handles = _mesh_gpu_handles(m)
   mut hi = 0
   def handles_n = handles.len
   while(hi < handles_n){
      def h = handles[hi]
      if(h != 0){ out[_ptr_key(h)] = 1 }
      hi += 1
   }
   out
}

fn _mesh_clear_shared_gpu_handles(any: m, dict: freed): any {
   mut out = m
   if(!is_dict(out)){ return out }
   def handles = _mesh_gpu_handles(out)
   mut hi = 0
   def handles_n = handles.len
   while(hi < handles_n){
      def h = handles[hi]
      if(h != 0 && freed.get(_ptr_key(h), 0)){
         out["sbuf"] = 0
         out["ibuf"] = 0
         return out
      }
      hi += 1
   }
   out
}

fn _mesh_mark_or_clear_ptr(any: m, str: key, dict: freed): list {
   mut out_m = m
   mut out_freed = freed
   def p = out_m.get(key, 0)
   if(p == 0){ return [out_m, out_freed] }
   if(out_freed.get(_ptr_key(p), 0)){ out_m[key] = 0 } else { out_freed = _mark_ptr_once(p, out_freed) }
   [out_m, out_freed]
}

fn _mesh_prepare_for_destroy(any: m, dict: freed): list {
   mut out_m = _mesh_clear_shared_gpu_handles(m, freed)
   mut out_freed = _free_ptr_list_once([out_m.get("ptr", 0), out_m.get("idx_ptr", 0)], freed)
   out_freed = _mark_mesh_gpu_handles_for_destroy(out_m, out_freed)
   out_m["ptr"] = 0
   out_m["idx_ptr"] = 0
   [out_m, out_freed]
}

fn _free_part_ptr_key_once(any: part, str: key, dict: freed): dict {
   mut out_freed = freed
   if(!is_dict(part)){ return out_freed }
   def p = part.get(key, 0)
   out_freed = _free_ptr_once(p, out_freed)
   if(p != 0){ part[key] = 0 }
   out_freed
}

fn _free_part_ptrs_once(any: part, dict: freed): dict {
   mut out = freed
   out = _free_part_ptr_key_once(part, "vptr", out)
   out = _free_part_ptr_key_once(part, "iptr", out)
   out = _free_part_ptr_key_once(part, "skin_bind_vptr", out)
   out = _free_part_ptr_key_once(part, "skin_joints_ptr", out)
   out = _free_part_ptr_key_once(part, "skin_weights_ptr", out)
   out = _free_part_ptr_key_once(part, "skin_runtime_slab", out)
   out
}

fn _destroy_group_textures_enabled(any: group): bool {
   if(!is_dict(group)){ return false }
   if(ui_profile.env_truthy_cached("NY_UI_KEEP_SCENE_TEXTURES")){ return false }
   if(ui_profile.env_truthy_cached("NY_UI_DESTROY_SCENE_TEXTURES")){ return true }
   def recs = group.get("mat_records", [])
   is_list(recs) && recs.len > 0
}

fn _destroy_tex_once(any: tex, dict: seen): dict {
   mut out = seen
   if(!is_int(tex)){ return out }
   def tid = int(tex)
   if(tid <= 0 || tid >= MAX_TEXTURES){ return out }
   def key = to_str(tid)
   if(out.get(key, 0)){ return out }
   out[key] = true
   texture_destroy(tid)
   out
}

fn _destroy_packed_ext2_tex_once(any: word, dict: seen): dict {
   if(!is_int(word)){ return seen }
   def w = int(word)
   def kind = band(bshr(w, 24), 0xff)
   if(kind == 0 || band(w, 0x80000000) != 0){ return seen }
   def tid = band(w, 0xffff)
   if(tid == 0xffff){ return seen }
   _destroy_tex_once(tid, seen)
}

fn _destroy_texture_dict_keys(any: rec, list: keys_in, dict: seen): dict {
   mut out = seen
   if(!is_dict(rec)){ return out }
   mut ki = 0
   def keys_n = keys_in.len
   while(ki < keys_n){
      out = _destroy_tex_once(rec.get(keys_in[ki], -1), out)
      ki += 1
   }
   out = _destroy_packed_ext2_tex_once(rec.get("ext2_tex_word", 0x80000000), out)
   out
}

fn _destroy_group_textures_once(any: group): bool {
   if(!_destroy_group_textures_enabled(group)){ return false }
   mut seen_tex = dict(32)
   def mat_keys = ["base", "normal", "metallic_roughness", "occlusion", "emissive"]
   def part_keys = ["tex_id", "normal_tex_id", "emissive_tex_id", "occlusion", "occlusion_tex_id", "metallic_roughness"]
   def recs = group.get("mat_records", [])
   if(is_list(recs)){
      mut ri = 0
      def recs_n = recs.len
      while(ri < recs_n){
         seen_tex = _destroy_texture_dict_keys(recs[ri], mat_keys, seen_tex)
         ri += 1
      }
   }
   def parts_live = group.get("parts", [])
   if(is_list(parts_live)){
      mut pi = 0
      def parts_live_n = parts_live.len
      while(pi < parts_live_n){
         seen_tex = _destroy_texture_dict_keys(parts_live[pi], part_keys, seen_tex)
         pi += 1
      }
   }
   true
}

fn _mesh_group_clear_destroyed_state(any: group): bool {
   if(!is_dict(group)){ return false }
   group["gpu_resources"] = []
   group["parts"] = []
   group["gpu_parts"] = []
   group["gpu_parts_count"] = 0
   group["scene_lights_count"] = 0
   group["scene_lights"] = []
   group["gpu_draw_state"] = [0, 0, 0, 0, 0, 0, 0, 0, 0]
   true
}

fn mesh_group_destroy(any: group): bool {
   "Destroys every mesh in a grouped mesh scene, or a single mesh fallback."
   if(!is_dict(group)){ return false }
   if(to_int(group) == to_int(_active_scene_group)){ _clear_active_scene_cache() }
   mut freed_ptrs = dict(16)
   def gpu_parts_slab = group.get("gpu_parts_slab", 0)
   freed_ptrs = _free_ptr_once(gpu_parts_slab, freed_ptrs)
   if(gpu_parts_slab){ group["gpu_parts_slab"] = 0 }
   def scene_lights_slab = group.get("scene_lights_slab", 0)
   freed_ptrs = _free_ptr_once(scene_lights_slab, freed_ptrs)
   if(scene_lights_slab){ group["scene_lights_slab"] = 0 }
   _destroy_group_textures_once(group)
   if(group.get("mat_records", 0)){ group["mat_records"] = [] }
   def gpu_resources = group.get("gpu_resources", [])
   if(is_list(gpu_resources)){
      mut gi = 0
      def gpu_resources_n = gpu_resources.len
      while(gi < gpu_resources_n){
         mut gm = gpu_resources[gi]
         if(is_dict(gm)){
            def prepared = _mesh_prepare_for_destroy(gm, freed_ptrs)
            gm, freed_ptrs = prepared.get(0, gm), prepared.get(1, freed_ptrs)
            mesh_destroy(gm)
            gpu_resources[gi] = gm
         }
         gi += 1
      }
   }
   def parts = group.get("parts", 0)
   if(!is_list(parts)){
      mesh_destroy(group)
      _mesh_group_clear_destroyed_state(group)
      return true
   }
   def parts_n = parts.len
   mut i = 0
   while(i < parts_n){
      def part = parts[i]
      if(is_dict(part)){
         freed_ptrs = _free_part_ptrs_once(part, freed_ptrs)
         mut m = part.get("mesh", 0)
         if(is_dict(m)){
            m = _mesh_clear_shared_gpu_handles(m, freed_ptrs)
            def ptr_mark = _mesh_mark_or_clear_ptr(m, "ptr", freed_ptrs)
            m, freed_ptrs = ptr_mark.get(0, m), ptr_mark.get(1, freed_ptrs)
            def idx_mark = _mesh_mark_or_clear_ptr(m, "idx_ptr", freed_ptrs)
            m, freed_ptrs = idx_mark.get(0, m), idx_mark.get(1, freed_ptrs)
            m["ptr"] = 0
            m["idx_ptr"] = 0
            freed_ptrs = _mark_mesh_gpu_handles_for_destroy(m, freed_ptrs)
            mesh_destroy(m)
            part["mesh"] = m
         }
         parts[i] = part
      }
      i += 1
   }
   _mesh_group_clear_destroyed_state(group)
   true
}

fn mesh_retire(any: m): bool {
   "Queues a mesh for deferred destruction. Useful for streamed mesh replacement."
   if(is_dict(m)){ _retired_meshes = _retired_meshes.append(m) return true }
   false
}

fn mesh_collect_retired(int: limit=0): int {
   "Destroys queued retired meshes. `limit<=0` drains the whole queue."
   if(_retired_meshes.len <= 0){ return 0 }
   def n = (limit <= 0 || limit >= _retired_meshes.len) ? _retired_meshes.len : limit
   mut i = 0
   while(i < n){
      mesh_destroy(_retired_meshes[i])
      i += 1
   }
   if(n >= _retired_meshes.len){ _retired_meshes = [] } else { _retired_meshes = slice(_retired_meshes, n, _retired_meshes.len, 1) }
   n
}

fn _cpu_project_vertex_at(?ptr: p, int: off, any: mvp): any { _cpu_project_vertex(load32_f32(p, off + 0), load32_f32(p, off + 4), load32_f32(p, off + 8), mvp) }

fn _cpu_draw_projected_thick_line(any: p0, any: p1, f64: width, int: color): bool {
   if(!p0 || !p1){ return false }
   def x0, y0 = p0.get(0, 0.0), p0.get(1, 0.0)
   def x1, y1 = p1.get(0, 0.0), p1.get(1, 0.0)
   def dx, dy = x1 - x0, y1 - y0
   def len = sqrt(dx * dx + dy * dy)
   if(len <= 0.000001){ return false }
   def px, py = -dy / len * (width * 0.5), dx / len * (width * 0.5)
   _cpu_draw_triangle(x0 + px, y0 + py, x0 - px, y0 - py, x1 - px, y1 - py, color)
   _cpu_draw_triangle(x0 + px, y0 + py, x1 - px, y1 - py, x1 + px, y1 + py, color)
   true
}

fn _cpu_draw_mesh_mock_lines(?ptr: p, int: count, f64: width, any: mvp): bool {
   mut i = 0
   while(i + 1 < count){
      def off0, off1 = i * VERTEX_STRIDE, (i + 1) * VERTEX_STRIDE
      def p0, p1 = _cpu_project_vertex_at(p, off0, mvp), _cpu_project_vertex_at(p, off1, mvp)
      if(p0 && p1){
         def c = _cpu_color_from_vk_packed(load32(p, off0 + 20))
         _cpu_draw_projected_thick_line(p0, p1, width, c)
      }
      i += 2
   }
   true
}

fn _cpu_draw_mesh_mock_tris(?ptr: p, int: count, any: mvp): bool {
   mut i = 0
   while(i + 2 < count){
      def off0, off1 = i * VERTEX_STRIDE, (i + 1) * VERTEX_STRIDE
      def off2 = (i + 2) * VERTEX_STRIDE
      def p0 = _cpu_project_vertex_at(p, off0, mvp)
      def p1 = _cpu_project_vertex_at(p, off1, mvp)
      def p2 = _cpu_project_vertex_at(p, off2, mvp)
      if(p0 && p1 && p2){
         def c = _cpu_color_from_vk_packed(load32(p, off0 + 20))
         _cpu_draw_triangle(
            p0.get(0, 0.0), p0.get(1, 0.0),
            p1.get(0, 0.0), p1.get(1, 0.0),
            p2.get(0, 0.0), p2.get(1, 0.0),
            c
         )
      }
      i += 3
   }
   true
}

fn _draw_mesh_mock(dict: m, bool: is_lines=false, f64: width=1.0): bool {
   def ptr = m.get("ptr", 0)
   def count = m.get("count", 0)
   if(!ptr || count <= 0){ return false }
   def lines = is_lines || m.get("is_lines", false)
   _cpu_mul_active_mvp_into(_cull_mvp)
   if(lines){
      _cpu_draw_mesh_mock_lines(ptr, count, width, _cull_mvp)
      return true
   }
   _cpu_draw_mesh_mock_tris(ptr, count, _cull_mvp)
}

fn draw_mesh(dict: m, bool: is_lines=false, f64: width=1.0): bool {
   "Draws a 3D mesh using the direct GPU-buffer path."
   if(!is_dict(m)){ return false }
   if(_backend == BACKEND_MOCK){
      return _draw_mesh_mock(m, is_lines, width)
   }
   if(_backend != BACKEND_VK){ return false }
   _draw_mesh_vk_fast(m, is_lines, width)
}

fn _mesh_pipe_override(
   bool: lines,
   bool: points,
   bool: mesh_blend,
   bool: use_unlit,
   bool: use_nocull,
   bool: mesh_flip
) : int {
   if(!lines && !points){
      if(mesh_blend){
         if(use_unlit){
            if(mesh_flip){ return use_nocull ? lib_vkr._get_mesh_alpha_unlit_nocull_flip_pipeline() : lib_vkr._get_mesh_alpha_unlit_flip_pipeline() }
            return use_nocull ? lib_vkr._get_mesh_alpha_unlit_nocull_pipeline() : lib_vkr._get_mesh_alpha_unlit_pipeline()
         }
         if(mesh_flip){ return use_nocull ? lib_vkr._get_mesh_alpha_nocull_flip_pipeline() : lib_vkr._get_mesh_alpha_flip_pipeline() }
         return use_nocull ? lib_vkr._get_mesh_alpha_nocull_pipeline() : lib_vkr._get_mesh_alpha_pipeline()
      }
      if(use_unlit){
         if(mesh_flip){ return use_nocull ? lib_vkr._get_mesh_opaque_unlit_nocull_flip_pipeline() : lib_vkr._get_flip_unlit_pipeline() }
         return use_nocull ? lib_vkr._get_mesh_opaque_unlit_nocull_pipeline() : lib_vkr._get_mesh_opaque_unlit_pipeline()
      }
      if(mesh_flip){ return use_nocull ? lib_vkr._get_mesh_opaque_nocull_flip_pipeline() : lib_vkr._get_flip_pipeline() }
      return use_nocull ? lib_vkr._get_mesh_opaque_nocull_pipeline() : lib_vkr._get_mesh_opaque_pipeline()
   }
   if(use_nocull){ return use_unlit ? lib_vkr._get_unlit_nocull_pipeline() : lib_vkr._get_nocull_pipeline() }
   0
}

fn _mesh_unpack_draw_source(dict: m, any: gpu_slab): list {
   mut gpu_draw = 0
   mut _has_gpu = false
   mut gpu_draw_len = 0
   if(gpu_slab){ _has_gpu = true } else {
      gpu_draw = m.get("gpu_draw", 0)
      if(is_list(gpu_draw)){ _has_gpu = true gpu_draw_len = gpu_draw.len }
   }
   mut gpu_flags = 0
   mut idx_u32 = false
   mut sbuf_handle = 0
   mut sbuf_offset = 0
   mut ibuf = 0
   mut ibuf_offset = 0
   mut draw_count = 0
   mut idx_count = 0
   if(gpu_slab){
      sbuf_handle = load_layout(gpu_slab, "MeshGpuDrawSlab", "sbuf_handle")
      sbuf_offset = load_layout(gpu_slab, "MeshGpuDrawSlab", "sbuf_offset")
      ibuf = load_layout(gpu_slab, "MeshGpuDrawSlab", "ibuf")
      ibuf_offset = load_layout(gpu_slab, "MeshGpuDrawSlab", "ibuf_offset")
      draw_count = load_layout(gpu_slab, "MeshGpuDrawSlab", "draw_count")
      idx_count = load_layout(gpu_slab, "MeshGpuDrawSlab", "idx_count")
      gpu_flags = load_layout(gpu_slab, "MeshGpuDrawSlab", "flags")
      idx_u32 = load_layout(gpu_slab, "MeshGpuDrawSlab", "idx_u32") != 0
   } elif(_has_gpu){
      sbuf_handle, sbuf_offset = gpu_draw.get(0, 0), gpu_draw.get(1, 0)
      ibuf, ibuf_offset = gpu_draw.get(2, 0), gpu_draw.get(3, 0)
      draw_count = int(gpu_draw.get(4, 0))
      idx_count = int(gpu_draw.get(5, 0))
      gpu_flags = int(gpu_draw.get(6, 0))
      if(gpu_draw_len > 7){ idx_u32 = gpu_draw.get(7, false) }
   } else {
      def sbuf_dict = m.get("sbuf", 0)
      mut sbuf_handle_fallback = 0
      mut sbuf_offset_fallback = 0
      if(is_dict(sbuf_dict)){ sbuf_handle_fallback, sbuf_offset_fallback = sbuf_dict.get("handle", 0), sbuf_dict.get("offset", 0) }
      sbuf_handle, sbuf_offset = m.get("sbuf_handle", sbuf_handle_fallback), m.get("sbuf_offset", sbuf_offset_fallback)
      ibuf, ibuf_offset = m.get("ibuf", 0), m.get("ibuf_offset", 0)
      draw_count = int(m.get("draw_count", -1))
      if(draw_count < 0){ draw_count = int(m.get("count", 0)) }
      idx_count = int(m.get("draw_index_count", -1))
      if(idx_count < 0){ idx_count = int(m.get("index_count", 0)) }
      gpu_flags = int(m.get("render_flags", 0))
      idx_u32 = m.get("index_type_u32", false)
   }
   [sbuf_handle, sbuf_offset, ibuf, ibuf_offset, draw_count, idx_count, gpu_flags, idx_u32, _has_gpu]
}

fn _mesh_unpack_material_state(dict: m, any: material_slab): list {
   mut cpu_tex_id = -1
   mut mesh_lines = false
   mut mesh_points = false
   mut mesh_unlit = false
   mut mesh_flip = false
   mut mesh_alpha_u32 = 0
   if(material_slab){
      cpu_tex_id = _slab_i32(material_slab, 20)
      mesh_lines = load32(material_slab, 120) != 0
      mesh_points = load32(material_slab, 140) != 0
      mesh_unlit = load32(material_slab, 128) != 0
      mesh_flip = load32(material_slab, 156) != 0
      mesh_alpha_u32 = load32(material_slab, 24)
   } else {
      cpu_tex_id = int(m.get("tex_id", -1))
      if(m.contains("draw_lines")){ mesh_lines = m.get("draw_lines", false) }
      else { mesh_lines = m.get("is_lines", false) }
      if(m.contains("draw_points")){ mesh_points = m.get("draw_points", false) }
      else { mesh_points = m.get("is_points", false) }
      if(m.contains("draw_unlit")){ mesh_unlit = m.get("draw_unlit", false) }
      else { mesh_unlit = m.get("unlit", false) }
      mesh_flip = m.get("flip_winding", false)
      mesh_alpha_u32 = int(m.get("alpha_u32", 0))
   }
   [cpu_tex_id, mesh_lines, mesh_points, mesh_unlit, mesh_flip, mesh_alpha_u32]
}

fn _mesh_draw_dispatch(?ptr: p, int: count, int: tex_id, bool: lines, bool: points, f64: width, int: pipe_override=0): bool {
   if(pipe_override){ lib_vkr.bind_pipeline(pipe_override) }
   if(points){ lib_vkr.draw_points_raw(p, count, tex_id) }
   elif(lines){ lib_vkr.draw_lines_raw(p, count / 2, width) }
   else { lib_vkr.draw_vertices(p, count, tex_id) }
   if(pipe_override){ lib_vkr.bind_pipeline(0) }
   true
}

fn _mesh_draw_dispatch_rewind(?ptr: p, int: count, int: tex_id, bool: lines, bool: points, f64: width, int: pipe_override=0, bool: cpu_rewind=false): bool {
   if(cpu_rewind){
      def rew = vk_utils.gltf_rewind_triangle_vertices(p, count)
      if(rew){
         _mesh_draw_dispatch(rew, count, tex_id, lines, points, width, pipe_override)
         free(rew)
         return true
      }
   }
   _mesh_draw_dispatch(p, count, tex_id, lines, points, width, pipe_override)
}

fn _mesh_draw_primitive_markers(?ptr: p, int: count, bool: lines, bool: points, f64: width): bool {
   if(!p || count <= 0 || (!lines && !points)){ return false }
   mut mi = 0
   while(mi < 18){
      _scratch_marker_model[mi] = _list_num_safe(_model_matrix, mi, (mi == 0 || mi == 1) ? 4.0 : ((mi == 2 || mi == 7 || mi == 12 || mi == 17) ? 1.0 : 0.0))
      mi += 1
   }
   def m00, m01 = _list_num_safe(_scratch_marker_model, 2, 1.0), _list_num_safe(_scratch_marker_model, 3, 0.0)
   def m02 = _list_num_safe(_scratch_marker_model, 4, 0.0)
   def m10 = _list_num_safe(_scratch_marker_model, 6, 0.0)
   def m11 = _list_num_safe(_scratch_marker_model, 7, 1.0)
   def m12 = _list_num_safe(_scratch_marker_model, 8, 0.0)
   def m20 = _list_num_safe(_scratch_marker_model, 10, 0.0)
   def m21 = _list_num_safe(_scratch_marker_model, 11, 0.0)
   def m22 = _list_num_safe(_scratch_marker_model, 12, 1.0)
   def tx = _list_num_safe(_scratch_marker_model, 14, 0.0)
   def ty = _list_num_safe(_scratch_marker_model, 15, 0.0)
   def tz = _list_num_safe(_scratch_marker_model, 16, 0.0)
   set_model_matrix(_scratch_ident)
   def thick = max(0.045, float(width) * 0.070)
   if(points){
      def radius = thick * 2.8
      def max_points = 512
      def point_step = max(1, int(count / max_points))
      mut i = 0
      while(i < count){
         def off = i * VERTEX_STRIDE
         def x = load32_f32(p, off + _VKR_OFF_X)
         def y = load32_f32(p, off + _VKR_OFF_Y)
         def z = load32_f32(p, off + _VKR_OFF_Z)
         def x0 = x - radius
         def x1 = x + radius
         def y0 = y - radius
         def y1 = y + radius
         def z0 = z - radius
         def z1 = z + radius
         lib_vkr.draw_line_3d(
            x0 * m00 + y * m10 + z * m20 + tx, x0 * m01 + y * m11 + z * m21 + ty, x0 * m02 + y * m12 + z * m22 + tz,
            x1 * m00 + y * m10 + z * m20 + tx, x1 * m01 + y * m11 + z * m21 + ty, x1 * m02 + y * m12 + z * m22 + tz,
            thick, 1.0, 1.0, 1.0, 1.0
         )
         lib_vkr.draw_line_3d(
            x * m00 + y0 * m10 + z * m20 + tx, x * m01 + y0 * m11 + z * m21 + ty, x * m02 + y0 * m12 + z * m22 + tz,
            x * m00 + y1 * m10 + z * m20 + tx, x * m01 + y1 * m11 + z * m21 + ty, x * m02 + y1 * m12 + z * m22 + tz,
            thick, 1.0, 1.0, 1.0, 1.0
         )
         lib_vkr.draw_line_3d(
            x * m00 + y * m10 + z0 * m20 + tx, x * m01 + y * m11 + z0 * m21 + ty, x * m02 + y * m12 + z0 * m22 + tz,
            x * m00 + y * m10 + z1 * m20 + tx, x * m01 + y * m11 + z1 * m21 + ty, x * m02 + y * m12 + z1 * m22 + tz,
            thick, 1.0, 1.0, 1.0, 1.0
         )
         i += point_step
      }
      set_model_matrix(_scratch_marker_model)
      return true
   }
   def seg_count = int(count / 2)
   def max_segments = 768
   def seg_step = max(1, int(seg_count / max_segments))
   mut seg_i = 0
   while(seg_i < seg_count){
      def i, a = seg_i * 2, i * VERTEX_STRIDE
      def b = (i + 1) * VERTEX_STRIDE
      def ax = load32_f32(p, a + _VKR_OFF_X)
      def ay = load32_f32(p, a + _VKR_OFF_Y)
      def az = load32_f32(p, a + _VKR_OFF_Z)
      def bx = load32_f32(p, b + _VKR_OFF_X)
      def by = load32_f32(p, b + _VKR_OFF_Y)
      def bz = load32_f32(p, b + _VKR_OFF_Z)
      lib_vkr.draw_line_3d(
         ax * m00 + ay * m10 + az * m20 + tx,
         ax * m01 + ay * m11 + az * m21 + ty,
         ax * m02 + ay * m12 + az * m22 + tz,
         bx * m00 + by * m10 + bz * m20 + tx,
         bx * m01 + by * m11 + bz * m21 + ty,
         bx * m02 + by * m12 + bz * m22 + tz,
         thick,
         1.0,
         1.0,
         1.0,
         1.0
      )
      seg_i += seg_step
   }
   set_model_matrix(_scratch_marker_model)
   true
}

fn _draw_mesh_vk_fast(dict: m, bool: is_lines=false, f64: width=1.0, bool: restore_unlit=true): bool {
   if(!is_dict(m) || _backend != BACKEND_VK){ return false }
   def gpu_slab = m.get("gpu_draw_slab", 0)
   def source = _mesh_unpack_draw_source(m, gpu_slab)
   def sbuf_handle = _list_any_safe(source, 0, 0)
   def sbuf_offset = _list_any_safe(source, 1, 0)
   def ibuf = _list_any_safe(source, 2, 0)
   def ibuf_offset = _list_any_safe(source, 3, 0)
   def draw_count = int(_list_any_safe(source, 4, 0))
   def idx_count = int(_list_any_safe(source, 5, 0))
   def gpu_flags = int(_list_any_safe(source, 6, 0))
   def idx_u32 = !!_list_any_safe(source, 7, false)
   def _has_gpu = !!_list_any_safe(source, 8, false)
   def index_type  = idx_u32 ? 1 : 0
   def material_slab = m.get("material_slab", 0)
   def mat_state = _mesh_unpack_material_state(m, material_slab)
   def cpu_tex_id = int(_list_any_safe(mat_state, 0, -1))
   def mesh_lines = !!_list_any_safe(mat_state, 1, false)
   def mesh_points = !!_list_any_safe(mat_state, 2, false)
   def mesh_unlit = !!_list_any_safe(mat_state, 3, false)
   def mesh_flip = !!_list_any_safe(mat_state, 4, false)
   def mesh_alpha_u32 = int(_list_any_safe(mat_state, 5, 0))
   def dynamic_vertices = m.get("dynamic_vertices", false)
   def _lines_flag  = band(gpu_flags, _MESH_GPU_LINES) != 0
   def _points_flag = band(gpu_flags, _MESH_GPU_POINTS) != 0
   def _unlit_flag  = band(gpu_flags, _MESH_GPU_UNLIT) != 0
   def _nocull_flag = band(gpu_flags, _MESH_GPU_NOCULL) != 0
   mut mesh_nocull = false
   if(!_nocull_flag){
      if(m.contains("draw_nocull")){ mesh_nocull = m.get("draw_nocull", false) }
      else { mesh_nocull = m.get("no_cull", false) }
   }
   def mesh_blend = (mesh_alpha_u32 & 3) == 2
   def points     = _points_flag || mesh_points
   def lines      = !points && (is_lines || _lines_flag || mesh_lines)
   def use_unlit  = _unlit_flag  || mesh_unlit || points
   def use_nocull = (_nocull_flag || mesh_nocull) && !lines && !points
   def log_mesh = _deep_should_log_mesh()
   mut cpu_ptr = 0
   mut cpu_idx_ptr = 0
   if(dynamic_vertices || !sbuf_handle || log_mesh){
      cpu_ptr = m.get("ptr", 0)
      if(idx_count > 0){ cpu_idx_ptr = m.get("idx_ptr", 0) }
   }
   def cpu_rewind = mesh_flip && cpu_ptr && !sbuf_handle && !lines && !points
   def pipe_override = _mesh_pipe_override(lines,
      points,
      mesh_blend,
      use_unlit,
      use_nocull,
   cpu_rewind ? false : mesh_flip)
   if(ui_profile.env_truthy_cached("NY_MESH_PIPE_TRACE")){
      ui_profile.print_text("[mesh:pipe] draw=" + to_str(draw_count) +
         " idx=" + to_str(idx_count) +
         " tex=" + to_str(cpu_tex_id) +
         " lines=" + to_str(lines) +
         " points=" + to_str(points) +
         " unlit=" + to_str(use_unlit) +
         " nocull=" + to_str(use_nocull) +
         " flip=" + to_str(mesh_flip) +
         " alpha=" + to_hex(mesh_alpha_u32) +
         " pipe=" + to_str(pipe_override) +
         " gpu=" + to_str(_has_gpu) +
         " cpu=" + to_str(cpu_ptr != 0) +
      " rewind=" + to_str(cpu_rewind))
   }
   if(log_mesh){ ui_profile.print_text("[mesh_fast] draw=" + to_str(draw_count) + " idx=" + to_str(idx_count) + " lines=" + to_str(lines) + " points=" + to_str(points) + " unlit=" + to_str(use_unlit) + " nocull=" + to_str(use_nocull) + " gpu=" + to_str(_has_gpu) + " cpu=" + to_str(cpu_ptr != 0)) }
   if(use_unlit){ lib_vkr.set_unlit(true) }
   else { lib_vkr.set_unlit(false) }
   if(cpu_tex_id >= 0){ lib_vkr.bind_texture(cpu_tex_id) }
   else { lib_vkr.bind_default_texture() }
   if(material_slab){ lib_vkr.set_material_from_slab(material_slab, m.get("vc_mode", false)) } else {
      _set_material_packed_defaults(
         0x0000ff00,
         cpu_tex_id,
         int(m.get("alpha_u32", 0)),
         -1,
         0x80000000,
         m.get("vc_mode", false)
      )
   }
   if(lines && !points && !sbuf_handle && ui_profile.env_truthy_cached("NY_UI_PRIMITIVE_LINE_MARKERS")){
      def marker_ptr = m.get("ptr", 0)
      if(_mesh_draw_primitive_markers(marker_ptr, draw_count, lines, points, width)){
         if(restore_unlit && use_unlit){ lib_vkr.set_unlit(false) }
         return true
      }
   }
   if(dynamic_vertices && ibuf != 0 && idx_count > 0 && cpu_ptr){ lib_vkr.draw_vertices_indexed_raw(cpu_ptr, draw_count, ibuf, ibuf_offset, idx_count, index_type, cpu_tex_id, lines, width, pipe_override, points) } elif(sbuf_handle){
      if(ibuf != 0 && idx_count > 0){ lib_vkr.draw_static_buffer_indexed_raw(sbuf_handle, sbuf_offset, ibuf, ibuf_offset, idx_count, lines, width, pipe_override, index_type, points) } else { lib_vkr.draw_static_buffer_raw(sbuf_handle, sbuf_offset, draw_count, lines, width, pipe_override, points) }
   } elif(idx_count > 0 && cpu_ptr && cpu_idx_ptr){
      def exp = vk_utils.gltf_expand_indexed_vertices(cpu_ptr, draw_count, cpu_idx_ptr, idx_count, idx_u32)
      if(exp){
         if(_deep_should_log_mesh()){
            def ax, ay, az = load32_f32(exp, _VKR_OFF_X), load32_f32(exp, _VKR_OFF_Y), load32_f32(exp, _VKR_OFF_Z)
            def bx, by, bz =
            load32_f32(exp + VERTEX_STRIDE, _VKR_OFF_X),
            load32_f32(exp + VERTEX_STRIDE, _VKR_OFF_Y),
            load32_f32(exp + VERTEX_STRIDE, _VKR_OFF_Z)
            def cx, cy, cz =
            load32_f32(exp + VERTEX_STRIDE * 2, _VKR_OFF_X),
            load32_f32(exp + VERTEX_STRIDE * 2, _VKR_OFF_Y),
            load32_f32(exp + VERTEX_STRIDE * 2, _VKR_OFF_Z)
            ui_profile.print_text("[mesh_fast:exp0] A=(" + to_str(ax) + "," + to_str(ay) + "," + to_str(az) + ") B=(" + to_str(bx) + "," + to_str(by) + "," + to_str(bz) + ") C=(" + to_str(cx) + "," + to_str(cy) + "," + to_str(cz) + ")")
         }
         _mesh_draw_dispatch_rewind(exp, idx_count, cpu_tex_id, lines, points, width, pipe_override, cpu_rewind)
         free(exp)
      }
   } elif(cpu_ptr){
      _mesh_draw_dispatch_rewind(cpu_ptr, draw_count, cpu_tex_id, lines, points, width, pipe_override, cpu_rewind)
   } else {
      _mesh_draw_dispatch(m.get("ptr"), draw_count, -1, lines, points, width, 0)
   }
   if(restore_unlit && use_unlit){ lib_vkr.set_unlit(false) }
   true
}

fn _model_matrix_to_render_mat(any: model_m): any {
   if(!is_list(model_m)){ return 0 }
   def n = model_m.len
   if(n == 18){
      def m0, m1 = model_m.get(0, 0), model_m.get(1, 0)
      if((is_int(m0) || is_float(m0)) && (is_int(m1) || is_float(m1)) && int(m0) == 4 && int(m1) == 4){ return model_m }
   }
   if(n == 18 && is_str(model_m.get(16, 0))){
      _scratch_model_conv[0] = 4 _scratch_model_conv[1] = 4
      mut _i = 0 while(_i < 16){ _scratch_model_conv[2 + _i] = _list_num_safe(model_m, _i, 0.0) _i += 1 }
      return _scratch_model_conv
   }
   if(n == 16){
      _scratch_model_conv[0] = 4
      _scratch_model_conv[1] = 4
      mut _i = 0
      while(_i < 16){
         _scratch_model_conv[2 + _i] = _list_num_safe(model_m, _i, 0.0)
         _i += 1
      }
      return _scratch_model_conv
   }
   0
}

fn _num_from_any(any: v, f64: fallback=0.0): f64 {
   if(is_int(v) || is_float(v)){ return float(v) }
   if(is_bool(v)){ return v ? 1.0 : 0.0 }
   fallback
}

fn _dict_num(any: d, str: key, f64: fallback=0.0): f64 {
   if(!is_dict(d)){ return fallback }
   _num_from_any(d.get(key, fallback), fallback)
}

fn _dict_int(any: d, str: key, int: fallback=0): int{ int(_dict_num(d, key, float(fallback))) }

fn _dict_bool(any: d, str: key, bool: fallback=false): bool {
   if(!is_dict(d)){ return fallback }
   def v = d.get(key, fallback)
   if(is_bool(v)){ return v }
   if(is_int(v) || is_float(v)){ return float(v) != 0.0 }
   fallback
}

fn _list_num_safe(any: xs, int: idx, f64: fallback=0.0): f64 {
   if(!is_list(xs) || idx < 0 || idx >= xs.len){ return fallback }
   _num_from_any(xs.get(idx, fallback), fallback)
}

fn _list_int_safe(any: xs, int: idx, int: fallback=0): int{ int(_list_num_safe(xs, idx, float(fallback))) }

fn _list_any_safe(any: xs, int: idx, any: fallback=0): any {
   if(!is_list(xs) || idx < 0 || idx >= xs.len){ return fallback }
   xs[idx]
}

fn _cpu_part_is_optical(any: part): bool {
   if(!is_dict(part)){ return false }
   def slab = part.get("material_slab", 0)
   if(slab){
      def alpha = load32(slab, 24)
      if((alpha & 3) == 2){ return false }
      def bsdf0, bsdf5 = load32(slab, 36), load32(slab, 148)
      return(((bshr(bsdf0, 16) & 255) > 0) || ((bsdf5 & 255) > 0) || ((bshr(bsdf5, 8) & 255) > 0))
   }
   def mesh = part.get("mesh", 0)
   def mesh_is_dict = is_dict(mesh)
   mut mslab = 0
   if(mesh_is_dict){ mslab = mesh.get("material_slab", 0) }
   if(mslab){
      def alpha = load32(mslab, 24)
      if((alpha & 3) == 2){ return false }
      def bsdf0, bsdf5 = load32(mslab, 36), load32(mslab, 148)
      return(((bshr(bsdf0, 16) & 255) > 0) || ((bsdf5 & 255) > 0) || ((bshr(bsdf5, 8) & 255) > 0))
   }
   def mesh_alpha = mesh_is_dict ? int(mesh.get("alpha_u32", 0)) : 0
   def alpha = int(part.get("alpha_u32", mesh_alpha))
   if((alpha & 3) == 2){ return false }
   def mesh_bsdf0 = mesh_is_dict ? int(mesh.get("bsdf0_u32", 0)) : 0
   def bsdf0 = int(part.get("bsdf0_u32", mesh_bsdf0))
   def mesh_bsdf5 = mesh_is_dict ? int(mesh.get("bsdf5_u32", 0)) : 0
   def bsdf5 = int(part.get("bsdf5_u32", mesh_bsdf5))
   ((bshr(bsdf0, 16) & 255) > 0) || ((bsdf5 & 255) > 0) || ((bshr(bsdf5, 8) & 255) > 0)
}

fn _cpu_parts_optical_start(any: parts): int {
   if(!is_list(parts)){ return 0 }
   def parts_n = parts.len
   mut i = 0
   while(i < parts_n){
      if(_cpu_part_is_optical(parts[i])){ return i }
      i += 1
   }
   parts_n
}

fn _active_scene_clamp_ranges(): bool {
   if(_active_scene_optical_start < 0){ _active_scene_optical_start = 0 }
   if(_active_scene_optical_start > _active_scene_count){ _active_scene_optical_start = _active_scene_count }
   if(_active_scene_blend_start < 0){ _active_scene_blend_start = 0 }
   if(_active_scene_blend_start > _active_scene_count){ _active_scene_blend_start = _active_scene_count }
   true
}

fn _active_scene_refresh_cpu_anim(dict: group): bool {
   _active_scene_group = group
   _active_scene_render_parts = group.get("parts", _active_scene_render_parts)
   _active_scene_gpu_slab = 0
   _active_scene_gpu_ready = false
   _active_scene_model_baked = false
   _active_scene_parts_baked = false
   _active_scene_force_cpu_anim = true
   if(is_list(_active_scene_render_parts)){
      def next_count = _active_scene_render_parts.len
      if(next_count != _active_scene_count){ _active_scene_cpu_optical_start = -1 }
      _active_scene_count = next_count
   } else {
      _active_scene_count = 0
      _active_scene_cpu_optical_start = -1
   }
   _active_scene_clamp_ranges()
   _active_scene_have_lights = _active_scene_light_count > 0
   true
}

fn _bind_scene_lights_for_draw(
   bool: have_scene_lights,
   any: light_slab,
   int: light_count,
   any: light_list,
   bool: trace_light_bind=false,
   str: label=""
): bool {
   if(have_scene_lights){
      if(light_slab){
         if(trace_light_bind){ ui_profile.print_text("[scene:call] vk " + label + " set_scene_lights_slab") }
         lib_vkr.set_scene_lights_slab(light_slab, light_count)
      } else {
         if(trace_light_bind){ ui_profile.print_text("[scene:call] vk " + label + " set_scene_lights list len=" + to_str(is_list(light_list) ? light_list.len : -1)) }
         lib_vkr.set_scene_lights(light_list)
      }
   } else {
      if(trace_light_bind){ ui_profile.print_text("[scene:call] vk " + label + " clear_scene_lights") }
      lib_vkr.set_scene_lights_slab(0, 0)
   }
   true
}

fn _scene_cpu_part_draw_info(any: part, any: saved_model, bool: have_saved_model, bool: parts_baked): any {
   if(!is_dict(part) || !part.get("visible", true)){ return 0 }
   def mesh = part.get("mesh", 0)
   def render_model = _model_matrix_to_render_mat(part.get("model", 0))
   mut have_render_model = false
   if(is_list(render_model)){ if(render_model.len == 18){ have_render_model = true } }
   mut draw_model = saved_model
   if(parts_baked && have_render_model){ draw_model = render_model } elif(have_saved_model && have_render_model){
      mat4_mul_into(saved_model, render_model, _scratch_model_mul)
      draw_model = _scratch_model_mul
   }
   if(!_scene_part_frustum_visible(part, draw_model)){ return 0 }
   [mesh,
      part.get("is_lines",
      false),
      part.get("unlit",
      false),
      float(part.get("width",
      1.0)),
      draw_model,
   have_render_model]
}

fn _scene_set_unlit_if_changed(bool: part_unlit, int: last_unlit): int {
   def want_unlit = part_unlit ? 1 : 0
   if(want_unlit != last_unlit){
      lib_vkr.set_unlit(want_unlit == 1)
      return want_unlit
   }
   last_unlit
}

fn _scene_debug_cpu_anim_draw(
   int: i,
   any: mesh,
   any: draw_model,
   bool: cpu_parts_baked,
   bool: have_saved_model,
   bool: have_render_model,
   bool: part_unlit
): bool {
   mut dbg_tx, dbg_ty, dbg_tz = 0.0, 0.0, 0.0
   if(is_list(draw_model) && draw_model.len == 18){
      dbg_tx, dbg_ty = _list_num_safe(draw_model, 14, 0.0), _list_num_safe(draw_model, 15, 0.0)
      dbg_tz = _list_num_safe(draw_model, 16, 0.0)
   }
   mut vwx, vwy, vwz = 0.0, 0.0, 0.0
   if(is_dict(mesh)){
      def dbg_ptr = mesh.get("ptr", 0)
      if(dbg_ptr && is_list(draw_model) && draw_model.len == 18){
         def vx, vy = load32_f32(dbg_ptr, _VKR_OFF_X), load32_f32(dbg_ptr, _VKR_OFF_Y)
         def vz = load32_f32(dbg_ptr, _VKR_OFF_Z)
         def m00 = _list_num_safe(draw_model, 2, 1.0)
         def m01 = _list_num_safe(draw_model, 3, 0.0)
         def m02 = _list_num_safe(draw_model, 4, 0.0)
         def m10 = _list_num_safe(draw_model, 6, 0.0)
         def m11 = _list_num_safe(draw_model, 7, 1.0)
         def m12 = _list_num_safe(draw_model, 8, 0.0)
         def m20 = _list_num_safe(draw_model, 10, 0.0)
         def m21 = _list_num_safe(draw_model, 11, 0.0)
         def m22 = _list_num_safe(draw_model, 12, 1.0)
         vwx, vwy = vx * m00 + vy * m10 + vz * m20 + dbg_tx, vx * m01 + vy * m11 + vz * m21 + dbg_ty
         vwz = vx * m02 + vy * m12 + vz * m22 + dbg_tz
      }
   }
   def mesh_is_dict = is_dict(mesh)
   mut mesh_draw_unlit = false
   mut mesh_draw_nocull = false
   mut mesh_draw_count = 0
   mut mesh_index_count = 0
   if(mesh_is_dict){
      mesh_draw_unlit, mesh_draw_nocull = mesh.get("draw_unlit", false), mesh.get("draw_nocull", false)
      mesh_draw_count = int(mesh.get("draw_count", 0))
      mesh_index_count = int(mesh.get("draw_index_count", 0))
   }
   ui_profile.print_text("[scene:cpu_anim_draw] i=" + to_str(i) +
      " baked=" + to_str(cpu_parts_baked) +
      " have_scene_model=" + to_str(have_saved_model) +
      " have_part_model=" + to_str(have_render_model) +
      " visible=true" +
      " unlit=" + to_str(part_unlit) +
      " mesh_unlit=" + to_str(mesh_draw_unlit) +
      " mesh_nocull=" + to_str(mesh_draw_nocull) +
      " draw=" + to_str(mesh_draw_count) +
      " idx=" + to_str(mesh_index_count) +
      " model_t=(" + to_str(dbg_tx) + "," + to_str(dbg_ty) + "," + to_str(dbg_tz) + ")" +
   " v0_world=(" + to_str(vwx) + "," + to_str(vwy) + "," + to_str(vwz) + ")")
   true
}

fn _scene_draw_cpu_part(
   dict: part,
   any: saved_model,
   bool: have_saved_model,
   bool: parts_baked,
   int: last_unlit,
   bool: debug=false,
   int: debug_index=0
): int {
   def draw_info = _scene_cpu_part_draw_info(part, saved_model, have_saved_model, parts_baked)
   if(!draw_info){ return last_unlit }
   def mesh = draw_info.get(0, 0)
   def part_lines = draw_info.get(1, false)
   def part_unlit = draw_info.get(2, false)
   def part_width = draw_info.get(3, 1.0)
   def draw_model = draw_info.get(4, saved_model)
   def have_render_model = draw_info.get(5, false)
   set_model_matrix(draw_model)
   if(debug){ _scene_debug_cpu_anim_draw(debug_index, mesh, draw_model, parts_baked, have_saved_model, have_render_model, part_unlit) }
   last_unlit = _scene_set_unlit_if_changed(part_unlit, last_unlit)
   draw_mesh(mesh, part_lines, part_width)
   last_unlit
}

fn _scene_cache_group(dict: group): bool {
   _active_scene_group = group
   _active_scene_gpu_slab = 0
   _active_scene_render_parts = group.get("parts", 0)
   _active_scene_gpu_parts = group.get("gpu_parts", 0)
   _active_scene_count = 0
   _active_scene_optical_start = 0
   _active_scene_blend_start = 0
   _active_scene_has_optical = false
   _active_scene_has_blend = true
   _active_scene_model_baked = false
   _active_scene_parts_baked = group.get("parts_model_baked", false) ? true : false
   _active_scene_light_slab = 0
   _active_scene_light_count = 0
   _active_scene_have_lights = false
   _active_scene_gpu_ready = false
   _active_scene_cpu_optical_start = -1
   def gpu_state = group.get("gpu_draw_state", 0)
   mut have_gpu_state = false
   if(is_list(gpu_state)){ if(gpu_state.len >= 7){ have_gpu_state = true } }
   if(have_gpu_state){
      _active_scene_gpu_slab = gpu_state.get(0, 0)
      _active_scene_count = int(gpu_state.get(1, 0))
      if(gpu_state.len >= 9){
         _active_scene_optical_start = int(gpu_state.get(2, 0))
         _active_scene_blend_start = int(gpu_state.get(3, 0))
         _active_scene_has_blend = int(gpu_state.get(4, 1)) != 0
         _active_scene_model_baked = int(gpu_state.get(5, 0)) != 0
         _active_scene_light_slab = gpu_state.get(6, 0)
         _active_scene_light_count = int(gpu_state.get(7, 0))
         _active_scene_has_optical = int(gpu_state.get(8, 0)) != 0
         if(!_active_scene_has_optical && !_active_scene_gpu_slab && (group.get("has_optical", false)? true : false)){ _active_scene_has_optical = true }
      } else {
         _active_scene_optical_start = int(group.get("gpu_optical_start", 0))
         _active_scene_blend_start = int(gpu_state.get(2, 0))
         _active_scene_has_blend = int(gpu_state.get(3, 1)) != 0
         _active_scene_model_baked = int(gpu_state.get(4, 0)) != 0
         _active_scene_light_slab = gpu_state.get(5, 0)
         _active_scene_light_count = int(gpu_state.get(6, 0))
         _active_scene_has_optical = group.get("has_optical", false) ? true : false
      }
   } else {
      def group_scene_lights_init = group.get("scene_lights", [])
      def gpu_parts = group.get("gpu_parts", 0)
      _active_scene_light_slab = group.get("scene_lights_slab", 0)
      if(_active_scene_light_slab){ _active_scene_light_count = int(group.get("scene_lights_count", 0)) } elif(is_list(group_scene_lights_init)){ _active_scene_light_count = group_scene_lights_init.len } else { _active_scene_light_count = 0 }
      _active_scene_has_blend, _active_scene_has_optical = group.get("has_blend", true), group.get("has_optical", false) ? true : false
      _active_scene_model_baked = group.get("gpu_model_baked", false) ? true : false
      _active_scene_parts_baked = group.get("parts_model_baked", _active_scene_model_baked) ? true : false
      _active_scene_gpu_slab = group.get("gpu_parts_slab", 0)
      if(_active_scene_gpu_slab){ _active_scene_count = int(group.get("gpu_parts_count", 0)) } elif(is_list(gpu_parts)){ _active_scene_count = gpu_parts.len } else { _active_scene_count = 0 }
      _active_scene_optical_start = int(group.get("gpu_optical_start", 0))
      _active_scene_blend_start = int(group.get("gpu_blend_start", 0))
   }
   _active_scene_clamp_ranges()
   _active_scene_have_lights = _active_scene_light_count > 0
   _active_scene_gpu_ready = bool(_active_scene_gpu_slab) && _active_scene_count > 0
   _active_scene_force_cpu_anim = (
      int(group.get("anim_count", 0)) > 0 ||
      int(group.get("skin_count", 0)) > 0 ||
      int(group.get("morph_target_count", 0)) > 0
   ) && is_dict(group.get("gltf_data", 0))
   true
}

fn _scene_draw_group_gpu(
   any: gpu_slab, int: gpu_part_count, int: gpu_optical_start, int: gpu_blend_start,
   any: render_parts_cached, any: gpu_parts_cached,
   bool: group_model_baked, bool: group_has_optical, bool: group_has_blend,
   bool: have_scene_lights, any: group_scene_light_slab, int: group_scene_light_count, any: group_scene_lights,
   bool: trace_light_bind, bool: force_cpu_draw
): bool {
   if(_backend != BACKEND_VK || gpu_part_count <= 0 || !bool(gpu_slab) || force_cpu_draw){ return false }
   def gpu_visible_count = _scene_update_gpu_visibility_slab(gpu_slab, gpu_part_count, render_parts_cached, gpu_parts_cached, group_model_baked)
   _bind_scene_lights_for_draw(have_scene_lights, group_scene_light_slab, group_scene_light_count, group_scene_lights, trace_light_bind, "gpu")
   if(gpu_visible_count <= 0){
      lib_vkr.set_mask(0)
      _reset_material_state()
      lib_vkr.set_unlit(false)
      return true
   }
   lib_vkr.set_mask(0)
   if(group_model_baked){
      set_model_matrix(_scratch_ident)
      lib_vkr.set_mask(2)
   } else {
      lib_vkr.set_mask(0)
   }
   mut gpu_drawn = 0
   if(gpu_part_count == 1
      && gpu_optical_start == 0
      && gpu_blend_start >= gpu_part_count
      && !group_has_optical
      && !group_has_blend){
      gpu_drawn += lib_vkr.draw_part0_flat_no_restore(gpu_slab)
   } else {
      if(gpu_optical_start > 0){ gpu_drawn += lib_vkr.draw_parts_flat_range_no_restore(gpu_slab, 0, gpu_optical_start, 0) }
      if(group_has_optical && gpu_optical_start < gpu_part_count){ lib_vkr.capture_scene_color_resume_pass() }
      if(gpu_optical_start < gpu_blend_start){ gpu_drawn += lib_vkr.draw_parts_flat_range_no_restore(gpu_slab, gpu_optical_start, gpu_blend_start, 0) }
      if(group_has_blend && gpu_blend_start < gpu_part_count){ gpu_drawn += lib_vkr.draw_parts_flat_range_no_restore(gpu_slab, gpu_blend_start, gpu_part_count, 1) } elif(!group_has_blend && gpu_optical_start == 0){ gpu_drawn += lib_vkr.draw_parts_flat_range_no_restore(gpu_slab, 0, gpu_part_count, 0) } elif(gpu_optical_start == 0 && gpu_part_count > 0){ gpu_drawn += lib_vkr.draw_parts_flat_range_no_restore(gpu_slab, 0, gpu_part_count, 0) }
   }
   lib_vkr.clear_scene_color_capture()
   if(gpu_drawn <= 0){
      lib_vkr.set_mask(0)
      return false
   }
   _reset_material_state()
   lib_vkr.set_mask(0)
   lib_vkr.set_unlit(false)
   true
}

fn _scene_draw_group_cpu(
   any: render_parts,
   int: render_count,
   bool: force_cpu_anim,
   bool: group_parts_baked,
   bool: group_has_optical,
   bool: group_has_blend,
   bool: have_scene_lights,
   any: group_scene_light_slab,
   int: group_scene_light_count,
   any: group_scene_lights,
   bool: trace_light_bind,
   bool: anim_trace
): bool {
   def cpu_parts_baked = force_cpu_anim ? false : group_parts_baked
   mut cpu_optical_start = _active_scene_cpu_optical_start
   if(cpu_optical_start < 0){
      cpu_optical_start = group_has_optical ? _cpu_parts_optical_start(render_parts) : render_count
      _active_scene_cpu_optical_start = cpu_optical_start
   }
   if(cpu_optical_start < 0){ cpu_optical_start = 0 }
   if(cpu_optical_start > render_count){ cpu_optical_start = render_count }
   mut last_unlit = -1
   def cpu_blend_start = group_has_blend ? _active_scene_blend_start : render_count
   if(!_mat4_valid(_scratch_model_saved_b)){
      _scratch_model_saved_b = render_matrix.mat4_identity()
   }
   mut saved_model2 = _scratch_model_saved_b
   mut have_saved_model = false
   if(is_list(_model_matrix) && _model_matrix.len == 18){
      mut si = 0
      while(si < 18){
         saved_model2[si] = _model_matrix[si]
         si += 1
      }
      have_saved_model = true
   }
   _bind_scene_lights_for_draw(have_scene_lights, group_scene_light_slab, group_scene_light_count, group_scene_lights, trace_light_bind, "cpu")
   lib_vkr.set_mask(0)
   def model_debug_on = _gltf_model_debug_enabled()
   def model_debug_all = model_debug_on && ui_profile.env_truthy_cached("NY_GLTF_MODEL_DEBUG_ALL")
   def cpu_opaque_t0 = anim_trace ? ticks() : 0
   if(cpu_optical_start > 0){
      mut i = 0
      while(i < cpu_optical_start){
         def part = render_parts[i]
         def debug_draw = model_debug_on && (i == 0 || model_debug_all)
         last_unlit = _scene_draw_cpu_part(part, saved_model2, have_saved_model, cpu_parts_baked, last_unlit, debug_draw, i)
         i += 1
      }
   }
   if(anim_trace){ ui_profile.print_text("[anim:draw] cpu_opaque_ms=" + to_str(ui_profile.elapsed_ms(cpu_opaque_t0))) }
   if(group_has_optical && cpu_optical_start < render_count){ lib_vkr.capture_scene_color_resume_pass() }
   def cpu_tail_t0 = anim_trace ? ticks() : 0
   mut pass = 1
   while(pass <= 2){
      mut i = (pass == 1) ? cpu_optical_start : cpu_blend_start
      def pass_end = (pass == 1) ? cpu_blend_start : render_count
      while(i < pass_end){
         def part = render_parts[i]
         def debug_draw = model_debug_on && pass == 1 && (i == cpu_optical_start || model_debug_all)
         last_unlit = _scene_draw_cpu_part(part, saved_model2, have_saved_model, cpu_parts_baked, last_unlit, debug_draw, i)
         i += 1
      }
      pass += 1
   }
   if(anim_trace){ ui_profile.print_text("[anim:draw] cpu_tail_ms=" + to_str(ui_profile.elapsed_ms(cpu_tail_t0))) }
   lib_vkr.clear_scene_color_capture()
   set_model_matrix(saved_model2)
   _reset_material_state()
   lib_vkr.set_unlit(false)
   true
}

fn _scene_group_edit_active(any: group): bool {
   if(!is_dict(group)){ return false }
   abs(float(group.get("edit_tx", 0.0))) > 0.000001 ||
   abs(float(group.get("edit_ty", 0.0))) > 0.000001 ||
   abs(float(group.get("edit_tz", 0.0))) > 0.000001
}

fn draw_mesh_group(any: group): bool {
   "Draws a grouped mesh scene, binding per-part textures when present."
   if(!is_dict(group)){ return false }
   if(to_int(group) != to_int(_active_scene_group)){ _scene_cache_group(group) }
   def force_cpu_anim = _active_scene_force_cpu_anim
   mut force_cpu_draw = force_cpu_anim || (_active_scene_has_blend && _gltf_force_cpu_blend_enabled())
   def anim_trace = _anim_frame_trace_enabled() && _anim_frame_trace_hits < 16
   def anim_trace_t0 = anim_trace ? ticks() : 0
   if(anim_trace){
      ui_profile.print_text("[anim:draw] begin gpu_ready=" + to_str(_active_scene_gpu_ready) +
         " parts=" + to_str(_active_scene_count) +
      " force_cpu_anim=" + to_str(force_cpu_anim))
   }
   if(force_cpu_anim){
      def anim_apply_t0 = anim_trace ? ticks() : 0
      group = _apply_group_gltf_animation(group)
      if(anim_trace){ ui_profile.print_text("[anim:draw] apply_ms=" + to_str(ui_profile.elapsed_ms(anim_apply_t0))) }
      _active_scene_refresh_cpu_anim(group)
   }
   def gpu_slab = _active_scene_gpu_slab
   def gpu_part_count = _active_scene_count
   mut gpu_optical_start = _active_scene_optical_start
   mut gpu_blend_start = _active_scene_blend_start
   def group_has_optical = _active_scene_has_optical
   def group_has_blend = _active_scene_has_blend
   mut group_model_baked = _active_scene_model_baked
   mut group_parts_baked = _active_scene_parts_baked
   def group_scene_light_slab = _active_scene_light_slab
   def group_scene_light_count = _active_scene_light_count
   def have_gpu_slab = _active_scene_gpu_ready
   def have_scene_lights = _active_scene_have_lights
   def render_parts_cached = _active_scene_render_parts
   def gpu_parts_cached = _active_scene_gpu_parts
   def group_has_edit = _scene_group_edit_active(group)
   if(group_has_edit){
      group_model_baked = false
      group_parts_baked = false
      if(is_list(render_parts_cached)){ force_cpu_draw = true }
   }
   def group_scene_lights = (have_scene_lights && !group_scene_light_slab) ? group.get("scene_lights", []) : []
   def trace_light_bind = _light_trace_bind_enabled()
   if(trace_light_bind){
      ui_profile.print_text(
         "[scene:draw] gpu_ready=" + to_str(have_gpu_slab) +
         " gpu_count=" + to_str(gpu_part_count) +
         " optical_start=" + to_str(gpu_optical_start) +
         " baked=" + to_str(group_model_baked) +
         " force_cpu=" + to_str(force_cpu_draw) +
         " has_optical=" + to_str(group_has_optical) +
         " lights=" + to_str(have_scene_lights) +
         " light_count=" + to_str(group_scene_light_count) +
         " slab=" + to_str(group_scene_light_slab) +
         " slab_bool=" + to_str(bool(group_scene_light_slab)) +
         " list_len=" + to_str(is_list(group_scene_lights) ? group_scene_lights.len : -1)
      )
   }
   if(_scene_draw_group_gpu(
         gpu_slab,
         gpu_part_count,
         gpu_optical_start,
         gpu_blend_start,
         render_parts_cached,
         gpu_parts_cached,
         group_model_baked,
         group_has_optical,
         group_has_blend,
         have_scene_lights,
         group_scene_light_slab,
         group_scene_light_count,
         group_scene_lights,
         trace_light_bind,
         force_cpu_draw
   )){ return true }
   def render_parts = render_parts_cached
   if(!is_list(render_parts)){ return false }
   def render_count = (_active_scene_count > 0) ? _active_scene_count : render_parts.len
   if(render_count <= 0){ return false }
   _scene_draw_group_cpu(
      render_parts,
      render_count,
      force_cpu_anim,
      group_parts_baked,
      group_has_optical,
      group_has_blend,
      have_scene_lights,
      group_scene_light_slab,
      group_scene_light_count,
      group_scene_lights,
      trace_light_bind,
      anim_trace
   )
   if(anim_trace){
      ui_profile.print_text("[anim:draw] total_ms=" + to_str(ui_profile.elapsed_ms(anim_trace_t0)))
      _anim_frame_trace_hits += 1
   }
   true
}

fn _mat4_data_off(any: m): int {
   if(!is_list(m)){ return -1 }
   def n = m.len
   if(n >= 18 && int(m.get(0, 0)) == 4 && int(m.get(1, 0)) == 4){ return 2 }
   if(n >= 16){ return 0 }
   -1
}

fn _mat4_valid(any: m): bool{ _mat4_data_off(m) >= 0 }

fn horizontal_mul_mvp(f64: x, f64: y, f64: z, int: row, any: m): f64 {
   "Multiplies point [x,y,z,1] by one row of a column-major MVP matrix."
   def off = _mat4_data_off(m)
   if(off < 0){ return row == 3 ? 1.0 : 0.0 }
   x * float(m.get(off + row, 0.0)) + y * float(m.get(off + 4 + row, 0.0)) + z * float(m.get(off + 8 + row, 0.0)) + float(m.get(off + 12 + row, 0.0))
}

fn _scene_bounds_cullable(any: min, any: max): bool {
   if(!is_list(min) || !is_list(max) || min.len < 3 || max.len < 3){ return false }
   def sx = abs(float(max.get(0, 0.0)) - float(min.get(0, 0.0)))
   def sy = abs(float(max.get(1, 0.0)) - float(min.get(1, 0.0)))
   def sz = abs(float(max.get(2, 0.0)) - float(min.get(2, 0.0)))
   (sx + sy + sz) > 0.000001
}

fn _clip_corner_reject_bits(f64: cx, f64: cy, f64: cz, f64: cw, f64: eps): int {
   mut bits = 0
   if(cx < (0.0 - cw - eps)){ bits = bits | 1 } if(cx > (cw + eps)){ bits = bits | 2 }
   if(cy < (0.0 - cw - eps)){ bits = bits | 4 } if(cy > (cw + eps)){ bits = bits | 8 }
   if(cz < (0.0 - eps)){ bits = bits | 16 } if(cz > (cw + eps)){ bits = bits | 32 }
   bits
}

fn _is_aabb_visible_xform(list: min, list: max, list: mvp, any: model=0): bool {
   if(!_mat4_valid(mvp)){ return true }
   if(!_scene_bounds_cullable(min, max)){ return true }
   def x1, y1, z1 = float(min.get(0, 0.0)), float(min.get(1, 0.0)), float(min.get(2, 0.0))
   def x2, y2, z2 = float(max.get(0, 0.0)), float(max.get(1, 0.0)), float(max.get(2, 0.0))
   mut out_bits = 63 ;; 111111 in binary
   def eps = 0.0001
   def use_model = _mat4_valid(model)
   mut i = 0 while(i < 8){
      def px, py = (i & 1) ? x2 : x1, (i & 2) ? y2 : y1
      def pz = (i & 4) ? z2 : z1
      mut tx, ty, tz = px, py, pz
      if(use_model){
         tx, ty = horizontal_mul_mvp(px, py, pz, 0, model), horizontal_mul_mvp(px, py, pz, 1, model)
         tz = horizontal_mul_mvp(px, py, pz, 2, model)
      }
      def cx, cy = horizontal_mul_mvp(tx, ty, tz, 0, mvp), horizontal_mul_mvp(tx, ty, tz, 1, mvp)
      def cz, cw = horizontal_mul_mvp(tx, ty, tz, 2, mvp), horizontal_mul_mvp(tx, ty, tz, 3, mvp)
      out_bits = out_bits & _clip_corner_reject_bits(cx, cy, cz, cw, eps)
      if(out_bits == 0){ return true } ;; At least one point potentially visible
      i += 1
   }
   false ;; Wholly outside at least one plane
}

fn _is_aabb_visible_model(any: min, any: max, any: model, any: mvp): bool{ _is_aabb_visible_xform(min, max, mvp, model) }

fn _scene_part_frustum_visible(any: part, any: model): bool {
   if(!_gltf_frustum_cull_enabled() || !_mat4_valid(_mvp_matrix)){ return true }
   if(!is_dict(part)){ return true }
   def bmin = part.get("min", 0)
   def bmax = part.get("max", 0)
   if(!_scene_bounds_cullable(bmin, bmax)){ return true }
   _is_aabb_visible_model(bmin, bmax, model, _mvp_matrix)
}

fn _scene_effective_part_model(any: render_model, any: scene_model, bool: group_model_baked): any {
   if(!_mat4_valid(render_model)){ return scene_model }
   if(!group_model_baked && _mat4_valid(scene_model)){
      mat4_mul_into(scene_model, render_model, _scratch_model_mul)
      return _scratch_model_mul
   }
   render_model
}

fn _scene_gpu_part_frame_visible_from_part(any: part, any: scene_model, bool: group_model_baked): bool {
   if(is_dict(part) && !part.get("visible", true)){ return false }
   if(!is_dict(part)){ return true }
   if(!_gltf_frustum_cull_enabled() || !_mat4_valid(_mvp_matrix)){ return true }
   def bmin = part.get("min", 0)
   def bmax = part.get("max", 0)
   if(!_scene_bounds_cullable(bmin, bmax)){ return true }
   def render_model = _model_matrix_to_render_mat(part.get("model", 0))
   def model = _scene_effective_part_model(render_model, scene_model, group_model_baked)
   _is_aabb_visible_model(bmin, bmax, model, _mvp_matrix)
}

fn _scene_gpu_part_frame_visible_from_rec(any: rec, any: scene_model, bool: group_model_baked): bool {
   if(is_list(rec) && _list_int_safe(rec, 37, 1) == 0){ return false }
   if(!is_list(rec)){ return true }
   if(!_gltf_frustum_cull_enabled() || !_mat4_valid(_mvp_matrix)){ return true }
   def bmin = _list_any_safe(rec, 38, 0)
   def bmax = _list_any_safe(rec, 39, 0)
   if(!_scene_bounds_cullable(bmin, bmax)){ return true }
   def render_model = _model_matrix_to_render_mat(_list_any_safe(rec, 10, 0))
   def model = _scene_effective_part_model(render_model, scene_model, group_model_baked)
   _is_aabb_visible_model(bmin, bmax, model, _mvp_matrix)
}

fn _scene_update_gpu_visibility_slab(any: slab_ptr, int: count, any: render_parts, any: gpu_parts, bool: group_model_baked): int {
   if(!slab_ptr || count <= 0){ return 0 }
   def have_render_parts = is_list(render_parts) && render_parts.len >= count
   if(!have_render_parts){ return count }
   def have_gpu_parts = is_list(gpu_parts) && gpu_parts.len >= count
   if(!have_render_parts && !have_gpu_parts){ return count }
   def frustum_on = _gltf_frustum_cull_enabled() && _mat4_valid(_mvp_matrix)
   if(count <= 2 || !frustum_on){
      mut visible_count_fast = 0
      mut fi = 0
      while(fi < count){
         mut visible_fast = true
         if(have_render_parts){
            def part = render_parts[fi]
            if(is_dict(part)){ visible_fast = part.get("visible", true) }
         } else {
            def rec = gpu_parts[fi]
            if(is_list(rec)){ visible_fast = _list_int_safe(rec, 37, 1) != 0 }
         }
         store32(slab_ptr + fi * 256, visible_fast ? 1 : 0, 228)
         if(visible_fast){ visible_count_fast += 1 }
         fi += 1
      }
      return visible_count_fast
   }
   def scene_model = _mat4_valid(_model_matrix) ? _model_matrix : _scratch_ident
   mut visible_count = 0
   mut i = 0
   while(i < count){
      mut visible = true
      if(have_render_parts){ visible = _scene_gpu_part_frame_visible_from_part(render_parts[i], scene_model, group_model_baked) } else { visible = _scene_gpu_part_frame_visible_from_rec(gpu_parts[i], scene_model, group_model_baked) }
      store32(slab_ptr + i * 256, visible ? 1 : 0, 228)
      if(visible){ visible_count += 1 }
      i += 1
   }
   visible_count
}

fn set_wireframe(bool: enabled): bool {
   "Enables or disables wireframe rendering mode(where supported)."
   if(_backend == BACKEND_VK){ lib_vkr.set_wireframe(enabled) }
   elif(_backend == BACKEND_GL){
      ; gl.polygon_mode
   }
   true
}
