;; Keywords: ui gfx vulkan renderer

module std.ui.gfx.vk.renderer (
   renderer_config,
   init,
   _update_default_mvp,
   _mvp_matrix,
   set_model_matrix, set_mvp, set_ortho, set_perspective,
   begin_frame, set_unlit, _sync_pc, _flush, _check_flush,
   end_frame, _end_frame_internal,
   clear, clear_depth, set_clear_color,
   shutdown, set_wireframe,
   _create_instance, _create_surface, _pick_physical_device, _create_logical_device,
   _choose_composite_alpha, _choose_present_mode, set_mask,
   _create_headless_image, _create_swapchain, _create_swapchain_image_views,
   _destroy_swapchain_objects, _recreate_swapchain, _create_image_views,
   _create_depth_resources, _create_render_pass, _create_framebuffers,
   _create_sync_objects, _create_command_pool, _create_command_buffers
)

use std.core *
use std.core.mem *
use std.math *
use std.math.matrix *
use std.ui.glfw as ui_glfw
use std.ui.consts *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.utils *
use std.ui.gfx.vk.buffers *
use std.ui.gfx.vk.texture *
use std.ui.gfx.vk.pipeline *
use std.ui.gfx.vk.vulkan *
use std.os *
use std.os.process as proc
use std.str as text
use std.util.common as common

fn renderer_config(vsync, filter, vert_spv_path, frag_spv_path, msaa){
   "Configures the renderer. Must be called BEFORE init_window().
   vsync: true/false (default false)
   filter: 0 for NEAREST, 1 for LINEAR (default 0)
   vert_spv_path: path to custom vertex shader .spv or empty for default
   frag_spv_path: path to custom fragment shader .spv or empty for default
   msaa: number of MSAA samples (1, 2, 4, 8) (default 1)"
   if(vsync){ _cfg_vsync = true } else { _cfg_vsync = false }
   if(filter){ _cfg_filter = 1 } else { _cfg_filter = 0 }
   _cfg_vert_spv = vert_spv_path
   _cfg_frag_spv = frag_spv_path
   _cfg_msaa = msaa
}

