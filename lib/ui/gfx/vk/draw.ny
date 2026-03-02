module std.ui.gfx.vk.draw (
  begin_frame,
  end_frame,
  clear,
  draw_rect,
  draw_rect_tex,
  draw_rect_tex_uv,
  draw_triangle,
  draw_triangle_3d,
  draw_line,
  bind_texture
)
use std.core *
use std.core.mem *
use std.os *
use std.text.io as tio
use std.math *
use std.math.matrix *
use std.ui.glfw as ui_glfw
use std.ui.gfx.vulkan *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.setup *
use std.ui.gfx.vk.utils *
use std.ui.gfx.snapshot as snp

fn begin_frame(){
   "Prepares the renderer for a new frame (sync, acquire image, begin recording)."
   if(_is_debug()){ print("draw.begin_frame: entry") }
   vk_set(VK_CTX_FRAME_OPEN, false)
   ;; Wait for previous frame's fence
   if(_is_debug()){
      print(f"draw.begin_frame: sync. device={vk_get(VK_CTX_DEVICE)} fences={vk_get(VK_CTX_IN_FLIGHT_FENCES)}")
   }
   def fences = vk_get(VK_CTX_IN_FLIGHT_FENCES)
   if(!fences){
      if(_is_debug()){ print("Vulkan: IN_FLIGHT_FENCES is null") }
      return false
   }
   def cur_frame = vk_get(VK_CTX_CURRENT_FRAME)
   if(_is_debug()){ print(f"draw.begin_frame: get fence at {cur_frame}") }
   def fence = get(fences, cur_frame)
   if(_is_debug()){ print(f"draw.begin_frame: fence={fence}") }
   mut fence_ptr = sys_malloc(8)
   store64_raw(fence_ptr, fence, 0)
   if(_is_debug()){ print("draw.begin_frame: wait_for_fences") }
   def wf = wait_for_fences(vk_get(VK_CTX_DEVICE), 1, fence_ptr, 1, 0xFFFFFFFFFFFFFFFF)
   if(wf != 0){
      sys_free(fence_ptr)
      return false
   }
   def rf = reset_fences(vk_get(VK_CTX_DEVICE), 1, fence_ptr)
   if(rf != 0){
      sys_free(fence_ptr)
      return false
   }
   ;; Acquire next image
   mut img_idx_ptr = sys_malloc(4)
   def sem = get(vk_get(VK_CTX_IMAGE_AVAILABLE_SEMAPHORES), vk_get(VK_CTX_CURRENT_FRAME))
   def acq = acquire_next_image_khr(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_SWAPCHAIN), 0xFFFFFFFFFFFFFFFF, sem, 0, img_idx_ptr)
   if(acq == 3294966292 || acq == -1000001004){ ;; VK_ERROR_OUT_OF_DATE_KHR
      sys_free(fence_ptr)
      sys_free(img_idx_ptr)
      _recreate_swapchain()
      return false
   }
   if(acq != 0 && acq != 1000001003){ ;; VK_SUBOPTIMAL_KHR is non-fatal
      if(_is_debug()){ print(f"Vulkan: vkAcquireNextImageKHR failed with code {acq}") }
      sys_free(fence_ptr)
      sys_free(img_idx_ptr)
      return false
   }
   vk_set(VK_CTX_IMAGE_INDEX, load32(img_idx_ptr, 0))
   ;; Begin recording
   def cb = get(vk_get(VK_CTX_COMMAND_BUFFERS), vk_get(VK_CTX_CURRENT_FRAME))
   mut bi = sys_malloc(32)
   memset(bi, 0, 32)
   store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   def bcb = begin_command_buffer(cb, bi)
   if(bcb != 0){
      if(_is_debug()){ print(f"Vulkan: vkBeginCommandBuffer failed with code {bcb}") }
      sys_free(fence_ptr)
      sys_free(img_idx_ptr)
      sys_free(bi)
      return false
   }
   ;; Begin Render Pass
   mut clear_values = sys_malloc(32)
   store32(clear_values, 0, 0) store32(clear_values, 0, 4) store32(clear_values, 0, 8) store32(clear_values, 0x3f800000, 12) ;; Color
   store32_f32(clear_values, 1.0, 16) store32(clear_values, 0, 20) ;; Depth

   mut ri = sys_malloc(64)
   memset(ri, 0, 64)
   store32(ri, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0)
   store64_raw(ri, to_int(vk_get(VK_CTX_RENDER_PASS)), 16)
   store64_raw(ri, get(vk_get(VK_CTX_FRAMEBUFFERS, vk_get(VK_CTX_IMAGE_INDEX))), 24)
   store32(ri, vk_get(VK_CTX_SWAPCHAIN_EXTENT_W), 40); store32(ri, vk_get(VK_CTX_SWAPCHAIN_EXTENT_H), 44)
   store32(ri, 2, 48) ;; clearValueCount
   store64_raw(ri, clear_values, 56)
   cmd_begin_render_pass(cb, ri, 0)

   ;; Set dynamic viewport/scissor
   mut vp = sys_malloc(24)
   store32_f32(vp, 0.0, 0) store32_f32(vp, 0.0, 4)
   store32_f32(vp, float(vk_get(VK_CTX_SWAPCHAIN_EXTENT_W)), 8) store32_f32(vp, float(vk_get(VK_CTX_SWAPCHAIN_EXTENT_H)), 12)
   store32_f32(vp, 0.0, 16) store32_f32(vp, 1.0, 20)
   cmd_set_viewport(cb, 0, 1, vp)

   mut sci = sys_malloc(16)
   store32(sci, 0, 0) store32(sci, 0, 4)
   store32(sci, vk_get(VK_CTX_SWAPCHAIN_EXTENT_W), 8) store32(sci, vk_get(VK_CTX_SWAPCHAIN_EXTENT_H), 12)
   cmd_set_scissor(cb, 0, 1, sci)

   sys_free(vp) sys_free(sci)

   vk_set(VK_CTX_FRAME_OPEN, true)
   vk_set(VK_CTX_VERTEX_OFFSET, 0)
   vk_set(VK_CTX_TOTAL_FRAMES, vk_get(VK_CTX_TOTAL_FRAMES) + 1)
   if(vk_get(VK_CTX_WINDOW_REF)){
      _update_default_mvp(vk_get(VK_CTX_WINDOW_REF))
      if(vk_get(VK_CTX_TOTAL_FRAMES) % 60 == 0 && _is_debug()){
         def w = get(vk_get(VK_CTX_WINDOW_REF), 5) def h = get(vk_get(VK_CTX_WINDOW_REF), 6)
         print(f"Vulkan Frame {vk_get(VK_CTX_TOTAL_FRAMES)}: Window {w}x{h}, Extent {vk_get(VK_CTX_SWAPCHAIN_EXTENT_W)}x{vk_get(VK_CTX_SWAPCHAIN_EXTENT_H)}")
      }
   }
   sys_free(fence_ptr)
   sys_free(img_idx_ptr)
   sys_free(bi)
   sys_free(clear_values)
   sys_free(ri)
   true
}

