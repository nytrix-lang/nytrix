;; Keywords: render vulkan gpu
;; Vulkan bindings for Nytrix
module std.os.ui.render.vk.vulkan(
   VkImageMemoryBarrierColor, vk_get_instance_proc_addr, vk_create_instance, destroy_instance,
   enumerate_instance_extension_properties, enumerate_instance_layer_properties, enumerate_physical_devices,
   get_physical_device_properties, get_physical_device_memory_properties,
   get_physical_device_queue_family_properties, get_physical_device_format_properties,
   get_physical_device_features2, create_device, destroy_device, get_device_queue, get_buffer_device_address,
   create_swapchain_khr, destroy_swapchain_khr, get_swapchain_images_khr, acquire_next_image_khr,
   queue_present_khr, create_image_view, destroy_image_view, create_image, destroy_image, create_buffer,
   destroy_buffer, get_buffer_memory_requirements, bind_buffer_memory, map_memory, unmap_memory,
   create_command_pool, destroy_command_pool, allocate_command_buffers, begin_command_buffer,
   end_command_buffer, cmd_begin_render_pass, cmd_end_render_pass, cmd_bind_pipeline, cmd_draw,
   cmd_draw_indexed, cmd_draw_indirect, cmd_draw_indexed_indirect, cmd_dispatch, cmd_dispatch_indirect,
   cmd_bind_vertex_buffers, cmd_bind_index_buffer, cmd_pipeline_barrier, cmd_copy_buffer,
   cmd_copy_buffer_to_image, cmd_copy_image, cmd_blit_image, create_semaphore, create_fence, destroy_semaphore,
   destroy_fence, wait_for_fences, reset_fences, queue_submit, create_render_pass, destroy_render_pass,
   create_framebuffer, destroy_framebuffer, create_descriptor_set_layout, destroy_descriptor_set_layout,
   create_descriptor_pool, destroy_descriptor_pool, allocate_descriptor_sets, update_descriptor_sets,
   cmd_bind_descriptor_sets, create_pipeline_layout, destroy_pipeline_layout, create_graphics_pipelines,
   create_compute_pipelines, destroy_pipeline, create_shader_module, destroy_shader_module,
   vk_create_xcb_surface_khr, vk_create_xlib_surface_khr, vk_create_win32_surface_khr,
   vk_create_wayland_surface_khr, vk_create_metal_surface_ext, vk_get_physical_device_surface_capabilities_khr,
   vk_get_physical_device_surface_support_khr, vk_get_physical_device_surface_formats_khr,
   vk_get_physical_device_surface_present_modes_khr, vk_get_physical_device_wayland_presentation_support_khr,
   get_physical_device_surface_support_khr, get_physical_device_surface_formats_khr,
   get_physical_device_surface_present_modes_khr, get_physical_device_surface_capabilities_khr,
   destroy_surface_khr, allocate_memory, free_memory, bind_image_memory, get_image_memory_requirements,
   device_wait_idle, free_command_buffers, create_sampler, destroy_sampler, cmd_set_viewport, cmd_set_scissor,
   cmd_set_line_width, cmd_push_constants, cmd_clear_attachments, cmd_copy_image_to_buffer, queue_wait_idle,
   reset_command_buffer, VK_STRUCTURE_TYPE_APPLICATION_INFO, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
   VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
   VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
   VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
   VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
   VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, VK_STRUCTURE_TYPE_SUBMIT_INFO,
   VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
   VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
   VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
   VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
   VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
   VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
   VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
   VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
   VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
   VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
   VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
   VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
   VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
   VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
   VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
   VK_ACCESS_SHADER_READ_BIT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FILTER_NEAREST, VK_FILTER_LINEAR, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
   VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
   VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
   VK_INDEX_TYPE_UINT16, VK_INDEX_TYPE_UINT32, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR,
   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
   VK_PIPELINE_BIND_POINT_GRAPHICS, VK_SUBPASS_CONTENTS_INLINE, VK_FENCE_CREATE_SIGNALED_BIT,
   VK_COMMAND_BUFFER_LEVEL_PRIMARY, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, VK_QUEUE_GRAPHICS_BIT,
   VK_QUEUE_TRANSFER_BIT, VkApplicationInfo, VkInstanceCreateInfo, VkDeviceQueueCreateInfo, VkDeviceCreateInfo,
   VkSubmitInfo, VkPresentInfoKHR, VkCommandBufferAllocateInfo, VkCommandBufferBeginInfo,
   VkRenderPassBeginInfo, VkMemoryAllocateInfo, VkBufferCreateInfo, VkImageViewCreateInfo, VkImageCreateInfo,
   VkPipelineShaderStageCreateInfo, VkSamplerCreateInfo, VkShaderModuleCreateInfo, VkPipelineLayoutCreateInfo,
   VkDescriptorSetLayoutBinding, VkDescriptorSetLayoutCreateInfo, VkPipelineVertexInputStateCreateInfo,
   VkPipelineInputAssemblyStateCreateInfo, VkPipelineViewportStateCreateInfo,
   VkPipelineRasterizationStateCreateInfo, VkPipelineMultisampleStateCreateInfo,
   VkPipelineDepthStencilStateCreateInfo, VkPipelineColorBlendAttachmentState,
   VkPipelineColorBlendStateCreateInfo, VkPipelineDynamicStateCreateInfo, VkGraphicsPipelineCreateInfo,
   VkFramebufferCreateInfo, VkRenderPassCreateInfo, VkAttachmentDescription, VkSubpassDescription,
   VkSubpassDependency, VkMetalSurfaceCreateInfoEXT, VkXlibSurfaceCreateInfoKHR, VkWaylandSurfaceCreateInfoKHR,
   VkWin32SurfaceCreateInfoKHR, get_memory_type_index, vk_debug_marker_begin, vk_debug_marker_end,
   vk_cmd_begin_debug_utils_label, vk_cmd_end_debug_utils_label
)

use std.core
use std.os.ffi (
   cstr, dlopen, dlsym, tag_native, __call2_ptr, __call3_ptr_u64_ptr_i32,
   __call4_ptr_ptr_ptr_ptr_i32, __call4_ptr_u32_u64_ptr_i32, __call4_ptr_u64_ptr_ptr_i32
)

fn _vk_alloc(int: size): ?ptr {
   def p = zalloc(size)
   if(!p){ panic("vulkan struct allocation failed") }
   p
}

fn _vk_struct(int: size, int: stype): ?ptr {
   def info = _vk_alloc(size)
   store32(info, stype, 0)
   info
}

