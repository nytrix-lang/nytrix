;; Keywords: ui gfx vulkan
;; Vulkan bindings for Nytrix

module std.ui.gfx.vulkan (
   sys_malloc, sys_free,
   vk_available, vk_init,
   create_instance, destroy_instance,
   enumerate_physical_devices, get_physical_device_properties,
   get_physical_device_memory_properties,
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
   create_image, destroy_image,
   get_image_memory_requirements,
   bind_image_memory,
   create_sampler, destroy_sampler,
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
   
   ;; New additions for buffers and memory
   create_buffer, destroy_buffer,
   get_buffer_memory_requirements,
   allocate_memory, free_memory,
   bind_buffer_memory, map_memory, unmap_memory,
   cmd_bind_vertex_buffers,

   create_descriptor_set_layout, destroy_descriptor_set_layout,
   create_descriptor_pool, destroy_descriptor_pool,
   allocate_descriptor_sets, free_descriptor_sets,
   update_descriptor_sets, cmd_bind_descriptor_sets,

   ;; Constants
   VK_STRUCTURE_TYPE_APPLICATION_INFO,
   VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
   VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
   VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
   VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
   VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
   VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
   VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
   VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
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
   VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
   VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
   VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
   VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
   VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
   VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
   VK_SHADER_STAGE_FRAGMENT_BIT,
   
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
   VK_DYNAMIC_STATE_SCISSOR,

   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   VK_IMAGE_USAGE_SAMPLED_BIT,
   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
   VK_IMAGE_LAYOUT_UNDEFINED,
   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
)

use std.core *
use std.os.ffi *

mut _lib = 0

fn sys_malloc(n){
   "Vulkan wrapper: sys_malloc."
   if(n <= 0){ return 0 }
   malloc(n)
}

fn sys_free(p){
   "Vulkan wrapper: sys_free."
   if(!p){ return 0 }
   free(p)
   0
}

;; Function pointers
mut _vkCreateInstance = 0
mut _vkDestroyInstance = 0
mut _vkEnumeratePhysicalDevices = 0
mut _vkGetPhysicalDeviceProperties = 0
mut _vkGetPhysicalDeviceMemoryProperties = 0
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
mut _vkCreateImage = 0
mut _vkDestroyImage = 0
mut _vkGetImageMemoryRequirements = 0
mut _vkBindImageMemory = 0
mut _vkCreateImageView = 0
mut _vkDestroyImageView = 0
mut _vkCreateSampler = 0
mut _vkDestroySampler = 0
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

;; New function pointers
mut _vkCreateBuffer = 0
mut _vkDestroyBuffer = 0
mut _vkGetBufferMemoryRequirements = 0
mut _vkAllocateMemory = 0
mut _vkFreeMemory = 0
mut _vkBindBufferMemory = 0
mut _vkMapMemory = 0
mut _vkUnmapMemory = 0
mut _vkCmdBindVertexBuffers = 0

mut _vkCreateDescriptorSetLayout = 0
mut _vkDestroyDescriptorSetLayout = 0
mut _vkCreateDescriptorPool = 0
mut _vkDestroyDescriptorPool = 0
mut _vkAllocateDescriptorSets = 0
mut _vkFreeDescriptorSets = 0
mut _vkUpdateDescriptorSets = 0
mut _vkCmdBindDescriptorSets = 0

;; Constants
def VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
def VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
def VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2
def VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3
def VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000
def VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000
def VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14
def VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15
def VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31
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
def VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12
def VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5
def VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32
def VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33
def VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34
def VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35

def VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1
def VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010

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

def VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x00000080
def VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x00000001
def VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x00000010
def VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x00000002
def VK_IMAGE_USAGE_SAMPLED_BIT = 0x00000004
def VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002
def VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000004
def VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x00000001
def VK_IMAGE_LAYOUT_UNDEFINED = 0
def VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7
def VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5

