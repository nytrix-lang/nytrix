;; Keywords: ui gfx vulkan
;; Vulkan bindings for Nytrix

module std.ui.gfx.vulkan (
   sys_malloc, sys_free,
   vk_available, vk_init,
   create_instance, destroy_instance,
   enumerate_physical_devices, get_physical_device_properties,
   get_physical_device_queue_family_properties,
   create_device, destroy_device,
   get_device_queue,
   create_xlib_surface_khr, destroy_surface_khr,
   get_physical_device_surface_support_khr,
   get_physical_device_surface_formats_khr,
   get_physical_device_surface_capabilities_khr,
   get_physical_device_surface_present_modes_khr,
   create_swapchain_khr, destroy_swapchain_khr,
   get_swapchain_images_khr, acquire_next_image_khr,
   queue_present_khr,
   create_image_view, destroy_image_view,
   create_shader_module, destroy_shader_module,
   create_pipeline_layout, destroy_pipeline_layout,
   create_render_pass, destroy_render_pass,
   create_graphics_pipelines, destroy_pipeline,
   create_framebuffer, destroy_framebuffer,
   create_command_pool, destroy_command_pool,
   allocate_command_buffers, free_command_buffers,
   begin_command_buffer, end_command_buffer,
   queue_submit,
   cmd_begin_render_pass, cmd_end_render_pass,
   cmd_bind_pipeline, cmd_draw,
   cmd_clear_attachments, cmd_set_viewport, cmd_set_scissor, cmd_push_constants,
   create_semaphore, destroy_semaphore,
   create_fence, destroy_fence,
   wait_for_fences, reset_fences, device_wait_idle,
   
   ;; Constants
   VK_STRUCTURE_TYPE_APPLICATION_INFO,
   VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
   VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
   VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
   VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
   VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
   VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
   VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
   VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
   VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
   VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
   VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
   VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
   VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
   VK_STRUCTURE_TYPE_SUBMIT_INFO,
   VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
   VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
   VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   
   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
   VK_SHARING_MODE_EXCLUSIVE,
   VK_IMAGE_ASPECT_COLOR_BIT,
   VK_VIEWPORT_TYPE_DEFAULT,
   VK_SCISSOR_TYPE_DEFAULT,
   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
   VK_DYNAMIC_STATE_VIEWPORT,
   VK_DYNAMIC_STATE_SCISSOR
)

use std.core *
use std.os.ffi *

mut _lib = 0

fn sys_malloc(n){
   "Auto-generated docstring: sys_malloc."
   if(n <= 0){ return 0 }
   malloc(n)
}

fn sys_free(p){
   "Auto-generated docstring: sys_free."
   if(!p){ return 0 }
   free(p)
   0
}

