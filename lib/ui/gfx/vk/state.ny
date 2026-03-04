;; Keywords: ui gfx vulkan renderer state

module std.ui.gfx.vk.state (
   _frame_draw_calls, _total_draw_calls, _static_vbo_ptr, _static_off_ptr,
   VERTEX_STRIDE, _VKR_VERT_STRIDE, _VKR_OFF_X, _VKR_OFF_Y, _VKR_OFF_Z, _VKR_OFF_U, _VKR_OFF_V, _VKR_OFF_C, _VKR_OFF_TEX, _VKR_OFF_NX, _VKR_OFF_NY, _VKR_OFF_NZ,
   _VKR_GLYPH_STRIDE, _VKR_G_ADV, _VKR_G_XOFF, _VKR_G_YOFF, _VKR_G_BW, _VKR_G_BH, _VKR_G_U1, _VKR_G_V1, _VKR_G_U2, _VKR_G_V2, _VKR_G_TEX, _VKR_G_PRESENT, _VKR_G_IS_COLOR,
   _cfg_vsync, _cfg_filter, _cfg_vert_spv, _cfg_frag_spv, _cfg_msaa, _debug_gfx_enabled,
   _instance, _physical_device, _device, _graphics_queue, _present_queue, _graphics_family_index, _surface,
   _swapchain, _swapchain_image_count, _swapchain_images, _swapchain_image_views, _swapchain_format, _swapchain_extent_w, _swapchain_extent_h,
   _render_pass, _framebuffers, _depth_image, _depth_memory, _depth_view, _msaa_color_image, _msaa_color_memory, _msaa_color_view,
   _command_pool, _command_buffers, _descriptor_set_layout, _pipeline_layout, _pipeline, _unlit_pipeline, _line_pipeline, _wire_pipeline,
   _circle_pipeline, _ring_pipeline,
   _vert_module, _frag_module, _is_wireframe, _descriptor_set_layout_ubo, _bindless_enabled, _bindless_ds, _ubo_enabled,
   _vertex_capacity, _current_frame_vertex_offset, _vertex_buffer, _vertex_memory, _vertex_map, _local_vertex_map, _vertex_offset, _last_flush_offset, _vertex_limit_hit,
   _staging_buffer, _staging_memory, _staging_map, _staging_capacity, _ubo_buffer, _ubo_memory, _ubo_map, _ubo_map_size, _ubo_stride, _ubo_descriptor_sets,
   _default_texture, _default_sampler, _descriptor_pool, _textures, _texture_ds_cache, _texture_fmt_cache, _free_texture_ids, _bindless_overflow_warned,
   _current_texture_id, _current_tex_index, _current_is_unlit, _last_is_unlit,
   _image_available_semaphores, _render_finished_semaphores, _in_flight_fences,
   _current_frame, _image_index, _total_frames, _pc_buffer, _current_mvp, _current_model, _frame_open, _window_ref,
   _upload_cb, _upload_alloc, _upload_bi, _upload_bar1, _upload_bar2, _upload_region, _upload_si, _upload_cb_arr, _upload_cb_ptr,
   _flush_off, _flush_buf, _last_bound_tex_id, _last_bound_ds, _last_bound_ubo_ds, _target_pipeline, _last_bound_pipe, _pc_dirty, _last_is_mask, _clear_ca, _clear_rect,
   _upload_fence, _upload_fence_ptr, _quad_template, _prof_flush_total, _prof_flush_count, _prof_flush_avg,
   _ptr_fence, _ptr_img_idx, _ptr_bi, _ptr_clear, _ptr_ri, _ptr_vp, _ptr_sci, _ptr_dsl, _ptr_ds, _ptr_sub, _ptr_wait_sems, _ptr_sig_sems, _ptr_stages,
   _clear_r, _clear_g, _clear_b, _clear_a, MAX_FRAMES_IN_FLIGHT, MAX_TEXTURES, _UBO_SIZE,
   _fps_last_time, _fps_count, _fps_curr,
   _mvp_dirty, _model_dirty, _blit_tex_id
)

mut _frame_draw_calls = 0
mut _total_draw_calls = 0
mut _static_vbo_ptr = 0
mut _static_off_ptr = 0
def VERTEX_STRIDE = 40
def _VKR_VERT_STRIDE = 40
def _VKR_OFF_X = 0
def _VKR_OFF_Y = 4
def _VKR_OFF_Z = 8
def _VKR_OFF_U = 12
def _VKR_OFF_V = 16
def _VKR_OFF_C = 20
def _VKR_OFF_TEX = 24
def _VKR_OFF_NX = 28
def _VKR_OFF_NY = 32
def _VKR_OFF_NZ = 36
def _VKR_GLYPH_STRIDE = 48
def _VKR_G_ADV = 0
def _VKR_G_XOFF = 4
def _VKR_G_YOFF = 8
def _VKR_G_BW = 12
def _VKR_G_BH = 16
def _VKR_G_U1 = 20
def _VKR_G_V1 = 24
def _VKR_G_U2 = 28
def _VKR_G_V2 = 32
def _VKR_G_TEX = 36
def _VKR_G_PRESENT = 40
def _VKR_G_IS_COLOR = 44
def MAX_TEXTURES = 1024
def _UBO_SIZE = 144

