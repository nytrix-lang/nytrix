;; Auto-generated split Vulkan renderer component
module std.ui.gfx.vk.setup (
  init,
  shutdown,
  _recreate_swapchain,
  _create_swapchain,
  _create_image_views,
  _create_framebuffers
)
use std.core *
use std.core.mem *
use std.os *
use std.text.io as tio
use std.math *
use std.math.matrix *
use std.ui.glfw as ui_glfw
use std.ui.gfx.vulkan *
use std.ui.gfx.vk.state as vk_state
use std.ui.gfx.vk.pipeline *
use std.ui.gfx.vk.buffers *
use std.ui.gfx.vk.texture *
use std.ui.gfx.vk.utils *

fn init(win){
   print("setup.init: called with win=", win)
   "Initializes the Vulkan renderer for the given window."
   if(vk_state.vk_get(VK_CTX_INITIALIZED)){ return true }
   vk_state.init_ctx()
   if(_is_debug()){ print("Vulkan: Initializing renderer...") }
   vk_state.vk_set(VK_CTX_WINDOW_REF, win)
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
   if(_is_debug()){ print("Vulkan: init stage -> depth resources") }
   if(!_create_depth_resources()){ return false }
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
   if(_is_debug()){ print("Vulkan: init stage -> vertex buffer") }
   if(!_create_vertex_buffer()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> staging buffer") }
   if(!_create_staging_buffer()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> descriptor pool") }
   if(!_create_descriptor_pool()){ return false }
   if(_is_debug()){ print("Vulkan: init stage -> default texture") }
   if(!_create_default_texture()){ return false }
   vk_state.vk_set(VK_CTX_CURRENT_MVP, sys_malloc(64))
   _update_default_mvp(win)
   vk_state.vk_set(VK_CTX_INITIALIZED, true)
   if(_is_debug()){ print("Vulkan: Renderer initialized successfully") }
   true
}

fn shutdown(){
   "Shuts down the Vulkan renderer and releases all associated resources."
   if(vk_state.vk_get(VK_CTX_DEVICE)){ device_wait_idle(vk_state.vk_get(VK_CTX_DEVICE)) }
   if(vk_state.vk_get(VK_CTX_VERTEX_BUFFER)){ destroy_buffer(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_VERTEX_BUFFER), 0) }
   if(vk_state.vk_get(VK_CTX_DEPTH_IMAGE)){ destroy_image(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_DEPTH_IMAGE), 0) }
   if(vk_state.vk_get(VK_CTX_DEPTH_VIEW)){ destroy_image_view(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_DEPTH_VIEW), 0) }
   if(vk_state.vk_get(VK_CTX_VERTEX_MEMORY)){ free_memory(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_VERTEX_MEMORY), 0) }
   if(vk_state.vk_get(VK_CTX_STAGING_BUFFER)){ destroy_buffer(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_STAGING_BUFFER), 0) }
   if(vk_state.vk_get(VK_CTX_STAGING_MEMORY)){ free_memory(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_STAGING_MEMORY), 0) }
   if(vk_state.vk_get(VK_CTX_DEFAULT_SAMPLER)){ destroy_sampler(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_DEFAULT_SAMPLER), 0) }
   if(vk_state.vk_get(VK_CTX_DESCRIPTOR_POOL)){ destroy_descriptor_pool(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_DESCRIPTOR_POOL), 0) }

   mut i = 0
   while(i < len(vk_state.vk_get(VK_CTX_TEXTURES))){
      def tex = get(vk_state.vk_get(VK_CTX_TEXTURES), i)
      def view = dict_get(tex, "view", 0)
      def img = dict_get(tex, "image", 0)
      def mem = dict_get(tex, "memory", 0)
      if(view){ destroy_image_view(vk_state.vk_get(VK_CTX_DEVICE), view, 0) }
      if(img){ destroy_image(vk_state.vk_get(VK_CTX_DEVICE), img, 0) }
      if(mem){ free_memory(vk_state.vk_get(VK_CTX_DEVICE), mem, 0) }
      i += 1
   }
   vk_state.vk_set(VK_CTX_TEXTURES, [])

   _destroy_swapchain_objects()
   if(vk_state.vk_get(VK_CTX_DEVICE)){ destroy_device(vk_state.vk_get(VK_CTX_DEVICE), 0) }
   if(vk_state.vk_get(VK_CTX_SURFACE)){ destroy_surface_khr(vk_state.vk_get(VK_CTX_INSTANCE), vk_state.vk_get(VK_CTX_SURFACE), 0) }
   if(vk_state.vk_get(VK_CTX_INSTANCE)){ destroy_instance(vk_state.vk_get(VK_CTX_INSTANCE), 0) }
}