fn vk_available(){
   "Vulkan wrapper: vk_available."
   if(_lib != 0){ return true }
   ;; Ensure RTLD_GLOBAL is used so drivers can find loader symbols
   def flags = RTLD_NOW() | 0x00100 ;; RTLD_GLOBAL
   _lib = dlopen_any("libvulkan.so.1", flags)
   if(!_lib){ _lib = dlopen_any("vulkan", flags) }
   _lib != 0
}

fn vk_init(){
   "Vulkan wrapper: vk_init."
   if(!vk_available()){ return false }
   _vkCreateInstance = dlsym(_lib, "vkCreateInstance")
   _vkDestroyInstance = dlsym(_lib, "vkDestroyInstance")
   _vkEnumeratePhysicalDevices = dlsym(_lib, "vkEnumeratePhysicalDevices")
   _vkGetPhysicalDeviceProperties = dlsym(_lib, "vkGetPhysicalDeviceProperties")
   _vkGetPhysicalDeviceMemoryProperties = dlsym(_lib, "vkGetPhysicalDeviceMemoryProperties")
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
   _vkCreateImage = dlsym(_lib, "vkCreateImage")
   _vkDestroyImage = dlsym(_lib, "vkDestroyImage")
   _vkGetImageMemoryRequirements = dlsym(_lib, "vkGetImageMemoryRequirements")
   _vkBindImageMemory = dlsym(_lib, "vkBindImageMemory")
   _vkCreateImageView = dlsym(_lib, "vkCreateImageView")
   _vkDestroyImageView = dlsym(_lib, "vkDestroyImageView")
   _vkCreateSampler = dlsym(_lib, "vkCreateSampler")
   _vkDestroySampler = dlsym(_lib, "vkDestroySampler")
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

   ;; Load new symbols
   _vkCreateBuffer = dlsym(_lib, "vkCreateBuffer")
   _vkDestroyBuffer = dlsym(_lib, "vkDestroyBuffer")
   _vkGetBufferMemoryRequirements = dlsym(_lib, "vkGetBufferMemoryRequirements")
   _vkAllocateMemory = dlsym(_lib, "vkAllocateMemory")
   _vkFreeMemory = dlsym(_lib, "vkFreeMemory")
   _vkBindBufferMemory = dlsym(_lib, "vkBindBufferMemory")
   _vkMapMemory = dlsym(_lib, "vkMapMemory")
   _vkUnmapMemory = dlsym(_lib, "vkUnmapMemory")
   _vkCmdBindVertexBuffers = dlsym(_lib, "vkCmdBindVertexBuffers")

   _vkCreateDescriptorSetLayout = dlsym(_lib, "vkCreateDescriptorSetLayout")
   _vkDestroyDescriptorSetLayout = dlsym(_lib, "vkDestroyDescriptorSetLayout")
   _vkCreateDescriptorPool = dlsym(_lib, "vkCreateDescriptorPool")
   _vkDestroyDescriptorPool = dlsym(_lib, "vkDestroyDescriptorPool")
   _vkAllocateDescriptorSets = dlsym(_lib, "vkAllocateDescriptorSets")
   _vkFreeDescriptorSets = dlsym(_lib, "vkFreeDescriptorSets")
   _vkUpdateDescriptorSets = dlsym(_lib, "vkUpdateDescriptorSets")
   _vkCmdBindDescriptorSets = dlsym(_lib, "vkCmdBindDescriptorSets")

   true
}

