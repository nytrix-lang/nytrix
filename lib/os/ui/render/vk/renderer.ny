;; Keywords: render vulkan gpu os ui
;; Vulkan renderer core for frames, command buffers, swapchains, and presentation.
;; References:
;; - std.os.ui.render.vk
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk.renderer(renderer_config, init, _update_default_mvp, _mvp_matrix, set_model_matrix, set_mvp, set_ortho, set_perspective, set_frame_time_sec, begin_frame, set_unlit, set_double_sided, set_vertex_color_mode, _sync_pc, _flush, _check_flush, set_next_frame_load_color, set_scissor_rect, reset_scissor_rect, end_frame, _end_frame_internal, clear, clear_depth, set_clear_color, shutdown, set_wireframe, request_frame_capture, capture_scene_color_resume_pass, clear_scene_color_capture, _create_instance, _create_surface, _pick_physical_device, _create_logical_device, _choose_composite_alpha, _choose_present_mode, set_mask, _create_headless_image, _create_swapchain, _create_swapchain_image_views, _destroy_swapchain_objects, _recreate_swapchain, _create_image_views, _create_depth_resources, _create_render_pass, _create_framebuffers, _create_sync_objects, _create_command_pool, _create_command_buffers, notify_window_resize, get_swapchain_size, get_swapchain_width, get_swapchain_height, set_cam_pos, set_env_tex, set_env_spec_tex, set_scene_lights, set_scene_lights_slab, set_ui_material, set_material_from_slab, draw_parts_flat, draw_parts_flat_range, draw_parts_flat_range_no_restore, draw_parts_flat_range_state_no_restore, draw_part_flat_no_restore, draw_part0_flat_no_restore, draw_part0_flat_state_no_restore, debug_stage)
use std.core
use std.core.mem
use std.math
use std.math.matrix
use std.math.simmd as simmd
use std.os.ui.render.matrix (mat4_identity)
use std.os.ui.window.native as native
use std.os.ui.window as lib_uiw
use std.os.ui.window.consts
use std.os.ui.window.consts (WINDOW_TRANSPARENT)
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.vk.state as vk_state
use std.os.ui.render.vk.utils
use std.os.ui.render.vk.buffers
use std.os.ui.render.vk.texture
use std.os.ui.render.vk.pipeline
use std.os.ui.render.vk.draw (draw_static_buffer_raw, draw_static_buffer_indexed_raw)
use std.os.ui.render.vk.vulkan
use std.os (msleep, ticks)
use std.os.path as ospath
use std.os.process as proc
use std.math.parse.data.json
use std.core.str as text
use std.core.common as common

fn _renderer_alloc(int size) any {
   def p = zalloc(size)
   if !p { panic("vulkan renderer allocation failed") }
   p
}

mut _flush_diag_counter = 0

fn _flush_diag(str msg) any {
   if !(common.env_truthy("NY_GLTF_FORCE_GROUP_DIAG") || common.env_truthy("NY_VK_PIPE_TRACE")) { return 0 }
   if _flush_diag_counter < 24 { ui_profile.print_text("[vk:flush:diag] " + msg) }
   _flush_diag_counter += 1
   0
}

fn _num_from_any(any v, any fallback=0) any {
   if is_int(v) || is_float(v) { return float(v) }
   if is_bool(v) { return v ? 1.0 : 0.0 }
   fallback
}

fn _list_num_safe(any xs, int idx, any fallback=0) any {
   if !is_list(xs) || idx < 0 || idx >= xs.len { return fallback }
   _num_from_any(xs.get(idx, fallback), fallback)
}

fn _scene_light_exposure_from_peak(any peak_light_value) any {
   def peak = float(peak_light_value)
   if peak <= 128.0 { return 1.0 }
   128.0 / peak
}

fn _handle_ok(any h) bool {
   if is_int(h) { return h != 0 }
   true
}

fn _frames_in_flight() int { 4 }

fn _ubo_size_value() int { 384 }

fn _scene_light_max_value() int { 8 }

fn _scene_light_ubo_size_value() int { 384 }

fn _vk_stype_descriptor_set_allocate_info() int { 34 }

fn _vk_stype_write_descriptor_set() int { 35 }

fn _vk_descriptor_uniform_buffer() int { 6 }

fn _headless_swapchain_sentinel() int { 0x8000000001 }

fn _allow_headless_surface() bool { common.env_truthy("NYTRIX_VK_ALLOW_HEADLESS") }

fn _same_int_value(any a, any b) bool {
   if !is_int(a) || !is_int(b) { return false }
   int(a) == int(b)
}

mut _vk_headless_bench_skip_default_mvp = -1
mut _cfg_present_policy = ""

fn _skip_default_mvp_this_frame() bool {
   if _vk_headless_bench_skip_default_mvp != -1 { return _vk_headless_bench_skip_default_mvp == 1 }
   _vk_headless_bench_skip_default_mvp = (common.env_truthy("NY_UI_BENCH") && common.env_truthy("NY_UI_HEADLESS")) ? 1 : 0
   _vk_headless_bench_skip_default_mvp == 1
}

def VK_NOT_READY, VK_TIMEOUT = 1, 2
def _WAYLAND_ACQUIRE_TIMEOUT_NS = 16000000
mut _active_scissor_x, _active_scissor_y, _active_scissor_w, _active_scissor_h = -1, -1, -1, -1
mut _vk_pc_trace_logs = 0

@inline
fn _store_mat4(any dst, any mat) bool {
   if !dst { ui_profile.print_text("[vk] _store_mat4: null ptr") return false }
   if !is_list(mat) || mat.len != 16 { return false }
   mut i = 0
   while i < 16 {
      store32_f32(dst, __load_item_fast(mat, i), ((i & 3) * 16) + ((i >> 2) * 4))
      i += 1
   }
   true
}

fn _store_mat4_cm(any dst, any mat) bool {
   if dst == 0 { ui_profile.print_text("[vk] _store_mat4_cm: null ptr") return false }
   store_mat4_cm_raw(dst, mat, true)
}

fn _store_identity_mat4(any dst) bool {
   if dst == 0 { ui_profile.print_text("[vk] _store_identity_mat4: null ptr") return false }
   memset(dst, 0, 64)
   store32_f32(dst, 1.0, 0)
   store32_f32(dst, 1.0, 20)
   store32_f32(dst, 1.0, 40)
   store32_f32(dst, 1.0, 60)
   true
}

fn renderer_config(any vsync, any filter, any vert_spv_path, any frag_spv_path, any msaa) any {
   "Configures the renderer. Must be called BEFORE init_window().
   vsync: true/false(default false)
   filter: 0 for NEAREST, 1 for LINEAR(default 0)
   vert_spv_path: path to custom vertex shader .spv or empty for default
   frag_spv_path: path to custom fragment shader .spv or empty for default
   msaa: number of MSAA samples(1, 2, 4, 8) (default 4)"
   _cfg_present_policy = is_str(vsync) ? text.lower(vsync) : ""
   if _cfg_present_policy.len > 0 {
      _cfg_vsync = (_cfg_present_policy == "fifo" || _cfg_present_policy == "vsync")
   } elif ui_profile.env_present_cached("NY_UI_VSYNC") {
      _cfg_vsync = ui_profile.env_truthy_cached("NY_UI_VSYNC")
   } else {
      _cfg_vsync = is_nil(vsync) ? false : !!vsync
   }
   _cfg_filter = filter ? 1 : 0
   _cfg_vert_spv = vert_spv_path
   _cfg_frag_spv = frag_spv_path
   def s = int(msaa)
   if s >= 8 { _cfg_msaa = 8 }
   elif s >= 4 { _cfg_msaa = 4 }
   elif s >= 2 { _cfg_msaa = 2 }
   else { _cfg_msaa = 1 }
}

mut _last_mvp_w, _last_mvp_h = -1, -1
mut _current_is_ortho, _last_ortho_valid = false, false
mut _last_ortho_l, _last_ortho_r, _last_ortho_b, _last_ortho_t = 0.0, 0.0, 0.0, 0.0
mut _last_ortho_n, _last_ortho_f = 0.0, 0.0
mut _last_persp_valid = false
mut _last_persp_fovy, _last_persp_aspect, _last_persp_near, _last_persp_far = 0.0, 0.0, 0.0, 0.0
mut _vk_markers_enabled, _ident_mat = false, 0
mut _logged_suboptimal_acquire, _logged_suboptimal_present = false, false
;; One-shot per frame: when _vertex_limit_hit trips we used to silently drop
;; the rest of the frame's text with no diagnostic. Track whether we have
;; already logged the truncation for this frame so we surface the issue once
;; instead of spamming on every subsequent _check_flush call.
mut _logged_vertex_limit_hit = false
mut _logged_waiting_acquire, _suboptimal_recreate_attempted = false, false
mut _verbose_last_report_frame, _deep_last_report_frame = -1, -1
mut _old_swapchain_hint, _backend_is_wayland_cached, _backend_is_win32_cached = 0, -1, -1
mut _vk_deep_debug, _vk_deep_emit_frame = -1, 0
mut _vk_deep_begin_recreate_ms, _vk_deep_begin_acquire_ms = 0.0, 0.0
mut _vk_deep_begin_wait_ms, _vk_deep_begin_reset_fence_ms = 0.0, 0.0
mut _vk_deep_begin_cmd_ms, _vk_deep_begin_rp_ms, _vk_deep_begin_vp_ms = 0.0, 0.0, 0.0
mut _vk_deep_begin_total_ms, _vk_deep_end_flush_ms = 0.0, 0.0
mut _vk_deep_end_cmd_ms, _vk_deep_end_submit_ms, _vk_deep_end_present_ms = 0.0, 0.0, 0.0
mut _vk_deep_end_total_ms = 0.0
mut _depth_format, _render_pass_load, _render_pass_load_color_clear_depth = 126, 0, 0
mut _next_begin_load_color = false
mut _scene_color_capture_tex_id, _active_scene_color_tex_id = -1, -1
mut _scene_color_capture_w, _scene_color_capture_h = 0, 0
mut _swapchain_image_views_count, _framebuffers_count, _command_buffers_count = 0, 0, 0
mut _image_available_semaphores_count, _render_finished_semaphores_count, _in_flight_fences_count = 0, 0, 0
mut _scene_light_frame_mask = 0
mut _trace_light_bind, _trace_light_detail, _trace_group, _trace_mat, _handles_valid = false, false, false, false, false
mut _proc_trace_next_sample_frame_vk = 0
mut _pending_resize_stamp_ns = 0
mut _resize_debounce_ms_cache = -1
mut _scene_light_tmp_slab = 0
mut _current_emissive_tex_word, _current_base_tex_word = 0x80000000, 0x80000000
mut _current_occlusion_tex_word, _current_normal_tex_word = 0x80000000, 0x80000000
mut _current_material_key = 0
mut _current_mat_slab_cache, _current_mat_slab_ptr = 0, 0
mut _flat_fast_on = -1
mut _material_state_no_sync = -1
mut _flat_cache_last_tex, _flat_cache_last_unlit = -2147483647, 0
mut _flat_cache_last_mat_ptr, _flat_cache_last_mat_key = 0, 0
mut _main_deletion_queue, _frame_deletion_queues = [], [[], [], [], []]
mut _draw_images, _draw_image_views, _draw_image_memories = [], [], []
mut _draw_image_views_count, _draw_image_format, _draw_extent_w, _draw_extent_h = 0, 0, 0, 0
mut _vk_offscreen_draw_mode = -1
mut _present_copy_slab, _present_dst_bar, _present_copy_region = 0, 0, 0
def _DQ_IMAGE_VIEW, _DQ_IMAGE_MEMORY = 1, 2
def VK_FORMAT_FEATURE_BLIT_SRC_BIT, VK_FORMAT_FEATURE_BLIT_DST_BIT = 0x00000400, 0x00000800

@inline
fn _backend_is_wayland_fast() bool {
   if _backend_is_wayland_cached < 0 { _backend_is_wayland_cached = (lib_uiw.backend() == "wayland") ? 1 : 0 }
   _backend_is_wayland_cached == 1
}

@inline
fn _backend_is_win32_fast() bool {
   if _backend_is_win32_cached < 0 { _backend_is_win32_cached = (lib_uiw.backend() == "win32") ? 1 : 0 }
   _backend_is_win32_cached == 1
}

fn _surface_wait_timeout_ns() int {
   #windows { return _WAYLAND_ACQUIRE_TIMEOUT_NS }
   #else { return _WAYLAND_ACQUIRE_TIMEOUT_NS }
   #endif
}

fn _pump_host_messages_if_needed() any {
   #windows { lib_uiw.poll_events() }
   #endif
}

fn _wait_for_fence_with_host_pump(any fence, int timeout_ns) int {
   store64_h(_ptr_fence, fence, 0)
   if !_backend_is_win32_fast() {
      return wait_for_fences(_device, 1, _ptr_fence, 1, timeout_ns)
   }
   mut elapsed = 0
   mut slice = 1000000
   if timeout_ns >= 0 && timeout_ns < slice { slice = timeout_ns }
   while true {
      _pump_host_messages_if_needed()
      def res = wait_for_fences(_device, 1, _ptr_fence, 1, slice)
      _pump_host_messages_if_needed()
      if !_same_int_value(res, VK_NOT_READY) && !_same_int_value(res, VK_TIMEOUT) { return res }
      if timeout_ns < 0 { return res }
      elapsed += slice
      if elapsed >= timeout_ns { return res }
      msleep(0)
   }
   VK_TIMEOUT
}

fn _vk_profile_every() int {
   ui_profile.env_int_cached("NY_VK_PROFILE_EVERY", 120, 1, 1000000)
}

fn _vk_deep_every() int {
   ui_profile.env_int_cached("NY_DEBUG_DEEP_EVERY", 60, 1, 1000000)
}

fn _vk_profile_enabled() bool {
   ui_profile.env_truthy_cached("NY_VK_PROFILE_TRACE")
}

fn _vk_profile_dump_enabled() bool {
   ui_profile.env_toggle_cached("NY_VK_PROFILE_DUMP", _vk_profile_enabled())
}

fn _vk_profile_dump_file() str {
   ui_profile.dump_path("NY_VK_PROFILE_DUMP_PATH", "nytrix_vk_profile.oasset.jsonl")
}

fn _vk_frame_scratch_ready() bool {
   if _handles_valid { return true }
   _handle_ok(_ptr_fence) && _handle_ok(_ptr_img_idx) && _handle_ok(_ptr_bi) && _handle_ok(_ptr_clear) &&
   _handle_ok(_ptr_ri) && _handle_ok(_ptr_vp) && _handle_ok(_ptr_sci) && _handle_ok(_flush_off) &&
   _handle_ok(_flush_buf) && _handle_ok(_pc_buffer) && _handle_ok(_current_mvp) && _handle_ok(_current_model)
}

fn _vk_refresh_handles_valid() bool {
   _handles_valid = _handle_ok(_device) && _handle_ok(_render_pass) && _handle_ok(_ubo_map) && _handle_ok(_vertex_buffer) && _vk_frame_scratch_ready()
   _handles_valid
}

fn _vk_frame_targets_ready(any has_surface=-1) bool {
   def surface_live = int(has_surface) >= 0 ? bool(has_surface) : _has_valid_surface()
   if surface_live && !_handle_ok(_swapchain) { return false }
   if !_framebuffers_slab || !_cmd_bufs_slab || !_sem_avail_slab || !_sem_finish_slab || !_fences_slab { return false }
   if _framebuffers_count < 1
   || _command_buffers_count < 1
   || _image_available_semaphores_count < 1
   || _render_finished_semaphores_count < 1
   || _in_flight_fences_count < 1{
      return false
   }
   if _current_frame < 0 || _current_frame >= _command_buffers_count { return false }
   if _current_frame >= _image_available_semaphores_count
   || _current_frame >= _render_finished_semaphores_count
   || _current_frame >= _in_flight_fences_count{
      return false
   }
   if _offscreen_draw_enabled()
   && (_draw_image_views_count < _swapchain_image_count
      || _draw_images.len < _swapchain_image_count){
      return false
   }
   true
}

fn _vk_alloc_handle_slab(any old, int count) any {
   _vk_stage("handle_slab.enter")
   if old {
      _vk_stage("handle_slab.free")
      __free(old)
   }
   _vk_stage("handle_slab.count")
   if count < 1 { return 0 }
   _vk_stage("handle_slab.alloc")
   def p = _renderer_alloc(count * 8)
   _vk_stage("handle_slab.memset")
   if p { memset(p, 0, count * 8) }
   _vk_stage("handle_slab.done")
   p
}

fn _vk_handle_slab_ready(any slab, int count, int idx) bool {
   if !slab || count < 1 || idx < 0 || idx >= count { return false }
   _handle_ok(load64_h(slab, int(idx) * 8))
}

fn _depth_format_supported(int fmt) bool {
   if !_physical_device || fmt <= 0 { return false }
   mut props = _renderer_alloc(16)
   get_physical_device_format_properties(_physical_device, fmt, props)
   def optimal = load32(props, 4)
   free(props)
   band(optimal, 0x00000200) != 0
}

fn _choose_depth_format() int {
   def candidates = [126, 124, 129]
   mut i = 0
   def candidates_n = candidates.len
   while i < candidates_n {
      def fmt = int(candidates[i])
      if _depth_format_supported(fmt) { return fmt }
      i += 1
   }
   126
}

fn _dq_flush_item(any item) bool {
   if !is_list(item) || item.len < 2 || _device == 0 { return false }
   def kind = int(item.get(0, 0))
   if kind == _DQ_IMAGE_VIEW {
      def view = item.get(1, 0)
      if view { destroy_image_view(_device, view, 0) }
      return true
   }
   if kind == _DQ_IMAGE_MEMORY {
      def image = item.get(1, 0)
      def memory = item.get(2, 0)
      if image { destroy_image(_device, image, 0) }
      if memory { free_memory(_device, memory, 0) }
      return true
   }
   false
}

fn _dq_flush(any queue) list {
   if !is_list(queue) { return [] }
   mut i = queue.len - 1
   while i >= 0 {
      _dq_flush_item(queue[i])
      i -= 1
   }
   []
}

fn _dq_push_main(int kind, any a, any b=0, any c=0) bool {
   if !is_list(_main_deletion_queue) { _main_deletion_queue = [] }
   _main_deletion_queue = _main_deletion_queue.append([kind, a, b, c])
   true
}

fn _flush_main_deletion_queue() bool {
   _main_deletion_queue = _dq_flush(_main_deletion_queue)
   true
}

fn _ensure_frame_deletion_queues() bool {
   if is_list(_frame_deletion_queues) && _frame_deletion_queues.len >= _frames_in_flight() { return true }
   _frame_deletion_queues = []
   mut i = 0
   while i < _frames_in_flight() {
      _frame_deletion_queues = _frame_deletion_queues.append([])
      i += 1
   }
   true
}

fn _flush_frame_deletion_queue(int frame_idx) bool {
   if !is_list(_frame_deletion_queues) || _frame_deletion_queues.len < _frames_in_flight() { _ensure_frame_deletion_queues() }
   def idx = int(frame_idx)
   if idx < 0 || idx >= _frame_deletion_queues.len { return false }
   def q = _frame_deletion_queues.get(idx, [])
   if !is_list(q) || q.len <= 0 { return true }
   _frame_deletion_queues[idx] = _dq_flush(q)
   true
}

fn _offscreen_draw_enabled() bool {
   if _vk_offscreen_draw_mode != -1 { return _vk_offscreen_draw_mode == 1 }
   if ui_profile.env_present_cached("NY_VK_OFFSCREEN_DRAW") {
      _vk_offscreen_draw_mode = ui_profile.env_truthy_cached("NY_VK_OFFSCREEN_DRAW") ? 1 : 0
   } else {
      _vk_offscreen_draw_mode = 0
   }
   _vk_offscreen_draw_mode == 1
}

fn _format_optimal_supports(int fmt, int feature) bool {
   if !_physical_device || fmt <= 0 { return false }
   mut props = _renderer_alloc(16)
   get_physical_device_format_properties(_physical_device, fmt, props)
   def optimal = load32(props, 4)
   free(props)
   band(optimal, feature) != 0
}