fn _create_instance(){
   "Creates the Vulkan instance."
   ;; Create all structures with system malloc to avoid any Nytrix metadata issues
   mut app_info = sys_malloc(48)
   memset(app_info, 0, 48)
   store32(app_info, VK_STRUCTURE_TYPE_APPLICATION_INFO, 0)
   store32(app_info, 1, 24)
   store32(app_info, 1, 40)
   store32(app_info, 0x00401000, 44)

   def exts_list = ui_glfw.get_required_instance_extensions()
   def ext_count = get(exts_list, 0)
   def ext_ptrs = get(exts_list, 1)

   ;; Create VkInstanceCreateInfo manually with explicit zeroing
   mut create_info = sys_malloc(64)
   memset(create_info, 0, 64)
   store32(create_info, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0)
   store64_raw(create_info, app_info, 24)
   store32(create_info, ext_count, 48)         ;; extensions
   store64_raw(create_info, ext_ptrs, 56)
   mut inst_ptr = sys_malloc(8)
   store32(inst_ptr, 0, 0) store32(inst_ptr, 0, 4)
   if(_is_debug()){ print(f"Vulkan: Creating instance with wrapper...") }
   def res = create_instance(create_info, 0, inst_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Instance creation failed with code {res}") }
      return false
   }
   if(_is_debug()){
      _dbg_handle("instance.out.raw", load64(inst_ptr, 0))
   }
   vk_state.vk_set(VK_CTX_INSTANCE, load64(inst_ptr, 0))
   if(_is_debug()){
      print("Vulkan: Instance created OK.")
      _dbg_handle("instance", vk_state.vk_get(VK_CTX_INSTANCE))
   }
   true
}

fn _create_surface(win){
   "Creates the native window surface (WSI)."
   def window = get(win, 22, 0)
   if(!window){
      if(_is_debug()){ print("Vulkan: No window handle") }
      return false
   }
   mut surf_ptr = sys_malloc(8)
   store32(surf_ptr, 0, 0) store32(surf_ptr, 0, 4)
   def res = ui_glfw.create_vulkan_surface(vk_state.vk_get(VK_CTX_INSTANCE), window, 0, surf_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Surface creation failed with code {res}") }
      return false
   }
   if(_is_debug()){
      _dbg_handle("surface.out.raw", load64(surf_ptr, 0))
   }
   vk_state.vk_set(VK_CTX_SURFACE, load64(surf_ptr, 0))
   if(_is_debug()){
      print(f"Vulkan: Surface created OK")
      _dbg_handle("surface", vk_state.vk_get(VK_CTX_SURFACE))
   }
   true
}

