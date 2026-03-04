;; Keywords: ui gfx vulkan renderer texture

module std.ui.gfx.vk.texture (
   create_texture, update_texture_rect, bind_texture, bind_default_texture, texture_size, texture_format, texture_descriptor, destroy_texture, read_framebuffer,
   blit_buffer,
   create_texture_ex, _create_default_texture, _texture_meta
)

use std.core *
use std.core.mem *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.vulkan *
use std.ui.gfx.vk.buffers (_find_memory_type)
use std.util.common as common

fn _ensure_upload_cb(){
   if(_upload_cb != 0){
      reset_command_buffer(_upload_cb, 0)
      return _upload_cb
   }
   memset(_upload_alloc, 0, 32)
   store32(_upload_alloc, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_h(_upload_alloc, _command_pool, 16)
   store32(_upload_alloc, 0, 24)
   store32(_upload_alloc, 1, 28)
   if(allocate_command_buffers(_device, _upload_alloc, _upload_cb_ptr) != 0){ return 0 }
   _upload_cb = load64(_upload_cb_ptr, 0)
   _upload_cb
}

fn _upload_image_region(image, x, y, w, h, old_layout){
   if(!_upload_cb_ptr || !_upload_cb_arr || !_upload_bi || !_upload_bar1 || !_upload_bar2 || !_upload_region || !_upload_si){ return false }
   def cb = _ensure_upload_cb()
   if(!cb){ return false }

   memset(_upload_bi, 0, 32)
   store32(_upload_bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(_upload_bi, 1, 16)
   begin_command_buffer(cb, _upload_bi)

   mut src_access = 0
   mut src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
   if(old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){
      src_access = VK_ACCESS_SHADER_READ_BIT
      src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
   }

   memset(_upload_bar1, 0, 72)
   store32(_upload_bar1, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
   store32(_upload_bar1, src_access, 16)
   store32(_upload_bar1, VK_ACCESS_TRANSFER_WRITE_BIT, 20)
   store32(_upload_bar1, old_layout, 24)
   store32(_upload_bar1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 28)
   store32(_upload_bar1, -1, 32)
   store32(_upload_bar1, -1, 36)
   store64_h(_upload_bar1, image, 40)
   store32(_upload_bar1, VK_IMAGE_ASPECT_COLOR_BIT, 48)
   store32(_upload_bar1, 0, 52) store32(_upload_bar1, 1, 56)
   store32(_upload_bar1, 0, 60) store32(_upload_bar1, 1, 64)
   cmd_pipeline_barrier(cb, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, _upload_bar1)

   memset(_upload_region, 0, 56)
   store32(_upload_region, VK_IMAGE_ASPECT_COLOR_BIT, 16)
   store32(_upload_region, 0, 20)
   store32(_upload_region, 0, 24)
   store32(_upload_region, 1, 28)
   store32(_upload_region, x, 32)
   store32(_upload_region, y, 36)
   store32(_upload_region, 0, 40)
   store32(_upload_region, w, 44)
   store32(_upload_region, h, 48)
   store32(_upload_region, 1, 52)
   cmd_copy_buffer_to_image(cb, _staging_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, _upload_region)

   memset(_upload_bar2, 0, 72)
   store32(_upload_bar2, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
   store32(_upload_bar2, VK_ACCESS_TRANSFER_WRITE_BIT, 16)
   store32(_upload_bar2, VK_ACCESS_SHADER_READ_BIT, 20)
   store32(_upload_bar2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 24)
   store32(_upload_bar2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 28)
   store32(_upload_bar2, -1, 32)
   store32(_upload_bar2, -1, 36)
   store64_h(_upload_bar2, image, 40)
   store32(_upload_bar2, VK_IMAGE_ASPECT_COLOR_BIT, 48)
   store32(_upload_bar2, 0, 52) store32(_upload_bar2, 1, 56)
   store32(_upload_bar2, 0, 60) store32(_upload_bar2, 1, 64)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, _upload_bar2)

   end_command_buffer(cb)

   memset(_upload_si, 0, 72)
   store32(_upload_si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(_upload_si, 1, 40)
   store64_h(_upload_cb_arr, cb, 0)
   store64_h(_upload_si, _upload_cb_arr, 48)
   reset_fences(_device, 1, _upload_fence_ptr)
   queue_submit(_graphics_queue, 1, _upload_si, _upload_fence)
   wait_for_fences(_device, 1, _upload_fence_ptr, 1, 0xFFFFFFFFFFFFFFFF)
   true
}
fn create_texture_ex(width, height, pixels, format=37){
   "Creates a GPU texture. Format 37=RGBA8, 9=R8."
   mut bpp = 4
   if(format == 9){ bpp = 1 }

   ; 1. Create Image
   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16) ; flags
   store32(img_ci, 1, 20) ; imageType = 2D
   store32(img_ci, format, 24)
   store32(img_ci, width, 28)
   store32(img_ci, height, 32)
   store32(img_ci, 1, 36) ; depth
   store32(img_ci, 1, 40) ; mipLevels
   store32(img_ci, 1, 44) ; arrayLayers
   store32(img_ci, 1, 48) ; samples
   store32(img_ci, 0, 52) ; tiling = OPTIMAL
   store32(img_ci, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 56)
   store32(img_ci, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(img_ci, 0, 80) ; initialLayout = UNDEFINED

   mut img_ptr = sys_malloc(8)
   def r1 = create_image(_device, img_ci, 0, img_ptr)
   if(r1 != 0){ return -1 }
   def image = load64(img_ptr, 0)

   ; 2. Allocate Memory
   mut mem_req = sys_malloc(24)
   get_image_memory_requirements(_device, image, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   common.touch(type_bits)
   def mem_idx = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, size, 16)
   store32(alloc_info, mem_idx, 24)

   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return -1 }
   def memory = load64(mem_ptr, 0)
   bind_image_memory(_device, image, memory, 0)

   ; 3. Create ImageView
   mut view_ci = sys_malloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_h(view_ci, image, 24)
   store32(view_ci, 1, 32) ; 2D
   store32(view_ci, format, 36)
   store32(view_ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
   store32(view_ci, 1, 64)
   store32(view_ci, 1, 72)

   mut view_ptr = sys_malloc(8)
   def r3 = create_image_view(_device, view_ci, 0, view_ptr)
   if(r3 != 0){ return -1 }
   def view = load64(view_ptr, 0)

   ; 4. Upload Initial Pixels
   def img_size = width * height * bpp
   if(pixels && _staging_map){
      if(img_size > _staging_capacity){
         if(_debug_gfx_enabled){ print(f"Vulkan: IMAGE TOO LARGE FOR STAGING: {img_size} > {_staging_capacity}") }
         return -1
      }
      memcpy(_staging_map, pixels, img_size)
      if(!_upload_image_region(image, 0, 0, width, height, VK_IMAGE_LAYOUT_UNDEFINED)){ return -1 }
   }

   ; 5. Descriptor Set (per-texture in non-bindless mode)
   mut ds = 0
   if(!_bindless_enabled){
      mut dsl_ptr = sys_malloc(8)
      store64_h(dsl_ptr, _descriptor_set_layout, 0)
      mut alloc_ds = sys_malloc(40)
      memset(alloc_ds, 0, 40)
      store32(alloc_ds, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0)
      store64_h(alloc_ds, _descriptor_pool, 16)
      store32(alloc_ds, 1, 24)
      store64_h(alloc_ds, dsl_ptr, 32)
      mut ds_ptr = sys_malloc(8)
      def dres = allocate_descriptor_sets(_device, alloc_ds, ds_ptr)
      if(dres == 0){ ds = load64(ds_ptr, 0) }

      if(ds){
         mut im_info = sys_malloc(24)
         store64_h(im_info, _default_sampler, 0)
         store64_h(im_info, view, 8)
         store32(im_info, 5, 16) ; SHADER_READ_ONLY_OPTIMAL

         mut write = sys_malloc(64)
         memset(write, 0, 64)
         store32(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
         store64_h(write, ds, 16)
         store32(write, 0, 24) ; binding
         store32(write, 0, 28) ; array element
         store32(write, 1, 32) ; count
         store32(write, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36)
         store64_h(write, im_info, 40)
         update_descriptor_sets(_device, 1, write, 0, 0)
         sys_free(im_info) sys_free(write)
      }

      sys_free(dsl_ptr) sys_free(alloc_ds) sys_free(ds_ptr)
   }

   mut tex = dict(8)
   tex = dict_set(tex, "image", image)
   tex = dict_set(tex, "view", view)
   tex = dict_set(tex, "memory", memory)
   tex = dict_set(tex, "ds", ds)
   tex = dict_set(tex, "width", width)
   tex = dict_set(tex, "height", height)
   tex = dict_set(tex, "format", format)
   tex = dict_set(tex, "bpp", bpp)

   mut tex_id = -1
   if(len(_free_texture_ids) > 0){
      tex_id = pop(_free_texture_ids)
      set_idx(_textures, tex_id, tex)
      set_idx(_texture_ds_cache, tex_id, ds)
      set_idx(_texture_fmt_cache, tex_id, format)
   } else {
      tex_id = len(_textures)
      _textures = append(_textures, tex)
      _texture_ds_cache = append(_texture_ds_cache, ds)
      _texture_fmt_cache = append(_texture_fmt_cache, format)
   }

   if(_bindless_enabled && _bindless_ds){
      mut im_info = sys_malloc(24)
      store64_h(im_info, _default_sampler, 0)
      store64_h(im_info, view, 8)
      store32(im_info, 5, 16)

      mut write = sys_malloc(64)
      memset(write, 0, 64)
      store32(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
      store64_h(write, _bindless_ds, 16)
      store32(write, 0, 24) ; binding
      store32(write, tex_id, 28) ; array element
      store32(write, 1, 32) ; count
      store32(write, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36)
      store64_h(write, im_info, 40)
      update_descriptor_sets(_device, 1, write, 0, 0)
      sys_free(im_info) sys_free(write)
   }

   sys_free(img_ci) sys_free(img_ptr) sys_free(mem_req) sys_free(alloc_info) sys_free(mem_ptr)
   sys_free(view_ci) sys_free(view_ptr)

   tex_id
}

fn update_texture_rect(tex_id, x, y, w, h, pixels){
   "Partially updates a texture's pixel data. Uses pre-allocated buffers."
   if(tex_id < 0 || tex_id >= len(_textures)){ return false }
   def tex_obj = get(_textures, tex_id)
   def image = dict_get(tex_obj, "image")
   def bpp = dict_get(tex_obj, "bpp", 4)
   def img_size = w * h * bpp
   if(img_size > _staging_capacity){ return false }

   memcpy(_staging_map, pixels, img_size)
   _upload_image_region(image, x, y, w, h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
}

fn create_texture(width, height, pixels){
   "Creates a GPU texture from raw pixel data (RGBA8)."
   create_texture_ex(width, height, pixels, 37)
}

fn _init_bindless_descriptor_set(default_view){
   "Initializes the bindless descriptor set with the default texture."
   if(!_bindless_enabled || _bindless_ds){ return true }
   mut dsl_ptr = sys_malloc(8)
   store64_h(dsl_ptr, _descriptor_set_layout, 0)
   mut alloc_ds = sys_malloc(40)
   memset(alloc_ds, 0, 40)
   store32(alloc_ds, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0)
   store64_h(alloc_ds, _descriptor_pool, 16)
   store32(alloc_ds, 1, 24)
   store64_h(alloc_ds, dsl_ptr, 32)
   mut ds_ptr = sys_malloc(8)
   if(allocate_descriptor_sets(_device, alloc_ds, ds_ptr) != 0){ return false }
   _bindless_ds = load64(ds_ptr, 0)

   mut infos = sys_malloc(24 * MAX_TEXTURES)
   mut i = 0
   while(i < MAX_TEXTURES){
      def off = infos + i * 24
      store64_h(off, _default_sampler, 0)
      store64_h(off, default_view, 8)
      store32(off, 5, 16)
      i += 1
   }

   mut write = sys_malloc(64)
   memset(write, 0, 64)
   store32(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
   store64_h(write, _bindless_ds, 16)
   store32(write, 0, 24) ; binding
   store32(write, 0, 28) ; array element
   store32(write, MAX_TEXTURES, 32) ; count
   store32(write, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36)
   store64_h(write, infos, 40)
   update_descriptor_sets(_device, 1, write, 0, 0)

   sys_free(infos) sys_free(write) sys_free(dsl_ptr) sys_free(alloc_ds) sys_free(ds_ptr)
   true
}

fn _create_default_texture(){
   "Creates the default 1x1 white texture for untextured drawing."
   ; Create sampler first
   mut sampler_ci = sys_malloc(80)
   memset(sampler_ci, 0, 80)
   store32(sampler_ci, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0)
   store32(sampler_ci, 0, 16) ; flags
   store32(sampler_ci, _cfg_filter, 20) ; magFilter
   store32(sampler_ci, _cfg_filter, 24) ; minFilter
   store32(sampler_ci, 0, 28) ; mipmapMode
   store32(sampler_ci, 2, 32) ; addressModeU = CLAMP_TO_EDGE
   store32(sampler_ci, 2, 36) ; addressModeV
   store32(sampler_ci, 2, 40) ; addressModeW

   mut sampler_ptr = sys_malloc(8)
   if(create_sampler(_device, sampler_ci, 0, sampler_ptr) != 0){ return false }
   _default_sampler = load64(sampler_ptr, 0)

   ; Create 1x1 white texture
   def pixels = sys_malloc(4)
   store32(pixels, 0xFFFFFFFF, 0)
   def tex_id = create_texture(1, 1, pixels)
   if(tex_id == -1){ return false }
   _default_texture = tex_id
   _current_texture_id = tex_id
   _current_tex_index = _bindless_enabled ? tex_id : 0
   if(_bindless_enabled){
      def tex_obj = get(_textures, tex_id, 0)
      def view = dict_get(tex_obj, "view", 0)
      if(!view || !_init_bindless_descriptor_set(view)){ return false }
   }
   true
}

mut _mvp_dirty = true
mut _model_dirty = true

fn bind_texture(tex_id){
   "Binds a texture by ID for subsequent drawing commands."
   if(!is_int(tex_id)){
      tex_id = _default_texture
   } elif(tex_id < 0 || tex_id >= MAX_TEXTURES){
      if(!_bindless_overflow_warned){
         ; print("Vulkan: Bindless tex_id " + to_str(tex_id) + " out of range")
         _bindless_overflow_warned = true
      }
      tex_id = _default_texture
   }
   if(tex_id == _current_texture_id){ return }
   if(!_bindless_enabled){ _flush() }
   _current_texture_id = tex_id
   _current_tex_index = _bindless_enabled ? tex_id : 0
   def fmt = texture_format(tex_id)
   def is_mask = (fmt == 9) ? 1 : 0
   if(is_mask != _last_is_mask){ _last_is_mask = is_mask _pc_dirty = true }
}

fn bind_default_texture(){
   "Binds the renderer's default 1x1 white texture."
   bind_texture(_default_texture)
}

fn texture_size(tex_id){
   "Returns [width, height] for a texture ID, or 0 if invalid."
   if(!is_int(tex_id)){
      tex_id = _default_texture
   } elif(tex_id < 0 || tex_id >= MAX_TEXTURES){
      if(!_bindless_overflow_warned){
         ; print("Vulkan: Bindless tex_id " + to_str(tex_id) + " out of range")
         _bindless_overflow_warned = true
      }
      tex_id = _default_texture
   }
   def tex = get(_textures, tex_id, 0)
   if(!tex || !is_dict(tex)){ return 0 }
   [dict_get(tex, "width", 0), dict_get(tex, "height", 0)]
}

fn _texture_meta(tex_id, key, fallback){
   "Internal: reads texture metadata field `key` from texture `tex_id`, or returns `fallback`."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_textures)){ return fallback }
   def tex_obj = get(_textures, tex_id, 0)
   if(!tex_obj || !is_dict(tex_obj)){ return fallback }
   dict_get(tex_obj, key, fallback)
}

fn texture_format(tex_id){
   "Returns the format of a texture."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_texture_fmt_cache)){ return 37 }
   get(_texture_fmt_cache, tex_id, 37)
}

fn texture_descriptor(tex_id){
   "Returns the descriptor set for a texture."
   if(_bindless_enabled){ return _bindless_ds }
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_texture_ds_cache)){ return 0 }
   get(_texture_ds_cache, tex_id, 0)
}

fn destroy_texture(tex_id){
   "Destroys a texture and frees its GPU resources."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= len(_textures)){ return }
   def tex = get(_textures, tex_id, 0)
   if(!tex || !is_dict(tex)){ return }
   if(_bindless_enabled && _bindless_ds && _default_texture >= 0){
      def def_tex = get(_textures, _default_texture, 0)
      def def_view = dict_get(def_tex, "view", 0)
      if(def_view){
         mut im_info = sys_malloc(24)
         store64_h(im_info, _default_sampler, 0)
         store64_h(im_info, def_view, 8)
         store32(im_info, 5, 16)

         mut write = sys_malloc(64)
         memset(write, 0, 64)
         store32(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
         store64_h(write, _bindless_ds, 16)
         store32(write, 0, 24)
         store32(write, tex_id, 28)
         store32(write, 1, 32)
         store32(write, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36)
         store64_h(write, im_info, 40)
         update_descriptor_sets(_device, 1, write, 0, 0)
         sys_free(im_info) sys_free(write)
      }
   }
   def img = dict_get(tex, "image", 0)
   def view = dict_get(tex, "view", 0)
   def mem = dict_get(tex, "memory", 0)
   if(view){ destroy_image_view(_device, view, 0) }
   if(img){ destroy_image(_device, img, 0) }
   if(mem){ free_memory(_device, mem, 0) }
   if(_current_texture_id == tex_id){
      _current_texture_id = -1
      _current_tex_index = 0
   }
   set_idx(_textures, tex_id, 0)
   set_idx(_texture_ds_cache, tex_id, 0)
   set_idx(_texture_fmt_cache, tex_id, 37)
   _free_texture_ids = append(_free_texture_ids, tex_id)
}

fn read_framebuffer(){
   "Reads the current swapchain image back to CPU memory. Returns {data, width, height, channels} or 0."
   if(!_device || !_swapchain || _image_index < 0){ return 0 }
   def w = _swapchain_extent_w
   def h = _swapchain_extent_h
   if(w <= 0 || h <= 0){ return 0 }
   def size = w * h * 4

   ; Create a host-visible buffer for readback
   mut buf_ci = sys_malloc(56)
   memset(buf_ci, 0, 56)
   store32(buf_ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_h(buf_ci, size, 24)
   store32(buf_ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 32)
   mut buf_ptr = sys_malloc(8)
   if(create_buffer(_device, buf_ci, 0, buf_ptr) != 0){ return 0 }
   def readback_buf = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, readback_buf, mem_req)
   def mem_idx = _find_memory_type(load32(mem_req, 16), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, load64_h(mem_req, 0), 16)
   store32(alloc_info, mem_idx, 24)
   mut mem_ptr = sys_malloc(8)
   allocate_memory(_device, alloc_info, 0, mem_ptr)
   def readback_mem = load64(mem_ptr, 0)
   bind_buffer_memory(_device, readback_buf, readback_mem, 0)

   ; Record copy commands
   mut ai = sys_malloc(32)
   memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_h(ai, _command_pool, 16)
   store32(ai, 1, 28)
   mut cb_p = sys_malloc(8)
   allocate_command_buffers(_device, ai, cb_p)
   def cb = load64(cb_p, 0)

   mut bi = sys_malloc(32)
   memset(bi, 0, 32)
   store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(bi, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 16)
   begin_command_buffer(cb, bi)

   def src_image = get(_swapchain_images, _image_index)
   mut barrier = sys_malloc(72)
   memset(barrier, 0, 72)
   store32(barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
   store32(barrier, 0, 16) ; srcAccess
   store32(barrier, VK_ACCESS_TRANSFER_READ_BIT, 20) ; dstAccess

   mut old_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
   if(!_surface){ old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }
   store32(barrier, old_layout, 24)
   store32(barrier, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 28)
   store64_h(barrier, src_image, 40)
   store32(barrier, VK_IMAGE_ASPECT_COLOR_BIT, 48)
   store32(barrier, 1, 56)
   store32(barrier, 1, 64)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, barrier)

   mut region = sys_malloc(56)
   memset(region, 0, 56)
   store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 16)
   store32(region, 0, 20)
   store32(region, 0, 24)
   store32(region, 1, 28)
   store32(region, w, 44)
   store32(region, h, 48)
   store32(region, 1, 52)
   cmd_copy_image_to_buffer(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf, 1, region)

   store32(barrier, VK_ACCESS_TRANSFER_READ_BIT, 16)
   store32(barrier, 0, 20)
   store32(barrier, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 24)
   store32(barrier, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 28)
   cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, barrier)
   end_command_buffer(cb)

   mut s_info = sys_malloc(72)
   memset(s_info, 0, 72)
   store32(s_info, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(s_info, 1, 40)
   store64_h(s_info, cb_p, 48)
   queue_submit(_graphics_queue, 1, s_info, 0)
   device_wait_idle(_device)

   mut map_ptr = sys_malloc(8)
   map_memory(_device, readback_mem, 0, size, 0, map_ptr)
   def mapped_data = load64(map_ptr, 0)

   ; Copy to Nytrix heap so we can free GPU resources
   def pixels = malloc(size)
   memcpy(pixels, mapped_data, size)

   unmap_memory(_device, readback_mem)
   destroy_buffer(_device, readback_buf, 0)
   free_memory(_device, readback_mem, 0)
   free_command_buffers(_device, _command_pool, 1, cb_p)

   ; Handle BGR swap for standard formats (44=BGRA8_UNORM, 50=BGRA8_SRGB, etc.)
   ; Standard BGRA formats: 44, 45, 46, 47, 48, 49, 50, 51, 52
   if(_swapchain_format >= 44 && _swapchain_format <= 52){
      mut b = 0
      while(b < size){
         def blue = load8(pixels, b)
         def red  = load8(pixels, b + 2)
         store8(pixels, red, b)
         store8(pixels, blue, b + 2)
         b += 4
      }
   }
   if(_debug_gfx_enabled){ print(f"Vulkan: Captured framebuffer format={_swapchain_format} size={w}x{h}") }

   mut res = dict(4)
   res = dict_set(res, "data",   pixels)
   res = dict_set(res, "width",  w)
   res = dict_set(res, "height", h)
   res = dict_set(res, "bpp",    4)
   res
}

fn blit_buffer(pixels, w, h){
   "Blits a raw RGBA8 pixel buffer to the full window."
   if(!_frame_open){ return }
   if(_blit_tex_id == -1 || texture_size(_blit_tex_id)[0] != w || texture_size(_blit_tex_id)[1] != h){
      if(_blit_tex_id != -1){ destroy_texture(_blit_tex_id) }
      _blit_tex_id = create_texture(w, h, pixels)
   } else {
      update_texture_rect(_blit_tex_id, 0, 0, w, h, pixels)
   }

   ; Draw full-screen quad unlit
   def last_unlit = _current_is_unlit
   set_unlit(true)
   def ws_w = float(_swapchain_extent_w)
   def ws_h = float(_swapchain_extent_h)

   ; Save MVP and set to identity for screen-space draw
   mut old_mvp = mat4_identity()
   mat4_from_buffer(old_mvp, _current_mvp)
   set_ortho(0.0, ws_w, 0.0, ws_h, -1.0, 1.0)

   draw_rect_tex(0.0, 0.0, ws_w, ws_h, _blit_tex_id, 1.0, 1.0, 1.0, 1.0)
   _flush()

   ; Restore state
   set_mvp(old_mvp)
   set_unlit(last_unlit != 0)
}