fn end_frame(){
   "Finalizes the frame (flush, end recording, submit to queue, present)."
   if(!vk_get(VK_CTX_FRAME_OPEN)){ return false }
   _flush()
   def cb = get(vk_get(VK_CTX_COMMAND_BUFFERS), vk_get(VK_CTX_CURRENT_FRAME))
   cmd_end_render_pass(cb)
   def ecb = end_command_buffer(cb)
   if(ecb != 0){
      if(_is_debug()){ print(f"Vulkan: vkEndCommandBuffer failed with code {ecb}") }
      return false
   }
   mut wait_sems = sys_malloc(8)
   store64_raw(wait_sems, to_int(get(vk_get(VK_CTX_IMAGE_AVAILABLE_SEMAPHORES), vk_get(VK_CTX_CURRENT_FRAME))), 0)
   mut signal_sems = sys_malloc(8)
   store64_raw(signal_sems, to_int(get(vk_get(VK_CTX_RENDER_FINISHED_SEMAPHORES), vk_get(VK_CTX_CURRENT_FRAME))), 0)
   mut wait_stages = sys_malloc(4)
   store32(wait_stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0)
   mut cb_ptr = sys_malloc(8) store64_raw(cb_ptr, cb, 0)
   mut si = sys_malloc(128)
   memset(si, 0, 128)
   store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(si, 1, 16) ;; waitSemaphoreCount
   store64_raw(si, wait_sems, 24)
   store64_raw(si, wait_stages, 32)
   store32(si, 1, 40) ;; commandBufferCount
   store64_raw(si, cb_ptr, 48)
   store32(si, 1, 56) ;; signalSemaphoreCount
   store64_raw(si, signal_sems, 64)
   def fence = get(vk_get(VK_CTX_IN_FLIGHT_FENCES), vk_get(VK_CTX_CURRENT_FRAME))
   def sub_res = queue_submit(vk_get(VK_CTX_GRAPHICS_QUEUE), 1, si, fence)
   if(sub_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkQueueSubmit failed with code {sub_res}") }
      sys_free(wait_sems)
      sys_free(signal_sems)
      sys_free(wait_stages)
      sys_free(cb_ptr)
      sys_free(si)
      return false
   }
   mut scs = sys_malloc(8) store64_raw(scs, to_int(vk_get(VK_CTX_SWAPCHAIN)), 0)
   mut idxs = sys_malloc(4) store32(idxs, vk_get(VK_CTX_IMAGE_INDEX), 0)
   mut pi = sys_malloc(64)
   memset(pi, 0, 64)
   store32(pi, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0)
   store32(pi, 1, 16) ;; waitSemaphoreCount
   store64_raw(pi, signal_sems, 24)
   store32(pi, 1, 32) ;; swapchainCount
   store64_raw(pi, scs, 40)
   store64_raw(pi, idxs, 48)
   def pr = queue_present_khr(vk_get(VK_CTX_PRESENT_QUEUE), pi)
   if(pr == 3294966292 || pr == -1000001004){ ;; VK_ERROR_OUT_OF_DATE_KHR
      vk_set(VK_CTX_FRAME_OPEN, false)
      sys_free(wait_sems)
      sys_free(signal_sems)
      sys_free(wait_stages)
      sys_free(cb_ptr)
      sys_free(si)
      sys_free(scs)
      sys_free(idxs)
      sys_free(pi)
      _recreate_swapchain()
      return false
   }
   if(pr != 0 && pr != 1000001003){ ;; VK_SUBOPTIMAL_KHR is non-fatal
      if(_is_debug()){ print(f"Vulkan: vkQueuePresentKHR failed with code {pr}") }
      sys_free(wait_sems)
      sys_free(signal_sems)
      sys_free(wait_stages)
      sys_free(cb_ptr)
      sys_free(si)
      sys_free(scs)
      sys_free(idxs)
      sys_free(pi)
      return false
   }
   sys_free(wait_sems)
   sys_free(signal_sems)
   sys_free(wait_stages)
   sys_free(cb_ptr)
   sys_free(si)
   sys_free(scs)
   sys_free(idxs)
   sys_free(pi)
   vk_set(VK_CTX_FRAME_OPEN, false)
   vk_set(VK_CTX_CURRENT_FRAME, (vk_get(VK_CTX_CURRENT_FRAME) + 1) % vk_get(VK_CTX_MAX_FRAMES_IN_FLIGHT))
   true
}