fn _pick_physical_device(){
   "Selects a suitable physical GPU for rendering."
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   def res1 = enumerate_physical_devices(vk_state.vk_get(VK_CTX_INSTANCE), count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(_is_debug()){ print(f"Vulkan: Physical devices found: {count}") }
   if(count == 0){ return false }
   mut devices_ptr = sys_malloc(count * 8)
   enumerate_physical_devices(vk_state.vk_get(VK_CTX_INSTANCE), count_ptr, devices_ptr)
   ;; Just pick the first device
   if(_is_debug()){
      _dbg_handle("physical.out.raw", load64(devices_ptr, 0))
   }
   vk_state.vk_set(VK_CTX_PHYSICAL_DEVICE, load64(devices_ptr, 0))
   if(_is_debug()){
      print(f"Vulkan: Selected physical device")
      _dbg_handle("physical", vk_state.vk_get(VK_CTX_PHYSICAL_DEVICE))
   }
   true
}

fn _create_logical_device(){
   "Creates the logical Vulkan device and retrieves queues."
   mut count_ptr = sys_malloc(4)
   store32(count_ptr, 0, 0)
   get_physical_device_queue_family_properties(vk_state.vk_get(VK_CTX_PHYSICAL_DEVICE), count_ptr, 0)
   def count = load32(count_ptr, 0)
   if(count == 0){ return false }
   def prop_stride = 24
   mut props = sys_malloc(count * prop_stride)
   get_physical_device_queue_family_properties(vk_state.vk_get(VK_CTX_PHYSICAL_DEVICE), count_ptr, props)
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
   vk_state.vk_set(VK_CTX_GRAPHICS_FAMILY_INDEX, graphics_family)
   ;; Queue priority (1.0f in IEEE-754)
   mut priorities = sys_malloc(4)
   store32(priorities, 0x3f800000, 0)
   mut queue_create_info = sys_malloc(40)
   store32(queue_create_info, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0)
   store32(queue_create_info, 0, 8) store32(queue_create_info, 0, 12)   ;; pNext
   store32(queue_create_info, 0, 16)          ;; flags
   store32(queue_create_info, graphics_family, 20) ;; queueFamilyIndex
   store32(queue_create_info, 1, 24)               ;; queueCount
   store64_raw(queue_create_info, priorities, 32) ;; pQueuePriorities
   mut ext1 = sys_malloc(32)
   memset(ext1, 0, 32)
   _strcpy(ext1, "VK_KHR_swapchain")
   mut ext_ptrs = sys_malloc(8)
   store64_raw(ext_ptrs, ext1, 0)
   mut create_info = sys_malloc(72)
   store32(create_info, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0)
   store32(create_info, 0, 8) store32(create_info, 0, 12)                  ;; pNext
   store32(create_info, 0, 16)                         ;; flags
   store32(create_info, 1, 20)                         ;; queueCreateInfoCount
   store64_raw(create_info, queue_create_info, 24) ;; pQueueCreateInfos
   store32(create_info, 0, 32)                         ;; enabledLayerCount
   store32(create_info, 0, 40) store32(create_info, 0, 44)                 ;; ppEnabledLayerNames
   store32(create_info, 1, 48)                         ;; enabledExtensionCount
   store64_raw(create_info, ext_ptrs, 56)          ;; ppEnabledExtensionNames
   store32(create_info, 0, 64) store32(create_info, 0, 68)                 ;; pEnabledFeatures
   mut dev_ptr = sys_malloc(8)
   store32(dev_ptr, 0, 0) store32(dev_ptr, 0, 4)
   if(_is_debug()){
      _dbg_handle("device.out.pre", load64(dev_ptr, 0))
   }
   def res = create_device(vk_state.vk_get(VK_CTX_PHYSICAL_DEVICE), create_info, 0, dev_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Logical device creation failed with code {res}") }
      return false
   }
   if(_is_debug()){
      _dbg_handle("device.out.post", load64(dev_ptr, 0))
   }
   vk_state.vk_set(VK_CTX_DEVICE, load64(dev_ptr, 0))
   if(_is_debug()){
      print(f"Vulkan: Logical device created OK")
      _dbg_handle("device", vk_state.vk_get(VK_CTX_DEVICE))
   }
   mut q_ptr = sys_malloc(8)
   store32(q_ptr, 0, 0) store32(q_ptr, 0, 4)
   get_device_queue(vk_state.vk_get(VK_CTX_DEVICE), graphics_family, 0, q_ptr)
   if(_is_debug()){
      _dbg_handle("queue.out.post", load64(q_ptr, 0))
   }
   vk_state.vk_set(VK_CTX_GRAPHICS_QUEUE, load64(q_ptr, 0))
   if(_is_debug()){
      _dbg_handle("queue", vk_state.vk_get(VK_CTX_GRAPHICS_QUEUE))
   }
   ;; Use same queue for presenting for now (most GPUs support this)
   vk_state.vk_set(VK_CTX_PRESENT_QUEUE, vk_state.vk_get(VK_CTX_GRAPHICS_QUEUE))
   true
}

fn _choose_composite_alpha(flags){
   "Heuristic to choose supported composite alpha mode for swapchain."
   if((flags & 0x1) != 0){ return 0x1 } ;; VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
   if((flags & 0x2) != 0){ return 0x2 } ;; PRE_MULTIPLIED
   if((flags & 0x4) != 0){ return 0x4 } ;; POST_MULTIPLIED
   if((flags & 0x8) != 0){ return 0x8 } ;; INHERIT
   0x1
}

fn _create_swapchain(win){
   "Initializes the Vulkan swapchain for the given window."
   mut caps = sys_malloc(128) ;; VkSurfaceCapabilitiesKHR
   memset(caps, 0, 128)
   get_physical_device_surface_capabilities_khr(vk_state.vk_get(VK_CTX_PHYSICAL_DEVICE), vk_state.vk_get(VK_CTX_SURFACE), caps)
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
   vk_state.vk_set(VK_CTX_SWAPCHAIN_EXTENT_W, w)
   vk_state.vk_set(VK_CTX_SWAPCHAIN_EXTENT_H, h)
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
   store32(create_info, 0, 8) store32(create_info, 0, 12)   ;; pNext
   store32(create_info, 0, 16)         ;; flags
   store64_raw(create_info, to_int(vk_state.vk_get(VK_CTX_SURFACE)), 24)
   store32(create_info, count, 32)
   store32(create_info, VK_FORMAT_B8G8R8A8_UNORM, 36) ;; format
   store32(create_info, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 40) ;; colorSpace
   store32(create_info, w, 44) ;; extent.width
   store32(create_info, h, 48) ;; extent.height
   store32(create_info, 1, 52) ;; imageArrayLayers
   store32(create_info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 56)
   store32(create_info, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(create_info, 0, 64) ;; queueCount
   store32(create_info, 0, 72) store32(create_info, 0, 76) ;; pQueueFamilyIndices
   store32(create_info, pre_transform, 80) ;; preTransform
   store32(create_info, composite_alpha, 84) ;; compositeAlpha
   store32(create_info, VK_PRESENT_MODE_FIFO_KHR, 88)
   store32(create_info, 1, 92) ;; clipped
   store32(create_info, 0, 96) store32(create_info, 0, 100) ;; oldSwapchain
   mut sc_ptr = sys_malloc(8)
   store32(sc_ptr, 0, 0) store32(sc_ptr, 0, 4)
   if(_is_debug()){
      _dbg_handle("swapchain.out.pre", load64(sc_ptr, 0))
   }
   def res = create_swapchain_khr(vk_state.vk_get(VK_CTX_DEVICE), create_info, 0, sc_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Swapchain creation failed with code {res}") }
      return false
   }
   if(_is_debug()){
      _dbg_handle("swapchain.out.post", load64(sc_ptr, 0))
   }
   vk_state.vk_set(VK_CTX_SWAPCHAIN, load64(sc_ptr, 0))
   if(_is_debug()){ _dbg_handle("swapchain", vk_state.vk_get(VK_CTX_SWAPCHAIN)) }
   vk_state.vk_set(VK_CTX_SWAPCHAIN_FORMAT, VK_FORMAT_B8G8R8A8_UNORM)
   ;; Get images
   mut img_count_ptr = sys_malloc(4)
   store32(img_count_ptr, 0, 0)
   def gi1 = get_swapchain_images_khr(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_SWAPCHAIN), img_count_ptr, 0)
   if(_is_debug()){ print(f"Vulkan: vkGetSwapchainImagesKHR(count) res={gi1} count={load32(img_count_ptr, 0)}") }
   vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGE_COUNT, load32(img_count_ptr, 0))
   mut imgs_ptr = sys_malloc(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_COUNT) * 8)
   def gi2 = get_swapchain_images_khr(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_SWAPCHAIN), img_count_ptr, imgs_ptr)
   if(_is_debug()){ print(f"Vulkan: vkGetSwapchainImagesKHR(images) res={gi2}") }
   vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGES, [])
   mut i = 0
   while(i < vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_COUNT)){
      def img = load64(imgs_ptr, i * 8)
      if(_is_debug()){ _dbg_handle(f"swapchain.image[{i}]", img) }
      vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGES, append(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGES), img))
      i += 1
   }
   if(_is_debug()){ print(f"Vulkan: Swapchain created with {vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_COUNT)} images") }
   true
}