mut _last_mvp_w = -1
mut _last_mvp_h = -1
mut _vk_markers_enabled = false
mut _vk_profile_flush = false
mut _ident_mat = 0
fn init(win){
   "Initializes the Vulkan renderer for the given window."
   _window_ref = win
   _check_debug_env()
   if(_debug_gfx_enabled){ print("Vulkan: UBO enabled=" + to_str(_ubo_enabled)) }
   if(win){
       def flags = int(dict_get(win, "flags", 0))
       _current_window_flags = flags
       if(band(flags, 32)){ ;; WINDOW_TRANSPARENT
          _clear_r = 0.0 _clear_g = 0.0 _clear_b = 0.0 _clear_a = 0.0
       }
   }

   def vb_mb = env("NYTRIX_VK_VERTEX_MB")
   if(vb_mb){
      def n = text.atoi(vb_mb)
      if(n >= 8){ _vertex_capacity = n * 1024 * 1024 }
   }
   def st_mb = env("NYTRIX_VK_STAGING_MB")
   if(st_mb){
      def n = text.atoi(st_mb)
      if(n >= 16){ _staging_capacity = n * 1024 * 1024 }
   }

   def mk = env("NYTRIX_VK_MARKERS")
   if(mk && (mk == "1" || mk == "true")){ _vk_markers_enabled = true }
   if(env("RENDERDOC") || env("RENDERDOC_CAPTUREOPTS") || env("RENDERDOC_CMD")){
      _vk_markers_enabled = true
   }
   def pf = env("NYTRIX_VK_PROFILE_FLUSH")
   if(pf && (pf == "1" || pf == "true")){ _vk_profile_flush = true }

   def fast = env("NYTRIX_FAST")
   if(fast && (fast == "1" || fast == "true")){
      _cfg_vsync = false
      _cfg_filter = 0
      _cfg_msaa = 1
      if(_debug_gfx_enabled){ print("Vulkan: NYTRIX_FAST enabled (vsync=0 filter=nearest msaa=1)") }
   }

   if(!vk_init()){ return false }
   if(!_create_instance()){ return false }
   if(!_create_surface(win)){ return false }
   if(!_pick_physical_device()){ return false }
   if(!_create_logical_device()){ return false }
   if(!_create_swapchain(win)){ return false }
   _create_swapchain_image_views()
   _create_depth_resources()
   _create_render_pass()
   _create_graphics_pipeline()
   _create_framebuffers()
   if(!_create_sync_objects()){ if(_debug_gfx_enabled){ print("Vulkan: Sync objects failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Sync objects OK") }
   _create_command_pool()
   if(!_create_command_buffers()){ if(_debug_gfx_enabled){ print("Vulkan: Command buffers failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Command buffers OK") }
   if(!_create_vertex_buffer()){ if(_debug_gfx_enabled){ print("Vulkan: Vertex buffer failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Vertex buffer OK") }
   if(!_create_staging_buffer()){ if(_debug_gfx_enabled){ print("Vulkan: Staging buffer failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Staging buffer OK") }
   if(!_create_descriptor_pool()){ if(_debug_gfx_enabled){ print("Vulkan: Descriptor pool failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Descriptor pool OK") }
   if(!_create_uniform_buffer()){ if(_debug_gfx_enabled){ print("Vulkan: Uniform buffer failed") } return false }
   if(!_create_ubo_descriptor_sets()){ if(_debug_gfx_enabled){ print("Vulkan: UBO descriptor sets failed") } return false }
   _upload_cb = 0
   _upload_alloc = sys_malloc(32)
   _upload_bi = sys_malloc(32)
   _upload_bar1 = sys_malloc(72)
   _upload_bar2 = sys_malloc(72)
   _upload_region = sys_malloc(56)
   _upload_si = sys_malloc(72)
   _upload_cb_arr = sys_malloc(8)
   _upload_cb_ptr = sys_malloc(8)
   _flush_off = sys_malloc(8)
   _flush_buf = sys_malloc(8)

   mut fence_ci = sys_malloc(16)
   memset(fence_ci, 0, 16)
   store32(fence_ci, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
   _upload_fence_ptr = sys_malloc(8)
   create_fence(_device, fence_ci, 0, _upload_fence_ptr)
   _upload_fence = load64(_upload_fence_ptr, 0)
   sys_free(fence_ci)

   _current_mvp = sys_malloc(64)
   _current_model = sys_malloc(64)
   _pc_buffer   = sys_malloc(160)
   _pc_buffer_custom = sys_malloc(160) ;; Separate buffer for custom pipelines
   memset(_pc_buffer, 0, 160)
   memset(_pc_buffer_custom, 0, 160)
   _ident_mat = mat4_identity()

   _ptr_fence = sys_malloc(8)
   _ptr_img_idx = sys_malloc(4)
   _ptr_bi = sys_malloc(64)
   _ptr_clear = sys_malloc(96) ; 3 * 32 bytes max for MSAA clear values
   _ptr_ri = sys_malloc(128)
   _ptr_vp = sys_malloc(32)
   _ptr_sci = sys_malloc(32)
   _ptr_dsl = sys_malloc(8)
   _ptr_ds = sys_malloc(16)
   _ptr_sub = sys_malloc(128)
   _ptr_wait_sems = sys_malloc(32)
   _ptr_sig_sems = sys_malloc(32)
   _ptr_stages = sys_malloc(128)

   _quad_template = sys_malloc(_VKR_VERT_STRIDE * 6)
   _init_quad_template()

   if(!_create_default_texture()){ if(_debug_gfx_enabled){ print("Vulkan: Default texture failed") } return false }
   if(_debug_gfx_enabled){ print("Vulkan: Default texture OK") }

   _static_vbo_ptr = sys_malloc(8)
   _static_off_ptr = sys_malloc(8)
   store64_raw(_static_off_ptr, 0, 0)

   _update_default_mvp(_window_ref)
   true
}

fn _update_default_mvp(win){
   "Recalculates the default orthographic projection matrix for the window/swapchain."
   mut w = float(_swapchain_extent_w)
   mut h = float(_swapchain_extent_h)
   if(win){
      w = float(dict_get(win, "w", w))
      h = float(dict_get(win, "h", h))
   }
   if(int(w) == _last_mvp_w && int(h) == _last_mvp_h){ return }
   _last_mvp_w = int(w)
   _last_mvp_h = int(h)
   ; Standard 2D coordinate system: (0,0) is top-left, (w,h) is bottom-right.
   set_ortho(0.0, w, 0.0, h, -1.0, 1.0)
}

fn _mvp_matrix(){
   "Returns the current internal MVP matrix, as set by begin_mode_3d."
   mut m = mat4_identity()
   if(_current_mvp){ mat4_from_buffer(m, _current_mvp) }
   return m
}

fn set_model_matrix(mat){
   "Updates the Model matrix for subsequent 3D draw calls."
   if(_current_model && is_list(mat)){
      if(_vertex_offset != _last_flush_offset){ _flush() }
      mat4_to_buffer(mat, _current_model)
      _model_dirty = true
      _pc_dirty = true
   }
}

fn set_mvp(mat){
   "Updates the View-Projection matrix for the renderer."
   if(_current_mvp && is_list(mat)){
      if(_vertex_offset != _last_flush_offset){ _flush() }
      mat4_to_buffer(mat, _current_mvp)
      _mvp_dirty = true
      _pc_dirty = true
   }
}

fn set_ortho(l, r, b, t, n, f){
   "Sets the MVP matrix to an orthographic projection."
   if(b < t){ def tmp = b b = t t = tmp }
   def mat = mat4_ortho(l, r, b, t, n, f)
   set_mvp(mat)
}

fn set_perspective(fovy, aspect, near, far){
   "Sets the View-Projection matrix to a perspective projection."
   def mat = mat4_perspective(fovy, aspect, near, far)
   set_mvp(mat)
}

fn begin_frame(){
   "Prepares the renderer for a new frame (sync, acquire image, begin recording)."
   if(!_device){ return false }

   if(_window_ref){
      mut cur_ww = int(dict_get(_window_ref, "w", _swapchain_extent_w))
      mut cur_wh = int(dict_get(_window_ref, "h", _swapchain_extent_h))

      ;; Handle minimization: spin until window is restored
      if(cur_ww == 0 || cur_wh == 0){
         while(cur_ww == 0 || cur_wh == 0){
         if(!_window_ref){ return false }
         msleep(10) ; yield to OS
         ;; Note: must poll here if not already doing so on main thread, but assume ui.ny does it.
         ;; Re-read size from dict (updated by GLFW callback in std.ui.window)
         cur_ww = int(dict_get(_window_ref, "w", 0))
         cur_wh = int(dict_get(_window_ref, "h", 0))
         }
      }

      if(cur_ww != _swapchain_extent_w || cur_wh != _swapchain_extent_h){
         if(_debug_gfx_enabled){ print(f"Vulkan: Window resized {cur_ww}x{cur_wh}") }
         if(!_recreate_swapchain()){ return false }
      }
   }

   _frame_open = false

   ; Wait for previous frame's fence
   def fence = get(_in_flight_fences, _current_frame)
   store64_raw(_ptr_fence, fence, 0)
   def wf = wait_for_fences(_device, 1, _ptr_fence, 1, 0xFFFFFFFFFFFFFFFF)
   if(wf != 0){ return false }
   ; Reset the fence BEFORE recording starts to avoid driver race
   reset_fences(_device, 1, _ptr_fence)

   ; Capture current image index
   mut acq = 0
   def sem = get(_image_available_semaphores, _current_frame)
   if(_surface){
      acq = acquire_next_image_khr(_device, _swapchain, 0xFFFFFFFFFFFFFFFF, sem, 0, _ptr_img_idx)
      if(acq == 0xC460C464 || acq == -1000001004){
         if(_debug_gfx_enabled){ print("Vulkan: Acquire next image out of date") }
         _recreate_swapchain()
         return false
      }
      if(acq != 0 && acq != 1000001003){ return false }
      _image_index = load32(_ptr_img_idx, 0)
   } else {
      _image_index = (_image_index + 1) % _swapchain_image_count
   }

   ; Reset + begin recording command buffer
   def cb = get(_command_buffers, _current_frame)
   memset(_ptr_bi, 0, 32)
   store32(_ptr_bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(_ptr_bi, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 16)
   if(begin_command_buffer(cb, _ptr_bi) != 0){ return false }
   if(_vk_markers_enabled){ vk_debug_marker_begin(cb, "Frame " + to_str(_total_frames), 0xFFFFFFFF) }

   ; Begin Render Pass
   ; Set clear values: color + depth (+ resolve slot if MSAA)
   ; VkClearValue is 16 bytes each: clear[0]=color@0, clear[1]=depth@16, clear[2]=resolve@32
   memset(_ptr_clear, 0, 96)
   store32_f32(_ptr_clear, _clear_r, 0) ; clear[0].color.r
   store32_f32(_ptr_clear, _clear_g, 4) ; clear[0].color.g
   store32_f32(_ptr_clear, _clear_b, 8) ; clear[0].color.b
   store32_f32(_ptr_clear, _clear_a, 12) ; clear[0].color.a
   store32_f32(_ptr_clear, 1.0, 16) ; clear[1].depthStencil.depth = 1.0
   store32(_ptr_clear, 0, 20) ; clear[1].depthStencil.stencil = 0

   def clear_count = (_cfg_msaa > 1) ? 3 : 2

   memset(_ptr_ri, 0, 64)
   store32(_ptr_ri, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0)
   store64_raw(_ptr_ri, _render_pass, 16)
   store64_raw(_ptr_ri, get(_framebuffers, _image_index), 24)
   store32(_ptr_ri, _swapchain_extent_w, 40)
   store32(_ptr_ri, _swapchain_extent_h, 44)
   store32(_ptr_ri, clear_count, 48)
   store64_raw(_ptr_ri, _ptr_clear, 56)
   if(_vk_markers_enabled){ vk_debug_marker_begin(cb, "RenderPass", 0x3366FFFF) }
   cmd_begin_render_pass(cb, _ptr_ri, 0)

   ; Vulkan Y-flip via negative viewport height (Maintenance 1)
   store32_f32(_ptr_vp, 0.0, 0)
   store32_f32(_ptr_vp, float(_swapchain_extent_h), 4)
   store32_f32(_ptr_vp, float(_swapchain_extent_w), 8)
   store32_f32(_ptr_vp, -float(_swapchain_extent_h), 12)
   store32_f32(_ptr_vp, 0.0, 16)
   store32_f32(_ptr_vp, 1.0, 20)
   cmd_set_viewport(cb, 0, 1, _ptr_vp)

   store32(_ptr_sci, 0, 0)
   store32(_ptr_sci, 0, 4)
   store32(_ptr_sci, _swapchain_extent_w, 8)
   store32(_ptr_sci, _swapchain_extent_h, 12)
   cmd_set_scissor(cb, 0, 1, _ptr_sci)

   _frame_open = true
   _current_frame_cb = cb
   _current_frame_ubo_ds = get(_ubo_descriptor_sets, _current_frame, 0)
   _total_frames += 1
   _fps_count += 1
   def now_t = get_time()
   if(now_t - _fps_last_time >= 1.0){
      _fps_curr = _fps_count
      _fps_count = 0
      _fps_last_time = now_t
   }

   _update_default_mvp(_window_ref)

   ; Reset per-frame vertex and state tracking
   _vertex_offset = 0
   _last_flush_offset = 0
   _vertex_limit_hit = false
   _current_frame_vertex_offset = _current_frame * _vertex_capacity
   if(_vertex_map){ _local_vertex_map = _vertex_map + _current_frame_vertex_offset }

   ; MUST reset these so bind_texture and _flush re-issue commands to the NEW command buffer
   _last_bound_ds = 0
   _last_bound_tex_id = -1
   _last_bound_pipe = 0
   _last_bound_ubo_ds = 0
   _target_pipeline = _pipeline ; Default to main pipeline
   _current_texture_id = -1 ; Force next bind_texture to actually do work
   _current_tex_index = 0
   _mvp_dirty = true
   _pc_dirty = true
   _last_is_mask = 0

   ; Initial pipeline and common state
   cmd_bind_pipeline(cb, 0, _pipeline)
   _last_bound_pipe = _pipeline

   ; Bind vertex buffer ONCE for this frame's slice — not again until draw_lines_raw
   store64_raw(_flush_off, _current_frame_vertex_offset, 0)
   store64_raw(_flush_buf, _vertex_buffer, 0)
   cmd_bind_vertex_buffers(cb, 0, 1, _flush_buf, _flush_off)

   ; Dynamic state already set above

   memcpy(_pc_buffer, _current_mvp, 64)
   mat4_to_buffer(_ident_mat, _current_model)
   memcpy(_pc_buffer + 64, _current_model, 64)
   store32(_pc_buffer, 0, 128)
   store32(_pc_buffer, _current_is_unlit, 132)
   _mvp_dirty = false
   _model_dirty = false
   _pc_dirty = true ;; Force push on first draw of frame
   _last_is_mask = 0
   _last_is_unlit = _current_is_unlit
   true
}

fn set_unlit(unlit){
   "Toggles lighting for subsequent draw calls."
   def val = unlit ? 1 : 0
   if(val != _current_is_unlit){
      _flush()
      _current_is_unlit = val
      _last_is_unlit = val
      _pc_dirty = true
   }
}

fn set_mask(m){
   "Toggles mask rendering (alpha-only texture) for subsequent draw calls."
   def val = m ? 1 : 0
   if(val != _last_is_mask){
      _flush()
      _last_is_mask = val
      _pc_dirty = true
   }
}

fn _sync_pc(){
   "Internal: Synchronizes per-draw constants with the GPU if dirty."
   ;; Use custom push constant buffer for custom pipelines
   def pc_ptr = _use_custom_pc ? _pc_buffer_custom : _pc_buffer

   if(!_use_custom_pc){
      ;; Standard engine pipeline - sync engine state
      if(_current_is_unlit != _last_is_unlit){ _last_is_unlit = _current_is_unlit _pc_dirty = true }
      if(_mvp_dirty || _model_dirty){ _pc_dirty = true }
      if(!_pc_dirty){ return }
      if(_mvp_dirty){ memcpy(_pc_buffer, _current_mvp, 64) _mvp_dirty = false }
      if(_model_dirty){ memcpy(_pc_buffer + 64, _current_model, 64) _model_dirty = false }

      store32(_pc_buffer, _last_is_mask, 128)
      store32(_pc_buffer, _last_is_unlit, 132)
      store32(_pc_buffer, 0, 136)
      store32(_pc_buffer, 0, 140)
   }

   def cb = _current_frame_cb
   if(cb && _pipeline_layout){
      cmd_push_constants(cb, _pipeline_layout, 17, 0, 160, pc_ptr)
   }
   _pc_dirty = false
}

fn _flush(){
   "Records a draw call for current pending triangle batch."
   if(!_frame_open){ return }
   if(_vertex_offset == _last_flush_offset){ return }
   def t0 = _vk_profile_flush ? ticks() : 0

   def count = (_vertex_offset - _last_flush_offset) / _VKR_VERT_STRIDE
   def first_vert = _last_flush_offset / _VKR_VERT_STRIDE

   def cb = _current_frame_cb

      ; Select appropriate triangle pipeline based on unlit state
   mut base_pipe = _pipeline
   if(_current_is_unlit != 0 && _unlit_pipeline != 0){ base_pipe = _unlit_pipeline }

   mut target = _target_pipeline
   if(target == _pipeline){ target = base_pipe } ; if target is default, use our unlit-aware base

   if(_is_wireframe && _wire_pipeline != 0){
      if(target == _pipeline || target == _unlit_pipeline){ target = _wire_pipeline }
   }

   if(_last_bound_pipe != target){
       cmd_bind_pipeline(cb, 0, target)
       _last_bound_pipe = target
       _pc_dirty = true
   }

   ; Bind Descriptor Sets only when changed
   def ubo_ds = _current_frame_ubo_ds
   if(_bindless_enabled){
      if(_bindless_ds && ( _bindless_ds != _last_bound_ds || ubo_ds != _last_bound_ubo_ds )){
         store64_raw(_ptr_ds, _bindless_ds, 0)
         store64_raw(_ptr_ds, ubo_ds, 8)
         cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, _ubo_enabled ? 2 : 1, _ptr_ds, 0, 0)
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
         cmd_bind_descriptor_sets(cb, 0, _pipeline_layout, 0, _ubo_enabled ? 2 : 1, _ptr_ds, 0, 0)
         _last_bound_ds = ds
         _last_bound_tex_id = tid
         _last_bound_ubo_ds = ubo_ds
      }
   }

   ; Push constants only when matrix, model, mask, or unlit changed
   _sync_pc()

   ; Depth state depends on unlit
   ; Simple way: just clear depth for every flush if unlit? No, too slow.
   ; Real way: we need a separate pipeline for 2D.
   ; For now, UI test calls clear_depth() which is fine.

   ; VBO is already bound in begin_frame — just draw using first_vert index
   if(count > 0){
      if(_vk_markers_enabled){ vk_debug_marker_begin(cb, "Flush Batch", 0x00FF00FF) }
      cmd_draw(cb, count, 1, first_vert, 0)
      if(_vk_markers_enabled){ vk_debug_marker_end(cb) }
      _total_draw_calls += 1
      _frame_draw_calls += 1
   }
   _last_flush_offset = _vertex_offset

   if(_vk_profile_flush){
      def t1 = ticks()
      _prof_flush_total += float(t1 - t0)
      _prof_flush_count += 1
      _prof_flush_avg = _prof_flush_total / float(_prof_flush_count)
   }
}

fn _check_flush(bytes){
   "Ensures enough space in the current frame buffer slice."
   if(_vertex_limit_hit){ return false }
   if(_vertex_offset + bytes > _vertex_capacity){
      _flush()
      if(_vertex_offset + bytes > _vertex_capacity){
          if(_debug_gfx_enabled){ print("Vulkan: VERTEX BUFFER FULL for current frame!") }
          _vertex_limit_hit = true
          return false
      }
   }
   true
}

fn end_frame(){
   "Finalizes rendering and presents the frame to the swapchain image."
   _end_frame_internal(true)
}

fn _end_frame_internal(present){
   "Finalizes command recording and triggers vertex upload."
   if(!_frame_open){ return false }
   _flush()

   ; Vertex data is written directly into the persistently-mapped GPU buffer.
   def cb = get(_command_buffers, _current_frame)
   cmd_end_render_pass(cb)
   if(_vk_markers_enabled){
      vk_debug_marker_end(cb) ;; End RenderPass
      vk_debug_marker_end(cb) ;; End Frame
   }
   def ecb = end_command_buffer(cb)
   if(ecb != 0){
      return false
   }

   def sem_avail = get(_image_available_semaphores, _current_frame)
   def sem_finish = get(_render_finished_semaphores, _current_frame)

   store64_raw(_ptr_wait_sems, sem_avail, 0)
   store64_raw(_ptr_sig_sems, sem_finish, 0)
   store32(_ptr_stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0)

   memset(_ptr_sub, 0, 128)
   store32(_ptr_sub, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(_ptr_sub, 1, 16) ; waitSemaphoreCount
   store64_raw(_ptr_sub, _ptr_wait_sems, 24)
   store64_raw(_ptr_sub, _ptr_stages, 32)
   store32(_ptr_sub, 1, 40) ; commandBufferCount
   mut cb_ptr = _ptr_sub + 80 ;; Reuse end of buffer for cb array
   store64_raw(cb_ptr, cb, 0)
   store64_raw(_ptr_sub, cb_ptr, 48)
   store32(_ptr_sub, 1, 56) ; signalSemaphoreCount
   store64_raw(_ptr_sub, _ptr_sig_sems, 64)

   def fence = get(_in_flight_fences, _current_frame)
   def sub_res = queue_submit(_graphics_queue, 1, _ptr_sub, fence)
   if(sub_res != 0){
      return false
   }

   if(present){
      def sc = _swapchain
      def img_idx = _image_index

      mut scs = _ptr_ri ;; Reuse ri buffer for swapchain array
      store64_raw(scs, sc, 0)
      mut idxs = scs + 8
      store32(idxs, img_idx, 0)

      mut pi = _ptr_ri + 32 ;; Reuse ri buffer for present info
      memset(pi, 0, 64)
      store32(pi, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0)
      store32(pi, 1, 16) ; waitSemaphoreCount
      store64_raw(pi, _ptr_sig_sems, 24)
      store32(pi, 1, 32) ; swapchainCount
      store64_raw(pi, scs, 40)
      store64_raw(pi, idxs, 48)

      def pr = queue_present_khr(_present_queue, pi)
      if(pr == 0xC460C464 || pr == -1000001004 || pr == 1000001003){ ; OUT_OF_DATE or SUBOPTIMAL
         _frame_open = false
         _recreate_swapchain()
         return false
      }
   }

   _frame_open = false
   _current_frame = (_current_frame + 1) % MAX_FRAMES_IN_FLIGHT
   true
}

fn clear(r, g, b, a){
   "Commands the GPU to clear the current color attachment."
   if(!_frame_open){ return 0 }
   if(!_clear_ca){ _clear_ca = sys_malloc(24) _clear_rect = sys_malloc(24) }
   def cb = get(_command_buffers, _current_frame)
   store32(_clear_ca, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(_clear_ca, 0, 4)
   store32_f32(_clear_ca, r, 8)
   store32_f32(_clear_ca, g, 12)
   store32_f32(_clear_ca, b, 16)
   store32_f32(_clear_ca, a, 20)
   store32(_clear_rect, 0, 0) store32(_clear_rect, 0, 4)
   store32(_clear_rect, _swapchain_extent_w, 8) store32(_clear_rect, _swapchain_extent_h, 12)
   store32(_clear_rect, 0, 16)
   store32(_clear_rect, 1, 20)
   cmd_clear_attachments(cb, 1, _clear_ca, 1, _clear_rect)
}

fn clear_depth(){
   "Clears the depth buffer, ensuring subsequent depth passes render correctly over past layers."
   if(!_frame_open){ return 0 }
   _flush() ; Flush pending vertex geometry to ensure it writes before clear
   if(!_clear_ca){ _clear_ca = sys_malloc(24) _clear_rect = sys_malloc(24) }
   def cb = get(_command_buffers, _current_frame)
   store32(_clear_ca, 2, 0) ; VK_IMAGE_ASPECT_DEPTH_BIT
   store32(_clear_ca, 0, 4) ; colorAttachment ignored
   store32_f32(_clear_ca, 1.0, 8) ; depth
   store32(_clear_ca, 0, 12) ; stencil
   store32(_clear_rect, 0, 0) store32(_clear_rect, 0, 4)
   store32(_clear_rect, _swapchain_extent_w, 8) store32(_clear_rect, _swapchain_extent_h, 12)
   store32(_clear_rect, 0, 16)
   store32(_clear_rect, 1, 20)
   cmd_clear_attachments(cb, 1, _clear_ca, 1, _clear_rect)
}

fn set_clear_color(r, g, b, a=1.0){
   "Sets the clear color for the next begin_frame."
   _clear_r = float(r) _clear_g = float(g) _clear_b = float(b) _clear_a = float(a)
}

fn shutdown(){
   "Shuts down the Vulkan renderer and releases all associated resources."
   if(!_device){
      if(_surface){ destroy_surface_khr(_instance, _surface, 0) }
      if(_instance){ destroy_instance(_instance, 0) }
      return
   }
   device_wait_idle(_device)
   if(_vertex_buffer){ destroy_buffer(_device, _vertex_buffer, 0) }
   if(_ubo_buffer){ destroy_buffer(_device, _ubo_buffer, 0) }
   if(_depth_image){ destroy_image(_device, _depth_image, 0) }
   if(_depth_view){ destroy_image_view(_device, _depth_view, 0) }
   if(_vertex_memory){ free_memory(_device, _vertex_memory, 0) }
   if(_ubo_memory){ free_memory(_device, _ubo_memory, 0) }
   if(_staging_buffer){ destroy_buffer(_device, _staging_buffer, 0) }
   if(_staging_memory){ free_memory(_device, _staging_memory, 0) }
   if(_default_sampler){ destroy_sampler(_device, _default_sampler, 0) }
   if(_descriptor_pool){ destroy_descriptor_pool(_device, _descriptor_pool, 0) }

   mut i = 0
   while(i < len(_textures)){
      def tex = get(_textures, i)
      def view = dict_get(tex, "view", 0)
      def img = dict_get(tex, "image", 0)
      def mem = dict_get(tex, "memory", 0)
      if(view){ destroy_image_view(_device, view, 0) }
      if(img){ destroy_image(_device, img, 0) }
      if(mem){ free_memory(_device, mem, 0) }
      i += 1
   }
   _textures = []

   _destroy_swapchain_objects()
   if(_device){ destroy_device(_device, 0) }
   if(_surface){ destroy_surface_khr(_instance, _surface, 0) }
   if(_instance){ destroy_instance(_instance, 0) }
}

fn set_wireframe(enabled){
   "Enables or disables wireframe rendering globally."
   _is_wireframe = !!enabled
   if(_vertex_offset != _last_flush_offset){ _flush() }
}
fn _create_instance(){
   "Creates the Vulkan instance."
   ; Create all structures with system malloc to avoid any Nytrix metadata issues
   mut app_info = sys_malloc(48)
   memset(app_info, 0, 48)
   store32(app_info, VK_STRUCTURE_TYPE_APPLICATION_INFO, 0)
   store32(app_info, 1, 24)
   store32(app_info, 1, 40)
   store32(app_info, 0x00402000, 44) ; VK_API_VERSION_1_2

   def exts_list = ui_glfw.required_extensions()
   mut e_count = get(exts_list, 0)
   def e_ptrs  = get(exts_list, 1)

   ; Check if VK_EXT_debug_utils is available before enabling
   mut debug_utils_ok = false
   mut ext_count_ptr = sys_malloc(4)
   store32(ext_count_ptr, 0, 0)
   if(enumerate_instance_extension_properties(0, ext_count_ptr, 0) == 0){
      def ext_count = load32(ext_count_ptr, 0)
      if(ext_count > 0){
         mut ext_props = sys_malloc(ext_count * 260)
         if(enumerate_instance_extension_properties(0, ext_count_ptr, ext_props) == 0){
         mut i = 0
         while(i < ext_count){
               def name = text.cstr_to_str(ext_props + i * 260)
               if(name && eq(name, "VK_EXT_debug_utils")){ debug_utils_ok = true break }
               i += 1
         }
         }
         sys_free(ext_props)
      }
   }
   sys_free(ext_count_ptr)

   mut extra_exts = debug_utils_ok ? 1 : 0
   mut all_ext_ptrs = sys_malloc((e_count + extra_exts) * 8)
   mut i = 0 while(i < e_count){ store64_raw(all_ext_ptrs, load64(e_ptrs, i * 8), i * 8) i += 1 }
   if(debug_utils_ok){
      mut debug_ext_name = sys_malloc(32) strcpy(debug_ext_name, "VK_EXT_debug_utils")
      store64_raw(all_ext_ptrs, debug_ext_name, e_count * 8)
      e_count += 1
   } else {
      if(_debug_gfx_enabled){ print("Vulkan: VK_EXT_debug_utils not available") }
   }

   ; If RenderDoc is present, try to enable its capture layer when available
   mut layer_ptrs = 0
   mut layer_count = 0
   if(env("RENDERDOC") || env("RENDERDOC_CAPTUREOPTS") || env("RENDERDOC_CMD")){
      def env_layers = env("VK_INSTANCE_LAYERS")
      if(!env_layers || env_layers == ""){
         mut lc_ptr = sys_malloc(4)
         store32(lc_ptr, 0, 0)
         if(enumerate_instance_layer_properties(lc_ptr, 0) == 0){
         def lc = load32(lc_ptr, 0)
         if(lc > 0){
               mut lprops = sys_malloc(lc * 260)
               if(enumerate_instance_layer_properties(lc_ptr, lprops) == 0){
                  mut j = 0
                  while(j < lc){
                     def lname = text.cstr_to_str(lprops + j * 260)
                     if(lname && eq(lname, "VK_LAYER_RENDERDOC_Capture")){
                  mut layer_name = sys_malloc(40)
                  strcpy(layer_name, "VK_LAYER_RENDERDOC_Capture")
                  layer_ptrs = sys_malloc(8)
                  store64_raw(layer_ptrs, layer_name, 0)
                  layer_count = 1
                  if(_debug_gfx_enabled){ print("Vulkan: Enabled VK_LAYER_RENDERDOC_Capture") }
                  break
                     }
                     j += 1
                  }
               }
               sys_free(lprops)
         }
         }
         sys_free(lc_ptr)
      }
   }

   ; Create VkInstanceCreateInfo manually with explicit zeroing
   mut create_info = sys_malloc(64)
   memset(create_info, 0, 64)
   store32(create_info, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0)
   store64_raw(create_info, app_info, 24)
   store32(create_info, layer_count, 32) ; enabledLayerCount
   store64_raw(create_info, layer_ptrs, 40) ; ppEnabledLayerNames
   store32(create_info, e_count, 48) ; extensions
   store64_raw(create_info, all_ext_ptrs, 56)
   mut inst_ptr = sys_malloc(8)
   store32(inst_ptr, 0, 0) store32(inst_ptr, 0, 4)

   if(_debug_gfx_enabled){
      print("Vulkan: Creating instance with wrapper...")
      ; print("Vulkan: resolve vk_create_instance = " + to_str(vk_create_instance)) ; Removed debug print
   }

   def res = vk_create_instance(create_info, 0, inst_ptr)

   if(_debug_gfx_enabled){
      print("Vulkan: create_instance returned " + to_str(res))
      ; print("Vulkan: inst_ptr[0] = " + to_str(load64(inst_ptr, 0))) ; Removed debug print
   }

   if(res != 0){
      return false
   }
   _instance = load64(inst_ptr, 0)
   if(_debug_gfx_enabled){
      print("Vulkan: Instance created OK.")
      _dbg_handle("instance", _instance)
   }
   if(vk_using_dispatch()){
      vk_load_instance_functions(_instance)
   }
   true
}

fn _create_surface(win){
   "Creates the native window surface (WSI)."
   def window = dict_get(win, "handle", 0)
   if(!window){
      return false
   }
   mut surf_ptr = sys_malloc(8)
   store32(surf_ptr, 0, 0) ; store32(surf_ptr, 0, 4)
   def res = ui_glfw.create_surface(_instance, window, 0, surf_ptr)
   if(res != 0){
      return false
   }
   _surface = load64(surf_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("surface", _surface) }
   true
}

fn _pick_physical_device(){
   "Selects a suitable physical GPU for rendering."
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   def _res1 = enumerate_physical_devices(_instance, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(count == 0){ return false }
   def _res2 = enumerate_physical_devices(_instance, count_ptr, 0) ; Added _res2 to avoid warning
   mut devices_ptr = sys_malloc(count * 8)
   enumerate_physical_devices(_instance, count_ptr, devices_ptr)
   _physical_device = load64(devices_ptr, 0)

   mut props = sys_malloc(1024)
   memset(props, 0, 1024)
   get_physical_device_properties(_physical_device, props)
   def device_name = text.cstr_to_str(props, 20)
   if(_debug_gfx_enabled){
      print("Vulkan: Selected GPU:", device_name)
      _dbg_handle("physical", _physical_device)
   }
   sys_free(devices_ptr)
   sys_free(props)
   true
}

fn _create_logical_device(){
   "Creates the logical Vulkan device and retrieves queues."
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   get_physical_device_queue_family_properties(_physical_device, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(count == 0){ return false }
   def prop_stride = 24
   mut props = sys_malloc(count * prop_stride)
   get_physical_device_queue_family_properties(_physical_device, count_ptr, props)
   mut graphics_family = -1
   mut i = 0
   while(i < count){
      def flags = load32(props, i * prop_stride)
      if((flags & 1) != 0){ ; VK_QUEUE_GRAPHICS_BIT
         graphics_family = i
         break
      }
      i += 1
   }
   if(graphics_family == -1){
      return false
   }
   _graphics_family_index = graphics_family
   ; Queue priority (1.0f in IEEE-754)
   mut priorities = sys_malloc(4)
   store32(priorities, 0x3f800000, 0)
   mut queue_create_info = sys_malloc(40)
   memset(queue_create_info, 0, 40)
   store32(queue_create_info, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0)
   store32(queue_create_info, 0, 8) store32(queue_create_info, 0, 12) ; pNext
   store32(queue_create_info, 0, 16) ; flags
   store32(queue_create_info, graphics_family, 20) ; queueFamilyIndex
   store32(queue_create_info, 1, 24) ; queueCount
   store64_raw(queue_create_info, priorities, 32) ; pQueuePriorities
   mut attempt = 0
   mut res = -1
   mut dev_ptr = sys_malloc(8)
   while(true){
      mut ext1 = sys_malloc(32)
      strcpy(ext1, "VK_KHR_swapchain")
      mut ext_ptrs = 0
      if(_bindless_enabled){
         mut ext2 = sys_malloc(40)
         mut ext3 = sys_malloc(40)
         strcpy(ext2, "VK_EXT_descriptor_indexing")
         strcpy(ext3, "VK_KHR_maintenance3")
         ext_ptrs = sys_malloc(24)
         store64_raw(ext_ptrs, ext1, 0)
         store64_raw(ext_ptrs, ext2, 8)
         store64_raw(ext_ptrs, ext3, 16)
      } else {
         ext_ptrs = sys_malloc(8)
         store64_raw(ext_ptrs, ext1, 0)
      }
      mut create_info = sys_malloc(72)
      memset(create_info, 0, 72)
      store32(create_info, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0)
      store32(create_info, 0, 8) store32(create_info, 0, 12) ; pNext
      store32(create_info, 0, 16) ; flags
      store32(create_info, 1, 20) ; queueCreateInfoCount
      store64_raw(create_info, queue_create_info, 24) ; pQueueCreateInfos
      store32(create_info, 0, 32) ; enabledLayerCount
      store32(create_info, 0, 40) store32(create_info, 0, 44) ; ppEnabledLayerNames
      store32(create_info, _bindless_enabled ? 3 : 1, 48) ; enabledExtensionCount
      store64_raw(create_info, ext_ptrs, 56) ; ppEnabledExtensionNames

      store32(create_info, 0, 64) store32(create_info, 0, 68) ; pEnabledFeatures (set below)
      ; Enable wideLines, fillModeNonSolid, shaderFloat64, and shaderInt64
      mut dev_features = sys_malloc(232)
      memset(dev_features, 0, 232)
      store32(dev_features, 1, 52) ; fillModeNonSolid = VK_TRUE
      store32(dev_features, 1, 60) ; wideLines = VK_TRUE
      store32(dev_features, 1, 156) ; shaderFloat64 = VK_TRUE
      store32(dev_features, 1, 160) ; shaderInt64 = VK_TRUE
      store64_raw(create_info, dev_features, 64)

      ; Descriptor indexing features (bindless)
      if(_bindless_enabled){
         mut di_feat = sys_malloc(104)
         memset(di_feat, 0, 104)
         store32(di_feat, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, 0)
         store32(di_feat, 1, 32) ; shaderSampledImageArrayNonUniformIndexing
         store32(di_feat, 1, 48) ; descriptorBindingSampledImageUpdateAfterBind
         store32(di_feat, 1, 68) ; descriptorBindingUpdateUnusedWhilePending
         store32(di_feat, 1, 72) ; descriptorBindingPartiallyBound
         store32(di_feat, 1, 92) ; runtimeDescriptorArray
         store64_raw(create_info, di_feat, 8)
      }
      store32(dev_ptr, 0, 0) store32(dev_ptr, 0, 4)
      res = create_device(_physical_device, create_info, 0, dev_ptr)

      if(res == 0){ break }
      if(_bindless_enabled && attempt == 0){
         if(_debug_gfx_enabled){ print("Vulkan: bindless create_device failed, retrying without bindless") }
         _bindless_enabled = false
         attempt = 1
         continue
      }
      if(_debug_gfx_enabled){ print(f"Vulkan: create_device failed with {res}") }
      return false
   }
   _device = load64(dev_ptr, 0)
   if(_debug_gfx_enabled){
      print(f"Vulkan: Logical device created OK")
      _dbg_handle("device", _device)
   }
   if(vk_using_dispatch()){
      vk_load_device_functions(_device)
   }
   mut q_ptr = sys_malloc(8)
   store32(q_ptr, 0, 0) store32(q_ptr, 0, 4)
   get_device_queue(_device, graphics_family, 0, q_ptr)
   _graphics_queue = load64(q_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("queue", _graphics_queue) }
   ; Use same queue for presenting for now (most GPUs support this)
   _present_queue = _graphics_queue
   true
}

fn _choose_composite_alpha(supported_flags){
   if(band(supported_flags, 2)){ return 2 } ; PRE_MULTIPLIED
   if(band(supported_flags, 1)){ return 1 } ; OPAQUE
   if(band(supported_flags, 8)){ return 8 } ; INHERIT
   1
}

fn _choose_present_mode(){
   "Chooses the fastest present mode available (MAILBOX > IMMEDIATE > FIFO)."
   mut count_ptr = sys_malloc(4)
   get_physical_device_surface_present_modes_khr(_physical_device, _surface, count_ptr, 0)
   def count = load32(count_ptr, 0)
   mut modes_ptr = sys_malloc(count * 4)
   get_physical_device_surface_present_modes_khr(_physical_device, _surface, count_ptr, modes_ptr)

   mut mailbox_supported = false
   mut immediate_supported = false
   mut i = 0
   while(i < count){
      def mode = load32(modes_ptr, i * 4)
      if(mode == VK_PRESENT_MODE_MAILBOX_KHR){ mailbox_supported = true }
      if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR){ immediate_supported = true }
      i += 1
   }
   sys_free(count_ptr)
   sys_free(modes_ptr)

   if(_cfg_vsync){
      return VK_PRESENT_MODE_FIFO_KHR
   } else {
      if(immediate_supported){ return VK_PRESENT_MODE_IMMEDIATE_KHR }
      if(mailbox_supported){ return VK_PRESENT_MODE_MAILBOX_KHR }
      return VK_PRESENT_MODE_FIFO_KHR
   }
}

fn _create_headless_image(w, h){
   "Internal: creates an offscreen color image for headless rendering."
   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 1, 20) ; 2D
   store32(img_ci, 37, 24) ; RGBA8
   store32(img_ci, w, 28)
   store32(img_ci, h, 32)
   store32(img_ci, 1, 36) ; depth
   store32(img_ci, 1, 40) ; mip
   store32(img_ci, 1, 44) ; layers
   store32(img_ci, 1, 48) ; samples
   store32(img_ci, 0, 52) ; tiling optimal
   store32(img_ci, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 56)
   store32(img_ci, 0, 60) ; sharing exclusive
   store32(img_ci, 0, 80) ; layout undefined

   mut p = sys_malloc(8)
   create_image(_device, img_ci, 0, p)
   def img = load64(p, 0)

   mut req = sys_malloc(24)
   get_image_memory_requirements(_device, img, req)
   def mem_type = _find_memory_type(load32(req, 16), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut ai = sys_malloc(64)
   memset(ai, 0, 64)
   store32(ai, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(ai, load64(req, 0), 16)
   store32(ai, mem_type, 24)
   allocate_memory(_device, ai, 0, p)
   def mem = load64(p, 0)
   bind_image_memory(_device, img, mem, 0)
   img
}

fn _create_swapchain(win){
   "Initializes the Vulkan swapchain or simulated images for headless mode."
   if(!_surface){
      ; Headless: Create 3 simulated images
      _swapchain_extent_w = 400
      _swapchain_extent_h = 300
      if(win){
         _swapchain_extent_w = dict_get(win, "w", 400)
         _swapchain_extent_h = dict_get(win, "h", 300)
      }
      _swapchain_format = 37 ; RGBA8
      _swapchain_image_count = 3
      _swapchain_images = []
      mut i = 0
      while(i < 3){
         _swapchain_images = push(_swapchain_images, _create_headless_image(_swapchain_extent_w, _swapchain_extent_h))
         i += 1
      }
      return true
   }
   mut caps = sys_malloc(128)
   memset(caps, 0, 128)
   get_physical_device_surface_capabilities_khr(_physical_device, _surface, caps)
   mut req_w = 400
   mut req_h = 300
   if(win){
      req_w = int(dict_get(win, "w", 400))
      req_h = int(dict_get(win, "h", 300))
   }
   def cur_w = load32(caps, 8)
   def cur_h = load32(caps, 12)
   def min_w = load32(caps, 16)
   def min_h = load32(caps, 20)
   def max_w = load32(caps, 24)
   def max_h = load32(caps, 28)
   mut w = req_w
   mut h = req_h
   if(cur_w != -1 && cur_h != -1 && cur_w > 0 && cur_h > 0){
      w = cur_w
      h = cur_h
   } else {
      if(w < min_w){ w = min_w }
      if(h < min_h){ h = min_h }
      if(max_w > 0 && w > max_w){ w = max_w }
      if(max_h > 0 && h > max_h){ h = max_h }
   }
   _swapchain_extent_w = w
   _swapchain_extent_h = h
   mut min_imgs = load32(caps, 0)
   mut max_imgs = load32(caps, 4)
   mut count = min_imgs + 1
   if(max_imgs > 0 && count > max_imgs){ count = max_imgs }
   def pre_transform = load32(caps, 40)
   def composite_alpha = _choose_composite_alpha(load32(caps, 44))

   mut create_info = sys_malloc(128)
   memset(create_info, 0, 128)
   store32(create_info, VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0)
   store64_raw(create_info, _surface, 24)
   store32(create_info, count, 32)
   _swapchain_format = 44 ; VK_FORMAT_B8G8R8A8_UNORM
   store32(create_info, _swapchain_format, 36) ; format
   store32(create_info, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 40) ; colorSpace
   store32(create_info, w, 44) ; width
   store32(create_info, h, 48) ; height
   store32(create_info, 1, 52) ; layers
   store32(create_info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 2, 56)
   store32(create_info, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(create_info, 0, 64) ; queueCount
   store32(create_info, 0, 72)
   store32(create_info, pre_transform, 80)
   store32(create_info, composite_alpha, 84)
   store32(create_info, _choose_present_mode(), 88)
   store32(create_info, 1, 92) ; clipped
   store32(create_info, 0, 96) ; oldSwapchain

   mut sc_ptr = sys_malloc(8)
   store32(sc_ptr, 0, 0)
   store32(sc_ptr, 0, 4)
   def res = create_swapchain_khr(_device, create_info, 0, sc_ptr)
   if(res != 0){
      if(_debug_gfx_enabled){ print(f"Vulkan: create_swapchain_khr failed with {res}") }
      return false
   }
   _swapchain = load64(sc_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("swapchain", _swapchain) }
   _swapchain_format = VK_FORMAT_B8G8R8A8_UNORM
   ; Get images
   mut img_count_ptr = sys_malloc(4)
   get_swapchain_images_khr(_device, _swapchain, img_count_ptr, 0)
   _swapchain_image_count = load32(img_count_ptr, 0)
   mut img_ptrs_raw = sys_malloc(_swapchain_image_count * 8)
   get_swapchain_images_khr(_device, _swapchain, img_count_ptr, img_ptrs_raw)
   _swapchain_images = []
   mut i = 0
   while(i < _swapchain_image_count){
      _swapchain_images = append(_swapchain_images, load64(img_ptrs_raw, i * 8))
      i += 1
   }
   sys_free(img_count_ptr)
   sys_free(img_ptrs_raw)
   true
}

fn _create_swapchain_image_views(){
   "Internal: creates image views for all swapchain images."
   _swapchain_image_views = []
   mut i = 0
   while(i < len(_swapchain_images)){
      mut ci = sys_malloc(80)
      memset(ci, 0, 80)
      store32(ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store64_raw(ci, get(_swapchain_images, i), 24)
      store32(ci, 1, 32) ; 2D
      store32(ci, _swapchain_format, 36)
      store32(ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(ci, 1, 64)
      store32(ci, 1, 72)
      mut view_ptr = sys_malloc(8)
      create_image_view(_device, ci, 0, view_ptr)
      _swapchain_image_views = append(_swapchain_image_views, load64(view_ptr, 0))
      sys_free(ci)
      sys_free(view_ptr)
      i += 1
   }
   true
}

fn _destroy_swapchain_objects(){
   "Releases swapchain-dependent resources (framebuffers, views, etc)."
   if(!_device){ return 0 }
   mut i = 0
   while(i < len(_framebuffers)){
      def fb = get(_framebuffers, i, 0)
      if(fb){ destroy_framebuffer(_device, fb, 0) }
      i += 1
   }
   _framebuffers = []
   i = 0
   while(i < len(_swapchain_image_views)){
      def iv = get(_swapchain_image_views, i, 0)
      if(iv){ destroy_image_view(_device, iv, 0) }
      i += 1
   }
   _swapchain_image_views = []
   if(_swapchain){
      destroy_swapchain_khr(_device, _swapchain, 0)
      _swapchain = 0
   }
   _swapchain_images = []
   _swapchain_image_count = 0
   0
}

fn _recreate_swapchain(){
   "Rebuilds the swapchain after window resize."
   if(!_window_ref || !_device){ return false }
   device_wait_idle(_device)
   _destroy_swapchain_objects()

   ; Clean up old depth + MSAA resources
   if(_depth_image){ destroy_image(_device, _depth_image, 0) _depth_image = 0 }
   if(_depth_view){ destroy_image_view(_device, _depth_view, 0) _depth_view = 0 }
   if(_depth_memory){ free_memory(_device, _depth_memory, 0) _depth_memory = 0 }
   if(_msaa_color_image){ destroy_image(_device, _msaa_color_image, 0) _msaa_color_image = 0 }
   if(_msaa_color_view){ destroy_image_view(_device, _msaa_color_view, 0) _msaa_color_view = 0 }
   if(_msaa_color_memory){ free_memory(_device, _msaa_color_memory, 0) _msaa_color_memory = 0 }

   if(!_create_swapchain(_window_ref)){ return false }
   if(!_create_swapchain_image_views()){ return false }

   ; Fix: Rebuild depth resources to match new swapchain size
   if(!_create_depth_resources()){ return false }

   if(!_create_framebuffers()){ return false }
   true
}

fn _create_image_views(){
   "Initializes Vulkan image views for each swapchain image."
   _swapchain_image_views = []
   mut i = 0
   while(i < _swapchain_image_count){
      def image_handle = get(_swapchain_images, i)
      mut create_info = sys_malloc(80)
      memset(create_info, 0, 80)
      store32(create_info, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store32(create_info, 0, 8) store32(create_info, 0, 12) ; pNext
      store32(create_info, 0, 16) ; flags
      store64_raw(create_info, image_handle, 24)
      store32(create_info, 1, 32) ; viewType (2D = 1)
      store32(create_info, _swapchain_format, 36)
      ; components (all identity=0)
      ; subresourceRange
      store32(create_info, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(create_info, 0, 60) ; baseMipLevel
      store32(create_info, 1, 64) ; levelCount
      store32(create_info, 0, 68) ; baseArrayLayer
      store32(create_info, 1, 72) ; layerCount
      mut view_ptr = sys_malloc(8)
      def iv_res = create_image_view(_device, create_info, 0, view_ptr)
      if(iv_res != 0){
         return false
      }
      def view_h = load64(view_ptr, 0)
      _swapchain_image_views = append(_swapchain_image_views, view_h)
      i += 1
   }
   true
}

fn _create_depth_resources(){
   "Allocates depth buffer and (if MSAA>1) MSAA color buffer for 3D rendering."
   def depth_format = 126 ; VK_FORMAT_D32_SFLOAT
   def samples = _cfg_msaa

   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16)
   store32(img_ci, 1, 20)
   store32(img_ci, depth_format, 24)
   store32(img_ci, _swapchain_extent_w, 28)
   store32(img_ci, _swapchain_extent_h, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, 1, 40)
   store32(img_ci, 1, 44)
   store32(img_ci, samples, 48) ; MSAA samples
   store32(img_ci, 0, 52)
   store32(img_ci, 32, 56) ; DEPTH_STENCIL_ATTACHMENT
   store32(img_ci, 0, 60)
   store32(img_ci, 0, 64)
   store32(img_ci, 0, 80)
   mut img_ptr = sys_malloc(8)
   if(create_image(_device, img_ci, 0, img_ptr) != 0){ return false }
   _depth_image = load64(img_ptr, 0)
   mut mem_req = sys_malloc(24)
   get_image_memory_requirements(_device, _depth_image, mem_req)
   def d_size = load64(mem_req, 0)
   def d_bits = load32(mem_req, 16)
   def d_mtype = _find_memory_type(d_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, d_size, 16)
   store32(alloc_info, d_mtype, 24)
   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return false }
   _depth_memory = load64(mem_ptr, 0)
   bind_image_memory(_device, _depth_image, _depth_memory, 0)
   mut view_ci = sys_malloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_raw(view_ci, _depth_image, 24)
   store32(view_ci, 1, 32)
   store32(view_ci, depth_format, 36)
   store32(view_ci, 0x00000002, 56)
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)
   mut view_ptr = sys_malloc(8)
   if(create_image_view(_device, view_ci, 0, view_ptr) != 0){ return false }
   _depth_view = load64(view_ptr, 0)

   if(samples > 1){
      mut ci2 = sys_malloc(88)
      memset(ci2, 0, 88)
      store32(ci2, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
      store32(ci2, 0, 16)
      store32(ci2, 1, 20)
      store32(ci2, _swapchain_format, 24) ; same format as swapchain
      store32(ci2, _swapchain_extent_w, 28)
      store32(ci2, _swapchain_extent_h, 32)
      store32(ci2, 1, 36)
      store32(ci2, 1, 40)
      store32(ci2, 1, 44)
      store32(ci2, samples, 48)
      store32(ci2, 0, 52) ; Tiling OPTIMAL
      store32(ci2, 0x00000010, 56) ; COLOR_ATTACHMENT_BIT (not TRANSIENT_ATTACHMENT_BIT for RADV stability)
      store32(ci2, 0, 60)
      store32(ci2, 0, 64)
      store32(ci2, 0, 80)
      mut ip2 = sys_malloc(8)
      if(create_image(_device, ci2, 0, ip2) != 0){ return false }
      _msaa_color_image = load64(ip2, 0)
      mut mr2 = sys_malloc(24)
      get_image_memory_requirements(_device, _msaa_color_image, mr2)
      def c_size = load64(mr2, 0)
      def c_bits = load32(mr2, 16)
      def c_mtype = _find_memory_type(c_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      mut ai2 = sys_malloc(64)
      memset(ai2, 0, 64)
      store32(ai2, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
      store64_raw(ai2, c_size, 16)
      store32(ai2, c_mtype, 24)
      mut mp2 = sys_malloc(8)
      if(allocate_memory(_device, ai2, 0, mp2) != 0){ return false }
      _msaa_color_memory = load64(mp2, 0)
      bind_image_memory(_device, _msaa_color_image, _msaa_color_memory, 0)
      mut vc2 = sys_malloc(80)
      memset(vc2, 0, 80)
      store32(vc2, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store64_raw(vc2, _msaa_color_image, 24)
      store32(vc2, 1, 32)
      store32(vc2, _swapchain_format, 36)
      store32(vc2, 0x00000001, 56) ; ASPECT_COLOR
      store32(vc2, 1, 64)
      store32(vc2, 1, 72)
      mut vp2 = sys_malloc(8)
      if(create_image_view(_device, vc2, 0, vp2) != 0){ return false }
      _msaa_color_view = load64(vp2, 0)
   }
   true
}

fn _create_render_pass(){
   "Defines the Vulkan render pass. Uses 3 attachments (MSAA color + depth + resolve) when MSAA>1, or 2 (color + depth) otherwise."
   def samples = _cfg_msaa
   def msaa = samples > 1

   if(msaa){

      ;; att 0: MSAA color (multisample, DONT_CARE store, COLOR_ATTACHMENT_OPTIMAL final)
      ;; att 1: depth (multisample)
      ;; att 2: resolve (1-sample, STORE, PRESENT_SRC_KHR)
      mut atts = sys_malloc(108) ; 3 * 36 bytes
      memset(atts, 0, 108)
      ; att 0 - MSAA color
      store32(atts, _swapchain_format, 4)
      store32(atts, samples, 8)
      store32(atts, 1, 12) ; loadOp CLEAR
      store32(atts, 2, 16) ; storeOp DONT_CARE (MSAA image doesn't need to be stored)
      store32(atts, 2, 20)
      store32(atts, 2, 24)
      store32(atts, 0, 28) ; initialLayout UNDEFINED
      store32(atts, 2, 32) ; finalLayout COLOR_ATTACHMENT_OPTIMAL
      ; att 1 - depth
      store32(atts, 126, 36+4)
      store32(atts, samples, 36+8)
      store32(atts, 1, 36+12) ; loadOp CLEAR
      store32(atts, 2, 36+16) ; storeOp DONT_CARE
      store32(atts, 2, 36+20)
      store32(atts, 2, 36+24)
      store32(atts, 0, 36+28)
      store32(atts, 3, 36+32) ; DEPTH_STENCIL_ATTACHMENT_OPTIMAL
      ; att 2 - resolve (1 sample, swapchain)
      store32(atts, _swapchain_format, 72+4)
      store32(atts, 1, 72+8)
      store32(atts, 2, 72+12) ; loadOp DONT_CARE
      store32(atts, 0, 72+16) ; storeOp STORE
      store32(atts, 2, 72+20)
      store32(atts, 2, 72+24)
      store32(atts, 0, 72+28) ; UNDEFINED
      store32(atts, 1000001002, 72+32) ; PRESENT_SRC_KHR

      mut car = sys_malloc(8) store32(car, 0, 0) store32(car, 2, 4) ; att0 COLOR_ATTACHMENT_OPTIMAL
      mut dar = sys_malloc(8) store32(dar, 1, 0) store32(dar, 3, 4) ; att1 DEPTH_STENCIL_ATTACHMENT_OPTIMAL
      mut rar = sys_malloc(8) store32(rar, 2, 0) store32(rar, 2, 4) ; att2 COLOR_ATTACHMENT_OPTIMAL (resolve)

      mut sd = sys_malloc(72)
      memset(sd, 0, 72)
      store32(sd, 0, 4) ; pipelineBindPoint = GRAPHICS
      store32(sd, 1, 24) ; colorAttachmentCount = 1
      store64_raw(sd, car, 32) ; pColorAttachments (offset 32)
      store64_raw(sd, rar, 40) ; pResolveAttachments (offset 40) — resolves MSAA to swapchain
      store64_raw(sd, dar, 48) ; pDepthStencilAttachment (offset 48)

      mut dep = sys_malloc(28)
      store32(dep, -1, 0)
      store32(dep, 0, 4)
      store32(dep, 0x00000400, 8)
      store32(dep, 0x00000400, 12)
      store32(dep, 0, 16)
      store32(dep, 0x00000100 | 0x00000010, 20)
      store32(dep, 0, 24)

      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
      store32(create_info, 3, 20) ; 3 attachments
      store64_raw(create_info, atts, 24)
      store32(create_info, 1, 32)
      store64_raw(create_info, sd, 40)
      store32(create_info, 1, 48)
      store64_raw(create_info, dep, 56)

      mut pass_ptr = sys_malloc(8)
      if(create_render_pass(_device, create_info, 0, pass_ptr) != 0){ return false }
      _render_pass = load64(pass_ptr, 0)
   } else {

      mut atts = sys_malloc(72)
      memset(atts, 0, 72)
      store32(atts, _swapchain_format, 4)
      store32(atts, 1, 8)
      store32(atts, 1, 12) ; CLEAR
      store32(atts, 0, 16) ; STORE
      store32(atts, 2, 20) store32(atts, 2, 24)
      store32(atts, 0, 28) store32(atts, 1000001002, 32) ; PRESENT_SRC_KHR
      store32(atts, 126, 36+4)
      store32(atts, 1, 36+8)
      store32(atts, 1, 36+12) ; loadOp CLEAR
      store32(atts, 2, 36+16) ; storeOp DONT_CARE
      store32(atts, 2, 36+20) store32(atts, 2, 36+24)
      store32(atts, 0, 36+28) store32(atts, 3, 36+32)

      mut car = sys_malloc(8) store32(car, 0, 0) store32(car, 2, 4)
      mut dar = sys_malloc(8) store32(dar, 1, 0) store32(dar, 3, 4)

      mut sd = sys_malloc(72)
      memset(sd, 0, 72)
      store32(sd, 0, 4)
      store32(sd, 1, 24)
      store64_raw(sd, car, 32)
      store64_raw(sd, dar, 48)

      mut dep = sys_malloc(28)
      store32(dep, -1, 0) store32(dep, 0, 4)
      store32(dep, 0x00000400, 8) store32(dep, 0x00000400, 12)
      store32(dep, 0, 16) store32(dep, 0x00000100 | 0x00000010, 20)
      store32(dep, 0, 24)

      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
      store32(create_info, 2, 20)
      store64_raw(create_info, atts, 24)
      store32(create_info, 1, 32)
      store64_raw(create_info, sd, 40)
      store32(create_info, 1, 48)
      store64_raw(create_info, dep, 56)

      mut pass_ptr = sys_malloc(8)
      if(create_render_pass(_device, create_info, 0, pass_ptr) != 0){ return false }
      _render_pass = load64(pass_ptr, 0)
   }
   true
}

fn _create_framebuffers(){
   "Creates Vulkan framebuffers. When MSAA>1: [msaa_color, depth, resolve(swapchain)]. Otherwise: [swapchain, depth]."
   _framebuffers = []
   def msaa = _cfg_msaa > 1
   mut i = 0
   while(i < _swapchain_image_count){
      mut attach_ptr = 0
      mut att_count = 0
      if(msaa){
         attach_ptr = sys_malloc(24)
         store64_raw(attach_ptr, _msaa_color_view, 0) ; att0 MSAA color
         store64_raw(attach_ptr, _depth_view, 8) ; att1 depth
         store64_raw(attach_ptr, get(_swapchain_image_views, i), 16) ; att2 resolve
         att_count = 3
      } else {
         attach_ptr = sys_malloc(16)
         store64_raw(attach_ptr, get(_swapchain_image_views, i), 0)
         store64_raw(attach_ptr, _depth_view, 8)
         att_count = 2
      }
      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0)
      store64_raw(create_info, _render_pass, 24)
      store32(create_info, att_count, 32)
      store64_raw(create_info, attach_ptr, 40)
      store32(create_info, _swapchain_extent_w, 48)
      store32(create_info, _swapchain_extent_h, 52)
      store32(create_info, 1, 56)
      mut fb_ptr = sys_malloc(8)
      if(_debug_gfx_enabled){ print(f"Vulkan: Creating framebuffer {i}...") }
      if(create_framebuffer(_device, create_info, 0, fb_ptr) != 0){ return false }
      def fb = load64(fb_ptr, 0)
      if(_debug_gfx_enabled){ _dbg_handle(f"framebuffer {i}", fb) }
      _framebuffers = append(_framebuffers, fb)
      i += 1
   }
   if(_debug_gfx_enabled){ print("Vulkan: All framebuffers created.") }
   true
}

fn _create_sync_objects(){
   "Initializes semaphores and fences for frame synchronization."
   _image_available_semaphores = []
   _render_finished_semaphores = []
   _in_flight_fences = []
   mut i = 0
   while(i < MAX_FRAMES_IN_FLIGHT){
      mut si = sys_malloc(24)
      memset(si, 0, 24)
      store32(si, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0)
      store32(si, 0, 8) store32(si, 0, 12) ; pNext
      store32(si, 0, 16) ; flags
      mut sem1 = sys_malloc(8)
      def s1_res = create_semaphore(_device, si, 0, sem1)
      if(s1_res != 0){
         return false
      }
      _image_available_semaphores = append(_image_available_semaphores, load64(sem1, 0))
      mut sem2 = sys_malloc(8)
      def s2_res = create_semaphore(_device, si, 0, sem2)
      if(s2_res != 0){
         return false
      }
      _render_finished_semaphores = append(_render_finished_semaphores, load64(sem2, 0))
      mut fi = sys_malloc(24)
      memset(fi, 0, 24)
      store32(fi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
      store32(fi, 0, 8) store32(fi, 0, 12) ; pNext
      store32(fi, 1, 16) ; flags (1 = SIGNAL_BIT)
      mut fence = sys_malloc(8)
      def f_res = create_fence(_device, fi, 0, fence)
      if(f_res != 0){
         return false
      }
      _in_flight_fences = append(_in_flight_fences, load64(fence, 0))
      i += 1
   }
   true
}

fn _create_command_pool(){
   "Creates the Vulkan command pool for recording draw commands."
   mut create_info = sys_malloc(32)
   memset(create_info, 0, 32)
   store32(create_info, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0)
   store32(create_info, 2, 16) ; flags (2 = RESET_BIT)
   store32(create_info, _graphics_family_index, 20)
   mut pool_ptr = sys_malloc(8)
   def cp_res = create_command_pool(_device, create_info, 0, pool_ptr)
   if(cp_res != 0){
      return false
   }
   _command_pool = load64(pool_ptr, 0)
   true
}

fn _create_command_buffers(){
   "Allocates primary command buffers from the pool."
   mut ai = sys_malloc(32)
   memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_raw(ai, _command_pool, 16)
   store32(ai, 0, 24) ; level (0 = PRIMARY)
   store32(ai, MAX_FRAMES_IN_FLIGHT, 28)
   mut bufs_ptr = sys_malloc(MAX_FRAMES_IN_FLIGHT * 8)
   def cb_res = allocate_command_buffers(_device, ai, bufs_ptr)
   if(cb_res != 0){
      return false
   }
   _command_buffers = []
   mut i = 0
   while(i < MAX_FRAMES_IN_FLIGHT){
      _command_buffers = append(_command_buffers, load64(bufs_ptr, i * 8))
      i += 1
   }
   true
}

fn _create_ubo_descriptor_sets(){
   "Allocates per-frame UBO descriptor sets."
   if(!_descriptor_pool || !_descriptor_set_layout_ubo || !_ubo_buffer){ return false }
   _ubo_descriptor_sets = []
   mut i = 0
   while(i < MAX_FRAMES_IN_FLIGHT){
      mut dsl_ptr = sys_malloc(8)
      store64_raw(dsl_ptr, _descriptor_set_layout_ubo, 0)
      mut alloc_ds = sys_malloc(40)
      memset(alloc_ds, 0, 40)
      store32(alloc_ds, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0)
      store64_raw(alloc_ds, _descriptor_pool, 16)
      store32(alloc_ds, 1, 24)
      store64_raw(alloc_ds, dsl_ptr, 32)
      mut ds_ptr = sys_malloc(8)
      if(allocate_descriptor_sets(_device, alloc_ds, ds_ptr) != 0){ return false }
      def ds = load64(ds_ptr, 0)

      mut buf_info = sys_malloc(24)
      store64_raw(buf_info, _ubo_buffer, 0)
      store64_raw(buf_info, i * _ubo_stride, 8)
      store64_raw(buf_info, _UBO_SIZE, 16)

      mut write = sys_malloc(64)
      memset(write, 0, 64)
      store32(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
      store64_raw(write, ds, 16)
      store32(write, 0, 24) ; binding
      store32(write, 0, 28) ; array element
      store32(write, 1, 32) ; count
      store32(write, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 36)
      store64_raw(write, buf_info, 48) ; pBufferInfo
      update_descriptor_sets(_device, 1, write, 0, 0)

      _ubo_descriptor_sets = append(_ubo_descriptor_sets, ds)

      sys_free(dsl_ptr) sys_free(alloc_ds) sys_free(ds_ptr)
      sys_free(buf_info) sys_free(write)
      i += 1
   }
   true
}
