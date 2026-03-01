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

;; --- Internal Unboxing ---
fn _u(x){ 
   "Unboxes x if it is a tagged integer, otherwise returns x as-is. 
   Critical for Vulkan handles which are stored as integers but used as raw values."
   if(is_int(x)){ return to_int(x) } 
   x 
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
   call3(_vkCreateInstance, _u(pCreateInfo), _u(pAllocator), _u(pInstance))
}
fn destroy_instance(instance, pAllocator){
   "Vulkan wrapper: destroy_instance."
   call2(_vkDestroyInstance, _u(instance), _u(pAllocator))
}
fn enumerate_physical_devices(instance, pCount, pDevices){
   "Vulkan wrapper: enumerate_physical_devices."
   call3(_vkEnumeratePhysicalDevices, _u(instance), _u(pCount), _u(pDevices))
}
fn get_physical_device_properties(device, pProperties){
   "Vulkan wrapper: get_physical_device_properties."
   call2(_vkGetPhysicalDeviceProperties, _u(device), _u(pProperties))
}
fn get_physical_device_memory_properties(device, pProperties){
   "Vulkan wrapper: get_physical_device_memory_properties."
   call2(_vkGetPhysicalDeviceMemoryProperties, _u(device), _u(pProperties))
}
fn get_physical_device_queue_family_properties(device, pCount, pProperties){
   "Vulkan wrapper: get_physical_device_queue_family_properties."
   call3(_vkGetPhysicalDeviceQueueFamilyProperties, _u(device), _u(pCount), _u(pProperties))
}
fn create_device(pd, pCreateInfo, pAllocator, pDevice){
   "Vulkan wrapper: create_device."
   call4(_vkCreateDevice, _u(pd), _u(pCreateInfo), _u(pAllocator), _u(pDevice))
}
fn destroy_device(device, pAllocator){
   "Vulkan wrapper: destroy_device."
   call2(_vkDestroyDevice, _u(device), _u(pAllocator))
}
fn get_device_queue(device, family, idx, pQueue){
   "Vulkan wrapper: get_device_queue."
   call4(_vkGetDeviceQueue, _u(device), _u(family), _u(idx), _u(pQueue))
}

fn create_xlib_surface_khr(instance, pCreateInfo, pAllocator, pSurface){
   "Vulkan wrapper: create_xlib_surface_khr."
   call4(_vkCreateXlibSurfaceKHR, _u(instance), _u(pCreateInfo), _u(pAllocator), _u(pSurface))
}
fn destroy_surface_khr(instance, surface, pAllocator){
   "Vulkan wrapper: destroy_surface_khr."
   call3(_vkDestroySurfaceKHR, _u(instance), _u(surface), _u(pAllocator))
}
fn get_physical_device_surface_support_khr(pd, family, surface, pSupported){
   "Vulkan wrapper: get_physical_device_surface_support_khr."
   call4(_vkGetPhysicalDeviceSurfaceSupportKHR, _u(pd), _u(family), _u(surface), _u(pSupported))
}
fn get_physical_device_surface_formats_khr(pd, surface, pCount, pFormats){
   "Vulkan wrapper: get_physical_device_surface_formats_khr."
   call4(_vkGetPhysicalDeviceSurfaceFormatsKHR, _u(pd), _u(surface), _u(pCount), _u(pFormats))
}
fn get_physical_device_surface_capabilities_khr(pd, surface, pCaps){
   "Vulkan wrapper: get_physical_device_surface_capabilities_khr."
   call3(_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, _u(pd), _u(surface), _u(pCaps))
}
fn get_physical_device_surface_present_modes_khr(pd, surface, pCount, pModes){
   "Vulkan wrapper: get_physical_device_surface_present_modes_khr."
   call4(_vkGetPhysicalDeviceSurfacePresentModesKHR, _u(pd), _u(surface), _u(pCount), _u(pModes))
}

fn create_swapchain_khr(device, pCreateInfo, pAllocator, pSwapchain){
   "Vulkan wrapper: create_swapchain_khr."
   call4(_vkCreateSwapchainKHR, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pSwapchain))
}
fn destroy_swapchain_khr(device, swapchain, pAllocator){
   "Vulkan wrapper: destroy_swapchain_khr."
   call3(_vkDestroySwapchainKHR, _u(device), _u(swapchain), _u(pAllocator))
}
fn get_swapchain_images_khr(device, swapchain, pCount, pImages){
   "Vulkan wrapper: get_swapchain_images_khr."
   call4(_vkGetSwapchainImagesKHR, _u(device), _u(swapchain), _u(pCount), _u(pImages))
}
fn acquire_next_image_khr(device, swapchain, timeout, semaphore, fence, pIndex){
   "Vulkan wrapper: acquire_next_image_khr."
   call6(_vkAcquireNextImageKHR, _u(device), _u(swapchain), _u(timeout), _u(semaphore), _u(fence), _u(pIndex))
}
fn queue_present_khr(queue, pPresentInfo){
   "Vulkan wrapper: queue_present_khr."
   call2(_vkQueuePresentKHR, _u(queue), _u(pPresentInfo))
}

