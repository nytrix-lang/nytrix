;; Keywords: render vulkan gpu buffers os ui
;; Vulkan buffer allocation, upload, staging, and lifetime management.
;; References:
;; - std.os.ui.render.vk
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk.buffers(create_static_buffer, create_static_index_buffer, create_static_indexed_buffer, buffer_device_address, static_buffer_address, create_gpu_storage_buffer, create_gpu_indirect_buffer, _create_vertex_buffer, _create_staging_buffer, _create_uniform_buffer, _create_descriptor_pool, _find_memory_type, _copy_buffer, _ensure_upload_cb, _begin_upload_cb, _submit_upload_cb)
use std.core
use std.core.mem (__copy_mem)
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.vk.state
use std.os.ui.render.vk.vulkan

fn _buf_trace_enabled() bool {
   ui_profile.env_truthy_cached("NY_VK_BUFFER_TRACE") || ui_profile.debug_enabled()
}

fn _buf_trace(any msg) any {
   if _buf_trace_enabled() { ui_profile.print_text("[gfx:vulkan:buffer] " + to_str(msg)) }
}

fn _frames_in_flight() int { 4 }

fn _positive_int(any value, int fallback) int {
   if is_int(value) && int(value) > 0 { return int(value) }
   fallback
}

fn _vertex_capacity_value() int { _positive_int(_vertex_capacity, 67108864) }

fn _staging_capacity_value() int { _positive_int(_staging_capacity, 134217728) }

fn _vk_buffer_create_info_type() int { 12 }

fn _vk_memory_allocate_info_type() int { 5 }

fn _vk_memory_allocate_flags_info_type() int { 1000060000 }

fn _vk_descriptor_pool_create_info_type() int { 33 }

fn _vk_usage_vertex() int { 0x00000080 }

fn _vk_usage_index() int { 0x00000040 }

fn _vk_usage_uniform() int { 0x00000010 }

fn _vk_usage_transfer_src() int { 0x00000001 }

fn _vk_usage_transfer_dst() int { 0x00000002 }

fn _vk_usage_storage() int { 0x00000200 }

fn _vk_usage_indirect() int { 0x00000800 }

fn _vk_usage_shader_device_address() int { 0x00020000 }

fn _vk_memory_device_local() int { 0x00000001 }

fn _vk_memory_host_visible_coherent() int { 0x00000006 }

fn _vk_memory_allocate_device_address() int { 0x00000002 }

fn _vk_descriptor_combined_image_sampler() int { 1 }

fn _vk_descriptor_uniform_buffer() int { 6 }

fn _max_textures_value() int { 4096 }

fn _vk_sharing_mode_exclusive() int { 0 }

fn _vk_buffer_device_address_info_type() int { 1000244001 }

fn _vk_descriptor_pool_free_descriptor_set() int { 0x00000001 }

fn _vk_command_buffer_allocate_info_type() int { 40 }

fn _vk_command_buffer_begin_info_type() int { 42 }

fn _ubo_size_value() int { 384 }

fn _buffer_create_info(int size, int usage) ?ptr {
   def ci = zalloc(56)
   if !ci { return 0 }
   store32(ci, _vk_buffer_create_info_type(), 0)
   store64_h(ci, size, 24)
   store32(ci, usage, 32)
   store32(ci, _vk_sharing_mode_exclusive(), 36)
   ci
}

fn _memory_alloc_info(handle size, int mem_type_index) ?ptr {
   def ai = zalloc(64)
   if !ai { return 0 }
   store32(ai, _vk_memory_allocate_info_type(), 0)
   store64_h(ai, size, 16)
   store32(ai, mem_type_index, 24)
   ai
}

fn _destroy_bound_buffer(?handle buf, ?handle mem) int {
   if buf { destroy_buffer(_device, buf, 0) }
   if mem { free_memory(_device, mem, 0) }
   0
}

fn _bound_buffer_result(?ptr buf_ptr, ?ptr mem_ptr, handle alloc_size) ?ptr {
   def out = zalloc(24)
   if !out { return 0 }
   __copy_mem(out, buf_ptr, 8)
   __copy_mem(out + 8, mem_ptr, 8)
   store64_h(out, alloc_size, 16)
   out
}

