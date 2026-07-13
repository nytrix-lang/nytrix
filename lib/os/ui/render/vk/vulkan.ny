;; Keywords: render vulkan gpu os ui
;; Vulkan bindings for Nytrix
;; References:
;; - std.os.ui.render.vk
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk.vulkan(
   VkImageMemoryBarrierColor, vk_get_instance_proc_addr, vk_create_instance, destroy_instance, vk_result_code,
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
   vk_create_wayland_surface_khr, vk_create_metal_surface_ext, vk_get_physical_device_wayland_presentation_support_khr,
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

fn _vk_alloc(int size) ?ptr {
   def p = zalloc(size)
   if !p { panic("vulkan struct allocation failed") }
   p
}

fn _vk_struct(int size, int stype) ?ptr {
   def info = _vk_alloc(size)
   store32(info, stype, 0)
   info
}

fn VkImageMemoryBarrierColor(any bar, any image, int src_access, int dst_access, int old_layout, int new_layout, int base_layer=0, int layer_count=1, int level_count=1) ?ptr {
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
   #link "vulkan-1.dll"
   #define VK_USE_PLATFORM_WIN32_KHR 1
   #include <vulkan/vulkan.h>
   #include <vulkan/vulkan_win32.h>
} #elif macos {
   #link "libMoltenVK.dylib"
   #define VK_USE_PLATFORM_METAL_EXT 1
   #include <vulkan/vulkan.h>
   #include <vulkan/vulkan_metal.h>
} #endif
extern "" {
   fn vkGetInstanceProcAddr(ptr inst, ptr name) ptr
   fn vkCreateInstance(ptr ci, ptr al, ptr p) i32
   fn vkDestroyInstance(ptr inst, ptr al)
   fn vkEnumerateInstanceExtensionProperties(ptr layer, ptr c, ptr p) i32
   fn vkEnumerateInstanceLayerProperties(ptr c, ptr p) i32
   fn vkEnumeratePhysicalDevices(ptr inst, ptr c, ptr p) i32
   fn vkGetPhysicalDeviceProperties(ptr pd, ptr p)
   fn vkGetPhysicalDeviceMemoryProperties(ptr pd, ptr p)
   fn vkGetPhysicalDeviceQueueFamilyProperties(ptr pd, ptr c, ptr p)
   fn vkGetPhysicalDeviceFormatProperties(ptr pd, i32 fmt, ptr p)
   fn vkGetPhysicalDeviceFeatures2(ptr pd, ptr p)
   fn vkCreateDevice(ptr pd, ptr ci, ptr al, ptr p) i32
   fn vkDestroyDevice(ptr dev, ptr al)
   fn vkGetDeviceQueue(ptr dev, u32 f, u32 idx, ptr p)
   fn vkGetBufferDeviceAddress(ptr dev, ptr info) u64
   fn vkCreateSwapchainKHR(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroySwapchainKHR(ptr dev, ?handle sc, ptr al)
   fn vkGetSwapchainImagesKHR(ptr dev, ?handle sc, ptr c, ptr p) i32
   fn vkAcquireNextImageKHR(ptr dev, ?handle sc, u64 timeout, ?handle sem, ?handle fence, ptr p) i32
   fn vkQueuePresentKHR(ptr q, ptr p) i32
   fn vkCreateImageView(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyImageView(ptr dev, ?handle iv, ptr al)
   fn vkCreateImage(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyImage(ptr dev, ?handle img, ptr al)
   fn vkCreateBuffer(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyBuffer(ptr dev, ?handle buf, ptr al)
   fn vkGetBufferMemoryRequirements(ptr dev, ?handle buf, ptr p)
   fn vkBindBufferMemory(ptr dev, ?handle buf, ?handle mem, u64 off) i32
   fn vkMapMemory(ptr dev, ?handle mem, u64 off, u64 sz, u32 flags, ptr p) i32
   fn vkUnmapMemory(ptr dev, ?handle mem)
   fn vkCreateCommandPool(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyCommandPool(ptr dev, ?handle cp, ptr al)
   fn vkAllocateCommandBuffers(ptr dev, ptr ai, ptr p) i32
   fn vkBeginCommandBuffer(ptr cb, ptr bi) i32
   fn vkEndCommandBuffer(ptr cb) i32
   fn vkCmdBeginRenderPass(ptr cb, ptr bi, i32 contents)
   fn vkCmdEndRenderPass(ptr cb) any
   fn vkCmdBindPipeline(ptr cb, i32 bp, ?handle pipe)
   fn vkCmdDraw(ptr cb, u32 vc, u32 ic, u32 fv, u32 fi)
   fn vkCmdDrawIndexed(ptr cb, u32 ic, u32 instc, u32 fi, i32 vo, u32 insto)
   fn vkCmdDrawIndirect(ptr cb, ?handle buf, u64 off, u32 count, u32 stride)
   fn vkCmdDrawIndexedIndirect(ptr cb, ?handle buf, u64 off, u32 count, u32 stride)
   fn vkCmdDispatch(ptr cb, u32 x, u32 y, u32 z)
   fn vkCmdDispatchIndirect(ptr cb, ?handle buf, u64 off)
   fn vkCmdBindVertexBuffers(ptr cb, u32 first, u32 count, ptr p_buf, ptr p_off)
   fn vkCmdBindIndexBuffer(ptr cb, ?handle buf, u64 off, i32 idx_type)
   fn vkCmdPipelineBarrier(ptr cb, u32 src, u32 dst, u32 dep, u32 mb_c, ptr mb, u32 bb_c, ptr bb, u32 ib_c, ptr ib)
   fn vkCmdCopyBuffer(ptr cb, ?handle src, ?handle dst, u32 r_count, ptr p_regions)
   fn vkCmdCopyBufferToImage(ptr cb, ?handle src, ?handle dst, i32 lyt, u32 r_count, ptr p_regions)
   fn vkCmdCopyImage(ptr cb, ?handle src_img, i32 src_lyt, ?handle dst_img, i32 dst_lyt, u32 r_count, ptr p_regions)
   fn vkCmdBlitImage(ptr cb, ?handle src_img, i32 src_lyt, ?handle dst_img, i32 dst_lyt, u32 r_count, ptr p_regions, i32 filter)
   fn vkCreateSemaphore(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkCreateFence(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroySemaphore(ptr dev, ?handle sem, ptr al)
   fn vkDestroyFence(ptr dev, ?handle f, ptr al)
   fn vkWaitForFences(ptr dev, u32 c, ptr p, u32 wait_all, u64 timeout) i32
   fn vkResetFences(ptr dev, u32 c, ptr p) i32
   fn vkQueueSubmit(ptr q, u32 c, ptr p, ?handle f) i32
   fn vkCreateRenderPass(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyRenderPass(ptr dev, ?handle rp, ptr al)
   fn vkCreateFramebuffer(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyFramebuffer(ptr dev, ?handle fb, ptr al)
   fn vkCreateDescriptorSetLayout(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyDescriptorSetLayout(ptr dev, ?handle dsl, ptr al)
   fn vkCreateDescriptorPool(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyDescriptorPool(ptr dev, ?handle dp, ptr al)
   fn vkAllocateDescriptorSets(ptr dev, ptr ai, ptr p) i32
   fn vkUpdateDescriptorSets(ptr dev, u32 wc, ptr wp, u32 cc, ptr cp)
   fn vkCmdBindDescriptorSets(ptr cb, i32 bp, ?handle lay, u32 f, u32 c, ptr p_sets, u32 od_count, ptr p_od)
   fn vkCreatePipelineLayout(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyPipelineLayout(ptr dev, ?handle pl, ptr al)
   fn vkCreateGraphicsPipelines(ptr dev, ?handle cache, u32 c, ptr p_ci, ptr al, ptr p) i32
   fn vkCreateComputePipelines(ptr dev, ?handle cache, u32 c, ptr p_ci, ptr al, ptr p) i32
   fn vkDestroyPipeline(ptr dev, ?handle p, ptr al)
   fn vkCreateShaderModule(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroyShaderModule(ptr dev, ?handle sm, ptr al)
   fn vkDestroySurfaceKHR(ptr inst, ?handle surf, ptr al)
   fn vkAllocateMemory(ptr dev, ptr ai, ptr al, ptr p) i32
   fn vkFreeMemory(ptr dev, ?handle mem, ptr al)
   fn vkBindImageMemory(ptr dev, ?handle img, ?handle mem, u64 off) i32
   fn vkGetImageMemoryRequirements(ptr dev, ?handle img, ptr p)
   fn vkDeviceWaitIdle(ptr dev) i32
   fn vkFreeCommandBuffers(ptr dev, ptr pool, u32 count, ptr p)
   fn vkCreateSampler(ptr dev, ptr ci, ptr al, ptr p) i32
   fn vkDestroySampler(ptr dev, ?handle sampler, ptr al)
   fn vkCmdSetViewport(ptr cb, u32 first, u32 count, ptr p)
   fn vkCmdSetScissor(ptr cb, u32 first, u32 count, ptr p)
   fn vkCmdSetLineWidth(ptr cb, f32 width)
   fn vkCmdPushConstants(ptr cb, ptr lay, u32 stages, u32 off, u32 sz, ptr values)
   fn vkCmdClearAttachments(ptr cb, u32 count, ptr attachments, u32 rect_count, ptr rects)
   fn vkCmdCopyImageToBuffer(ptr cb, ?handle img, i32 lay, ?handle buf, u32 r_count, ptr p_regions)
   fn vkQueueWaitIdle(ptr q) i32
   fn vkResetCommandBuffer(ptr cb, u32 flags) i32
   fn vkCreateXcbSurfaceKHR(ptr inst, ptr ci, ptr al, ptr s) i32
   fn vkCreateXlibSurfaceKHR(ptr inst, ptr ci, ptr al, ptr s) i32
   fn vkCreateWin32SurfaceKHR(ptr inst, ptr ci, ptr al, ptr s) i32
   fn vkCreateWaylandSurfaceKHR(ptr inst, ptr ci, ptr al, ptr s) i32
   fn vkCreateMetalSurfaceEXT(ptr inst, ptr ci, ptr al, ptr s) i32
   fn vkGetPhysicalDeviceSurfaceSupportKHR(ptr pd, u32 fam, ptr surf, ptr p) i32
   fn vkGetPhysicalDeviceSurfaceFormatsKHR(ptr pd, ptr surf, ptr c, ptr p) i32
   fn vkGetPhysicalDeviceSurfacePresentModesKHR(ptr pd, ptr surf, ptr c, ptr p) i32
   fn vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ptr pd, ptr surf, ptr p) i32
   fn vkGetPhysicalDeviceWaylandPresentationSupportKHR(ptr pd, u32 fam, ptr dpy) i32
}

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

fn _vk_native_proc_ptr(any p) any {
   "Runs the vkGetPhysicalDeviceWaylandPresentationSupportKHR operation."
   if !p { return 0 }
   if band(p, 7) == 6 { return p }
   tag_native(p)
}

fn _vk_get_instance_proc_addr_raw(any inst, any proc_name) any {
   if !_pfn_vkGetInstanceProcAddr {
      #linux {
         if !_lib_vulkan_loader { _lib_vulkan_loader = dlopen("libvulkan.so.1", 1) }
         if !_lib_vulkan_loader { _lib_vulkan_loader = dlopen("libvulkan.so", 1) }
         if _lib_vulkan_loader { _pfn_vkGetInstanceProcAddr = dlsym(_lib_vulkan_loader, "vkGetInstanceProcAddr") }
      }
   }
   if _pfn_vkGetInstanceProcAddr { return __call2_ptr(_pfn_vkGetInstanceProcAddr, inst, proc_name) }
   vkGetInstanceProcAddr(inst, proc_name)
}

fn vk_get_instance_proc_addr(any inst, str name) any {
   "Looks up a Vulkan instance procedure by name."
   def any proc_name_s = cstr(name)
   def ptr proc_name = proc_name_s
   _vk_get_instance_proc_addr_raw(inst, proc_name)
}

fn vk_create_instance(any ci, any al, any p) int { vkCreateInstance(ci, al, p) }

fn destroy_instance(any inst, any al) any { vkDestroyInstance(inst, al) }

fn enumerate_instance_extension_properties(any layer, any c, any p) int { vkEnumerateInstanceExtensionProperties(layer, c, p) }

fn enumerate_instance_layer_properties(any c, any p) int { vkEnumerateInstanceLayerProperties(c, p) }

fn enumerate_physical_devices(any inst, any c, any p) int { vkEnumeratePhysicalDevices(inst, c, p) }

fn get_physical_device_properties(any pd, any p) any { vkGetPhysicalDeviceProperties(pd, p) }

fn get_physical_device_memory_properties(any pd, any p) any { vkGetPhysicalDeviceMemoryProperties(pd, p) }

fn get_physical_device_queue_family_properties(any pd, any c, any p) any { vkGetPhysicalDeviceQueueFamilyProperties(pd, c, p) }

fn get_physical_device_format_properties(any pd, int fmt, any p) any { vkGetPhysicalDeviceFormatProperties(pd, fmt, p) }

fn get_physical_device_features2(any pd, any p) any { vkGetPhysicalDeviceFeatures2(pd, p) }

fn create_device(any pd, any ci, any al, any p) int { vkCreateDevice(pd, ci, al, p) }

fn destroy_device(any dev, any al) any { vkDestroyDevice(dev, al) }

fn get_device_queue(any dev, int f, int idx, any p) any { vkGetDeviceQueue(dev, f, idx, p) }

fn get_buffer_device_address(any dev, any info) int { vkGetBufferDeviceAddress(dev, info) }

fn create_swapchain_khr(any dev, any ci, any al, any p) int { vkCreateSwapchainKHR(dev, ci, al, p) }

;; Releases the swapchain khr.
fn destroy_swapchain_khr(any dev, ?handle sc, any al) any {
   if !sc { return 0 }
   vkDestroySwapchainKHR(dev, sc, al)
}

fn get_swapchain_images_khr(any dev, ?handle sc, any c, any p) int {
   "Queries swapchain image handles with basic invalid-handle guards."
   if !dev || !sc || !c { return -1 }
   if sc == 0x8000000000 || sc == 0xc000000000 || sc == 0x18000000001 { return -1 }
   vkGetSwapchainImagesKHR(dev, sc, c, p)
}

;; Returns the result of the `acquire_next_image_khr` operation.
fn acquire_next_image_khr(any dev, ?handle sc, int to, ?handle sem, ?handle f, any p) int {
   if !dev || !sc || !p { return -1 }
   vkAcquireNextImageKHR(dev, sc, to, sem, f, p)
}

fn queue_present_khr(any q, any p) int { vkQueuePresentKHR(q, p) }

fn create_image_view(any dev, any ci, any al, any p) int { vkCreateImageView(dev, ci, al, p) }

;; Releases the image view.
fn destroy_image_view(any dev, ?handle iv, any al) any {
   if !iv { return 0 }
   vkDestroyImageView(dev, iv, al)
}

fn create_image(any dev, any ci, any al, any p) int { vkCreateImage(dev, ci, al, p) }

;; Releases the image.
fn destroy_image(any dev, ?handle img, any al) any {
   if !img { return 0 }
   vkDestroyImage(dev, img, al)
}

fn create_buffer(any dev, any ci, any al, any p) int { vkCreateBuffer(dev, ci, al, p) }

;; Releases the buffer.
fn destroy_buffer(any dev, ?handle buf, any al) any {
   if !buf { return 0 }
   vkDestroyBuffer(dev, buf, al)
}

;; Returns the buffer memory requirements.
fn get_buffer_memory_requirements(any dev, ?handle buf, any p) any {
   if !buf || !p { return 0 }
   vkGetBufferMemoryRequirements(dev, buf, p)
}

;; Writes the buffer memory and returns the result.
fn bind_buffer_memory(any dev, ?handle buf, ?handle mem, int off) int {
   if !buf || !mem { return -1 }
   vkBindBufferMemory(dev, buf, mem, off)
}

;; Returns the result of the `map_memory` operation.
fn map_memory(any dev, ?handle mem, int off, int sz, int flags, any p) int {
   if !mem || !p { return -1 }
   vkMapMemory(dev, mem, off, sz, flags, p)
}

;; Releases the memory.
fn unmap_memory(any dev, ?handle mem) any {
   if !mem { return 0 }
   vkUnmapMemory(dev, mem)
}

fn create_command_pool(any dev, any ci, any al, any p) int { vkCreateCommandPool(dev, ci, al, p) }

;; Releases the command pool.
fn destroy_command_pool(any dev, ?handle cp, any al) any {
   if !cp { return 0 }
   vkDestroyCommandPool(dev, cp, al)
}

fn allocate_command_buffers(any dev, any ai, any p) int { vkAllocateCommandBuffers(dev, ai, p) }

fn begin_command_buffer(any cb, any bi) int { vkBeginCommandBuffer(cb, bi) }

fn end_command_buffer(any cb) int { vkEndCommandBuffer(cb) }

fn cmd_begin_render_pass(any cb, any bi, int c) any { vkCmdBeginRenderPass(cb, bi, c) }

fn cmd_end_render_pass(any cb) any { vkCmdEndRenderPass(cb) }

;; Records the bind pipeline command in a command buffer.
fn cmd_bind_pipeline(any cb, int bp, ?handle pipe) any {
   if !pipe { return 0 }
   vkCmdBindPipeline(cb, bp, pipe)
}

fn cmd_draw(any cb, int vc, int ic, int fv, int fi) any { vkCmdDraw(cb, vc, ic, fv, fi) }

fn cmd_draw_indexed(any cb, int ic, int instc, int fi, int vo, int insto) any { vkCmdDrawIndexed(cb, ic, instc, fi, vo, insto) }

;; Records the draw indirect command in a command buffer.
fn cmd_draw_indirect(any cb, ?handle buf, int off, int count, int stride) any {
   if !buf { return 0 }
   vkCmdDrawIndirect(cb, buf, off, count, stride)
}

;; Records the draw indexed indirect command in a command buffer.
fn cmd_draw_indexed_indirect(any cb, ?handle buf, int off, int count, int stride) any {
   if !buf { return 0 }
   vkCmdDrawIndexedIndirect(cb, buf, off, count, stride)
}

fn cmd_dispatch(any cb, int x, int y, int z) any { vkCmdDispatch(cb, x, y, z) }

;; Records the dispatch indirect command in a command buffer.
fn cmd_dispatch_indirect(any cb, ?handle buf, int off) any {
   if !buf { return 0 }
   vkCmdDispatchIndirect(cb, buf, off)
}

fn cmd_bind_vertex_buffers(any cb, int f, int c, any p_buf, any p_off) any { vkCmdBindVertexBuffers(cb, f, c, p_buf, p_off) }

;; Records the bind index buffer command in a command buffer.
fn cmd_bind_index_buffer(any cb, ?handle buf, int off, int idx_type) any {
   if !buf { return 0 }
   vkCmdBindIndexBuffer(cb, buf, off, idx_type)
}

fn cmd_pipeline_barrier(any cb, int src, int dst, int dep, int mb_c, any mb, int bb_c, any bb, int ib_c, any ib) any { vkCmdPipelineBarrier(cb, src, dst, dep, mb_c, mb, bb_c, bb, ib_c, ib) }

;; Records the copy buffer command in a command buffer.
fn cmd_copy_buffer(any cb, ?handle src, ?handle dst, int r_count, any p_regions) any {
   if !src || !dst { return 0 }
   vkCmdCopyBuffer(cb, src, dst, r_count, p_regions)
}

;; Records the copy buffer to image command in a command buffer.
fn cmd_copy_buffer_to_image(any cb, ?handle src, ?handle dst, int lyt, int r_count, any p_regions) any {
   if !src || !dst { return 0 }
   vkCmdCopyBufferToImage(cb, src, dst, lyt, r_count, p_regions)
}

;; Records the copy image command in a command buffer.
fn cmd_copy_image(any cb, ?handle src_img, int src_lyt, ?handle dst_img, int dst_lyt, int r_count, any p_regions) any {
   if !src_img || !dst_img { return 0 }
   vkCmdCopyImage(cb, src_img, src_lyt, dst_img, dst_lyt, r_count, p_regions)
}

;; Records the blit image command in a command buffer.
fn cmd_blit_image(any cb, ?handle src_img, int src_lyt, ?handle dst_img, int dst_lyt, int r_count, any p_regions, int filter) any {
   if !src_img || !dst_img { return 0 }
   vkCmdBlitImage(cb, src_img, src_lyt, dst_img, dst_lyt, r_count, p_regions, filter)
}

fn create_semaphore(any dev, any ci, any al, any p) int { vkCreateSemaphore(dev, ci, al, p) }

fn create_fence(any dev, any ci, any al, any p) int { vkCreateFence(dev, ci, al, p) }

;; Releases the semaphore.
fn destroy_semaphore(any dev, ?handle sem, any al) any {
   if !sem { return 0 }
   vkDestroySemaphore(dev, sem, al)
}

;; Releases the fence.
fn destroy_fence(any dev, ?handle f, any al) any {
   if !f { return 0 }
   vkDestroyFence(dev, f, al)
}

fn wait_for_fences(any dev, int c, any p, int wait_all, int tm) int { vkWaitForFences(dev, c, p, wait_all, tm) }

fn reset_fences(any dev, int c, any p) int { vkResetFences(dev, c, p) }

;; Returns the result of the `queue_submit` operation.
fn queue_submit(any q, int c, any p, ?handle f) int {
   if !q || !p { return -1 }
   vkQueueSubmit(q, c, p, f)
}

fn create_render_pass(any dev, any ci, any al, any p) int { vkCreateRenderPass(dev, ci, al, p) }

;; Releases the render pass.
fn destroy_render_pass(any dev, ?handle rp, any al) any {
   if !rp { return 0 }
   vkDestroyRenderPass(dev, rp, al)
}

fn create_framebuffer(any dev, any ci, any al, any p) int { vkCreateFramebuffer(dev, ci, al, p) }

;; Releases the framebuffer.
fn destroy_framebuffer(any dev, ?handle fb, any al) any {
   if !fb { return 0 }
   vkDestroyFramebuffer(dev, fb, al)
}

fn create_descriptor_set_layout(any dev, any ci, any al, any p) int { vkCreateDescriptorSetLayout(dev, ci, al, p) }

;; Releases the descriptor set layout.
fn destroy_descriptor_set_layout(any dev, ?handle dsl, any al) any {
   if !dsl { return 0 }
   vkDestroyDescriptorSetLayout(dev, dsl, al)
}

fn create_descriptor_pool(any dev, any ci, any al, any p) int { vkCreateDescriptorPool(dev, ci, al, p) }

;; Releases the descriptor pool.
fn destroy_descriptor_pool(any dev, ?handle dp, any al) any {
   if !dp { return 0 }
   vkDestroyDescriptorPool(dev, dp, al)
}

fn allocate_descriptor_sets(any dev, any ai, any p) int { vkAllocateDescriptorSets(dev, ai, p) }

fn update_descriptor_sets(any dev, int wc, any wp, int cc, any cp) any { vkUpdateDescriptorSets(dev, wc, wp, cc, cp) }

;; Records the bind descriptor sets command in a command buffer.
fn cmd_bind_descriptor_sets(any cb, int bp, ?handle lay, int f, int c, any p_sets, int od_count, any p_od) any {
   if !lay { return 0 }
   vkCmdBindDescriptorSets(cb, bp, lay, f, c, p_sets, od_count, p_od)
}

fn create_pipeline_layout(any dev, any ci, any al, any p) int { vkCreatePipelineLayout(dev, ci, al, p) }

;; Releases the pipeline layout.
fn destroy_pipeline_layout(any dev, ?handle pl, any al) any {
   if !pl { return 0 }
   vkDestroyPipelineLayout(dev, pl, al)
}

fn create_graphics_pipelines(any dev, ?handle cache, int c, any p_ci, any al, any p) int { vkCreateGraphicsPipelines(dev, cache, c, p_ci, al, p) }

fn create_compute_pipelines(any dev, ?handle cache, int c, any p_ci, any al, any p) int { vkCreateComputePipelines(dev, cache, c, p_ci, al, p) }

;; Releases the pipeline.
fn destroy_pipeline(any dev, ?handle p, any al) any {
   if !p { return 0 }
   vkDestroyPipeline(dev, p, al)
}

fn create_shader_module(any dev, any ci, any al, any p) int { vkCreateShaderModule(dev, ci, al, p) }

;; Releases the shader module.
fn destroy_shader_module(any dev, ?handle sm, any al) any {
   if !sm { return 0 }
   vkDestroyShaderModule(dev, sm, al)
}

fn _vk_instance_proc_cached(any inst, str slot_name, str name) any {
   def any proc_name_s = cstr(name)
   def ptr proc_name = proc_name_s
   if slot_name == "vkCreateXcbSurfaceKHR" {
      if !_pfn_vkCreateXcbSurfaceKHR { _pfn_vkCreateXcbSurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateXcbSurfaceKHR
   }
   if slot_name == "vkCreateXlibSurfaceKHR" {
      if !_pfn_vkCreateXlibSurfaceKHR { _pfn_vkCreateXlibSurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateXlibSurfaceKHR
   }
   if slot_name == "vkCreateWin32SurfaceKHR" {
      if !_pfn_vkCreateWin32SurfaceKHR { _pfn_vkCreateWin32SurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateWin32SurfaceKHR
   }
   if slot_name == "vkCreateWaylandSurfaceKHR" {
      if !_pfn_vkCreateWaylandSurfaceKHR { _pfn_vkCreateWaylandSurfaceKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateWaylandSurfaceKHR
   }
   if slot_name == "vkCreateMetalSurfaceEXT" {
      if !_pfn_vkCreateMetalSurfaceEXT { _pfn_vkCreateMetalSurfaceEXT = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkCreateMetalSurfaceEXT
   }
   if slot_name == "vkGetPhysicalDeviceSurfaceSupportKHR" {
      if !_pfn_vkGetPhysicalDeviceSurfaceSupportKHR { _pfn_vkGetPhysicalDeviceSurfaceSupportKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfaceSupportKHR
   }
   if slot_name == "vkGetPhysicalDeviceSurfaceFormatsKHR" {
      if !_pfn_vkGetPhysicalDeviceSurfaceFormatsKHR { _pfn_vkGetPhysicalDeviceSurfaceFormatsKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfaceFormatsKHR
   }
   if slot_name == "vkGetPhysicalDeviceSurfacePresentModesKHR" {
      if !_pfn_vkGetPhysicalDeviceSurfacePresentModesKHR { _pfn_vkGetPhysicalDeviceSurfacePresentModesKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfacePresentModesKHR
   }
   if slot_name == "vkGetPhysicalDeviceSurfaceCapabilitiesKHR" {
      if !_pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR { _pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = _vk_native_proc_ptr(_vk_get_instance_proc_addr_raw(inst, proc_name)) }
      return _pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
   }
   0
}

fn vk_result_code(any res) int {
   "Runs the result code operation."
   int(res)
}

fn _vk_create_surface4(any inst, any ci, any al, any s, str name, int missing=-1) int {
   def f = _vk_instance_proc_cached(inst, name, name)
   if !f { return missing }
   def res = __call4_ptr_ptr_ptr_ptr_i32(f, inst, ci, al, s)
   vk_result_code(res)
}

fn _vk_surface_call4(any inst, str name, any a, any b, any c, any d) int {
   def f = _vk_instance_proc_cached(inst, name, name)
   if !f { return -1 }
   mut res = 0
   if name == "vkGetPhysicalDeviceSurfaceSupportKHR" {
      res = __call4_ptr_u32_u64_ptr_i32(f, a, b, c, d)
   } else {
      res = __call4_ptr_u64_ptr_ptr_i32(f, a, b, c, d)
   }
   vk_result_code(res)
}

fn vk_create_xcb_surface_khr(any inst, any ci, any al, any s) int {
   "Creates an XCB Vulkan surface on Linux, or returns -1 elsewhere."
   #linux {
      _vk_create_surface4(inst, ci, al, s, "vkCreateXcbSurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_xlib_surface_khr(any inst, any ci, any al, any s) int {
   "Creates an Xlib Vulkan surface on Linux, or returns -1 elsewhere."
   #linux {
      _vk_create_surface4(inst, ci, al, s, "vkCreateXlibSurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_win32_surface_khr(any inst, any ci, any al, any s) int {
   "Creates a Win32 Vulkan surface on Windows, or returns -1 elsewhere."
   #windows {
      _vk_create_surface4(inst, ci, al, s, "vkCreateWin32SurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_wayland_surface_khr(any inst, any ci, any al, any s) int {
   "Creates a Wayland Vulkan surface on Linux, or returns -1 elsewhere."
   #linux {
      _vk_create_surface4(inst, ci, al, s, "vkCreateWaylandSurfaceKHR")
   } #else {
      -1
   } #endif
}

fn vk_create_metal_surface_ext(any instance, any info, any allocator, any surface) int {
   "Creates a Metal Vulkan surface on macOS, or returns -7 elsewhere."
   #macos {
      _vk_create_surface4(instance, info, allocator, surface, "vkCreateMetalSurfaceEXT", -7)
   } #else {
      -7
   } #endif
}

fn vk_get_physical_device_surface_capabilities_khr(any pd, any surf, any p) int { "Reserved direct surface capability hook." 0 }

fn vk_get_physical_device_surface_support_khr(any pd, int fam, any surf, any p) int { "Reserved direct surface support hook." 0 }

fn vk_get_physical_device_surface_formats_khr(any pd, any surf, any c, any p) int { "Reserved direct surface formats hook." 0 }

fn vk_get_physical_device_surface_present_modes_khr(any pd, any surf, any c, any p) int { "Reserved direct surface present-modes hook." 0 }

fn vk_get_physical_device_wayland_presentation_support_khr(any pd, int fam, any dpy) int {
   "Checks Wayland presentation support on Linux, or returns false elsewhere."
   #linux {
      vkGetPhysicalDeviceWaylandPresentationSupportKHR(pd, fam, dpy)
   } #else {
      0
   } #endif
}

fn get_physical_device_surface_support_khr(any inst, any pd, int qf, any surf, any p) int {
   "Calls `vkGetPhysicalDeviceSurfaceSupportKHR` through the instance loader."
   if !inst || !pd || !surf || surf == 0x8000000000 || !p { return -1 }
   _vk_surface_call4(inst, "vkGetPhysicalDeviceSurfaceSupportKHR", pd, qf, surf, p)
}

fn get_physical_device_surface_formats_khr(any inst, any pd, any surf, any c, any p) int {
   "Calls `vkGetPhysicalDeviceSurfaceFormatsKHR` through the instance loader."
   if !inst || !pd || !surf || surf == 0x8000000000 || !c { return -1 }
   _vk_surface_call4(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR", pd, surf, c, p)
}

fn get_physical_device_surface_present_modes_khr(any inst, any pd, any surf, any c, any p) int {
   "Calls `vkGetPhysicalDeviceSurfacePresentModesKHR` through the instance loader."
   if !inst || !pd || !surf || surf == 0x8000000000 || !c { return -1 }
   _vk_surface_call4(inst, "vkGetPhysicalDeviceSurfacePresentModesKHR", pd, surf, c, p)
}

fn get_physical_device_surface_capabilities_khr(any inst, any pd, any surf, any p) int {
   "Calls `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` through the instance loader."
   if !inst || !pd || !surf || surf == 0x8000000000 || !p { return -1 }
   def f = _vk_instance_proc_cached(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")
   if !f { return -1 }
   def res = __call3_ptr_u64_ptr_i32(f, pd, surf, p)
   vk_result_code(res)
}

;; Releases the surface khr.
fn destroy_surface_khr(any inst, ?handle surf, any al) any {
   if !surf { return 0 }
   vkDestroySurfaceKHR(inst, surf, al)
}

fn allocate_memory(any dev, any ai, any al, any p) int { vkAllocateMemory(dev, ai, al, p) }

;; Returns the result of the `free_memory` operation.
fn free_memory(any dev, ?handle mem, any al) any {
   if !mem { return 0 }
   vkFreeMemory(dev, mem, al)
}

;; Writes the image memory and returns the result.
fn bind_image_memory(any dev, ?handle img, ?handle mem, int off) int {
   if !img || !mem { return -1 }
   vkBindImageMemory(dev, img, mem, off)
}

;; Returns the image memory requirements.
fn get_image_memory_requirements(any dev, ?handle img, any p) any {
   if !img || !p { return 0 }
   vkGetImageMemoryRequirements(dev, img, p)
}

fn device_wait_idle(any dev) int { vkDeviceWaitIdle(dev) }

fn free_command_buffers(any dev, any pool, int count, any p) any { vkFreeCommandBuffers(dev, pool, count, p) }

fn create_sampler(any dev, any ci, any al, any p) int { vkCreateSampler(dev, ci, al, p) }

;; Releases the sampler.
fn destroy_sampler(any dev, ?handle sampler, any al) any {
   if !sampler { return 0 }
   vkDestroySampler(dev, sampler, al)
}

fn cmd_set_viewport(any cb, int first, int count, any p) any { vkCmdSetViewport(cb, first, count, p) }

fn cmd_set_scissor(any cb, int first, int count, any p) any { vkCmdSetScissor(cb, first, count, p) }

fn cmd_set_line_width(any cb, f64 width) any { vkCmdSetLineWidth(cb, float(width)) }

fn cmd_push_constants(any cb, any lay, int stages, int off, int sz, any values) any { vkCmdPushConstants(cb, lay, stages, off, sz, values) }

fn cmd_clear_attachments(any cb, int count, any attachments, int rect_count, any rects) any { vkCmdClearAttachments(cb, count, attachments, rect_count, rects) }

;; Records the copy image to buffer command in a command buffer.
fn cmd_copy_image_to_buffer(any cb, ?handle img, int lay, ?handle buf, int r_count, any p_regions) any {
   if !img || !buf { return 0 }
   vkCmdCopyImageToBuffer(cb, img, lay, buf, r_count, p_regions)
}

fn queue_wait_idle(any q) int { vkQueueWaitIdle(q) }

fn reset_command_buffer(any cb, int flags) int { vkResetCommandBuffer(cb, flags) }
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
def VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO = 41
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
def VK_ACCESS_TRANSFER_READ_BIT = 0x00000800
def VK_ACCESS_TRANSFER_WRITE_BIT = 0x00001000
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

fn VkApplicationInfo(any name, int version, any engine, int engine_v, int api_v) ?ptr {
   "Creates a VkApplicationInfo structure."
   def info = _vk_struct(48, 0)
   store64_h(info, name, 16)
   store32(info, version, 24)
   store64_h(info, engine, 32)
   store32(info, engine_v, 40)
   store32(info, api_v, 44)
   info
}

fn VkInstanceCreateInfo(any app_info, int ext_count, any exts) ?ptr {
   "Creates a VkInstanceCreateInfo structure."
   def info = _vk_struct(64, 1)
   store64_h(info, 0, 8)
   store64_h(info, 0, 16)
   store64_h(info, app_info, 24)
   store32(info, ext_count, 48)
   store64_h(info, exts, 56)
   info
}

fn VkDeviceQueueCreateInfo(int family, int count, any priorities) ?ptr {
   "Creates a VkDeviceQueueCreateInfo structure."
   def info = _vk_struct(40, 2)
   store32(info, family, 20)
   store32(info, count, 24)
   store64_h(info, priorities, 32)
   info
}

fn VkDeviceCreateInfo(int q_count, any queues, int ext_count, any exts, any features) ?ptr {
   "Creates a VkDeviceCreateInfo structure."
   def info = _vk_struct(72, 3)
   store32(info, q_count, 20)
   store64_h(info, queues, 24)
   store32(info, ext_count, 48)
   store64_h(info, exts, 56)
   store64_h(info, features, 64)
   info
}

fn VkSubmitInfo(int wait_count, any wait_sems, any wait_stages, int cb_count, any cbs, int signal_count, any signal_sems) ?ptr {
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

fn VkPresentInfoKHR(int wait_count, any wait_sems, int sc_count, any scs, any indices, any results) ?ptr {
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

fn VkCommandBufferAllocateInfo(any pool, int level, int count) ?ptr {
   "Creates a VkCommandBufferAllocateInfo structure."
   def info = _vk_struct(32, 40)
   store64_h(info, pool, 16)
   store32(info, level, 24)
   store32(info, count, 28)
   info
}

fn VkCommandBufferBeginInfo(int flags) ?ptr {
   "Creates a VkCommandBufferBeginInfo structure."
   def info = _vk_struct(32, 42)
   store32(info, flags, 16)
   info
}

fn VkRenderPassBeginInfo(any rp, any fb, int x, int y, int w, int h, int clear_count, any clears) ?ptr {
   "Creates a VkRenderPassBeginInfo structure."
   def info = _vk_struct(64, 43)
   store64_h(info, rp, 16)
   store64_h(info, fb, 24)
   store32(info, x, 32)
   store32(info, y, 36)
   store32(info, w, 40)
   store32(info, h, 44)
   store32(info, clear_count, 48)
   store64_h(info, clears, 56)
   info
}

fn VkMemoryAllocateInfo(int size, int type_idx) ?ptr {
   "Creates a VkMemoryAllocateInfo structure."
   def info = _vk_struct(32, 5)
   store64_h(info, size, 16)
   store32(info, type_idx, 24)
   info
}

fn VkBufferCreateInfo(int size, int usage, int mode) ?ptr {
   "Creates a VkBufferCreateInfo structure."
   def info = _vk_struct(56, 12)
   store64_h(info, size, 24)
   store32(info, usage, 32)
   store32(info, mode, 36)
   info
}

fn VkImageViewCreateInfo(any img, int view_type, int fmt, any components_ptr, any subresource_ptr) ?ptr {
   "Creates a VkImageViewCreateInfo structure."
   def info = _vk_struct(80, 15)
   store64_h(info, img, 24)
   store32(info, view_type, 32)
   store32(info, fmt, 36)
   if components_ptr { memcpy(info + 40, components_ptr, 16) }
   if subresource_ptr { memcpy(info + 56, subresource_ptr, 20) } else {
      store32(info, 1, 56)
      store32(info, 0, 60)
      store32(info, 1, 64)
      store32(info, 0, 68)
      store32(info, 1, 72)
   }
   info
}

fn VkImageCreateInfo(int flags, int image_type, int fmt, int w, int h, int d, int mips, int layers, int samples, int tiling, int usage, int mode, int lyt) ?ptr {
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
   store32(info, lyt, 80)
   info
}

fn VkPipelineShaderStageCreateInfo(int stage, any shader_mod, any entry_name) ?ptr {
   "Creates a VkPipelineShaderStageCreateInfo structure."
   def info = _vk_struct(48, 18)
   store32(info, stage, 20)
   store64_h(info, shader_mod, 24)
   store64_h(info, entry_name, 32)
   info
}

fn VkSamplerCreateInfo(int mag, int min_filter, int m_mode, int a_u, int a_v, int a_w) ?ptr {
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

fn VkShaderModuleCreateInfo(int code_size, any code_ptr) ?ptr {
   "Creates a VkShaderModuleCreateInfo structure."
   def info = _vk_struct(48, 16)
   store64_h(info, code_size, 24)
   store64_h(info, code_ptr, 32)
   info
}

fn VkPipelineLayoutCreateInfo(int sl_count, any l_layouts, int pr_count, any p_ranges) ?ptr {
   "Creates a VkPipelineLayoutCreateInfo structure."
   def info = _vk_struct(48, 30)
   store32(info, sl_count, 20)
   store64_h(info, l_layouts, 24)
   store32(info, pr_count, 32)
   store64_h(info, p_ranges, 40)
   info
}

fn VkDescriptorSetLayoutBinding(int binding, int descriptor_type, int count, int stages, any samplers) ?ptr {
   "Creates a VkDescriptorSetLayoutBinding structure."
   def b = _vk_alloc(24)
   store32(b, binding, 0)
   store32(b, descriptor_type, 4)
   store32(b, count, 8)
   store32(b, stages, 12)
   store64_h(b, samplers, 16)
   b
}

fn VkDescriptorSetLayoutCreateInfo(int b_count, any bindings) ?ptr {
   "Creates a VkDescriptorSetLayoutCreateInfo structure."
   def info = _vk_struct(32, 32)
   store32(info, b_count, 20)
   store64_h(info, bindings, 24)
   info
}

fn VkPipelineVertexInputStateCreateInfo(int b_count, any bindings, int a_count, any attrs) ?ptr {
   "Creates a VkPipelineVertexInputStateCreateInfo structure."
   def info = _vk_struct(48, 19)
   store32(info, b_count, 20)
   store64_h(info, bindings, 24)
   store32(info, a_count, 32)
   store64_h(info, attrs, 40)
   info
}

fn VkPipelineInputAssemblyStateCreateInfo(int topo, int restart) ?ptr {
   "Creates a VkPipelineInputAssemblyStateCreateInfo structure."
   def info = _vk_struct(32, 20)
   store32(info, topo, 20)
   store32(info, restart, 24)
   info
}

fn VkPipelineViewportStateCreateInfo(int v_count, any viewports, int s_count, any scissors) ?ptr {
   "Creates a VkPipelineViewportStateCreateInfo structure."
   def info = _vk_struct(48, 22)
   store32(info, v_count, 20)
   store64_h(info, viewports, 24)
   store32(info, s_count, 32)
   store64_h(info, scissors, 40)
   info
}

fn VkPipelineRasterizationStateCreateInfo(int depth_clamp, int discard, int polygon_mode, int cull_mode, int front, int depth_bias, f64 db_const, f64 db_clamp, f64 db_slope, f64 line_width) ?ptr {
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

fn VkPipelineMultisampleStateCreateInfo(int samples, int shading, f64 min_shading, any mask, int alpha_to_coverage, int alpha_to_one) ?ptr {
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

fn VkPipelineDepthStencilStateCreateInfo(int depth_test, int depth_write, int depth_compare, int depth_bounds_test, int stencil_test, any _front, any _back, f64 min_depth, f64 max_depth) ?ptr {
   "Creates a VkPipelineDepthStencilStateCreateInfo structure."
   def info = _vk_struct(104, 25)
   store32(info, depth_test, 20)
   store32(info, depth_write, 24)
   store32(info, depth_compare, 28)
   store32(info, depth_bounds_test, 32)
   store32(info, stencil_test, 36)
   store32_f32(info, min_depth, 96)
   store32_f32(info, max_depth, 100)
   info
}

fn VkPipelineColorBlendAttachmentState(int blend, int src_color, int dst_color, int color_op, int src_alpha, int dst_alpha, int alpha_op, int mask) ?ptr {
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

fn VkPipelineColorBlendStateCreateInfo(int logic_op_enable, int logic_op, int a_count, any attachments, any _blend_constants) ?ptr {
   "Creates a VkPipelineColorBlendStateCreateInfo structure."
   def info = _vk_struct(56, 26)
   store32(info, logic_op_enable, 20)
   store32(info, logic_op, 24)
   store32(info, a_count, 28)
   store64_h(info, attachments, 32)
   info
}

fn VkPipelineDynamicStateCreateInfo(int d_count, any d_states) ?ptr {
   "Creates a VkPipelineDynamicStateCreateInfo structure."
   def info = _vk_struct(32, 27)
   store32(info, d_count, 20)
   store64_h(info, d_states, 24)
   info
}

fn VkGraphicsPipelineCreateInfo(int stage_count, any stages, any v_input, any i_assembly, any tess, any v_port, any raster, any multi, any depth, any blend, any dynamic_state, any lyt, any rp, int subpass, any base_pipe, int base_idx) ?ptr {
   "Creates a VkGraphicsPipelineCreateInfo structure."
   def info = _vk_struct(144, 28)
   store32(info, stage_count, 20)
   store64_h(info, stages, 24)
   store64_h(info, v_input, 32)
   store64_h(info, i_assembly, 40)
   store64_h(info, tess, 48)
   store64_h(info, v_port, 56)
   store64_h(info, raster, 64)
   store64_h(info, multi, 72)
   store64_h(info, depth, 80)
   store64_h(info, blend, 88)
   store64_h(info, dynamic_state, 96)
   store64_h(info, lyt, 104)
   store64_h(info, rp, 112)
   store32(info, subpass, 120)
   store64_h(info, base_pipe, 128)
   store32(info, base_idx, 136)
   info
}

fn VkFramebufferCreateInfo(any rp, int a_count, any attachments, int width, int height, int layers) ?ptr {
   "Creates a VkFramebufferCreateInfo structure."
   def info = _vk_struct(64, 37)
   store64_h(info, rp, 24)
   store32(info, a_count, 32)
   store64_h(info, attachments, 40)
   store32(info, width, 48)
   store32(info, height, 52)
   store32(info, layers, 56)
   info
}

fn VkRenderPassCreateInfo(int a_count, any attachments, int s_count, any subpasses, int d_count, any dependencies) ?ptr {
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

fn VkAttachmentDescription(int flags, int fmt, int samples, int load, int store, int s_load, int s_store, int initial, int final_layout) ?ptr {
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

fn VkSubpassDescription(int flags, int bind_point, int in_count, any in_refs, int col_count, any col_refs, any resolve_refs, any depth_ref, int p_count, any p_refs) ?ptr {
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

fn VkSubpassDependency(int src_s, int dst_s, int src_mask, int dst_mask, int src_access, int dst_access, int flags) ?ptr {
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

fn VkMetalSurfaceCreateInfoEXT(any layer) ?ptr {
   "Creates a VkMetalSurfaceCreateInfoEXT structure."
   def info = _vk_struct(32, 1000217000)
   store64_h(info, layer, 24)
   info
}

fn VkXlibSurfaceCreateInfoKHR(int flags, any dpy, any window) ?ptr {
   "Creates a VkXlibSurfaceCreateInfoKHR structure."
   def info = _vk_struct(48, 1000004000)
   store32(info, flags, 16)
   store64_h(info, dpy, 24)
   store64_h(info, window, 32)
   info
}

fn VkWaylandSurfaceCreateInfoKHR(int flags, any display, any surface) ?ptr {
   "Creates a VkWaylandSurfaceCreateInfoKHR structure."
   def info = _vk_struct(40, 1000006000)
   store32(info, flags, 16)
   store64_h(info, display, 24)
   store64_h(info, surface, 32)
   info
}

fn VkWin32SurfaceCreateInfoKHR(int flags, any hinstance, any hwnd) ?ptr {
   "Creates a VkWin32SurfaceCreateInfoKHR structure."
   def info = _vk_struct(40, 1000009000)
   store32(info, flags, 16)
   store64_h(info, hinstance, 24)
   store64_h(info, hwnd, 32)
   info
}

fn get_memory_type_index(any pd, int filter, int flags) int {
   "Finds a suitable memory type index for the given criteria."
   mut props = _vk_alloc(512)
   get_physical_device_memory_properties(pd, props)
   def count = load32(props, 0)
   mut i = 0
   while i < count {
      if band(filter, (1 << i)) {
         def type_flags = load32(props, 4 + i * 8 + 0)
         if band(type_flags, flags) == flags {
            free(props)
            return i
         }
      }
      i += 1
   }
   free(props)
   -1
}

fn vk_debug_marker_begin(any cb, str name, any color=0) int { "Begins a debug marker when backend support is present." 0 }

fn vk_debug_marker_end(any cb) int { "Ends a debug marker when backend support is present." 0 }

fn vk_cmd_begin_debug_utils_label(any cb, any pLabelInfo) int { "Begins a debug utils label when backend support is present." 0 }

fn vk_cmd_end_debug_utils_label(any cb) int { "Ends a debug utils label when backend support is present." 0 }

#main {
   fn expect_stype(any p, int stype, str label) any {
      assert(load32(p, 0) == stype, label)
   }
   def app = VkApplicationInfo(0, 1, 0, 2, 0x00400000)
   expect_stype(app, VK_STRUCTURE_TYPE_APPLICATION_INFO, "vulkan application info stype")
   assert(load32(app, 24) == 1 && load32(app, 40) == 2, "vulkan application versions")
   free(app)
   def buf = VkBufferCreateInfo(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE)
   expect_stype(buf, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, "vulkan buffer stype")
   assert(load64_h(buf, 24) == 4096 && load32(buf, 32) == (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT), "vulkan buffer fields")
   free(buf)
   def img = VkImageCreateInfo(0, 2, VK_FORMAT_R8G8B8A8_UNORM, 64, 32, 1, 1, 1, 1, 0, VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_LAYOUT_UNDEFINED)
   expect_stype(img, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, "vulkan image stype")
   assert(load32(img, 20) == 2 && load32(img, 28) == 64 && load32(img, 32) == 32 && load32(img, 80) == VK_IMAGE_LAYOUT_UNDEFINED, "vulkan image fields")
   free(img)
   def barrier = zalloc(72)
   assert(VkImageMemoryBarrierColor(barrier, 0x1234, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) == barrier, "vulkan barrier returns input")
   expect_stype(barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, "vulkan barrier stype")
   assert(load32(barrier, 48) == VK_IMAGE_ASPECT_COLOR_BIT && load32(barrier, 56) == 1 && load32(barrier, 64) == 1, "vulkan barrier ranges")
   free(barrier)
   def binding = VkDescriptorSetLayoutBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, 0)
   assert(load32(binding, 0) == 3 && load32(binding, 4) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && load32(binding, 12) == VK_SHADER_STAGE_VERTEX_BIT, "vulkan descriptor binding")
   free(binding)
   def attachment = VkAttachmentDescription(0, VK_FORMAT_B8G8R8A8_UNORM, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
   assert(load32(attachment, 4) == VK_FORMAT_B8G8R8A8_UNORM && load32(attachment, 12) == VK_ATTACHMENT_LOAD_OP_CLEAR && load32(attachment, 32) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, "vulkan attachment")
   free(attachment)
   def dep = VkSubpassDependency(0, 1, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0)
   assert(load32(dep, 0) == 0 && load32(dep, 4) == 1 && load32(dep, 20) == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, "vulkan subpass dependency")
   free(dep)
   assert(get_swapchain_images_khr(0, 0, 0, 0) == -1, "vulkan swapchain guard")
   assert(vk_get_physical_device_surface_support_khr(0, 0, 0, 0) == 0, "vulkan surface support stub")
   assert(vk_debug_marker_begin(0, "probe") == 0 && vk_debug_marker_end(0) == 0, "vulkan debug marker stubs")
   assert(vk_cmd_begin_debug_utils_label(0, 0) == 0 && vk_cmd_end_debug_utils_label(0) == 0, "vulkan debug utils stubs")
   print("✓ std.os.ui.render.vk.vulkan self-test passed")
}