fn _choose_draw_image_format() int {
   if !_offscreen_draw_enabled() { return _swapchain_format }
   if common.env_lower("NY_VK_DRAW_IMAGE_FORMAT") == "swapchain" { return _swapchain_format }
   if _format_optimal_supports(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
   _format_optimal_supports(_swapchain_format, VK_FORMAT_FEATURE_BLIT_DST_BIT){
      return VK_FORMAT_R16G16B16A16_SFLOAT
   }
   _swapchain_format
}

fn _prepare_draw_image_format() bool {
   _draw_image_format = _choose_draw_image_format()
   if _draw_image_format <= 0 { _draw_image_format = _swapchain_format }
   true
}

fn _render_color_format() int {
   if _offscreen_draw_enabled() {
      if _draw_image_format <= 0 { _prepare_draw_image_format() }
      return _draw_image_format
   }
   _swapchain_format
}

fn _format_is_srgb(int fmt) bool {
   fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB
}

fn _srgb_to_linear_channel(any x) f64 {
   mut c = float(x)
   if c <= 0.0 { return 0.0 }
   if c >= 1.0 { return 1.0 }
   if c <= 0.04045 { return c / 12.92 }
   pow((c + 0.055) / 1.055, 2.4)
}

fn _clear_color_channel(any x) f64 {
   _format_is_srgb(_render_color_format()) ? _srgb_to_linear_channel(x) : float(x)
}

fn _render_final_color_layout() int {
   if _offscreen_draw_enabled() { return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL }
   _has_valid_surface() ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
}

fn _frame_draw_image() any {
   if _offscreen_draw_enabled() { return _draw_images.get(_image_index, 0) }
   _swapchain_images.get(_image_index, 0)
}

fn _frame_draw_image_pass_layout() int { _render_final_color_layout() }
mut _vk_debug_verbose = -1
mut _vk_debug_basic = 0
mut _vk_begin_false_logs = 0
mut _vk_debug_stage = ""

fn _vk_stage(str stage) bool {
   _vk_debug_stage = to_str(stage)
   if ui_profile.env_truthy_cached("NY_VK_STAGE_TRACE") { ui_profile.print_text("[gfx:vulkan:stage] " + _vk_debug_stage) }
   true
}

fn debug_stage() str {
   "Runs the debug stage operation."
   _vk_debug_stage
}

fn _vk_begin_trace_enabled() bool { _vk_debug_basic == 1 || vk_state._debug_gfx_enabled || ui_profile.env_truthy_cached("NY_VK_BEGIN_TRACE") }

fn _vk_begin_false(str reason) bool {
   if _vk_begin_trace_enabled() && _vk_begin_false_logs < 48 {
      _vk_begin_false_logs += 1
      ui_profile.print_text("[gfx:vulkan] begin_frame false reason=" + reason +
         " frame=" + to_str(_current_frame) +
         " image=" + to_str(_image_index) +
         " extent=" + to_str(_swapchain_extent_w) + "x" + to_str(_swapchain_extent_h) +
         " pending=" + to_str(_swapchain_recreate_pending) +
         " swap_imgs=" + to_str(_swapchain_image_count) +
         " fb=" + to_str(_framebuffers_count) +
         " cmd=" + to_str(_command_buffers_count) +
         " sem=" + to_str(_image_available_semaphores_count) +
      " fence=" + to_str(_in_flight_fences_count))
   }
   false
}

fn _vk_validation_enabled() bool {
   ui_profile.env_truthy_cached("NY_VK_VALIDATION")
}

fn _debug_deep() bool {
   if _vk_deep_debug == -1 { _vk_deep_debug = ui_profile.debug_deep_enabled() ? 1 : 0 }
   _vk_deep_debug == 1
}

fn _vk_deep_should_emit(int frame) bool {
   (int(frame) % _vk_deep_every()) == 0
}

fn _vk_init_trace_enabled() bool {
   if _debug_deep() { return true }
   ui_profile.env_truthy_cached("NY_VK_INIT_TRACE")
}

fn _vk_init_trace(str stage, any t0) any {
   _pump_host_messages_if_needed()
   if !_vk_init_trace_enabled() { return 0.0 }
   def ms = ui_profile.elapsed_ms(t0)
   ui_profile.print_text("[vk:init] " + stage + "=" + to_str(ms) + "ms")
   ms
}

fn _resize_debounce_ms() int {
   if _resize_debounce_ms_cache < 0 {
      _resize_debounce_ms_cache = ui_profile.env_int_cached("NY_VK_RESIZE_DEBOUNCE_MS", 0, 0, 1000)
   }
   _resize_debounce_ms_cache
}

fn _resize_debounce_waiting(bool force) bool {
   if force || _pending_resize_stamp_ns <= 0 { return false }
   def ms = _resize_debounce_ms()
   if ms <= 0 { return false }
   (float(ticks() - _pending_resize_stamp_ns) / 1000000.0) < float(ms)
}

fn _schedule_swapchain_recreate(bool force=false) bool {
   if _pending_resize_w <= 0 { _pending_resize_w = _swapchain_extent_w }
   if _pending_resize_h <= 0 { _pending_resize_h = _swapchain_extent_h }
   if _pending_resize_stamp_ns <= 0 { _pending_resize_stamp_ns = ticks() }
   _swapchain_recreate_pending = true
   if force { _swapchain_recreate_force = true }
   _suboptimal_recreate_attempted = false
   true
}

fn _destroy_scene_color_capture_tex() any {
   if _scene_color_capture_tex_id >= 0 { destroy_texture(_scene_color_capture_tex_id) }
   _scene_color_capture_tex_id = -1
   _active_scene_color_tex_id = -1
   _scene_color_capture_w = 0
   _scene_color_capture_h = 0
   0
}

fn _packed_env_scene_word() int {
   if _active_scene_color_tex_id >= 0 {
      def scene_word = band(_active_scene_color_tex_id + 1, 0x7ff)
      def env_word = (_current_env_tex_id >= 0) ? band(_current_env_tex_id + 1, 0x7ff) : 0
      return bor(0x80000000, bor(scene_word, bshl(env_word, 11)))
   }
   _current_env_tex_id
}

fn _ensure_scene_color_capture_tex() bool {
   if _swapchain_extent_w <= 0 || _swapchain_extent_h <= 0 { return false }
   def capture_format = _render_color_format()
   if _scene_color_capture_tex_id >= 0 {
      def tex_obj = _textures.get(_scene_color_capture_tex_id, 0)
      if is_dict(tex_obj) &&
      _scene_color_capture_w == _swapchain_extent_w &&
      _scene_color_capture_h == _swapchain_extent_h &&
      int(tex_obj.get("format", 0)) == capture_format {
         return true
      }
   }
   _destroy_scene_color_capture_tex()
   def tex_id = create_texture_ex(_swapchain_extent_w, _swapchain_extent_h, 0, capture_format, 1, 33071, 33071, false)
   if tex_id < 0 { return false }
   _scene_color_capture_tex_id = tex_id
   _scene_color_capture_w = _swapchain_extent_w
   _scene_color_capture_h = _swapchain_extent_h
   bindless_sync_texture_slot(tex_id)
   true
}

fn _begin_load_render_pass() bool {
   if !_frame_open { return false }
   if !_handle_ok(_render_pass_load) { return false }
   if !_vk_handle_slab_ready(_framebuffers_slab, _framebuffers_count, _image_index) { return false }
   def fb = load64_h(_framebuffers_slab, _image_index * 8)
   if !_handle_ok(fb) { return false }
   store32(_ptr_ri, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0)
   store64_h(_ptr_ri, 0, 8)
   store64_h(_ptr_ri, _render_pass_load, 16)
   store64_h(_ptr_ri, fb, 24)
   store32(_ptr_ri, 0, 32)
   store32(_ptr_ri, 0, 36)
   store32(_ptr_ri, _swapchain_extent_w, 40)
   store32(_ptr_ri, _swapchain_extent_h, 44)
   store32(_ptr_ri, 0, 48)
   store64_h(_ptr_ri, 0, 56)
   cmd_begin_render_pass(_current_frame_cb, _ptr_ri, 0)
   store32_f32(_ptr_vp, 0.0, 0)
   store32_f32(_ptr_vp, float(_swapchain_extent_h), 4)
   store32_f32(_ptr_vp, float(_swapchain_extent_w), 8)
   store32_f32(_ptr_vp, -float(_swapchain_extent_h), 12)
   store32_f32(_ptr_vp, 0.0, 16)
   store32_f32(_ptr_vp, 1.0, 20)
   cmd_set_viewport(_current_frame_cb, 0, 1, _ptr_vp)
   store32(_ptr_sci, 0, 0)
   store32(_ptr_sci, 0, 4)
   store32(_ptr_sci, _swapchain_extent_w, 8)
   store32(_ptr_sci, _swapchain_extent_h, 12)
   cmd_set_scissor(_current_frame_cb, 0, 1, _ptr_sci)
   _active_scissor_x, _active_scissor_y = 0, 0
   _active_scissor_w, _active_scissor_h = _swapchain_extent_w, _swapchain_extent_h
   _last_bound_pipe = 0
   _last_bound_ds = 0
   _last_bound_ubo_ds = 0
   _last_static_vbo = 0
   _last_static_vbo_off = 0
   _last_static_ibuf = 0
   _last_static_ibuf_off = -1
   _last_static_ibuf_type = -1
   _last_line_width = -1.0
   _dynamic_vbo_bound = false
   _pc_dirty = true
   true
}

fn set_next_frame_load_color(bool enabled) any {
   "Requests the next begin_frame to load the existing color attachment instead of clearing it."
   _next_begin_load_color = enabled ? true : false
}

fn _apply_scissor_rect_i32(int x, int y, int w, int h) bool {
   if !_frame_open || !_current_frame_cb { return false }
   mut sx, sy = int(x), int(y)
   mut sw, sh = int(w), int(h)
   if sx < 0 {
      sw += sx
      sx = 0
   }
   if sy < 0 {
      sh += sy
      sy = 0
   }
   if sw < 0 { sw = 0 }
   if sh < 0 { sh = 0 }
   if sx > _swapchain_extent_w { sx = _swapchain_extent_w }
   if sy > _swapchain_extent_h { sy = _swapchain_extent_h }
   if sx + sw > _swapchain_extent_w { sw = max(0, _swapchain_extent_w - sx) }
   if sy + sh > _swapchain_extent_h { sh = max(0, _swapchain_extent_h - sy) }
   if sx == _active_scissor_x && sy == _active_scissor_y && sw == _active_scissor_w && sh == _active_scissor_h { return true }
   _flush()
   store32(_ptr_sci, sx, 0)
   store32(_ptr_sci, sy, 4)
   store32(_ptr_sci, sw, 8)
   store32(_ptr_sci, sh, 12)
   cmd_set_scissor(_current_frame_cb, 0, 1, _ptr_sci)
   _active_scissor_x, _active_scissor_y = sx, sy
   _active_scissor_w, _active_scissor_h = sw, sh
   true
}

fn set_scissor_rect(int x, int y, int w, int h) bool {
   "Applies a clipped scissor rectangle for subsequent draw calls."
   _apply_scissor_rect_i32(x, y, w, h)
}

fn reset_scissor_rect() bool {
   "Restores scissor to the full swapchain extent."
   _apply_scissor_rect_i32(0, 0, _swapchain_extent_w, _swapchain_extent_h)
}

fn capture_scene_color_resume_pass() bool {
   "Runs the capture scene color resume pass operation."
   if !_frame_open || !_current_frame_cb { return false }
   if !_ensure_scene_color_capture_tex() { return false }
   if _scene_color_capture_tex_id < 0 { return false }
   def tex_obj = _textures.get(_scene_color_capture_tex_id, 0)
   if !is_dict(tex_obj) { return false }
   def dst_image = tex_obj.get("image", 0)
   if !_handle_ok(dst_image) { return false }
   def src_image = _frame_draw_image()
   if !_handle_ok(src_image) { return false }
   _flush()
   cmd_end_render_pass(_current_frame_cb)
   if !_scene_capture_slab {
      _scene_capture_slab = _renderer_alloc(212)
      if !_scene_capture_slab { return false }
      memset(_scene_capture_slab, 0, 212)
      _scene_capture_src_bar = _scene_capture_slab
      _scene_capture_dst_bar = _scene_capture_slab + 72
      _scene_capture_copy_region = _scene_capture_slab + 144
   }
   def src_bar = _scene_capture_src_bar
   def dst_bar = _scene_capture_dst_bar
   def copy_region = _scene_capture_copy_region
   if !src_bar || !dst_bar || !copy_region { return false }
   VkImageMemoryBarrierColor(src_bar,
      src_image,
      0,
      VK_ACCESS_TRANSFER_READ_BIT,
      _frame_draw_image_pass_layout(),
   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
   VkImageMemoryBarrierColor(dst_bar,
      dst_image,
      VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
   cmd_pipeline_barrier(_current_frame_cb,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0,
      0,
      0,
      0,
      0,
      1,
   src_bar)
   cmd_pipeline_barrier(_current_frame_cb,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0,
      0,
      0,
      0,
      0,
      1,
   dst_bar)
   memset(copy_region, 0, 68)
   store32(copy_region, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(copy_region, 0, 4)
   store32(copy_region, 0, 8)
   store32(copy_region, 1, 12)
   store32(copy_region, 0, 16)
   store32(copy_region, 0, 20)
   store32(copy_region, 0, 24)
   store32(copy_region, VK_IMAGE_ASPECT_COLOR_BIT, 28)
   store32(copy_region, 0, 32)
   store32(copy_region, 0, 36)
   store32(copy_region, 1, 40)
   store32(copy_region, _swapchain_extent_w, 56)
   store32(copy_region, _swapchain_extent_h, 60)
   store32(copy_region, 1, 64)
   cmd_copy_image(_current_frame_cb,
      src_image,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      dst_image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1,
   copy_region)
   VkImageMemoryBarrierColor(src_bar,
      src_image,
      VK_ACCESS_TRANSFER_READ_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
   VkImageMemoryBarrierColor(dst_bar,
      dst_image,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
   cmd_pipeline_barrier(_current_frame_cb,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0,
      0,
      0,
      0,
      0,
      1,
   src_bar)
   cmd_pipeline_barrier(_current_frame_cb,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0,
      0,
      0,
      0,
      0,
      1,
   dst_bar)
   _active_scene_color_tex_id = _scene_color_capture_tex_id
   _pc_dirty = true
   _begin_load_render_pass()
}

fn clear_scene_color_capture() any {
   "Runs the clear scene color capture operation."
   if _active_scene_color_tex_id < 0 { return 0 }
   _active_scene_color_tex_id = -1
   _pc_dirty = true
   0
}

fn _render_pass_dependency() any {
   def dep = _renderer_alloc(28)
   store32(dep, -1, 0)
   store32(dep, 0, 4)
   store32(dep, 0x00000400, 8)
   store32(dep, 0x00000400, 12)
   store32(dep, 0, 16)
   store32(dep, 0x00000100, 20)
   store32(dep, 0, 24)
   dep
}

fn _store_attachment_desc(
   any atts,
   int off,
   int format,
   int samples,
   int load_op,
   int store_op,
   int stencil_load_op,
   int stencil_store_op,
   int initial_layout,
   int final_layout
) any {
   store32(atts, format, off + 4)
   store32(atts, samples, off + 8)
   store32(atts, load_op, off + 12)
   def safe_store_op = store_op == 2 ? 1 : store_op
   def safe_stencil_store_op = stencil_store_op == 2 ? 1 : stencil_store_op
   store32(atts, safe_store_op, off + 16)
   store32(atts, stencil_load_op, off + 20)
   store32(atts, safe_stencil_store_op, off + 24)
   store32(atts, initial_layout, off + 28)
   store32(atts, final_layout, off + 32)
}

fn _create_render_pass_handle(any atts, int att_count, any subpass_desc) any {
   def create_info = _renderer_alloc(64)
   store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
   store32(create_info, att_count, 20)
   store64_h(create_info, atts, 24)
   store32(create_info, 1, 32)
   store64_h(create_info, subpass_desc, 40)
   store32(create_info, 1, 48)
   store64_h(create_info, _render_pass_dependency(), 56)
   def pass_ptr = _renderer_alloc(8)
   if create_render_pass(_device, create_info, 0, pass_ptr) != 0 { return 0 }
   load64_h(pass_ptr, 0)
}

fn _create_load_render_pass(any msaa, int final_color_layout) bool {
   if _render_pass_load { destroy_render_pass(_device, _render_pass_load, 0) _render_pass_load = 0 }
   def color_format = _render_color_format()
   if msaa {
      mut atts = _renderer_alloc(108)
      _store_attachment_desc(atts, 0, color_format, _cfg_msaa, 0, 2, 2, 2, 2, 2)
      _store_attachment_desc(atts, 36, _depth_format, _cfg_msaa, 0, 2, 2, 2, 3, 3)
      _store_attachment_desc(atts, 72, color_format, 1, 0, 0, 2, 2, 2, final_color_layout)
      mut car = _renderer_alloc(8) store32(car, 0, 0) store32(car, 2, 4)
      mut dar = _renderer_alloc(8) store32(dar, 1, 0) store32(dar, 3, 4)
      mut rar = _renderer_alloc(8) store32(rar, 2, 0) store32(rar, 2, 4)
      mut sd = _renderer_alloc(72)
      store32(sd, 0, 4)
      store32(sd, 1, 24)
      store64_h(sd, car, 32)
      store64_h(sd, rar, 40)
      store64_h(sd, dar, 48)
      _render_pass_load = _create_render_pass_handle(atts, 3, sd)
      return _render_pass_load != 0
   }
   mut atts = _renderer_alloc(72)
   _store_attachment_desc(atts, 0, color_format, 1, 0, 0, 2, 2, 2, final_color_layout)
   _store_attachment_desc(atts, 36, _depth_format, 1, 0, 2, 2, 2, 3, 3)
   mut car = _renderer_alloc(8) store32(car, 0, 0) store32(car, 2, 4)
   mut dar = _renderer_alloc(8) store32(dar, 1, 0) store32(dar, 3, 4)
   mut sd = _renderer_alloc(72)
   store32(sd, 0, 4)
   store32(sd, 1, 24)
   store64_h(sd, car, 32)
   store64_h(sd, dar, 48)
   _render_pass_load = _create_render_pass_handle(atts, 2, sd)
   _render_pass_load != 0
}

fn _create_load_color_clear_depth_render_pass(any msaa, int final_color_layout) bool {
   if _render_pass_load_color_clear_depth {
      destroy_render_pass(_device, _render_pass_load_color_clear_depth, 0)
      _render_pass_load_color_clear_depth = 0
   }
   def color_format = _render_color_format()
   if msaa {
      mut atts = _renderer_alloc(108)
      _store_attachment_desc(atts, 0, color_format, _cfg_msaa, 0, 2, 2, 2, 2, 2)
      _store_attachment_desc(atts, 36, _depth_format, _cfg_msaa, 1, 2, 2, 2, 3, 3)
      _store_attachment_desc(atts, 72, color_format, 1, 0, 0, 2, 2, 2, final_color_layout)
      mut car = _renderer_alloc(8) store32(car, 0, 0) store32(car, 2, 4)
      mut dar = _renderer_alloc(8) store32(dar, 1, 0) store32(dar, 3, 4)
      mut rar = _renderer_alloc(8) store32(rar, 2, 0) store32(rar, 2, 4)
      mut sd = _renderer_alloc(72)
      store32(sd, 0, 4)
      store32(sd, 1, 24)
      store64_h(sd, car, 32)
      store64_h(sd, rar, 40)
      store64_h(sd, dar, 48)
      _render_pass_load_color_clear_depth = _create_render_pass_handle(atts, 3, sd)
      return _render_pass_load_color_clear_depth != 0
   }
   mut atts = _renderer_alloc(72)
   _store_attachment_desc(atts, 0, color_format, 1, 0, 0, 2, 2, 2, final_color_layout)
   _store_attachment_desc(atts, 36, _depth_format, 1, 1, 2, 2, 2, 3, 3)
   mut car = _renderer_alloc(8) store32(car, 0, 0) store32(car, 2, 4)
   mut dar = _renderer_alloc(8) store32(dar, 1, 0) store32(dar, 3, 4)
   mut sd = _renderer_alloc(72)
   store32(sd, 0, 4)
   store32(sd, 1, 24)
   store64_h(sd, car, 32)
   store64_h(sd, dar, 48)
   _render_pass_load_color_clear_depth = _create_render_pass_handle(atts, 2, sd)
   _render_pass_load_color_clear_depth != 0
}

comptime template _vk_init_stage_guard(name, call_fn, stage_label, dbg_only){
   fn ${name}() bool {
      if !call_fn() {
         if dbg_only { if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] init failed stage=" + stage_label) } } else { ui_profile.print_text("[gfx:vulkan] init failed stage=" + stage_label) }
         return false
      }
      true
   }
}

comptime emit _vk_init_stage_guard(_vk_init_stage_create_vertex_buffer, _create_vertex_buffer, "create_vertex_buffer", true)
comptime emit _vk_init_stage_guard(_vk_init_stage_create_staging_buffer, _create_staging_buffer, "create_staging_buffer", true)
comptime emit _vk_init_stage_guard(_vk_init_stage_create_descriptor_pool, _create_descriptor_pool, "create_descriptor_pool", true)
comptime emit _vk_init_stage_guard(_vk_init_stage_create_uniform_buffer, _create_uniform_buffer, "create_uniform_buffer", true)
comptime emit _vk_init_stage_guard(_vk_init_stage_create_ubo_descriptor_sets, _create_ubo_descriptor_sets, "create_ubo_descriptor_sets", true)
comptime emit _vk_init_stage_guard(_vk_init_stage_create_default_texture, _create_default_texture, "create_default_texture", true)

fn _vk_check_vertex_buffer() bool { _vertex_buffer != 0 }

fn _vk_check_staging_buffer() bool { _staging_buffer != 0 }

fn _vk_check_descriptor_pool() bool { _descriptor_pool && _descriptor_pool != 0 }

fn _vk_check_uniform_buffer() bool { _ubo_buffer != 0 }

fn _vk_check_ubo_descriptor_sets() bool {
   if !_ubo_ds_slab { return false }
   mut i = 0
   while i < _frames_in_flight() {
      if load64(_ubo_ds_slab, i * 8) == 0 { return false }
      i += 1
   }
   true
}

comptime template _vk_init_run_checked(name, stage_fn, stage_label, check_fn, fail_msg){
   fn ${name}() bool {
      def t0 = ticks()
      if !stage_fn() { return false }
      if !check_fn() {
         print(fail_msg)
         return false
      }
      _vk_init_trace(stage_label, t0)
      true
   }
}

comptime emit _vk_init_run_checked(_vk_init_run_vertex_buffer,
   _vk_init_stage_create_vertex_buffer, "create_vertex_buffer",
_vk_check_vertex_buffer, "[gfx:vulkan] vertex buffer creation failed: null handle")
comptime emit _vk_init_run_checked(_vk_init_run_staging_buffer,
   _vk_init_stage_create_staging_buffer, "create_staging_buffer",
_vk_check_staging_buffer, "[gfx:vulkan] staging buffer creation failed: null handle")
comptime emit _vk_init_run_checked(_vk_init_run_descriptor_pool,
   _vk_init_stage_create_descriptor_pool, "create_descriptor_pool",
_vk_check_descriptor_pool, "[gfx:vulkan] descriptor pool creation failed: null handle")
comptime emit _vk_init_run_checked(_vk_init_run_uniform_buffer,
   _vk_init_stage_create_uniform_buffer, "create_uniform_buffer",
_vk_check_uniform_buffer, "[gfx:vulkan] uniform buffer creation failed: null handle")
comptime emit _vk_init_run_checked(_vk_init_run_ubo_descriptor_sets,
   _vk_init_stage_create_ubo_descriptor_sets, "create_ubo_descriptor_sets",
_vk_check_ubo_descriptor_sets, "[gfx:vulkan] UBO descriptor sets allocation failed: none allocated")

fn init(any win) bool {
   "Initializes the Vulkan renderer for the given window."
   def init_t0 = ticks()
   mut stage_t0 = init_t0
   _handles_valid = false
   _window_ref = win
   _check_debug_env()
   _backend_is_wayland_cached = (lib_uiw.backend() == "wayland") ? 1 : 0
   _backend_is_win32_cached = (lib_uiw.backend() == "win32") ? 1 : 0
   _vk_deep_debug, _vk_debug_basic, _vk_debug_verbose = ui_profile.debug_deep_enabled() ? 1 : 0, ui_profile.gfx_frame_trace_enabled() ? 1 : 0, ui_profile.debug_verbose_enabled() ? 1 : 0
   def light_trace_raw = ui_profile.env_lower_cached("NY_UI_LIGHT_TRACE")
   _trace_light_bind, _trace_light_detail = light_trace_raw == "bind", light_trace_raw == "1"
   _trace_group, _trace_mat = ui_profile.env_truthy_cached("NY_UI_GROUP_TRACE"), ui_profile.env_truthy_cached("NY_UI_MAT_TRACE")
   _vk_init_trace("check_debug_env", stage_t0)
   if win {
      def flags = int(win.get("flags", 0))
      _current_window_flags = flags
      if band(flags, 32) {
         _clear_r, _clear_g, _clear_b, _clear_a = 0.0, 0.0, 0.0, 0.0
      }
   }
   if ui_profile.env_present_cached("NYTRIX_VK_VERTEX_MB") {
      def n = ui_profile.env_int_cached("NYTRIX_VK_VERTEX_MB", 0, 0, 262144)
      if n >= 8 { _vertex_capacity = n * 1024 * 1024 }
   }
   if ui_profile.env_present_cached("NYTRIX_VK_STAGING_MB") {
      def n = ui_profile.env_int_cached("NYTRIX_VK_STAGING_MB", 0, 0, 262144)
      if n >= 16 { _staging_capacity = n * 1024 * 1024 }
   }
   if common.env_truthy("NYTRIX_VK_MARKERS") { _vk_markers_enabled = true }
   if common.env_present("RENDERDOC")
   || common.env_present("RENDERDOC_CAPTUREOPTS")
   || common.env_present("RENDERDOC_CMD"){
      _vk_markers_enabled = true
   }
   if common.env_truthy("NYTRIX_FAST") {
      _cfg_vsync = false
      _cfg_filter = 0
      if common.env_truthy("NY_UI_FAST_MSAA_OFF") { _cfg_msaa = 1 }
   }
   stage_t0 = ticks()
   if !_create_instance() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_instance") return false }
   if !_instance || _instance == 0 { ui_profile.print_text("[gfx:vulkan] instance creation failed: null handle") return false }
   _vk_init_trace("create_instance", stage_t0)
   stage_t0 = ticks()
   if !_create_surface(win) { ui_profile.print_text("[gfx:vulkan] init failed stage=create_surface") return false }
   if _surface == 0 { ui_profile.print_text("[gfx:vulkan] surface creation failed: null handle") return false }
   _vk_init_trace("create_surface", stage_t0)
   stage_t0 = ticks()
   if !_pick_physical_device() { ui_profile.print_text("[gfx:vulkan] init failed stage=pick_physical_device") return false }
   if _physical_device == 0 { ui_profile.print_text("[gfx:vulkan] physical device selection failed: null handle") return false }
   _vk_init_trace("pick_physical_device", stage_t0)
   stage_t0 = ticks()
   if !_create_logical_device() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_logical_device") return false }
   if _device == 0 { ui_profile.print_text("[gfx:vulkan] logical device creation failed: null handle") return false }
   _vk_init_trace("create_logical_device", stage_t0)
   stage_t0 = ticks()
   if !_create_swapchain(win) { ui_profile.print_text("[gfx:vulkan] init failed stage=create_swapchain") return false }
   if _swapchain == 0 { ui_profile.print_text("[gfx:vulkan] swapchain creation failed: null handle") return false }
   _vk_init_trace("create_swapchain", stage_t0)
   stage_t0 = ticks()
   if !_create_swapchain_image_views() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_image_views") return false }
   if _swapchain_image_views_count < 1 { ui_profile.print_text("[gfx:vulkan] swapchain image views creation failed: none allocated") return false }
   _vk_init_trace("create_image_views", stage_t0)
   stage_t0 = ticks()
   if !_prepare_draw_image_format() { ui_profile.print_text("[gfx:vulkan] init failed stage=prepare_draw_image_format") return false }
   _vk_init_trace("prepare_draw_image_format", stage_t0)
   stage_t0 = ticks()
   if !_create_depth_resources() {
      if _cfg_msaa > 1 {
         def requested_msaa = _cfg_msaa
         _destroy_depth_msaa_resources()
         _cfg_msaa = 1
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] MSAA " + to_str(requested_msaa) + "x unsupported for depth/color target; falling back to 1x") }
      }
      if !_create_depth_resources() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_depth_resources") return false }
   }
   if _depth_image == 0 || _depth_view == 0 { ui_profile.print_text("[gfx:vulkan] depth resources creation failed: null handle") return false }
   _vk_init_trace("create_depth_resources", stage_t0)
   stage_t0 = ticks()
   if !_create_render_pass() {
      if _cfg_msaa > 1 {
         def requested_msaa = _cfg_msaa
         _destroy_depth_msaa_resources()
         _cfg_msaa = 1
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] MSAA " + to_str(requested_msaa) + "x render pass unsupported; falling back to 1x") }
         if !_create_depth_resources() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_depth_resources_msaa_fallback") return false }
      }
      if !_create_render_pass() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_render_pass") return false }
   }
   if _render_pass == 0 { ui_profile.print_text("[gfx:vulkan] render pass creation failed: null handle") return false }
   _vk_init_trace("create_render_pass", stage_t0)
   stage_t0 = ticks()
   if !_create_graphics_pipeline() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_graphics_pipeline") return false }
   if _pipeline == 0 { ui_profile.print_text("[gfx:vulkan] graphics pipeline creation failed: null handle") return false }
   _vk_init_trace("create_graphics_pipeline", stage_t0)
   stage_t0 = ticks()
   if !_create_draw_images() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_draw_images") return false }
   _vk_init_trace("create_draw_images", stage_t0)
   stage_t0 = ticks()
   if !_create_framebuffers() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_framebuffers") return false }
   if _framebuffers_count < 1 { ui_profile.print_text("[gfx:vulkan] framebuffer creation failed: none allocated") return false }
   _vk_init_trace("create_framebuffers", stage_t0)
   stage_t0 = ticks()
   if !_create_sync_objects() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_sync_objects") return false }
   if !_image_available_semaphores || !_render_finished_semaphores || !_in_flight_fences { ui_profile.print_text("[gfx:vulkan] sync objects creation failed: missing semaphores/fences") return false }
   _vk_init_trace("create_sync_objects", stage_t0)
   stage_t0 = ticks()
   _create_command_pool()
   if !_command_pool || _command_pool == 0 { ui_profile.print_text("[gfx:vulkan] command pool creation failed: null handle") return false }
   _vk_init_trace("create_command_pool", stage_t0)
   stage_t0 = ticks()
   if !_create_command_buffers() { ui_profile.print_text("[gfx:vulkan] init failed stage=create_command_buffers") return false }
   if _command_buffers_count < 1 { ui_profile.print_text("[gfx:vulkan] command buffer allocation failed: none allocated") return false }
   _vk_init_trace("create_command_buffers", stage_t0)
   if !_vk_init_run_vertex_buffer() { return false }
   if !_vk_init_run_staging_buffer() { return false }
   if !_vk_init_run_descriptor_pool() { return false }
   if !_vk_init_run_uniform_buffer() { return false }
   if !_vk_init_run_ubo_descriptor_sets() { return false }
   _upload_cb = 0
   _upload_slab = _renderer_alloc(368)
   memset(_upload_slab, 0, 368)
   _upload_alloc, _upload_bi, _upload_bar1, _upload_bar2 = _upload_slab, _upload_slab + 32, _upload_slab + 64, _upload_slab + 136
   _upload_region, _upload_si, _upload_cb_arr = _upload_slab + 208, _upload_slab + 264, _upload_slab + 336
   _upload_cb_ptr, _flush_off, _flush_buf = _upload_slab + 344, _upload_slab + 352, _upload_slab + 360
   mut fence_ci = _renderer_alloc(16)
   store32(fence_ci, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
   _upload_fence_ptr = _renderer_alloc(8)
   def upload_fence_res = create_fence(_device, fence_ci, 0, _upload_fence_ptr)
   if upload_fence_res != 0 {
      ui_profile.print_text("[gfx:vulkan] upload fence creation failed code=" + to_str(upload_fence_res))
      return false
   }
   _upload_fence = load64_h(_upload_fence_ptr, 0)
   if !_upload_fence {
      ui_profile.print_text("[gfx:vulkan] upload fence creation returned null")
      return false
   }
   free(fence_ci)
   def _mat_slab_bytes = 128 + shader_pc_bytes() * 2
   _mat_slab = _renderer_alloc(_mat_slab_bytes)
   memset(_mat_slab, 0, _mat_slab_bytes)
   _current_mvp      = _mat_slab
   _current_model    = _mat_slab + 64
   _pc_buffer        = _mat_slab + 128
   _pc_buffer_custom = _mat_slab + 128 + shader_pc_bytes()
   _ident_mat = mat4_identity()
   _reset_texture_descriptor_state()
   if _texture_fmt_cache { free(_texture_fmt_cache) }
   _texture_fmt_cache = _renderer_alloc(MAX_TEXTURES)
   memset(_texture_fmt_cache, 0, MAX_TEXTURES)
   _frame_slab = _renderer_alloc(712)
   memset(_frame_slab, 0, 712)
   _ptr_fence      = _frame_slab
   _ptr_img_idx    = _frame_slab + 8
   _ptr_bi         = _frame_slab + 16
   _ptr_clear      = _frame_slab + 80
   _ptr_ri         = _frame_slab + 176
   _ptr_vp         = _frame_slab + 304
   _ptr_sci        = _frame_slab + 336
   _ptr_dsl        = _frame_slab + 368
   _ptr_ds         = _frame_slab + 376
   _ptr_sub        = _frame_slab + 392
   _ptr_wait_sems  = _frame_slab + 520
   _ptr_sig_sems   = _frame_slab + 552
   _ptr_stages     = _frame_slab + 584
   store32(_ptr_bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(_ptr_bi, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 16)
   store32(_ptr_sub, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store64_h(_ptr_sub, 0, 8)
   store64_h(_ptr_sub, _ptr_wait_sems, 24)
   store64_h(_ptr_sub, _ptr_stages, 32)
   store32(_ptr_sub, 1, 40)
   store64_h(_ptr_sub, _ptr_sub + 80, 48)
   store64_h(_ptr_sub, _ptr_sig_sems, 64)
   store32(_ptr_stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0)
   _quad_template = _renderer_alloc(_VKR_VERT_STRIDE * 6)
   _init_quad_template()
   if !_vk_init_stage_create_default_texture() { return false }
   _svbo_slab = _renderer_alloc(16)
   memset(_svbo_slab, 0, 16)
   _static_vbo_ptr = _svbo_slab
   _static_off_ptr = _svbo_slab + 8
   store64_h(_static_off_ptr, 0, 0)
   _scene_capture_slab = _renderer_alloc(212)
   if _scene_capture_slab {
      memset(_scene_capture_slab, 0, 212)
      _scene_capture_src_bar = _scene_capture_slab
      _scene_capture_dst_bar = _scene_capture_slab + 72
      _scene_capture_copy_region = _scene_capture_slab + 144
   }
   _update_default_mvp(_window_ref)
   _vk_refresh_handles_valid()
   _vk_init_trace("total", init_t0)
   true
}

fn _update_default_mvp(any win) any {
   def w, h = float(_swapchain_extent_w), float(_swapchain_extent_h)
   if int(w) == _last_mvp_w && int(h) == _last_mvp_h &&
   _last_ortho_valid && _current_is_ortho &&
   _last_ortho_l == 0.0 && _last_ortho_r == w &&
   _last_ortho_b == h && _last_ortho_t == 0.0 &&
   _last_ortho_n == -1.0 && _last_ortho_f == 1.0{
      return 0
   }
   _last_mvp_w, _last_mvp_h = int(w), int(h)
   set_ortho(0.0, w, 0.0, h, -1.0, 1.0)
}

fn _mvp_matrix() list {
   mut m = mat4_identity()
   if _current_mvp { memcpy(m + 16, _current_mvp, 128) }
   return m
}

fn set_model_matrix(any mat) any {
   "Updates the Model matrix for subsequent 3D draw calls."
   if !_handle_ok(_current_model) || !is_list(mat) { return 0 }
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 2
      _flush()
   }
   def changed = _store_mat4_cm(_current_model, mat)
   if !changed {
      _model_dirty = false
      return 0
   }
   _model_dirty = true
   _pc_dirty = true
   0
}

fn set_mvp(any mat) any {
   "Updates the View-Projection matrix for the renderer."
   if _handle_ok(_current_mvp) && is_list(mat) {
      if _vertex_offset != _last_flush_offset { _flush() }
      if _store_mat4_cm(_current_mvp, mat) {
         _mvp_dirty = true
         _pc_dirty = true
         _current_is_ortho = false
         _last_ortho_valid = false
         _last_persp_valid = false
      }
   }
   0
}

fn set_ortho(any l, any r, any b, any t, any n, any f) any {
   "Sets the MVP matrix to an orthographic projection(row-major memory layout, Vulkan clip Z=[0,1])."
   if b < t { def tmp = b b = t t = tmp }
   if !_handle_ok(_current_mvp) { return 0 }
   l, r, b, t, n, f = float(l), float(r), float(b), float(t), float(n), float(f)
   if _last_ortho_valid && _current_is_ortho &&
   _last_ortho_l == l && _last_ortho_r == r &&
   _last_ortho_b == b && _last_ortho_t == t &&
   _last_ortho_n == n && _last_ortho_f == f{
      return 0
   }
   def rl = float(r - l)
   def tb = float(t - b)
   def fnv = float(f - n)
   if rl == 0.0 || tb == 0.0 || fnv == 0.0 { return 0 }
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 2
      _flush()
   }
   def mat = [
      2.0 / rl, 0.0, 0.0, -float(r + l) / rl,
      0.0, 2.0 / tb, 0.0, -float(t + b) / tb,
      0.0, 0.0, 1.0 / fnv, -float(n) / fnv,
      0.0, 0.0, 0.0, 1.0
   ]
   if _store_mat4(_current_mvp, mat) {
      _mvp_dirty = true
      _pc_dirty = true
      _current_is_ortho = true
      _last_ortho_valid = true
      _last_ortho_l = l
      _last_ortho_r = r
      _last_ortho_b = b
      _last_ortho_t = t
      _last_ortho_n = n
      _last_ortho_f = f
      _last_persp_valid = false
   }
   0
}

fn _mat4_perspective(any fov, any aspect, any near, any far) list {
   def f = 1.0 / tan(float(fov) * 0.5)
   def nf = 1.0 / (float(near) - float(far))
   [
      f/float(aspect), 0.0, 0.0, 0.0,
      0.0, f, 0.0, 0.0,
      0.0, 0.0, float(far)*nf, float(near)*float(far)*nf,
      0.0, 0.0, -1.0, 0.0
   ]
}

fn set_perspective(any fovy, any aspect, any near, any far) any {
   "Sets the View-Projection matrix to a perspective projection."
   fovy, aspect, near, far = float(fovy), float(aspect), float(near), float(far)
   if _last_persp_valid && !_current_is_ortho &&
   _last_persp_fovy == fovy && _last_persp_aspect == aspect &&
   _last_persp_near == near && _last_persp_far == far{
      return 0
   }
   def mat = _mat4_perspective(fovy, aspect, near, far)
   if _handle_ok(_current_mvp) && is_list(mat) {
      if _vertex_offset != _last_flush_offset {
         _flush_reason = 2
         _flush()
      }
      if _store_mat4(_current_mvp, mat) {
         _mvp_dirty = true
         _pc_dirty = true
         _current_is_ortho = false
         _last_ortho_valid = false
         _last_persp_valid = true
         _last_persp_fovy = fovy
         _last_persp_aspect = aspect
         _last_persp_near = near
         _last_persp_far = far
      }
   }
   0
}

fn set_frame_time_sec(f64 t) any {
   "Sets the frame time uniform used by animated materials and debug shaders."
   _frame_time_sec = t
}

fn draw_parts_flat(?ptr slab_ptr, int count, int pass_num) int {
   "Draws flat packed scene parts with minimal interpreter overhead."
   draw_parts_flat_range(slab_ptr, 0, count, pass_num)
}

fn draw_parts_flat_range(?ptr slab_ptr, int start_idx, int end_idx, int pass_num) int {
   "Draws a contiguous flat packed range and restores the caller model matrix."
   _draw_parts_flat_range_impl(slab_ptr, start_idx, end_idx, pass_num, true, true)
}

fn draw_parts_flat_range_no_restore(?ptr slab_ptr, int start_idx, int end_idx, int pass_num) int {
   "Draws a contiguous flat packed range without restoring model matrix."
   _draw_parts_flat_range_impl(slab_ptr, start_idx, end_idx, pass_num, false, true)
}

fn draw_parts_flat_range_state_no_restore(?ptr slab_ptr, int start_idx, int end_idx, int pass_num) int {
   "Draws packed parts using cached material state while still validating bindless texture slots."
   def safe_sync = !common.env_truthy("NY_VK_MATERIAL_STATE_NO_SYNC")
   _draw_parts_flat_range_impl(slab_ptr, start_idx, end_idx, pass_num, false, safe_sync)
}

fn draw_part_flat_no_restore(?ptr slab_ptr, int idx, int pass_num) int {
   "Draws one packed scene part without the flat-range loop overhead."
   _draw_part_flat_base_no_restore(slab_ptr + idx * 256)
}

fn _flat_part_active(?ptr base) bool { !!base && load32_h(base, 228) != 0 }

fn _sync_flat_part_model(?ptr base) any {
   def model_ptr = base + 64
   if memcmp(model_ptr, _current_model, 64) != 0 {
      memcpy(_current_model, model_ptr, 64)
      _model_dirty = true
      _pc_dirty = true
   }
   0
}

@inline
fn _mat4_ptr_is_identity(any p) bool {
   if !p { return false }
   load32_f32(p, 0) == 1.0 && load32_f32(p, 20) == 1.0 && load32_f32(p, 40) == 1.0 && load32_f32(p, 60) == 1.0 &&
   load32_f32(p, 4) == 0.0 && load32_f32(p, 8) == 0.0 && load32_f32(p, 12) == 0.0 &&
   load32_f32(p, 16) == 0.0 && load32_f32(p, 24) == 0.0 && load32_f32(p, 28) == 0.0 &&
   load32_f32(p, 32) == 0.0 && load32_f32(p, 36) == 0.0 && load32_f32(p, 44) == 0.0 &&
   load32_f32(p, 48) == 0.0 && load32_f32(p, 52) == 0.0 && load32_f32(p, 56) == 0.0
}

@inline
fn _mat4_ptr_mul_store(any a, any b, any dst) bool {
   if !a || !b || !dst { return false }
   simmd.mat4_mul_ptr(a, b, dst)
   true
}

fn _sync_flat_part_model_composed(?ptr base, any base_model) any {
   if !base_model || _mat4_ptr_is_identity(base_model) { return _sync_flat_part_model(base) }
   def model_ptr = base + 64
   _mat4_ptr_mul_store(base_model, model_ptr, _current_model)
   _model_dirty = true
   _pc_dirty = true
   0
}

fn _restore_flat_base_model(any base_model) bool {
   if !base_model || !_current_model || memcmp(base_model, _current_model, 64) == 0 { return false }
   memcpy(_current_model, base_model, 64)
   _model_dirty = true
   _pc_dirty = true
   true
}

fn _sync_flat_part_shading(?ptr base, bool sync_tex=true, bool sync_mat=true, bool trace=false) any {
   if sync_tex {
      def tex_id = load32_h(base, 0)
      if tex_id >= 0 { bind_texture(tex_id) }
      else { bind_default_texture() }
   }
   def is_unlit = load32_h(base, 132) != 0
   if is_unlit != _current_is_unlit { set_unlit(is_unlit) }
   if !sync_mat { return 0 }
   def mat_ptr = base + 136
   def mat_key = load32_h(base, 128)
   if trace && _trace_group {
      ui_profile.print_text("[group:draw0] tex=" + to_str(load32_h(base, 0)) +
         " matKey=" + to_str(load32_h(base, 128)) +
         " matTex=" + to_str(load32_h(mat_ptr, 20)) +
         " mat=0x" + text.to_hex(load32_h(mat_ptr, 4)) +
      " normal=0x" + text.to_hex(load32_h(mat_ptr, 100)))
   }
   if mat_key != 0 { _set_material_from_part_slab_key(mat_ptr, mat_key) }
   else { _set_material_from_part_slab(mat_ptr) }
   0
}

@inline
fn _vk_flat_fast_on_cached() bool {
   if _flat_fast_on < 0 { _flat_fast_on = common.env_truthy("NY_VK_FLAT_FAST_ON") ? 1 : 0 }
   _flat_fast_on == 1
}

@inline
fn _draw_flat_part_buffers_loaded(
   ?ptr base,
   any sbuf,
   int soff,
   any ibuf,
   int ioff,
   int idx_count,
   int draw_count,
   int index_type,
   int topo,
   any pipe,
   bool flat_fast_on
) int {
   def is_lines = topo == 1
   def is_points = topo == 2
   def use_pipe = pipe ? pipe : 0
   if topo == 0 && use_pipe && flat_fast_on && _draw_static_mesh_part_fast(sbuf, soff, ibuf, ioff, idx_count, draw_count, index_type, use_pipe) {
   } elif ibuf && idx_count > 0 {
      draw_static_buffer_indexed_raw(sbuf, soff, ibuf, ioff, idx_count, is_lines, 1.0, use_pipe, index_type, is_points)
   } else {
      draw_static_buffer_raw(sbuf, soff, draw_count, is_lines, 1.0, use_pipe, is_points)
   }
   1
}

fn _draw_flat_part_buffers(?ptr base) int {
   def sbuf = load64(base, 16)
   if !sbuf { return 0 }
   def soff = load64_h(base, 24)
   def ibuf = load64(base, 32)
   def ioff = load64_h(base, 40)
   def idx_count = load32_h(base, 48)
   def draw_count = load32_h(base, 52)
   def index_type = load32_h(base, 56)
   def topo = load32_h(base, 60)
   _draw_flat_part_buffers_loaded(base, sbuf, soff, ibuf, ioff, idx_count, draw_count, index_type, topo, load64(base, 8), _vk_flat_fast_on_cached())
}

@inline
fn _flat_range_sync_model_cache_raw(?ptr base, int last_model_key) int {
   def model_key = load32_h(base, 252)
   if model_key != 0 && model_key == last_model_key && !_model_dirty { return last_model_key }
   _sync_flat_part_model(base)
   model_key
}

@inline
fn _flat_range_sync_model_cache_composed(?ptr base, int last_model_key, any base_model) int {
   def model_key = load32_h(base, 252)
   if model_key != 0 && model_key == last_model_key && !_model_dirty { return last_model_key }
   def model_ptr = base + 64
   _mat4_ptr_mul_store(base_model, model_ptr, _current_model)
   _model_dirty = true
   _pc_dirty = true
   model_key
}

@inline
fn _flat_range_reset_shading_cache() any {
   _flat_cache_last_tex = -2147483647
   _flat_cache_last_unlit = _current_is_unlit ? 1 : 0
   _flat_cache_last_mat_ptr = 0
   _flat_cache_last_mat_key = 0
   0
}

@inline
fn _flat_range_sync_shading_cache(?ptr base, bool sync_material_textures) any {
   if sync_material_textures {
      def tex_id = load32_h(base, 0)
      if tex_id != _flat_cache_last_tex {
         if tex_id >= 0 { bind_texture(tex_id) }
         else { bind_default_texture() }
         _flat_cache_last_tex = tex_id
      }
   }
   def is_unlit = load32_h(base, 132) != 0
   def want_unlit = is_unlit ? 1 : 0
   if want_unlit != _flat_cache_last_unlit {
      set_unlit(is_unlit)
      _flat_cache_last_unlit = want_unlit
   }
   def mat_ptr = base + 136
   def mat_key = load32_h(base, 128)
   if mat_key != 0 {
      if mat_key != _flat_cache_last_mat_key {
         if sync_material_textures { _set_material_from_part_slab_key(mat_ptr, mat_key) }
         else { _set_material_from_part_slab_key_no_sync(mat_ptr, mat_key) }
         _flat_cache_last_mat_key = mat_key
         _flat_cache_last_mat_ptr = mat_ptr
      }
   } elif mat_ptr != _flat_cache_last_mat_ptr {
      if !_flat_cache_last_mat_ptr || memcmp(mat_ptr, _flat_cache_last_mat_ptr, 116) != 0 { _set_material_from_part_slab(mat_ptr) }
      _flat_cache_last_mat_key = 0
      _flat_cache_last_mat_ptr = mat_ptr
   }
   0
}

fn draw_part0_flat_no_restore(?ptr slab_ptr) int {
   "Draws packed scene part 0 without index arithmetic for single-part hot paths."
   if !_flat_part_active(slab_ptr) { return 0 }
   def base_model = _snapshot_model_matrix(_scratch_model_saved_a)
   _sync_flat_part_model_composed(slab_ptr, base_model)
   _sync_flat_part_shading(slab_ptr, true, true, true)
   def drawn = _draw_flat_part_buffers(slab_ptr)
   if drawn && _current_is_unlit { set_unlit(false) }
   _restore_flat_base_model(base_model)
   drawn
}

fn draw_part0_flat_state_no_restore(?ptr slab_ptr) int {
   "Draws packed scene part 0 assuming material and texture state were already loaded."
   if !_handle_ok(_current_frame_cb) || !_handle_ok(_pipeline_layout) || !_handle_ok(_current_model) { return 0 }
   if !_flat_part_active(slab_ptr) { return 0 }
   def base_model = _snapshot_model_matrix(_scratch_model_saved_a)
   _sync_flat_part_model_composed(slab_ptr, base_model)
   _sync_flat_part_shading(slab_ptr, false, false, false)
   def drawn = _draw_flat_part_buffers(slab_ptr)
   if drawn && _current_is_unlit { set_unlit(false) }
   _restore_flat_base_model(base_model)
   drawn
}

fn _draw_part_flat_base_no_restore(?ptr base) int {
   if !_handle_ok(_current_frame_cb) || !_handle_ok(_pipeline_layout) || !_handle_ok(_current_model) { return 0 }
   if !_flat_part_active(base) { return 0 }
   def base_model = _snapshot_model_matrix(_scratch_model_saved_a)
   _sync_flat_part_model_composed(base, base_model)
   _sync_flat_part_shading(base, true, true, false)
   def drawn = _draw_flat_part_buffers(base)
   if drawn && _current_is_unlit { set_unlit(false) }
   _restore_flat_base_model(base_model)
   drawn
}

fn _draw_static_mesh_part_fast(any sbuf, int soff, any ibuf, int ioff, int idx_count, int draw_count, int index_type, any pipe) bool {
   if !_frame_open || !sbuf || (!ibuf && draw_count <= 0) || (ibuf && idx_count <= 0) || !pipe { return false }
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 3
      _flush()
   }
   def cb = _current_frame_cb
   if !_vkr_bind_pipeline_if_needed(cb, pipe) { return false }
   def ubo_ds = _current_frame_ubo_ds
   if _bindless_ds && (_bindless_ds != _last_bound_ds || ubo_ds != _last_bound_ubo_ds) {
      store64_h(_ptr_ds, _bindless_ds, 0)
      store64_h(_ptr_ds, ubo_ds, 8)
      cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 2, _ptr_ds, 0, 0)
      _last_bound_ds = _bindless_ds
      _last_bound_ubo_ds = ubo_ds
      _descriptor_bind_count += 1
   }
   _sync_pc()
   if !_static_vbo_ptr || !_static_off_ptr { return false }
   def can_base_vertex = (soff % _VKR_VERT_STRIDE) == 0
   def vbo_bind_off = can_base_vertex ? 0 : soff
   def first_vertex = can_base_vertex ? int(soff / _VKR_VERT_STRIDE) : 0
   if _dynamic_vbo_bound || sbuf != _last_static_vbo || vbo_bind_off != _last_static_vbo_off {
      store64_h(_static_vbo_ptr, sbuf, 0)
      store64_h(_static_off_ptr, vbo_bind_off, 0)
      cmd_bind_vertex_buffers(cb, 0, 1, _static_vbo_ptr, _static_off_ptr)
      _last_static_vbo = sbuf
      _last_static_vbo_off = vbo_bind_off
      _dynamic_vbo_bound = false
   }
   if ibuf {
      def idx_size = (index_type == 1) ? 4 : 2
      def can_base_index = (ioff % idx_size) == 0
      def ibo_bind_off = can_base_index ? 0 : ioff
      def first_index = can_base_index ? int(ioff / idx_size) : 0
      if ibuf != _last_static_ibuf || ibo_bind_off != _last_static_ibuf_off || index_type != _last_static_ibuf_type {
         cmd_bind_index_buffer(cb, ibuf, ibo_bind_off, index_type)
         _last_static_ibuf = ibuf
         _last_static_ibuf_off = ibo_bind_off
         _last_static_ibuf_type = index_type
      }
      cmd_draw_indexed(cb, idx_count, 1, first_index, first_vertex, 0)
      _frame_indexed_draw_calls += 1
   } else {
      cmd_draw(cb, draw_count, 1, first_vertex, 0)
   }
   _total_draw_calls += 1
   _frame_draw_calls += 1
   _frame_static_draw_calls += 1
   true
}

fn _draw_parts_flat_range_impl(
   ?ptr slab_ptr,
   int start_idx,
   int end_idx,
   int pass_num,
   bool restore_model,
   bool sync_material_textures
) int {
   "Draws a contiguous range of flat packed scene parts with minimal interpreter overhead."
   if !_handle_ok(_current_frame_cb) || !_handle_ok(_pipeline_layout) || !_handle_ok(_current_model) { return 0 }
   if !slab_ptr || end_idx <= start_idx { return 0 }
   def saved_model = restore_model ? _snapshot_model_matrix(_scratch_model_saved_b) : 0
   def base_model = restore_model ? saved_model : _snapshot_model_matrix(_scratch_model_saved_a)
   mut drawn = 0
   _flat_range_reset_shading_cache()
   def flat_fast_on = _vk_flat_fast_on_cached()
   def base_model_identity = !base_model || _mat4_ptr_is_identity(base_model)
   def trace_part_models = _trace_light_detail
   mut last_model_key = 0
   mut traced_part = 0
   def trace_group_end = _trace_group ? (int(start_idx) + common.env_int_clamped("NY_UI_GROUP_TRACE_LIMIT", 6, 1, 4096)) : 0
   mut i = int(start_idx)
   while i < int(end_idx) {
      def base = slab_ptr + i * 256
      if !_flat_part_active(base) { i += 1 continue }
      if base_model_identity { last_model_key = _flat_range_sync_model_cache_raw(base, last_model_key) }
      else { last_model_key = _flat_range_sync_model_cache_composed(base, last_model_key, base_model) }
      if trace_part_models && traced_part < 6 {
         print("[vk:part] idx=" + to_str(i) + " model_t=(" +
            to_str(load32_f32(base + 64, 48)) + "," +
            to_str(load32_f32(base + 64, 52)) + "," +
            to_str(load32_f32(base + 64, 56)) + ")")
         traced_part += 1
      }
      _flat_range_sync_shading_cache(base, sync_material_textures)
      def sbuf = load64(base, 16)
      if !sbuf { i += 1 continue }
      def soff = load64_h(base, 24)
      def ibuf = load64(base, 32)
      def ioff = load64_h(base, 40)
      def idx_count = load32_h(base, 48)
      def draw_count = load32_h(base, 52)
      def index_type = load32_h(base, 56)
      def topo = load32_h(base, 60)
      def pipe = load64(base, 8)
      if _trace_group && i < trace_group_end {
         def mat_ptr = base + 136
         ui_profile.print_text("[group:draw] part=" + to_str(i) +
            " sbuf=" + to_str(sbuf) +
            " ibuf=" + to_str(ibuf) +
            " soff=" + to_str(soff) +
            " ioff=" + to_str(ioff) +
            " draw=" + to_str(draw_count) +
            " idx=" + to_str(idx_count) +
            " itype=" + to_str(index_type) +
            " topo=" + to_str(topo) +
            " tex=" + to_str(load32_h(base, 0)) +
            " matKey=" + to_str(load32_h(base, 128)) +
            " matTex=" + to_str(load32_h(mat_ptr, 20)) +
         " normal=0x" + text.to_hex(load32_h(mat_ptr, 100)))
      }
      drawn += _draw_flat_part_buffers_loaded(base, sbuf, soff, ibuf, ioff, idx_count, draw_count, index_type, topo, pipe, flat_fast_on)
      i += 1
   }
   if saved_model && _current_model {
      memcpy(_current_model, saved_model, 64)
      _model_dirty = true
      _pc_dirty = true
   } elif base_model {
      _restore_flat_base_model(base_model)
   }
   if _current_is_unlit { set_unlit(false) }
   drawn
}

fn begin_frame() bool {
   "Prepares the renderer for a new frame(sync, acquire image, begin recording)."
   _vk_stage("begin.enter")
   def _prof_on = _vk_profile_enabled()
   def _t_begin = _prof_on ? ticks() : 0
   def _profile_frame = _total_frames + 1
   def _deep_on = (_vk_deep_debug == 1) && ((_prof_on && ((_profile_frame % _vk_profile_every()) == 0)) || ((_profile_frame % _vk_deep_every()) == 0))
   def _t_begin_deep = _deep_on ? ticks() : 0
   _vk_stage("begin.validate")
   if !_handles_valid {
      if !_handle_ok(_device) { ui_profile.print_text("[begin_frame] FAIL: no device") return false }
      if !_handle_ok(_render_pass) { ui_profile.print_text("[begin_frame] FAIL: no render pass") return false }
      if !_handle_ok(_ubo_map) { ui_profile.print_text("[begin_frame] FAIL: no UBO map(uniform buffer not allocated or null)") return false }
      if !_handle_ok(_vertex_buffer) { ui_profile.print_text("[begin_frame] FAIL: no vertex buffer allocated") return false }
      if !_vk_frame_scratch_ready() { ui_profile.print_text("[begin_frame] FAIL: frame scratch buffers unavailable") return false }
      _handles_valid = true
   }
   def has_surface = _has_valid_surface_fast()
   def backend_is_wayland = has_surface && _backend_is_wayland_fast()
   def backend_is_win32 = has_surface && _backend_is_win32_fast()
   if _swapchain_image_count < 1 { ui_profile.print_text("[begin_frame] FAIL: invalid swapchain image count") return false }
   if _framebuffers_count < 1 { ui_profile.print_text("[begin_frame] FAIL: no framebuffers allocated") return false }
   if _command_buffers_count < 1 { ui_profile.print_text("[begin_frame] FAIL: no command buffers allocated") return false }
   if !_vk_frame_targets_ready(has_surface) { ui_profile.print_text("[begin_frame] FAIL: frame targets unavailable") return false }
   _vk_stage("begin.resize")
   if _swapchain_recreate_pending {
      mut cur_ww = _pending_resize_w > 0 ? _pending_resize_w : _swapchain_extent_w
      mut cur_wh = _pending_resize_h > 0 ? _pending_resize_h : _swapchain_extent_h
      if cur_ww <= 0 || cur_wh <= 0 { return _vk_begin_false("pending_invalid_size") }
      _pending_resize_w, _pending_resize_h = cur_ww, cur_wh
   }
   _vk_stage("begin.reset")
   _frame_open = false
   _active_scene_color_tex_id = -1
   _flush_total = 0
   _flush_reason_tex = 0
   _flush_reason_pipe = 0
   _flush_reason_static = 0
   _flush_reason_special = 0
   _flush_reason_vertex_full = 0
   _pipeline_bind_count = 0
   _descriptor_bind_count = 0
   _prim_rect_quads = 0
   _prim_outline_quads = 0
   _prim_line_quads = 0
   _prim_raw_lines = 0
   _prim_raw_points = 0
   _prim_text_calls = 0
   _prim_text_glyphs = 0
   _frame_draw_calls = 0
   _frame_dynamic_draw_calls = 0
   _frame_static_draw_calls = 0
   _frame_indexed_draw_calls = 0
   _frame_begin_cpu_us = 0
   _frame_end_cpu_us = 0
   _frame_flush_cpu_us = 0
   _frame_sync_pc_cpu_us = 0
   _vk_deep_emit_frame = 0
   if _deep_on || _prof_on {
      _vk_deep_begin_recreate_ms = 0.0
      _vk_deep_begin_acquire_ms = 0.0
      _vk_deep_begin_wait_ms = 0.0
      _vk_deep_begin_reset_fence_ms = 0.0
      _vk_deep_begin_cmd_ms = 0.0
      _vk_deep_begin_rp_ms = 0.0
      _vk_deep_begin_vp_ms = 0.0
      _vk_deep_begin_total_ms = 0.0
      _vk_deep_end_flush_ms = 0.0
      _vk_deep_end_cmd_ms = 0.0
      _vk_deep_end_submit_ms = 0.0
      _vk_deep_end_present_ms = 0.0
      _vk_deep_end_total_ms = 0.0
   }
   _vk_stage("begin.maybe_recreate")
   if _deep_on && _vk_deep_should_emit(_profile_frame) && _deep_last_report_frame != _profile_frame {
      _deep_last_report_frame = _profile_frame
      print(
         "[vk] frame=" + to_str(_profile_frame)
         + " " + to_str(_swapchain_extent_w) + "x" + to_str(_swapchain_extent_h)
         + " img=" + to_str(_current_frame) + "/" + to_str(_swapchain_image_count)
      )
   }
   if _swapchain_recreate_pending {
      _vk_stage("begin.recreate")
      def _t_recreate = _deep_on ? ticks() : 0
      def force_recreate = _swapchain_recreate_force
      if _resize_debounce_waiting(force_recreate) {
         _vk_stage("begin.recreate.defer")
      } else {
         _swapchain_recreate_pending = false
         _swapchain_recreate_force = false
         if !force_recreate && _pending_resize_w > 0 && _pending_resize_h > 0 &&
         _same_int_value(_pending_resize_w, _swapchain_extent_w) &&
         _same_int_value(_pending_resize_h, _swapchain_extent_h){
            _pending_resize_w, _pending_resize_h = 0, 0
            _pending_resize_stamp_ns = 0
         } else {
            _vk_stage("begin.recreate.call")
            if !_recreate_swapchain() { return _vk_begin_false("recreate_failed") }
            _pending_resize_w, _pending_resize_h = 0, 0
            _pending_resize_stamp_ns = 0
         }
      }
      if has_surface && !_handle_ok(_swapchain) {
         ui_profile.print_text("[begin_frame] FAIL: swapchain unavailable after recreate")
         return false
      }
      if _framebuffers_count < 1 {
         ui_profile.print_text("[begin_frame] FAIL: no framebuffers after recreate")
         return false
      }
      if _command_buffers_count < 1 {
         ui_profile.print_text("[begin_frame] FAIL: no command buffers after recreate")
         return false
      }
      if !_vk_frame_scratch_ready() { ui_profile.print_text("[begin_frame] FAIL: frame scratch buffers unavailable after recreate") return _vk_begin_false("scratch_after_recreate") }
      if !_vk_frame_targets_ready(has_surface) { ui_profile.print_text("[begin_frame] FAIL: frame targets unavailable after recreate") return _vk_begin_false("targets_after_recreate") }
      if _deep_on { _vk_deep_begin_recreate_ms = ui_profile.elapsed_ms(_t_recreate) }
      ;; Continue into the same frame after a successful recreate. Returning
      ;; false here made the caller skip drawing for one refresh, which looked
      ;; like black bars or flicker during resize/load churn.
   }
   _vk_stage("begin.fence")
   _vk_stage("begin.fence.slab")
   if !_vk_handle_slab_ready(_fences_slab, _in_flight_fences_count, _current_frame) { return _vk_begin_false("fence_slab_not_ready") }
   _vk_stage("begin.fence.load")
   def fence = load64_h(_fences_slab, _current_frame * 8)
   if !_handle_ok(fence) { ui_profile.print_text("[begin_frame] FAIL: null fence at frame=" + to_str(_current_frame)) return false }
   def _t_wait = _deep_on ? ticks() : 0
   mut fence_wait_timeout = -1
   if has_surface && (backend_is_wayland || backend_is_win32) {
      fence_wait_timeout = _surface_wait_timeout_ns()
   }
   _vk_stage("begin.fence.wait")
   def wf = _wait_for_fence_with_host_pump(fence, fence_wait_timeout)
   _vk_stage("begin.fence.waited")
   if _deep_on { _vk_deep_begin_wait_ms = ui_profile.elapsed_ms(_t_wait) }
   if _same_int_value(wf, VK_NOT_READY) || _same_int_value(wf, VK_TIMEOUT) { return _vk_begin_false("fence_wait_timeout") }
   if wf { return _vk_begin_false("fence_wait_error_" + to_str(wf)) }
   _vk_stage("begin.fence.flush_delete")
   _flush_frame_deletion_queue(_current_frame)
   mut acq = 0
   _vk_stage("begin.frame.index_check")
   if _current_frame < 0 || _current_frame >= _frames_in_flight() {
      ui_profile.print_text("[begin_frame] FAIL: invalid frame index=" + to_str(_current_frame))
      return false
   }
   if has_surface {
      _vk_stage("begin.acquire.surface")
      if !_vk_handle_slab_ready(_sem_avail_slab, _image_available_semaphores_count, _current_frame) { return _vk_begin_false("sem_avail_not_ready") }
      def sem = load64_h(_sem_avail_slab, _current_frame * 8)
      def _t_acquire = _deep_on ? ticks() : 0
      mut acquire_timeout = -1
      if backend_is_wayland || backend_is_win32 {
         acquire_timeout = _surface_wait_timeout_ns()
      }
      if _vk_debug_basic == 1 {
         def backend_name = lib_uiw.backend()
         ui_profile.print_text("[gfx:vulkan] begin_frame before acquire backend=" + backend_name +
            " extent=" + to_str(_swapchain_extent_w) + "x" + to_str(_swapchain_extent_h) +
         " timeout_ns=" + to_str(acquire_timeout))
      }
      if backend_is_win32 { _pump_host_messages_if_needed() }
      acq = acquire_next_image_khr(_device, _swapchain, acquire_timeout, sem, 0, _ptr_img_idx)
      if backend_is_win32 { _pump_host_messages_if_needed() }
      if _vk_debug_basic == 1 { ui_profile.print_text("[gfx:vulkan] begin_frame after acquire result=" + to_str(acq)) }
      if _same_int_value(acq, VK_NOT_READY) || _same_int_value(acq, VK_TIMEOUT) {
         if _vk_debug_basic == 1 && !_logged_waiting_acquire {
            _logged_waiting_acquire = true
            def backend_name = lib_uiw.backend()
            ui_profile.print_text("[gfx:vulkan] acquire waiting backend=" + backend_name +
               " extent=" + to_str(_swapchain_extent_w) + "x" + to_str(_swapchain_extent_h) +
            " flags=0x" + text.to_hex(_current_window_flags))
         }
         return _vk_begin_false("acquire_wait")
      }
      _logged_waiting_acquire = false
      if _same_int_value(acq, -1000001004) {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] acquire out_of_date") }
         _schedule_swapchain_recreate(true)
         return _vk_begin_false("acquire_out_of_date")
      }
      if _same_int_value(acq, 1000001003) {
         if vk_state._debug_gfx_enabled && !_logged_suboptimal_acquire {
            ui_profile.print_text("[gfx:vulkan] acquire suboptimal continuing=true")
            _logged_suboptimal_acquire = true
         }
         _suboptimal_recreate_attempted = false
      }
      if !_same_int_value(acq, 0) && !_same_int_value(acq, 1000001003) { return _vk_begin_false("acquire_error_" + to_str(acq)) }
      if _deep_on { _vk_deep_begin_acquire_ms = ui_profile.elapsed_ms(_t_acquire) }
      _image_index = load32_h(_ptr_img_idx, 0)
   } else {
      _vk_stage("begin.acquire.headless")
      if _swapchain_image_count < 1 {
         ui_profile.print_text("[begin_frame] FAIL: invalid headless image count=" + to_str(_swapchain_image_count))
         return _vk_begin_false("headless_image_count")
      }
      _image_index = (_image_index + 1) % _swapchain_image_count
   }
   _vk_stage("begin.acquire.done")
   _vk_stage("begin.reset_fence")
   def _t_reset_fence = _deep_on ? ticks() : 0
   reset_fences(_device, 1, _ptr_fence)
   if _deep_on { _vk_deep_begin_reset_fence_ms = ui_profile.elapsed_ms(_t_reset_fence) }
   _vk_stage("begin.image.validate")
   if _image_index >= _framebuffers_count {
      ui_profile.print_text("[begin_frame] FAIL: invalid image_index=" + to_str(_image_index))
      return false
   }
   if !_vk_handle_slab_ready(_framebuffers_slab, _framebuffers_count, _image_index) { return _vk_begin_false("framebuffer_slab_not_ready") }
   _vk_stage("begin.image.framebuffer")
   def fb = load64_h(_framebuffers_slab, _image_index * 8)
   if !_handle_ok(fb) { ui_profile.print_text("[begin_frame] FAIL: null or zero framebuffer at index=" + to_str(_image_index)) return false }
   def use_load_color_pass = _next_begin_load_color && _handle_ok(_render_pass_load_color_clear_depth)
   _next_begin_load_color = false
   if !_handles_valid && !_vk_frame_scratch_ready() { ui_profile.print_text("[begin_frame] FAIL: frame scratch buffers unavailable before command recording") return false }
   _vk_stage("begin.command.slab")
   if !_vk_handle_slab_ready(_cmd_bufs_slab, _command_buffers_count, _current_frame) { return _vk_begin_false("cmd_slab_not_ready") }
   def cb = load64(_cmd_bufs_slab, _current_frame * 8)
   if !_handle_ok(cb) { ui_profile.print_text("[begin_frame] FAIL: null or zero command buffer") return false }
   def _t_begin_cmd = _deep_on ? ticks() : 0
   _vk_stage("begin.command.reset")
   def rcb = reset_command_buffer(cb, 0)
   if rcb != 0 { return _vk_begin_false("reset_command_buffer_failed_" + to_str(rcb)) }
   _vk_stage("begin.command.begin")
   if begin_command_buffer(cb, _ptr_bi) { return _vk_begin_false("begin_command_buffer_failed") }
   _vk_stage("begin.command.began")
   if _deep_on { _vk_deep_begin_cmd_ms = ui_profile.elapsed_ms(_t_begin_cmd) }
   if _vk_markers_enabled { vk_debug_marker_begin(cb, "Frame " + to_str(_total_frames), 0xFFFFFFFF) }
   _vk_stage("begin.renderpass.setup")
   mut clear_count = 0
   mut clear_values = 0
   if use_load_color_pass {
      store32_f32(_ptr_clear, 1.0, 16)
      store32(_ptr_clear, 0, 20)
      clear_count = 2
      clear_values = _ptr_clear
   } else {
      def cr = _clear_color_channel(_clear_r)
      def cg = _clear_color_channel(_clear_g)
      def cbg = _clear_color_channel(_clear_b)
      store32_f32(_ptr_clear, cr, 0)
      store32_f32(_ptr_clear, cg, 4)
      store32_f32(_ptr_clear, cbg, 8)
      store32_f32(_ptr_clear, _clear_a, 12)
      store32_f32(_ptr_clear, 1.0, 16)
      store32(_ptr_clear, 0, 20)
      if _cfg_msaa > 1 {
         store32_f32(_ptr_clear, cr, 32)
         store32_f32(_ptr_clear, cg, 36)
         store32_f32(_ptr_clear, cbg, 40)
         store32_f32(_ptr_clear, _clear_a, 44)
      }
      clear_count = (_cfg_msaa > 1) ? 3 : 2
      clear_values = _ptr_clear
   }
   if !_handle_ok(fb) { ui_profile.print_text("[begin_frame] FAIL: null or zero framebuffer at index=" + to_str(_image_index)) return false }
   store32(_ptr_ri, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0)
   store64_h(_ptr_ri, 0, 8)
   store64_h(_ptr_ri, use_load_color_pass ? _render_pass_load_color_clear_depth : _render_pass, 16)
   store64_h(_ptr_ri, fb, 24)
   store32(_ptr_ri, 0, 32)
   store32(_ptr_ri, 0, 36)
   store32(_ptr_ri, _swapchain_extent_w, 40)
   store32(_ptr_ri, _swapchain_extent_h, 44)
   store32(_ptr_ri, clear_count, 48)
   store64_h(_ptr_ri, clear_values, 56)
   def _t_begin_rp = _deep_on ? ticks() : 0
   if _vk_markers_enabled { vk_debug_marker_begin(cb, "RenderPass", 0x3366FFFF) }
   _vk_stage("begin.renderpass.begin")
   cmd_begin_render_pass(cb, _ptr_ri, 0)
   _vk_stage("begin.renderpass.began")
   if _deep_on { _vk_deep_begin_rp_ms = ui_profile.elapsed_ms(_t_begin_rp) }
   _vk_stage("begin.viewport")
   store32_f32(_ptr_vp, 0.0, 0)
   store32_f32(_ptr_vp, float(_swapchain_extent_h), 4)
   store32_f32(_ptr_vp, float(_swapchain_extent_w), 8)
   store32_f32(_ptr_vp, -float(_swapchain_extent_h), 12)
   store32_f32(_ptr_vp, 0.0, 16)
   store32_f32(_ptr_vp, 1.0, 20)
   cmd_set_viewport(cb, 0, 1, _ptr_vp)
   def _t_vp = _deep_on ? ticks() : 0
   _vk_stage("begin.scissor")
   store32(_ptr_sci, 0, 0)
   store32(_ptr_sci, 0, 4)
   store32(_ptr_sci, _swapchain_extent_w, 8)
   store32(_ptr_sci, _swapchain_extent_h, 12)
   cmd_set_scissor(cb, 0, 1, _ptr_sci)
   _vk_stage("begin.dynamic.done")
   if _deep_on { _vk_deep_begin_vp_ms = ui_profile.elapsed_ms(_t_vp) }
   _frame_open = true
   _current_frame_cb = cb
   _current_frame_ubo_ds = load64_h(_ubo_ds_slab, _current_frame * 8)
   _total_frames += 1
   _fps_count += 1
   if (_total_frames % 60) == 0 {
      _fps_curr = _fps_count
      _fps_count = 0
   }
   _vk_stage("begin.mvp")
   if !_skip_default_mvp_this_frame() { _update_default_mvp(_window_ref) }
   _vk_stage("begin.mvp.done")
   _vk_stage("begin.vertex.reset")
   _vertex_offset = 0
   _last_flush_offset = 0
   _vertex_limit_hit = false
   _logged_vertex_limit_hit = false
   _current_frame_vertex_offset = _current_frame * _vertex_capacity
   if _vertex_map { _local_vertex_map = _vertex_map + _current_frame_vertex_offset }
   else { _local_vertex_map = 0 }
   _vk_stage("begin.state.reset")
   _last_bound_ds = 0
   _last_bound_tex_id = -1
   _last_bound_pipe = 0
   _last_bound_ubo_ds = 0
   _last_static_vbo = 0
   _last_static_vbo_off = 0
   _last_static_ibuf = 0
   _last_static_ibuf_off = -1
   _last_static_ibuf_type = -1
   _last_line_width = -1.0
   _target_pipeline = _pipeline
   _use_custom_pc = 0
   _current_texture_id = -1
   _current_tex_index = 0
   _batch_texture_id = -1
   _batch_tex_index = 0
   _mvp_dirty = true
   _pc_dirty = true
   _last_is_mask = 0
   _vk_stage("begin.pipeline.validate")
   mut frame_pipe = _pipeline
   if !_handle_ok(frame_pipe) {
      frame_pipe = _get_nocull_pipeline()
      if !_handle_ok(frame_pipe) { frame_pipe = _get_unlit_nocull_pipeline() }
   }
   if !_handle_ok(frame_pipe) { ui_profile.print_text("[begin_frame] FAIL: no graphics pipeline") return false }
   if !_handle_ok(_vertex_buffer) { ui_profile.print_text("[begin_frame] FAIL: vertex buffer is null or zero") return false }
   _vk_stage("begin.flush.ptrs")
   store64_h(_flush_off, _current_frame_vertex_offset, 0)
   store64_h(_flush_buf, _vertex_buffer, 0)
   _dynamic_vbo_bound = false
   _vk_stage("begin.pc.init")
   memcpy(_pc_buffer, _current_mvp, 64)
   _store_identity_mat4(_current_model)
   memcpy(_pc_buffer + 64, _current_model, 64)
   store32(_pc_buffer, 0, 128)
   store32(_pc_buffer, _current_is_unlit, 132)
   store32(_pc_buffer, _swapchain_extent_w, 136)
   store32(_pc_buffer, _swapchain_extent_h, 140)
   _mvp_dirty = false
   _model_dirty = false
   _pc_dirty = true
   _last_is_mask = 0
   _last_is_unlit = _current_is_unlit
   if _prof_on { _frame_begin_cpu_us = float(ticks() - _t_begin) / 1000.0 }
   if _deep_on { _vk_deep_begin_total_ms = ui_profile.elapsed_ms(_t_begin_deep) }
   _vk_stage("begin.done")
   true
}

fn set_unlit(bool unlit) any {
   "Marks lighting mode for subsequent draws without forcing an immediate flush."
   def val = unlit ? 1 : 0
   if val != _current_is_unlit {
      if _vertex_offset != _last_flush_offset { _flush() }
      _current_is_unlit = val
      _pc_dirty = true
   }
   0
}

fn set_double_sided(bool enabled) any {
   "Marks subsequent lit draws as true double-sided surfaces so only those flip normals on backfaces."
   def val = enabled ? 1 : 0
   if val != _current_double_sided_lighting {
      if _vertex_offset != _last_flush_offset { _flush() }
      _current_double_sided_lighting = val
      def m = int(_current_metallic * 255.0) & 255
      def ro = int(_current_roughness * 255.0) & 255
      _current_metallic_roughness_u32 = m | (ro << 8)
      _pc_dirty = true
   }
   0
}

fn set_vertex_color_mode(int vc_mode) any {
   "Marks how subsequent draws use vertex color: 0=off, 1=primary, 4=multiply."
   def val = vc_mode
   if val != _current_vc_mode {
      if _vertex_offset != _last_flush_offset {
         _flush_reason = 2
         _flush()
      }
      _current_vc_mode = val
      _current_material_key = 0
      _current_base_tex_word = _pack_base_tex_word(_current_base_tex_id, _current_vc_mode)
      _pc_dirty = true
   }
   0
}

fn set_material(any base_color, any metallic, any roughness) any {
   "Sets PBR material parameters for subsequent draws."
   _current_base_color = base_color
   _current_metallic = metallic
   _current_roughness = roughness
   def r, g = int(base_color.get(0, 1.0) * 255.0) & 255, int(base_color.get(1, 1.0) * 255.0) & 255
   def b, a = int(base_color.get(2, 1.0) * 255.0) & 255, int(base_color.get(3, 1.0) * 255.0) & 255
   _current_base_color_u32 = r | (g << 8) | (b << 16) | (a << 24)
   def m = int(metallic * 255.0) & 255
   def ro = int(roughness * 255.0) & 255
   _current_metallic_roughness_u32 = m | (ro << 8)
   _current_emissive_u32 = 0
   _current_emissive_tex_id = -1
   _current_base_tex_id = -1
   _current_alpha_u32 = 0
   _current_occlusion_tex_id = -1
   _current_occlusion_uv_set = 0
   _current_base_uv_xf0 = 0
   _current_base_uv_xf1 = 0
   _current_normal_uv_xf0 = 0
   _current_normal_uv_xf1 = 0
   _current_mr_uv_xf0 = 0
   _current_mr_uv_xf1 = 0
   _current_occlusion_uv_xf0 = 0
   _current_occlusion_uv_xf1 = 0
   _current_emissive_uv_xf0 = 0
   _current_emissive_uv_xf1 = 0
   _current_bsdf0_u32 = 0
   _current_bsdf1_u32 = 0
   _current_bsdf2_u32 = 0
   _current_bsdf3_u32 = 0
   _current_bsdf4_u32 = 0
   _current_bsdf5_u32 = 0
   _current_ext2_tex_word = 0x80000000
   _current_vc_mode = 0
   _current_emissive_tex_word = 0x80000000
   _current_base_tex_word = 0x80000000
   _current_occlusion_tex_word = 0x80000000
   _current_normal_tex_word = 0x80000000
   _current_material_key = 0
   _pc_dirty = true
   0
}

fn _norm_i32(int v) int {
   if v > 2147483647 { return v - 4294967296 }
   return v
}

fn _norm_normal_tex_word(int v) int {
   def tid = band(v, 0xffff)
   if tid >= MAX_TEXTURES { return bor(band(v, 0xfffe0000), 0xffff) }
   v
}

fn _pack_emissive_tex_word(int tex_id, int uv_set) int {
   if tex_id < 0 { return 0x80000000 }
   mut word = band(tex_id, 0xffff)
   if band(uv_set, 1) != 0 { word = word | 0x10000 }
   if band(uv_set, 2) != 0 { word = word | 0x20000 }
   if band(uv_set, 4) != 0 { word = word | 0x40000 }
   word
}

@inline
fn _pack_base_tex_word(int tex_id, int vc_mode) int {
   mut flags = 0
   if band(vc_mode, 1) != 0 { flags = flags | 0x40000000 }
   if band(vc_mode, 4) != 0 { flags = flags | 0x10000000 }
   if band(vc_mode, 2) != 0 { flags = flags | 0x20000000 }
   if band(vc_mode, 8) != 0 { flags = flags | 0x04000000 }
   if band(vc_mode, 16) != 0 { flags = flags | 0x08000000 }
   if tex_id >= 0 && !_format_is_srgb(texture_format(tex_id)) { flags = flags | 0x02000000 }
   (tex_id >= 0 ? band(tex_id, 0xffff) : 0x80000000) | flags
}

fn _pack_occlusion_tex_word(int tex_id, int uv_set) int {
   if tex_id < 0 { return 0x80000000 }
   mut word = band(tex_id, 0xffff)
   if uv_set == 1 { word = word | 0x10000 }
   word
}

fn _pack_normal_tex_word_current(int normal_tex_id, int normal_uv_xf1) int {
   def normal_tid = band(normal_tex_id, 0xffff)
   if normal_tid < MAX_TEXTURES {
      mut word = bor(band(normal_tex_id, 0xfffe0000), normal_tid)
      if band(normal_uv_xf1, 0x40000000) != 0 { word = word | 0x10000 }
      return word
   }
   bor(band(normal_tex_id, 0xfffe0000), 0xffff)
}

@inline
fn _material_state_no_sync_cached() bool {
   if _material_state_no_sync < 0 { _material_state_no_sync = common.env_truthy("NY_VK_MATERIAL_STATE_NO_SYNC") ? 1 : 0 }
   _material_state_no_sync == 1
}

@inline
fn _sync_current_material_texture_slots_impl(bool honor_no_sync_env) any {
   "Ensures every texture referenced by the current material has a valid bindless slot."
   if honor_no_sync_env && _material_state_no_sync_cached() { return 0 }
   if _current_base_tex_id >= 0 { bindless_sync_texture_slot(_current_base_tex_id) }
   def mr_word = band(bshr(_current_metallic_roughness_u32, 16), 0x7fff)
   def mr_tid = mr_word > 0 ? mr_word - 1 : -1
   if mr_tid >= 0 { bindless_sync_texture_slot(mr_tid) }
   def normal_tid = band(_current_normal_tex_id, 0xffff)
   if normal_tid < MAX_TEXTURES { bindless_sync_texture_slot(normal_tid) }
   if _current_emissive_tex_id >= 0 { bindless_sync_texture_slot(_current_emissive_tex_id) }
   if _current_occlusion_tex_id >= 0 { bindless_sync_texture_slot(_current_occlusion_tex_id) }
   if (_current_ext2_tex_word & 0x80000000) == 0 {
      def ext2_tid = band(_current_ext2_tex_word, 0xffff)
      if ext2_tid < MAX_TEXTURES { bindless_sync_texture_slot(ext2_tid) }
   }
   0
}

fn _sync_current_material_texture_slots() any { _sync_current_material_texture_slots_impl(true) }

fn set_ui_material(int base_tex_id = -1, int alpha_u32 = 0, int vc_mode = 12) any {
   "Sets the compact material state used by 2D UI, text, and terminal draws."
   def next_base_tex_id = _norm_i32(base_tex_id)
   def next_base_tex_word = _pack_base_tex_word(next_base_tex_id, vc_mode)
   if _current_base_color_u32 == 0xffffffff &&
   _current_metallic_roughness_u32 == 0 &&
   _current_emissive_u32 == 0 &&
   _current_emissive_tex_id == -1 &&
   _current_emissive_uv_set == 0 &&
   _current_base_tex_id == next_base_tex_id &&
   _current_normal_tex_id == -1 &&
   _current_alpha_u32 == alpha_u32 &&
   _current_occlusion_tex_id == -1 &&
   _current_occlusion_uv_set == 0 &&
   _current_bsdf0_u32 == 0 &&
   _current_bsdf1_u32 == 0 &&
   _current_bsdf2_u32 == 0 &&
   _current_bsdf3_u32 == 0 &&
   _current_bsdf4_u32 == 0 &&
   _current_bsdf5_u32 == 0 &&
   _current_base_uv_xf0 == 0 &&
   _current_base_uv_xf1 == 0 &&
   _current_normal_uv_xf0 == 0 &&
   _current_normal_uv_xf1 == 0 &&
   _current_mr_uv_xf0 == 0 &&
   _current_mr_uv_xf1 == 0 &&
   _current_occlusion_uv_xf0 == 0 &&
   _current_occlusion_uv_xf1 == 0 &&
   _current_emissive_uv_xf0 == 0 &&
   _current_emissive_uv_xf1 == 0 &&
   _current_ext2_tex_word == 0x80000000 &&
   _current_vc_mode == vc_mode{
      return 0
   }
   _current_material_key = 0
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 2
      _flush()
   }
   _current_base_color_u32 = 0xffffffff
   _current_metallic_roughness_u32 = 0
   _current_emissive_u32 = 0
   _current_emissive_tex_id = -1
   _current_emissive_uv_set = 0
   _current_base_tex_id = next_base_tex_id
   _current_normal_tex_id = -1
   _current_alpha_u32 = alpha_u32
   _current_occlusion_tex_id = -1
   _current_occlusion_uv_set = 0
   _current_bsdf0_u32 = 0
   _current_bsdf1_u32 = 0
   _current_bsdf2_u32 = 0
   _current_bsdf3_u32 = 0
   _current_bsdf4_u32 = 0
   _current_bsdf5_u32 = 0
   _current_base_uv_xf0 = 0
   _current_base_uv_xf1 = 0
   _current_normal_uv_xf0 = 0
   _current_normal_uv_xf1 = 0
   _current_mr_uv_xf0 = 0
   _current_mr_uv_xf1 = 0
   _current_occlusion_uv_xf0 = 0
   _current_occlusion_uv_xf1 = 0
   _current_emissive_uv_xf0 = 0
   _current_emissive_uv_xf1 = 0
   _current_ext2_tex_word = 0x80000000
   _current_vc_mode = vc_mode
   _current_emissive_tex_word = 0x80000000
   _current_base_tex_word = next_base_tex_word
   _current_occlusion_tex_word = 0x80000000
   _current_normal_tex_word = -1
   if next_base_tex_id >= 0 { bind_texture(next_base_tex_id) }
   _pc_dirty = true
   0
}

fn set_material_packed(
   int base_color_u32, int material_u32, int emissive_u32 = 0, int emissive_tex_id = -1,
   int emissive_uv_set = 0, int base_tex_id = -1, int alpha_u32 = 0, int occlusion_tex_id = -1,
   int occlusion_uv_set = 0, int bsdf0_u32 = 0, int bsdf1_u32 = 0, int bsdf2_u32 = 0,
   int bsdf3_u32 = 0, int bsdf4_u32 = 0, int bsdf5_u32 = 0, int base_uv_xf0 = 0,
   int base_uv_xf1 = 0, int normal_uv_xf0 = 0, int normal_uv_xf1 = 0, int mr_uv_xf0 = 0,
   int mr_uv_xf1 = 0, int occlusion_uv_xf0 = 0, int occlusion_uv_xf1 = 0,
   int emissive_uv_xf0 = 0, int emissive_uv_xf1 = 0, int normal_tex_id = -1,
   int ext2_tex_word = 0x80000000, int vc_mode = 0
) any {
   "Sets packed base-color/material words directly for subsequent draws."
   def next_base_color_u32 = base_color_u32
   def next_material_u32 = material_u32
   def next_emissive_u32 = emissive_u32
   def next_emissive_tex_id = _norm_i32(emissive_tex_id)
   def next_emissive_uv_set = emissive_uv_set
   def next_base_tex_id = _norm_i32(base_tex_id)
   def next_normal_tex_id = _norm_normal_tex_word(normal_tex_id)
   def next_alpha_u32 = alpha_u32
   def next_occlusion_tex_id = _norm_i32(occlusion_tex_id)
   def next_occlusion_uv_set = occlusion_uv_set
   def next_bsdf0_u32 = bsdf0_u32
   def next_bsdf1_u32 = bsdf1_u32
   def next_bsdf2_u32 = bsdf2_u32
   def next_bsdf3_u32 = bsdf3_u32
   def next_bsdf4_u32 = bsdf4_u32
   def next_bsdf5_u32 = bsdf5_u32
   def next_base_uv_xf0 = base_uv_xf0
   def next_base_uv_xf1 = base_uv_xf1
   def next_normal_uv_xf0 = normal_uv_xf0
   def next_normal_uv_xf1 = normal_uv_xf1
   def next_mr_uv_xf0 = mr_uv_xf0
   def next_mr_uv_xf1 = mr_uv_xf1
   def next_occlusion_uv_xf0 = occlusion_uv_xf0
   def next_occlusion_uv_xf1 = occlusion_uv_xf1
   def next_emissive_uv_xf0 = emissive_uv_xf0
   def next_emissive_uv_xf1 = emissive_uv_xf1
   def next_ext2_tex_word = ext2_tex_word
   def next_vc_mode = vc_mode
   def next_emissive_tex_word = _pack_emissive_tex_word(next_emissive_tex_id, next_emissive_uv_set)
   def next_base_tex_word = _pack_base_tex_word(next_base_tex_id, next_vc_mode)
   def next_occlusion_tex_word = _pack_occlusion_tex_word(next_occlusion_tex_id, next_occlusion_uv_set)
   def next_normal_tex_word = _pack_normal_tex_word_current(next_normal_tex_id, next_normal_uv_xf1)
   if _current_base_color_u32 == next_base_color_u32 &&
   _current_metallic_roughness_u32 == next_material_u32 &&
   _current_emissive_u32 == next_emissive_u32 &&
   _current_emissive_tex_id == next_emissive_tex_id &&
   _current_emissive_uv_set == next_emissive_uv_set &&
   _current_base_tex_id == next_base_tex_id &&
   _current_normal_tex_id == next_normal_tex_id &&
   _current_alpha_u32 == next_alpha_u32 &&
   _current_occlusion_tex_id == next_occlusion_tex_id &&
   _current_occlusion_uv_set == next_occlusion_uv_set &&
   _current_bsdf0_u32 == next_bsdf0_u32 &&
   _current_bsdf1_u32 == next_bsdf1_u32 &&
   _current_bsdf2_u32 == next_bsdf2_u32 &&
   _current_bsdf3_u32 == next_bsdf3_u32 &&
   _current_bsdf4_u32 == next_bsdf4_u32 &&
   _current_bsdf5_u32 == next_bsdf5_u32 &&
   _current_base_uv_xf0 == next_base_uv_xf0 &&
   _current_base_uv_xf1 == next_base_uv_xf1 &&
   _current_normal_uv_xf0 == next_normal_uv_xf0 &&
   _current_normal_uv_xf1 == next_normal_uv_xf1 &&
   _current_mr_uv_xf0 == next_mr_uv_xf0 &&
   _current_mr_uv_xf1 == next_mr_uv_xf1 &&
   _current_occlusion_uv_xf0 == next_occlusion_uv_xf0 &&
   _current_occlusion_uv_xf1 == next_occlusion_uv_xf1 &&
   _current_emissive_uv_xf0 == next_emissive_uv_xf0 &&
   _current_emissive_uv_xf1 == next_emissive_uv_xf1 &&
   _current_ext2_tex_word == next_ext2_tex_word &&
   _current_vc_mode == next_vc_mode{
      return 0
   }
   _current_material_key = 0
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 2
      _flush()
   }
   _current_base_color_u32 = next_base_color_u32
   _current_metallic_roughness_u32 = next_material_u32
   _current_emissive_u32 = next_emissive_u32
   _current_emissive_tex_id = next_emissive_tex_id
   _current_emissive_uv_set = next_emissive_uv_set
   _current_base_tex_id = next_base_tex_id
   _current_normal_tex_id = next_normal_tex_id
   _current_alpha_u32 = next_alpha_u32
   _current_occlusion_tex_id = next_occlusion_tex_id
   _current_occlusion_uv_set = next_occlusion_uv_set
   _current_bsdf0_u32 = next_bsdf0_u32
   _current_bsdf1_u32 = next_bsdf1_u32
   _current_bsdf2_u32 = next_bsdf2_u32
   _current_bsdf3_u32 = next_bsdf3_u32
   _current_bsdf4_u32 = next_bsdf4_u32
   _current_bsdf5_u32 = next_bsdf5_u32
   _current_base_uv_xf0 = next_base_uv_xf0
   _current_base_uv_xf1 = next_base_uv_xf1
   _current_normal_uv_xf0 = next_normal_uv_xf0
   _current_normal_uv_xf1 = next_normal_uv_xf1
   _current_mr_uv_xf0 = next_mr_uv_xf0
   _current_mr_uv_xf1 = next_mr_uv_xf1
   _current_occlusion_uv_xf0 = next_occlusion_uv_xf0
   _current_occlusion_uv_xf1 = next_occlusion_uv_xf1
   _current_emissive_uv_xf0 = next_emissive_uv_xf0
   _current_emissive_uv_xf1 = next_emissive_uv_xf1
   _current_ext2_tex_word = next_ext2_tex_word
   _current_vc_mode = next_vc_mode
   _current_emissive_tex_word = next_emissive_tex_word
   _current_base_tex_word = next_base_tex_word
   _current_occlusion_tex_word = next_occlusion_tex_word
   _current_normal_tex_word = next_normal_tex_word
   if _current_base_tex_id >= 0 { bind_texture(_current_base_tex_id) }
   def mr_word = band(bshr(_current_metallic_roughness_u32, 16), 0x7fff)
   def mr_tid = mr_word > 0 ? mr_word - 1 : -1
   def normal_tid = band(_current_normal_tex_id, 0xffff)
   if mr_tid >= 0 { bindless_sync_texture_slot(mr_tid) }
   if normal_tid < MAX_TEXTURES { bindless_sync_texture_slot(normal_tid) }
   if _current_emissive_tex_id >= 0 { bindless_sync_texture_slot(_current_emissive_tex_id) }
   if _current_occlusion_tex_id >= 0 { bindless_sync_texture_slot(_current_occlusion_tex_id) }
   if (_current_ext2_tex_word & 0x80000000) == 0 {
      def ext2_tid = band(_current_ext2_tex_word, 0xffff)
      if ext2_tid < MAX_TEXTURES { bindless_sync_texture_slot(ext2_tid) }
   }
   if _trace_mat {
      def _base_tex_path = _texture_meta(_current_base_tex_id, "path", "")
      def _emit_tex_path = _texture_meta(_current_emissive_tex_id, "path", "")
      ui_profile.print_text("[vk:mat] base=0x" + text.to_hex(_current_base_color_u32) +
         " mat=0x" + text.to_hex(_current_metallic_roughness_u32) +
         " baseTex=" + to_str(_current_base_tex_id) +
         " basePath=" + to_str(_base_tex_path) +
         " emitTex=" + to_str(_current_emissive_tex_id) +
         " emitPath=" + to_str(_emit_tex_path) +
         " emit=0x" + text.to_hex(_current_emissive_u32) +
         " alpha=0x" + text.to_hex(_current_alpha_u32) +
         " occTex=" + to_str(_current_occlusion_tex_id) +
         " nrmTex=" + to_str(_current_normal_tex_id) +
         " ext2=0x" + text.to_hex(_current_ext2_tex_word) +
         " baseUvXf=0x" + text.to_hex(_current_base_uv_xf0) + "/0x" + text.to_hex(_current_base_uv_xf1) +
         " vc=" + to_str(_current_vc_mode) +
         " bsdf0=0x" + text.to_hex(_current_bsdf0_u32) +
         " bsdf1=0x" + text.to_hex(_current_bsdf1_u32) +
         " bsdf2=0x" + text.to_hex(_current_bsdf2_u32) +
         " bsdf3=0x" + text.to_hex(_current_bsdf3_u32) +
         " bsdf4=0x" + text.to_hex(_current_bsdf4_u32) +
      " bsdf5=0x" + text.to_hex(_current_bsdf5_u32))
   }
   _pc_dirty = true
   0
}