;; Function pointers
mut _vkCreateInstance = 0
mut _vkDestroyInstance = 0
mut _vkEnumeratePhysicalDevices = 0
mut _vkGetPhysicalDeviceProperties = 0
mut _vkGetPhysicalDeviceQueueFamilyProperties = 0
mut _vkCreateDevice = 0
mut _vkDestroyDevice = 0
mut _vkGetDeviceQueue = 0
mut _vkCreateXlibSurfaceKHR = 0
mut _vkDestroySurfaceKHR = 0
mut _vkGetPhysicalDeviceSurfaceSupportKHR = 0
mut _vkGetPhysicalDeviceSurfaceFormatsKHR = 0
mut _vkGetPhysicalDeviceSurfaceCapabilitiesKHR = 0
mut _vkGetPhysicalDeviceSurfacePresentModesKHR = 0
mut _vkCreateSwapchainKHR = 0
mut _vkDestroySwapchainKHR = 0
mut _vkGetSwapchainImagesKHR = 0
mut _vkAcquireNextImageKHR = 0
mut _vkQueuePresentKHR = 0
mut _vkCreateImageView = 0
mut _vkDestroyImageView = 0
mut _vkCreateShaderModule = 0
mut _vkDestroyShaderModule = 0
mut _vkCreatePipelineLayout = 0
mut _vkDestroyPipelineLayout = 0
mut _vkCreateRenderPass = 0
mut _vkDestroyRenderPass = 0
mut _vkCreateGraphicsPipelines = 0
mut _vkDestroyPipeline = 0
mut _vkCreateFramebuffer = 0
mut _vkDestroyFramebuffer = 0
mut _vkCreateCommandPool = 0
mut _vkDestroyCommandPool = 0
mut _vkAllocateCommandBuffers = 0
mut _vkFreeCommandBuffers = 0
mut _vkBeginCommandBuffer = 0
mut _vkEndCommandBuffer = 0
mut _vkQueueSubmit = 0
mut _vkCmdBeginRenderPass = 0
mut _vkCmdEndRenderPass = 0
mut _vkCmdBindPipeline = 0
mut _vkCmdDraw = 0
mut _vkCmdClearAttachments = 0
mut _vkCmdSetViewport = 0
mut _vkCmdSetScissor = 0
mut _vkCmdPushConstants = 0
mut _vkCreateSemaphore = 0
mut _vkDestroySemaphore = 0
mut _vkCreateFence = 0
mut _vkDestroyFence = 0
mut _vkWaitForFences = 0
mut _vkResetFences = 0
mut _vkDeviceWaitIdle = 0

;; Constants
def VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
def VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
def VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2
def VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3
def VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000
def VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000
def VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15
def VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16
def VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 17
def VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 18
def VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28
def VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37
def VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39
def VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40
def VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42
def VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43
def VK_STRUCTURE_TYPE_SUBMIT_INFO = 4
def VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001
def VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9
def VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8

def VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400
def VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x00000100
def VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010
def VK_FORMAT_B8G8R8A8_UNORM = 44
def VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0
def VK_PRESENT_MODE_FIFO_KHR = 2
def VK_SHARING_MODE_EXCLUSIVE = 0
def VK_IMAGE_ASPECT_COLOR_BIT = 0x00000001
def VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2
def VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002
def VK_DYNAMIC_STATE_VIEWPORT = 0
def VK_DYNAMIC_STATE_SCISSOR = 1

fn vk_available(){
   "Auto-generated docstring: vk_available."
   if(_lib != 0){ return true }
   ;; Ensure RTLD_GLOBAL is used so drivers can find loader symbols
   def flags = RTLD_NOW() | 0x00100 ;; RTLD_GLOBAL
   _lib = dlopen_any("libvulkan.so.1", flags)
   if(!_lib){ _lib = dlopen_any("vulkan", flags) }
   _lib != 0
}

