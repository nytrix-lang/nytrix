;; Keywords: ui gfx vulkan renderer buffers

module std.ui.gfx.vk.buffers (
   create_static_buffer,
   _create_vertex_buffer, _create_staging_buffer, _create_uniform_buffer, _create_descriptor_pool, _find_memory_type, _copy_buffer
)

use std.core *
use std.core.mem *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.vulkan *
use std.util.common as common

fn _create_staging_buffer(){
   "Creates the GPU staging buffer for data uploads."
   mut ci = sys_malloc(56)
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store32(ci, 0, 16) ; flags
   store64_h(ci, _staging_capacity, 24) ; size
   store32(ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 32)
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36)
   mut buf_ptr = sys_malloc(8)
   if(create_buffer(_device, ci, 0, buf_ptr) != 0){ return false }
   _staging_buffer = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, _staging_buffer, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)

   def mem_type_index = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
   if(mem_type_index == -1){ return false }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)
   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return false }
   _staging_memory = load64(mem_ptr, 0)
   bind_buffer_memory(_device, _staging_buffer, _staging_memory, 0)

   mut map_ptr = sys_malloc(8)
   map_memory(_device, _staging_memory, 0, size, 0, map_ptr)
   _staging_map = load64(map_ptr, 0)

   true
}

fn _create_descriptor_pool(){
   "Initializes the Vulkan descriptor pool for shaders."
   def tex_count = _bindless_enabled ? MAX_TEXTURES : 1000
   mut pool_sizes = sys_malloc(16)
   store32(pool_sizes, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0)
   store32(pool_sizes, tex_count, 4)
   store32(pool_sizes, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8)
   store32(pool_sizes, MAX_FRAMES_IN_FLIGHT, 12)

   mut pool_ci = sys_malloc(40)
   memset(pool_ci, 0, 40)
   store32(pool_ci, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0)
   store32(pool_ci, _bindless_enabled ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0, 16) ; flags
   store32(pool_ci, (_bindless_enabled ? 1 : 1000) + MAX_FRAMES_IN_FLIGHT, 20) ; maxSets
   store32(pool_ci, 2, 24) ; poolSizeCount
   store64_h(pool_ci, pool_sizes, 32)

   mut pool_ptr = sys_malloc(8)
   if(create_descriptor_pool(_device, pool_ci, 0, pool_ptr) != 0){ return false }
   _descriptor_pool = load64(pool_ptr, 0)
   true
}

fn _create_uniform_buffer(){
   "Creates a persistently-mapped uniform buffer for per-frame matrices."
   def align = 256
   _ubo_stride = (( _UBO_SIZE + align - 1) / align) * align
   def total = _ubo_stride * MAX_FRAMES_IN_FLIGHT

   mut ci = sys_malloc(56)
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_h(ci, total, 24)
   store32(ci, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 32)
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36)
   mut buf_ptr = sys_malloc(8)
   if(create_buffer(_device, ci, 0, buf_ptr) != 0){ return false }
   _ubo_buffer = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, _ubo_buffer, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   common.touch(type_bits)

   def mem_type_index = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
   if(mem_type_index == -1){ return false }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)

   mut mem_ptr = sys_malloc(8)
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){ return false }
   _ubo_memory = load64(mem_ptr, 0)
   bind_buffer_memory(_device, _ubo_buffer, _ubo_memory, 0)

   mut map_ptr = sys_malloc(8)
   if(map_memory(_device, _ubo_memory, 0, size, 0, map_ptr) != 0){ return false }
   _ubo_map = load64(map_ptr, 0)
   _ubo_map_size = size
   true
}

fn _find_memory_type(type_filter, properties){
   "Heuristic to find the best Vulkan memory type index for given filter/props."
   mut mem_props = sys_malloc(520)
   get_physical_device_memory_properties(_physical_device, mem_props)
   def count = load32(mem_props, 0)
   mut i = 0
   while(i < count){
      if((type_filter & (1 << i)) != 0){
         def flags = load32(mem_props, 4 + i * 8)
         if((flags & properties) == properties){
         sys_free(mem_props)
         return i
         }
      }
      i += 1
   }
   sys_free(mem_props)
   -1
}