fn _create_bound_buffer(int size, int usage, int properties) any {
   if size <= 0 {
      _buf_trace("create_bound_buffer invalid size=" + to_str(size))
      return 0
   }
   def ci = _buffer_create_info(size, usage)
   if !ci {
      _buf_trace("create_bound_buffer alloc BufferCreateInfo failed size=" + to_str(size))
      return 0
   }
   defer { free(ci) }
   def buf_ptr = zalloc(8)
   if !buf_ptr {
      _buf_trace("create_bound_buffer alloc buf_ptr failed size=" + to_str(size))
      return 0
   }
   defer { free(buf_ptr) }
   def create_res = create_buffer(_device, ci, 0, buf_ptr)
   if create_res != 0 {
      _buf_trace("create_buffer failed code=" + to_str(create_res) + " size=" + to_str(size) + " usage=0x" + to_hex(usage))
      return 0
   }
   def buf = load64_h(buf_ptr, 0)
   if !buf {
      _buf_trace("create_buffer returned null handle size=" + to_str(size))
      return 0
   }
   def mem_req = zalloc(24)
   if !mem_req {
      _buf_trace("memory requirements alloc failed size=" + to_str(size))
      destroy_buffer(_device, buf, 0)
      return 0
   }
   defer { free(mem_req) }
   get_buffer_memory_requirements(_device, buf, mem_req)
   def alloc_size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   def mem_type = _find_memory_type(type_bits, properties)
   if mem_type == -1 {
      _buf_trace("memory type not found size=" + to_str(size) + " alloc=" + to_str(alloc_size) + " bits=0x" + to_hex(type_bits) + " props=0x" + to_hex(properties))
      destroy_buffer(_device, buf, 0)
      return 0
   }
   def alloc_info = _memory_alloc_info(alloc_size, mem_type)
   if !alloc_info {
      _buf_trace("memory alloc info failed size=" + to_str(size) + " alloc=" + to_str(alloc_size) + " type=" + to_str(mem_type))
      destroy_buffer(_device, buf, 0)
      return 0
   }
   defer { free(alloc_info) }
   mut alloc_flags = 0
   if _bda_enabled && ((usage & _vk_usage_shader_device_address()) != 0) {
      alloc_flags = zalloc(24)
      if !alloc_flags {
         _buf_trace("memory alloc flags failed size=" + to_str(size))
         destroy_buffer(_device, buf, 0)
         return 0
      }
      memset(alloc_flags, 0, 24)
      store32(alloc_flags, _vk_memory_allocate_flags_info_type(), 0)
      store32(alloc_flags, _vk_memory_allocate_device_address(), 16)
      store64_h(alloc_info, alloc_flags, 8)
   }
   defer { if alloc_flags { free(alloc_flags) } }
   def mem_ptr = zalloc(8)
   if !mem_ptr {
      _buf_trace("memory handle pointer alloc failed size=" + to_str(size))
      destroy_buffer(_device, buf, 0)
      return 0
   }
   defer { free(mem_ptr) }
   def alloc_res = allocate_memory(_device, alloc_info, 0, mem_ptr)
   if alloc_res != 0 {
      _buf_trace("allocate_memory failed code=" + to_str(alloc_res) + " size=" + to_str(size) + " alloc=" + to_str(alloc_size) + " type=" + to_str(mem_type) + " usage=0x" + to_hex(usage))
      destroy_buffer(_device, buf, 0)
      return 0
   }
   def mem = load64_h(mem_ptr, 0)
   if !mem {
      _buf_trace("allocate_memory returned null size=" + to_str(size) + " alloc=" + to_str(alloc_size) + " type=" + to_str(mem_type))
      destroy_buffer(_device, buf, 0)
      return 0
   }
   def bind_res = bind_buffer_memory(_device, buf, mem, 0)
   if bind_res != 0 {
      _buf_trace("bind_buffer_memory failed code=" + to_str(bind_res) + " size=" + to_str(size) + " alloc=" + to_str(alloc_size) + " type=" + to_str(mem_type))
      _destroy_bound_buffer(buf, mem)
      return 0
   }
   _buf_trace("created size=" + to_str(size) + " alloc=" + to_str(alloc_size) + " type=" + to_str(mem_type) + " usage=0x" + to_hex(usage) + " props=0x" + to_hex(properties))
   def out = _bound_buffer_result(buf_ptr, mem_ptr, alloc_size)
   if !out {
      _destroy_bound_buffer(buf, mem)
      return 0
   }
   out
}