fn create_image(device, pCreateInfo, pAllocator, pImage){
   "Vulkan wrapper: create_image."
   call4(_vkCreateImage, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pImage))
}
fn destroy_image(device, image, pAllocator){
   "Vulkan wrapper: destroy_image."
   call3(_vkDestroyImage, _u(device), _u(image), _u(pAllocator))
}
fn get_image_memory_requirements(device, image, pMemoryRequirements){
   "Vulkan wrapper: get_image_memory_requirements."
   call3(_vkGetImageMemoryRequirements, _u(device), _u(image), _u(pMemoryRequirements))
}
fn bind_image_memory(device, image, memory, memoryOffset){
   "Vulkan wrapper: bind_image_memory."
   call4(_vkBindImageMemory, _u(device), _u(image), _u(memory), _u(memoryOffset))
}
fn create_image_view(device, pCreateInfo, pAllocator, pView){
   "Vulkan wrapper: create_image_view."
   call4(_vkCreateImageView, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pView))
}
fn destroy_image_view(device, view, pAllocator){
   "Vulkan wrapper: destroy_image_view."
   call3(_vkDestroyImageView, _u(device), _u(view), _u(pAllocator))
}
fn create_sampler(device, pCreateInfo, pAllocator, pSampler){
   "Vulkan wrapper: create_sampler."
   call4(_vkCreateSampler, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pSampler))
}
fn destroy_sampler(device, sampler, pAllocator){
   "Vulkan wrapper: destroy_sampler."
   call3(_vkDestroySampler, _u(device), _u(sampler), _u(pAllocator))
}
fn create_shader_module(device, pCreateInfo, pAllocator, pModule){
   "Vulkan wrapper: create_shader_module."
   call4(_vkCreateShaderModule, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pModule))
}
fn destroy_shader_module(device, mod_obj, pAllocator){
   "Vulkan wrapper: destroy_shader_module."
   call3(_vkDestroyShaderModule, _u(device), _u(mod_obj), _u(pAllocator))
}
fn create_pipeline_layout(device, pCreateInfo, pAllocator, pLayout){
   "Vulkan wrapper: create_pipeline_layout."
   call4(_vkCreatePipelineLayout, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pLayout))
}
fn destroy_pipeline_layout(device, layout_obj, pAllocator){
   "Vulkan wrapper: destroy_pipeline_layout."
   call3(_vkDestroyPipelineLayout, _u(device), _u(layout_obj), _u(pAllocator))
}
fn create_render_pass(device, pCreateInfo, pAllocator, pPass){
   "Vulkan wrapper: create_render_pass."
   call4(_vkCreateRenderPass, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pPass))
}
fn destroy_render_pass(device, pass, pAllocator){
   "Vulkan wrapper: destroy_render_pass."
   call3(_vkDestroyRenderPass, _u(device), _u(pass), _u(pAllocator))
}
fn create_graphics_pipelines(device, cache, n_count, pCreateInfos, pAllocator, pPipelines){
   "Vulkan wrapper: create_graphics_pipelines."
   call6(_vkCreateGraphicsPipelines, _u(device), _u(cache), _u(n_count), _u(pCreateInfos), _u(pAllocator), _u(pPipelines))
}
fn destroy_pipeline(device, pipeline, pAllocator){
   "Vulkan wrapper: destroy_pipeline."
   call3(_vkDestroyPipeline, _u(device), _u(pipeline), _u(pAllocator))
}
fn create_framebuffer(device, pCreateInfo, pAllocator, pFramebuffer){
   "Vulkan wrapper: create_framebuffer."
   call4(_vkCreateFramebuffer, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pFramebuffer))
}
fn destroy_framebuffer(device, framebuffer, pAllocator){
   "Vulkan wrapper: destroy_framebuffer."
   call3(_vkDestroyFramebuffer, _u(device), _u(framebuffer), _u(pAllocator))
}
fn create_command_pool(device, pCreateInfo, pAllocator, pPool){
   "Vulkan wrapper: create_command_pool."
   call4(_vkCreateCommandPool, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pPool))
}
fn destroy_command_pool(device, pool, pAllocator){
   "Vulkan wrapper: destroy_command_pool."
   call3(_vkDestroyCommandPool, _u(device), _u(pool), _u(pAllocator))
}
fn allocate_command_buffers(device, pAllocateInfo, pBuffers){
   "Vulkan wrapper: allocate_command_buffers."
   call3(_vkAllocateCommandBuffers, _u(device), _u(pAllocateInfo), _u(pBuffers))
}
fn free_command_buffers(device, pool, n_count, pBuffers){
   "Vulkan wrapper: free_command_buffers."
   call4(_vkFreeCommandBuffers, _u(device), _u(pool), _u(n_count), _u(pBuffers))
}
fn begin_command_buffer(buffer, pBeginInfo){
   "Vulkan wrapper: begin_command_buffer."
   call2(_vkBeginCommandBuffer, _u(buffer), _u(pBeginInfo))
}
fn end_command_buffer(buffer){
   "Vulkan wrapper: end_command_buffer."
   call1(_vkEndCommandBuffer, _u(buffer))
}
fn queue_submit(queue, n_count, pSubmits, fence){
   "Vulkan wrapper: queue_submit."
   call4(_vkQueueSubmit, _u(queue), _u(n_count), _u(pSubmits), _u(fence))
}
fn cmd_begin_render_pass(buffer, pBeginInfo, contents){
   "Vulkan wrapper: cmd_begin_render_pass."
   call3(_vkCmdBeginRenderPass, _u(buffer), _u(pBeginInfo), _u(contents))
}
fn cmd_end_render_pass(buffer){
   "Vulkan wrapper: cmd_end_render_pass."
   call1(_vkCmdEndRenderPass, _u(buffer))
}
fn cmd_bind_pipeline(buffer, bindPoint, pipeline){
   "Vulkan wrapper: cmd_bind_pipeline."
   call3(_vkCmdBindPipeline, _u(buffer), _u(bindPoint), _u(pipeline))
}
fn cmd_draw(buffer, vertexCount, instanceCount, firstVertex, firstInstance){
   "Vulkan wrapper: cmd_draw."
   call5(_vkCmdDraw, _u(buffer), _u(vertexCount), _u(instanceCount), _u(firstVertex), _u(firstInstance))
}
fn cmd_clear_attachments(buffer, n_count, pAttachments, rect_count, pRects){
   "Vulkan wrapper: cmd_clear_attachments."
   call5(_vkCmdClearAttachments, _u(buffer), _u(n_count), _u(pAttachments), _u(rect_count), _u(pRects))
}
fn cmd_set_viewport(buffer, first, count, pViewports){
   "Vulkan wrapper: cmd_set_viewport."
   call4(_vkCmdSetViewport, _u(buffer), _u(first), _u(count), _u(pViewports))
}
fn cmd_set_scissor(buffer, first, count, pScissors){
   "Vulkan wrapper: cmd_set_scissor."
   call4(_vkCmdSetScissor, _u(buffer), _u(first), _u(count), _u(pScissors))
}
fn cmd_push_constants(buffer, pipe_layout, stages, offset, size, pValues){
   "Vulkan wrapper: cmd_push_constants."
   call6(_vkCmdPushConstants, _u(buffer), _u(pipe_layout), _u(stages), _u(offset), _u(size), _u(pValues))
}
fn create_semaphore(device, pCreateInfo, pAllocator, pSemaphore){
   "Vulkan wrapper: create_semaphore."
   call4(_vkCreateSemaphore, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pSemaphore))
}
fn destroy_semaphore(device, semaphore, pAllocator){
   "Vulkan wrapper: destroy_semaphore."
   call3(_vkDestroySemaphore, _u(device), _u(semaphore), _u(pAllocator))
}
fn create_fence(device, pCreateInfo, pAllocator, pFence){
   "Vulkan wrapper: create_fence."
   call4(_vkCreateFence, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pFence))
}
fn destroy_fence(device, fence, pAllocator){
   "Vulkan wrapper: destroy_fence."
   call3(_vkDestroyFence, _u(device), _u(fence), _u(pAllocator))
}
fn wait_for_fences(device, n_count, pFences, waitAll, timeout){
   "Vulkan wrapper: wait_for_fences."
   call5(_vkWaitForFences, _u(device), _u(n_count), _u(pFences), _u(waitAll), _u(timeout))
}
fn reset_fences(device, n_count, pFences){
   "Vulkan wrapper: reset_fences."
   call3(_vkResetFences, _u(device), _u(n_count), _u(pFences))
}
fn device_wait_idle(device){
   "Vulkan wrapper: device_wait_idle."
   call1(_vkDeviceWaitIdle, _u(device))
}