fn VkImageMemoryBarrierColor(any: bar, any: image, int: src_access, int: dst_access, int: old_layout, int: new_layout, int: base_layer=0, int: layer_count=1, int: level_count=1): ?ptr {
   "Writes a color-image memory barrier into `bar` and returns it."
   memset(bar, 0, 72)
   store32(bar, 45, 0)
   store32(bar, src_access, 16)
   store32(bar, dst_access, 20)
   store32(bar, old_layout, 24)
   store32(bar, new_layout, 28)
   store32(bar, -1, 32)
   store32(bar, -1, 36)
   store64_h(bar, image, 40)
   store32(bar, 1, 48)
   store32(bar, 0, 52)
   store32(bar, level_count, 56)
   store32(bar, base_layer, 60)
   store32(bar, layer_count, 64)
   bar
}

#linux {
   #link "libvulkan.so.1"
   #define VK_USE_PLATFORM_XCB_KHR 1
   #define VK_USE_PLATFORM_XLIB_KHR 1
   #define VK_USE_PLATFORM_WAYLAND_KHR 1
   #include <vulkan/vulkan.h>
   #include <vulkan/vulkan_xcb.h>
   #include <vulkan/vulkan_xlib.h>
   #include <vulkan/vulkan_wayland.h>
} #elif windows {
   #link "vulkan-1.lib"
   #define VK_USE_PLATFORM_WIN32_KHR 1
   #include <vulkan/vulkan.h>
   #include <vulkan/vulkan_win32.h>
} #elif macos {
   #link "libvulkan.dylib"
   #define VK_USE_PLATFORM_METAL_EXT 1
   #include <vulkan/vulkan.h>
   #include <vulkan/vulkan_metal.h>
} #endif
mut _pfn_vkCreateXcbSurfaceKHR = 0
mut _pfn_vkCreateXlibSurfaceKHR = 0
mut _pfn_vkCreateWin32SurfaceKHR = 0
mut _pfn_vkCreateWaylandSurfaceKHR = 0
mut _pfn_vkCreateMetalSurfaceEXT = 0
mut _pfn_vkGetPhysicalDeviceSurfaceSupportKHR = 0
mut _pfn_vkGetPhysicalDeviceSurfaceFormatsKHR = 0
mut _pfn_vkGetPhysicalDeviceSurfacePresentModesKHR = 0
mut _pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = 0
mut _lib_vulkan_loader = 0
mut _pfn_vkGetInstanceProcAddr = 0

fn _vk_native_proc_ptr(any: p): any {
   if(!p){ return 0 }
   if(band(p, 7) == 6){ return p }
   tag_native(p)
}

fn _vk_get_instance_proc_addr_raw(any: inst, any: proc_name): any {
   if(!_pfn_vkGetInstanceProcAddr){
      if(!_lib_vulkan_loader){ _lib_vulkan_loader = dlopen("libvulkan.so.1", 1) }
      if(!_lib_vulkan_loader){ _lib_vulkan_loader = dlopen("libvulkan.so", 1) }
      if(_lib_vulkan_loader){ _pfn_vkGetInstanceProcAddr = dlsym(_lib_vulkan_loader, "vkGetInstanceProcAddr") }
   }
   if(_pfn_vkGetInstanceProcAddr){ return __call2_ptr(_pfn_vkGetInstanceProcAddr, inst, proc_name) }
   vkGetInstanceProcAddr(inst, proc_name)
}

fn vk_get_instance_proc_addr(any: inst, str: name): any {
   "Looks up a Vulkan instance procedure by name."
   def any: proc_name_s = cstr(name)
   def ptr: proc_name = proc_name_s
   _vk_get_instance_proc_addr_raw(inst, proc_name)
}

fn vk_create_instance(any: ci, any: al, any: p): int { vkCreateInstance(ci, al, p) }

fn destroy_instance(any: inst, any: al): any { vkDestroyInstance(inst, al) }

fn enumerate_instance_extension_properties(any: layer, any: c, any: p): int { vkEnumerateInstanceExtensionProperties(layer, c, p) }

fn enumerate_instance_layer_properties(any: c, any: p): int { vkEnumerateInstanceLayerProperties(c, p) }

fn enumerate_physical_devices(any: inst, any: c, any: p): int { vkEnumeratePhysicalDevices(inst, c, p) }

fn get_physical_device_properties(any: pd, any: p): any { vkGetPhysicalDeviceProperties(pd, p) }

fn get_physical_device_memory_properties(any: pd, any: p): any { vkGetPhysicalDeviceMemoryProperties(pd, p) }

fn get_physical_device_queue_family_properties(any: pd, any: c, any: p): any { vkGetPhysicalDeviceQueueFamilyProperties(pd, c, p) }

fn get_physical_device_format_properties(any: pd, int: fmt, any: p): any { vkGetPhysicalDeviceFormatProperties(pd, fmt, p) }

fn get_physical_device_features2(any: pd, any: p): any { vkGetPhysicalDeviceFeatures2(pd, p) }

fn create_device(any: pd, any: ci, any: al, any: p): int { vkCreateDevice(pd, ci, al, p) }

fn destroy_device(any: dev, any: al): any { vkDestroyDevice(dev, al) }

fn get_device_queue(any: dev, int: f, int: idx, any: p): any { vkGetDeviceQueue(dev, f, idx, p) }

fn get_buffer_device_address(any: dev, any: info): int { vkGetBufferDeviceAddress(dev, info) }

fn create_swapchain_khr(any: dev, any: ci, any: al, any: p): int { vkCreateSwapchainKHR(dev, ci, al, p) }

fn destroy_swapchain_khr(any: dev, any: sc, any: al): any { vkDestroySwapchainKHR(dev, sc, al) }

fn get_swapchain_images_khr(any: dev, any: sc, any: c, any: p): int {
   "Queries swapchain image handles with basic invalid-handle guards."
   if(!dev || !sc || !c){ return -1 }
   if(sc == 0x8000000000 || sc == 0xc000000000 || sc == 0x18000000001){ return -1 }
   vkGetSwapchainImagesKHR(dev, sc, c, p)
}

fn acquire_next_image_khr(any: dev, any: sc, int: to, any: sem, any: f, any: p): int { vkAcquireNextImageKHR(dev, sc, to, sem, f, p) }

fn queue_present_khr(any: q, any: p): int { vkQueuePresentKHR(q, p) }

fn create_image_view(any: dev, any: ci, any: al, any: p): int { vkCreateImageView(dev, ci, al, p) }

fn destroy_image_view(any: dev, any: iv, any: al): any { vkDestroyImageView(dev, iv, al) }

fn create_image(any: dev, any: ci, any: al, any: p): int { vkCreateImage(dev, ci, al, p) }

fn destroy_image(any: dev, any: img, any: al): any { vkDestroyImage(dev, img, al) }

fn create_buffer(any: dev, any: ci, any: al, any: p): int { vkCreateBuffer(dev, ci, al, p) }

fn destroy_buffer(any: dev, any: buf, any: al): any { vkDestroyBuffer(dev, buf, al) }

fn get_buffer_memory_requirements(any: dev, any: buf, any: p): any { vkGetBufferMemoryRequirements(dev, buf, p) }