fn _map_memory_ptr(?handle mem, handle size) ?ptr {
   if !mem || size <= 0 {
      _buf_trace("map_memory invalid mem=0x" + to_hex(int(mem)) + " size=" + to_str(size))
      return 0
   }
   def map_ptr = zalloc(8)
   if !map_ptr {
      _buf_trace("map_memory pointer alloc failed size=" + to_str(size))
      return 0
   }
   defer { free(map_ptr) }
   def map_res = map_memory(_device, mem, 0, size, 0, map_ptr)
   if map_res != 0 {
      _buf_trace("map_memory failed code=" + to_str(map_res) + " mem=0x" + to_hex(int(mem)) + " size=" + to_str(size))
      return 0
   }
   def p = load64(map_ptr, 0)
   if !p { _buf_trace("map_memory returned null mem=0x" + to_hex(int(mem)) + " size=" + to_str(size)) }
   p
}

fn buffer_device_address(?handle buf) int {
   "Returns the GPU virtual address for a buffer when BDA is enabled."
   if !_bda_enabled || !buf { return 0 }
   def info = zalloc(24)
   if !info { return 0 }
   defer { free(info) }
   store32(info, _vk_buffer_device_address_info_type(), 0)
   store64_h(info, buf, 16)
   get_buffer_device_address(_device, info)
}

fn static_buffer_address(any desc) int {
   "Returns the GPU address stored on a static buffer descriptor, including any mega-buffer offset."
   if !is_dict(desc) { return 0 }
   def cached = int(desc.get("address", 0))
   if cached != 0 { return cached }
   def base = buffer_device_address(desc.get("handle", 0))
   if base == 0 { return 0 }
   base + int(desc.get("offset", 0))
}

fn _static_buffer_desc(?handle buf, ?handle mem, int count, int off=-1, ?handle ibuf=0, ?handle imem=0, int idx_count=0, int idx_off=-1, bool use_u32=false) dict {
   mut out = {"handle": buf, "memory": mem, "count": count}
   if off >= 0 {
      out["offset"] = off
      out["shared"] = true
   }
   if ibuf {
      out["ibuf"] = ibuf
      out["imemory"] = imem
      out["index_count"] = idx_count
      out["index_type_u32"] = use_u32
      if idx_off >= 0 { out["ioffset"] = idx_off }
   }
   out
}

fn _create_staging_buffer() bool {
   def staging_cap = _staging_capacity_value()
   _staging_capacity = staging_cap
   def b = _create_bound_buffer(staging_cap, _vk_usage_transfer_dst() | _vk_usage_transfer_src(), _vk_memory_host_visible_coherent())
   if !b { return false }
   _staging_buffer, _staging_memory = load64_h(b, 0), load64_h(b, 8)
   def map_size = load64_h(b, 16)
   free(b)
   _staging_map = _map_memory_ptr(_staging_memory, map_size)
   if _staging_map { return true }
   _destroy_bound_buffer(_staging_buffer, _staging_memory)
   _staging_buffer = 0
   _staging_memory = 0
   false
}

fn _create_descriptor_pool() bool {
   def tex_count = _max_textures_value()
   def max_sets = 1
   def pool_sizes = zalloc(16)
   if !pool_sizes { return false }
   defer { free(pool_sizes) }
   store32(pool_sizes, _vk_descriptor_combined_image_sampler(), 0)
   store32(pool_sizes, tex_count + 64, 4)
   store32(pool_sizes, _vk_descriptor_uniform_buffer(), 8)
   store32(pool_sizes, _frames_in_flight(), 12)
   def pool_ci = zalloc(40)
   if !pool_ci { return false }
   defer { free(pool_ci) }
   store32(pool_ci, _vk_descriptor_pool_create_info_type(), 0)
   store32(pool_ci, _vk_descriptor_pool_free_descriptor_set(), 16)
   store32(pool_ci, max_sets + _frames_in_flight() + 64, 20)
   store32(pool_ci, 2, 24)
   store64_h(pool_ci, pool_sizes, 32)
   def pool_ptr = zalloc(8)
   if !pool_ptr { return false }
   defer { free(pool_ptr) }
   if create_descriptor_pool(_device, pool_ci, 0, pool_ptr) != 0 { return false }
   _descriptor_pool = load64_h(pool_ptr, 0)
   _descriptor_pool != 0
}