fn _destroy_swapchain_objects(){
   "Releases swapchain-dependent resources (framebuffers, views, etc)."
   if(!vk_state.vk_get(VK_CTX_DEVICE)){ return 0 }
   mut i = 0
   while(i < len(vk_state.vk_get(VK_CTX_FRAMEBUFFERS))){
      def fb = get(vk_state.vk_get(VK_CTX_FRAMEBUFFERS), i, 0)
      if(fb){ destroy_framebuffer(vk_state.vk_get(VK_CTX_DEVICE), fb, 0) }
      i += 1
   }
   vk_state.vk_set(VK_CTX_FRAMEBUFFERS, [])
   i = 0
   while(i < len(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_VIEWS))){
      def iv = get(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_VIEWS), i, 0)
      if(iv){ destroy_image_view(vk_state.vk_get(VK_CTX_DEVICE), iv, 0) }
      i += 1
   }
   vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGE_VIEWS, [])
   if(vk_state.vk_get(VK_CTX_SWAPCHAIN)){
      destroy_swapchain_khr(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_SWAPCHAIN), 0)
      vk_state.vk_set(VK_CTX_SWAPCHAIN, 0)
   }
   vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGES, [])
   vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGE_COUNT, 0)
   0
}

fn _recreate_swapchain(){
   "Rebuilds the swapchain after window resize."
   if(!vk_state.vk_get(VK_CTX_WINDOW_REF) || !vk_state.vk_get(VK_CTX_DEVICE)){ return false }
   if(_is_debug()){ print("Vulkan: Recreating swapchain for window resize/out-of-date") }
   def device = vk_state.vk_get(VK_CTX_DEVICE)
   device_wait_idle(device)
   _destroy_swapchain_objects()

   ;; Fix: Clean up old depth resources
   def di = vk_state.vk_get(VK_CTX_DEPTH_IMAGE)
   if(di){ destroy_image(device, di, 0) vk_state.vk_set(VK_CTX_DEPTH_IMAGE, 0) }
   def dv = vk_state.vk_get(VK_CTX_DEPTH_VIEW)
   if(dv){ destroy_image_view(device, dv, 0) vk_state.vk_set(VK_CTX_DEPTH_VIEW, 0) }
   def dm = vk_state.vk_get(VK_CTX_DEPTH_MEMORY)
   if(dm){ free_memory(device, dm, 0) vk_state.vk_set(VK_CTX_DEPTH_MEMORY, 0) }

   if(!_create_swapchain(vk_state.vk_get(VK_CTX_WINDOW_REF))){ return false }
   if(!_create_image_views()){ return false }

   ;; Fix: Rebuild depth resources
   if(!_create_depth_resources()){ return false }

   if(!_create_framebuffers()){ return false }
   true
}