@inline
fn _set_material_from_slab_offsets(?ptr p, int bsdf4_off, int bsdf5_off, int ext2_off, int vc_mode) any {
   if !p { return 0 }
   set_material_packed(
      load32_h(p, 0),
      load32_h(p, 4),
      load32_h(p, 8),
      load32_h(p, 12),
      load32_h(p, 16),
      load32_h(p, 20),
      load32_h(p, 24),
      load32_h(p, 28),
      load32_h(p, 32),
      load32_h(p, 36),
      load32_h(p, 40),
      load32_h(p, 44),
      load32_h(p, 48),
      load32_h(p, bsdf4_off),
      load32_h(p, bsdf5_off),
      load32_h(p, 52),
      load32_h(p, 56),
      load32_h(p, 60),
      load32_h(p, 64),
      load32_h(p, 68),
      load32_h(p, 72),
      load32_h(p, 76),
      load32_h(p, 80),
      load32_h(p, 84),
      load32_h(p, 88),
      load32_h(p, 100),
      load32_h(p, ext2_off),
      vc_mode
   )
   0
}

fn _set_material_from_part_slab(?ptr p) any {
   if !p { return 0 }
   _set_material_from_slab_offsets(p, 96, 104, 108, load32_h(p, 112))
}

@inline
fn _cache_current_part_material(?ptr p) any {
   if !p { return 0 }
   if !_current_mat_slab_cache { _current_mat_slab_cache = _renderer_alloc(116) }
   memcpy(_current_mat_slab_cache, p, 116)
   _current_mat_slab_ptr = p
   0
}