;; Wrapper functions
fn create_instance(pCreateInfo, pAllocator, pInstance){
   "Vulkan wrapper: create_instance."
   _vkCreateInstance( pCreateInfo, pAllocator, pInstance)
}
fn destroy_instance(instance, pAllocator){
   "Vulkan wrapper: destroy_instance."
   _vkDestroyInstance( instance, pAllocator)
}
fn enumerate_physical_devices(instance, pCount, pDevices){
   "Vulkan wrapper: enumerate_physical_devices."
   _vkEnumeratePhysicalDevices( instance, pCount, pDevices)
}
fn get_physical_device_properties(device, pProperties){
   "Vulkan wrapper: get_physical_device_properties."
   _vkGetPhysicalDeviceProperties( device, pProperties)
}
fn get_physical_device_memory_properties(device, pProperties){
   "Vulkan wrapper: get_physical_device_memory_properties."
   _vkGetPhysicalDeviceMemoryProperties( device, pProperties)
}
fn get_physical_device_queue_family_properties(device, pCount, pProperties){
   "Vulkan wrapper: get_physical_device_queue_family_properties."
   _vkGetPhysicalDeviceQueueFamilyProperties( device, pCount, pProperties)
}
fn create_device(pd, pCreateInfo, pAllocator, pDevice){
   "Vulkan wrapper: create_device."
   _vkCreateDevice( pd, pCreateInfo, pAllocator, pDevice)
}
fn destroy_device(device, pAllocator){
   "Vulkan wrapper: destroy_device."
   _vkDestroyDevice( device, pAllocator)
}
fn get_device_queue(device, family, idx, pQueue){
   "Vulkan wrapper: get_device_queue."
   _vkGetDeviceQueue( device, family, idx, pQueue)
}

fn create_xlib_surface_khr(instance, pCreateInfo, pAllocator, pSurface){
   "Vulkan wrapper: create_xlib_surface_khr."
   _vkCreateXlibSurfaceKHR( instance, pCreateInfo, pAllocator, pSurface)
}
fn destroy_surface_khr(instance, surface, pAllocator){
   "Vulkan wrapper: destroy_surface_khr."
   _vkDestroySurfaceKHR( instance, surface, pAllocator)
}
fn get_physical_device_surface_support_khr(pd, family, surface, pSupported){
   "Vulkan wrapper: get_physical_device_surface_support_khr."
   _vkGetPhysicalDeviceSurfaceSupportKHR( pd, family, surface, pSupported)
}
fn get_physical_device_surface_formats_khr(pd, surface, pCount, pFormats){
   "Vulkan wrapper: get_physical_device_surface_formats_khr."
   _vkGetPhysicalDeviceSurfaceFormatsKHR( pd, surface, pCount, pFormats)
}
fn get_physical_device_surface_capabilities_khr(pd, surface, pCaps){
   "Vulkan wrapper: get_physical_device_surface_capabilities_khr."
   _vkGetPhysicalDeviceSurfaceCapabilitiesKHR( pd, surface, pCaps)
}
fn get_physical_device_surface_present_modes_khr(pd, surface, pCount, pModes){
   "Vulkan wrapper: get_physical_device_surface_present_modes_khr."
   _vkGetPhysicalDeviceSurfacePresentModesKHR( pd, surface, pCount, pModes)
}

fn create_swapchain_khr(device, pCreateInfo, pAllocator, pSwapchain){
   "Vulkan wrapper: create_swapchain_khr."
   _vkCreateSwapchainKHR( device, pCreateInfo, pAllocator, pSwapchain)
}
fn destroy_swapchain_khr(device, swapchain, pAllocator){
   "Vulkan wrapper: destroy_swapchain_khr."
   _vkDestroySwapchainKHR( device, swapchain, pAllocator)
}
fn get_swapchain_images_khr(device, swapchain, pCount, pImages){
   "Vulkan wrapper: get_swapchain_images_khr."
   _vkGetSwapchainImagesKHR( device, swapchain, pCount, pImages)
}
fn acquire_next_image_khr(device, swapchain, timeout, semaphore, fence, pIndex){
   "Vulkan wrapper: acquire_next_image_khr."
   _vkAcquireNextImageKHR( device, swapchain, timeout, semaphore, fence, pIndex)
}
fn queue_present_khr(queue, pPresentInfo){
   "Vulkan wrapper: queue_present_khr."
   _vkQueuePresentKHR( queue, pPresentInfo)
}