fn _create_image_views(){
   "Initializes Vulkan image views for each swapchain image."
   vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGE_VIEWS, [])
   mut i = 0
   while(i < vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_COUNT)){
      def image_handle = get(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGES), i)
      if(_is_debug()){ _dbg_handle(f"image_view.image[{i}]", image_handle) }
      mut create_info = sys_malloc(80)
      memset(create_info, 0, 80)
      store32(create_info, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
      store32(create_info, 0, 8) store32(create_info, 0, 12) ;; pNext
      store32(create_info, 0, 16) ;; flags
      store64_raw(create_info, image_handle, 24)
      store32(create_info, 1, 32) ;; viewType (2D = 1)
      store32(create_info, vk_state.vk_get(VK_CTX_SWAPCHAIN_FORMAT), 36)
      ;; components (all identity=0)
      ;; subresourceRange
      store32(create_info, VK_IMAGE_ASPECT_COLOR_BIT, 56)
      store32(create_info, 0, 60) ;; baseMipLevel
      store32(create_info, 1, 64) ;; levelCount
      store32(create_info, 0, 68) ;; baseArrayLayer
      store32(create_info, 1, 72) ;; layerCount
      mut view_ptr = sys_malloc(8)
      def iv_res = create_image_view(vk_state.vk_get(VK_CTX_DEVICE), create_info, 0, view_ptr)
      if(iv_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateImageView failed at image[{i}] with code {iv_res}") }
         return false
      }
      def view_h = load64(view_ptr, 0)
      if(_is_debug()){ _dbg_handle(f"image_view.out[{i}]", view_h) }
      vk_state.vk_set(VK_CTX_SWAPCHAIN_IMAGE_VIEWS, append(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_VIEWS), view_h))
      i += 1
   }
   true
}