@inline
fn _current_part_material_cached(?ptr p, int key) bool {
   key != 0 && key == _current_material_key && _current_mat_slab_cache &&
   memcmp(p, _current_mat_slab_cache, 116) == 0
}

@inline
fn _current_part_material_fields_match(?ptr p, int key) bool {
   key != 0 && key == _current_material_key &&
   _current_base_color_u32 == load32_h(p, 0) &&
   _current_metallic_roughness_u32 == load32_h(p, 4) &&
   _current_emissive_u32 == load32_h(p, 8) &&
   _current_emissive_tex_id == _norm_i32(load32_h(p, 12)) &&
   _current_emissive_uv_set == load32_h(p, 16) &&
   _current_base_tex_id == _norm_i32(load32_h(p, 20)) &&
   _current_alpha_u32 == load32_h(p, 24) &&
   _current_occlusion_tex_id == _norm_i32(load32_h(p, 28)) &&
   _current_occlusion_uv_set == load32_h(p, 32) &&
   _current_bsdf0_u32 == load32_h(p, 36) &&
   _current_bsdf1_u32 == load32_h(p, 40) &&
   _current_bsdf2_u32 == load32_h(p, 44) &&
   _current_bsdf3_u32 == load32_h(p, 48) &&
   _current_base_uv_xf0 == load32_h(p, 52) &&
   _current_base_uv_xf1 == load32_h(p, 56) &&
   _current_normal_uv_xf0 == load32_h(p, 60) &&
   _current_normal_uv_xf1 == load32_h(p, 64) &&
   _current_mr_uv_xf0 == load32_h(p, 68) &&
   _current_mr_uv_xf1 == load32_h(p, 72) &&
   _current_occlusion_uv_xf0 == load32_h(p, 76) &&
   _current_occlusion_uv_xf1 == load32_h(p, 80) &&
   _current_emissive_uv_xf0 == load32_h(p, 84) &&
   _current_emissive_uv_xf1 == load32_h(p, 88) &&
   _current_bsdf4_u32 == load32_h(p, 96) &&
   _current_normal_tex_id == _norm_normal_tex_word(load32_h(p, 100)) &&
   _current_bsdf5_u32 == load32_h(p, 104) &&
   _current_ext2_tex_word == load32_h(p, 108) &&
   _current_vc_mode == load32_h(p, 112)
}

@inline
fn _load_current_part_material(?ptr p, int key) any {
   _current_material_key = key
   _current_base_color_u32 = load32_h(p, 0)
   _current_metallic_roughness_u32 = load32_h(p, 4)
   _current_emissive_u32 = load32_h(p, 8)
   _current_emissive_tex_id = _norm_i32(load32_h(p, 12))
   _current_emissive_uv_set = load32_h(p, 16)
   _current_base_tex_id = _norm_i32(load32_h(p, 20))
   _current_alpha_u32 = load32_h(p, 24)
   _current_occlusion_tex_id = _norm_i32(load32_h(p, 28))
   _current_occlusion_uv_set = load32_h(p, 32)
   _current_bsdf0_u32 = load32_h(p, 36)
   _current_bsdf1_u32 = load32_h(p, 40)
   _current_bsdf2_u32 = load32_h(p, 44)
   _current_bsdf3_u32 = load32_h(p, 48)
   _current_base_uv_xf0 = load32_h(p, 52)
   _current_base_uv_xf1 = load32_h(p, 56)
   _current_normal_uv_xf0 = load32_h(p, 60)
   _current_normal_uv_xf1 = load32_h(p, 64)
   _current_mr_uv_xf0 = load32_h(p, 68)
   _current_mr_uv_xf1 = load32_h(p, 72)
   _current_occlusion_uv_xf0 = load32_h(p, 76)
   _current_occlusion_uv_xf1 = load32_h(p, 80)
   _current_emissive_uv_xf0 = load32_h(p, 84)
   _current_emissive_uv_xf1 = load32_h(p, 88)
   _current_bsdf4_u32 = load32_h(p, 96)
   _current_normal_tex_id = _norm_normal_tex_word(load32_h(p, 100))
   _current_bsdf5_u32 = load32_h(p, 104)
   _current_ext2_tex_word = load32_h(p, 108)
   _current_vc_mode = load32_h(p, 112)
   _cache_current_part_material(p)
   _current_emissive_tex_word = _pack_emissive_tex_word(_current_emissive_tex_id, _current_emissive_uv_set)
   _current_base_tex_word = _pack_base_tex_word(_current_base_tex_id, _current_vc_mode)
   _current_occlusion_tex_word = _pack_occlusion_tex_word(_current_occlusion_tex_id, _current_occlusion_uv_set)
   _current_normal_tex_word = _pack_normal_tex_word_current(_current_normal_tex_id, _current_normal_uv_xf1)
   0
}

@inline
fn _trace_current_part_material(int key) any {
   if _trace_mat {
      def _base_tex_path = _texture_meta(_current_base_tex_id, "path", "")
      ui_profile.print_text("[vk:matpart] key=" + to_str(key) +
         " base=0x" + text.to_hex(_current_base_color_u32) +
         " mat=0x" + text.to_hex(_current_metallic_roughness_u32) +
         " baseTex=" + to_str(_current_base_tex_id) +
         " basePath=" + to_str(_base_tex_path) +
         " nrmTex=" + to_str(_current_normal_tex_id) +
         " ext2=0x" + text.to_hex(_current_ext2_tex_word) +
      " vc=" + to_str(_current_vc_mode))
   }
   0
}

@inline
fn _set_material_from_part_slab_key_impl(?ptr p, int key, bool bind_base_texture) any {
   if !p { return 0 }
   if _current_part_material_cached(p, key) || _current_part_material_fields_match(p, key) { return 0 }
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 2
      _flush()
   }
   _load_current_part_material(p, key)
   if bind_base_texture && _current_base_tex_id >= 0 { bind_texture(_current_base_tex_id) }
   _sync_current_material_texture_slots_impl(!bind_base_texture)
   _trace_current_part_material(key)
   _pc_dirty = true
   0
}

fn _set_material_from_part_slab_key(?ptr p, int key) any {
   _set_material_from_part_slab_key_impl(p, key, true)
}

fn _set_material_from_part_slab_key_no_sync(?ptr p, int key) any {
   _set_material_from_part_slab_key_impl(p, key, false)
}

fn set_material_from_slab(?ptr p, int vc_mode = 0) any {
   "Sets material state from the standard 160-byte renderer material slab."
   _set_material_from_slab_offsets(p, 144, 148, 152, vc_mode)
}