fn clear(r, g, b, a){
   "Commands the GPU to clear the current color attachment."
   if(!vk_get(VK_CTX_FRAME_OPEN)){ return 0 }
   def cb = get(vk_get(VK_CTX_COMMAND_BUFFERS), vk_get(VK_CTX_CURRENT_FRAME))
   mut ca = sys_malloc(24)
   memset(ca, 0, 24)
   store32(ca, VK_IMAGE_ASPECT_COLOR_BIT, 0)
   store32(ca, 0, 4) ;; colorAttachment
   store32_f32(ca, r, 8)
   store32_f32(ca, g, 12)
   store32_f32(ca, b, 16)
   store32_f32(ca, a, 20)
   mut rect = sys_malloc(24)
   memset(rect, 0, 24)
   store32(rect, 0, 0) store32(rect, 0, 4) ;; offset
   store32(rect, vk_get(VK_CTX_SWAPCHAIN_EXTENT_W), 8) store32(rect, vk_get(VK_CTX_SWAPCHAIN_EXTENT_H), 12) ;; extent
   store32(rect, 0, 16) ;; baseArrayLayer
   store32(rect, 1, 20) ;; layerCount
   cmd_clear_attachments(cb, 1, ca, 1, rect)
   sys_free(ca)
   sys_free(rect)
}