fn _create_depth_resources(){
   "Allocates and initializes the depth buffer for 3D/ordered rendering."
   ;; Format 126 = VK_FORMAT_D32_SFLOAT, 129 = D24_UNORM_S8_UINT
   ;; We will try D32_SFLOAT first
   def depth_format = 126

   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16) ;; flags
   store32(img_ci, 1, 20) ;; imageType 2D
   store32(img_ci, depth_format, 24)
   store32(img_ci, vk_state.vk_get(VK_CTX_SWAPCHAIN_EXTENT_W), 28)
   store32(img_ci, vk_state.vk_get(VK_CTX_SWAPCHAIN_EXTENT_H), 32)
   store32(img_ci, 1, 36) ;; extent.depth
   store32(img_ci, 1, 40) ;; mipLevels
   store32(img_ci, 1, 44) ;; arrayLayers
   store32(img_ci, 1, 48) ;; samples
   store32(img_ci, 0, 52) ;; tiling OPTIMAL
   store32(img_ci, 32, 56) ;; usage DEPTH_STENCIL_ATTACHMENT
   store32(img_ci, 0, 60) ;; sharing exclusive
   store32(img_ci, 0, 64) ;; queueCount
   store32(img_ci, 0, 80) ;; initialLayout undefined

   mut img_ptr = sys_malloc(8)
   if(create_image(vk_state.vk_get(VK_CTX_DEVICE), img_ci, 0, img_ptr) != 0){ return false }
   vk_state.vk_set(VK_CTX_DEPTH_IMAGE, load64(img_ptr, 0))

   mut mem_req = sys_malloc(24)
   get_image_memory_requirements(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_DEPTH_IMAGE), mem_req)
   def size = load64(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   def mem_type = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_type, 24)
   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(vk_state.vk_get(VK_CTX_DEVICE), alloc_info, 0, mem_ptr) != 0){ return false }
   vk_state.vk_set(VK_CTX_DEPTH_MEMORY, load64(mem_ptr, 0))
   bind_image_memory(vk_state.vk_get(VK_CTX_DEVICE), vk_state.vk_get(VK_CTX_DEPTH_IMAGE), vk_state.vk_get(VK_CTX_DEPTH_MEMORY), 0)

   mut view_ci = sys_malloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_raw(view_ci, to_int(vk_state.vk_get(VK_CTX_DEPTH_IMAGE)), 24)
   store32(view_ci, 1, 32)
   store32(view_ci, depth_format, 36)
   store32(view_ci, 0x00000002, 56) ;; ASPECT_DEPTH
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)
   mut view_ptr = sys_malloc(8)
   if(create_image_view(vk_state.vk_get(VK_CTX_DEVICE), view_ci, 0, view_ptr) != 0){ return false }
   vk_state.vk_set(VK_CTX_DEPTH_VIEW, load64(view_ptr, 0))
   true
}