fn set_cam_pos(f64 x, f64 y, f64 z) any {
   "Sets camera world-space position for PBR specular calculations."
   def nx, ny = x, y
   def nz = z
   if nx == _cam_pos_x && ny == _cam_pos_y && nz == _cam_pos_z { return 0 }
   _cam_pos_x, _cam_pos_y = nx, ny
   _cam_pos_z = nz
   _pc_dirty = true
   0
}

fn set_env_tex(int tex_id) any {
   "Sets the active environment/sky texture id for lit shading."
   def next_id = tex_id
   if next_id == _current_env_tex_id { return 0 }
   _current_env_tex_id = next_id
   if _trace_mat { ui_profile.print_text("[vk:env] diffuse=" + to_str(_current_env_tex_id) + " spec=" + to_str(_current_env_spec_tex_id)) }
   _pc_dirty = true
   0
}

fn set_env_spec_tex(int tex_id) any {
   "Sets the active specular IBL texture id for lit shading."
   def next_id = tex_id
   if next_id == _current_env_spec_tex_id { return 0 }
   _current_env_spec_tex_id = next_id
   if _trace_mat { ui_profile.print_text("[vk:env] diffuse=" + to_str(_current_env_tex_id) + " spec=" + to_str(_current_env_spec_tex_id)) }
   _pc_dirty = true
   0
}

fn _pack_light_ubo_slab() any {
   if !_scene_light_ubo_slab { return 0 }
   def light_max = _scene_light_max_value()
   def light_ubo_size = _scene_light_ubo_size_value()
   memset(_scene_light_ubo_slab, 0, light_ubo_size)
   def pos_base = 0
   def color_base = light_max * 16
   def dir_base = light_max * 32
   def l0 = 0
   def l1 = 16
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_pos_type,   0, 0.0), pos_base + l0 + 0)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_pos_type,   1, 0.0), pos_base + l0 + 4)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_pos_type,   2, 0.0), pos_base + l0 + 8)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_pos_type,   3, 0.0), pos_base + l0 + 12)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_pos_type,   0, 0.0), pos_base + l1 + 0)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_pos_type,   1, 0.0), pos_base + l1 + 4)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_pos_type,   2, 0.0), pos_base + l1 + 8)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_pos_type,   3, 0.0), pos_base + l1 + 12)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_color_range,0, 0.0), color_base + l0 + 0)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_color_range,1, 0.0), color_base + l0 + 4)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_color_range,2, 0.0), color_base + l0 + 8)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_color_range,3, 0.0), color_base + l0 + 12)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_color_range,0, 0.0), color_base + l1 + 0)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_color_range,1, 0.0), color_base + l1 + 4)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_color_range,2, 0.0), color_base + l1 + 8)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_color_range,3, 0.0), color_base + l1 + 12)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_dir_outer,  0, 0.0), dir_base + l0 + 0)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_dir_outer,  1, 0.0), dir_base + l0 + 4)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_dir_outer,  2,-1.0), dir_base + l0 + 8)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light0_dir_outer,  3, 0.0), dir_base + l0 + 12)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_dir_outer,  0, 0.0), dir_base + l1 + 0)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_dir_outer,  1, 0.0), dir_base + l1 + 4)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_dir_outer,  2,-1.0), dir_base + l1 + 8)
   store32_f32(_scene_light_ubo_slab, _list_num_safe(_scene_light1_dir_outer,  3, 0.0), dir_base + l1 + 12)
   0
}

fn _sync_scene_light_ubo() bool {
   def light_ubo_size = _scene_light_ubo_size_value()
   if _trace_light_bind {
      print(
         "[vk:ubo-guard] map=" + to_str(_ubo_map) +
         " slab=" + to_str(_scene_light_ubo_slab) +
         " stride=" + to_str(_ubo_stride) +
         " map_size=" + to_str(_ubo_map_size) +
         " need=" + to_str(light_ubo_size)
      )
   }
   if !_ubo_map || !_scene_light_ubo_slab || _ubo_stride <= 0 || _ubo_map_size < light_ubo_size { return false }
   def frame = int(_current_frame)
   if _trace_light_bind { ui_profile.print_text("[vk:ubo-frame] " + to_str(frame)) }
   if frame < 0 || frame >= _frames_in_flight() { return false }
   def bo = 0
   if _trace_light_bind { ui_profile.print_text("[vk:ubo-bo] " + to_str(bo)) }
   if bo < 0 || bo + light_ubo_size > int(_ubo_map_size) { return false }
   __copy_mem(_ubo_map, _scene_light_ubo_slab, light_ubo_size)
   if _trace_light_bind {
      print(
         "[vk:ubo] frame=" + to_str(frame) +
         " bo=" + to_str(bo) +
         " p0=(" +
         to_str(load32_f32(_scene_light_ubo_slab, 0)) + "," +
         to_str(load32_f32(_scene_light_ubo_slab, 4)) + "," +
         to_str(load32_f32(_scene_light_ubo_slab, 8)) + "," +
         to_str(load32_f32(_scene_light_ubo_slab, 12)) + ") c0=(" +
         to_str(load32_f32(_scene_light_ubo_slab, 16)) + "," +
         to_str(load32_f32(_scene_light_ubo_slab, 20)) + "," +
         to_str(load32_f32(_scene_light_ubo_slab, 24)) + "," +
         to_str(load32_f32(_scene_light_ubo_slab, 28)) + ")"
      )
   }
   true
}

fn _scene_light_scratch_slab() int {
   def light_max = _scene_light_max_value()
   if !_scene_light_tmp_slab { _scene_light_tmp_slab = _renderer_alloc(light_max * 56) }
   if _scene_light_tmp_slab { memset(_scene_light_tmp_slab, 0, light_max * 56) }
   _scene_light_tmp_slab
}

fn set_scene_lights(any lights) any {
   "Sets up to 8 punctual scene lights for subsequent 3D draws."
   mut new_count = 0
   mut new_l0p = [0.0,0.0,0.0,0.0]
   mut new_l0c = [0.0,0.0,0.0,0.0]
   mut new_l0d = [0.0,0.0,-1.0,0.0]
   mut new_l1p = [0.0,0.0,0.0,0.0]
   mut new_l1c = [0.0,0.0,0.0,0.0]
   mut new_l1d = [0.0,0.0,-1.0,0.0]
   mut tmp_slab = 0
   mut peak_light_value = 0.0
   if is_list(lights) && lights.len > 0 { tmp_slab = _scene_light_scratch_slab() }
   if is_list(lights) {
      mut i = 0
      def light_max = _scene_light_max_value()
      while i < lights.len && new_count < light_max {
         def l = lights[i]
         if is_dict(l) {
            def pos = l.get("position", [0.0,0.0,0.0])
            def dir = l.get("direction", [0.0,0.0,-1.0])
            def col = l.get("color", [1.0,1.0,1.0])
            def intensity = _num_from_any(l.get("intensity", 1.0), 1.0)
            def range = _num_from_any(l.get("range", 0.0), 0.0)
            def outer = _num_from_any(l.get("outer_cone_cos", 0.0), 0.0)
            def typ_s = to_str(l.get("type", "point"))
            def typ = typ_s == "directional" ? 0.0 : (typ_s == "spot" ? 2.0 : 1.0)
            def light_peak = max(
               _list_num_safe(col, 0, 1.0),
               max(_list_num_safe(col, 1, 1.0), _list_num_safe(col, 2, 1.0))
            ) * intensity
            if light_peak > peak_light_value { peak_light_value = light_peak }
            def pos_type = [_list_num_safe(pos, 0, 0.0), _list_num_safe(pos, 1, 0.0), _list_num_safe(pos, 2, 0.0), typ]
            def col_rng = [
               _list_num_safe(col, 0, 1.0) * intensity,
               _list_num_safe(col, 1, 1.0) * intensity,
               _list_num_safe(col, 2, 1.0) * intensity,
               range
            ]
            def dir_out = [_list_num_safe(dir, 0, 0.0), _list_num_safe(dir, 1, 0.0), _list_num_safe(dir, 2, -1.0), outer]
            if new_count == 0 {
               new_l0p, new_l0c = pos_type, col_rng
               new_l0d = dir_out
            } elif new_count == 1 {
               new_l1p, new_l1c = pos_type, col_rng
               new_l1d = dir_out
            }
            if tmp_slab {
               def dst = tmp_slab + new_count * 56
               store32_f32(dst, _list_num_safe(pos, 0, 0.0), 0)
               store32_f32(dst, _list_num_safe(pos, 1, 0.0), 4)
               store32_f32(dst, _list_num_safe(pos, 2, 0.0), 8)
               store32_f32(dst, _list_num_safe(col, 0, 1.0), 12)
               store32_f32(dst, _list_num_safe(col, 1, 1.0), 16)
               store32_f32(dst, _list_num_safe(col, 2, 1.0), 20)
               store32_f32(dst, intensity, 24)
               store32_f32(dst, range, 28)
               store32(dst, int(typ), 32)
               store32_f32(dst, _list_num_safe(dir, 0, 0.0), 36)
               store32_f32(dst, _list_num_safe(dir, 1, 0.0), 40)
               store32_f32(dst, _list_num_safe(dir, 2, -1.0), 44)
               store32_f32(dst, outer, 48)
            }
            new_count += 1
         }
         i += 1
      }
   }
   _scene_light_count = new_count
   _scene_light0_pos_type = new_l0p
   _scene_light0_color_range = new_l0c
   _scene_light0_dir_outer = new_l0d
   _scene_light1_pos_type = new_l1p
   _scene_light1_color_range = new_l1c
   _scene_light1_dir_outer = new_l1d
   if tmp_slab {
      def scene_exposure = _scene_light_exposure_from_peak(peak_light_value)
      if scene_exposure < 0.999999 {
         mut si = 0
         while si < new_count {
            def dst = tmp_slab + si * 56
            store32_f32(dst, load32_f32(dst, 12) * scene_exposure, 12)
            store32_f32(dst, load32_f32(dst, 16) * scene_exposure, 16)
            store32_f32(dst, load32_f32(dst, 20) * scene_exposure, 20)
            si += 1
         }
      }
      set_scene_lights_slab(tmp_slab, new_count)
      _scene_light_slab_src = 0
      _scene_light_slab_src_count = -1
      return 0
   }
   _scene_lights_dirty = true
   _pc_dirty = true
   _pack_light_ubo_slab()
   if _sync_scene_light_ubo() { _scene_lights_dirty = false }
   0
}

fn set_scene_lights_slab(?ptr slab_ptr, int count) any {
   "Optimized path to set scene lights from a packed slab.
   Format: [pos:12][color:12][intensity:4][range:4][type:4][dir:12][outer:4][pad:4] = 56 bytes per light."
   if _trace_light_bind { ui_profile.print_text("[vk:slab-fn] enter") }
   def frame = int(_current_frame)
   def frame_bit = 1 << frame
   if !slab_ptr {
      if _scene_light_slab_src == 0
      && _scene_light_slab_src_count == 0
      && band(_scene_light_frame_mask, frame_bit) != 0{
         return 0
      }
      if !(_scene_light_slab_src == 0 && _scene_light_slab_src_count == 0) { _scene_light_frame_mask = 0 }
      _scene_light_count = 0
      _scene_light_slab_src = 0
      _scene_light_slab_src_count = 0
      _scene_light0_pos_type = [0.0,0.0,0.0,0.0]
      _scene_light0_color_range = [0.0,0.0,0.0,0.0]
      _scene_light0_dir_outer = [0.0,0.0,-1.0,0.0]
      _scene_light1_pos_type = [0.0,0.0,0.0,0.0]
      _scene_light1_color_range = [0.0,0.0,0.0,0.0]
      _scene_light1_dir_outer = [0.0,0.0,-1.0,0.0]
      if _scene_light_ubo_slab { memset(_scene_light_ubo_slab, 0, _scene_light_ubo_size_value()) }
      _scene_lights_dirty = true
      _pc_dirty = true
      if _sync_scene_light_ubo() {
         _scene_lights_dirty = false
         _scene_light_frame_mask = _scene_light_frame_mask | frame_bit
      }
      return 0
   }
   mut n = count
   def light_max = _scene_light_max_value()
   if n > light_max { n = light_max }
   if slab_ptr == _scene_light_slab_src && n == _scene_light_slab_src_count {
      if band(_scene_light_frame_mask, frame_bit) != 0 { return 0 }
      _scene_lights_dirty = true
      _pc_dirty = true
      if _sync_scene_light_ubo() {
         _scene_lights_dirty = false
         _scene_light_frame_mask = _scene_light_frame_mask | frame_bit
      }
      return 0
   }
   _scene_light_frame_mask = 0
   _scene_light_count = n
   _scene_light_slab_src = slab_ptr
   _scene_light_slab_src_count = n
   mut peak_light_value = 0.0
   mut pi = 0
   while pi < n {
      def src = slab_ptr + pi * 56
      def intensity = load32_f32(src, 24)
      def light_peak = max(load32_f32(src, 12), max(load32_f32(src, 16), load32_f32(src, 20))) * intensity
      if light_peak > peak_light_value { peak_light_value = light_peak }
      pi += 1
   }
   def scene_exposure = _scene_light_exposure_from_peak(peak_light_value)
   if !_scene_light_ubo_slab { _scene_light_ubo_slab = _renderer_alloc(_scene_light_ubo_size_value()) }
   if !_scene_light_ubo_slab { return 0 }
   def s = _scene_light_ubo_slab
   memset(s, 0, _scene_light_ubo_size_value())
   def pos_base = 0
   def color_base = light_max * 16
   def dir_base = light_max * 32
   mut i = 0
   while i < light_max {
      def pos_dst = s + pos_base + i * 16
      def color_dst = s + color_base + i * 16
      def dir_dst = s + dir_base + i * 16
      store32_f32(dir_dst, -1.0, 8)
      if i < n {
         def src = slab_ptr + i * 56
         def intensity = load32_f32(src, 24)
         store32_f32(pos_dst, load32_f32(src, 0),                0)
         store32_f32(pos_dst, load32_f32(src, 4),                4)
         store32_f32(pos_dst, load32_f32(src, 8),                8)
         store32_f32(pos_dst, float(load32(src, 32)),           12)
         store32_f32(color_dst, load32_f32(src, 12) * intensity * scene_exposure, 0)
         store32_f32(color_dst, load32_f32(src, 16) * intensity * scene_exposure, 4)
         store32_f32(color_dst, load32_f32(src, 20) * intensity * scene_exposure, 8)
         store32_f32(color_dst, load32_f32(src, 28),            12)
         store32_f32(dir_dst, load32_f32(src, 36),               0)
         store32_f32(dir_dst, load32_f32(src, 40),               4)
         store32_f32(dir_dst, load32_f32(src, 44),               8)
         store32_f32(dir_dst, load32_f32(src, 48),              12)
      }
      i += 1
   }
   if _trace_light_detail {
      ui_profile.print_text("[vk:lights] count=" + to_str(n))
      mut ti = 0
      while ti < n && ti < 4 {
         def pos_dst = s + pos_base + ti * 16
         def color_dst = s + color_base + ti * 16
         def dir_dst = s + dir_base + ti * 16
         print(
            "  l[" + to_str(ti) + "] pos=(" +
            to_str(load32_f32(pos_dst, 0)) + "," +
            to_str(load32_f32(pos_dst, 4)) + "," +
            to_str(load32_f32(pos_dst, 8)) + ") type=" +
            to_str(load32_f32(pos_dst, 12)) + " color=(" +
            to_str(load32_f32(color_dst, 0)) + "," +
            to_str(load32_f32(color_dst, 4)) + "," +
            to_str(load32_f32(color_dst, 8)) + ") range=" +
            to_str(load32_f32(color_dst, 12)) + " dir=(" +
            to_str(load32_f32(dir_dst, 0)) + "," +
            to_str(load32_f32(dir_dst, 4)) + "," +
            to_str(load32_f32(dir_dst, 8)) + ") outer=" +
            to_str(load32_f32(dir_dst, 12))
         )
         ti += 1
      }
   }
   _scene_lights_dirty = true
   _pc_dirty = true
   if _sync_scene_light_ubo() {
      _scene_lights_dirty = false
      _scene_light_frame_mask = _scene_light_frame_mask | frame_bit
   }
   0
}

fn set_mask(int m) any {
   "Marks mask mode for subsequent draws without forcing an immediate flush."
   def val = m
   if val != _last_is_mask {
      if _vertex_offset != _last_flush_offset { _flush() }
      _last_is_mask = val
      _pc_dirty = true
   }
   0
}

fn _sync_pc() any {
   def _prof_on = _vk_profile_enabled()
   def _t0 = _prof_on ? ticks() : 0
   def pc_ptr = _use_custom_pc ? _pc_buffer_custom : _pc_buffer
   if !_use_custom_pc {
      if _current_is_unlit != _last_is_unlit { _last_is_unlit = _current_is_unlit _pc_dirty = true }
      if _current_vc_mode != _last_vc_mode { _last_vc_mode = _current_vc_mode _pc_dirty = true }
      if _current_metallic != _last_metallic { _pc_dirty = true }
      if _current_roughness != _last_roughness { _pc_dirty = true }
      if _mvp_dirty || _model_dirty { _pc_dirty = true }
      if !_pc_dirty { return 0 }
      if _mvp_dirty {
         memcpy(pc_ptr, _current_mvp, 64)
         _mvp_dirty = false
      }
      if _model_dirty {
         memcpy(pc_ptr + 64, _current_model, 64)
         _model_dirty = false
      }
      store32_h(pc_ptr, _last_is_mask, 128)
      store32_h(pc_ptr, _last_is_unlit, 132)
      store32_h(pc_ptr, _current_base_color_u32, 136)
      store32_h(pc_ptr, _current_metallic_roughness_u32, 140)
      store32_h(pc_ptr, _packed_env_scene_word(), 144)
      store32_h(pc_ptr, _current_env_spec_tex_id, 148)
      store32_h(pc_ptr, _current_bsdf4_u32, 152)
      store32_h(pc_ptr, _current_bsdf5_u32, 156)
      store32_f32(pc_ptr, _cam_pos_x, 160)
      store32_f32(pc_ptr, _cam_pos_y, 164)
      store32_f32(pc_ptr, _cam_pos_z, 168)
      store32_h(pc_ptr, _current_emissive_u32, 172)
      store32_h(pc_ptr, _current_emissive_tex_word, 176)
      store32_h(pc_ptr, _current_base_tex_word, 180)
      store32_h(pc_ptr, _current_alpha_u32, 184)
      store32_h(pc_ptr, _current_occlusion_tex_word, 188)
      store32_h(pc_ptr, _current_bsdf0_u32, 192)
      store32_h(pc_ptr, _current_bsdf1_u32, 196)
      store32_h(pc_ptr, _current_bsdf2_u32, 200)
      store32_h(pc_ptr, _current_bsdf3_u32, 204)
      store32_h(pc_ptr, _current_base_uv_xf0, 208)
      store32_h(pc_ptr, _current_base_uv_xf1, 212)
      store32_h(pc_ptr, _current_normal_uv_xf0, 216)
      store32_h(pc_ptr, _current_normal_uv_xf1, 220)
      store32_h(pc_ptr, _current_mr_uv_xf0, 224)
      store32_h(pc_ptr, _current_mr_uv_xf1, 228)
      store32_h(pc_ptr, _current_occlusion_uv_xf0, 232)
      store32_h(pc_ptr, _current_occlusion_uv_xf1, 236)
      store32_h(pc_ptr, _current_emissive_uv_xf0, 240)
      store32_h(pc_ptr, _current_emissive_uv_xf1, 244)
      store32_h(pc_ptr, _current_normal_tex_word, 248)
      store32_h(pc_ptr, _current_ext2_tex_word, 252)
      _last_metallic = _current_metallic
      _last_roughness = _current_roughness
   }
   def cb = _current_frame_cb
   if cb && _pipeline_layout && _pc_dirty {
      if common.env_truthy("NY_VK_CAPTURE_TRACE") && _vk_pc_trace_logs < 24 {
         _vk_pc_trace_logs += 1
         ui_profile.print_text("[vk:pc] frame=" + to_str(_total_frames) +
            " mvp=(" + to_str(load32_f32(pc_ptr, 0)) + "," + to_str(load32_f32(pc_ptr, 20)) + "," + to_str(load32_f32(pc_ptr, 40)) + "," + to_str(load32_f32(pc_ptr, 60)) + ")" +
            " mvp_t=(" + to_str(load32_f32(pc_ptr, 48)) + "," + to_str(load32_f32(pc_ptr, 52)) + "," + to_str(load32_f32(pc_ptr, 56)) + ")" +
            " model_diag=(" + to_str(load32_f32(pc_ptr, 64)) + "," + to_str(load32_f32(pc_ptr, 84)) + "," + to_str(load32_f32(pc_ptr, 104)) + "," + to_str(load32_f32(pc_ptr, 124)) + ")" +
            " model_t=(" + to_str(load32_f32(pc_ptr, 112)) + "," + to_str(load32_f32(pc_ptr, 116)) + "," + to_str(load32_f32(pc_ptr, 120)) + ")" +
            " mask=" + to_str(load32(pc_ptr, 128)) +
            " unlit=" + to_str(load32(pc_ptr, 132)) +
            " base=0x" + text.to_hex(load32(pc_ptr, 136)) +
            " mat=0x" + text.to_hex(load32(pc_ptr, 140)) +
            " env=0x" + text.to_hex(load32(pc_ptr, 144)) +
            " spec=" + to_str(load32(pc_ptr, 148)) +
            " btex=0x" + text.to_hex(load32(pc_ptr, 180)) +
            " alpha=0x" + text.to_hex(load32(pc_ptr, 184)) +
            " baseUvXf=0x" + text.to_hex(load32(pc_ptr, 208)) + "/0x" + text.to_hex(load32(pc_ptr, 212)) +
            " ntex=0x" + text.to_hex(load32(pc_ptr, 248)))
      }
      cmd_push_constants(cb, _pipeline_layout, 0x00000001 | 0x00000010, 0, shader_pc_bytes(), pc_ptr)
      _pc_dirty = false
   }
   if _prof_on { _frame_sync_pc_cpu_us += float(ticks() - _t0) / 1000.0 }
   0
}

fn _flush() any {
   def _prof_on = _vk_profile_enabled()
   def _t0 = _prof_on ? ticks() : 0
   if !_frame_open { return 0 }
   if _vertex_offset == _last_flush_offset { return 0 }
   _flush_total += 1
   if _flush_reason == 1 { _flush_reason_tex += 1 }
   elif _flush_reason == 2 { _flush_reason_pipe += 1 }
   elif _flush_reason == 3 { _flush_reason_static += 1 }
   elif _flush_reason == 4 { _flush_reason_special += 1 }
   elif _flush_reason == 5 { _flush_reason_vertex_full += 1 }
   def count = (_vertex_offset - _last_flush_offset) / _VKR_VERT_STRIDE
   if count <= 0 {
      _last_flush_offset = _vertex_offset
      _flush_reason = 0
      return 0
   }
   def first_vert = _last_flush_offset / _VKR_VERT_STRIDE
   def cb = _current_frame_cb
   mut target = _target_pipeline
   if target == _pipeline {
      if _current_is_unlit != 0 {
         def up = _get_unlit_nocull_pipeline()
         if up != 0 { target = up }
      }
      else { target = _pipeline }
   }
   if _is_wireframe && _wire_pipeline != 0 { if target == _pipeline || target == _unlit_pipeline { target = _wire_pipeline } }
   _flush_diag("before bind pipeline")
   if !_vkr_bind_pipeline_if_needed(cb, target) {
      _last_flush_offset = _vertex_offset
      _flush_reason = 0
      return 0
   }
   _flush_diag("after bind pipeline")
   def ubo_ds = _current_frame_ubo_ds
   if _bindless_ds && (_bindless_ds != _last_bound_ds || ubo_ds != _last_bound_ubo_ds) {
      _flush_diag("before descriptors")
      store64_h(_ptr_ds, _bindless_ds, 0)
      store64_h(_ptr_ds, ubo_ds, 8)
      cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, 2, _ptr_ds, 0, 0)
      _last_bound_ds = _bindless_ds
      _last_bound_ubo_ds = ubo_ds
      _descriptor_bind_count += 1
      _flush_diag("after descriptors")
   }
   _flush_diag("before pc")
   _sync_pc()
   _flush_diag("after pc")
   _flush_diag("before vbo")
   _vkr_bind_dynamic_vertex_buffer(cb)
   _flush_diag("after vbo")
   if (_vk_deep_debug == 1) && (_total_frames % 300) == 0 {
      print(
         "[vk:flush] first=" + to_str(first_vert)
         + " cnt=" + to_str(count)
         + " pipe=0x" + text.to_hex(target)
         + " tex=" + to_str(_current_texture_id)
         + " unlit=" + to_str(_current_is_unlit)
      )
   }
   if common.env_truthy("NY_VK_CAPTURE_TRACE") {
      ui_profile.print_text("[vk:flush] first=" + to_str(first_vert) +
         " count=" + to_str(count) +
         " pipe=0x" + text.to_hex(target) +
         " tex=" + to_str(_current_texture_id) +
         " btex=0x" + text.to_hex(_current_base_tex_word) +
         " alpha=0x" + text.to_hex(_current_alpha_u32) +
      " vc=" + to_str(_current_vc_mode))
   }
   if _vk_markers_enabled { vk_debug_marker_begin(cb, "Flush Batch", 0x00FF00FF) }
   _flush_diag("before draw")
   cmd_draw(cb, count, 1, first_vert, 0)
   _flush_diag("after draw")
   if _vk_markers_enabled { vk_debug_marker_end(cb) }
   _total_draw_calls += 1
   _frame_draw_calls += 1
   _frame_dynamic_draw_calls += 1
   _last_flush_offset = _vertex_offset
   _flush_reason = 0
   _batch_texture_id = _current_texture_id
   _batch_tex_index = _current_tex_index
   if _prof_on { _frame_flush_cpu_us += float(ticks() - _t0) / 1000.0 }
   0
}

fn _check_flush(int bytes) bool {
   if _vertex_limit_hit {
      ;; Surface the truncation once per frame so a single oversized text run
      ;; (e.g. paste of 174K+ chars, which exceeds the 64MB vertex buffer)
      ;; does not blank out the rest of the frame's text with no diagnostic.
      if !_logged_vertex_limit_hit {
         _logged_vertex_limit_hit = true
         if vk_state._debug_gfx_enabled {
            ui_profile.print_text("[gfx:vulkan] vertex buffer full — text runs truncated for the rest of this frame (bytes=" + to_str(bytes) + ")")
         }
      }
      return false
   }
   if _vertex_offset + bytes > _vertex_capacity {
      _flush_reason = 5
      _flush()
      if _vertex_offset + bytes > _vertex_capacity {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] vertex buffer full current_frame=true") }
         _vertex_limit_hit = true
         return false
      }
   }
   true
}

fn end_frame() bool {
   "Finalizes rendering and presents the frame to the swapchain image."
   _end_frame_internal(true)
}