fn draw_rect(x, y, w, h, r, g, b, a){
   "Batches a colored rectangle for rendering."
   ;; Optimized to use textured path with default white texture
   draw_rect_tex(x, y, w, h, vk_get(VK_CTX_DEFAULT_TEXTURE), r, g, b, a)
}

fn draw_rect_tex(x, y, w, h, tex_id, r, g, b, a){
   "Batches a textured rectangle for rendering."
   if(!vk_get(VK_CTX_FRAME_OPEN)){ return 0 }
   bind_texture(tex_id)
   _check_flush(6 * 36)
   _push_vertex(x, y, 0.0, 0.0, 0.0, r, g, b, a)
   _push_vertex(x, y + h, 0.0, 0.0, 1.0, r, g, b, a)
   _push_vertex(x + w, y + h, 0.0, 1.0, 1.0, r, g, b, a)
   _push_vertex(x, y, 0.0, 0.0, 0.0, r, g, b, a)
   _push_vertex(x + w, y + h, 0.0, 1.0, 1.0, r, g, b, a)
   _push_vertex(x + w, y, 0.0, 1.0, 0.0, r, g, b, a)
}

fn draw_rect_tex_uv(x, y, w, h, tex_id, u1, v1, u2, v2, r, g, b, a){
   "Batches a textured rectangle with explicit UV coordinates."
   if(!vk_get(VK_CTX_FRAME_OPEN)){ return 0 }
   bind_texture(tex_id)
   _check_flush(6 * 36)
   _push_vertex(x, y, 0.0, u1, v1, r, g, b, a)
   _push_vertex(x, y + h, 0.0, u1, v2, r, g, b, a)
   _push_vertex(x + w, y + h, 0.0, u2, v2, r, g, b, a)
   _push_vertex(x, y, 0.0, u1, v1, r, g, b, a)
   _push_vertex(x + w, y + h, 0.0, u2, v2, r, g, b, a)
   _push_vertex(x + w, y, 0.0, u2, v1, r, g, b, a)
}

fn draw_triangle(x1, y1, x2, y2, x3, y3, r, g, b, a){
   "Batches a colored triangle for rendering."
   if(!vk_get(VK_CTX_FRAME_OPEN)){ return 0 }
   _check_flush(3 * 36)
   _push_vertex(x1, y1, 0.0, 0.0, 0.0, r, g, b, a)
   _push_vertex(x2, y2, 0.0, 0.0, 0.0, r, g, b, a)
   _push_vertex(x3, y3, 0.0, 0.0, 0.0, r, g, b, a)
}

fn draw_triangle_3d(x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b, a){
   "Batches a colored 3D triangle for rendering."
   if(!vk_get(VK_CTX_FRAME_OPEN)){ return 0 }
   bind_texture(vk_get(VK_CTX_DEFAULT_TEXTURE))
   _check_flush(3 * 36)
   _push_vertex(x1, y1, z1, 0.0, 0.0, r, g, b, a)
   _push_vertex(x2, y2, z2, 0.0, 0.0, r, g, b, a)
   _push_vertex(x3, y3, z3, 0.0, 0.0, r, g, b, a)
}

fn draw_line(x1, y1, x2, y2, thickness, r, g, b, a){
   "Batches a colored line for rendering by expanding it into two triangles."
   def dx = x2 - x1
   def dy = y2 - y1
   def len = sqrt(dx * dx + dy * dy)
   if(len <= 0.00001){ return 0 }
   def inv = 1.0 / len
   def nx = -dy * inv
   def ny = dx * inv
   if(thickness <= 0.0){ thickness = 1.0 }
   def half_t = thickness * 0.5
   def ax = x1 + nx * half_t
   def ay = y1 + ny * half_t
   def bx = x2 + nx * half_t
   def by = y2 + ny * half_t
   def cx = x2 - nx * half_t
   def cy = y2 - ny * half_t
   def dx2 = x1 - nx * half_t
   def dy2 = y1 - ny * half_t
   draw_triangle(ax, ay, bx, by, cx, cy, r, g, b, a)
   draw_triangle(ax, ay, cx, cy, dx2, dy2, r, g, b, a)
}