fn _create_render_pass(){
   "Defines the Vulkan render pass (color + depth attachments)."
   ;; attachments: 0=color, 1=depth
   mut atts = sys_malloc(72)
   ;; Color Attachment
   store32(atts, 0, 0) ;; flags
   store32(atts, vk_state.vk_get(VK_CTX_SWAPCHAIN_FORMAT), 4) ;; format
   store32(atts, 1, 8) ;; samples (VK_SAMPLE_COUNT_1_BIT)
   store32(atts, 1, 12) ;; loadOp (VK_ATTACHMENT_LOAD_OP_CLEAR = 1)
   store32(atts, 0, 16) ;; storeOp (VK_ATTACHMENT_STORE_OP_STORE = 0)
   store32(atts, 2, 20) ;; stencilLoadOp (DONT_CARE = 2)
   store32(atts, 1, 24) ;; stencilStoreOp (DONT_CARE = 1)
   store32(atts, 0, 28) ;; initialLayout (UNDEFINED = 0)
   store32(atts, 1000001002, 32) ;; finalLayout (PRESENT_SRC_KHR = 1000001002)

   ;; Depth Attachment (offset 36)
   store32(atts, 0, 36+0) ;; flags
   store32(atts, 126, 36+4) ;; format (D32_SFLOAT = 126)
   store32(atts, 1, 36+8) ;; samples 1
   store32(atts, 1, 36+12) ;; loadOp CLEAR = 1
   store32(atts, 1, 36+16) ;; storeOp DONT_CARE = 1
   store32(atts, 2, 36+20) ;; stencilLoad DONT_CARE = 2
   store32(atts, 1, 36+24) ;; stencilStore DONT_CARE = 1
   store32(atts, 0, 36+28) ;; initialLayout UNDEFINED = 0
   store32(atts, 254, 36+32) ;; finalLayout DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3 -> wait, DEPTH_STENCIL_ATTACHMENT_OPTIMAL is enum 3 in Vulkan? Wait. Let me check the enum. Wait, Wikipedia or Vulkan spec: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3. Let's keep it 3.
   ;; Actually wait, `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` is 3.
   store32(atts, 3, 36+32)

   ;; Refs
   mut car = sys_malloc(8)
   store32(car, 0, 0)
   store32(car, 2, 4) ;; layout COLOR_ATTACHMENT

   mut dar = sys_malloc(8)
   store32(dar, 1, 0)
   store32(dar, 3, 4) ;; layout DEPTH_STENCIL_ATTACHMENT

   ;; Subpass
   mut sd = sys_malloc(72)
   memset(sd, 0, 72)
   store32(sd, 0, 4) ;; BIND_POINT_GRAPHICS
   store32(sd, 1, 24) ;; colorCount
   store64_raw(sd, car, 32)
   store64_raw(sd, dar, 48) ;; pDepthStencilAttachment

   ;; Dependency
   mut dep = sys_malloc(28)
   store32(dep, -1, 0) ;; srcSubpass EXTERNAL
   store32(dep, 0, 4) ;; dstSubpass
   store32(dep, 0x00000400, 8) ;; srcStageMask COLOR_ATTACHMENT_OUTPUT
   store32(dep, 0x00000400, 12) ;; dstStageMask COLOR_ATTACHMENT_OUTPUT
   store32(dep, 0, 16) ;; srcAccessMask
   store32(dep, 0x00000100 | 0x00000010, 20) ;; dstAccessMask COLOR_WRITE | DEPTH_WRITE
   store32(dep, 0, 24) ;; dependencyFlags

   mut create_info = sys_malloc(64)
   memset(create_info, 0, 64)
   store32(create_info, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0)
   store32(create_info, 2, 20) ;; attachmentCount
   store64_raw(create_info, atts, 24)
   store32(create_info, 1, 32) ;; subpassCount
   store64_raw(create_info, sd, 40)
   store32(create_info, 1, 48) ;; depCount
   store64_raw(create_info, dep, 56)

   mut pass_ptr = sys_malloc(8)
   def rp_res = create_render_pass(vk_state.vk_get(VK_CTX_DEVICE), create_info, 0, pass_ptr)
   if(rp_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreateRenderPass failed with code {rp_res}") }
      return false
   }
   vk_state.vk_set(VK_CTX_RENDER_PASS, load64(pass_ptr, 0))
   if(_is_debug()){ _dbg_handle("render_pass", vk_state.vk_get(VK_CTX_RENDER_PASS)) }
   true
}