fn _end_frame_internal(bool present) bool {
   def _prof_on = _vk_profile_enabled()
   def _t_end = _prof_on ? ticks() : 0
   def _profile_frame = _total_frames
   def _deep_on = (_vk_deep_debug == 1) && ((_prof_on && ((_profile_frame % _vk_profile_every()) == 0)) || ((_profile_frame % _vk_deep_every()) == 0))
   def _t_end_deep = _deep_on ? ticks() : 0
   if !_frame_open { return false }
   def has_surface = _has_valid_surface_fast()
   def backend_is_win32 = has_surface && _backend_is_win32_fast()
   if !_vk_frame_targets_ready(has_surface) { return false }
   def _t_flush = _deep_on ? ticks() : 0
   _flush()
   if _deep_on { _vk_deep_end_flush_ms = ui_profile.elapsed_ms(_t_flush) }
   def cb = load64(_cmd_bufs_slab, _current_frame * 8)
   cmd_end_render_pass(cb)
   if _offscreen_draw_enabled() && !_record_draw_image_to_swapchain(cb, has_surface) { return false }
   if _capture_request {
      def w, h = _swapchain_extent_w, _swapchain_extent_h
      def size = w * h * 4
      if w > 0 && h > 0 && _staging_buffer && _staging_map && _staging_capacity >= size {
         if _ensure_readback_slab() {
            def barrier = _readback_barrier
            def region = _readback_region
            def src_image = _swapchain_images.get(_image_index)
            if barrier && region && _handle_ok(src_image) {
               def old_layout = has_surface ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
               _record_image_readback_to_buffer(cb,
                  src_image,
                  old_layout,
                  _staging_buffer,
                  w,
                  h,
                  barrier,
                  region,
               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
            }
         }
      }
   }
   if _vk_markers_enabled {
      vk_debug_marker_end(cb)
      vk_debug_marker_end(cb)
   }
   def _t_end_cmd = _deep_on ? ticks() : 0
   def ecb = end_command_buffer(cb)
   if _deep_on { _vk_deep_end_cmd_ms = ui_profile.elapsed_ms(_t_end_cmd) }
   if ecb != 0 { return false }
   if has_surface {
      def sem_avail = load64_h(_sem_avail_slab, _current_frame * 8)
      def sem_finish = load64_h(_sem_finish_slab, _current_frame * 8)
      store64_h(_ptr_wait_sems, sem_avail, 0)
      store64_h(_ptr_sig_sems, sem_finish, 0)
      store32(_ptr_stages,
         _offscreen_draw_enabled() ? (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT) : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0)
   }
   store32(_ptr_sub, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store64_h(_ptr_sub, 0, 8)
   if has_surface {
      store32(_ptr_sub, 1, 16)
      store64_h(_ptr_sub, _ptr_wait_sems, 24)
      store64_h(_ptr_sub, _ptr_stages, 32)
   } else {
      store32(_ptr_sub, 0, 16)
      store64_h(_ptr_sub, 0, 24)
      store64_h(_ptr_sub, 0, 32)
   }
   store32(_ptr_sub, 1, 40)
   mut cb_ptr = _ptr_sub + 80
   store64_h(cb_ptr, cb, 0)
   store64_h(_ptr_sub, cb_ptr, 48)
   if has_surface {
      store32(_ptr_sub, 1, 56)
      store64_h(_ptr_sub, _ptr_sig_sems, 64)
   } else {
      store32(_ptr_sub, 0, 56)
      store64_h(_ptr_sub, 0, 64)
   }
   def fence = load64_h(_fences_slab, _current_frame * 8)
   if _vk_debug_basic == 1 {
      ui_profile.print_text("[gfx:vulkan] end_frame before submit frame=" + to_str(_current_frame) +
      " image=" + to_str(_image_index))
   }
   def _t_submit = _deep_on ? ticks() : 0
   if backend_is_win32 { _pump_host_messages_if_needed() }
   def sub_res = queue_submit(_graphics_queue, 1, _ptr_sub, fence)
   if backend_is_win32 { _pump_host_messages_if_needed() }
   if _deep_on { _vk_deep_end_submit_ms = ui_profile.elapsed_ms(_t_submit) }
   if _vk_debug_basic == 1 { ui_profile.print_text("[gfx:vulkan] end_frame after submit result=" + to_str(sub_res)) }
   if sub_res != 0 { return false }
   if _capture_request {
      _capture_request = false
      _capture_ready = false
      def capture_wait_res = wait_for_fences(_device, 1, _ptr_fence, 1, 5000000000)
      if capture_wait_res == 0 {
         def w, h = _swapchain_extent_w, _swapchain_extent_h
         def size = w * h * 4
         if _staging_map && w > 0 && h > 0 && _staging_capacity >= size {
            if _capture_pixels { free(_capture_pixels) _capture_pixels = 0 }
            def pixels = _renderer_alloc(size)
            if pixels {
               __copy_mem(pixels, _staging_map, size)
               _vkr_bgra_to_rgba_if_needed(pixels, size, _swapchain_format)
               if _debug_gfx_enabled || common.env_truthy("NY_VK_CAPTURE_TRACE") || common.env_truthy("NY_GLTF_INDEX_TRACE") {
                  def p0 = size >= 4 ? (load8(pixels, 0) | (load8(pixels, 1) << 8) | (load8(pixels, 2) << 16) | (load8(pixels, 3) << 24)) : 0
                  def pc = size >= 4 ? (((h / 2) * w + (w / 2)) * 4) : 0
                  def c0 = (pc >= 0 && pc + 3 < size) ? (load8(pixels, pc) | (load8(pixels, pc + 1) << 8) | (load8(pixels, pc + 2) << 16) | (load8(pixels, pc + 3) << 24)) : 0
                  ui_profile.print_text("[vk:capture] frame=" + to_str(_total_frames) +
                     " draws=" + to_str(_frame_draw_calls) +
                     " dyn=" + to_str(_frame_dynamic_draw_calls) +
                     " static=" + to_str(_frame_static_draw_calls) +
                     " indexed=" + to_str(_frame_indexed_draw_calls) +
                     " img=" + to_str(_image_index) +
                     " surface=" + to_str(has_surface) +
                     " p0=0x" + text.to_hex(p0) +
                  " pc=0x" + text.to_hex(c0))
                  if _local_vertex_map && _vertex_offset >= _VKR_VERT_STRIDE {
                     ui_profile.print_text("[vk:capture:geom] v0=(" +
                        to_str(load32_f32(_local_vertex_map, _VKR_OFF_X)) + "," +
                        to_str(load32_f32(_local_vertex_map, _VKR_OFF_Y)) + "," +
                        to_str(load32_f32(_local_vertex_map, _VKR_OFF_Z)) + ")" +
                        " c=0x" + text.to_hex(load32(_local_vertex_map, _VKR_OFF_C)) +
                     " voff=" + to_str(_vertex_offset))
                  }
                  ui_profile.print_text("[vk:capture:mvp] m00=" + to_str(load32_f32(_pc_buffer, 0)) +
                     " m11=" + to_str(load32_f32(_pc_buffer, 20)) +
                     " m22=" + to_str(load32_f32(_pc_buffer, 40)) +
                     " m33=" + to_str(load32_f32(_pc_buffer, 60)) +
                     " isUnlit=" + to_str(load32(_pc_buffer, 132)) +
                  " base=0x" + text.to_hex(load32(_pc_buffer, 136)))
               }
               _capture_pixels = pixels
               _capture_w = w
               _capture_h = h
               _capture_ready = true
            }
         }
      }
      elif _debug_gfx_enabled || common.env_truthy("NY_VK_CAPTURE_TRACE") {
         ui_profile.print_text("[gfx:vulkan] capture fence wait failed code=" + to_str(capture_wait_res))
      }
   }
   if present && has_surface && _swapchain != 0 {
      def sc = _swapchain
      def img_idx = _image_index
      mut scs = _ptr_ri
      store64_h(scs, sc, 0)
      mut idxs = scs + 8
      store32(idxs, img_idx, 0)
      mut pi = _ptr_ri + 32
      store32(pi, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0)
      store64_h(pi, 0, 8)
      store32(pi, 1, 16)
      store64_h(pi, _ptr_sig_sems, 24)
      store32(pi, 1, 32)
      store64_h(pi, scs, 40)
      store64_h(pi, idxs, 48)
      store64_h(pi, 0, 56)
      if _vk_debug_basic == 1 {
         ui_profile.print_text("[gfx:vulkan] pre-present stats draws=" + to_str(_frame_draw_calls) +
            " dyn=" + to_str(_frame_dynamic_draw_calls) +
            " static=" + to_str(_frame_static_draw_calls) +
            " indexed=" + to_str(_frame_indexed_draw_calls) +
            " flush=" + to_str(_flush_total) +
         " verts=" + to_str(_vertex_offset / _VKR_VERT_STRIDE))
         ui_profile.print_text("[gfx:vulkan] end_frame before present image=" + to_str(img_idx))
      }
      def _t_present = _deep_on ? ticks() : 0
      if backend_is_win32 { _pump_host_messages_if_needed() }
      def pr = queue_present_khr(_present_queue, pi)
      if backend_is_win32 { _pump_host_messages_if_needed() }
      if _deep_on { _vk_deep_end_present_ms = ui_profile.elapsed_ms(_t_present) }
      if _vk_debug_basic == 1 { ui_profile.print_text("[gfx:vulkan] end_frame after present result=" + to_str(pr)) }
      if pr == 0xC460C464 || pr == -1000001004 {
         _frame_open = false
         _schedule_swapchain_recreate()
         return false
      }
      if pr == 1000001003 {
         if vk_state._debug_gfx_enabled && !_logged_suboptimal_present {
            ui_profile.print_text("[gfx:vulkan] present suboptimal continuing=true")
            _logged_suboptimal_present = true
         }
      } elif pr != 0 {
         ;; Surface lost / device lost / full-screen exclusive lost: previously
         ;; these error codes were silently ignored and the frame was treated
         ;; as successful, leaving a dead swapchain that the next begin_frame
         ;; would fail on without any link back to the present error. Surface
         ;; the failure and schedule a recreate so the next frame has a chance
         ;; to recover.
         if vk_state._debug_gfx_enabled {
            ui_profile.print_text("[gfx:vulkan] present failed code=" + to_str(pr) + " — scheduling recreate")
         }
         _frame_open = false
         _schedule_swapchain_recreate(true)
         return false
      }
      if backend_is_win32 {
         _pump_host_messages_if_needed()
         msleep(1)
         _pump_host_messages_if_needed()
      }
   }
   ;; Backend-local sleep fights the window-system present mode and makes camera,
   ;; gizmo and FPS text cadence visibly uneven under compositors/WMs.  Keep it
   ;; as an explicit diagnostic throttle only.
   def frame_sleep_default = 0
   def frame_sleep_ms = ui_profile.env_present_cached("NY_VK_FRAME_SLEEP_MS") ? ui_profile.env_int_cached("NY_VK_FRAME_SLEEP_MS", 0, 0, 64) : frame_sleep_default
   if frame_sleep_ms > 0 { msleep(frame_sleep_ms) }
   _last_frame_draw_calls = _frame_draw_calls
   _last_frame_dynamic_draw_calls = _frame_dynamic_draw_calls
   _last_frame_static_draw_calls = _frame_static_draw_calls
   _last_frame_indexed_draw_calls = _frame_indexed_draw_calls
   _last_flush_total = _flush_total
   _last_pipeline_bind_count = _pipeline_bind_count
   _last_descriptor_bind_count = _descriptor_bind_count
   _last_submitted_vertices = int(_vertex_offset / _VKR_VERT_STRIDE)
   _last_prim_rect_quads = _prim_rect_quads
   _last_prim_outline_quads = _prim_outline_quads
   _last_prim_line_quads = _prim_line_quads
   _last_prim_raw_lines = _prim_raw_lines
   _last_prim_raw_points = _prim_raw_points
   _last_prim_text_calls = _prim_text_calls
   _last_prim_text_glyphs = _prim_text_glyphs
   if _prof_on { _frame_end_cpu_us = float(ticks() - _t_end) / 1000.0 }
   _last_frame_begin_cpu_us = _frame_begin_cpu_us
   _last_frame_end_cpu_us = _frame_end_cpu_us
   _last_frame_flush_cpu_us = _frame_flush_cpu_us
   _last_frame_sync_pc_cpu_us = _frame_sync_pc_cpu_us
   if _deep_on { _vk_deep_end_total_ms = ui_profile.elapsed_ms(_t_end_deep) }
   if ui_profile.trace_process_enabled() {
      mut proc_every_frames = ui_profile.env_int_cached("NY_TRACE_PROC_EVERY_FRAMES", 120, 1, 1000000)
      ;; Keep NY_TRACE=1 cheap even if the trace layer/cache resolves the proc
      ;; cadence incorrectly.  The renderer gates before calling into /proc, so
      ;; the sampler cannot become the allocation/perf problem.
      if !common.env_present("NY_TRACE_PROC_EVERY_FRAMES") && common.env_present("NY_TRACE") {
         def tm = common.env_lower("NY_TRACE")
         if tm == "spam" || tm == "3" { proc_every_frames = 1 }
         elif tm == "deep" || tm == "2" || tm == "full" || tm == "verbose" { proc_every_frames = 30 }
         else { proc_every_frames = 120 }
      }
      if _total_frames <= 2 || proc_every_frames <= 1 || _proc_trace_next_sample_frame_vk <= 0 || _total_frames >= _proc_trace_next_sample_frame_vk {
         if ui_profile.trace_process_sample(_total_frames, "vk-end") {
            _proc_trace_next_sample_frame_vk = _total_frames + proc_every_frames
         }
      }
   }
   if _deep_on && _vk_deep_should_emit(_total_frames) {
      ui_profile.print_text("[vk:end] frame=" + to_str(_total_frames) +
         " draws=" + to_str(_frame_draw_calls) +
         " verts=" + to_str(_vertex_offset / _VKR_VERT_STRIDE) +
         " present=" + to_str(present) +
         " acq_ms=" + to_str(_vk_deep_begin_acquire_ms) +
         " wait_ms=" + to_str(_vk_deep_begin_wait_ms) +
         " resetf_ms=" + to_str(_vk_deep_begin_reset_fence_ms) +
         " begin_cmd_ms=" + to_str(_vk_deep_begin_cmd_ms) +
         " begin_rp_ms=" + to_str(_vk_deep_begin_rp_ms) +
         " vp_ms=" + to_str(_vk_deep_begin_vp_ms) +
         " flush_ms=" + to_str(_vk_deep_end_flush_ms) +
         " end_cmd_ms=" + to_str(_vk_deep_end_cmd_ms) +
         " submit_ms=" + to_str(_vk_deep_end_submit_ms) +
         " present_ms=" + to_str(_vk_deep_end_present_ms) +
         " begin_total_ms=" + to_str(_vk_deep_begin_total_ms) +
         " end_total_ms=" + to_str(_vk_deep_end_total_ms) +
         " flush_tex=" + to_str(_flush_reason_tex) +
         " flush_pipe=" + to_str(_flush_reason_pipe) +
         " flush_static=" + to_str(_flush_reason_static) +
         " flush_special=" + to_str(_flush_reason_special) +
      " flush_full=" + to_str(_flush_reason_vertex_full))
   }
   if ui_profile.dump_trace_enabled() && _total_frames < 8 {
      ui_profile.print_text("[vk:dump] frame=" + to_str(_total_frames) +
         " draws=" + to_str(_frame_draw_calls) +
         " dyn=" + to_str(_frame_dynamic_draw_calls) +
         " static=" + to_str(_frame_static_draw_calls) +
         " indexed=" + to_str(_frame_indexed_draw_calls) +
         " verts=" + to_str(_vertex_offset / _VKR_VERT_STRIDE) +
         " img=" + to_str(_image_index) +
         " present=" + to_str(present) +
      " surface=" + to_str(has_surface))
   }
   if _prof_on && (_total_frames % _vk_profile_every()) == 0 {
      def _profile_surface = has_surface
      ui_profile.print_text("[vk:prof] frame=" + to_str(_total_frames) +
         " draws=" + to_str(_frame_draw_calls) +
         " dyn=" + to_str(_frame_dynamic_draw_calls) +
         " static=" + to_str(_frame_static_draw_calls) +
         " indexed=" + to_str(_frame_indexed_draw_calls) +
         " flush=" + to_str(_flush_total) +
         " tex=" + to_str(_flush_reason_tex) +
         " state=" + to_str(_flush_reason_pipe) +
         " static=" + to_str(_flush_reason_static) +
         " special=" + to_str(_flush_reason_special) +
         " full=" + to_str(_flush_reason_vertex_full) +
         " pipe=" + to_str(_pipeline_bind_count) +
         " ds=" + to_str(_descriptor_bind_count) +
         " begin_ms=" + to_str(float(_frame_begin_cpu_us) / 1000.0) +
         " syncpc_ms=" + to_str(float(_frame_sync_pc_cpu_us) / 1000.0) +
         " flush_ms=" + to_str(float(_frame_flush_cpu_us) / 1000.0) +
         " end_ms=" + to_str(float(_frame_end_cpu_us) / 1000.0) +
         " verts=" + to_str(_vertex_offset / _VKR_VERT_STRIDE) +
         " rectq=" + to_str(_prim_rect_quads) +
         " outlineq=" + to_str(_prim_outline_quads) +
         " lineq=" + to_str(_prim_line_quads) +
         " text=" + to_str(_prim_text_calls) + "/" + to_str(_prim_text_glyphs) +
      " surface=" + to_str(_profile_surface))
      if _vk_profile_dump_enabled() {
         def row = {
            "format": "nytrix.vk.profile.v1", "frame": _total_frames,
            "draws": _frame_draw_calls, "draws_dynamic": _frame_dynamic_draw_calls,
            "draws_static": _frame_static_draw_calls, "draws_indexed": _frame_indexed_draw_calls,
            "flush_total": _flush_total, "flush_tex": _flush_reason_tex,
            "flush_pipe": _flush_reason_pipe, "flush_static": _flush_reason_static,
            "flush_special": _flush_reason_special, "flush_vertex_full": _flush_reason_vertex_full,
            "pipeline_binds": _pipeline_bind_count, "descriptor_binds": _descriptor_bind_count,
            "begin_ms": float(_frame_begin_cpu_us) / 1000.0,
            "syncpc_ms": float(_frame_sync_pc_cpu_us) / 1000.0,
            "flush_ms": float(_frame_flush_cpu_us) / 1000.0,
            "end_ms": float(_frame_end_cpu_us) / 1000.0,
            "deep_recreate_ms": _vk_deep_begin_recreate_ms,
            "deep_acquire_ms": _vk_deep_begin_acquire_ms,
            "deep_wait_ms": _vk_deep_begin_wait_ms,
            "deep_reset_fence_ms": _vk_deep_begin_reset_fence_ms,
            "deep_begin_cmd_ms": _vk_deep_begin_cmd_ms,
            "deep_begin_rp_ms": _vk_deep_begin_rp_ms,
            "deep_vp_ms": _vk_deep_begin_vp_ms,
            "deep_begin_total_ms": _vk_deep_begin_total_ms,
            "deep_end_flush_ms": _vk_deep_end_flush_ms,
            "deep_end_cmd_ms": _vk_deep_end_cmd_ms,
            "deep_submit_ms": _vk_deep_end_submit_ms,
            "deep_present_ms": _vk_deep_end_present_ms,
            "deep_end_total_ms": _vk_deep_end_total_ms,
            "verts": _vertex_offset / _VKR_VERT_STRIDE,
            "prim_rect_quads": _prim_rect_quads,
            "prim_outline_quads": _prim_outline_quads,
            "prim_line_quads": _prim_line_quads,
            "prim_raw_lines": _prim_raw_lines,
            "prim_raw_points": _prim_raw_points,
            "prim_text_calls": _prim_text_calls,
            "prim_text_glyphs": _prim_text_glyphs,
            "surface": _profile_surface, "present": present && _profile_surface
         }
         ui_profile.append_line(_vk_profile_dump_file(), json_encode(row))
      }
   }
   _frame_open = false
   _current_frame = (_current_frame + 1) % _frames_in_flight()
   true
}

fn clear(any r, any g, any b, any a) any {
   "Commands the GPU to clear the current color attachment."
   if !_frame_open { return 0 }
   _flush()
   if _clear_ca == 0 { _clear_ca = _renderer_alloc(24) _clear_rect = _renderer_alloc(24) }
   def cb = load64(_cmd_bufs_slab, _current_frame * 8)
   store32(_clear_ca, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(_clear_ca, 0, 4)
   store32_f32(_clear_ca, _clear_color_channel(r), 8)
   store32_f32(_clear_ca, _clear_color_channel(g), 12)
   store32_f32(_clear_ca, _clear_color_channel(b), 16)
   store32_f32(_clear_ca, a, 20)
   store32(_clear_rect, 0, 0) store32(_clear_rect, 0, 4)
   store32(_clear_rect, _swapchain_extent_w, 8) store32(_clear_rect, _swapchain_extent_h, 12)
   store32(_clear_rect, 0, 16)
   store32(_clear_rect, 1, 20)
   cmd_clear_attachments(cb, 1, _clear_ca, 1, _clear_rect)
   0
}

fn clear_depth() any {
   "Clears the depth buffer, ensuring subsequent depth passes render correctly over past layers."
   if !_frame_open { return 0 }
   _flush()
   if _clear_ca == 0 { _clear_ca = _renderer_alloc(24) _clear_rect = _renderer_alloc(24) }
   def cb = load64(_cmd_bufs_slab, _current_frame * 8)
   store32(_clear_ca, 2, 0)
   store32(_clear_ca, 0, 4)
   store32_f32(_clear_ca, 1.0, 8)
   store32(_clear_ca, 0, 12)
   store32(_clear_rect, 0, 0) store32(_clear_rect, 0, 4)
   store32(_clear_rect, _swapchain_extent_w, 8) store32(_clear_rect, _swapchain_extent_h, 12)
   store32(_clear_rect, 0, 16)
   store32(_clear_rect, 1, 20)
   cmd_clear_attachments(cb, 1, _clear_ca, 1, _clear_rect)
   0
}

fn set_clear_color(any r, any g, any b, any a=1.0) any {
   "Sets the clear color for the next begin_frame."
   _clear_r, _clear_g, _clear_b, _clear_a = float(r), float(g), float(b), float(a)
   0
}

fn _destroy_surface_storage() any {
   if _surface != 0 {
      def surf = _raw_surface_handle()
      if surf != 0 && surf != 0x8000000000 { destroy_surface_khr(_instance, surf, 0) }
      free(_surface)
      _surface = 0
      _surface_handle = 0
   }
   0
}

fn _destroy_depth_msaa_resources() any {
   if _depth_view != 0 { destroy_image_view(_device, _depth_view, 0) _depth_view = 0 }
   if _depth_image != 0 { destroy_image(_device, _depth_image, 0) _depth_image = 0 }
   if _depth_memory { free_memory(_device, _depth_memory, 0) _depth_memory = 0 }
   if _msaa_color_view { destroy_image_view(_device, _msaa_color_view, 0) _msaa_color_view = 0 }
   if _msaa_color_image { destroy_image(_device, _msaa_color_image, 0) _msaa_color_image = 0 }
   if _msaa_color_memory { free_memory(_device, _msaa_color_memory, 0) _msaa_color_memory = 0 }
   0
}

fn _destroy_sync_objects() any {
   if _device == 0 { return 0 }
   mut i = 0
   while i < _image_available_semaphores_count {
      def sem = _image_available_semaphores.get(i, 0)
      if sem { destroy_semaphore(_device, sem, 0) }
      i += 1
   }
   i = 0
   while i < _render_finished_semaphores_count {
      def sem = _render_finished_semaphores.get(i, 0)
      if sem { destroy_semaphore(_device, sem, 0) }
      i += 1
   }
   i = 0
   while i < _in_flight_fences_count {
      def fence = _in_flight_fences.get(i, 0)
      if fence { destroy_fence(_device, fence, 0) }
      i += 1
   }
   _image_available_semaphores = []
   _render_finished_semaphores = []
   _in_flight_fences = []
   _image_available_semaphores_count = 0
   _render_finished_semaphores_count = 0
   _in_flight_fences_count = 0
   if _sem_avail_slab { free(_sem_avail_slab) _sem_avail_slab = 0 }
   if _sem_finish_slab { free(_sem_finish_slab) _sem_finish_slab = 0 }
   if _fences_slab { free(_fences_slab) _fences_slab = 0 }
   0
}

fn _reset_texture_descriptor_state() any {
   _bindless_ds = 0
   _default_texture = -1
   _default_normal_texture = -1
   _default_black_texture = -1
   _default_sampler = 0
   _nearest_sampler = 0
   _linear_sampler = 0
   _sampler_cache = dict(16)
   _textures = []
   _texture_ds_cache = []
   _material_ds_cache = dict(64)
   _free_texture_ids = []
   _bindless_overflow_warned = false
   _current_texture_id = -1
   _current_tex_index = 0
   _batch_texture_id = -1
   _batch_tex_index = 0
   _current_env_tex_id = -1
   _current_env_spec_tex_id = -1
   _current_base_tex_id = -1
   _current_normal_tex_id = -1
   _current_emissive_tex_id = -1
   _current_occlusion_tex_id = -1
   _current_ext2_tex_word = 0x80000000
   _current_material_key = 0
   _current_mat_slab_ptr = 0
   _last_bound_tex_id = -1
   _last_bound_ds = 0
   _last_bound_ubo_ds = 0
   _skybox_ds_cache = 0
   _skybox_ds_tex_id = -1
   0
}

fn shutdown() any {
   "Shuts down the Vulkan renderer and releases all associated resources."
   _handles_valid = false
   if _device == 0 {
      _destroy_surface_storage()
      if _instance { destroy_instance(_instance, 0) }
      _instance = 0
      _physical_device = 0
      _device = 0
      _surface = 0
      _surface_handle = 0
      _graphics_queue = 0
      _present_queue = 0
      _swapchain = 0
      _render_pass = 0
      _render_pass_load = 0
      _render_pass_load_color_clear_depth = 0
      _window_ref = 0
      if _scene_light_tmp_slab { free(_scene_light_tmp_slab) }
      _scene_light_tmp_slab = 0
      if _current_mat_slab_cache { free(_current_mat_slab_cache) }
      _current_mat_slab_cache = 0
      _current_mat_slab_ptr = 0
      return 0
   }
   device_wait_idle(_device)
   _destroy_scene_color_capture_tex()
   _destroy_swapchain_objects()
   _destroy_depth_msaa_resources()
   _destroy_sync_objects()
   if _vertex_buffer != 0 { destroy_buffer(_device, _vertex_buffer, 0) }
   if _ubo_buffer != 0 { destroy_buffer(_device, _ubo_buffer, 0) }
   if _vertex_memory != 0 { free_memory(_device, _vertex_memory, 0) }
   if _ubo_memory != 0 { free_memory(_device, _ubo_memory, 0) }
   if _staging_buffer != 0 { destroy_buffer(_device, _staging_buffer, 0) }
   if _staging_memory { free_memory(_device, _staging_memory, 0) }
   if _nearest_sampler { destroy_sampler(_device, _nearest_sampler, 0) }
   if _linear_sampler && _linear_sampler != _nearest_sampler { destroy_sampler(_device, _linear_sampler, 0) }
   if _descriptor_pool { destroy_descriptor_pool(_device, _descriptor_pool, 0) }
   if _scene_capture_slab { free(_scene_capture_slab) }
   _scene_capture_slab = 0
   _scene_capture_src_bar = 0
   _scene_capture_dst_bar = 0
   _scene_capture_copy_region = 0
   if _present_copy_slab { free(_present_copy_slab) }
   _present_copy_slab = 0
   _present_dst_bar = 0
   _present_copy_region = 0
   if _readback_slab { free(_readback_slab) }
   _readback_slab = 0
   _readback_ai = 0
   _readback_cb_p = 0
   _readback_bi = 0
   _readback_barrier = 0
   _readback_region = 0
   _readback_s_info = 0
   _capture_request = false
   _capture_ready = false
   if _capture_pixels { free(_capture_pixels) }
   _capture_pixels = 0
   _capture_w = 0
   _capture_h = 0
   if _scene_light_tmp_slab { free(_scene_light_tmp_slab) }
   _scene_light_tmp_slab = 0
   if _current_mat_slab_cache { free(_current_mat_slab_cache) }
   _current_mat_slab_cache = 0
   _current_mat_slab_ptr = 0
   mut i = 0
   def textures_n = _textures.len
   while i < textures_n {
      def tex = _textures.get(i)
      def view = tex.get("view", 0)
      def img = tex.get("image", 0)
      def mem = tex.get("memory", 0)
      if view { destroy_image_view(_device, view, 0) }
      if img { destroy_image(_device, img, 0) }
      if mem { free_memory(_device, mem, 0) }
      i += 1
   }
   _textures = []
   _texture_ds_cache = []
   _free_texture_ids = []
   if _texture_fmt_cache { free(_texture_fmt_cache) }
   _texture_fmt_cache = 0
   _reset_texture_descriptor_state()
   if _device != 0 { destroy_device(_device, 0) }
   _destroy_surface_storage()
   if _instance { destroy_instance(_instance, 0) }
   _instance = 0
   _physical_device = 0
   _device = 0
   _graphics_queue = 0
   _present_queue = 0
   _surface = 0
   _surface_handle = 0
   _swapchain = 0
   _render_pass = 0
   _render_pass_load = 0
   _render_pass_load_color_clear_depth = 0
   _command_pool = 0
   _descriptor_pool = 0
   _default_sampler = 0
   _nearest_sampler = 0
   _linear_sampler = 0
   _descriptor_set_layout = 0
   _descriptor_set_layout_ubo = 0
   _pipeline_layout = 0
   _pipeline = 0
   _nocull_pipeline = 0
   _unlit_pipeline = 0
   _mesh_opaque_pipeline = 0
   _mesh_opaque_nocull_pipeline = 0
   _mesh_opaque_nocull_flip_pipeline = 0
   _mesh_opaque_unlit_pipeline = 0
   _mesh_opaque_unlit_nocull_pipeline = 0
   _mesh_opaque_unlit_nocull_flip_pipeline = 0
   _mesh_alpha_pipeline = 0
   _mesh_alpha_nocull_pipeline = 0
   _mesh_alpha_nocull_flip_pipeline = 0
   _mesh_alpha_unlit_pipeline = 0
   _mesh_alpha_unlit_nocull_pipeline = 0
   _mesh_alpha_unlit_nocull_flip_pipeline = 0
   _mesh_alpha_flip_pipeline = 0
   _mesh_alpha_unlit_flip_pipeline = 0
   _mesh_fast_opaque_pipeline = 0
   _mesh_fast_opaque_nocull_pipeline = 0
   _mesh_fast_opaque_flip_pipeline = 0
   _mesh_fast_opaque_nocull_flip_pipeline = 0
   _mesh_fast_env_opaque_pipeline = 0
   _mesh_fast_env_opaque_nocull_pipeline = 0
   _mesh_fast_env_opaque_flip_pipeline = 0
   _mesh_fast_env_opaque_nocull_flip_pipeline = 0
   _line_pipeline = 0
   _sdf_line_pipeline = 0
   _wire_pipeline = 0
   _circle_pipeline = 0
   _ring_pipeline = 0
   _rounded_rect_pipeline = 0
   _vert_module = 0
   _frag_module = 0
   _frag_fast_module = 0
   _frag_fast_env_module = 0
   _window_ref = 0
   0
}

fn set_wireframe(bool enabled) any {
   "Enables or disables wireframe rendering globally."
   if enabled && !_wire_pipeline { _ = _ensure_wire_pipeline() }
   _is_wireframe = !!enabled
   if _vertex_offset != _last_flush_offset { _flush() }
   0
}

fn request_frame_capture() any {
   "Requests a staging-buffer readback to be recorded for the current frame."
   _capture_request = true
   0
}

fn _ensure_present_copy_slab() bool {
   if !_present_copy_slab {
      _present_copy_slab = _renderer_alloc(160)
      if !_present_copy_slab { return false }
      memset(_present_copy_slab, 0, 160)
      _present_dst_bar = _present_copy_slab
      _present_copy_region = _present_copy_slab + 72
   }
   _present_dst_bar != 0 && _present_copy_region != 0
}

fn _store_present_copy_region(any region, int w, int h) any {
   memset(region, 0, 88)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(region, 0, 4)
   store32(region, 0, 8)
   store32(region, 1, 12)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 28)
   store32(region, 0, 32)
   store32(region, 0, 36)
   store32(region, 1, 40)
   store32(region, w, 56)
   store32(region, h, 60)
   store32(region, 1, 64)
   0
}

fn _store_present_blit_region(any region, int src_w, int src_h, int dst_w, int dst_h) any {
   memset(region, 0, 88)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(region, 0, 4)
   store32(region, 0, 8)
   store32(region, 1, 12)
   store32(region, src_w, 28)
   store32(region, src_h, 32)
   store32(region, 1, 36)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 40)
   store32(region, 0, 44)
   store32(region, 0, 48)
   store32(region, 1, 52)
   store32(region, dst_w, 68)
   store32(region, dst_h, 72)
   store32(region, 1, 76)
   0
}

fn _record_draw_image_to_swapchain(any cb, bool has_surface) bool {
   if !_offscreen_draw_enabled() { return true }
   if !_ensure_present_copy_slab() { return false }
   def src_image = _draw_images.get(_image_index, 0)
   def dst_image = _swapchain_images.get(_image_index, 0)
   if !_handle_ok(src_image) || !_handle_ok(dst_image) { return false }
   def dst_final_layout = has_surface ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
   def dst_bar = _present_dst_bar
   def region = _present_copy_region
   VkImageMemoryBarrierColor(dst_bar,
      dst_image,
      0,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED,
   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
   cmd_pipeline_barrier(cb,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0,
      0,
      0,
      0,
      0,
      1,
   dst_bar)
   if _draw_image_format == _swapchain_format
   && _draw_extent_w == _swapchain_extent_w
   && _draw_extent_h == _swapchain_extent_h{
      _store_present_copy_region(region, _swapchain_extent_w, _swapchain_extent_h)
      cmd_copy_image(cb,
         src_image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         dst_image,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         1,
      region)
   } else {
      _store_present_blit_region(region, _draw_extent_w, _draw_extent_h, _swapchain_extent_w, _swapchain_extent_h)
      cmd_blit_image(cb,
         src_image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         dst_image,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         1,
         region,
      VK_FILTER_LINEAR)
   }
   VkImageMemoryBarrierColor(dst_bar,
      dst_image,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      0,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   dst_final_layout)
   cmd_pipeline_barrier(cb,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      has_surface ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0,
      0,
      0,
      0,
      0,
      1,
   dst_bar)
   true
}

fn _create_instance() bool {
   def exts_list = native.required_extensions()
   mut e_count = 0
   mut e_ptrs = 0
   if is_list(exts_list) && exts_list.len >= 2 {
      e_count = int(exts_list.get(0, 0))
      e_ptrs = exts_list.get(1, 0)
      if e_count < 0 || !e_ptrs {
         e_count = 0
         e_ptrs = 0
      }
   }
   if vk_state._debug_gfx_enabled {
      ui_profile.print_text("[gfx:vulkan] required instance extensions=" + to_str(e_count))
      mut ei = 0
      while ei < e_count {
         ui_profile.print_text("[gfx:vulkan]   ext " + text.cstr_to_str(load64(e_ptrs, ei * 8)))
         ei += 1
      }
   }
   mut debug_utils_ok = false
   mut portability_enum_ok = false
   mut properties2_ok = false
   mut ext_count_ptr = _renderer_alloc(4)
   store32(ext_count_ptr, 0, 0)
   if enumerate_instance_extension_properties(0, ext_count_ptr, 0) == 0 {
      def ext_count = load32(ext_count_ptr, 0)
      if ext_count > 0 {
         mut ext_props = _renderer_alloc(ext_count * 260)
         if enumerate_instance_extension_properties(0, ext_count_ptr, ext_props) == 0 {
            mut i = 0
            while i < ext_count {
               def name = text.cstr_to_str(ext_props + i * 260)
               if name && eq(name, "VK_EXT_debug_utils") { debug_utils_ok = true }
               elif name && eq(name, "VK_KHR_portability_enumeration") { portability_enum_ok = true }
               elif name && eq(name, "VK_KHR_get_physical_device_properties2") { properties2_ok = true }
               i += 1
            }
         }
         free(ext_props)
      }
   }
   free(ext_count_ptr)
   mut extra_exts = 0
   if debug_utils_ok { extra_exts += 1 }
   if portability_enum_ok { extra_exts += 1 }
   if properties2_ok { extra_exts += 1 }
   mut all_ext_ptrs = 0
   if e_count + extra_exts > 0 { all_ext_ptrs = _renderer_alloc((e_count + extra_exts) * 8) }
   mut i = 0 while i < e_count {
      store64_h(all_ext_ptrs, load64_h(e_ptrs, i * 8), i * 8)
      i += 1
   }
   if debug_utils_ok {
      mut debug_ext_name = _renderer_alloc(32) strcpy(debug_ext_name, "VK_EXT_debug_utils")
      store64_h(all_ext_ptrs, debug_ext_name, e_count * 8)
      e_count += 1
   } else {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] ext VK_EXT_debug_utils unavailable") }
   }
   if portability_enum_ok {
      mut portability_ext_name = _renderer_alloc(48) strcpy(portability_ext_name, "VK_KHR_portability_enumeration")
      store64_h(all_ext_ptrs, portability_ext_name, e_count * 8)
      e_count += 1
   }
   if properties2_ok {
      mut properties2_ext_name = _renderer_alloc(56) strcpy(properties2_ext_name, "VK_KHR_get_physical_device_properties2")
      store64_h(all_ext_ptrs, properties2_ext_name, e_count * 8)
      e_count += 1
   }
   mut layer_ptrs = 0
   mut layer_count = 0
   if _vk_validation_enabled() {
      mut lc_vptr = _renderer_alloc(4)
      store32(lc_vptr, 0, 0)
      if enumerate_instance_layer_properties(lc_vptr, 0) == 0 {
         def lc_v = load32(lc_vptr, 0)
         if lc_v > 0 {
            mut lprops_v = _renderer_alloc(lc_v * 520)
            if enumerate_instance_layer_properties(lc_vptr, lprops_v) == 0 {
               mut found_val = false
               mut jv = 0
               while jv < lc_v && !found_val {
                  def lname = text.cstr_to_str(lprops_v + jv * 520)
                  if lname && eq(lname, "VK_LAYER_KHRONOS_validation") { found_val = true }
                  jv += 1
               }
               if found_val {
                  mut val_name = _renderer_alloc(32)
                  strcpy(val_name, "VK_LAYER_KHRONOS_validation")
                  mut new_lptrs = _renderer_alloc((layer_count + 1) * 8)
                  mut ci = 0
                  while ci < layer_count {
                     store64_h(new_lptrs, load64_h(layer_ptrs, ci * 8), ci * 8)
                     ci += 1
                  }
                  store64_h(new_lptrs, val_name, layer_count * 8)
                  if layer_ptrs { free(layer_ptrs) }
                  layer_ptrs = new_lptrs
                  layer_count += 1
                  ui_profile.print_text("[gfx:vulkan] validation layer ENABLED(NY_VK_VALIDATION=1) — stderr for VK errors")
               } else {
                  ui_profile.print_text("[gfx:vulkan] NY_VK_VALIDATION=1 but VK_LAYER_KHRONOS_validation not installed — install vulkan-validation-layers")
               }
            }
            free(lprops_v)
         }
      }
      free(lc_vptr)
   }
   mut instance = 0
   mut tried_compat_retry = false
   mut last_res_code = 0
   mut api_versions = [0x00403000, 0x00402000, 0x00401000, 0x00400000]
   mut v_i = 0
   def api_versions_n = api_versions.len
   while v_i < api_versions_n {
      def api_version = api_versions.get(v_i)
      mut app_info = _renderer_alloc(48)
      store32(app_info, VK_STRUCTURE_TYPE_APPLICATION_INFO, 0)
      store64_h(app_info, 0, 8)
      store64_h(app_info, 0, 16)
      store32(app_info, 1, 24)
      store64_h(app_info, 0, 32)
      store32(app_info, 1, 40)
      store32(app_info, api_version, 44)
      mut create_info = _renderer_alloc(64)
      store32(create_info, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0)
      store32(create_info, portability_enum_ok ? 1 : 0, 16)
      mut validation_features = 0
      mut disabled_validation_features = 0
      if vk_state._debug_gfx_enabled && layer_count > 0 {
         disabled_validation_features = _renderer_alloc(4)
         store32(disabled_validation_features, 0, 0)
         validation_features = _renderer_alloc(48)
         memset(validation_features, 0, 48)
         store32(validation_features, 1000247000, 0)
         store32(validation_features, 1, 32)
         store64_h(validation_features, disabled_validation_features, 40)
         store64_h(create_info, validation_features, 8)
      }
      store64_h(create_info, app_info, 24)
      store32(create_info, layer_count, 32)
      store64_h(create_info, layer_ptrs, 40)
      store32(create_info, e_count, 48)
      store64_h(create_info, all_ext_ptrs, 56)
      mut inst_ptr = _renderer_alloc(8)
      store32(inst_ptr, 0, 0) store32(inst_ptr, 0, 4)
      def res_tagged = vk_create_instance(create_info, 0, inst_ptr)
      def res_code = vk_result_code(res_tagged)
      def inst_after = load64(inst_ptr, 0)
      if res_code == 0 && inst_after != 0 {
         instance = inst_after
         free(inst_ptr)
         if validation_features { free(validation_features) }
         if disabled_validation_features { free(disabled_validation_features) }
         free(create_info, app_info)
         if instance {
            if vk_state._debug_gfx_enabled && tried_compat_retry { ui_profile.print_text("[gfx:vulkan] instance api_compat_fallback=true") }
            _instance = instance
            if vk_state._debug_gfx_enabled {
               def rawp = _renderer_alloc(8)
               store64(rawp, _instance, 0)
               ui_profile.print_text("[gfx:vulkan] instance raw_dec=" + to_str(load64(rawp, 0)))
               free(rawp)
               _dbg_handle("instance", _instance)
            }
            return true
         }
         ui_profile.print_text("[gfx:vulkan] create_instance returned null instance")
         return false
      }
      free(inst_ptr)
      if validation_features { free(validation_features) }
      if disabled_validation_features { free(disabled_validation_features) }
      free(create_info, app_info)
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create_instance failed code=" + to_str(res_code) + " api=0x" + text.to_hex(api_version)) }
      last_res_code = res_code
      if res_code != -9 { return false }
      tried_compat_retry = true
      v_i += 1
   }
   if vk_state._debug_gfx_enabled && last_res_code == -9 { ui_profile.print_text("[gfx:vulkan] no compatible Vulkan ICD driver") }
   false
}

fn _create_surface(any win) bool {
   def no_surface_env = common.env_lower("NY_UI_BACKEND") == "none" || common.env_truthy("NY_UI_REAL_HEADLESS_SIM") || common.env_truthy("NY_UI_HEADLESS_SIM")
   if !win || no_surface_env || lib_uiw.backend() == "none" {
      if !_allow_headless_surface() {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] headless surface disabled no_window=true") }
         return false
      }
      _surface = _renderer_alloc(8)
      memset(_surface, 0, 8)
      store64(_surface, 0x8000000000, 0)
      _surface_handle = 0x8000000000
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] headless surface sentinel installed") }
      return true
   }
   if !win { return false }
   _surface = _renderer_alloc(8)
   store64(_surface, 0, 0)
   def res = native.create_surface(_instance, win, 0, _surface)
   if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] native.create_surface res=" + to_str(res) + " raw=0x" + text.to_hex(int(load64(_surface, 0)))) }
   if res != 0 {
      ui_profile.print_text("[gfx:vulkan] create_surface failed res=" + to_str(res))
      free(_surface)
      _surface = 0
      return false
   }
   def raw_surface = load64_h(_surface, 0)
   if raw_surface == 0 || raw_surface == 0x8000000000 {
      ui_profile.print_text("[gfx:vulkan] create_surface returned invalid handle=0x" + text.to_hex(int(raw_surface)))
      free(_surface)
      _surface = 0
      _surface_handle = 0
      return false
   }
   _surface_handle = load64_h(_surface, 0)
   if vk_state._debug_gfx_enabled { _dbg_handle("surface", _surface_handle) }
   true
}