fn _push_vertex(x, y, z, u, v, r, g, b, a){
   "Appends a single vertex to the mapped vertex buffer."
   def off = vk_get(VK_CTX_VERTEX_MAP) + vk_get(VK_CTX_VERTEX_OFFSET)
   store32_f32(off, x, 0)
   store32_f32(off, y, 4)
   store32_f32(off, z, 8)
   store32_f32(off, u, 12)
   store32_f32(off, v, 16)
   store32_f32(off, r, 20)
   store32_f32(off, g, 24)
   store32_f32(off, b, 28)
   store32_f32(off, a, 32)
   vk_set(VK_CTX_VERTEX_OFFSET, vk_get(VK_CTX_VERTEX_OFFSET) + 36)
}

fn _flush(){
   "Submits pending vertex data to the GPU and executes a draw call."
   if(vk_get(VK_CTX_VERTEX_OFFSET) == 0){ return }
   def cb = get(vk_get(VK_CTX_COMMAND_BUFFERS), vk_get(VK_CTX_CURRENT_FRAME))
   cmd_bind_pipeline(cb, 0, vk_get(VK_CTX_PIPELINE))

   ;; Bind Texture
   mut tid = vk_get(VK_CTX_CURRENT_TEXTURE_ID)
   if(tid < 0 || tid >= len(vk_get(VK_CTX_TEXTURES))){ tid = vk_get(VK_CTX_DEFAULT_TEXTURE) }
   def tex = get(vk_get(VK_CTX_TEXTURES), tid)
   def ds = dict_get(tex, "ds", 0)
   if(ds){
      mut ds_ptr = sys_malloc(8)
      store64_raw(ds_ptr, ds, 0)
      cmd_bind_descriptor_sets(cb, 0, vk_get(VK_CTX_PIPELINE_LAYOUT), 0, 1, ds_ptr, 0, 0)
      sys_free(ds_ptr)
   }

   mut off = sys_malloc(8)
   store32(off, 0, 0) store32(off, 0, 4)
   mut buf_ptr = sys_malloc(8)
   store64_raw(buf_ptr, to_int(vk_get(VK_CTX_VERTEX_BUFFER)), 0)
   cmd_bind_vertex_buffers(cb, 0, 1, buf_ptr, off)
   sys_free(off)
   sys_free(buf_ptr)

   cmd_push_constants(cb, vk_get(VK_CTX_PIPELINE_LAYOUT), 1, 0, 64, vk_get(VK_CTX_CURRENT_MVP))

   def count = to_int(vk_get(VK_CTX_VERTEX_OFFSET) / 36)
   cmd_draw(cb, count, 1, 0, 0)

   vk_set(VK_CTX_VERTEX_OFFSET, 0)
}

fn _check_flush(bytes){
   "Internal helper to flush the buffer if it cannot accommodate more data."
   if(vk_get(VK_CTX_VERTEX_OFFSET) + bytes > vk_get(VK_CTX_VERTEX_CAPACITY)){
      _flush()
   }
}

fn bind_texture(tex_id){
   "Binds a texture by ID for subsequent drawing commands."
   if(tex_id == vk_get(VK_CTX_CURRENT_TEXTURE_ID)){ return }
   _flush()
   vk_set(VK_CTX_CURRENT_TEXTURE_ID, tex_id)
}

