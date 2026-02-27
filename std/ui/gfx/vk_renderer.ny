;; Keywords: ui gfx vulkan renderer
;; Vulkan 2D Renderer for Nytrix

module std.ui.gfx.vk_renderer (
   init, shutdown,
   begin_frame, end_frame,
   clear, draw_rect, draw_line
)

use std.core *
use std.core.mem *
use std.math *
use std.ui.gfx.vulkan *
use std.ui.backend.x11 as ui_x11
use std.os *
use std.os.process as proc
use std.os.ffi *

fn _is_debug(){ 
   "Auto-generated docstring: _is_debug."
   def d = env("NYTRIX_DEBUG_GFX")
   if(d){ return true }
   false
}

fn _touch(...args){
   "Auto-generated docstring: _touch."
   0
}

fn _dbg_handle(label, h){
   "Auto-generated docstring: _dbg_handle."
   if(!_is_debug()){ return 0 }
   print(f"Vulkan: {label} h={h}")
   0
}

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

mut _command_pool = 0
mut _command_buffers = []

mut _pipeline_layout = 0
mut _rect_pipeline = 0
mut _tri_pipeline = 0
mut _vert_module = 0
mut _frag_module = 0
mut _tri_vert_module = 0
mut _tri_frag_module = 0

mut _image_available_semaphores = []
mut _render_finished_semaphores = []
mut _in_flight_fences = []

mut _current_frame = 0
mut _image_index = 0
mut _frame_open = false
mut _window_ref = 0
def MAX_FRAMES_IN_FLIGHT = 2