fn _raw_surface_handle() any {
   if _surface_handle == 0x8000000000 { return _surface_handle }
   if _surface != 0 {
      def raw = load64_h(_surface, 0)
      if raw == 0 || raw == 0x8000000000 { return raw }
      if raw { return load64(_surface, 0) }
   }
   _surface_handle
}

fn _has_valid_surface() bool {
   if _surface_handle == 0x8000000000 { return false }
   def raw = _raw_surface_handle()
   raw != 0 && raw != 0x8000000000
}

@inline
fn _has_valid_surface_fast() bool { _surface_handle != 0 && _surface_handle != 0x8000000000 }

@inline
fn _pick_physical_device() bool {
   mut c_ptr = _renderer_alloc(4)
   store32(c_ptr, 0, 0)
   mut pd_res = enumerate_physical_devices(_instance, c_ptr, 0)
   if pd_res != 0 {
      ui_profile.print_text("[gfx:vulkan] vkEnumeratePhysicalDevices(count) failed code=" + to_str(pd_res))
      free(c_ptr)
      return false
   }
   def count = load32(c_ptr, 0)
   if count == 0 {
      ui_profile.print_text("[gfx:vulkan] vkEnumeratePhysicalDevices(count)=0")
      free(c_ptr)
      return false
   }
   mut p_ptr = _renderer_alloc(count * 8)
   pd_res = enumerate_physical_devices(_instance, c_ptr, p_ptr)
   if pd_res != 0 {
      ui_profile.print_text("[gfx:vulkan] vkEnumeratePhysicalDevices(devices) failed code=" + to_str(pd_res) + " count=" + to_str(count))
      free(c_ptr, p_ptr)
      return false
   }
   _physical_device = load64(p_ptr, 0)
   mut props = _renderer_alloc(1024)
   get_physical_device_properties(_physical_device, props)
   def device_name = text.cstr_to_str(props, 20)
   if vk_state._debug_gfx_enabled {
      def rawp = _renderer_alloc(8)
      store64(rawp, _physical_device, 0)
      ui_profile.print_text("[gfx:vulkan] physical raw_dec=" + to_str(load64(rawp, 0)))
      free(rawp)
      ui_profile.print_text("[gfx:vulkan] gpu=" + to_str(device_name))
      _dbg_handle("physical", _physical_device)
   }
   free(p_ptr, props)
   true
}

fn _physical_device_bda_supported() bool {
   if !_physical_device || common.env_truthy("NY_VK_BDA_OFF") { return false }
   def features2 = _renderer_alloc(256)
   def bda = _renderer_alloc(32)
   memset(features2, 0, 256)
   memset(bda, 0, 32)
   store32(features2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, 0)
   store32(bda, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, 0)
   store64_h(features2, bda, 8)
   get_physical_device_features2(_physical_device, features2)
   def ok = load32(bda, 16) != 0
   free(features2, bda)
   ok
}

fn _create_logical_device() bool {
   mut count_ptr = _renderer_alloc(4)
   store32(count_ptr, 0, 0)
   get_physical_device_queue_family_properties(_physical_device, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if count == 0 { return false }
   def prop_stride = 24
   mut props = _renderer_alloc(count * prop_stride)
   get_physical_device_queue_family_properties(_physical_device, count_ptr, props)
   mut graphics_family = -1
   mut present_family = -1
   mut support_ptr = _renderer_alloc(4)
   mut i = 0
   while i < count {
      def flags = load32(props, i * prop_stride)
      mut present_supported = true
      if _has_valid_surface() {
         def surf = _raw_surface_handle()
         store32(support_ptr, 0, 0)
         def ps = get_physical_device_surface_support_khr(_instance, _physical_device, i, surf, support_ptr)
         present_supported = (ps == 0) && (load32(support_ptr, 0) != 0)
         if present_supported && present_family == -1 { present_family = i }
      }
      if (flags & 1) != 0 {
         if graphics_family == -1 { graphics_family = i }
         if present_supported {
            graphics_family = i
            if present_family == -1 { present_family = i }
            break
         }
      }
      i += 1
   }
   if !_has_valid_surface() && present_family == -1 { present_family = graphics_family }
   if graphics_family == -1 { return false }
   if present_family == -1 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] no present-capable queue family for surface") }
      return false
   }
   _graphics_family_index = graphics_family
   _present_family_index = present_family
   mut priorities = _renderer_alloc(4)
   store32(priorities, 0x3f800000, 0)
   def queue_info_count = (graphics_family == present_family) ? 1 : 2
   mut queue_create_info = _renderer_alloc(queue_info_count * 56)
   store32(queue_create_info, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0)
   store32(queue_create_info, 0, 8) store32(queue_create_info, 0, 12)
   store32(queue_create_info, 0, 16)
   store32(queue_create_info, graphics_family, 20)
   store32(queue_create_info, 1, 24)
   store64_h(queue_create_info, priorities, 32)
   if queue_info_count == 2 {
      def q2 = queue_create_info + 56
      store32(q2, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0)
      store32(q2, 0, 8) store32(q2, 0, 12)
      store32(q2, 0, 16)
      store32(q2, present_family, 20)
      store32(q2, 1, 24)
      store64_h(q2, priorities, 32)
   }
   mut res = -1
   mut dev_ptr = _renderer_alloc(8)
   mut ext1 = _renderer_alloc(32)
   strcpy(ext1, "VK_KHR_swapchain")
   mut ext2, ext3 = _renderer_alloc(40), _renderer_alloc(40)
   strcpy(ext2, "VK_EXT_descriptor_indexing")
   strcpy(ext3, "VK_KHR_maintenance3")
   mut ext_ptrs = _renderer_alloc(24)
   mut create_info = _renderer_alloc(72)
   mut dev_features = _renderer_alloc(232)
   mut di_feat = _renderer_alloc(104)
   mut supported_features2 = _renderer_alloc(256)
   mut supported_di_feat = _renderer_alloc(104)
   mut bda_feat = 0
   memset(supported_features2, 0, 256)
   memset(supported_di_feat, 0, 104)
   store32(supported_features2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, 0)
   store32(supported_di_feat, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, 0)
   store64_h(supported_features2, supported_di_feat, 8)
   get_physical_device_features2(_physical_device, supported_features2)
   def supported_core = supported_features2 + 16
   def bindless_feature_supported =
   load32(supported_di_feat, 32) != 0 &&
   load32(supported_di_feat, 60) != 0 &&
   load32(supported_di_feat, 80) != 0 &&
   load32(supported_di_feat, 84) != 0 &&
   load32(supported_di_feat, 92) != 0
   def enable_bindless_features = bindless_feature_supported && !common.env_truthy("NY_VK_BINDLESS_OFF")
   def want_bda = _physical_device_bda_supported()
   if want_bda { bda_feat = _renderer_alloc(32) }
   store64_h(ext_ptrs, ext1, 0)
   store64_h(ext_ptrs, ext2, 8)
   store64_h(ext_ptrs, ext3, 16)
   memset(create_info, 0, 72)
   store32(create_info, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0)
   store32(create_info, 0, 8) store32(create_info, 0, 12)
   store32(create_info, 0, 16)
   store32(create_info, queue_info_count, 20)
   store64_h(create_info, queue_create_info, 24)
   store32(create_info, 0, 32)
   store32(create_info, 0, 40) store32(create_info, 0, 44)
   store32(create_info, enable_bindless_features ? 3 : 1, 48)
   store64_h(create_info, ext_ptrs, 56)
   store32(create_info, 0, 64) store32(create_info, 0, 68)
   memset(dev_features, 0, 232)
   if load32(supported_core, 52) != 0 { store32(dev_features, 1, 52) }
   if load32(supported_core, 60) != 0 { store32(dev_features, 1, 60) }
   if load32(supported_core, 136) != 0 { store32(dev_features, 1, 136) }
   if load32(supported_core, 156) != 0 { store32(dev_features, 1, 156) }
   if load32(supported_core, 160) != 0 { store32(dev_features, 1, 160) }
   store64_h(create_info, dev_features, 64)
   memset(di_feat, 0, 104)
   store32(di_feat, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, 0)
   if enable_bindless_features {
      store32(di_feat, 1, 32)
      store32(di_feat, 1, 60)
      store32(di_feat, 1, 80)
      store32(di_feat, 1, 84)
      store32(di_feat, 1, 92)
   }
   if bda_feat && enable_bindless_features {
      memset(bda_feat, 0, 32)
      store32(bda_feat, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, 0)
      store32(bda_feat, 1, 16)
      store64_h(di_feat, bda_feat, 8)
   }
   if enable_bindless_features { store64_h(create_info, di_feat, 8) }
   elif vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] descriptor indexing unavailable; creating basic swapchain device") }
   store32(dev_ptr, 0, 0) store32(dev_ptr, 0, 4)
   _bda_enabled = false
   res = create_device(_physical_device, create_info, 0, dev_ptr)
   if res != 0 && bda_feat {
      if vk_state._debug_gfx_enabled { ui_profile.print_text(f"[gfx:vulkan] BDA create_device failed code={res}; retrying without BDA") }
      store64_h(di_feat, 0, 8)
      store32(dev_ptr, 0, 0) store32(dev_ptr, 0, 4)
      res = create_device(_physical_device, create_info, 0, dev_ptr)
   } elif res == 0 && bda_feat && enable_bindless_features {
      _bda_enabled = true
   }
   if res != 0 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text(f"[gfx:vulkan] bindless create_device failed code={res}") }
      return false
   }
   _device = load64(dev_ptr, 0)
   if vk_state._debug_gfx_enabled { _dbg_handle("device", _device) }
   mut q_ptr = _renderer_alloc(8)
   store32(q_ptr, 0, 0) store32(q_ptr, 0, 4)
   get_device_queue(_device, graphics_family, 0, q_ptr)
   _graphics_queue = load64(q_ptr, 0)
   if vk_state._debug_gfx_enabled { _dbg_handle("queue", _graphics_queue) }
   if present_family == graphics_family { _present_queue = _graphics_queue } else {
      store32(q_ptr, 0, 0) store32(q_ptr, 0, 4)
      get_device_queue(_device, present_family, 0, q_ptr)
      _present_queue = load64(q_ptr, 0)
   }
   true
}

fn _choose_surface_format() list {
   mut count_ptr = _renderer_alloc(4)
   store32(count_ptr, 0, 0)
   def surf = _raw_surface_handle()
   if surf == 0 || surf == 0x8000000000 { return [VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR] }
   mut rc = get_physical_device_surface_formats_khr(_instance, _physical_device, surf, count_ptr, 0)
   if rc != 0 { return [VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR] }
   def count = load32(count_ptr, 0)
   if count <= 0 { return [VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR] }
   def stride = 8
   mut formats_ptr = _renderer_alloc(count * stride)
   rc = get_physical_device_surface_formats_khr(_instance, _physical_device, surf, count_ptr, formats_ptr)
   if rc != 0 { return [VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR] }
   mut chosen_format = load32(formats_ptr, 0)
   mut chosen_space = load32(formats_ptr, 4)
   mut i = 0
   while i < count {
      def off = i * stride
      def fmt = load32(formats_ptr, off)
      def cs = load32(formats_ptr, off + 4)
      if fmt == VK_FORMAT_B8G8R8A8_UNORM && cs == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR {
         chosen_format = fmt
         chosen_space = cs
         break
      }
      if fmt == 37 && cs == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR {
         chosen_format = fmt
         chosen_space = cs
      } elif fmt == 44 {
         chosen_format = fmt
         chosen_space = cs
      } elif fmt == 37 {
         chosen_format = fmt
         chosen_space = cs
      }
      i += 1
   }
   [chosen_format, chosen_space]
}

fn _choose_composite_alpha(int supported_flags, bool transparent=false) int {
   if transparent {
      if band(supported_flags, 2) { return 2 }
      if band(supported_flags, 4) { return 4 }
      if band(supported_flags, 8) { return 8 }
      if band(supported_flags, 1) { return 1 }
      return 1
   }
   if band(supported_flags, 1) { return 1 }
   if band(supported_flags, 2) { return 2 }
   if band(supported_flags, 4) { return 4 }
   if band(supported_flags, 8) { return 8 }
   1
}

fn _choose_present_mode() int {
   mut count_ptr = _renderer_alloc(4)
   def surf = _raw_surface_handle()
   if surf == 0 || surf == 0x8000000000 { return VK_PRESENT_MODE_FIFO_KHR }
   get_physical_device_surface_present_modes_khr(_instance, _physical_device, surf, count_ptr, 0)
   def count = load32(count_ptr, 0)
   mut modes_ptr = _renderer_alloc(count * 4)
   get_physical_device_surface_present_modes_khr(_instance, _physical_device, surf, count_ptr, modes_ptr)
   mut mailbox_supported = false
   mut immediate_supported = false
   mut i = 0
   while i < count {
      def mode = load32(modes_ptr, i * 4)
      if mode == VK_PRESENT_MODE_MAILBOX_KHR { mailbox_supported = true }
      if mode == VK_PRESENT_MODE_IMMEDIATE_KHR { immediate_supported = true }
      i += 1
   }
   free(count_ptr, modes_ptr)
   def forced = common.env_lower("NY_VK_PRESENT_MODE")
   if forced.len > 0 {
      if forced == "fifo" { return VK_PRESENT_MODE_FIFO_KHR }
      if forced == "mailbox" && mailbox_supported { return VK_PRESENT_MODE_MAILBOX_KHR }
      if forced == "immediate" && immediate_supported { return VK_PRESENT_MODE_IMMEDIATE_KHR }
   }
   if _cfg_present_policy == "immediate" || _cfg_present_policy == "unlimited" {
      if immediate_supported { return VK_PRESENT_MODE_IMMEDIATE_KHR }
      if mailbox_supported { return VK_PRESENT_MODE_MAILBOX_KHR }
      return VK_PRESENT_MODE_FIFO_KHR
   }
   if lib_uiw.backend() == "wayland" {
      return VK_PRESENT_MODE_FIFO_KHR
   }
   if lib_uiw.backend() == "win32" {
      if mailbox_supported { return VK_PRESENT_MODE_MAILBOX_KHR }
      if immediate_supported { return VK_PRESENT_MODE_IMMEDIATE_KHR }
      return VK_PRESENT_MODE_FIFO_KHR
   }
   if common.env_truthy("NYTRIX_FAST") {
      if immediate_supported { return VK_PRESENT_MODE_IMMEDIATE_KHR }
      if mailbox_supported { return VK_PRESENT_MODE_MAILBOX_KHR }
      return VK_PRESENT_MODE_FIFO_KHR
   }
   if _cfg_vsync { return VK_PRESENT_MODE_FIFO_KHR } else {
      if mailbox_supported { return VK_PRESENT_MODE_MAILBOX_KHR }
      if immediate_supported { return VK_PRESENT_MODE_IMMEDIATE_KHR }
      return VK_PRESENT_MODE_FIFO_KHR
   }
}

fn _create_headless_image(int w, int h) any {
   mut img_ci = _renderer_alloc(88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 1, 20)
   store32(img_ci, 37, 24)
   store32(img_ci, w, 28)
   store32(img_ci, h, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, 1, 40)
   store32(img_ci, 1, 44)
   store32(img_ci, 1, 48)
   store32(img_ci, 0, 52)
   store32(img_ci,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   56)
   store32(img_ci, 0, 60)
   store32(img_ci, 0, 80)
   mut p = _renderer_alloc(8)
   if create_image(_device, img_ci, 0, p) != 0 { return 0 }
   def img = load64_h(p, 0)
   if img == 0 { return 0 }
   mut req = _renderer_alloc(24)
   get_image_memory_requirements(_device, img, req)
   def mem_type = _find_memory_type(load32(req, 16), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
   mut ai = _renderer_alloc(64)
   store32(ai, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(ai, load64_h(req, 0), 16)
   store32(ai, mem_type, 24)
   if allocate_memory(_device, ai, 0, p) != 0 { return 0 }
   def mem = load64_h(p, 0)
   if mem == 0 { return 0 }
   if bind_image_memory(_device, img, mem, 0) != 0 { return 0 }
   img
}

fn _create_swapchain_with_old(any win, any old_sc) bool {
   _vk_stage("swapchain.with_old")
   _old_swapchain_hint = old_sc
   def ok = _create_swapchain(win)
   _old_swapchain_hint = 0
   ok
}

fn _surface_caps_need_windows_retry(any win, int cur_w, int cur_h, int min_w, int min_h, int max_w, int max_h) bool {
   #windows {
      if !win { return false }
      if cur_w == 0xFFFFFFFF && cur_h == 0xFFFFFFFF { return false }
      if cur_w <= 1 || cur_h <= 1 { return true }
      if min_w <= 1 || min_h <= 1 { return true }
      if max_w <= 1 || max_h <= 1 { return true }
      if cur_w > 32768 || cur_h > 32768 { return true }
   }
   #endif
   false
}

fn _surface_extent_usable(int w, int h) bool {
   if w <= 1 || h <= 1 { return false }
   #windows {
      if w > 32768 || h > 32768 { return false }
   }
   #endif
   true
}

fn _surface_bound_usable(int w, int h) bool {
   if w <= 1 || h <= 1 { return false }
   #windows {
      if w > 32768 || h > 32768 { return false }
   }
   #endif
   true
}

fn _swapchain_create_retry_limit() int {
   #windows { return 8 }
   #else { return 1 }
   #endif
}

fn _settle_window_for_swapchain(any win, int attempt) any {
   #windows {
      if win { lib_uiw.show(win) }
      lib_uiw.poll_events()
      msleep(16 + attempt * 8)
   }
   #endif
   0
}

fn _window_swapchain_extent(any win, int fallback_w, int fallback_h) list {
   mut w, h = fallback_w, fallback_h
   if win && is_dict(win) {
      w = int(win.get("w", w))
      h = int(win.get("h", h))
   }
   if win {
      def fb = lib_uiw.get_framebuffer_size(win)
      if is_list(fb) && fb.len >= 2 {
         def fw, fh = int(fb.get(0, 0)), int(fb.get(1, 0))
         if fw > 1 && fh > 1 { return [fw, fh] }
      }
      def sz = lib_uiw.size(win)
      if is_list(sz) && sz.len >= 2 {
         def sw, sh = int(sz.get(0, 0)), int(sz.get(1, 0))
         if sw > 1 && sh > 1 { return [sw, sh] }
      }
   }
   if w <= 1 { w = fallback_w }
   if h <= 1 { h = fallback_h }
   [w, h]
}