fn _copy_buffer(src, dst, size){
   "Internal: performs a synchronous GPU-to-GPU buffer copy using a transient command buffer."
   mut bi = sys_malloc(32) memset(bi, 0, 32)
   store32(bi, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0)
   store32(bi, 1, 16) ; ONE_TIME_SUBMIT

   mut ai = sys_malloc(32) memset(ai, 0, 32)
   store32(ai, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0)
   store64_h(ai, _command_pool, 16)
   store32(ai, 0, 24) ; PRIMARY
   store32(ai, 1, 28) ; 1
   mut cb_ptr = sys_malloc(8)
   allocate_command_buffers(_device, ai, cb_ptr)
   def cb = load64(cb_ptr, 0)

   begin_command_buffer(cb, bi)
   mut region = sys_malloc(24) memset(region, 0, 24)
   store64_h(region, 0, 0) ; srcOffset
   store64_h(region, 0, 8) ; dstOffset
   store64_h(region, size, 16)
   cmd_copy_buffer(cb, src, dst, 1, region)
   end_command_buffer(cb)

   mut si = sys_malloc(72) memset(si, 0, 72)
   store32(si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(si, 1, 40) ; cb count
   mut cb_arr = sys_malloc(8) store64_h(cb_arr, cb, 0)
   store64_h(si, cb_arr, 48)

   queue_submit(_graphics_queue, 1, si, 0)
   queue_wait_idle(_graphics_queue)

   free_command_buffers(_device, _command_pool, 1, cb_ptr)
   sys_free(bi) sys_free(ai) sys_free(cb_ptr) sys_free(region) sys_free(si) sys_free(cb_arr)
}

fn create_static_buffer(ptr, count){
   "Creates a device-local GPU vertex buffer and uploads data to it. Returns a buffer descriptor dict."
   if(!ptr || count <= 0){ return 0 }
   def size = count * _VKR_VERT_STRIDE

   ;; 1. Staging Buffer
   mut s_ci = sys_malloc(56) memset(s_ci, 0, 56)
   store32(s_ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_h(s_ci, size, 24)
   store32(s_ci, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 32)
   mut s_ptr = sys_malloc(8)
   if(create_buffer(_device, s_ci, 0, s_ptr) != 0){ return 0 }
   def s_buf = load64(s_ptr, 0)

   mut s_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, s_buf, s_req)
   def s_size = load64_h(s_req, 0)
   def s_type = _find_memory_type(load32(s_req, 16), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

   mut s_alloc = sys_malloc(64) memset(s_alloc, 0, 64)
   store32(s_alloc, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(s_alloc, s_size, 16)
   store32(s_alloc, s_type, 24)
   mut s_mem_ptr = sys_malloc(8)
   allocate_memory(_device, s_alloc, 0, s_mem_ptr)
   def s_mem = load64(s_mem_ptr, 0)
   bind_buffer_memory(_device, s_buf, s_mem, 0)

   mut s_map = sys_malloc(8)
   map_memory(_device, s_mem, 0, size, 0, s_map)
   def s_ptr_map = load64(s_map, 0)
   memcpy(s_ptr_map, ptr, size)
   unmap_memory(_device, s_mem)

   ;; 2. Final Device-Local Buffer
   mut d_ci = sys_malloc(56) memset(d_ci, 0, 56)
   store32(d_ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_h(d_ci, size, 24)
   store32(d_ci, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 32)
   mut d_ptr = sys_malloc(8)
   create_buffer(_device, d_ci, 0, d_ptr)
   def d_buf = load64(d_ptr, 0)

   mut d_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, d_buf, d_req)
   def d_size = load64_h(d_req, 0)
   def d_type = _find_memory_type(load32(d_req, 16), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)

   mut d_alloc = sys_malloc(64) memset(d_alloc, 0, 64)
   store32(d_alloc, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(d_alloc, d_size, 16)
   store32(d_alloc, d_type, 24)
   mut d_mem_ptr = sys_malloc(8)
   allocate_memory(_device, d_alloc, 0, d_mem_ptr)
   def d_mem = load64(d_mem_ptr, 0)
   bind_buffer_memory(_device, d_buf, d_mem, 0)

   ;; 3. Transfer
   _copy_buffer(s_buf, d_buf, size)

   ;; 4. Cleanup Staging
   destroy_buffer(_device, s_buf, 0)
   free_memory(_device, s_mem, 0)

   sys_free(s_ci) sys_free(s_ptr) sys_free(s_req) sys_free(s_alloc) sys_free(s_mem_ptr) sys_free(s_map)
   sys_free(d_ci) sys_free(d_ptr) sys_free(d_req) sys_free(d_alloc) sys_free(d_mem_ptr)

   mut m = dict()
   m = dict_set(m, "handle", d_buf)
   m = dict_set(m, "memory", d_mem)
   m = dict_set(m, "count", count)
   m
}

fn _create_vertex_buffer(){
   "Creates the GPU vertex buffer for batch rendering."
   mut ci = sys_malloc(56)
   memset(ci, 0, 56)
   store32(ci, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0)
   store64_h(ci, _vertex_capacity * MAX_FRAMES_IN_FLIGHT, 24) ; total bytes across frame slices
   store32(ci, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 32)
   store32(ci, VK_SHARING_MODE_EXCLUSIVE, 36)
   mut buf_ptr = sys_malloc(8)
   def res = create_buffer(_device, ci, 0, buf_ptr)
   if(res != 0){
      return false
   }
   _vertex_buffer = load64(buf_ptr, 0)

   mut mem_req = sys_malloc(24)
   get_buffer_memory_requirements(_device, _vertex_buffer, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   common.touch(type_bits)

   def mem_type_index = _find_memory_type(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
   if(mem_type_index == -1){ return false }

   mut alloc_info = sys_malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0)
   store64_h(alloc_info, size, 16)
   store32(alloc_info, mem_type_index, 24)

   mut mem_ptr = sys_malloc(8)
   def alloc_res = allocate_memory(_device, alloc_info, 0, mem_ptr)
   if(alloc_res != 0){
      return false
   }
   _vertex_memory = load64(mem_ptr, 0)

   bind_buffer_memory(_device, _vertex_buffer, _vertex_memory, 0)

   mut map_ptr = sys_malloc(8)
   def map_res = map_memory(_device, _vertex_memory, 0, size, 0, map_ptr)
   if(map_res == 0){
       _vertex_map = load64(map_ptr, 0)
   } else {
       _vertex_map = 0
   }

   if(!_vertex_map){ return false }
   ; Use the persistently-mapped GPU buffer directly (host-visible + coherent)
   _local_vertex_map = _vertex_map
   true
}