fn bind_buffer_memory(any: dev, any: buf, any: mem, int: off): int { vkBindBufferMemory(dev, buf, mem, off) }

fn map_memory(any: dev, any: mem, int: off, int: sz, int: flags, any: p): int { vkMapMemory(dev, mem, off, sz, flags, p) }

fn unmap_memory(any: dev, any: mem): any { vkUnmapMemory(dev, mem) }

fn create_command_pool(any: dev, any: ci, any: al, any: p): int { vkCreateCommandPool(dev, ci, al, p) }

fn destroy_command_pool(any: dev, any: cp, any: al): any { vkDestroyCommandPool(dev, cp, al) }

fn allocate_command_buffers(any: dev, any: ai, any: p): int { vkAllocateCommandBuffers(dev, ai, p) }

fn begin_command_buffer(any: cb, any: bi): int { vkBeginCommandBuffer(cb, bi) }

fn end_command_buffer(any: cb): int { vkEndCommandBuffer(cb) }

fn cmd_begin_render_pass(any: cb, any: bi, int: c): any { vkCmdBeginRenderPass(cb, bi, c) }

fn cmd_end_render_pass(any: cb): any { vkCmdEndRenderPass(cb) }

fn cmd_bind_pipeline(any: cb, int: bp, any: pipe): any { vkCmdBindPipeline(cb, bp, pipe) }

fn cmd_draw(any: cb, int: vc, int: ic, int: fv, int: fi): any { vkCmdDraw(cb, vc, ic, fv, fi) }

fn cmd_draw_indexed(any: cb, int: ic, int: instc, int: fi, int: vo, int: insto): any { vkCmdDrawIndexed(cb, ic, instc, fi, vo, insto) }

fn cmd_draw_indirect(any: cb, any: buf, int: off, int: count, int: stride): any { vkCmdDrawIndirect(cb, buf, off, count, stride) }

fn cmd_draw_indexed_indirect(any: cb, any: buf, int: off, int: count, int: stride): any { vkCmdDrawIndexedIndirect(cb, buf, off, count, stride) }

fn cmd_dispatch(any: cb, int: x, int: y, int: z): any { vkCmdDispatch(cb, x, y, z) }

fn cmd_dispatch_indirect(any: cb, any: buf, int: off): any { vkCmdDispatchIndirect(cb, buf, off) }

fn cmd_bind_vertex_buffers(any: cb, int: f, int: c, any: p_buf, any: p_off): any { vkCmdBindVertexBuffers(cb, f, c, p_buf, p_off) }

fn cmd_bind_index_buffer(any: cb, any: buf, int: off, int: idx_type): any { vkCmdBindIndexBuffer(cb, buf, off, idx_type) }

fn cmd_pipeline_barrier(any: cb, int: src, int: dst, int: dep, int: mb_c, any: mb, int: bb_c, any: bb, int: ib_c, any: ib): any { vkCmdPipelineBarrier(cb, src, dst, dep, mb_c, mb, bb_c, bb, ib_c, ib) }

fn cmd_copy_buffer(any: cb, any: src, any: dst, int: r_count, any: p_regions): any { vkCmdCopyBuffer(cb, src, dst, r_count, p_regions) }

fn cmd_copy_buffer_to_image(any: cb, any: src, any: dst, int: lyt, int: r_count, any: p_regions): any { vkCmdCopyBufferToImage(cb, src, dst, lyt, r_count, p_regions) }

fn cmd_copy_image(any: cb, any: src_img, int: src_lyt, any: dst_img, int: dst_lyt, int: r_count, any: p_regions): any { vkCmdCopyImage(cb, src_img, src_lyt, dst_img, dst_lyt, r_count, p_regions) }

fn cmd_blit_image(any: cb, any: src_img, int: src_lyt, any: dst_img, int: dst_lyt, int: r_count, any: p_regions, int: filter): any { vkCmdBlitImage(cb, src_img, src_lyt, dst_img, dst_lyt, r_count, p_regions, filter) }

fn create_semaphore(any: dev, any: ci, any: al, any: p): int { vkCreateSemaphore(dev, ci, al, p) }

fn create_fence(any: dev, any: ci, any: al, any: p): int { vkCreateFence(dev, ci, al, p) }

fn destroy_semaphore(any: dev, any: sem, any: al): any { vkDestroySemaphore(dev, sem, al) }

fn destroy_fence(any: dev, any: f, any: al): any { vkDestroyFence(dev, f, al) }

fn wait_for_fences(any: dev, int: c, any: p, int: wait_all, int: tm): int { vkWaitForFences(dev, c, p, wait_all, tm) }

fn reset_fences(any: dev, int: c, any: p): int { vkResetFences(dev, c, p) }

fn queue_submit(any: q, int: c, any: p, any: f): int { vkQueueSubmit(q, c, p, f) }

fn create_render_pass(any: dev, any: ci, any: al, any: p): int { vkCreateRenderPass(dev, ci, al, p) }

fn destroy_render_pass(any: dev, any: rp, any: al): any { vkDestroyRenderPass(dev, rp, al) }

fn create_framebuffer(any: dev, any: ci, any: al, any: p): int { vkCreateFramebuffer(dev, ci, al, p) }

fn destroy_framebuffer(any: dev, any: fb, any: al): any { vkDestroyFramebuffer(dev, fb, al) }

fn create_descriptor_set_layout(any: dev, any: ci, any: al, any: p): int { vkCreateDescriptorSetLayout(dev, ci, al, p) }

fn destroy_descriptor_set_layout(any: dev, any: dsl, any: al): any { vkDestroyDescriptorSetLayout(dev, dsl, al) }

fn create_descriptor_pool(any: dev, any: ci, any: al, any: p): int { vkCreateDescriptorPool(dev, ci, al, p) }

fn destroy_descriptor_pool(any: dev, any: dp, any: al): any { vkDestroyDescriptorPool(dev, dp, al) }

fn allocate_descriptor_sets(any: dev, any: ai, any: p): int { vkAllocateDescriptorSets(dev, ai, p) }

fn update_descriptor_sets(any: dev, int: wc, any: wp, int: cc, any: cp): any { vkUpdateDescriptorSets(dev, wc, wp, cc, cp) }

fn cmd_bind_descriptor_sets(any: cb, int: bp, any: lay, int: f, int: c, any: p_sets, int: od_count, any: p_od): any { vkCmdBindDescriptorSets(cb, bp, lay, f, c, p_sets, od_count, p_od) }

fn create_pipeline_layout(any: dev, any: ci, any: al, any: p): int { vkCreatePipelineLayout(dev, ci, al, p) }

fn destroy_pipeline_layout(any: dev, any: pl, any: al): any { vkDestroyPipelineLayout(dev, pl, al) }

fn create_graphics_pipelines(any: dev, any: cache, int: c, any: p_ci, any: al, any: p): int { vkCreateGraphicsPipelines(dev, cache, c, p_ci, al, p) }