fn _create_swapchain(any win) bool {
   _vk_stage("swapchain.enter")
   if !_has_valid_surface() {
      _vk_stage("swapchain.headless")
      if !_allow_headless_surface() {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] headless swapchain disabled no_surface=true") }
         return false
      }
      _swapchain_extent_w, _swapchain_extent_h = 400, 300
      if win { _swapchain_extent_w, _swapchain_extent_h = win.get("w", 400), win.get("h", 300) }
      _swapchain_format = 37
      _swapchain_image_count = 3
      _swapchain_images = []
      mut i = 0
      while i < 3 {
         def img = _create_headless_image(_swapchain_extent_w, _swapchain_extent_h)
         if img == 0 {
            if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] headless image creation failed index=" + to_str(i)) }
            _swapchain_images = []
            _swapchain_image_count = 0
            return false
         }
         _swapchain_images = _swapchain_images.append(img)
         i += 1
      }
      _swapchain = _headless_swapchain_sentinel()
      return true
   }
   _vk_stage("swapchain.caps")
   mut caps = _renderer_alloc(128)
   def surf = _raw_surface_handle()
   if surf == 0 || surf == 0x8000000000 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] invalid surface handle before swapchain creation") }
      return false
   }
   mut caps_res = 0
   mut caps_attempt = 0
   mut cur_w, cur_h = 0, 0
   mut min_w, min_h = 0, 0
   mut max_w, max_h = 0, 0
   while true {
      caps_res = get_physical_device_surface_capabilities_khr(_instance, _physical_device, surf, caps)
      if caps_res != 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] get_surface_capabilities failed code=" + to_str(caps_res)) }
         return false
      }
      cur_w, cur_h = load32(caps, 8), load32(caps, 12)
      min_w, min_h = load32(caps, 16), load32(caps, 20)
      max_w, max_h = load32(caps, 24), load32(caps, 28)
      if !_surface_caps_need_windows_retry(win, cur_w, cur_h, min_w, min_h, max_w, max_h) { break }
      caps_attempt += 1
      if caps_attempt >= 20 { break }
      if caps_attempt == 1 { lib_uiw.show(win) }
      lib_uiw.poll_events()
      msleep(16)
   }
   def req_extent = _window_swapchain_extent(win, 400, 300)
   mut req_w, req_h = int(req_extent.get(0, 400)), int(req_extent.get(1, 300))
   mut w, h = req_w, req_h
   if cur_w != 0xFFFFFFFF && cur_h != 0xFFFFFFFF && _surface_extent_usable(cur_w, cur_h) {
      w, h = cur_w, cur_h
   } else {
      if _surface_bound_usable(min_w, min_h) {
         if w < min_w { w = min_w }
         if h < min_h { h = min_h }
      }
      if _surface_bound_usable(max_w, max_h) {
         if w > max_w { w = max_w }
         if h > max_h { h = max_h }
      }
   }
   if w <= 1 || h <= 1 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] swapchain extent invalid after caps retry cur=" + to_str(cur_w) + "x" + to_str(cur_h) + " min=" + to_str(min_w) + "x" + to_str(min_h) + " max=" + to_str(max_w) + "x" + to_str(max_h) + " choose=" + to_str(w) + "x" + to_str(h)) }
      return false
   }
   _swapchain_extent_w, _swapchain_extent_h = w, h
   mut min_imgs = load32(caps, 0)
   mut max_imgs = load32(caps, 4)
   mut count = min_imgs + 1
   if max_imgs > 0 && count > max_imgs { count = max_imgs }
   def pre_transform = load32(caps, 40)
   def supported_alpha = load32(caps, 44)
   _vk_stage("swapchain.alpha")
   def want_transparent = win && band(int(win.get("flags", 0)), WINDOW_TRANSPARENT)
   def composite_alpha = _choose_composite_alpha(supported_alpha, want_transparent)
   if vk_state._debug_gfx_enabled && want_transparent && composite_alpha == 1 { ui_profile.print_text("[gfx:vulkan] transparent requested alpha_modes=opaque_only") }
   _vk_stage("swapchain.format")
   def sf = _choose_surface_format()
   def sfmt = sf.get(0, VK_FORMAT_B8G8R8A8_UNORM)
   def scol = sf.get(1, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
   _vk_stage("swapchain.present_mode")
   def present_mode = _choose_present_mode()
   if vk_state._debug_gfx_enabled {
      ui_profile.print_text("[gfx:vulkan] swapchain caps minImgs=" + to_str(min_imgs) +
         " maxImgs=" + to_str(max_imgs) +
         " cur=" + to_str(cur_w) + "x" + to_str(cur_h) +
         " min=" + to_str(min_w) + "x" + to_str(min_h) +
         " max=" + to_str(max_w) + "x" + to_str(max_h) +
         " choose=" + to_str(w) + "x" + to_str(h) +
         " alphaFlags=" + to_str(supported_alpha) +
         " alpha=" + to_str(composite_alpha) +
         " transform=" + to_str(pre_transform) +
         " fmt=" + to_str(sfmt) +
         " cs=" + to_str(scol) +
      " present=" + to_str(present_mode))
   }
   _vk_stage("swapchain.create_info")
   mut create_info = _renderer_alloc(128)
   store32(create_info, VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0)
   store64_h(create_info, surf, 24)
   store32(create_info, count, 32)
   _swapchain_format = sfmt
   store32(create_info, _swapchain_format, 36)
   store32(create_info, scol, 40)
   store32(create_info, w, 44)
   store32(create_info, h, 48)
   store32(create_info, 1, 52)
   store32(create_info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 2, 56)
   if _graphics_family_index != _present_family_index {
      def qfi = _renderer_alloc(8)
      store32(qfi, _graphics_family_index, 0)
      store32(qfi, _present_family_index, 4)
      store32(create_info, 1, 60)
      store32(create_info, 2, 64)
      store64_h(create_info, qfi, 72)
   } else {
      store32(create_info, VK_SHARING_MODE_EXCLUSIVE, 60)
      store32(create_info, 0, 64)
      store32(create_info, 0, 72)
   }
   store32(create_info, pre_transform, 80)
   store32(create_info, composite_alpha, 84)
   store32(create_info, present_mode, 88)
   store32(create_info, (common.env_truthy("NY_UI_HEADLESS") || common.env_truthy("NYTRIX_AUTO_DUMP")) ? 0 : 1, 92)
   store64_h(create_info, _old_swapchain_hint, 96)
   mut sc_ptr = _renderer_alloc(8)
   store32(sc_ptr, 0, 0)
   store32(sc_ptr, 0, 4)
   _vk_stage("swapchain.create_call")
   mut res = -1
   mut create_attempt = 0
   def retry_limit = _swapchain_create_retry_limit()
   while create_attempt < retry_limit {
      store32(sc_ptr, 0, 0)
      store32(sc_ptr, 0, 4)
      res = create_swapchain_khr(_device, create_info, 0, sc_ptr)
      if res == 0 { break }
      if vk_state._debug_gfx_enabled {
         ui_profile.print_text("[gfx:vulkan] vkCreateSwapchainKHR failed attempt=" +
            to_str(create_attempt + 1) + "/" + to_str(retry_limit) +
            " code=" + to_str(res) +
         " extent=" + to_str(w) + "x" + to_str(h))
      }
      create_attempt += 1
      if create_attempt < retry_limit { _settle_window_for_swapchain(win, create_attempt) }
   }
   if res != 0 {
      ui_profile.print_text("[gfx:vulkan] vkCreateSwapchainKHR failed code=" + to_str(res) +
         " attempts=" + to_str(create_attempt) +
         " extent=" + to_str(w) + "x" + to_str(h) +
         " min=" + to_str(min_w) + "x" + to_str(min_h) +
         " max=" + to_str(max_w) + "x" + to_str(max_h) +
      " present=" + to_str(present_mode))
      return false
   }
   _swapchain = load64_h(sc_ptr, 0)
   if _swapchain == 0 || _swapchain == 0x8000000000 || _swapchain == 0xc000000000 {
      ui_profile.print_text("[gfx:vulkan] invalid swapchain handle=0x" + text.to_hex(int(_swapchain)))
      return false
   }
   if vk_state._debug_gfx_enabled { _dbg_handle("swapchain", _swapchain) }
   _vk_stage("swapchain.images_count")
   mut img_count_ptr = _renderer_alloc(4)
   def rc_img_count = get_swapchain_images_khr(_device, _swapchain, img_count_ptr, 0)
   if rc_img_count != 0 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] get_swapchain_images(count) failed code=" + to_str(rc_img_count) + " swapchain=0x" + text.to_hex(int(_swapchain))) }
      return false
   }
   def allocated_count = load32(img_count_ptr, 0)
   if allocated_count < 1 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] get_swapchain_images(count) returned 0") }
      free(img_count_ptr)
      return false
   }
   _vk_stage("swapchain.images_list")
   mut img_count_ptr2 = _renderer_alloc(4)
   store32(img_count_ptr2, allocated_count, 0)
   mut img_ptrs_raw = _renderer_alloc(allocated_count * 8)
   def rc_imgs = get_swapchain_images_khr(_device, _swapchain, img_count_ptr2, img_ptrs_raw)
   if rc_imgs != 0 {
      if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] get_swapchain_images(list) failed code=" + to_str(rc_imgs) + " swapchain=0x" + text.to_hex(int(_swapchain))) }
      free(img_count_ptr) free(img_count_ptr2) free(img_ptrs_raw)
      return false
   }
   def returned_count = load32(img_count_ptr2, 0)
   _swapchain_image_count = (returned_count > 0 && returned_count <= allocated_count) ? returned_count : allocated_count
   _swapchain_images = []
   mut i = 0
   while i < _swapchain_image_count {
      def img_h = load64_h(img_ptrs_raw, i * 8)
      if !img_h || img_h == 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] null swapchain image at index=" + to_str(i)) }
         free(img_count_ptr) free(img_count_ptr2) free(img_ptrs_raw)
         return false
      }
      _swapchain_images = _swapchain_images.append(img_h)
      i += 1
   }
   free(img_count_ptr, img_count_ptr2, img_ptrs_raw)
   true
}

fn _create_swapchain_image_views() bool {
   _swapchain_image_views = []
   mut i = 0
   while i < _swapchain_image_count {
      def image_handle = _swapchain_images.get(i, 0)
      if !_handle_ok(image_handle) {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] invalid swapchain image at index=" + to_str(i)) }
         return false
      }
      mut ci = _renderer_alloc(80)
      store32(ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store32(ci, 0, 8) store32(ci, 0, 12)
      store32(ci, 0, 16)
      store64_h(ci, image_handle, 24)
      store32(ci, 1, 32)
      store32(ci, _swapchain_format, 36)
      store32(ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(ci, 0, 60)
      store32(ci, 1, 64)
      store32(ci, 0, 68)
      store32(ci, 1, 72)
      mut view_ptr = _renderer_alloc(8)
      store32(view_ptr, 0, 0) store32(view_ptr, 0, 4)
      def iv_res = create_image_view(_device, ci, 0, view_ptr)
      if iv_res != 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create_image_view failed code=" + to_str(iv_res) + " index=" + to_str(i)) }
         free(ci) free(view_ptr) return false
      }
      def view_h = load64_h(view_ptr, 0)
      if !_handle_ok(view_h) {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create_image_view returned null index=" + to_str(i)) }
         free(ci) free(view_ptr) return false
      }
      _swapchain_image_views = _swapchain_image_views.append(view_h)
      free(ci, view_ptr)
      i += 1
   }
   _swapchain_image_views_count = _swapchain_image_views.len
   true
}

fn _destroy_swapchain_objects() any {
   if _device == 0 { return 0 }
   mut i = 0
   while i < _framebuffers_count {
      def fb = _framebuffers.get(i, 0)
      if fb { destroy_framebuffer(_device, fb, 0) }
      i += 1
   }
   _framebuffers = []
   _framebuffers_count = 0
   _destroy_draw_images_queued()
   _flush_main_deletion_queue()
   i = 0
   while i < _swapchain_image_views_count {
      def iv = _swapchain_image_views.get(i, 0)
      if iv { destroy_image_view(_device, iv, 0) }
      i += 1
   }
   _swapchain_image_views = []
   _swapchain_image_views_count = 0
   if _swapchain != 0 && _swapchain != _headless_swapchain_sentinel() {
      destroy_swapchain_khr(_device, _swapchain, 0)
      _swapchain = 0
   }
   _swapchain_images = []
   _swapchain_image_count = 0
   _image_available_semaphores_count = 0
   _render_finished_semaphores_count = 0
   _in_flight_fences_count = 0
   0
}

fn _recreate_swapchain() bool {
   _vk_stage("recreate.enter")
   if _window_ref == 0 || _device == 0 { return false }
   device_wait_idle(_device)
   def old_swapchain = _swapchain
   _swapchain = 0
   mut i = 0
   while i < _framebuffers_count {
      def fb = _framebuffers.get(i, 0)
      if fb { destroy_framebuffer(_device, fb, 0) }
      i += 1
   }
   _framebuffers = []
   _framebuffers_count = 0
   _destroy_draw_images_queued()
   _flush_main_deletion_queue()
   i = 0
   while i < _swapchain_image_views_count {
      def iv = _swapchain_image_views.get(i, 0)
      if iv { destroy_image_view(_device, iv, 0) }
      i += 1
   }
   _swapchain_image_views = []
   _swapchain_image_views_count = 0
   _swapchain_images = []
   _swapchain_image_count = 0
   _destroy_depth_msaa_resources()
   _vk_stage("recreate.create_swapchain")
   def ok = _create_swapchain_with_old(_window_ref, old_swapchain)
   if old_swapchain != 0 && old_swapchain != _headless_swapchain_sentinel() { destroy_swapchain_khr(_device, old_swapchain, 0) }
   if !ok { return false }
   if _swapchain == 0 { ui_profile.print_text("[gfx:vulkan] _recreate_swapchain: swapchain null after create") return false }
   if _swapchain_image_count < 1 { ui_profile.print_text("[gfx:vulkan] _recreate_swapchain: zero image count after create") return false }
   _vk_stage("recreate.image_views")
   if !_create_swapchain_image_views() { return false }
   if _swapchain_image_views_count < 1 { return false }
   _vk_stage("recreate.draw_format")
   if !_prepare_draw_image_format() { return false }
   _vk_stage("recreate.depth")
   if !_create_depth_resources() { return false }
   _vk_stage("recreate.draw_images")
   if !_create_draw_images() { return false }
   _vk_stage("recreate.framebuffers")
   if !_create_framebuffers() { return false }
   if _framebuffers_count < 1 { return false }
   true
}

fn _create_image_views() bool {
   _swapchain_image_views = []
   mut i = 0
   while i < _swapchain_image_count {
      def image_handle = _swapchain_images.get(i, 0)
      if !_handle_ok(image_handle) {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] invalid swapchain image at index=" + to_str(i)) }
         return false
      }
      mut create_info = _renderer_alloc(80)
      store32(create_info, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store32(create_info, 0, 8) store32(create_info, 0, 12)
      store32(create_info, 0, 16)
      store64_h(create_info, image_handle, 24)
      store32(create_info, 1, 32)
      store32(create_info, _swapchain_format, 36)
      store32(create_info, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(create_info, 0, 60)
      store32(create_info, 1, 64)
      store32(create_info, 0, 68)
      store32(create_info, 1, 72)
      mut view_ptr = _renderer_alloc(8)
      def iv_res = create_image_view(_device, create_info, 0, view_ptr)
      if iv_res != 0 { return false }
      def view_h = load64_h(view_ptr, 0)
      _swapchain_image_views = _swapchain_image_views.append(view_h)
      free(create_info) free(view_ptr)
      i += 1
   }
   true
}

fn _destroy_draw_images_queued() bool {
   mut i = 0
   while i < _draw_image_views_count {
      def image = _draw_images.get(i, 0)
      def view = _draw_image_views.get(i, 0)
      def memory = _draw_image_memories.get(i, 0)
      if image || memory { _dq_push_main(_DQ_IMAGE_MEMORY, image, memory) }
      if view { _dq_push_main(_DQ_IMAGE_VIEW, view) }
      i += 1
   }
   _draw_images = []
   _draw_image_views = []
   _draw_image_memories = []
   _draw_image_views_count = 0
   _draw_extent_w = 0
   _draw_extent_h = 0
   true
}

fn _create_draw_image_one(int width, int height, int format) any {
   mut img_ci = _renderer_alloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16)
   store32(img_ci, 1, 20)
   store32(img_ci, format, 24)
   store32(img_ci, width, 28)
   store32(img_ci, height, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, 1, 40)
   store32(img_ci, 1, 44)
   store32(img_ci, 1, 48)
   store32(img_ci, 0, 52)
   store32(img_ci,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   56)
   store32(img_ci, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(img_ci, 0, 80)
   mut img_ptr = _renderer_alloc(8)
   if create_image(_device, img_ci, 0, img_ptr) != 0 {
      free(img_ci) free(img_ptr)
      return 0
   }
   def image = load64_h(img_ptr, 0)
   if !image {
      free(img_ci) free(img_ptr)
      return 0
   }
   mut mem_req = _renderer_alloc(24)
   get_image_memory_requirements(_device, image, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   def mem_type = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
   mut alloc_info = _renderer_alloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, size, 16)
   store32(alloc_info, mem_type, 24)
   mut mem_ptr = _renderer_alloc(8)
   if allocate_memory(_device, alloc_info, 0, mem_ptr) != 0 {
      destroy_image(_device, image, 0)
      free(img_ci) free(img_ptr) free(mem_req) free(alloc_info) free(mem_ptr)
      return 0
   }
   def memory = load64_h(mem_ptr, 0)
   if !memory || bind_image_memory(_device, image, memory, 0) != 0 {
      if memory { free_memory(_device, memory, 0) }
      destroy_image(_device, image, 0)
      free(img_ci) free(img_ptr) free(mem_req) free(alloc_info) free(mem_ptr)
      return 0
   }
   mut view_ci = _renderer_alloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_h(view_ci, image, 24)
   store32(view_ci, 1, 32)
   store32(view_ci, format, 36)
   store32(view_ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)
   mut view_ptr = _renderer_alloc(8)
   if create_image_view(_device, view_ci, 0, view_ptr) != 0 {
      free_memory(_device, memory, 0)
      destroy_image(_device, image, 0)
      free(img_ci) free(img_ptr) free(mem_req) free(alloc_info) free(mem_ptr) free(view_ci) free(view_ptr)
      return 0
   }
   def view = load64_h(view_ptr, 0)
   free(img_ci) free(img_ptr) free(mem_req) free(alloc_info) free(mem_ptr) free(view_ci) free(view_ptr)
   if !view {
      free_memory(_device, memory, 0)
      destroy_image(_device, image, 0)
      return 0
   }
   [image, memory, view]
}

fn _create_draw_images() bool {
   _destroy_draw_images_queued()
   _flush_main_deletion_queue()
   if !_offscreen_draw_enabled() { return true }
   if _swapchain_image_count < 1 || _swapchain_extent_w <= 0 || _swapchain_extent_h <= 0 { return false }
   _prepare_draw_image_format()
   _draw_extent_w, _draw_extent_h = _swapchain_extent_w, _swapchain_extent_h
   mut i = 0
   while i < _swapchain_image_count {
      def made = _create_draw_image_one(_draw_extent_w, _draw_extent_h, _draw_image_format)
      if !is_list(made) || made.len < 3 {
         _destroy_draw_images_queued()
         _flush_main_deletion_queue()
         return false
      }
      _draw_images = _draw_images.append(made[0])
      _draw_image_memories = _draw_image_memories.append(made[1])
      _draw_image_views = _draw_image_views.append(made[2])
      i += 1
   }
   _draw_image_views_count = _draw_image_views.len
   true
}

fn _create_depth_resources() bool {
   _prepare_draw_image_format()
   _depth_format = _choose_depth_format()
   def depth_format = _depth_format
   def samples = _cfg_msaa
   mut img_ci = _renderer_alloc(88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16)
   store32(img_ci, 1, 20)
   store32(img_ci, depth_format, 24)
   store32(img_ci, _swapchain_extent_w, 28)
   store32(img_ci, _swapchain_extent_h, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, 1, 40)
   store32(img_ci, 1, 44)
   store32(img_ci, samples, 48)
   store32(img_ci, 0, 52)
   store32(img_ci, 32, 56)
   store32(img_ci, 0, 60)
   store32(img_ci, 0, 64)
   store32(img_ci, 0, 80)
   mut img_ptr = _renderer_alloc(8)
   if create_image(_device, img_ci, 0, img_ptr) != 0 { return false }
   _depth_image = load64_h(img_ptr, 0)
   mut mem_req = _renderer_alloc(24)
   get_image_memory_requirements(_device, _depth_image, mem_req)
   def d_size = load64_h(mem_req, 0)
   def d_bits = load32(mem_req, 16)
   def d_mtype = _find_memory_type(d_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
   mut alloc_info = _renderer_alloc(64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, d_size, 16)
   store32(alloc_info, d_mtype, 24)
   mut mem_ptr = _renderer_alloc(8)
   if allocate_memory(_device, alloc_info, 0, mem_ptr) != 0 { return false }
   _depth_memory = load64_h(mem_ptr, 0)
   bind_image_memory(_device, _depth_image, _depth_memory, 0)
   mut view_ci = _renderer_alloc(80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_h(view_ci, _depth_image, 24)
   store32(view_ci, 1, 32)
   store32(view_ci, depth_format, 36)
   store32(view_ci, 0x00000002, 56)
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)
   mut view_ptr = _renderer_alloc(8)
   if create_image_view(_device, view_ci, 0, view_ptr) != 0 { return false }
   _depth_view = load64_h(view_ptr, 0)
   if samples > 1 {
      def color_format = _render_color_format()
      mut ci2 = _renderer_alloc(88)
      store32(ci2, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
      store32(ci2, 0, 16)
      store32(ci2, 1, 20)
      store32(ci2, color_format, 24)
      store32(ci2, _swapchain_extent_w, 28)
      store32(ci2, _swapchain_extent_h, 32)
      store32(ci2, 1, 36)
      store32(ci2, 1, 40)
      store32(ci2, 1, 44)
      store32(ci2, samples, 48)
      store32(ci2, 0, 52)
      store32(ci2, 0x00000010, 56)
      store32(ci2, 0, 60)
      store32(ci2, 0, 64)
      store32(ci2, 0, 80)
      mut ip2 = _renderer_alloc(8)
      if create_image(_device, ci2, 0, ip2) != 0 { return false }
      _msaa_color_image = load64_h(ip2, 0)
      mut mr2 = _renderer_alloc(24)
      get_image_memory_requirements(_device, _msaa_color_image, mr2)
      def c_size = load64_h(mr2, 0)
      def c_bits = load32(mr2, 16)
      def c_mtype = _find_memory_type(c_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      mut ai2 = _renderer_alloc(64)
      store32(ai2, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
      store64_h(ai2, c_size, 16)
      store32(ai2, c_mtype, 24)
      mut mp2 = _renderer_alloc(8)
      if allocate_memory(_device, ai2, 0, mp2) != 0 { return false }
      _msaa_color_memory = load64_h(mp2, 0)
      bind_image_memory(_device, _msaa_color_image, _msaa_color_memory, 0)
      mut vc2 = _renderer_alloc(80)
      store32(vc2, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store64_h(vc2, _msaa_color_image, 24)
      store32(vc2, 1, 32)
      store32(vc2, color_format, 36)
      store32(vc2, 0x00000001, 56)
      store32(vc2, 1, 64)
      store32(vc2, 1, 72)
      mut vp2 = _renderer_alloc(8)
      if create_image_view(_device, vc2, 0, vp2) != 0 { return false }
      _msaa_color_view = load64_h(vp2, 0)
   }
   true
}

fn _create_render_pass() bool {
   def samples = _cfg_msaa
   def msaa = samples > 1
   def color_format = _render_color_format()
   def final_color_layout = _render_final_color_layout()
   if msaa {
      mut atts = _renderer_alloc(108)
      _store_attachment_desc(atts, 0, color_format, samples, 1, 2, 2, 2, 0, 2)
      _store_attachment_desc(atts, 36, _depth_format, samples, 1, 2, 2, 2, 0, 3)
      _store_attachment_desc(atts, 72, color_format, 1, 1, 0, 2, 2, 0, final_color_layout)
      mut car = _renderer_alloc(8) store32(car, 0, 0) store32(car, 2, 4)
      mut dar = _renderer_alloc(8) store32(dar, 1, 0) store32(dar, 3, 4)
      mut rar = _renderer_alloc(8) store32(rar, 2, 0) store32(rar, 2, 4)
      mut sd = _renderer_alloc(72)
      store32(sd, 0, 4)
      store32(sd, 1, 24)
      store64_h(sd, car, 32)
      store64_h(sd, rar, 40)
      store64_h(sd, dar, 48)
      _render_pass = _create_render_pass_handle(atts, 3, sd)
   } else {
      mut atts = _renderer_alloc(72)
      _store_attachment_desc(atts, 0, color_format, 1, 1, 0, 2, 2, 0, final_color_layout)
      _store_attachment_desc(atts, 36, _depth_format, 1, 1, 2, 2, 2, 0, 3)
      mut car = _renderer_alloc(8) store32(car, 0, 0) store32(car, 2, 4)
      mut dar = _renderer_alloc(8) store32(dar, 1, 0) store32(dar, 3, 4)
      mut sd = _renderer_alloc(72)
      store32(sd, 0, 4)
      store32(sd, 1, 24)
      store64_h(sd, car, 32)
      store64_h(sd, dar, 48)
      _render_pass = _create_render_pass_handle(atts, 2, sd)
   }
   if !_render_pass { return false }
   _create_load_render_pass(msaa, final_color_layout)
   _create_load_color_clear_depth_render_pass(msaa, final_color_layout)
}

fn _create_framebuffers() bool {
   _vk_stage("framebuffers.start")
   _framebuffers = []
   _framebuffers_count = 0
   if _offscreen_draw_enabled() && _draw_image_views_count < _swapchain_image_count { return false }
   _vk_stage("framebuffers.after_offscreen")
   _vk_stage("framebuffers.alloc_slab")
   _framebuffers_slab = _vk_alloc_handle_slab(_framebuffers_slab, _swapchain_image_count)
   if !_framebuffers_slab { return false }
   _vk_stage("framebuffers.loop")
   def msaa = _cfg_msaa > 1
   mut i = 0
   while i < _swapchain_image_count {
      _vk_stage("framebuffers.item")
      mut attach_ptr = 0
      mut att_count = 0
      def color_view = _offscreen_draw_enabled() ? _draw_image_views.get(i, 0) : _swapchain_image_views.get(i, 0)
      if !_handle_ok(color_view) { return false }
      if msaa {
         attach_ptr = _renderer_alloc(24)
         store64_h(attach_ptr, _msaa_color_view, 0)
         store64_h(attach_ptr, _depth_view, 8)
         store64_h(attach_ptr, color_view, 16)
         att_count = 3
      } else {
         attach_ptr = _renderer_alloc(16)
         store64_h(attach_ptr, color_view, 0)
         store64_h(attach_ptr, _depth_view, 8)
         att_count = 2
      }
      mut create_info = _renderer_alloc(64)
      store32(create_info, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0)
      store64_h(create_info, _render_pass, 24)
      store32(create_info, att_count, 32)
      store64_h(create_info, attach_ptr, 40)
      store32(create_info, _swapchain_extent_w, 48)
      store32(create_info, _swapchain_extent_h, 52)
      store32(create_info, 1, 56)
      mut fb_ptr = _renderer_alloc(8)
      if !fb_ptr { free(attach_ptr) free(create_info) return false }
      memset(fb_ptr, 0, 8)
      def fb_res = create_framebuffer(_device, create_info, 0, fb_ptr)
      def fb = load64_h(fb_ptr, 0)
      free(attach_ptr, create_info, fb_ptr)
      if fb_res != 0 || !fb || fb == 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create_framebuffer failed i=" + to_str(i) + " res=" + to_str(fb_res) + " h=0x" + text.to_hex(fb)) }
         return false
      }
      if vk_state._debug_gfx_enabled { _dbg_handle(f"framebuffer {i}", fb) }
      _framebuffers = _framebuffers.append(fb)
      store64_h(_framebuffers_slab, fb, i * 8)
      i += 1
   }
   _vk_stage("framebuffers.count")
   _framebuffers_count = _framebuffers.len
   true
}

fn _create_sync_objects() bool {
   _image_available_semaphores = []
   _render_finished_semaphores = []
   _in_flight_fences = []
   _sem_avail_slab = _vk_alloc_handle_slab(_sem_avail_slab, _frames_in_flight())
   _sem_finish_slab = _vk_alloc_handle_slab(_sem_finish_slab, _frames_in_flight())
   _fences_slab = _vk_alloc_handle_slab(_fences_slab, _frames_in_flight())
   if !_sem_avail_slab || !_sem_finish_slab || !_fences_slab {
      if ui_profile.env_truthy_cached("NY_VK_DESCRIPTOR_TRACE") {
         ui_profile.print_text("[gfx:vulkan] sync slab allocation failed frames=" + to_str(_frames_in_flight()) +
            " sem_avail=" + to_str(_sem_avail_slab) +
            " sem_finish=" + to_str(_sem_finish_slab) +
         " fences=" + to_str(_fences_slab))
      }
      return false
   }
   _image_available_semaphores_count = 0
   _render_finished_semaphores_count = 0
   _in_flight_fences_count = 0
   mut i = 0
   while i < _frames_in_flight() {
      mut si = _renderer_alloc(24)
      store32(si, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0)
      store32(si, 0, 8) store32(si, 0, 12)
      store32(si, 0, 16)
      mut sem1 = _renderer_alloc(8)
      def s1_res = create_semaphore(_device, si, 0, sem1)
      if s1_res != 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create semaphore image_available failed res=" + to_str(s1_res) + " stype=" + to_str(load32(si, 0))) }
         return false
      }
      def sem1_h = load64_h(sem1, 0)
      _image_available_semaphores = _image_available_semaphores.append(sem1_h)
      _image_available_semaphores_count = _image_available_semaphores.len
      store64_h(_sem_avail_slab, sem1_h, i * 8)
      mut sem2 = _renderer_alloc(8)
      def s2_res = create_semaphore(_device, si, 0, sem2)
      if s2_res != 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create semaphore render_finished failed res=" + to_str(s2_res) + " stype=" + to_str(load32(si, 0))) }
         return false
      }
      def sem2_h = load64_h(sem2, 0)
      _render_finished_semaphores = _render_finished_semaphores.append(sem2_h)
      _render_finished_semaphores_count = _render_finished_semaphores.len
      store64_h(_sem_finish_slab, sem2_h, i * 8)
      mut fi = _renderer_alloc(24)
      store32(fi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
      store32(fi, 0, 8) store32(fi, 0, 12)
      store32(fi, 1, 16)
      mut fence = _renderer_alloc(8)
      def f_res = create_fence(_device, fi, 0, fence)
      if f_res != 0 {
         if vk_state._debug_gfx_enabled { ui_profile.print_text("[gfx:vulkan] create fence failed res=" + to_str(f_res) + " stype=" + to_str(load32(fi, 0))) }
         return false
      }
      def fence_h = load64_h(fence, 0)
      _in_flight_fences = _in_flight_fences.append(fence_h)
      _in_flight_fences_count = _in_flight_fences.len
      store64_h(_fences_slab, fence_h, i * 8)
      i += 1
   }
   true
}

fn _create_command_pool() bool {
   mut create_info = _renderer_alloc(32)
   store32(create_info, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0)
   store32(create_info, 2, 16)
   store32(create_info, _graphics_family_index, 20)
   mut pool_ptr = _renderer_alloc(8)
   def cp_res = create_command_pool(_device, create_info, 0, pool_ptr)
   if cp_res != 0 { return false }
   _command_pool = load64_h(pool_ptr, 0)
   true
}

fn _create_command_buffers() bool {
   mut ai = _renderer_alloc(32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_h(ai, _command_pool, 16)
   store32(ai, 0, 24)
   store32(ai, _frames_in_flight(), 28)
   mut bufs_ptr = _renderer_alloc(_frames_in_flight() * 8)
   def cb_res = allocate_command_buffers(_device, ai, bufs_ptr)
   if cb_res != 0 { return false }
   _command_buffers = []
   _cmd_bufs_slab = _vk_alloc_handle_slab(_cmd_bufs_slab, _frames_in_flight())
   if !_cmd_bufs_slab { return false }
   mut i = 0
   while i < _frames_in_flight() {
      def cb_h = load64(bufs_ptr, i * 8)
      _command_buffers = _command_buffers.append(cb_h)
      store64_h(_cmd_bufs_slab, cb_h, i * 8)
      i += 1
   }
   _command_buffers_count = _command_buffers.len
   true
}

fn _create_ubo_descriptor_sets() bool {
   if !_descriptor_pool || !_descriptor_set_layout_ubo || !_ubo_buffer { return false }
   _ubo_descriptor_sets = []
   _ubo_ds_slab = _vk_alloc_handle_slab(_ubo_ds_slab, _frames_in_flight())
   if !_ubo_ds_slab { return false }
   mut i = 0
   while i < _frames_in_flight() {
      mut dsl_ptr = _renderer_alloc(8)
      store64_h(dsl_ptr, _descriptor_set_layout_ubo, 0)
      mut alloc_ds = _renderer_alloc(40)
      store32(alloc_ds, _vk_stype_descriptor_set_allocate_info(), 0)
      store64_h(alloc_ds, _descriptor_pool, 16)
      store32(alloc_ds, 1, 24)
      store64_h(alloc_ds, dsl_ptr, 32)
      mut ds_ptr = _renderer_alloc(8)
      if allocate_descriptor_sets(_device, alloc_ds, ds_ptr) != 0 { return false }
      def ds = load64_h(ds_ptr, 0)
      mut buf_info = _renderer_alloc(24)
      store64_h(buf_info, _ubo_buffer, 0)
      store64_h(buf_info, 0, 8)
      store64_h(buf_info, _ubo_size_value(), 16)
      mut write = _renderer_alloc(64)
      store32(write, _vk_stype_write_descriptor_set(), 0)
      store64_h(write, ds, 16)
      store32(write, 0, 24)
      store32(write, 0, 28)
      store32(write, 1, 32)
      store32(write, _vk_descriptor_uniform_buffer(), 36)
      store64_h(write, buf_info, 48)
      update_descriptor_sets(_device, 1, write, 0, 0)
      store64_h(_ubo_ds_slab, ds, i * 8)
      free(dsl_ptr) free(alloc_ds) free(ds_ptr)
      free(buf_info) free(write)
      i += 1
   }
   _ubo_descriptor_sets = [1]
   true
}

fn notify_window_resize(int w, int h) bool {
   "Notifies the renderer that the window has been resized."
   if w <= 0 || h <= 0 { return false }
   if _same_int_value(_pending_resize_w, int(w)) && _same_int_value(_pending_resize_h, int(h)) { return true }
   if _same_int_value(_swapchain_extent_w, int(w)) && _same_int_value(_swapchain_extent_h, int(h)) { return true }
   _pending_resize_w, _pending_resize_h = int(w), int(h)
   _pending_resize_stamp_ns = ticks()
   _swapchain_recreate_pending = true
   _swapchain_recreate_force = false
   _suboptimal_recreate_attempted = false
   _logged_suboptimal_acquire = false
   _logged_suboptimal_present = false
   true
}

fn get_swapchain_size() list {
   "Returns the current swapchain dimensions as [w, h]."
   [_swapchain_extent_w, _swapchain_extent_h]
}

fn get_swapchain_width() int {
   "Runs the get swapchain width operation."
   _swapchain_extent_w
}

fn get_swapchain_height() int {
   "Runs the get swapchain height operation."
   _swapchain_extent_h
}

fn get_swapchain_image_count() int {
   "Runs the get swapchain image count operation."
   _swapchain_image_count
}