fn create_image(device, pCreateInfo, pAllocator, pImage){
   "Vulkan wrapper: create_image."
   _vkCreateImage( device, pCreateInfo, pAllocator, pImage)
}
fn destroy_image(device, image, pAllocator){
   "Vulkan wrapper: destroy_image."
   _vkDestroyImage( device, image, pAllocator)
}
fn get_image_memory_requirements(device, image, pMemoryRequirements){
   "Vulkan wrapper: get_image_memory_requirements."
   _vkGetImageMemoryRequirements( device, image, pMemoryRequirements)
}
fn bind_image_memory(device, image, memory, memoryOffset){
   "Vulkan wrapper: bind_image_memory."
   _vkBindImageMemory( device, image, memory, memoryOffset)
}
fn create_image_view(device, pCreateInfo, pAllocator, pView){
   "Vulkan wrapper: create_image_view."
   _vkCreateImageView( device, pCreateInfo, pAllocator, pView)
}
fn destroy_image_view(device, view, pAllocator){
   "Vulkan wrapper: destroy_image_view."
   _vkDestroyImageView( device, view, pAllocator)
}
fn create_sampler(device, pCreateInfo, pAllocator, pSampler){
   "Vulkan wrapper: create_sampler."
   _vkCreateSampler( device, pCreateInfo, pAllocator, pSampler)
}
fn destroy_sampler(device, sampler, pAllocator){
   "Vulkan wrapper: destroy_sampler."
   _vkDestroySampler( device, sampler, pAllocator)
}
fn create_shader_module(device, pCreateInfo, pAllocator, pModule){
   "Vulkan wrapper: create_shader_module."
   _vkCreateShaderModule( device, pCreateInfo, pAllocator, pModule)
}
fn destroy_shader_module(device, mod_obj, pAllocator){
   "Vulkan wrapper: destroy_shader_module."
   _vkDestroyShaderModule( device, mod_obj, pAllocator)
}
fn create_pipeline_layout(device, pCreateInfo, pAllocator, pLayout){
   "Vulkan wrapper: create_pipeline_layout."
   _vkCreatePipelineLayout( device, pCreateInfo, pAllocator, pLayout)
}
fn destroy_pipeline_layout(device, layout_obj, pAllocator){
   "Vulkan wrapper: destroy_pipeline_layout."
   _vkDestroyPipelineLayout( device, layout_obj, pAllocator)
}
fn create_render_pass(device, pCreateInfo, pAllocator, pPass){
   "Vulkan wrapper: create_render_pass."
   _vkCreateRenderPass( device, pCreateInfo, pAllocator, pPass)
}
fn destroy_render_pass(device, pass, pAllocator){
   "Vulkan wrapper: destroy_render_pass."
   _vkDestroyRenderPass( device, pass, pAllocator)
}
fn create_graphics_pipelines(device, cache, n_count, pCreateInfos, pAllocator, pPipelines){
   "Vulkan wrapper: create_graphics_pipelines."
   _vkCreateGraphicsPipelines( device, cache, n_count, pCreateInfos, pAllocator, pPipelines)
}
fn destroy_pipeline(device, pipeline, pAllocator){
   "Vulkan wrapper: destroy_pipeline."
   _vkDestroyPipeline( device, pipeline, pAllocator)
}
fn create_framebuffer(device, pCreateInfo, pAllocator, pFramebuffer){
   "Vulkan wrapper: create_framebuffer."
   _vkCreateFramebuffer( device, pCreateInfo, pAllocator, pFramebuffer)
}
fn destroy_framebuffer(device, framebuffer, pAllocator){
   "Vulkan wrapper: destroy_framebuffer."
   _vkDestroyFramebuffer( device, framebuffer, pAllocator)
}
fn create_command_pool(device, pCreateInfo, pAllocator, pPool){
   "Vulkan wrapper: create_command_pool."
   _vkCreateCommandPool( device, pCreateInfo, pAllocator, pPool)
}
fn destroy_command_pool(device, pool, pAllocator){
   "Vulkan wrapper: destroy_command_pool."
   _vkDestroyCommandPool( device, pool, pAllocator)
}
fn allocate_command_buffers(device, pAllocateInfo, pBuffers){
   "Vulkan wrapper: allocate_command_buffers."
   _vkAllocateCommandBuffers( device, pAllocateInfo, pBuffers)
}
fn free_command_buffers(device, pool, n_count, pBuffers){
   "Vulkan wrapper: free_command_buffers."
   _vkFreeCommandBuffers( device, pool, n_count, pBuffers)
}
fn begin_command_buffer(buffer, pBeginInfo){
   "Vulkan wrapper: begin_command_buffer."
   _vkBeginCommandBuffer( buffer, pBeginInfo)
}
fn end_command_buffer(buffer){
   "Vulkan wrapper: end_command_buffer."
   _vkEndCommandBuffer( buffer)
}
fn queue_submit(queue, n_count, pSubmits, fence){
   "Vulkan wrapper: queue_submit."
   _vkQueueSubmit( queue, n_count, pSubmits, fence)
}
fn cmd_begin_render_pass(buffer, pBeginInfo, contents){
   "Vulkan wrapper: cmd_begin_render_pass."
   _vkCmdBeginRenderPass( buffer, pBeginInfo, contents)
}
fn cmd_end_render_pass(buffer){
   "Vulkan wrapper: cmd_end_render_pass."
   _vkCmdEndRenderPass( buffer)
}
fn cmd_bind_pipeline(buffer, bindPoint, pipeline){
   "Vulkan wrapper: cmd_bind_pipeline."
   _vkCmdBindPipeline( buffer, bindPoint, pipeline)
}
fn cmd_draw(buffer, vertexCount, instanceCount, firstVertex, firstInstance){
   "Vulkan wrapper: cmd_draw."
   _vkCmdDraw( buffer, vertexCount, instanceCount, firstVertex, firstInstance)
}
fn cmd_clear_attachments(buffer, n_count, pAttachments, rect_count, pRects){
   "Vulkan wrapper: cmd_clear_attachments."
   _vkCmdClearAttachments(buffer, n_count, pAttachments, rect_count, pRects)
}
fn cmd_set_viewport(buffer, first, count, pViewports){
   "Vulkan wrapper: cmd_set_viewport."
   _vkCmdSetViewport(buffer, first, count, pViewports)
}
fn cmd_set_scissor(buffer, first, count, pScissors){
   "Vulkan wrapper: cmd_set_scissor."
   _vkCmdSetScissor(buffer, first, count, pScissors)
}
fn cmd_push_constants(buffer, pipe_layout, stages, offset, size, pValues){
   "Vulkan wrapper: cmd_push_constants."
   _vkCmdPushConstants(buffer, pipe_layout, stages, offset, size, pValues)
}
fn create_semaphore(device, pCreateInfo, pAllocator, pSemaphore){
   "Vulkan wrapper: create_semaphore."
   _vkCreateSemaphore( device, pCreateInfo, pAllocator, pSemaphore)
}
fn destroy_semaphore(device, semaphore, pAllocator){
   "Vulkan wrapper: destroy_semaphore."
   _vkDestroySemaphore( device, semaphore, pAllocator)
}
fn create_fence(device, pCreateInfo, pAllocator, pFence){
   "Vulkan wrapper: create_fence."
   _vkCreateFence( device, pCreateInfo, pAllocator, pFence)
}
fn destroy_fence(device, fence, pAllocator){
   "Vulkan wrapper: destroy_fence."
   _vkDestroyFence( device, fence, pAllocator)
}
fn wait_for_fences(device, n_count, pFences, waitAll, timeout){
   "Vulkan wrapper: wait_for_fences."
   _vkWaitForFences( device, n_count, pFences, waitAll, timeout)
}
fn reset_fences(device, n_count, pFences){
   "Vulkan wrapper: reset_fences."
   _vkResetFences( device, n_count, pFences)
}
fn device_wait_idle(device){
   "Vulkan wrapper: device_wait_idle."
   _vkDeviceWaitIdle(device)
}

