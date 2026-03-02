;; Auto-generated split Vulkan renderer component
module std.ui.gfx.vk.texture (
  create_texture,
  update_texture_rect,
  _create_default_texture
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
use std.ui.gfx.vk.buffers *
use std.ui.gfx.vk.utils *

fn _create_default_texture(){
   "Creates the default 1x1 white texture for untextured drawing."
   ;; Create sampler first
   mut sampler_ci = sys_malloc(80)
   memset(sampler_ci, 0, 80)
   store32(sampler_ci, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0)
   store32(sampler_ci, 0, 16) ;; flags
   store32(sampler_ci, 1, 20) ;; magFilter = LINEAR
   store32(sampler_ci, 1, 24) ;; minFilter = LINEAR
   store32(sampler_ci, 0, 28) ;; mipmapMode = NEAREST
   store32(sampler_ci, 2, 32) ;; addressModeU = CLAMP_TO_EDGE
   store32(sampler_ci, 2, 36) ;; addressModeV
   store32(sampler_ci, 2, 40) ;; addressModeW

   mut sampler_ptr = sys_malloc(8)
   if(create_sampler(vk_get(VK_CTX_DEVICE), sampler_ci, 0, sampler_ptr) != 0){ return false }
   vk_set(VK_CTX_DEFAULT_SAMPLER, load64(sampler_ptr, 0))

   ;; Create 1x1 white texture
   def pixels = sys_malloc(4)
   store32(pixels, 0xFFFFFFFF, 0)
   def tex_id = create_texture(1, 1, pixels)
   if(tex_id == -1){ return false }
   vk_set(VK_CTX_DEFAULT_TEXTURE, tex_id)
   vk_set(VK_CTX_CURRENT_TEXTURE_ID, tex_id)
   true
}

fn create_texture(width, height, pixels){
   "Creates a GPU texture from raw pixel data."
   ;; 1. Create Image
   mut img_ci = sys_malloc(88)
   memset(img_ci, 0, 88)
   store32(img_ci, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0)
   store32(img_ci, 0, 16) ;; flags
   store32(img_ci, 1, 20) ;; imageType = 2D
   store32(img_ci, 37, 24) ;; format R8G8B8A8_UNORM
   store32(img_ci, width, 28)
   store32(img_ci, height, 32)
   store32(img_ci, 1, 36) ;; depth
   store32(img_ci, 1, 40) ;; mipLevels
   store32(img_ci, 1, 44) ;; arrayLayers
   store32(img_ci, 1, 48) ;; samples
   store32(img_ci, 0, 52) ;; tiling = OPTIMAL
   store32(img_ci, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 56)
   store32(img_ci, VK_SHARING_MODE_EXCLUSIVE, 60)
   store32(img_ci, 0, 64) ;; queueCount
   store32(img_ci, 0, 80) ;; initialLayout = UNDEFINED

   mut img_ptr = sys_malloc(8)
   if(create_image(vk_get(VK_CTX_DEVICE), img_ci, 0, img_ptr) != 0){ return 0 }
   def image = load64(img_ptr, 0)

   ;; 2. Allocate Memory
   mut mem_req = sys_malloc(24)
   get_image_memory_requirements(vk_get(VK_CTX_DEVICE), image, mem_req)
   def size = load64(mem_req, 0)
   def align = load64(mem_req, 8)
   def type_bits = load32(mem_req, 16)
   def mem_type = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_type, 24)

   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(vk_get(VK_CTX_DEVICE), alloc_info, 0, mem_ptr) != 0){ return 0 }
   def memory = load64(mem_ptr, 0)
   bind_image_memory(vk_get(VK_CTX_DEVICE), image, memory, 0)

   ;; 3. Create ImageView
   mut view_ci = sys_malloc(80)
   memset(view_ci, 0, 80)
   store32(view_ci, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0)
   store64_raw(view_ci, image, 24)
   store32(view_ci, 1, 32) ;; viewType 2D
   store32(view_ci, 37, 36) ;; format R8G8B8A8
   store32(view_ci, VK_IMAGE_ASPECT_COLOR_BIT, 56)
   store32(view_ci, 1, 64) ;; levelCount
   store32(view_ci, 1, 72) ;; layerCount

   mut view_ptr = sys_malloc(8)
   if(create_image_view(vk_get(VK_CTX_DEVICE), view_ci, 0, view_ptr) != 0){ return 0 }
   def view = load64(view_ptr, 0)

   ;; 4. Upload Pixels via staging + one-time command buffer
   def img_size = width * height * 4
   if(vk_get(VK_CTX_STAGING_MAP) && vk_get(VK_CTX_COMMAND_POOL) && pixels){
      ;; Copy pixels into staging buffer
      memcpy(vk_get(VK_CTX_STAGING_MAP), pixels, img_size)

      ;; Allocate a one-time command buffer
      mut ai = sys_malloc(32)
      memset(ai, 0, 32)
      store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
      store64_raw(ai, to_int(vk_get(VK_CTX_COMMAND_POOL)), 16)
      store32(ai, 0, 24) ;; PRIMARY
      store32(ai, 1, 28)
      mut cb_ptr = sys_malloc(8)
      if(allocate_command_buffers(vk_get(VK_CTX_DEVICE), ai, cb_ptr) == 0){
         def cb = load64(cb_ptr, 0)
         mut bi = sys_malloc(32)
         memset(bi, 0, 32)
         store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
         store32(bi, 1, 16) ;; ONE_TIME_SUBMIT
         begin_command_buffer(cb, bi)

         ;; VkImageMemoryBarrier size = 72 bytes
         ;; sType(4) pNext(8) srcAccessMask(4) dstAccessMask(4) oldLayout(4) newLayout(4)
         ;; srcQueueFamilyIndex(4) dstQueueFamilyIndex(4) image(8) subresourceRange(20)
         ;; Transition: UNDEFINED -> TRANSFER_DST_OPTIMAL
         mut bar1 = sys_malloc(72)
         memset(bar1, 0, 72)
         store32(bar1, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)  ;; VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45
         store32(bar1, 0, 16)  ;; srcAccessMask = 0
         store32(bar1, VK_ACCESS_TRANSFER_WRITE_BIT, 20) ;; dstAccessMask
         store32(bar1, VK_IMAGE_LAYOUT_UNDEFINED, 24)    ;; oldLayout
         store32(bar1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 28) ;; newLayout
         store32(bar1, 0xFFFFFFFF, 32) ;; srcQueueFamilyIndex = IGNORED
         store32(bar1, 0xFFFFFFFF, 36) ;; dstQueueFamilyIndex = IGNORED
         store64_raw(bar1, image, 40)      ;; image
         store32(bar1, VK_IMAGE_ASPECT_COLOR_BIT, 48) ;; aspectMask
         store32(bar1, 0, 52)  ;; baseMipLevel
         store32(bar1, 1, 56)  ;; levelCount
         store32(bar1, 0, 60)  ;; baseArrayLayer
         store32(bar1, 1, 64)  ;; layerCount
         cmd_pipeline_barrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, 0, 0, 0, 1, bar1)

         ;; VkBufferImageCopy: 56 bytes
         ;; bufferOffset(8) bufferRowLength(4) bufferImageHeight(4)
         ;; imageSubresource(16) imageOffset(12) imageExtent(12)
         mut region = sys_malloc(56)
         memset(region, 0, 56)
         store32(region, 0, 0) store32(region, 0, 4)   ;; bufferOffset
         store32(region, 0, 8)   ;; bufferRowLength (0=tightly packed)
         store32(region, 0, 12)  ;; bufferImageHeight
         store32(region, VK_IMAGE_ASPECT_COLOR_BIT, 16) ;; imageSubresource.aspectMask
         store32(region, 0, 20)  ;; mipLevel
         store32(region, 0, 24)  ;; baseArrayLayer
         store32(region, 1, 28)  ;; layerCount
         store32(region, 0, 32)  ;; imageOffset.x
         store32(region, 0, 36)  ;; imageOffset.y
         store32(region, 0, 40)  ;; imageOffset.z
         store32(region, width, 44)  ;; imageExtent.width
         store32(region, height, 48) ;; imageExtent.height
         store32(region, 1, 52)      ;; imageExtent.depth
         cmd_copy_buffer_to_image(cb, vk_get(VK_CTX_STAGING_BUFFER), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, region)

         ;; Transition: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
         mut bar2 = sys_malloc(72)
         memset(bar2, 0, 72)
         store32(bar2, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0)
         store32(bar2, VK_ACCESS_TRANSFER_WRITE_BIT, 16) ;; srcAccessMask
         store32(bar2, VK_ACCESS_SHADER_READ_BIT, 20)    ;; dstAccessMask
         store32(bar2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 24)    ;; oldLayout
         store32(bar2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 28) ;; newLayout
         store32(bar2, 0xFFFFFFFF, 32)
         store32(bar2, 0xFFFFFFFF, 36)
         store64_raw(bar2, image, 40)
         store32(bar2, VK_IMAGE_ASPECT_COLOR_BIT, 48)
         store32(bar2, 0, 52) store32(bar2, 1, 56)
         store32(bar2, 0, 60) store32(bar2, 1, 64)
         cmd_pipeline_barrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, 0, 0, 0, 1, bar2)

         end_command_buffer(cb)

         ;; Submit and wait
         mut si = sys_malloc(72)
         memset(si, 0, 72)
         store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
         store32(si, 1, 40)  ;; commandBufferCount
         mut cbp = sys_malloc(8)
         store64_raw(cbp, cb, 0)
         store64_raw(si, cbp, 48)
         queue_submit(vk_get(VK_CTX_GRAPHICS_QUEUE), 1, si, 0)
         device_wait_idle(vk_get(VK_CTX_DEVICE))

         free_command_buffers(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_COMMAND_POOL), 1, cb_ptr)
         sys_free(ai) sys_free(bi) sys_free(cb_ptr)
         sys_free(bar1) sys_free(bar2) sys_free(region)
         sys_free(si) sys_free(cbp)
      } else {
         sys_free(ai) sys_free(cb_ptr)
      }
   }

   ;; 5. Create Descriptor Set
   mut alloc_info_ds = sys_malloc(40)
   memset(alloc_info_ds, 0, 40)
   store32(alloc_info_ds, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0)
   store64_raw(alloc_info_ds, to_int(vk_get(VK_CTX_DESCRIPTOR_POOL)), 16)
   store32(alloc_info_ds, 1, 24)
   mut layout_ptr = sys_malloc(8)
   store64_raw(layout_ptr, to_int(vk_get(VK_CTX_DESCRIPTOR_SET_LAYOUT)), 0)
   store64_raw(alloc_info_ds, layout_ptr, 32)

   mut ds_ptr = sys_malloc(8)
   if(allocate_descriptor_sets(vk_get(VK_CTX_DEVICE), alloc_info_ds, ds_ptr) != 0){ return 0 }
   def ds = load64(ds_ptr, 0)

   ;; 6. Update Descriptor Set
   mut image_info = sys_malloc(24)
   store64_raw(image_info, to_int(vk_get(VK_CTX_DEFAULT_SAMPLER)), 0)
   store64_raw(image_info, view, 8)
   store32(image_info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 16)

   mut write_ds = sys_malloc(64)
   memset(write_ds, 0, 64)
   store32(write_ds, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0)
   store64_raw(write_ds, ds, 16)
   store32(write_ds, 0, 24) ;; dstBinding
   store32(write_ds, 0, 28) ;; dstArrayElement
   store32(write_ds, 1, 32) ;; descriptorCount
   store32(write_ds, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36)
   store64_raw(write_ds, image_info, 40)

   update_descriptor_sets(vk_get(VK_CTX_DEVICE), 1, write_ds, 0, 0)

   mut tex = dict(6)
   tex = dict_set(tex, "image", image)
   tex = dict_set(tex, "view", view)
   tex = dict_set(tex, "memory", memory)
   tex = dict_set(tex, "ds", ds)
   tex = dict_set(tex, "width", width)
   tex = dict_set(tex, "height", height)

   vk_set(VK_CTX_TEXTURES, append(vk_get(VK_CTX_TEXTURES), tex))
   len(vk_get(VK_CTX_TEXTURES)) - 1
}