fn _create_uniform_buffer() bool {
   def align = 256
   def ubo_size = _ubo_size_value()
   _ubo_stride = int(((ubo_size + align - 1) / align) * align)
   def total = _ubo_stride * _frames_in_flight()
   _buf_trace("uniform start ubo_size=" + to_str(ubo_size) + " stride=" + to_str(_ubo_stride) + " total=" + to_str(total))
   def b = _create_bound_buffer(total, _vk_usage_uniform(), _vk_memory_host_visible_coherent())
   if !b {
      _buf_trace("uniform create_bound_buffer failed total=" + to_str(total))
      return false
   }
   _ubo_buffer, _ubo_memory = load64_h(b, 0), load64_h(b, 8)
   def size = load64_h(b, 16)
   free(b)
   _ubo_map = _map_memory_ptr(_ubo_memory, size)
   if !_ubo_map {
      _buf_trace("uniform map failed size=" + to_str(size) + " mem=0x" + to_hex(int(_ubo_memory)))
      _destroy_bound_buffer(_ubo_buffer, _ubo_memory)
      _ubo_buffer = 0
      _ubo_memory = 0
      return false
   }
   _ubo_map_size = size
   _ubo_staging = zalloc(size)
   if _ubo_staging { return true }
   _buf_trace("uniform staging alloc failed size=" + to_str(size))
   _destroy_bound_buffer(_ubo_buffer, _ubo_memory)
   _ubo_buffer = 0
   _ubo_memory = 0
   _ubo_map = 0
   false
}

fn _find_memory_type(int type_filter, int properties) int {
   def mem_props = zalloc(520)
   if !mem_props { return -1 }
   defer { free(mem_props) }
   get_physical_device_memory_properties(_physical_device, mem_props)
   def count = load32(mem_props, 0)
   mut i = 0
   while i < count {
      if (type_filter & (1 << i)) != 0 {
         def flags = load32(mem_props, 4 + i * 8)
         if (flags & properties) == properties { return i }
      }
      i += 1
   }
   -1
}

fn _copy_buffer(?handle src, ?handle dst, int size) bool { _copy_buffer_region(src, dst, 0, 0, size) }

fn _ensure_upload_cb() any {
   if !_upload_alloc || !_upload_cb_ptr { return 0 }
   if _upload_cb != 0 {
      reset_command_buffer(_upload_cb, 0)
      return _upload_cb
   }
   memset(_upload_alloc, 0, 32)
   store32(_upload_alloc, _vk_command_buffer_allocate_info_type(), 0)
   store64_h(_upload_alloc, _command_pool, 16)
   store32(_upload_alloc, 0, 24)
   store32(_upload_alloc, 1, 28)
   if allocate_command_buffers(_device, _upload_alloc, _upload_cb_ptr) != 0 { return 0 }
   _upload_cb = load64(_upload_cb_ptr, 0)
   _upload_cb
}

fn _begin_upload_cb(any cb) bool {
   if !cb || !_upload_bi { return false }
   memset(_upload_bi, 0, 32)
   store32(_upload_bi, _vk_command_buffer_begin_info_type(), 0)
   store32(_upload_bi, 1, 16)
   def res = begin_command_buffer(cb, _upload_bi)
   if res != 0 {
      _buf_trace("begin upload command buffer failed code=" + to_str(res))
      return false
   }
   true
}

fn _submit_upload_cb(any cb) bool {
   if !cb || !_upload_si || !_upload_cb_arr || !_upload_cb_ptr || !_upload_fence_ptr || !_upload_fence { return false }
   memset(_upload_si, 0, 72)
   store32(_upload_si, VK_STRUCTURE_TYPE_SUBMIT_INFO, 0)
   store32(_upload_si, 1, 40)
   store64_h(_upload_cb_arr, load64(_upload_cb_ptr, 0), 0)
   store64_h(_upload_si, _upload_cb_arr, 48)
   def reset_res = reset_fences(_device, 1, _upload_fence_ptr)
   if reset_res != 0 {
      _buf_trace("reset upload fence failed code=" + to_str(reset_res))
      return false
   }
   def submit_res = queue_submit(_graphics_queue, 1, _upload_si, _upload_fence)
   if submit_res != 0 {
      _buf_trace("submit upload command buffer failed code=" + to_str(submit_res))
      return false
   }
   def wait_res = wait_for_fences(_device, 1, _upload_fence_ptr, 1, 5_000_000_000) ;; 5 second timeout
   if wait_res != 0 {
      _buf_trace("wait upload fence failed code=" + to_str(wait_res))
      return false
   }
   true
}