;; New Wrappers
fn create_buffer(device, pCreateInfo, pAllocator, pBuffer){
   "Vulkan wrapper: create_buffer."
   call4(_vkCreateBuffer, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pBuffer))
}
fn destroy_buffer(device, buffer, pAllocator){
   "Vulkan wrapper: destroy_buffer."
   call3(_vkDestroyBuffer, _u(device), _u(buffer), _u(pAllocator))
}
fn get_buffer_memory_requirements(device, buffer, pMemoryRequirements){
   "Vulkan wrapper: get_buffer_memory_requirements."
   call3(_vkGetBufferMemoryRequirements, _u(device), _u(buffer), _u(pMemoryRequirements))
}
fn allocate_memory(device, pAllocateInfo, pAllocator, pMemory){
   "Vulkan wrapper: allocate_memory."
   call4(_vkAllocateMemory, _u(device), _u(pAllocateInfo), _u(pAllocator), _u(pMemory))
}
fn free_memory(device, memory, pAllocator){
   "Vulkan wrapper: free_memory."
   call3(_vkFreeMemory, _u(device), _u(memory), _u(pAllocator))
}
fn bind_buffer_memory(device, buffer, memory, memoryOffset){
   "Vulkan wrapper: bind_buffer_memory."
   call4(_vkBindBufferMemory, _u(device), _u(buffer), _u(memory), _u(memoryOffset))
}
fn map_memory(device, memory, offset, size, flags, ppData){
   "Vulkan wrapper: map_memory."
   call6(_vkMapMemory, _u(device), _u(memory), _u(offset), _u(size), _u(flags), _u(ppData))
}
fn unmap_memory(device, memory){
   "Vulkan wrapper: unmap_memory."
   call2(_vkUnmapMemory, _u(device), _u(memory))
}
fn cmd_bind_vertex_buffers(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets){
   "Vulkan wrapper: cmd_bind_vertex_buffers."
   call5(_vkCmdBindVertexBuffers, _u(commandBuffer), _u(firstBinding), _u(bindingCount), _u(pBuffers), _u(pOffsets))
}