fn snapshot(filename){
   "Saves the current swapchain image to a TGA file."
   def device = vk_get(VK_CTX_DEVICE)
   def w = vk_get(VK_CTX_SWAPCHAIN_EXTENT_W)
   def h = vk_get(VK_CTX_SWAPCHAIN_EXTENT_H)
   def size = w * h * 4

   if(!vk_get(VK_CTX_FRAME_OPEN)){
      if(_is_debug()){ print("Vulkan: Snapshot requires an open frame (call between begin_frame/end_frame)") }
      return false
   }

   ;; Ensure we have a host-visible buffer to copy to
   mut buf = vk_get(VK_CTX_SCREENSHOT_BUFFER)
   mut mem = vk_get(VK_CTX_SCREENSHOT_MEMORY)
   if(!buf){
      mut bci = sys_malloc(64)
      memset(bci, 0, 64)
      store32(bci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
      store64_raw(bci, size, 16)
      store32(bci, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 24)
      store32(bci, VK_SHARING_MODE_EXCLUSIVE, 28)
      mut b_ptr = sys_malloc(8)
      create_buffer(device, bci, 0, b_ptr)
      buf = load64(b_ptr, 0)
      vk_set(VK_CTX_SCREENSHOT_BUFFER, buf)

      mut mr = sys_malloc(24)
      get_buffer_memory_requirements(device, buf, mr)
      def m_type = _find_memory_type(load32(mr, 16), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

      mut ai = sys_malloc(64)
      memset(ai, 0, 64)
      store32(ai, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
      store64_raw(ai, load64(mr, 0), 16)
      store32(ai, m_type, 24)
      mut m_ptr = sys_malloc(8)
      allocate_memory(device, ai, 0, m_ptr)
      mem = load64(m_ptr, 0)
      vk_set(VK_CTX_SCREENSHOT_MEMORY, mem)
      bind_buffer_memory(device, buf, mem, 0)
   }

   def cb = get(vk_get(VK_CTX_COMMAND_BUFFERS), vk_get(VK_CTX_CURRENT_FRAME))
   def img = get(vk_get(VK_CTX_SWAPCHAIN_IMAGES), vk_get(VK_CTX_IMAGE_INDEX))

   ;; 1. End render pass (mandatory for copy)
   cmd_end_render_pass(cb)

   ;; 2. Transition image to TRANSFER_SRC
   mut barriers = sys_malloc(72)
   memset(barriers, 0, 72)
   store32(barriers, 45, 0) ;; IMAGE_MEMORY_BARRIER
   store32(barriers, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 8) ;; srcAccess
   store32(barriers, VK_ACCESS_TRANSFER_READ_BIT, 12) ;; dstAccess
   store32(barriers, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 16) ;; oldLayout (was inside RP)
   store32(barriers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 20) ;; newLayout
   store64_raw(barriers, img, 32)
   store32(barriers, VK_IMAGE_ASPECT_COLOR_BIT, 40) ;; aspect
   store32(barriers, 1, 48) ;; levelCount
   store32(barriers, 1, 56) ;; layerCount

   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, barriers)

   ;; 3. Copy image to buffer
   mut region = sys_malloc(56)
   memset(region, 0, 56)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 8) ;; aspect
   store32(region, w, 28) ;; extent.w
   store32(region, h, 32) ;; extent.h
   store32(region, 1, 36) ;; extent.d
   cmd_copy_image_to_buffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, region)

   ;; 4. Transition back to COLOR_ATTACHMENT (so we can resume if needed? No, usually presenting)
   store32(barriers, VK_ACCESS_TRANSFER_READ_BIT, 8)
   store32(barriers, VK_ACCESS_MEMORY_READ_BIT, 12)
   store32(barriers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 16)
   store32(barriers, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 20)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, barriers)

   ;; 5. Finish frame and Wait
   ;; We manually end command buffer recording since we ended the render pass.
   end_command_buffer(cb)

   ;; Submit
   mut si = sys_malloc(128)
   memset(si, 0, 128)
   store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   mut pcb = sys_malloc(8) store64_raw(pcb, cb, 0)
   store32(si, 1, 40)
   store64_raw(si, pcb, 48)

   queue_submit(vk_get(VK_CTX_GRAPHICS_QUEUE), 1, si, 0)
   device_wait_idle(device)

   ;; Map and save
   mut p_data = sys_malloc(8)
   map_memory(device, mem, 0, size, 0, p_data)
   def raw = load64(p_data, 0)

    snp.save_tga(filename, raw, w, h)

    unmap_memory(device, mem)

    ;; Reset frame state so next begin_frame works correctly
    vk_set(VK_CTX_FRAME_OPEN, false)
    vk_set(VK_CTX_CURRENT_FRAME, (vk_get(VK_CTX_CURRENT_FRAME) + 1) % vk_get(VK_CTX_MAX_FRAMES_IN_FLIGHT))

    begin_frame()
    true
}