fn _create_framebuffers(){
   "Creates Vulkan framebuffers for each swapchain image."
   vk_state.vk_set(VK_CTX_FRAMEBUFFERS, [])
   mut i = 0
   while(i < vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_COUNT)){
      mut attach_ptr = sys_malloc(16)
      store64_raw(attach_ptr, get(vk_state.vk_get(VK_CTX_SWAPCHAIN_IMAGE_VIEWS, i)), 0)
      store64_raw(attach_ptr, to_int(vk_state.vk_get(VK_CTX_DEPTH_VIEW)), 8)
      mut create_info = sys_malloc(64)
      memset(create_info, 0, 64)
      store32(create_info, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0)
      store64_raw(create_info, to_int(vk_state.vk_get(VK_CTX_RENDER_PASS)), 24)
      store32(create_info, 2, 32) ;; attachmentCount
      store64_raw(create_info, attach_ptr, 40)
      store32(create_info, vk_state.vk_get(VK_CTX_SWAPCHAIN_EXTENT_W), 48)
      store32(create_info, vk_state.vk_get(VK_CTX_SWAPCHAIN_EXTENT_H), 52)
      store32(create_info, 1, 56) ;; layers
      mut fb_ptr = sys_malloc(8)
      def fb_res = create_framebuffer(vk_state.vk_get(VK_CTX_DEVICE), create_info, 0, fb_ptr)
      if(fb_res != 0){ return false }
      vk_state.vk_set(VK_CTX_FRAMEBUFFERS, append(vk_state.vk_get(VK_CTX_FRAMEBUFFERS), load64(fb_ptr, 0)))
      i += 1
   }
   true
}

fn _create_sync_objects(){
   "Initializes semaphores and fences for frame synchronization."
   vk_state.vk_set(VK_CTX_IMAGE_AVAILABLE_SEMAPHORES, [])
   vk_state.vk_set(VK_CTX_RENDER_FINISHED_SEMAPHORES, [])
   vk_state.vk_set(VK_CTX_IN_FLIGHT_FENCES, [])
   mut i = 0
   while(i < vk_state.vk_get(VK_CTX_MAX_FRAMES_IN_FLIGHT)){
      mut si = sys_malloc(24)
      memset(si, 0, 24)
      store32(si, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0)
      store32(si, 0, 8) store32(si, 0, 12) ;; pNext
      store32(si, 0, 16) ;; flags
      mut sem1 = sys_malloc(8)
      def s1_res = create_semaphore(vk_state.vk_get(VK_CTX_DEVICE), si, 0, sem1)
      if(s1_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateSemaphore(imageAvailable) failed at {i} with code {s1_res}") }
         return false
      }
      vk_state.vk_set(VK_CTX_IMAGE_AVAILABLE_SEMAPHORES, append(vk_state.vk_get(VK_CTX_IMAGE_AVAILABLE_SEMAPHORES), load64(sem1, 0)))
      mut sem2 = sys_malloc(8)
      def s2_res = create_semaphore(vk_state.vk_get(VK_CTX_DEVICE), si, 0, sem2)
      if(s2_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateSemaphore(renderFinished) failed at {i} with code {s2_res}") }
         return false
      }
      vk_state.vk_set(VK_CTX_RENDER_FINISHED_SEMAPHORES, append(vk_state.vk_get(VK_CTX_RENDER_FINISHED_SEMAPHORES), load64(sem2, 0)))
      mut fi = sys_malloc(24)
      memset(fi, 0, 24)
      store32(fi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0)
      store32(fi, 0, 8) store32(fi, 0, 12) ;; pNext
      store32(fi, 1, 16) ;; flags (1 = SIGNAL_BIT)
      mut fence = sys_malloc(8)
      def f_res = create_fence(vk_state.vk_get(VK_CTX_DEVICE), fi, 0, fence)
      if(f_res != 0){
         if(_is_debug()){ print(f"Vulkan: vkCreateFence failed at {i} with code {f_res}") }
         return false
      }
      vk_state.vk_set(VK_CTX_IN_FLIGHT_FENCES, append(vk_state.vk_get(VK_CTX_IN_FLIGHT_FENCES), load64(fence, 0)))
      i += 1
   }
   true
}