;; New Wrappers
fn create_buffer(device, pCreateInfo, pAllocator, pBuffer){
   "Vulkan wrapper: create_buffer."
   _vkCreateBuffer( device, pCreateInfo, pAllocator, pBuffer)
}
fn destroy_buffer(device, buffer, pAllocator){
   "Vulkan wrapper: destroy_buffer."
   _vkDestroyBuffer( device, buffer, pAllocator)
}
fn get_buffer_memory_requirements(device, buffer, pMemoryRequirements){
   "Vulkan wrapper: get_buffer_memory_requirements."
   _vkGetBufferMemoryRequirements( device, buffer, pMemoryRequirements)
}
fn allocate_memory(device, pAllocateInfo, pAllocator, pMemory){
   "Vulkan wrapper: allocate_memory."
   _vkAllocateMemory( device, pAllocateInfo, pAllocator, pMemory)
}
fn free_memory(device, memory, pAllocator){
   "Vulkan wrapper: free_memory."
   _vkFreeMemory( device, memory, pAllocator)
}
fn bind_buffer_memory(device, buffer, memory, memoryOffset){
   "Vulkan wrapper: bind_buffer_memory."
   _vkBindBufferMemory( device, buffer, memory, memoryOffset)
}
fn map_memory(device, memory, offset, size, flags, ppData){
   "Vulkan wrapper: map_memory."
   _vkMapMemory( device, memory, offset, size, flags, ppData)
}
fn unmap_memory(device, memory){
   "Vulkan wrapper: unmap_memory."
   _vkUnmapMemory( device, memory)
}
fn cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets){
   "Vulkan wrapper: cmd_bind_vertex_buffers."
   _vkCmdBindVertexBuffers( commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets)
}