fn create_descriptor_set_layout(device, pCreateInfo, pAllocator, pSetLayout){
   "Vulkan wrapper: create_descriptor_set_layout."
   call4(_vkCreateDescriptorSetLayout, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pSetLayout))
}
fn destroy_descriptor_set_layout(device, setLayout, pAllocator){
   "Vulkan wrapper: destroy_descriptor_set_layout."
   call3(_vkDestroyDescriptorSetLayout, _u(device), _u(setLayout), _u(pAllocator))
}
fn create_descriptor_pool(device, pCreateInfo, pAllocator, pDescriptorPool){
   "Vulkan wrapper: create_descriptor_pool."
   call4(_vkCreateDescriptorPool, _u(device), _u(pCreateInfo), _u(pAllocator), _u(pDescriptorPool))
}
fn destroy_descriptor_pool(device, descriptorPool, pAllocator){
   "Vulkan wrapper: destroy_descriptor_pool."
   call3(_vkDestroyDescriptorPool, _u(device), _u(descriptorPool), _u(pAllocator))
}
fn allocate_descriptor_sets(device, pAllocateInfo, pDescriptorSets){
   "Vulkan wrapper: allocate_descriptor_sets."
   call3(_vkAllocateDescriptorSets, _u(device), _u(pAllocateInfo), _u(pDescriptorSets))
}
fn free_descriptor_sets(device, descriptorPool, descriptorSetCount, pDescriptorSets){
   "Vulkan wrapper: free_descriptor_sets."
   call4(_vkFreeDescriptorSets, _u(device), _u(descriptorPool), _u(descriptorSetCount), _u(pDescriptorSets))
}
fn update_descriptor_sets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies){
   "Vulkan wrapper: update_descriptor_sets."
   call5(_vkUpdateDescriptorSets, _u(device), _u(descriptorWriteCount), _u(pDescriptorWrites), _u(descriptorCopyCount), _u(pDescriptorCopies))
}
fn cmd_bind_descriptor_sets(commandBuffer, pipelineBindPoint, layout_h, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets){
   "Vulkan wrapper: cmd_bind_descriptor_sets."
   call8(_vkCmdBindDescriptorSets, _u(commandBuffer), _u(pipelineBindPoint), _u(layout_h), _u(firstSet), _u(descriptorSetCount), _u(pDescriptorSets), _u(dynamicOffsetCount), _u(pDynamicOffsets))
}