fn create_compute_pipelines(any: dev, any: cache, int: c, any: p_ci, any: al, any: p): int { vkCreateComputePipelines(dev, cache, c, p_ci, al, p) }

fn destroy_pipeline(any: dev, any: p, any: al): any { vkDestroyPipeline(dev, p, al) }

fn create_shader_module(any: dev, any: ci, any: al, any: p): int { vkCreateShaderModule(dev, ci, al, p) }

fn destroy_shader_module(any: dev, any: sm, any: al): any { vkDestroyShaderModule(dev, sm, al) }

fn _vk_instance_proc_cached(any: inst, str: slot_name, str: name): any {
   def any: proc_name_s = cstr(name)
   def ptr: proc_name = proc_name_s
   if(slot_name == "vkCreateXcbSurfaceKHR"){
      if(!_pfn_vkCreateXcbSurfaceKHR){ _pfn_vkCreateXcbSurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateXcbSurfaceKHR
   }
   if(slot_name == "vkCreateXlibSurfaceKHR"){
      if(!_pfn_vkCreateXlibSurfaceKHR){ _pfn_vkCreateXlibSurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateXlibSurfaceKHR
   }
   if(slot_name == "vkCreateWin32SurfaceKHR"){
      if(!_pfn_vkCreateWin32SurfaceKHR){ _pfn_vkCreateWin32SurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateWin32SurfaceKHR
   }
   if(slot_name == "vkCreateWaylandSurfaceKHR"){
      if(!_pfn_vkCreateWaylandSurfaceKHR){ _pfn_vkCreateWaylandSurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateWaylandSurfaceKHR
   }
   if(slot_name == "vkCreateMetalSurfaceEXT"){
      if(!_pfn_vkCreateMetalSurfaceEXT){ _pfn_vkCreateMetalSurfaceEXT = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateMetalSurfaceEXT
   }
   if(slot_name == "vkGetPhysicalDeviceSurfaceSupportKHR"){
      if(!_pfn_vkGetPhysicalDeviceSurfaceSupportKHR){ _pfn_vkGetPhysicalDeviceSurfaceSupportKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfaceSupportKHR
   }
   if(slot_name == "vkGetPhysicalDeviceSurfaceFormatsKHR"){
      if(!_pfn_vkGetPhysicalDeviceSurfaceFormatsKHR){ _pfn_vkGetPhysicalDeviceSurfaceFormatsKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfaceFormatsKHR
   }
   if(slot_name == "vkGetPhysicalDeviceSurfacePresentModesKHR"){
      if(!_pfn_vkGetPhysicalDeviceSurfacePresentModesKHR){ _pfn_vkGetPhysicalDeviceSurfacePresentModesKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfacePresentModesKHR
   }
   if(slot_name == "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"){
      if(!_pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR){ _pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
   }
   0
}

fn _vk_create_surface4(any: inst, any: ci, any: al, any: s, str: name, int: missing=-1): int {
   def f = _vk_instance_proc_cached(inst, name, name)
   if(!f){ return missing }
   def res = __call4_ptr_ptr_ptr_ptr_i32(f, inst, ci, al, s)
   (res & 1) ? (res >> 1) : res
}

fn _vk_surface_call4(any: inst, str: name, any: a, any: b, any: c, any: d): int {
   def f = _vk_instance_proc_cached(inst, name, name)
   if(!f){ return -1 }
   mut res = 0
   if(name == "vkGetPhysicalDeviceSurfaceSupportKHR"){
      res = __call4_ptr_u32_u64_ptr_i32(f, a, b, c, d)
   } else {
      res = __call4_ptr_u64_ptr_ptr_i32(f, a, b, c, d)
   }
   (res & 1) ? (res >> 1) : res
}

fn vk_create_xcb_surface_khr(any: inst, any: ci, any: al, any: s): int {
   "Creates an XCB Vulkan surface on Linux, or returns -1 elsewhere."
   #linux {
      _vk_create_surface4(inst, ci, al, s, "vkCreateXcbSurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_xlib_surface_khr(any: inst, any: ci, any: al, any: s): int {
   "Creates an Xlib Vulkan surface on Linux, or returns -1 elsewhere."
   #linux {
      _vk_create_surface4(inst, ci, al, s, "vkCreateXlibSurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_win32_surface_khr(any: inst, any: ci, any: al, any: s): int {
   "Creates a Win32 Vulkan surface on Windows, or returns -1 elsewhere."
   #windows {
      _vk_create_surface4(inst, ci, al, s, "vkCreateWin32SurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_wayland_surface_khr(any: inst, any: ci, any: al, any: s): int {
   "Creates a Wayland Vulkan surface on Linux, or returns -1 elsewhere."
   #linux {
      _vk_create_surface4(inst, ci, al, s, "vkCreateWaylandSurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_metal_surface_ext(any: instance, any: info, any: allocator, any: surface): int {
   "Creates a Metal Vulkan surface on macOS, or returns -7 elsewhere."
   #macos {
      _vk_create_surface4(instance, info, allocator, surface, "vkCreateMetalSurfaceEXT", -7)
   } #else {
      -7
   } #endif
}

fn vk_get_physical_device_surface_capabilities_khr(any: pd, any: surf, any: p): int { "Reserved direct surface capability hook." 0 }

fn vk_get_physical_device_surface_support_khr(any: pd, int: fam, any: surf, any: p): int { "Reserved direct surface support hook." 0 }

fn vk_get_physical_device_surface_formats_khr(any: pd, any: surf, any: c, any: p): int { "Reserved direct surface formats hook." 0 }

fn vk_get_physical_device_surface_present_modes_khr(any: pd, any: surf, any: c, any: p): int { "Reserved direct surface present-modes hook." 0 }

fn vk_get_physical_device_wayland_presentation_support_khr(any: pd, int: fam, any: dpy): int {
   "Checks Wayland presentation support on Linux, or returns false elsewhere."
   #linux {
      vkGetPhysicalDeviceWaylandPresentationSupportKHR(pd, fam, dpy)
   } #else {
      0
   } #endif
}

fn get_physical_device_surface_support_khr(any: inst, any: pd, int: qf, any: surf, any: p): int {
   "Calls `vkGetPhysicalDeviceSurfaceSupportKHR` through the instance loader."
   if(!inst || !pd || !surf || surf == 0x8000000000 || !p){ return -1 }
   _vk_surface_call4(inst, "vkGetPhysicalDeviceSurfaceSupportKHR", pd, qf, surf, p)
}

fn get_physical_device_surface_formats_khr(any: inst, any: pd, any: surf, any: c, any: p): int {
   "Calls `vkGetPhysicalDeviceSurfaceFormatsKHR` through the instance loader."
   if(!inst || !pd || !surf || surf == 0x8000000000 || !c){ return -1 }
   _vk_surface_call4(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR", pd, surf, c, p)
}

fn get_physical_device_surface_present_modes_khr(any: inst, any: pd, any: surf, any: c, any: p): int {
   "Calls `vkGetPhysicalDeviceSurfacePresentModesKHR` through the instance loader."
   if(!inst || !pd || !surf || surf == 0x8000000000 || !c){ return -1 }
   _vk_surface_call4(inst, "vkGetPhysicalDeviceSurfacePresentModesKHR", pd, surf, c, p)
}

fn get_physical_device_surface_capabilities_khr(any: inst, any: pd, any: surf, any: p): int {
   "Calls `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` through the instance loader."
   if(!inst || !pd || !surf || surf == 0x8000000000 || !p){ return -1 }
   def f = _vk_instance_proc_cached(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")
   if(!f){ return -1 }
   def res = __call3_ptr_u64_ptr_i32(f, pd, surf, p)
   (res & 1) ? (res >> 1) : res
}

fn destroy_surface_khr(any: inst, any: surf, any: al): any { vkDestroySurfaceKHR(inst, surf, al) }

fn allocate_memory(any: dev, any: ai, any: al, any: p): int { vkAllocateMemory(dev, ai, al, p) }

fn free_memory(any: dev, any: mem, any: al): any { vkFreeMemory(dev, mem, al) }

fn bind_image_memory(any: dev, any: img, any: mem, int: off): int { vkBindImageMemory(dev, img, mem, off) }

fn get_image_memory_requirements(any: dev, any: img, any: p): any { vkGetImageMemoryRequirements(dev, img, p) }

fn device_wait_idle(any: dev): int { vkDeviceWaitIdle(dev) }

fn free_command_buffers(any: dev, any: pool, int: count, any: p): any { vkFreeCommandBuffers(dev, pool, count, p) }

fn create_sampler(any: dev, any: ci, any: al, any: p): int { vkCreateSampler(dev, ci, al, p) }

fn destroy_sampler(any: dev, any: sampler, any: al): any { vkDestroySampler(dev, sampler, al) }

fn cmd_set_viewport(any: cb, int: first, int: count, any: p): any { vkCmdSetViewport(cb, first, count, p) }

fn cmd_set_scissor(any: cb, int: first, int: count, any: p): any { vkCmdSetScissor(cb, first, count, p) }

fn cmd_set_line_width(any: cb, f64: width): any { vkCmdSetLineWidth(cb, float(width)) }

fn cmd_push_constants(any: cb, any: lay, int: stages, int: off, int: sz, any: values): any { vkCmdPushConstants(cb, lay, stages, off, sz, values) }

fn cmd_clear_attachments(any: cb, int: count, any: attachments, int: rect_count, any: rects): any { vkCmdClearAttachments(cb, count, attachments, rect_count, rects) }

fn cmd_copy_image_to_buffer(any: cb, any: img, int: lay, any: buf, int: r_count, any: p_regions): any { vkCmdCopyImageToBuffer(cb, img, lay, buf, r_count, p_regions) }

fn queue_wait_idle(any: q): int { vkQueueWaitIdle(q) }

fn reset_command_buffer(any: cb, int: flags): int { vkResetCommandBuffer(cb, flags) }
def VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
def VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
def VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2
def VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3
def VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES = 1000161001
def VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 = 1000059000
def VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES = 1000257000
def VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO = 1000244001
def VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO = 1000161000
def VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT = 0x00000010
def VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT = 0x00000001
def VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT = 0x00000002
def VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT = 0x00000002
def VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT = 0x00000001
def VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT = 0x00000002
def VK_STRUCTURE_TYPE_SUBMIT_INFO = 4
def VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5
def VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO = 1000060000
def VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE = 6
def VK_STRUCTURE_TYPE_BIND_SPARSE_INFO = 7
def VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT = 1000217000
def VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8
def VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9
def VK_STRUCTURE_TYPE_EVENT_CREATE_INFO = 10
def VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO = 11
def VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12
def VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO = 13
def VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14
def VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15
def VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16
def VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO = 17
def VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18
def VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19
def VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20
def VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO = 21
def VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22
def VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23
def VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24
def VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO = 25
def VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26
def VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27
def VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28
def VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29
def VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30
def VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31
def VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32
def VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33
def VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34
def VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35
def VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET = 36
def VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37
def VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38
def VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39
def VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40
def VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42
def VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43
def VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45
def VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000
def VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000
def VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR = 1000006000
def VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000
def VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001
def VK_SHARING_MODE_EXCLUSIVE = 0
def VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010
def VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x00000001
def VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x00000002
def VK_IMAGE_USAGE_SAMPLED_BIT = 0x00000004
def VK_IMAGE_USAGE_STORAGE_BIT = 0x00000008
def VK_IMAGE_ASPECT_COLOR_BIT = 0x00000001
def VK_IMAGE_ASPECT_DEPTH_BIT = 0x00000002
def VK_IMAGE_LAYOUT_UNDEFINED = 0
def VK_IMAGE_LAYOUT_GENERAL = 1
def VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002
def VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2
def VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6
def VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7
def VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5
def VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x00000001
def VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x00002000
def VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400
def VK_PIPELINE_STAGE_TRANSFER_BIT = 0x00001000
def VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x00000080
def VK_ACCESS_COLOR_ATTACHMENT_READ_BIT = 0x00000080
def VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x00000100
def VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT = 0x00000400
def VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT = 0x00000800
def VK_ACCESS_TRANSFER_READ_BIT = 0x00000001
def VK_ACCESS_TRANSFER_WRITE_BIT = 0x00000002
def VK_ACCESS_SHADER_READ_BIT = 0x00000020
def VK_FORMAT_R8G8B8A8_UNORM = 37
def VK_FORMAT_R8G8B8A8_SRGB = 43
def VK_FORMAT_B8G8R8A8_UNORM = 44
def VK_FORMAT_B8G8R8A8_SRGB = 50
def VK_FORMAT_R16G16B16A16_SFLOAT = 97
def VK_FILTER_NEAREST = 0
def VK_FILTER_LINEAR = 1
def VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0
def VK_PRESENT_MODE_IMMEDIATE_KHR = 0
def VK_PRESENT_MODE_MAILBOX_KHR = 1
def VK_PRESENT_MODE_FIFO_KHR = 2
def VK_PRESENT_MODE_FIFO_RELAXED_KHR = 3
def VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x00000080
def VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 0x00000040
def VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 0x00000010
def VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x00000001
def VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x00000002
def VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x00000200
def VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT = 0x00000800
def VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT = 0x00020000
def VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT = 0x00000020
def VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT = 0x00000040
def VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x00000001
def VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002
def VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000004
def VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT = 0x00000002
def VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6
def VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1
def VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7
def VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC = 9
def VK_SHADER_STAGE_VERTEX_BIT = 0x00000001
def VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010
def VK_INDEX_TYPE_UINT16 = 0
def VK_INDEX_TYPE_UINT32 = 1
def VK_ATTACHMENT_LOAD_OP_LOAD = 0
def VK_ATTACHMENT_LOAD_OP_CLEAR = 1
def VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2
def VK_ATTACHMENT_STORE_OP_STORE = 0
def VK_ATTACHMENT_STORE_OP_DONT_CARE = 1
def VK_PIPELINE_BIND_POINT_GRAPHICS = 0
def VK_SUBPASS_CONTENTS_INLINE = 0
def VK_FENCE_CREATE_SIGNALED_BIT = 0x00000001
def VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0
def VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x00000001
def VK_QUEUE_GRAPHICS_BIT = 0x00000001
def VK_QUEUE_TRANSFER_BIT = 0x00000002

fn VkApplicationInfo(any: name, int: version, any: engine, int: engine_v, int: api_v): ?ptr {
   "Creates a VkApplicationInfo structure."
   def info = _vk_struct(48, 0)
   store64_h(info, name, 16) ; pApplicationName (pointer)
   store32(info, version, 24)
   store64_h(info, engine, 32) ; pEngineName (pointer)
   store32(info, engine_v, 40)
   store32(info, api_v, 44) ; apiVersion
   info
}

fn VkInstanceCreateInfo(any: app_info, int: ext_count, any: exts): ?ptr {
   "Creates a VkInstanceCreateInfo structure."
   def info = _vk_struct(64, 1)
   store64_h(info, 0, 8) ; pNext
   store64_h(info, 0, 16) ; flags
   store64_h(info, app_info, 24) ; pApplicationInfo
   store32(info, ext_count, 48) ; enabledExtensionCount
   store64_h(info, exts, 56) ; ppEnabledExtensionNames
   info
}

fn VkDeviceQueueCreateInfo(int: family, int: count, any: priorities): ?ptr {
   "Creates a VkDeviceQueueCreateInfo structure."
   def info = _vk_struct(40, 2)
   store32(info, family, 20)
   store32(info, count, 24)
   store64_h(info, priorities, 32) ; pQueuePriorities (pointer)
   info
}

fn VkDeviceCreateInfo(int: q_count, any: queues, int: ext_count, any: exts, any: features): ?ptr {
   "Creates a VkDeviceCreateInfo structure."
   def info = _vk_struct(72, 3)
   store32(info, q_count, 20)
   store64_h(info, queues, 24) ; pQueueCreateInfos
   store32(info, ext_count, 48)
   store64_h(info, exts, 56) ; ppEnabledExtensionNames
   store64_h(info, features, 64) ; pEnabledFeatures
   info
}

fn VkSubmitInfo(int: wait_count, any: wait_sems, any: wait_stages, int: cb_count, any: cbs, int: signal_count, any: signal_sems): ?ptr {
   "Creates a VkSubmitInfo structure."
   def info = _vk_struct(72, 4)
   store32(info, wait_count, 16)
   store64_h(info, wait_sems, 24)
   store64_h(info, wait_stages, 32)
   store32(info, cb_count, 40)
   store64_h(info, cbs, 48)
   store32(info, signal_count, 56)
   store64_h(info, signal_sems, 64)
   info
}

fn VkPresentInfoKHR(int: wait_count, any: wait_sems, int: sc_count, any: scs, any: indices, any: results): ?ptr {
   "Creates a VkPresentInfoKHR structure."
   def info = _vk_struct(64, 1000001001)
   store32(info, wait_count, 16)
   store64_h(info, wait_sems, 24)
   store32(info, sc_count, 32)
   store64_h(info, scs, 40)
   store64_h(info, indices, 48)
   store64_h(info, results, 56)
   info
}

fn VkCommandBufferAllocateInfo(any: pool, int: level, int: count): ?ptr {
   "Creates a VkCommandBufferAllocateInfo structure."
   def info = _vk_struct(32, 40)
   store64_h(info, pool, 16) ; VkCommandPool (handle)
   store32(info, level, 24)
   store32(info, count, 28)
   info
}

fn VkCommandBufferBeginInfo(int: flags): ?ptr {
   "Creates a VkCommandBufferBeginInfo structure."
   def info = _vk_struct(32, 42)
   store32(info, flags, 16)
   info
}

fn VkRenderPassBeginInfo(any: rp, any: fb, int: x, int: y, int: w, int: h, int: clear_count, any: clears): ?ptr {
   "Creates a VkRenderPassBeginInfo structure."
   def info = _vk_struct(64, 43)
   store64_h(info, rp, 16) ; VkRenderPass (handle)
   store64_h(info, fb, 24) ; VkFramebuffer (handle)
   store32(info, x, 32)
   store32(info, y, 36)
   store32(info, w, 40)
   store32(info, h, 44)
   store32(info, clear_count, 48)
   store64_h(info, clears, 56) ; pClearValues
   info
}

fn VkMemoryAllocateInfo(int: size, int: type_idx): ?ptr {
   "Creates a VkMemoryAllocateInfo structure."
   def info = _vk_struct(32, 5)
   store64_h(info, size, 16)
   store32(info, type_idx, 24)
   info
}

fn VkBufferCreateInfo(int: size, int: usage, int: mode): ?ptr {
   "Creates a VkBufferCreateInfo structure."
   def info = _vk_struct(56, 12)
   store64_h(info, size, 24)
   store32(info, usage, 32)
   store32(info, mode, 36)
   info
}

fn VkImageViewCreateInfo(any: img, int: view_type, int: fmt, any: components_ptr, any: subresource_ptr): ?ptr {
   "Creates a VkImageViewCreateInfo structure."
   def info = _vk_struct(80, 15)
   store64_h(info, img, 24) ; VkImage (handle)
   store32(info, view_type, 32)
   store32(info, fmt, 36)
   if(components_ptr){ memcpy(info + 56, components_ptr, 16) }
   if(subresource_ptr){ memcpy(info + 56, subresource_ptr, 20) } else {
      ; Default subresource range: COLOR aspect, 1 mip, 1 layer
      store32(info, 1, 56) ; aspectMask = COLOR
      store32(info, 0, 60) ; baseMipLevel
      store32(info, 1, 64) ; levelCount
      store32(info, 0, 68) ; baseArrayLayer
      store32(info, 1, 72) ; layerCount
   }
   info
}

fn VkImageCreateInfo(int: flags, int: image_type, int: fmt, int: w, int: h, int: d, int: mips, int: layers, int: samples, int: tiling, int: usage, int: mode, int: lyt): ?ptr {
   "Creates a VkImageCreateInfo structure."
   def info = _vk_struct(88, 14)
   store32(info, flags, 16)
   store32(info, image_type, 20)
   store32(info, fmt, 24)
   store32(info, w, 28)
   store32(info, h, 32)
   store32(info, d, 36)
   store32(info, mips, 40)
   store32(info, layers, 44)
   store32(info, samples, 48)
   store32(info, tiling, 52)
   store32(info, usage, 56)
   store32(info, mode, 60)
   store32(info, lyt, 80) ; initialLayout
   info
}

fn VkPipelineShaderStageCreateInfo(int: stage, any: shader_mod, any: entry_name): ?ptr {
   "Creates a VkPipelineShaderStageCreateInfo structure."
   def info = _vk_struct(48, 18)
   store32(info, stage, 20)
   store64_h(info, shader_mod, 24) ; VkShaderModule (handle)
   store64_h(info, entry_name, 32) ; pName
   info
}

fn VkSamplerCreateInfo(int: mag, int: min_filter, int: m_mode, int: a_u, int: a_v, int: a_w): ?ptr {
   "Creates a VkSamplerCreateInfo structure."
   def info = _vk_struct(80, 31)
   store32(info, mag, 20)
   store32(info, min_filter, 24)
   store32(info, m_mode, 28)
   store32(info, a_u, 32)
   store32(info, a_v, 36)
   store32(info, a_w, 40)
   info
}

fn VkShaderModuleCreateInfo(int: code_size, any: code_ptr): ?ptr {
   "Creates a VkShaderModuleCreateInfo structure."
   def info = _vk_struct(48, 16)
   store64_h(info, code_size, 24)
   store64_h(info, code_ptr, 32)
   info
}

fn VkPipelineLayoutCreateInfo(int: sl_count, any: l_layouts, int: pr_count, any: p_ranges): ?ptr {
   "Creates a VkPipelineLayoutCreateInfo structure."
   def info = _vk_struct(48, 30)
   store32(info, sl_count, 20)
   store64_h(info, l_layouts, 24) ; pSetLayouts
   store32(info, pr_count, 32)
   store64_h(info, p_ranges, 40) ; pPushConstantRanges
   info
}

fn VkDescriptorSetLayoutBinding(int: binding, int: descriptor_type, int: count, int: stages, any: samplers): ?ptr {
   "Creates a VkDescriptorSetLayoutBinding structure."
   def b = _vk_alloc(24)
   store32(b, binding, 0)
   store32(b, descriptor_type, 4)
   store32(b, count, 8)
   store32(b, stages, 12)
   store64_h(b, samplers, 16)
   b
}

fn VkDescriptorSetLayoutCreateInfo(int: b_count, any: bindings): ?ptr {
   "Creates a VkDescriptorSetLayoutCreateInfo structure."
   def info = _vk_struct(32, 32)
   store32(info, b_count, 20)
   store64_h(info, bindings, 24) ; pBindings
   info
}

fn VkPipelineVertexInputStateCreateInfo(int: b_count, any: bindings, int: a_count, any: attrs): ?ptr {
   "Creates a VkPipelineVertexInputStateCreateInfo structure."
   def info = _vk_struct(48, 19)
   store32(info, b_count, 20)
   store64_h(info, bindings, 24) ; pVertexBindingDescriptions
   store32(info, a_count, 32)
   store64_h(info, attrs, 40) ; pVertexAttributeDescriptions
   info
}

fn VkPipelineInputAssemblyStateCreateInfo(int: topo, int: restart): ?ptr {
   "Creates a VkPipelineInputAssemblyStateCreateInfo structure."
   def info = _vk_struct(32, 20)
   store32(info, topo, 20)
   store32(info, restart, 24)
   info
}

fn VkPipelineViewportStateCreateInfo(int: v_count, any: viewports, int: s_count, any: scissors): ?ptr {
   "Creates a VkPipelineViewportStateCreateInfo structure."
   def info = _vk_struct(48, 22)
   store32(info, v_count, 20)
   store64_h(info, viewports, 24) ; pViewports
   store32(info, s_count, 32)
   store64_h(info, scissors, 40) ; pScissors
   info
}

fn VkPipelineRasterizationStateCreateInfo(int: depth_clamp, int: discard, int: polygon_mode, int: cull_mode, int: front, int: depth_bias, f64: db_const, f64: db_clamp, f64: db_slope, f64: line_width): ?ptr {
   "Creates a VkPipelineRasterizationStateCreateInfo structure."
   def info = _vk_struct(64, 23)
   store32(info, depth_clamp, 20)
   store32(info, discard, 24)
   store32(info, polygon_mode, 28)
   store32(info, cull_mode, 32)
   store32(info, front, 36)
   store32(info, depth_bias, 40)
   store32_f32(info, db_const, 44)
   store32_f32(info, db_clamp, 48)
   store32_f32(info, db_slope, 52)
   store32_f32(info, line_width, 56)
   info
}

fn VkPipelineMultisampleStateCreateInfo(int: samples, int: shading, f64: min_shading, any: mask, int: alpha_to_coverage, int: alpha_to_one): ?ptr {
   "Creates a VkPipelineMultisampleStateCreateInfo structure."
   def info = _vk_struct(48, 24)
   store32(info, samples, 20)
   store32(info, shading, 24)
   store32_f32(info, min_shading, 28)
   store64_h(info, mask, 32)
   store32(info, alpha_to_coverage, 40)
   store32(info, alpha_to_one, 44)
   info
}

fn VkPipelineDepthStencilStateCreateInfo(int: depth_test, int: depth_write, int: depth_compare, int: depth_bounds_test, int: stencil_test, any: _front, any: _back, f64: min_depth, f64: max_depth): ?ptr {
   "Creates a VkPipelineDepthStencilStateCreateInfo structure."
   def info = _vk_struct(104, 25)
   store32(info, depth_test, 20)
   store32(info, depth_write, 24)
   store32(info, depth_compare, 28)
   store32(info, depth_bounds_test, 32)
   store32(info, stencil_test, 36)
   ; front (40) back (68)
   store32_f32(info, min_depth, 96)
   store32_f32(info, max_depth, 100)
   info
}

fn VkPipelineColorBlendAttachmentState(int: blend, int: src_color, int: dst_color, int: color_op, int: src_alpha, int: dst_alpha, int: alpha_op, int: mask): ?ptr {
   "Creates a VkPipelineColorBlendAttachmentState structure."
   def b = _vk_alloc(32)
   store32(b, blend, 0)
   store32(b, src_color, 4)
   store32(b, dst_color, 8)
   store32(b, color_op, 12)
   store32(b, src_alpha, 16)
   store32(b, dst_alpha, 20)
   store32(b, alpha_op, 24)
   store32(b, mask, 28)
   b
}

fn VkPipelineColorBlendStateCreateInfo(int: logic_op_enable, int: logic_op, int: a_count, any: attachments, any: _blend_constants): ?ptr {
   "Creates a VkPipelineColorBlendStateCreateInfo structure."
   def info = _vk_struct(56, 26)
   store32(info, logic_op_enable, 20)
   store32(info, logic_op, 24)
   store32(info, a_count, 28)
   store64_h(info, attachments, 32) ; pAttachments
   ; blend_constants (40)
   info
}

fn VkPipelineDynamicStateCreateInfo(int: d_count, any: d_states): ?ptr {
   "Creates a VkPipelineDynamicStateCreateInfo structure."
   def info = _vk_struct(32, 27)
   store32(info, d_count, 20)
   store64_h(info, d_states, 24)
   info
}

fn VkGraphicsPipelineCreateInfo(int: stage_count, any: stages, any: v_input, any: i_assembly, any: tess, any: v_port, any: raster, any: multi, any: depth, any: blend, any: dynamic_state, any: lyt, any: rp, int: subpass, any: base_pipe, int: base_idx): ?ptr {
   "Creates a VkGraphicsPipelineCreateInfo structure."
   def info = _vk_struct(144, 28)
   store32(info, stage_count, 20)
   store64_h(info, stages, 24) ; pStages
   store64_h(info, v_input, 32) ; pVertexInputState
   store64_h(info, i_assembly, 40) ; pInputAssemblyState
   store64_h(info, tess, 48) ; pTessellationState
   store64_h(info, v_port, 56) ; pViewportState
   store64_h(info, raster, 64) ; pRasterizationState
   store64_h(info, multi, 72) ; pMultisampleState
   store64_h(info, depth, 80) ; pDepthStencilState
   store64_h(info, blend, 88) ; pColorBlendState
   store64_h(info, dynamic_state, 96) ; pDynamicState
   store64_h(info, lyt, 104) ; VkPipelineLayout (handle)
   store64_h(info, rp, 112) ; VkRenderPass (handle)
   store32(info, subpass, 120)
   store64_h(info, base_pipe, 128) ; VkPipeline (handle)
   store32(info, base_idx, 136)
   info
}

fn VkFramebufferCreateInfo(any: rp, int: a_count, any: attachments, int: width, int: height, int: layers): ?ptr {
   "Creates a VkFramebufferCreateInfo structure."
   def info = _vk_struct(64, 37)
   store64_h(info, rp, 24) ; VkRenderPass (handle)
   store32(info, a_count, 32)
   store64_h(info, attachments, 40) ; pAttachments
   store32(info, width, 48)
   store32(info, height, 52)
   store32(info, layers, 56)
   info
}

fn VkRenderPassCreateInfo(int: a_count, any: attachments, int: s_count, any: subpasses, int: d_count, any: dependencies): ?ptr {
   "Creates a VkRenderPassCreateInfo structure."
   def info = _vk_struct(64, 38)
   store32(info, a_count, 20)
   store64_h(info, attachments, 24)
   store32(info, s_count, 32)
   store64_h(info, subpasses, 40)
   store32(info, d_count, 48)
   store64_h(info, dependencies, 56)
   info
}

fn VkAttachmentDescription(int: flags, int: fmt, int: samples, int: load, int: store, int: s_load, int: s_store, int: initial, int: final_layout): ?ptr {
   "Creates a VkAttachmentDescription structure."
   def b = _vk_alloc(36)
   store32(b, flags, 0)
   store32(b, fmt, 4)
   store32(b, samples, 8)
   store32(b, load, 12)
   store32(b, store, 16)
   store32(b, s_load, 20)
   store32(b, s_store, 24)
   store32(b, initial, 28)
   store32(b, final_layout, 32)
   b
}

fn VkSubpassDescription(int: flags, int: bind_point, int: in_count, any: in_refs, int: col_count, any: col_refs, any: resolve_refs, any: depth_ref, int: p_count, any: p_refs): ?ptr {
   "Creates a VkSubpassDescription structure."
   def b = _vk_alloc(72)
   store32(b, flags, 0)
   store32(b, bind_point, 4)
   store32(b, in_count, 8)
   store64_h(b, in_refs, 16)
   store32(b, col_count, 24)
   store64_h(b, col_refs, 32)
   store64_h(b, resolve_refs, 40)
   store64_h(b, depth_ref, 48)
   store32(b, p_count, 56)
   store64_h(b, p_refs, 64)
   b
}

fn VkSubpassDependency(int: src_s, int: dst_s, int: src_mask, int: dst_mask, int: src_access, int: dst_access, int: flags): ?ptr {
   "Creates a VkSubpassDependency structure."
   def b = _vk_alloc(28)
   store32(b, src_s, 0)
   store32(b, dst_s, 4)
   store32(b, src_mask, 8)
   store32(b, dst_mask, 12)
   store32(b, src_access, 16)
   store32(b, dst_access, 20)
   store32(b, flags, 24)
   b
}

fn VkMetalSurfaceCreateInfoEXT(any: layer): ?ptr {
   "Creates a VkMetalSurfaceCreateInfoEXT structure."
   def info = _vk_struct(32, 1000217000) ; sType, pNext, flags, pLayer
   ; pNext is at 8, flags at 16 (default 0)
   store64_h(info, layer, 24) ; pLayer
   info
}

fn VkXlibSurfaceCreateInfoKHR(int: flags, any: dpy, any: window): ?ptr {
   "Creates a VkXlibSurfaceCreateInfoKHR structure."
   def info = _vk_struct(48, 1000004000)
   store32(info, flags, 16)
   store64_h(info, dpy, 24)
   store64_h(info, window, 32)
   info
}

fn VkWaylandSurfaceCreateInfoKHR(int: flags, any: display, any: surface): ?ptr {
   "Creates a VkWaylandSurfaceCreateInfoKHR structure."
   def info = _vk_struct(40, 1000006000)
   store32(info, flags, 16)
   store64_h(info, display, 24)
   store64_h(info, surface, 32)
   info
}

fn VkWin32SurfaceCreateInfoKHR(int: flags, any: hinstance, any: hwnd): ?ptr {
   "Creates a VkWin32SurfaceCreateInfoKHR structure."
   def info = _vk_struct(40, 1000009000)
   store32(info, flags, 16)
   store64_h(info, hinstance, 24)
   store64_h(info, hwnd, 32)
   info
}

fn get_memory_type_index(any: pd, int: filter, int: flags): int {
   "Finds a suitable memory type index for the given criteria."
   mut props = _vk_alloc(512)
   get_physical_device_memory_properties(pd, props)
   def count = load32(props, 0)
   mut i = 0
   while(i < count){
      if(band(filter, (1 << i))){
         def type_flags = load32(props, 4 + i * 8 + 0)
         if(band(type_flags, flags) == flags){
            free(props)
            return i
         }
      }
      i += 1
   }
   free(props)
   -1
}

fn vk_debug_marker_begin(any: cb, str: name, any: color=0): int { "Begins a debug marker when backend support is present." 0 }

fn vk_debug_marker_end(any: cb): int { "Ends a debug marker when backend support is present." 0 }

fn vk_cmd_begin_debug_utils_label(any: cb, any: pLabelInfo): int { "Begins a debug utils label when backend support is present." 0 }

fn vk_cmd_end_debug_utils_label(any: cb): int { "Ends a debug utils label when backend support is present." 0 }