fn update_texture_rect(tex_id, x, y, w, h, pixels){
   "Partially updates a texture's pixel data."
   if(tex_id < 0 || tex_id >= len(vk_get(VK_CTX_TEXTURES))){ return false }
   def tex_obj = get(vk_get(VK_CTX_TEXTURES), tex_id)
   def image = dict_get(tex_obj, "image")

   def img_size = w * h * 4
   if(img_size > vk_get(VK_CTX_STAGING_CAPACITY)){ return false }

   memcpy(vk_get(VK_CTX_STAGING_MAP), pixels, img_size)

   ;; 1. Transition Image: Shader Read Only -> Transfer Dst
   ;; 2. Copy Staging -> Image
   ;; 3. Transition Image: Transfer Dst -> Shader Read Only

   mut alloc_info = sys_malloc(32)
   memset(alloc_info, 0, 32)
   store32(alloc_info, to_int(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO), 0)
   store64_raw(alloc_info, to_int(vk_get(VK_CTX_COMMAND_POOL)), 16)
   store32(alloc_info, 0, 24) ;; PRIMARY
   store32(alloc_info, 1, 28)
   mut cb_ptr = sys_malloc(8)
   if(allocate_command_buffers(vk_get(VK_CTX_DEVICE), alloc_info, cb_ptr) != 0){ return false }
   def cb = load64(cb_ptr, 0)

   mut bi = sys_malloc(32)
   memset(bi, 0, 32)
   store32(bi, to_int(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO), 0)
   store32(bi, 1, 16) ;; ONE_TIME_SUBMIT
   begin_command_buffer(cb, bi)

   mut region = sys_malloc(56)
   memset(region, 0, 56)
   store32(region, to_int(VK_IMAGE_ASPECT_COLOR_BIT), 16)
   store32(region, 0, 20) ;; mipLevel
   store32(region, 0, 24) ;; baseArrayLayer
   store32(region, 1, 28) ;; layerCount
   store32(region, x, 32) ;; offset.x
   store32(region, y, 36) ;; offset.y
   store32(region, 0, 40) ;; offset.z
   store32(region, w, 44) ;; extent.width
   store32(region, h, 48) ;; extent.height
   store32(region, 1, 52) ;; extent.depth

   cmd_copy_buffer_to_image(cb, to_int(vk_get(VK_CTX_STAGING_BUFFER)), to_int(image), to_int(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, region)

   end_command_buffer(cb)

   mut submit_info = sys_malloc(72)
   memset(submit_info, 0, 72)
   store32(submit_info, to_int(VK_STRUCTURE_TYPE_SUBMIT_INFO), 0)
   store32(submit_info, 1, 40) ;; commandBufferCount
   mut cb_ptr_arr = sys_malloc(8)
   store64_raw(cb_ptr_arr, cb, 0)
   store64_raw(submit_info, cb_ptr_arr, 48)

   queue_submit(to_int(vk_get(VK_CTX_GRAPHICS_QUEUE)), 1, submit_info, 0)
   device_wait_idle(vk_get(VK_CTX_DEVICE))

   free_command_buffers(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_COMMAND_POOL), 1, cb_ptr)
   sys_free(alloc_info)
   sys_free(cb_ptr)
   sys_free(bi)
   sys_free(region)
   sys_free(submit_info)
   sys_free(cb_ptr_arr)
   true
}