mut _descriptor_set_layout_ubo = 0
mut _bindless_enabled = true
mut _bindless_ds = 0
mut _ubo_enabled = false
mut _ubo_buffer = 0
mut _ubo_memory = 0
mut _ubo_map = 0
mut _ubo_map_size = 0
mut _ubo_stride = _UBO_SIZE
mut _ubo_descriptor_sets = []
mut _last_bound_ubo_ds = 0

;; Renderer configuration + debug
mut _cfg_vsync = false
mut _cfg_filter = 0
mut _cfg_vert_spv = ""
mut _cfg_frag_spv = ""
mut _cfg_msaa = 1
mut _debug_gfx_enabled = false

;; Core Vulkan handles/state
mut _instance = 0
mut _physical_device = 0
mut _device = 0
mut _graphics_queue = 0
mut _present_queue = 0
mut _graphics_family_index = 0
mut _surface = 0
mut _swapchain = 0
mut _swapchain_image_count = 0
mut _swapchain_images = []
mut _swapchain_image_views = []
mut _swapchain_format = 0
mut _swapchain_extent_w = 0
mut _swapchain_extent_h = 0
mut _render_pass = 0
mut _framebuffers = []
mut _depth_image = 0
mut _depth_memory = 0
mut _depth_view = 0
mut _msaa_color_image = 0
mut _msaa_color_memory = 0
mut _msaa_color_view = 0
mut _command_pool = 0
mut _command_buffers = []
mut _descriptor_set_layout = 0
mut _pipeline_layout = 0
mut _pipeline = 0
mut _unlit_pipeline = 0
mut _line_pipeline = 0
mut _wire_pipeline = 0
mut _circle_pipeline = 0
mut _ring_pipeline = 0
mut _vert_module = 0
mut _frag_module = 0
mut _is_wireframe = false

;; GPU buffers + maps
mut _vertex_capacity = 33554432
mut _current_frame_vertex_offset = 0
mut _vertex_buffer = 0
mut _vertex_memory = 0
mut _vertex_map = 0
mut _local_vertex_map = 0
mut _vertex_offset = 0
mut _last_flush_offset = 0
mut _vertex_limit_hit = false

mut _staging_buffer = 0
mut _staging_memory = 0
mut _staging_map = 0
mut _staging_capacity = 67108864

;; Textures
mut _default_texture = 0
mut _default_sampler = 0
mut _descriptor_pool = 0
mut _textures = []
mut _texture_ds_cache = []
mut _texture_fmt_cache = []
mut _free_texture_ids = []
mut _bindless_overflow_warned = false

;; Draw state
mut _current_texture_id = -1
mut _current_tex_index = 0
mut _current_is_unlit = 0
mut _last_is_unlit = 0

mut _image_available_semaphores = []
mut _render_finished_semaphores = []
mut _in_flight_fences = []

mut _current_frame = 0
mut _current_frame_cb = 0
mut _current_frame_ubo_ds = 0
mut _image_index = 0
mut _total_frames = 0
mut _pc_buffer = 0
mut _pc_buffer_custom = 0 ;; Separate buffer for custom pipeline push constants
mut _current_mvp = 0
mut _current_model = 0
mut _frame_open = false
mut _window_ref = 0
mut _use_custom_pc = false ;; Flag to use custom push constants

;; Upload helpers
mut _upload_cb = 0
mut _upload_alloc = 0
mut _upload_bi = 0
mut _upload_bar1 = 0
mut _upload_bar2 = 0
mut _upload_region = 0
mut _upload_si = 0
mut _upload_cb_arr = 0
mut _upload_cb_ptr = 0

;; Flush + profiling
mut _flush_off = 0
mut _flush_buf = 0
mut _last_bound_tex_id = -1
mut _last_bound_ds = 0
mut _target_pipeline = 0
mut _last_bound_pipe = 0
mut _pc_dirty = false
mut _last_is_mask = 0
mut _clear_ca = 0
mut _clear_rect = 0
mut _upload_fence = 0
mut _upload_fence_ptr = 0
mut _quad_template = 0
mut _prof_flush_total = 0.0
mut _prof_flush_count = 0
mut _prof_flush_avg = 0.0

;; Frame scratch pointers
mut _ptr_fence = 0
mut _ptr_img_idx = 0
mut _ptr_bi = 0
mut _ptr_clear = 0
mut _ptr_ri = 0
mut _ptr_vp = 0
mut _ptr_sci = 0
mut _ptr_dsl = 0
mut _ptr_ds = 0
mut _ptr_sub = 0
mut _ptr_wait_sems = 0
mut _ptr_sig_sems = 0
mut _ptr_stages = 0

;; Clear + frames
mut _clear_r = 0.05
mut _clear_g = 0.05
mut _clear_b = 0.1
mut _clear_a = 1.0
def MAX_FRAMES_IN_FLIGHT = 3

mut _fps_last_time = 0.0
mut _fps_count = 0
mut _fps_curr = 0

mut _mvp_dirty = true
mut _model_dirty = true
mut _blit_tex_id = -1