fn vk_init(){
   "Auto-generated docstring: vk_init."
   if(!vk_available()){ return false }
   _vkCreateInstance = dlsym(_lib, "vkCreateInstance")
   _vkDestroyInstance = dlsym(_lib, "vkDestroyInstance")
   _vkEnumeratePhysicalDevices = dlsym(_lib, "vkEnumeratePhysicalDevices")
   _vkGetPhysicalDeviceProperties = dlsym(_lib, "vkGetPhysicalDeviceProperties")
   _vkGetPhysicalDeviceQueueFamilyProperties = dlsym(_lib, "vkGetPhysicalDeviceQueueFamilyProperties")
   _vkCreateDevice = dlsym(_lib, "vkCreateDevice")
   _vkDestroyDevice = dlsym(_lib, "vkDestroyDevice")
   _vkGetDeviceQueue = dlsym(_lib, "vkGetDeviceQueue")
   if(_vkCreateInstance == 0 || _vkEnumeratePhysicalDevices == 0){ return false }
   ;; KHR Extensions (may need vkGetInstanceProcAddr for some)
   _vkCreateXlibSurfaceKHR = dlsym(_lib, "vkCreateXlibSurfaceKHR")
   _vkDestroySurfaceKHR = dlsym(_lib, "vkDestroySurfaceKHR")
   _vkGetPhysicalDeviceSurfaceSupportKHR = dlsym(_lib, "vkGetPhysicalDeviceSurfaceSupportKHR")
   _vkGetPhysicalDeviceSurfaceFormatsKHR = dlsym(_lib, "vkGetPhysicalDeviceSurfaceFormatsKHR")
   _vkGetPhysicalDeviceSurfaceCapabilitiesKHR = dlsym(_lib, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")
   _vkGetPhysicalDeviceSurfacePresentModesKHR = dlsym(_lib, "vkGetPhysicalDeviceSurfacePresentModesKHR")
   _vkCreateSwapchainKHR = dlsym(_lib, "vkCreateSwapchainKHR")
   _vkDestroySwapchainKHR = dlsym(_lib, "vkDestroySwapchainKHR")
   _vkGetSwapchainImagesKHR = dlsym(_lib, "vkGetSwapchainImagesKHR")
   _vkAcquireNextImageKHR = dlsym(_lib, "vkAcquireNextImageKHR")
   _vkQueuePresentKHR = dlsym(_lib, "vkQueuePresentKHR")
   _vkCreateImageView = dlsym(_lib, "vkCreateImageView")
   _vkDestroyImageView = dlsym(_lib, "vkDestroyImageView")
   _vkCreateShaderModule = dlsym(_lib, "vkCreateShaderModule")
   _vkDestroyShaderModule = dlsym(_lib, "vkDestroyShaderModule")
   _vkCreatePipelineLayout = dlsym(_lib, "vkCreatePipelineLayout")
   _vkDestroyPipelineLayout = dlsym(_lib, "vkDestroyPipelineLayout")
   _vkCreateRenderPass = dlsym(_lib, "vkCreateRenderPass")
   _vkDestroyRenderPass = dlsym(_lib, "vkDestroyRenderPass")
   _vkCreateGraphicsPipelines = dlsym(_lib, "vkCreateGraphicsPipelines")
   _vkDestroyPipeline = dlsym(_lib, "vkDestroyPipeline")
   _vkCreateFramebuffer = dlsym(_lib, "vkCreateFramebuffer")
   _vkDestroyFramebuffer = dlsym(_lib, "vkDestroyFramebuffer")
   _vkCreateCommandPool = dlsym(_lib, "vkCreateCommandPool")
   _vkDestroyCommandPool = dlsym(_lib, "vkDestroyCommandPool")
   _vkAllocateCommandBuffers = dlsym(_lib, "vkAllocateCommandBuffers")
   _vkFreeCommandBuffers = dlsym(_lib, "vkFreeCommandBuffers")
   _vkBeginCommandBuffer = dlsym(_lib, "vkBeginCommandBuffer")
   _vkEndCommandBuffer = dlsym(_lib, "vkEndCommandBuffer")
   _vkQueueSubmit = dlsym(_lib, "vkQueueSubmit")
   _vkCmdBeginRenderPass = dlsym(_lib, "vkCmdBeginRenderPass")
   _vkCmdEndRenderPass = dlsym(_lib, "vkCmdEndRenderPass")
   _vkCmdBindPipeline = dlsym(_lib, "vkCmdBindPipeline")
   _vkCmdDraw = dlsym(_lib, "vkCmdDraw")
   _vkCmdClearAttachments = dlsym(_lib, "vkCmdClearAttachments")
   _vkCmdSetViewport = dlsym(_lib, "vkCmdSetViewport")
   _vkCmdSetScissor = dlsym(_lib, "vkCmdSetScissor")
   _vkCmdPushConstants = dlsym(_lib, "vkCmdPushConstants")
   _vkCreateSemaphore = dlsym(_lib, "vkCreateSemaphore")
   _vkDestroySemaphore = dlsym(_lib, "vkDestroySemaphore")
   _vkCreateFence = dlsym(_lib, "vkCreateFence")
   _vkDestroyFence = dlsym(_lib, "vkDestroyFence")
   _vkWaitForFences = dlsym(_lib, "vkWaitForFences")
   _vkResetFences = dlsym(_lib, "vkResetFences")
   _vkDeviceWaitIdle = dlsym(_lib, "vkDeviceWaitIdle")
   true
}

;; Wrapper functions
fn create_instance(pCreateInfo, pAllocator, pInstance){
   "Auto-generated docstring: create_instance."
   _vkCreateInstance( pCreateInfo, pAllocator, pInstance)
}
fn destroy_instance(instance, pAllocator){
   "Auto-generated docstring: destroy_instance."
   _vkDestroyInstance( instance, pAllocator)
}
fn enumerate_physical_devices(instance, pCount, pDevices){
   "Auto-generated docstring: enumerate_physical_devices."
   _vkEnumeratePhysicalDevices( instance, pCount, pDevices)
}
fn get_physical_device_properties(device, pProperties){
   "Auto-generated docstring: get_physical_device_properties."
   _vkGetPhysicalDeviceProperties( device, pProperties)
}
fn get_physical_device_queue_family_properties(device, pCount, pProperties){
   "Auto-generated docstring: get_physical_device_queue_family_properties."
   _vkGetPhysicalDeviceQueueFamilyProperties( device, pCount, pProperties)
}
fn create_device(pd, pCreateInfo, pAllocator, pDevice){
   "Auto-generated docstring: create_device."
   _vkCreateDevice( pd, pCreateInfo, pAllocator, pDevice)
}
fn destroy_device(device, pAllocator){
   "Auto-generated docstring: destroy_device."
   _vkDestroyDevice( device, pAllocator)
}
fn get_device_queue(device, family, idx, pQueue){
   "Auto-generated docstring: get_device_queue."
   _vkGetDeviceQueue( device, family, idx, pQueue)
}

fn create_xlib_surface_khr(instance, pCreateInfo, pAllocator, pSurface){
   "Auto-generated docstring: create_xlib_surface_khr."
   _vkCreateXlibSurfaceKHR( instance, pCreateInfo, pAllocator, pSurface)
}
fn destroy_surface_khr(instance, surface, pAllocator){
   "Auto-generated docstring: destroy_surface_khr."
   _vkDestroySurfaceKHR( instance, surface, pAllocator)
}
fn get_physical_device_surface_support_khr(pd, family, surface, pSupported){
   "Auto-generated docstring: get_physical_device_surface_support_khr."
   _vkGetPhysicalDeviceSurfaceSupportKHR( pd, family, surface, pSupported)
}
fn get_physical_device_surface_formats_khr(pd, surface, pCount, pFormats){
   "Auto-generated docstring: get_physical_device_surface_formats_khr."
   _vkGetPhysicalDeviceSurfaceFormatsKHR( pd, surface, pCount, pFormats)
}
fn get_physical_device_surface_capabilities_khr(pd, surface, pCaps){
   "Auto-generated docstring: get_physical_device_surface_capabilities_khr."
   _vkGetPhysicalDeviceSurfaceCapabilitiesKHR( pd, surface, pCaps)
}
fn get_physical_device_surface_present_modes_khr(pd, surface, pCount, pModes){
   "Auto-generated docstring: get_physical_device_surface_present_modes_khr."
   _vkGetPhysicalDeviceSurfacePresentModesKHR( pd, surface, pCount, pModes)
}

fn create_swapchain_khr(device, pCreateInfo, pAllocator, pSwapchain){
   "Auto-generated docstring: create_swapchain_khr."
   _vkCreateSwapchainKHR( device, pCreateInfo, pAllocator, pSwapchain)
}
fn destroy_swapchain_khr(device, swapchain, pAllocator){
   "Auto-generated docstring: destroy_swapchain_khr."
   _vkDestroySwapchainKHR( device, swapchain, pAllocator)
}
fn get_swapchain_images_khr(device, swapchain, pCount, pImages){
   "Auto-generated docstring: get_swapchain_images_khr."
   _vkGetSwapchainImagesKHR( device, swapchain, pCount, pImages)
}
fn acquire_next_image_khr(device, swapchain, timeout, semaphore, fence, pIndex){
   "Auto-generated docstring: acquire_next_image_khr."
   _vkAcquireNextImageKHR( device, swapchain, timeout, semaphore, fence, pIndex)
}
fn queue_present_khr(queue, pPresentInfo){
   "Auto-generated docstring: queue_present_khr."
   _vkQueuePresentKHR( queue, pPresentInfo)
}

fn create_image_view(device, pCreateInfo, pAllocator, pView){
   "Auto-generated docstring: create_image_view."
   _vkCreateImageView( device, pCreateInfo, pAllocator, pView)
}
fn destroy_image_view(device, view, pAllocator){
   "Auto-generated docstring: destroy_image_view."
   _vkDestroyImageView( device, view, pAllocator)
}
fn create_shader_module(device, pCreateInfo, pAllocator, pModule){
   "Auto-generated docstring: create_shader_module."
   _vkCreateShaderModule( device, pCreateInfo, pAllocator, pModule)
}
fn destroy_shader_module(device, mod_obj, pAllocator){
   "Auto-generated docstring: destroy_shader_module."
   _vkDestroyShaderModule( device, mod_obj, pAllocator)
}
fn create_pipeline_layout(device, pCreateInfo, pAllocator, pLayout){
   "Auto-generated docstring: create_pipeline_layout."
   _vkCreatePipelineLayout( device, pCreateInfo, pAllocator, pLayout)
}
fn destroy_pipeline_layout(device, layout_obj, pAllocator){
   "Auto-generated docstring: destroy_pipeline_layout."
   _vkDestroyPipelineLayout( device, layout_obj, pAllocator)
}
fn create_render_pass(device, pCreateInfo, pAllocator, pPass){
   "Auto-generated docstring: create_render_pass."
   _vkCreateRenderPass( device, pCreateInfo, pAllocator, pPass)
}
fn destroy_render_pass(device, pass, pAllocator){
   "Auto-generated docstring: destroy_render_pass."
   _vkDestroyRenderPass( device, pass, pAllocator)
}
fn create_graphics_pipelines(device, cache, n_count, pCreateInfos, pAllocator, pPipelines){
   "Auto-generated docstring: create_graphics_pipelines."
   _vkCreateGraphicsPipelines( device, cache, n_count, pCreateInfos, pAllocator, pPipelines)
}
fn destroy_pipeline(device, pipeline, pAllocator){
   "Auto-generated docstring: destroy_pipeline."
   _vkDestroyPipeline( device, pipeline, pAllocator)
}
fn create_framebuffer(device, pCreateInfo, pAllocator, pFramebuffer){
   "Auto-generated docstring: create_framebuffer."
   _vkCreateFramebuffer( device, pCreateInfo, pAllocator, pFramebuffer)
}
fn destroy_framebuffer(device, framebuffer, pAllocator){
   "Auto-generated docstring: destroy_framebuffer."
   _vkDestroyFramebuffer( device, framebuffer, pAllocator)
}
fn create_command_pool(device, pCreateInfo, pAllocator, pPool){
   "Auto-generated docstring: create_command_pool."
   _vkCreateCommandPool( device, pCreateInfo, pAllocator, pPool)
}
fn destroy_command_pool(device, pool, pAllocator){
   "Auto-generated docstring: destroy_command_pool."
   _vkDestroyCommandPool( device, pool, pAllocator)
}
fn allocate_command_buffers(device, pAllocateInfo, pBuffers){
   "Auto-generated docstring: allocate_command_buffers."
   _vkAllocateCommandBuffers( device, pAllocateInfo, pBuffers)
}
fn free_command_buffers(device, pool, n_count, pBuffers){
   "Auto-generated docstring: free_command_buffers."
   _vkFreeCommandBuffers( device, pool, n_count, pBuffers)
}
fn begin_command_buffer(buffer, pBeginInfo){
   "Auto-generated docstring: begin_command_buffer."
   _vkBeginCommandBuffer( buffer, pBeginInfo)
}
fn end_command_buffer(buffer){
   "Auto-generated docstring: end_command_buffer."
   _vkEndCommandBuffer( buffer)
}
fn queue_submit(queue, n_count, pSubmits, fence){
   "Auto-generated docstring: queue_submit."
   _vkQueueSubmit( queue, n_count, pSubmits, fence)
}
fn cmd_begin_render_pass(buffer, pBeginInfo, contents){
   "Auto-generated docstring: cmd_begin_render_pass."
   _vkCmdBeginRenderPass( buffer, pBeginInfo, contents)
}
fn cmd_end_render_pass(buffer){
   "Auto-generated docstring: cmd_end_render_pass."
   _vkCmdEndRenderPass( buffer)
}
fn cmd_bind_pipeline(buffer, bindPoint, pipeline){
   "Auto-generated docstring: cmd_bind_pipeline."
   _vkCmdBindPipeline( buffer, bindPoint, pipeline)
}
fn cmd_draw(buffer, vertexCount, instanceCount, firstVertex, firstInstance){
   "Auto-generated docstring: cmd_draw."
   _vkCmdDraw( buffer, vertexCount, instanceCount, firstVertex, firstInstance)
}
fn cmd_clear_attachments(buffer, n_count, pAttachments, rect_count, pRects){
   "Auto-generated docstring: cmd_clear_attachments."
   _vkCmdClearAttachments(buffer, n_count, pAttachments, rect_count, pRects)
}
fn cmd_set_viewport(buffer, first, count, pViewports){
   "Auto-generated docstring: cmd_set_viewport."
   _vkCmdSetViewport(buffer, first, count, pViewports)
}
fn cmd_set_scissor(buffer, first, count, pScissors){
   "Auto-generated docstring: cmd_set_scissor."
   _vkCmdSetScissor(buffer, first, count, pScissors)
}
fn cmd_push_constants(buffer, pipe_layout, stages, offset, size, pValues){
   "Auto-generated docstring: cmd_push_constants."
   _vkCmdPushConstants(buffer, pipe_layout, stages, offset, size, pValues)
}
fn create_semaphore(device, pCreateInfo, pAllocator, pSemaphore){
   "Auto-generated docstring: create_semaphore."
   _vkCreateSemaphore( device, pCreateInfo, pAllocator, pSemaphore)
}
fn destroy_semaphore(device, semaphore, pAllocator){
   "Auto-generated docstring: destroy_semaphore."
   _vkDestroySemaphore( device, semaphore, pAllocator)
}
fn create_fence(device, pCreateInfo, pAllocator, pFence){
   "Auto-generated docstring: create_fence."
   _vkCreateFence( device, pCreateInfo, pAllocator, pFence)
}
fn destroy_fence(device, fence, pAllocator){
   "Auto-generated docstring: destroy_fence."
   _vkDestroyFence( device, fence, pAllocator)
}
fn wait_for_fences(device, n_count, pFences, waitAll, timeout){
   "Auto-generated docstring: wait_for_fences."
   _vkWaitForFences( device, n_count, pFences, waitAll, timeout)
}
fn reset_fences(device, n_count, pFences){
   "Auto-generated docstring: reset_fences."
   _vkResetFences( device, n_count, pFences)
}
fn device_wait_idle(device){
   "Auto-generated docstring: device_wait_idle."
   _vkDeviceWaitIdle(device)
}