fn create_descriptor_set_layout(device, pCreateInfo, pAllocator, pSetLayout){
   "Vulkan wrapper: create_descriptor_set_layout."
   _vkCreateDescriptorSetLayout( device, pCreateInfo, pAllocator, pSetLayout)
}
fn destroy_descriptor_set_layout(device, setLayout, pAllocator){
   "Vulkan wrapper: destroy_descriptor_set_layout."
   _vkDestroyDescriptorSetLayout( device, setLayout, pAllocator)
}
fn create_descriptor_pool(device, pCreateInfo, pAllocator, pDescriptorPool){
   "Vulkan wrapper: create_descriptor_pool."
   _vkCreateDescriptorPool( device, pCreateInfo, pAllocator, pDescriptorPool)
}
fn destroy_descriptor_pool(device, descriptorPool, pAllocator){
   "Vulkan wrapper: destroy_descriptor_pool."
   _vkDestroyDescriptorPool( device, descriptorPool, pAllocator)
}
fn allocate_descriptor_sets(device, pAllocateInfo, pDescriptorSets){
   "Vulkan wrapper: allocate_descriptor_sets."
   _vkAllocateDescriptorSets( device, pAllocateInfo, pDescriptorSets)
}
fn free_descriptor_sets(device, descriptorPool, descriptorSetCount, pDescriptorSets){
   "Vulkan wrapper: free_descriptor_sets."
   _vkFreeDescriptorSets( device, descriptorPool, descriptorSetCount, pDescriptorSets)
}
fn update_descriptor_sets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies){
   "Vulkan wrapper: update_descriptor_sets."
   _vkUpdateDescriptorSets( device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies)
}
fn cmd_bind_descriptor_sets(commandBuffer, pipelineBindPoint, layout_h, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets){
   "Vulkan wrapper: cmd_bind_descriptor_sets."
   _vkCmdBindDescriptorSets( commandBuffer, pipelineBindPoint, layout_h, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets)
}