fn _copy_buffer_region(?handle src, ?handle dst, int src_off, int dst_off, int size) bool {
   if size <= 0 || !src || !dst || !_device || !_graphics_queue || !_upload_bi || !_upload_region || !_upload_si || !_upload_cb_arr || !_upload_fence_ptr || !_upload_fence { return false }
   def cb = _ensure_upload_cb()
   if !cb { return false }
   if !_begin_upload_cb(cb) { return false }
   memset(_upload_region, 0, 24)
   store64_h(_upload_region, src_off, 0)
   store64_h(_upload_region, dst_off, 8)
   store64_h(_upload_region, size, 16)
   cmd_copy_buffer(cb, int(src), int(dst), 1, _upload_region)
   def end_res = end_command_buffer(cb)
   if end_res != 0 {
      _buf_trace("end upload command buffer failed code=" + to_str(end_res))
      return false
   }
   _submit_upload_cb(cb)
}

fn _upload_host_to_buffer(?ptr src_ptr, ?handle dst_buf, int size, int dst_off=0) bool {
   def staging_cap = _staging_capacity_value()
   if !src_ptr || !dst_buf || size <= 0 || !_staging_map || staging_cap <= 0 { return false }
   mut off = 0
   while off < size {
      def chunk = min(staging_cap, size - off)
      __copy_mem(_staging_map, src_ptr + off, chunk)
      if !_copy_buffer_region(_staging_buffer, dst_buf, 0, dst_off + off, chunk) { return false }
      off += chunk
   }
   true
}

fn _create_device_local_buffer(int size, int usage) any {
   mut final_usage = usage | _vk_usage_transfer_dst()
   if _bda_enabled && (final_usage & _vk_usage_storage()) != 0 {
      final_usage = final_usage | _vk_usage_shader_device_address()
   }
   _create_bound_buffer(size, final_usage, _vk_memory_device_local())
}

fn _copy_cpu_buffer(?ptr src_ptr, int size) ?ptr {
   if !src_ptr || size <= 0 { return 0 }
   def dst = malloc(size)
   if !dst { return 0 }
   __copy_mem(dst, src_ptr, size)
   dst
}

fn create_static_buffer(?ptr src_ptr, int count) any {
   "Creates a static GPU vertex buffer and uploads data to it. Returns a buffer descriptor dict."
   if !src_ptr || count <= 0 { return 0 }
   def size = count * _VKR_VERT_STRIDE
   def db = _create_device_local_buffer(size, _vk_usage_vertex())
   if !db { return 0 }
   def d_buf = load64_h(db, 0)
   def d_mem = load64_h(db, 8)
   free(db)
   if !_upload_host_to_buffer(src_ptr, d_buf, size) {
      _destroy_bound_buffer(d_buf, d_mem)
      return 0
   }
   def out = _static_buffer_desc(d_buf, d_mem, count)
   def cpu = _copy_cpu_buffer(src_ptr, size)
   if cpu {
      out["cpu_ptr"] = cpu
      out["cpu_count"] = count
   }
   out
}

fn create_static_index_buffer(?ptr idx_ptr, int idx_count, bool use_u32=false) any {
   "Creates a static GPU index buffer for dynamic vertex streams."
   if !idx_ptr || idx_count <= 0 { return 0 }
   def isize = idx_count * (use_u32 ? 4 : 2)
   def ib = _create_device_local_buffer(isize, _vk_usage_index())
   if !ib { return 0 }
   def di_buf = load64_h(ib, 0)
   def di_mem = load64_h(ib, 8)
   free(ib)
   if !_upload_host_to_buffer(idx_ptr, di_buf, isize) {
      _destroy_bound_buffer(di_buf, di_mem)
      return 0
   }
   def out = _static_buffer_desc(0, 0, 0, -1, di_buf, di_mem, idx_count, -1, use_u32)
   def cpu_idx = _copy_cpu_buffer(idx_ptr, isize)
   if cpu_idx {
      out["cpu_idx_ptr"] = cpu_idx
      out["cpu_idx_count"] = idx_count
      out["cpu_index_type_u32"] = use_u32
   }
   out
}