fn init(win){
   "Auto-generated docstring: init."
   if(_is_debug()){ print("Vulkan: Initializing renderer...") }
   _window_ref = win
   if(!vk_init()){ 
      if(_is_debug()){ print("Vulkan: vk_init failed") }
      return false 
   }
   if(!_create_instance()){ 
      if(_is_debug()){ print("Vulkan: _create_instance failed") }
      return false 
   }
   if(!_create_surface(win)){ 
      if(_is_debug()){ print("Vulkan: _create_surface failed") }
      return false 
   }
   if(!_pick_physical_device()){ 
      if(_is_debug()){ print("Vulkan: _pick_physical_device failed") }
      return false 
   }
   if(!_create_logical_device()){
      if(_is_debug()){ print("Vulkan: _create_logical_device failed") }
      return false
   }
   if(!_create_swapchain(win)){ 
      if(_is_debug()){ print("Vulkan: _create_swapchain failed") }
      return false 
   }
   if(_is_debug()){ print("Vulkan: init stage -> image views") }
   if(!_create_image_views()){ return false }
    if(_is_debug()){ print("Vulkan: init stage -> render pass") }
   if(!_create_render_pass()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> framebuffers") }
   if(!_create_framebuffers()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> sync objects") }
   if(!_create_sync_objects()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> command pool") }
   if(!_create_command_pool()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> command buffers") }
   if(!_create_command_buffers()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> graphics pipeline") }
   if(!_create_graphics_pipeline()){ return false }
   if(_is_debug()){ print("Vulkan: Renderer initialized successfully") }
   true
}

fn _create_instance(){
   "Auto-generated docstring: _create_instance."
   ;; Create all structures with system malloc to avoid any Nytrix metadata issues
   mut app_info = sys_malloc(48)
   memset(app_info, 0, 48)
   store32(app_info, VK_STRUCTURE_TYPE_APPLICATION_INFO, 0)
   store64(app_info, to_int(0), 8)      ;; pNext
   store64(app_info, to_int(0), 16)     ;; pApplicationName
   store32(app_info, 1, 24)
   store64(app_info, to_int(0), 32)     ;; pEngineName
   store32(app_info, 1, 40)
   store32(app_info, 0x00401000, 44)
   mut ext1 = sys_malloc(64) 
   mut i1 = 0 def s1 = "VK_KHR_surface" while(i1 < 14){ store8(ext1, load8(s1, i1), i1) i1 += 1 } store8(ext1, 0, 14)
   mut ext2 = sys_malloc(64)
   mut i2 = 0 def s2 = "VK_KHR_xlib_surface" while(i2 < 19){ store8(ext2, load8(s2, i2), i2) i2 += 1 } store8(ext2, 0, 19)
   mut ext_ptrs = sys_malloc(32)
   memset(ext_ptrs, 0, 32)
   store64(ext_ptrs, to_int(ext1), 0)
   store64(ext_ptrs, to_int(ext2), 8)
   ;; Create VkInstanceCreateInfo manually with explicit zeroing
   mut create_info = sys_malloc(64)
   memset(create_info, 0, 64)
   store32(create_info, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0)
   store64(create_info, to_int(0), 8)   ;; pNext
   store32(create_info, 0, 16)         ;; flags
   store64(create_info, to_int(app_info), 24)
   store32(create_info, 0, 32)         ;; layers
   store64(create_info, to_int(0), 40)
   store32(create_info, 2, 48)         ;; extensions
   store64(create_info, to_int(ext_ptrs), 56)
   mut inst_ptr = sys_malloc(8)
   store64(inst_ptr, to_int(0), 0)
   if(_is_debug()){ print(f"Vulkan: Creating instance with _vkCreateInstance @ {_vkCreateInstance}") }
   def res = call3(_vkCreateInstance, create_info, 0, inst_ptr)
   if(res != 0){ 
      if(_is_debug()){ print(f"Vulkan: Instance creation failed with code {res}") }
      return false 
   }
   if(_is_debug()){
      _dbg_handle("instance.out.raw", load64(inst_ptr, 0))
   }
   _instance = load64(inst_ptr, 0)
   if(_is_debug()){
      print("Vulkan: Instance created OK.")
      _dbg_handle("instance", _instance)
   }
   true
}

fn _create_surface(win){
   "Auto-generated docstring: _create_surface."
   ;; Pull native X11 handles from backend state/window metadata.
   def disp = ui_x11.native_display()
   def window = get(win, 22, 0)
   if(!_vkCreateXlibSurfaceKHR || !disp || !window){
      if(_is_debug()){ print("Vulkan: No X11 surface support or no display/window handle") }
      return false
   }
   mut create_info = sys_malloc(40)
   store32(create_info, VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, 0)
   store64(create_info, to_int(0), 8)    ;; pNext
   store32(create_info, 0, 16)          ;; flags
   ;; Store raw native handles into the C struct fields.
   ;; `storeptr` has inconsistent behavior in this runtime path.
   store64(create_info, to_int(disp), 24)   ;; Display*
   store64(create_info, to_int(window), 32) ;; Window (XID / unsigned long)
   mut surf_ptr = sys_malloc(8)
   store64(surf_ptr, 0, 0)
   def res = call4(_vkCreateXlibSurfaceKHR, _instance, create_info, 0, surf_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Surface creation failed with code {res}") }
      return false
   }
   if(_is_debug()){
      _dbg_handle("surface.out.raw", load64(surf_ptr, 0))
   }
   _surface = load64(surf_ptr, 0)
   if(_is_debug()){
      print(f"Vulkan: Surface created OK")
      _dbg_handle("surface", _surface)
   }
   true
}

fn _pick_physical_device(){
   "Auto-generated docstring: _pick_physical_device."
   if(!_vkEnumeratePhysicalDevices){ return false }
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   def res1 = call3(_vkEnumeratePhysicalDevices, _instance, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(_is_debug()){ print(f"Vulkan: Physical devices found: {count}") }
   if(count == 0){ return false }
   mut devices_ptr = sys_malloc(count * 8)
   call3(_vkEnumeratePhysicalDevices, _instance, count_ptr, devices_ptr)
   ;; Just pick the first device
   if(_is_debug()){
      _dbg_handle("physical.out.raw", load64(devices_ptr, 0))
   }
   _physical_device = load64(devices_ptr, 0)
   if(_is_debug()){
      print(f"Vulkan: Selected physical device")
      _dbg_handle("physical", _physical_device)
   }
   true
}

fn _create_logical_device(){
   "Auto-generated docstring: _create_logical_device."
   if(!_vkGetPhysicalDeviceQueueFamilyProperties || !_vkCreateDevice){ return false }
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   call3(_vkGetPhysicalDeviceQueueFamilyProperties, _physical_device, count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(count == 0){ return false }
   def prop_stride = 24
   mut props = sys_malloc(count * prop_stride)
   call3(_vkGetPhysicalDeviceQueueFamilyProperties, _physical_device, count_ptr, props)
   mut graphics_family = -1
   mut i = 0
   while(i < count){
      def flags = load32(props, i * prop_stride)
      if((flags & 1) != 0){ ;; VK_QUEUE_GRAPHICS_BIT
         graphics_family = i
         break
      }
      i += 1
   }
   if(graphics_family == -1){ 
      if(_is_debug()){ print("Vulkan: No graphics queue family found") }
      return false 
   }
   _graphics_family_index = graphics_family
   ;; Queue priority (1.0f in IEEE-754)
   mut priorities = sys_malloc(4)
   store32(priorities, 0x3f800000, 0)
   mut queue_create_info = sys_malloc(40)
   store32(queue_create_info, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0)
   store64(queue_create_info, to_int(0), 8)   ;; pNext
   store32(queue_create_info, 0, 16)          ;; flags
   store32(queue_create_info, graphics_family, 20) ;; queueFamilyIndex
   store32(queue_create_info, 1, 24)               ;; queueCount
   store64(queue_create_info, to_int(priorities), 32) ;; pQueuePriorities
   mut ext1 = sys_malloc(32)
   memset(ext1, 0, 32)
   _strcpy(ext1, "VK_KHR_swapchain")
   mut ext_ptrs = sys_malloc(8)
   store64(ext_ptrs, to_int(ext1), 0)
   mut create_info = sys_malloc(72)
   store32(create_info, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0)
   store64(create_info, to_int(0), 8)                   ;; pNext
   store32(create_info, 0, 16)                         ;; flags
   store32(create_info, 1, 20)                         ;; queueCreateInfoCount
   store64(create_info, to_int(queue_create_info), 24) ;; pQueueCreateInfos
   store32(create_info, 0, 32)                         ;; enabledLayerCount
   store64(create_info, to_int(0), 40)                 ;; ppEnabledLayerNames
   store32(create_info, 1, 48)                         ;; enabledExtensionCount
   store64(create_info, to_int(ext_ptrs), 56)          ;; ppEnabledExtensionNames
   store64(create_info, to_int(0), 64)                 ;; pEnabledFeatures
   mut dev_ptr = sys_malloc(8)
   store64(dev_ptr, to_int(0), 0)
   if(_is_debug()){
      _dbg_handle("device.out.pre", load64(dev_ptr, 0))
   }
   def res = call4(_vkCreateDevice, _physical_device, create_info, 0, dev_ptr)
   if(res != 0){ 
      if(_is_debug()){ print(f"Vulkan: Logical device creation failed with code {res}") }
      return false 
   }
   if(_is_debug()){
      _dbg_handle("device.out.post", load64(dev_ptr, 0))
   }
   _device = load64(dev_ptr, 0)
   if(_is_debug()){
      print(f"Vulkan: Logical device created OK")
      _dbg_handle("device", _device)
   }
   mut q_ptr = sys_malloc(8)
   store64(q_ptr, to_int(0), 0)
   call4(_vkGetDeviceQueue, _device, graphics_family, 0, q_ptr)
   if(_is_debug()){
      _dbg_handle("queue.out.post", load64(q_ptr, 0))
   }
   _graphics_queue = load64(q_ptr, 0)
   if(_is_debug()){
      _dbg_handle("queue", _graphics_queue)
   }
   ;; Use same queue for presenting for now (most GPUs support this)
   _present_queue = _graphics_queue
   true
}

fn _choose_composite_alpha(flags){
   "Auto-generated docstring: _choose_composite_alpha."
   if((flags & 0x1) != 0){ return 0x1 } ;; VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
   if((flags & 0x2) != 0){ return 0x2 } ;; PRE_MULTIPLIED
   if((flags & 0x4) != 0){ return 0x4 } ;; POST_MULTIPLIED
   if((flags & 0x8) != 0){ return 0x8 } ;; INHERIT
   0x1
}

fn _create_swapchain(win){
   "Auto-generated docstring: _create_swapchain."
   mut caps = sys_malloc(128) ;; VkSurfaceCapabilitiesKHR
   memset(caps, 0, 128)
   call3(_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, _physical_device, _surface, caps)
   mut req_w = int(get(win, 5, 1))
   mut req_h = int(get(win, 6, 1))
   if(req_w < 1){ req_w = 1 }
   if(req_h < 1){ req_h = 1 }
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
   ;; Min image count + 1 for smoother frame pacing when possible.
   mut min_imgs = load32(caps, 0)
   mut max_imgs = load32(caps, 4)
   mut count = min_imgs + 1
   if(max_imgs > 0 && count > max_imgs){ count = max_imgs }
   def pre_transform = load32(caps, 40) ;; currentTransform
   def composite_alpha = _choose_composite_alpha(load32(caps, 44)) ;; supportedCompositeAlpha
   mut create_info = sys_malloc(128)
   memset(create_info, 0, 128)
   store32(create_info, VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0)
   store64(create_info, to_int(0), 8)   ;; pNext
   store32(create_info, 0, 16)         ;; flags
   store64(create_info, _surface, 24)
   store32(create_info, count, 32)
   store32(create_info, VK_FORMAT_B8G8R8A8_UNORM, 36) ;; format
   store32(create_info, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 40) ;; colorSpace
   store32(create_info, w, 44) ;; extent.width
   store32(create_info, h, 48) ;; extent.height
   store32(create_info, 1, 52) ;; imageArrayLayers
   store32(create_info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 56)
   store32(create_info, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(create_info, 0, 64) ;; queueCount
   store64(create_info, to_int(0), 72) ;; pQueueFamilyIndices
   store32(create_info, pre_transform, 80) ;; preTransform
   store32(create_info, composite_alpha, 84) ;; compositeAlpha
   store32(create_info, VK_PRESENT_MODE_FIFO_KHR, 88)
   store32(create_info, 1, 92) ;; clipped
   store64(create_info, to_int(0), 96) ;; oldSwapchain
   mut sc_ptr = sys_malloc(8)
   store64(sc_ptr, to_int(0), 0)
   if(_is_debug()){
      _dbg_handle("swapchain.out.pre", load64(sc_ptr, 0))
   }
   def res = call4(_vkCreateSwapchainKHR, _device, create_info, 0, sc_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Swapchain creation failed with code {res}") }
      return false
   }
   if(_is_debug()){
      _dbg_handle("swapchain.out.post", load64(sc_ptr, 0))
   }
   _swapchain = load64(sc_ptr, 0)
   if(_is_debug()){ _dbg_handle("swapchain", _swapchain) }
   _swapchain_format = VK_FORMAT_B8G8R8A8_UNORM
   ;; Get images
   mut img_count_ptr = sys_malloc(4)
   store32(img_count_ptr, 0, 0)
   def gi1 = call4(_vkGetSwapchainImagesKHR, _device, _swapchain, img_count_ptr, 0)
   if(_is_debug()){ print(f"Vulkan: vkGetSwapchainImagesKHR(count) res={gi1} count={load32(img_count_ptr, 0)}") }
   _swapchain_image_count = load32(img_count_ptr, 0)
   mut imgs_ptr = sys_malloc(_swapchain_image_count * 8)
   def gi2 = call4(_vkGetSwapchainImagesKHR, _device, _swapchain, img_count_ptr, imgs_ptr)
   if(_is_debug()){ print(f"Vulkan: vkGetSwapchainImagesKHR(images) res={gi2}") }
   _swapchain_images = []
   mut i = 0
   while(i < _swapchain_image_count){
      def img = load64(imgs_ptr, i * 8)
      if(_is_debug()){ _dbg_handle(f"swapchain.image[{i}]", img) }
      _swapchain_images = append(_swapchain_images, img)
      i += 1
   }
   if(_is_debug()){ print(f"Vulkan: Swapchain created with {_swapchain_image_count} images") }
   true
}

fn _destroy_swapchain_objects(){
   "Auto-generated docstring: _destroy_swapchain_objects."
   if(!_device){ return 0 }
   mut i = 0
   while(i < len(_framebuffers)){
      def fb = get(_framebuffers, i, 0)
      if(fb){ call3(_vkDestroyFramebuffer, _device, fb, 0) }
      i += 1
   }
   _framebuffers = []
   i = 0
   while(i < len(_swapchain_image_views)){
      def iv = get(_swapchain_image_views, i, 0)
      if(iv){ call3(_vkDestroyImageView, _device, iv, 0) }
      i += 1
   }
   _swapchain_image_views = []
   if(_swapchain){
      call3(_vkDestroySwapchainKHR, _device, _swapchain, 0)
      _swapchain = 0
   }
   _swapchain_images = []
   _swapchain_image_count = 0
   0
}

fn _recreate_swapchain(){
   "Auto-generated docstring: _recreate_swapchain."
   if(!_window_ref || !_device){ return false }
   if(_is_debug()){ print("Vulkan: Recreating swapchain for window resize/out-of-date") }
   device_wait_idle(_device)
   _destroy_swapchain_objects()
   if(!_create_swapchain(_window_ref)){ return false }
   if(!_create_image_views()){ return false }
   if(!_create_framebuffers()){ return false }
   true
}

fn _create_image_views(){
   "Auto-generated docstring: _create_image_views."
   _swapchain_image_views = []
   mut i = 0
   while(i < _swapchain_image_count){
      def image_handle = get(_swapchain_images, i)
      if(_is_debug()){ _dbg_handle(f"image_view.image[{i}]", image_handle) }
      mut create_info = sys_malloc(80)
      memset(create_info, 0, 80)
      store32(create_info, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store64(create_info, to_int(0), 8) ;; pNext
      store32(create_info, 0, 16) ;; flags
      store64(create_info, image_handle, 24)
      store32(create_info, 1, 32) ;; viewType (2D = 1)
      store32(create_info, _swapchain_format, 36)
      ;; components (all identity=0)
      ;; subresourceRange
      store32(create_info, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(create_info, 0, 60) ;; baseMipLevel
      store32(create_info, 1, 64) ;; levelCount
      store32(create_info, 0, 68) ;; baseArrayLayer
      store32(create_info, 1, 72) ;; layerCount
      mut view_ptr = sys_malloc(8)
      def iv_res = call4(_vkCreateImageView, _device, create_info, 0, view_ptr)
      if(iv_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateImageView failed at image[{i}] with code {iv_res}") }
         return false
      }
      def view_h = load64(view_ptr, 0)
      if(_is_debug()){ _dbg_handle(f"image_view.out[{i}]", view_h) }
      _swapchain_image_views = append(_swapchain_image_views, view_h)
      i += 1
   }
   true
}

fn _create_render_pass(){
   "Auto-generated docstring: _create_render_pass."
   ;; attachment
   mut ad = sys_malloc(36)
   memset(ad, 0, 36)
   store32(ad, _swapchain_format, 4) ;; format
   store32(ad, 1, 8) ;; samples (1 = VK_SAMPLE_COUNT_1_BIT)
   store32(ad, 2, 12) ;; loadOp (2 = CLEAR)
   store32(ad, 1, 16) ;; storeOp (1 = STORE)
   store32(ad, 0, 20) ;; stencilLoadOp
   store32(ad, 0, 24) ;; stencilStoreOp
   store32(ad, 0, 28) ;; initialLayout (0 = UNDEFINED)
   store32(ad, 1000001002, 32) ;; finalLayout (PRESENT_SRC_KHR)
   ;; color_attachment_ref
   mut car = sys_malloc(8)
   store32(car, 0, 0) ;; attachment index
   store32(car, 2, 4) ;; layout (2 = COLOR_ATTACHMENT_OPTIMAL)
   ;; subpass
   mut sd = sys_malloc(72)
   memset(sd, 0, 72)
   store32(sd, 0, 0)  ;; flags
   store32(sd, 0, 4)  ;; pipelineBindPoint (0 = GRAPHICS)
   store32(sd, 0, 8)  ;; inputAttachmentCount
   store64(sd, to_int(0), 16) ;; pInputAttachments
   store32(sd, 1, 24) ;; colorAttachmentCount
   store64(sd, to_int(car), 32) ;; pColorAttachments
   store64(sd, to_int(0), 40) ;; pResolveAttachments
   store64(sd, to_int(0), 48) ;; pDepthStencilAttachment
   store32(sd, 0, 56) ;; preserveAttachmentCount
   store64(sd, to_int(0), 64) ;; pPreserveAttachments
   mut create_info = sys_malloc(64)
   memset(create_info, 0, 64)
   store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
   store64(create_info, to_int(0), 8) ;; pNext
   store32(create_info, 0, 16) ;; flags
   store32(create_info, 1, 20) ;; attachmentCount
   store64(create_info, to_int(ad), 24) ;; pAttachments
   store32(create_info, 1, 32) ;; subpassCount
   store64(create_info, to_int(sd), 40) ;; pSubpasses
   store32(create_info, 0, 48) ;; dependencyCount
   store64(create_info, to_int(0), 56) ;; pDependencies
   mut pass_ptr = sys_malloc(8)
   def rp_res = call4(_vkCreateRenderPass, _device, create_info, 0, pass_ptr)
   if(rp_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreateRenderPass failed with code {rp_res}") }
      return false
   }
   _render_pass = load64(pass_ptr, 0)
   if(_is_debug()){ _dbg_handle("render_pass", _render_pass) }
   true
}

fn _create_framebuffers(){
   "Auto-generated docstring: _create_framebuffers."
   _framebuffers = []
   mut i = 0
   while(i < _swapchain_image_count){
      mut attach_ptr = sys_malloc(8)
      store64(attach_ptr, get(_swapchain_image_views, i), 0)
      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0)
      store64(create_info, to_int(0), 8) ;; pNext
      store32(create_info, 0, 16) ;; flags
      store64(create_info, _render_pass, 24)
      store32(create_info, 1, 32) ;; attachmentCount
      store64(create_info, to_int(attach_ptr), 40)
      store32(create_info, _swapchain_extent_w, 48)
      store32(create_info, _swapchain_extent_h, 52)
      store32(create_info, 1, 56) ;; layers
      mut fb_ptr = sys_malloc(8)
      def fb_res = call4(_vkCreateFramebuffer, _device, create_info, 0, fb_ptr)
      if(fb_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateFramebuffer failed at {i} with code {fb_res}") }
         return false
      }
      def fb_h = load64(fb_ptr, 0)
      if(_is_debug()){ _dbg_handle(f"framebuffer[{i}]", fb_h) }
      _framebuffers = append(_framebuffers, fb_h)
      i += 1
   }
   true
}

fn _create_sync_objects(){
   "Auto-generated docstring: _create_sync_objects."
   _image_available_semaphores = []
   _render_finished_semaphores = []
   _in_flight_fences = []
   mut i = 0
   while(i < MAX_FRAMES_IN_FLIGHT){
      mut si = sys_malloc(24)
      memset(si, 0, 24)
      store32(si, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0)
      store64(si, to_int(0), 8) ;; pNext
      store32(si, 0, 16) ;; flags
      mut sem1 = sys_malloc(8)
      def s1_res = call4(_vkCreateSemaphore, _device, si, 0, sem1)
      if(s1_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateSemaphore(imageAvailable) failed at {i} with code {s1_res}") }
         return false
      }
      _image_available_semaphores = append(_image_available_semaphores, load64(sem1, 0))
      mut sem2 = sys_malloc(8)
      def s2_res = call4(_vkCreateSemaphore, _device, si, 0, sem2)
      if(s2_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateSemaphore(renderFinished) failed at {i} with code {s2_res}") }
         return false
      }
      _render_finished_semaphores = append(_render_finished_semaphores, load64(sem2, 0))
      mut fi = sys_malloc(24)
      memset(fi, 0, 24)
      store32(fi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
      store64(fi, to_int(0), 8) ;; pNext
      store32(fi, 1, 16) ;; flags (1 = SIGNAL_BIT)
      mut fence = sys_malloc(8)
      def f_res = call4(_vkCreateFence, _device, fi, 0, fence)
      if(f_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateFence failed at {i} with code {f_res}") }
         return false
      }
      _in_flight_fences = append(_in_flight_fences, load64(fence, 0))
      i += 1
   }
   true
}

fn _create_command_pool(){
   "Auto-generated docstring: _create_command_pool."
   mut create_info = sys_malloc(32)
   memset(create_info, 0, 32)
   store32(create_info, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0)
   store32(create_info, 2, 16) ;; flags (2 = RESET_BIT)
   store32(create_info, _graphics_family_index, 20)
   mut pool_ptr = sys_malloc(8)
   def cp_res = call4(_vkCreateCommandPool, _device, create_info, 0, pool_ptr)
   if(cp_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreateCommandPool failed with code {cp_res}") }
      return false
   }
   _command_pool = load64(pool_ptr, 0)
    if(_is_debug()){ _dbg_handle("command_pool", _command_pool) }
   true
}

fn _create_command_buffers(){
   "Auto-generated docstring: _create_command_buffers."
   mut ai = sys_malloc(32)
   memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64(ai, _command_pool, 16)
   store32(ai, 0, 24) ;; level (0 = PRIMARY)
   store32(ai, MAX_FRAMES_IN_FLIGHT, 28)
   mut bufs_ptr = sys_malloc(MAX_FRAMES_IN_FLIGHT * 8)
   def cb_res = call3(_vkAllocateCommandBuffers, _device, ai, bufs_ptr)
   if(cb_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkAllocateCommandBuffers failed with code {cb_res}") }
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

fn _create_shader_module(path){
   "Auto-generated docstring: _create_shader_module."
   def res = file_read(path)
   if(is_err(res)){ 
      if(_is_debug()){ print(f"Vulkan: Failed to read shader {path}") }
      return 0 
   }
   def code = unwrap(res)
   def size = len(code)
   mut ci = sys_malloc(128)
   memset(ci, 0, 128)
   store32(ci, 16, 0) ;; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store64(ci, to_int(0), 8)
   store32(ci, 0, 16) ;; flags
   store64(ci, to_int(size), 24) ;; codeSize (bytes)
   store64(ci, to_int(code), 32) ;; pCode
   mut mod_ptr = sys_malloc(8)
   def vk_res = call4(_vkCreateShaderModule, _device, ci, 0, mod_ptr)
   if(vk_res != 0){
      if(_is_debug()){ print(f"Vulkan: Failed to create shader module for {path}, code {vk_res}") }
      return 0
   }
   load64(mod_ptr, 0)
}

fn _ensure_shader_binaries(){
   "Auto-generated docstring: _ensure_shader_binaries."
   def rect_vert_spv = "build/cache/rect.vert.spv"
   def rect_frag_spv = "build/cache/rect.frag.spv"
   def tri_vert_spv = "build/cache/tri.tri_vert.spv"
   def tri_frag_spv = "build/cache/tri.tri_frag.spv"
   if(_is_debug()){ print("Vulkan: Generating shader binaries with glslc...") }
   ;; Vulkan NDC uses +Y down in this viewport convention; do not apply GL-style Y inversion.
   def rect_vert_src = "#version 450\nlayout(push_constant) uniform PC { vec4 p0; vec4 p1; vec4 p2; } pc;\nlayout(location = 0) out vec4 vColor;\nvoid main(){\n  vec2 local = vec2((gl_VertexIndex == 1 || gl_VertexIndex == 2) ? 1.0 : 0.0, (gl_VertexIndex >= 2) ? 1.0 : 0.0);\n  vec2 pos = pc.p0.xy + local * pc.p0.zw;\n  float nx = (pos.x / pc.p2.x) * 2.0 - 1.0;\n  float ny = (pos.y / pc.p2.y) * 2.0 - 1.0;\n  gl_Position = vec4(nx, ny, 0.0, 1.0);\n  vColor = pc.p1;\n}\n"
   def rect_frag_src = "#version 450\nlayout(location = 0) in vec4 vColor;\nlayout(location = 0) out vec4 outColor;\nvoid main(){ outColor = vColor; }\n"
   def tri_vert_src = "#version 450\nlayout(push_constant) uniform PC { vec4 p0; vec4 p1; vec4 p2; } pc;\nlayout(location = 0) out vec4 vColor;\nvoid main(){\n  vec2 v0 = pc.p0.xy;\n  vec2 v1 = pc.p0.zw;\n  vec2 v2 = pc.p1.xy;\n  vec2 pos = (gl_VertexIndex == 0) ? v0 : ((gl_VertexIndex == 1) ? v1 : v2);\n  float nx = (pos.x / pc.p2.z) * 2.0 - 1.0;\n  float ny = (pos.y / pc.p2.w) * 2.0 - 1.0;\n  gl_Position = vec4(nx, ny, 0.0, 1.0);\n  vColor = vec4(pc.p1.z, pc.p1.w, pc.p2.x, pc.p2.y);\n}\n"
   def tri_frag_src = "#version 450\nlayout(location = 0) in vec4 vColor;\nlayout(location = 0) out vec4 outColor;\nvoid main(){ outColor = vColor; }\n"
   if(is_err(file_write("build/cache/rect.vert", rect_vert_src))){ return false }
   if(is_err(file_write("build/cache/rect.frag", rect_frag_src))){ return false }
   if(is_err(file_write("build/cache/tri.vert", tri_vert_src))){ return false }
   if(is_err(file_write("build/cache/tri.frag", tri_frag_src))){ return false }
   if(proc.run("glslc", ["glslc", "build/cache/rect.vert", "-o", rect_vert_spv]) != 0){ return false }
   if(proc.run("glslc", ["glslc", "build/cache/rect.frag", "-o", rect_frag_spv]) != 0){ return false }
   if(proc.run("glslc", ["glslc", "build/cache/tri.vert", "-o", tri_vert_spv]) != 0){ return false }
   if(proc.run("glslc", ["glslc", "build/cache/tri.frag", "-o", tri_frag_spv]) != 0){ return false }
   file_exists(rect_vert_spv) && file_exists(rect_frag_spv) &&
   file_exists(tri_vert_spv) && file_exists(tri_frag_spv)
}

fn _create_graphics_pipeline(){
   "Auto-generated docstring: _create_graphics_pipeline."
   if(!_ensure_shader_binaries()){
      if(_is_debug()){ print("Vulkan: Could not prepare shader binaries") }
      return false
   }
   _vert_module = _create_shader_module("build/cache/rect.vert.spv")
   _frag_module = _create_shader_module("build/cache/rect.frag.spv")
   _tri_vert_module = _create_shader_module("build/cache/tri.tri_vert.spv")
   _tri_frag_module = _create_shader_module("build/cache/tri.tri_frag.spv")
   if(!_vert_module || !_frag_module || !_tri_vert_module || !_tri_frag_module){ return false }
   ;; 1. Global Pipeline Layout (64 bytes PC)
   mut pc_range = sys_malloc(12)
   store32(pc_range, 1, 0) ;; STAGE_VERTEX
   store32(pc_range, 0, 4)
   store32(pc_range, 64, 8)
   mut layout_ci = sys_malloc(48)
   memset(layout_ci, 0, 48)
   store32(layout_ci, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0)
   store64(layout_ci, to_int(0), 8) ;; pNext
   store32(layout_ci, 0, 16) ;; flags
   store32(layout_ci, 0, 20) ;; setLayoutCount
   store64(layout_ci, to_int(0), 24) ;; pSetLayouts
   store32(layout_ci, 1, 32) ;; pushConstantRangeCount
   store64(layout_ci, to_int(pc_range), 40) ;; pPushConstantRanges
   mut layout_ptr = sys_malloc(8)
   def pl_res = call4(_vkCreatePipelineLayout, _device, layout_ci, 0, layout_ptr)
   if(pl_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreatePipelineLayout failed with code {pl_res}") }
      return false
   }
   _pipeline_layout = load64(layout_ptr, 0)
   if(_is_debug()){ _dbg_handle("pipeline_layout", _pipeline_layout) }
   ;; Common States
   mut vi = sys_malloc(48)
   memset(vi, 0, 48)
   store32(vi, 19, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
   mut viewport_state = sys_malloc(48)
   memset(viewport_state, 0, 48)
   store32(viewport_state, 22, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
   store32(viewport_state, 1, 20) ;; viewportCount
   store32(viewport_state, 1, 32) ;; scissorCount
   mut rs = sys_malloc(64)
   memset(rs, 0, 64)
   store32(rs, 23, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
   store32(rs, 0, 20) ;; depthClampEnable
   store32(rs, 0, 24) ;; rasterizerDiscardEnable
   store32(rs, 0, 28) ;; polygonMode = FILL
   store32(rs, 0, 32) ;; cullMode = NONE
   store32(rs, 0, 36) ;; frontFace
   store32(rs, 0, 40) ;; depthBiasEnable
   store32_f32(rs, 1.0, 56) ;; lineWidth
   mut ms = sys_malloc(64)
   memset(ms, 0, 64)
   store32(ms, 24, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
   store32(ms, 1, 20) ;; rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
   mut cba = sys_malloc(32)
   memset(cba, 0, 32)
   ;; Standard source-alpha blending for UI and glyph coverage.
   store32(cba, 1, 0)  ;; blendEnable
   store32(cba, 6, 4)  ;; srcColorBlendFactor = SRC_ALPHA
   store32(cba, 7, 8)  ;; dstColorBlendFactor = ONE_MINUS_SRC_ALPHA
   store32(cba, 0, 12) ;; colorBlendOp = ADD
   store32(cba, 1, 16) ;; srcAlphaBlendFactor = ONE
   store32(cba, 7, 20) ;; dstAlphaBlendFactor = ONE_MINUS_SRC_ALPHA
   store32(cba, 0, 24) ;; alphaBlendOp = ADD
   store32(cba, 15, 28) ;; colorWriteMask RGBA
   mut cb = sys_malloc(64)
   memset(cb, 0, 64)
   store32(cb, 26, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
   store32(cb, 0, 20) ;; logicOpEnable
   store32(cb, 0, 24) ;; logicOp
   store32(cb, 1, 28) ;; attachmentCount
   store64(cb, to_int(cba), 32) ;; pAttachments
   mut dyn_states = sys_malloc(8)
   store32(dyn_states, 0, 0) ;; VK_DYNAMIC_STATE_VIEWPORT
   store32(dyn_states, 1, 4) ;; VK_DYNAMIC_STATE_SCISSOR
   mut ds = sys_malloc(32)
   memset(ds, 0, 32)
   store32(ds, 27, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
   store32(ds, 2, 20) ;; dynamicStateCount
   store64(ds, to_int(dyn_states), 24) ;; pDynamicStates
   ;; 2. Rect Pipeline (STRIP)
   mut stages = sys_malloc(96)
   memset(stages, 0, 96)
   mut main_str = sys_malloc(8)
   store8(main_str, 109, 0) ;; m
   store8(main_str, 97, 1)  ;; a
   store8(main_str, 105, 2) ;; i
   store8(main_str, 110, 3) ;; n
   store8(main_str, 0, 4)
   ;; Vertex stage
   store32(stages, 18, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
   store32(stages, 1, 20) ;; stage = VERTEX
   store64(stages, _vert_module, 24)
   store64(stages, to_int(main_str), 32)
   ;; Fragment stage
   store32(stages, 18, 48)
   store32(stages, 16, 68) ;; stage = FRAGMENT
   store64(stages, _frag_module, 72)
   store64(stages, to_int(main_str), 80)
   mut ia_rect = sys_malloc(32)
   memset(ia_rect, 0, 32)
   store32(ia_rect, 20, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
   store32(ia_rect, 5, 20) ;; topology = TRIANGLE_STRIP
   store32(ia_rect, 0, 24) ;; primitiveRestartEnable
   mut ci = sys_malloc(144)
   memset(ci, 0, 144)
   store32(ci, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0)
   store32(ci, 2, 20) ;; stageCount
   store64(ci, to_int(stages), 24) ;; pStages
   store64(ci, to_int(vi), 32) ;; pVertexInputState
   store64(ci, to_int(ia_rect), 40) ;; pInputAssemblyState
   store64(ci, to_int(0), 48) ;; pTessellationState
   store64(ci, to_int(viewport_state), 56) ;; pViewportState
   store64(ci, to_int(rs), 64) ;; pRasterizationState
   store64(ci, to_int(ms), 72) ;; pMultisampleState
   store64(ci, to_int(0), 80) ;; pDepthStencilState
   store64(ci, to_int(cb), 88) ;; pColorBlendState
   store64(ci, to_int(ds), 96) ;; pDynamicState
   store64(ci, _pipeline_layout, 104)
   store64(ci, _render_pass, 112)
   store32(ci, 0, 120) ;; subpass
   store64(ci, to_int(0), 128) ;; basePipelineHandle
   store32(ci, -1, 136) ;; basePipelineIndex
   mut pipe_ptr = sys_malloc(8)
   def gp_res = call6(_vkCreateGraphicsPipelines, _device, 0, 1, ci, 0, pipe_ptr)
   if(gp_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreateGraphicsPipelines(rect) failed with code {gp_res}") }
      return false
   }
   _rect_pipeline = load64(pipe_ptr, 0)
   if(_is_debug()){ _dbg_handle("rect_pipeline", _rect_pipeline) }
   ;; 3. Tri Pipeline (LIST)
   memset(stages, 0, 96)
   store32(stages, 18, 0)
   store32(stages, 1, 20)
   store64(stages, _tri_vert_module, 24)
   store64(stages, to_int(main_str), 32)
   store32(stages, 18, 48)
   store32(stages, 16, 68)
   store64(stages, _tri_frag_module, 72)
   store64(stages, to_int(main_str), 80)
   mut ia_tri = sys_malloc(32)
   memset(ia_tri, 0, 32)
   store32(ia_tri, 20, 0)
   store32(ia_tri, 3, 20) ;; topology = TRIANGLE_LIST
   store32(ia_tri, 0, 24)
   store64(ci, to_int(ia_tri), 40)
   def gp_res2 = call6(_vkCreateGraphicsPipelines, _device, 0, 1, ci, 0, pipe_ptr)
   if(gp_res2 != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreateGraphicsPipelines(tri) failed with code {gp_res2}") }
      return false
   }
   _tri_pipeline = load64(pipe_ptr, 0)
   if(_is_debug()){ _dbg_handle("tri_pipeline", _tri_pipeline) }
   true
}

fn begin_frame(){
   "Auto-generated docstring: begin_frame."
   _frame_open = false
   if(_window_ref){
      def nw = get(_window_ref, 5, _swapchain_extent_w)
      def nh = get(_window_ref, 6, _swapchain_extent_h)
      if(nw <= 0 || nh <= 0){ return false }
      if(nw != _swapchain_extent_w || nh != _swapchain_extent_h){
         if(!_recreate_swapchain()){ return false }
      }
   }
   ;; Wait for previous frame's fence
   def fence = get(_in_flight_fences, _current_frame)
   mut fence_ptr = sys_malloc(8)
   store64(fence_ptr, fence, 0)
   def wf = call5(_vkWaitForFences, _device, 1, fence_ptr, 1, 0xFFFFFFFFFFFFFFFF)
   if(wf != 0){
      sys_free(fence_ptr)
      return false
   }
   def rf = call3(_vkResetFences, _device, 1, fence_ptr)
   if(rf != 0){
      sys_free(fence_ptr)
      return false
   }
   ;; Acquire next image
   mut img_idx_ptr = sys_malloc(4)
   def sem = get(_image_available_semaphores, _current_frame)
   def acq = call6(_vkAcquireNextImageKHR, _device, _swapchain, 0xFFFFFFFFFFFFFFFF, sem, 0, img_idx_ptr)
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
   _image_index = load32(img_idx_ptr, 0)
   ;; Begin recording
   def cb = get(_command_buffers, _current_frame)
   mut bi = sys_malloc(32)
   memset(bi, 0, 32)
   store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   def bcb = call2(_vkBeginCommandBuffer, cb, bi)
   if(bcb != 0){
      if(_is_debug()){ print(f"Vulkan: vkBeginCommandBuffer failed with code {bcb}") }
      sys_free(fence_ptr)
      sys_free(img_idx_ptr)
      sys_free(bi)
      return false
   }
   ;; Begin Render Pass
   mut clear_color = sys_malloc(16)
   store32(clear_color, 0, 0) store32(clear_color, 0, 4) store32(clear_color, 0, 8) store32(clear_color, 0x3f800000, 12) ;; RGBA(0,0,0,1)
   mut ri = sys_malloc(64)
   memset(ri, 0, 64)
   store32(ri, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0)
   store64(ri, _render_pass, 16)
   store64(ri, get(_framebuffers, _image_index), 24)
   store32(ri, 0, 32) store32(ri, 0, 36) ;; offset
   store32(ri, _swapchain_extent_w, 40) store32(ri, _swapchain_extent_h, 44) ;; extent
   store32(ri, 1, 48) ;; clearValueCount
   store64(ri, to_int(clear_color), 56) ;; pClearValues
   call3(_vkCmdBeginRenderPass, cb, ri, 0) ;; 0 = VK_SUBPASS_CONTENTS_INLINE
   _frame_open = true
   sys_free(fence_ptr)
   sys_free(img_idx_ptr)
   sys_free(bi)
   sys_free(clear_color)
   sys_free(ri)
   true
}

fn end_frame(){
   "Auto-generated docstring: end_frame."
   if(!_frame_open){ return false }
   def cb = get(_command_buffers, _current_frame)
   call1(_vkCmdEndRenderPass, cb)
   def ecb = call1(_vkEndCommandBuffer, cb)
   if(ecb != 0){
      if(_is_debug()){ print(f"Vulkan: vkEndCommandBuffer failed with code {ecb}") }
      return false
   }
   mut wait_sems = sys_malloc(8) store64(wait_sems, get(_image_available_semaphores, _current_frame), 0)
   mut signal_sems = sys_malloc(8) store64(signal_sems, get(_render_finished_semaphores, _current_frame), 0)
   mut wait_stages = sys_malloc(4) store32(wait_stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0)
   mut cb_ptr = sys_malloc(8) store64(cb_ptr, cb, 0)
   mut si = sys_malloc(128)
   memset(si, 0, 128)
   store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(si, 1, 16) ;; waitSemaphoreCount
   store64(si, to_int(wait_sems), 24)
   store64(si, to_int(wait_stages), 32)
   store32(si, 1, 40) ;; commandBufferCount
   store64(si, to_int(cb_ptr), 48)
   store32(si, 1, 56) ;; signalSemaphoreCount
   store64(si, to_int(signal_sems), 64)
   def fence = get(_in_flight_fences, _current_frame)
   def sub_res = call4(_vkQueueSubmit, _graphics_queue, 1, si, fence)
   if(sub_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkQueueSubmit failed with code {sub_res}") }
      sys_free(wait_sems)
      sys_free(signal_sems)
      sys_free(wait_stages)
      sys_free(cb_ptr)
      sys_free(si)
      return false
   }
   mut scs = sys_malloc(8) store64(scs, _swapchain, 0)
   mut idxs = sys_malloc(4) store32(idxs, _image_index, 0)
   mut pi = sys_malloc(64)
   memset(pi, 0, 64)
   store32(pi, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0)
   store32(pi, 1, 16) ;; waitSemaphoreCount
   store64(pi, to_int(signal_sems), 24)
   store32(pi, 1, 32) ;; swapchainCount
   store64(pi, to_int(scs), 40)
   store64(pi, to_int(idxs), 48)
   def pr = call2(_vkQueuePresentKHR, _present_queue, pi)
   if(pr == 3294966292 || pr == -1000001004){ ;; VK_ERROR_OUT_OF_DATE_KHR
      _frame_open = false
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
   _frame_open = false
   _current_frame = (_current_frame + 1) % MAX_FRAMES_IN_FLIGHT
   true
}

fn clear(r, g, b, a){
   "Auto-generated docstring: clear."
   if(!_frame_open){ return 0 }
   def cb = get(_command_buffers, _current_frame)
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
   store32(rect, _swapchain_extent_w, 8) store32(rect, _swapchain_extent_h, 12) ;; extent
   store32(rect, 0, 16) ;; baseArrayLayer
   store32(rect, 1, 20) ;; layerCount
   call5(_vkCmdClearAttachments, cb, 1, ca, 1, rect)
   sys_free(ca)
   sys_free(rect)
}

fn draw_rect(x, y, w, h, r, g, b, a){
   "Auto-generated docstring: draw_rect."
   if(!_frame_open){ return 0 }
   def cb = get(_command_buffers, _current_frame)
   call3(_vkCmdBindPipeline, cb, 0, _rect_pipeline) ;; 0 = VK_PIPELINE_BIND_POINT_GRAPHICS
   ;; Set Viewport
   mut vp = sys_malloc(24)
   store32_f32(vp, 0.0, 0) store32_f32(vp, 0.0, 4)
   store32_f32(vp, _swapchain_extent_w, 8) store32_f32(vp, _swapchain_extent_h, 12)
   store32_f32(vp, 0.0, 16) store32_f32(vp, 1.0, 20)
   call4(_vkCmdSetViewport, cb, 0, 1, vp)
   ;; Set Scissor
   mut sc = sys_malloc(16)
   store32(sc, 0, 0) store32(sc, 0, 4) ;; offset
   store32(sc, _swapchain_extent_w, 8) store32(sc, _swapchain_extent_h, 12) ;; extent
   call4(_vkCmdSetScissor, cb, 0, 1, sc)
   ;; Push Constants (vec4 rect, vec4 color, vec2 screen)
   mut pc = sys_malloc(64)
   memset(pc, 0, 64)
   store32_f32(pc, x, 0) store32_f32(pc, y, 4) store32_f32(pc, w, 8) store32_f32(pc, h, 12)
   store32_f32(pc, r, 16) store32_f32(pc, g, 20) store32_f32(pc, b, 24) store32_f32(pc, a, 28)
   store32_f32(pc, _swapchain_extent_w, 32) store32_f32(pc, _swapchain_extent_h, 36)
   call6(_vkCmdPushConstants, cb, _pipeline_layout, 1, 0, 64, pc)
   call5(_vkCmdDraw, cb, 4, 1, 0, 0)
   sys_free(vp)
   sys_free(sc)
   sys_free(pc)
}

fn draw_triangle(x1, y1, x2, y2, x3, y3, r, g, b, a){
   "Auto-generated docstring: draw_triangle."
   if(!_frame_open){ return 0 }
   def cb = get(_command_buffers, _current_frame)
   call3(_vkCmdBindPipeline, cb, 0, _tri_pipeline)
   mut vp = sys_malloc(24)
   store32_f32(vp, 0.0, 0) store32_f32(vp, 0.0, 4)
   store32_f32(vp, _swapchain_extent_w, 8) store32_f32(vp, _swapchain_extent_h, 12)
   store32_f32(vp, 0.0, 16) store32_f32(vp, 1.0, 20)
   call4(_vkCmdSetViewport, cb, 0, 1, vp)
   mut sc = sys_malloc(16)
   store32(sc, 0, 0) store32(sc, 0, 4)
   store32(sc, _swapchain_extent_w, 8) store32(sc, _swapchain_extent_h, 12)
   call4(_vkCmdSetScissor, cb, 0, 1, sc)
   ;; Push Constants (vec2 v[3], vec4 color, vec2 screen)
   mut pc = sys_malloc(64)
   memset(pc, 0, 64)
   store32_f32(pc, x1, 0) store32_f32(pc, y1, 4)
   store32_f32(pc, x2, 8) store32_f32(pc, y2, 12)
   store32_f32(pc, x3, 16) store32_f32(pc, y3, 20)
   store32_f32(pc, r, 24) store32_f32(pc, g, 28) store32_f32(pc, b, 32) store32_f32(pc, a, 36)
   store32_f32(pc, _swapchain_extent_w, 40) store32_f32(pc, _swapchain_extent_h, 44)
   call6(_vkCmdPushConstants, cb, _pipeline_layout, 1, 0, 64, pc)
   call5(_vkCmdDraw, cb, 3, 1, 0, 0)
   sys_free(vp)
   sys_free(sc)
   sys_free(pc)
}

fn draw_line(x1, y1, x2, y2, thickness, r, g, b, a){
   "Auto-generated docstring: draw_line."
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

fn _strcpy(dst, src){
   "Auto-generated docstring: _strcpy."
   mut i = 0
   while(true){
      def c = load8(src, i)
      store8(dst, c, i)
      if(c == 0){ break }
      i += 1
   }
}

fn shutdown(){
   "Auto-generated docstring: shutdown."
   if(_device){ device_wait_idle(_device) }
   _destroy_swapchain_objects()
   if(_device){ destroy_device(_device, 0) }
   if(_surface){ destroy_surface_khr(_instance, _surface, 0) }
   if(_instance){ destroy_instance(_instance, 0) }
}
