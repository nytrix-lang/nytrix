;; Auto-generated split Vulkan renderer component
module std.ui.gfx.vk.buffers (
  _create_command_pool,
  _create_command_buffers,
  _create_vertex_buffer,
  _create_staging_buffer,
  _find_memory_type
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
use std.ui.gfx.vk.utils *

fn _create_command_pool(){
   "Creates the Vulkan command pool for recording draw commands."
   mut create_info = sys_malloc(32)
   memset(create_info, 0, 32)
   store32(create_info, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0)
   store32(create_info, 2, 16) ;; flags (2 = RESET_BIT)
   store32(create_info, vk_get(VK_CTX_GRAPHICS_FAMILY_INDEX), 20)
   mut pool_ptr = sys_malloc(8)
   def cp_res = create_command_pool(vk_get(VK_CTX_DEVICE), create_info, 0, pool_ptr)
   if(cp_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreateCommandPool failed with code {cp_res}") }
      return false
   }
   vk_set(VK_CTX_COMMAND_POOL, load64(pool_ptr, 0))
    if(_is_debug()){ _dbg_handle("command_pool", vk_get(VK_CTX_COMMAND_POOL)) }
   true
}

fn _create_command_buffers(){
   "Allocates primary command buffers from the pool."
   mut ai = sys_malloc(32)
   memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_raw(ai, vk_get(VK_CTX_COMMAND_POOL), 16)
   store32(ai, 0, 24) ;; level (0 = PRIMARY)
   store32(ai, vk_get(VK_CTX_MAX_FRAMES_IN_FLIGHT), 28)
   mut bufs_ptr = sys_malloc(vk_get(VK_CTX_MAX_FRAMES_IN_FLIGHT) * 8)
   def cb_res = allocate_command_buffers(vk_get(VK_CTX_DEVICE), ai, bufs_ptr)
   if(cb_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkAllocateCommandBuffers failed with code {cb_res}") }
      return false
   }
   vk_set(VK_CTX_COMMAND_BUFFERS, [])
   mut i = 0
   while(i < vk_get(VK_CTX_MAX_FRAMES_IN_FLIGHT)){
      vk_set(VK_CTX_COMMAND_BUFFERS, append(vk_get(VK_CTX_COMMAND_BUFFERS), load64(bufs_ptr, i * 8)))
      i += 1
   }
   true
}

fn _create_vertex_buffer(){
   "Creates the GPU vertex buffer for batch rendering."
   mut ci = sys_malloc(56) ;; VkBufferCreateInfo
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store32(ci, 0, 16) ;; flags
   store64_raw(ci, to_int(vk_get(VK_CTX_VERTEX_CAPACITY)), 24) ;; size
   store32(ci, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 32) ;; usage
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36) ;; sharingMode
   mut buf_ptr = sys_malloc(8)
   def res = create_buffer(vk_get(VK_CTX_DEVICE), ci, 0, buf_ptr)
   if(res != 0){
      if(_is_debug()){ print(f"Vulkan: Vertex buffer creation failed {res}") }
      return false
   }
   vk_set(VK_CTX_VERTEX_BUFFER, load64(buf_ptr, 0))

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_VERTEX_BUFFER), mem_req)
   def size = load64(mem_req, 0)
   def align = load64(mem_req, 8)
   def type_bits = load32(mem_req, 16)

   ;; Find memory type (Host Visible | Host Coherent)
   mut mem_props = sys_malloc(520) ;; VkPhysicalDeviceMemoryProperties (roughly)
   get_physical_device_memory_properties(vk_get(VK_CTX_PHYSICAL_DEVICE), mem_props)
   def mem_type_count = load32(mem_props, 0)
   mut mem_type_index = -1
   mut i = 0
   while(i < mem_type_count){
      if((type_bits & (1 << i)) != 0){
         def flags = load32(mem_props, 8 + i * 8 + 4) ;; propertyFlags
         if((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
            mem_type_index = i
            break
         }
      }
      i += 1
   }
   if(mem_type_index == -1){
      if(_is_debug()){ print("Vulkan: Could not find suitable memory for vertex buffer") }
      return false
   }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)

   mut mem_ptr = sys_malloc(8)
   def alloc_res = allocate_memory(vk_get(VK_CTX_DEVICE), alloc_info, 0, mem_ptr)
   if(alloc_res != 0){
      if(_is_debug()){ print(f"Vulkan: Vertex memory allocation failed {alloc_res}") }
      return false
   }
   vk_set(VK_CTX_VERTEX_MEMORY, load64(mem_ptr, 0))

   bind_buffer_memory(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_VERTEX_BUFFER), vk_get(VK_CTX_VERTEX_MEMORY), 0)

   mut map_ptr = sys_malloc(8)
   map_memory(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_VERTEX_MEMORY), 0, size, 0, map_ptr)
   vk_set(VK_CTX_VERTEX_MAP, load64(map_ptr, 0))

   true
}

fn _create_staging_buffer(){
   "Creates the GPU staging buffer for data uploads."
   mut ci = sys_malloc(56)
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store32(ci, 0, 16) ;; flags
   store64_raw(ci, to_int(vk_get(VK_CTX_STAGING_CAPACITY)), 24) ;; size
   store32(ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 32)
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36)
   mut buf_ptr = sys_malloc(8)
   if(create_buffer(vk_get(VK_CTX_DEVICE), ci, 0, buf_ptr) != 0){ return false }
   vk_set(VK_CTX_STAGING_BUFFER, load64(buf_ptr, 0))

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_STAGING_BUFFER), mem_req)
   def size = load64(mem_req, 0)
   def type_bits = load32(mem_req, 16)

   mut mem_props = sys_malloc(520)
   get_physical_device_memory_properties(vk_get(VK_CTX_PHYSICAL_DEVICE), mem_props)
   def mem_type_count = load32(mem_props, 0)
   mut mem_type_index = -1
   mut i = 0
   while(i < mem_type_count){
      if((type_bits & (1 << i)) != 0){
         def flags = load32(mem_props, 8 + i * 8 + 4)
         if((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
            mem_type_index = i
            break
         }
      }
      i += 1
   }
   if(mem_type_index == -1){ return false }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_raw(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)
   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(vk_get(VK_CTX_DEVICE), alloc_info, 0, mem_ptr) != 0){ return false }
   vk_set(VK_CTX_STAGING_MEMORY, load64(mem_ptr, 0))
   bind_buffer_memory(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_STAGING_BUFFER), vk_get(VK_CTX_STAGING_MEMORY), 0)

   mut map_ptr = sys_malloc(8)
   map_memory(vk_get(VK_CTX_DEVICE), vk_get(VK_CTX_STAGING_MEMORY), 0, size, 0, map_ptr)
   vk_set(VK_CTX_STAGING_MAP, load64(map_ptr, 0))

   true
}

fn _find_memory_type(type_filter, properties){
   "Heuristic to find the best Vulkan memory type index for given filter/props."
   mut mem_props = sys_malloc(520)
   get_physical_device_memory_properties(vk_get(VK_CTX_PHYSICAL_DEVICE), mem_props)
   def count = load32(mem_props, 0)
   mut i = 0
   while(i < count){
      if((type_filter & (1 << i)) != 0){
         def flags = load32(mem_props, 8 + i * 8 + 4)
         if((flags & properties) == properties){ return i }
      }
      i += 1
   }
   -1
}