fn create_static_indexed_buffer(?ptr vert_ptr, int count, ?ptr idx_ptr, int idx_count, any opts=0) any {
   "Creates static GPU vertex and index buffers with indexed drawing."
   if !vert_ptr || count <= 0 || !idx_ptr || idx_count <= 0 { return 0 }
   def use_u32 = is_dict(opts) && opts.get("index_type_u32", false)
   def vsize = count * _VKR_VERT_STRIDE
   def isize = idx_count * (use_u32 ? 4 : 2)
   def db = _create_device_local_buffer(vsize, _vk_usage_vertex())
   if !db { return 0 }
   def d_buf = load64_h(db, 0)
   def d_mem = load64_h(db, 8)
   free(db)
   if !_upload_host_to_buffer(vert_ptr, d_buf, vsize) {
      _destroy_bound_buffer(d_buf, d_mem)
      return 0
   }
   def ib = _create_device_local_buffer(isize, _vk_usage_index())
   if !ib {
      _destroy_bound_buffer(d_buf, d_mem)
      return 0
   }
   def di_buf = load64_h(ib, 0)
   def di_mem = load64_h(ib, 8)
   free(ib)
   if !_upload_host_to_buffer(idx_ptr, di_buf, isize) {
      _destroy_bound_buffer(di_buf, di_mem)
      _destroy_bound_buffer(d_buf, d_mem)
      return 0
   }
   def out = _static_buffer_desc(d_buf, d_mem, count, -1, di_buf, di_mem, idx_count, -1, use_u32)
   def cpu = _copy_cpu_buffer(vert_ptr, vsize)
   if cpu {
      out["cpu_ptr"] = cpu
      out["cpu_count"] = count
   }
   def cpu_idx = _copy_cpu_buffer(idx_ptr, isize)
   if cpu_idx {
      out["cpu_idx_ptr"] = cpu_idx
      out["cpu_idx_count"] = idx_count
      out["cpu_index_type_u32"] = use_u32
   }
   out
}

fn create_gpu_storage_buffer(?ptr src_ptr, int size, int usage=0) any {
   "Creates a GPU-only SSBO-style buffer. If src_ptr is provided, data is uploaded through staging."
   if size <= 0 { return 0 }
   def storage_usage = _vk_usage_storage() | usage
   def db = _create_device_local_buffer(size, storage_usage)
   if !db { return 0 }
   def d_buf = load64_h(db, 0)
   def d_mem = load64_h(db, 8)
   free(db)
   if src_ptr && !_upload_host_to_buffer(src_ptr, d_buf, size) {
      _destroy_bound_buffer(d_buf, d_mem)
      return 0
   }
   mut out = {"handle": d_buf, "memory": d_mem, "size": size, "usage": storage_usage}
   def addr = buffer_device_address(d_buf)
   if addr != 0 { out["address"] = addr }
   out
}

fn create_gpu_indirect_buffer(int draw_count, bool indexed=true) any {
   "Creates a GPU-only indirect-draw buffer for compute-written draw commands."
   if draw_count <= 0 { return 0 }
   def stride = indexed ? 20 : 16
   def usage = _vk_usage_indirect() | _vk_usage_storage()
   mut out = create_gpu_storage_buffer(0, draw_count * stride, usage)
   if is_dict(out) {
      out["draw_count"] = draw_count
      out["stride"] = stride
      out["indexed"] = indexed
   }
   out
}

fn _create_vertex_buffer() bool {
   def vertex_cap = _vertex_capacity_value()
   _vertex_capacity = vertex_cap
   def total = vertex_cap * _frames_in_flight()
   def b = _create_bound_buffer(total, _vk_usage_vertex(), _vk_memory_host_visible_coherent())
   if !b { return false }
   _vertex_buffer, _vertex_memory = load64_h(b, 0), load64_h(b, 8)
   if !_vertex_buffer_raw { _vertex_buffer_raw = zalloc(8) }
   if _vertex_buffer_raw { __copy_mem(_vertex_buffer_raw, b, 8) }
   def map_size = load64_h(b, 16)
   free(b)
   _vertex_map = _map_memory_ptr(_vertex_memory, map_size)
   if !_vertex_map {
      _destroy_bound_buffer(_vertex_buffer, _vertex_memory)
      _vertex_buffer = 0
      if _vertex_buffer_raw { store64(_vertex_buffer_raw, 0, 0) }
      _vertex_memory = 0
      return false
   }
   _local_vertex_map = _vertex_map
   true
}
