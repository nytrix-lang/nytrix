;; Auto-generated split Vulkan renderer component
module std.ui.gfx.vk.pipeline (
  _create_shader_module,
  _ensure_shader_binaries,
  _create_descriptor_pool,
  _create_graphics_pipeline
)
use std.core *
use std.core.mem *
use std.os *
use std.os.process as proc
use std.text.io as tio
use std.math *
use std.math.matrix *
use std.ui.glfw as ui_glfw
use std.ui.gfx.vulkan *
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.buffers *
use std.ui.gfx.vk.utils *

fn _create_shader_module(path){
   "Loads a SPIR-V shader file and creates a Vulkan shader module handle."
   def res = file_read(path)
   if(is_err(res)){
      if(_is_debug()){ print(f"Vulkan: Failed to read shader {path}") }
      return 0
   }
   def code = unwrap(res)
   def size = len(code)
   mut ci = sys_malloc(128)
   memset(ci, 0, 128)
   store32(ci, 16, 0) ;; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store32(ci, 0, 8) store32(ci, 0, 12)
   store32(ci, 0, 16) ;; flags
   store64_raw(ci, to_int(size), 24) ;; codeSize (bytes)
   store64_raw(ci, to_int(code), 32) ;; pCode
   mut mod_ptr = sys_malloc(8)
   def vk_res = create_shader_module(vk_get(VK_CTX_DEVICE), ci, 0, mod_ptr)
   if(vk_res != 0){
      if(_is_debug()){ print(f"Vulkan: Failed to create shader module for {path}, code {vk_res}") }
      return 0
   }
   load64(mod_ptr, 0)
}

fn _ensure_shader_binaries(){
   "Internal helper to compile default shader sources via glslc."
   def vert_spv = "build/cache/shader.vert.spv"
   def frag_spv = "build/cache/shader.frag.spv"
   if(_is_debug()){ print("Vulkan: Generating shader binaries with glslc...") }
   def vert_src = "#version 450\nlayout(location=0) in vec3 inPos;\nlayout(location=1) in vec2 inUV;\nlayout(location=2) in vec4 inColor;\nlayout(push_constant) uniform PC { mat4 mvp; } pc;\nlayout(location=0) out vec4 vColor;\nlayout(location=1) out vec2 vUV;\nvoid main(){\n  gl_Position = pc.mvp * vec4(inPos, 1.0);\n  vColor = inColor;\n  vUV = inUV;\n}\n"
   def frag_src = "#version 450\nlayout(location=0) in vec4 vColor;\nlayout(location=1) in vec2 vUV;\nlayout(binding=0) uniform sampler2D texSampler;\nlayout(location=0) out vec4 outColor;\nvoid main(){\n  outColor = texture(texSampler, vUV) * vColor;\n}\n"
   if(is_err(file_write("build/cache/shader.vert", vert_src))){ return false }
   if(is_err(file_write("build/cache/shader.frag", frag_src))){ return false }

   def v_res = proc.run("glslc", ["glslc", "build/cache/shader.vert", "-o", vert_spv])
   if(_is_debug()){ print(f"Vulkan: glslc vert res: {v_res}") }
   if(v_res != 0){ return false }

   def f_res = proc.run("glslc", ["glslc", "build/cache/shader.frag", "-o", frag_spv])
   if(_is_debug()){ print(f"Vulkan: glslc frag res: {f_res}") }
   if(f_res != 0){ return false }

   def v_exists = file_exists(vert_spv)
   def f_exists = file_exists(frag_spv)
   if(_is_debug()){ print(f"Vulkan: shaders exist: v={v_exists} f={f_exists}") }
   v_exists && f_exists
}

fn _create_descriptor_pool(){
   "Initializes the Vulkan descriptor pool for shaders."
   mut pool_size = sys_malloc(8)
   store32(pool_size, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0)
   store32(pool_size, 1000, 4)

   mut pool_ci = sys_malloc(40)
   memset(pool_ci, 0, 40)
   store32(pool_ci, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0)
   store32(pool_ci, 0, 16) ;; flags
   store32(pool_ci, 1000, 20) ;; maxSets
   store32(pool_ci, 1, 24) ;; poolSizeCount
   store64_raw(pool_ci, to_int(pool_size), 32)

   mut pool_ptr = sys_malloc(8)
   if(create_descriptor_pool(vk_get(VK_CTX_DEVICE), pool_ci, 0, pool_ptr) != 0){ return false }
   vk_set(VK_CTX_DESCRIPTOR_POOL, load64(pool_ptr, 0))
   true
}

fn _create_graphics_pipeline(){
   "Configures and creates the graphics pipeline (shaders, vertex input, blending)."
   if(!_ensure_shader_binaries()){
      if(_is_debug()){ print("Vulkan: Could not prepare shader binaries") }
      return false
   }
   vk_set(VK_CTX_VERT_MODULE, _create_shader_module("build/cache/shader.vert.spv"))
   vk_set(VK_CTX_FRAG_MODULE, _create_shader_module("build/cache/shader.frag.spv"))
   if(_is_debug()){
      print(f"Vulkan: shader modules h_v={vk_get(VK_CTX_VERT_MODULE)} h_f={vk_get(VK_CTX_FRAG_MODULE)}")
   }
   if(!vk_get(VK_CTX_VERT_MODULE) || !vk_get(VK_CTX_FRAG_MODULE)){ return false }

   ;; Descriptor Set Layout
   mut dsl_binding = sys_malloc(24) ;; VkDescriptorSetLayoutBinding
   store32(dsl_binding, 0, 0) ;; binding
   store32(dsl_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4) ;; descriptorType
   store32(dsl_binding, 1, 8) ;; descriptorCount
   store32(dsl_binding, VK_SHADER_STAGE_FRAGMENT_BIT, 12) ;; stageFlags
   store32(dsl_binding, 0, 16) store32(dsl_binding, 0, 20) ;; pImmutableSamplers

   mut dsl_ci = sys_malloc(32)
   memset(dsl_ci, 0, 32)
   store32(dsl_ci, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0)
   store32(dsl_ci, 1, 20) ;; bindingCount
   store64_raw(dsl_ci, to_int(dsl_binding), 24) ;; pBindings

   mut dsl_ptr = sys_malloc(8)
   def dsl_res = create_descriptor_set_layout(vk_get(VK_CTX_DEVICE), dsl_ci, 0, dsl_ptr)
   if(dsl_res != 0){
      if(_is_debug()){ print(f"Vulkan: create_descriptor_set_layout failed {dsl_res}") }
      return false
   }
   vk_set(VK_CTX_DESCRIPTOR_SET_LAYOUT, load64(dsl_ptr, 0))
   if(_is_debug()){ _dbg_handle("dsl", vk_get(VK_CTX_DESCRIPTOR_SET_LAYOUT)) }

   ;; Pipeline Layout
   mut pc_range = sys_malloc(12)
   store32(pc_range, 1, 0) ;; STAGE_VERTEX
   store32(pc_range, 0, 4)
   store32(pc_range, 64, 8)
   mut dsl_arr = sys_malloc(8)
   store64_raw(dsl_arr, vk_get(VK_CTX_DESCRIPTOR_SET_LAYOUT), 0)
   mut layout_ci = sys_malloc(48)
   memset(layout_ci, 0, 48)
   store32(layout_ci, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0)
   store32(layout_ci, 0, 8) store32(layout_ci, 0, 12) ;; pNext
   store32(layout_ci, 0, 16) ;; flags
   store32(layout_ci, 1, 20) ;; setLayoutCount
   store64_raw(layout_ci, to_int(dsl_arr), 24) ;; pSetLayouts
   store32(layout_ci, 1, 32) ;; pushConstantRangeCount
   store64_raw(layout_ci, to_int(pc_range), 40) ;; pPushConstantRanges
   mut layout_ptr = sys_malloc(8)
   def pl_res = create_pipeline_layout(vk_get(VK_CTX_DEVICE), layout_ci, 0, layout_ptr)
   if(pl_res != 0){
      if(_is_debug()){ print(f"Vulkan: vkCreatePipelineLayout failed with code {pl_res}") }
      return false
   }
   vk_set(VK_CTX_PIPELINE_LAYOUT, load64(layout_ptr, 0))
   if(_is_debug()){ _dbg_handle("layout", vk_get(VK_CTX_PIPELINE_LAYOUT)) }

   ;; Vertex Input State
   mut binding_desc = sys_malloc(12)
   store32(binding_desc, 0, 0) ;; binding
   store32(binding_desc, 36, 4) ;; stride
   store32(binding_desc, 0, 8) ;; inputRate VERTEX

   mut attr_desc = sys_malloc(48) ;; 3 attributes
   ;; 0: Position (vec3) offset 0
   store32(attr_desc, 0, 0) store32(attr_desc, 0, 4) store32(attr_desc, 106, 8) store32(attr_desc, 0, 12)
   ;; 1: UV (vec2) offset 12
   store32(attr_desc, 1, 16) store32(attr_desc, 0, 20) store32(attr_desc, 103, 24) store32(attr_desc, 12, 28)
   ;; 2: Color (vec4) offset 20
   store32(attr_desc, 2, 32) store32(attr_desc, 0, 36) store32(attr_desc, 109, 40) store32(attr_desc, 20, 44)

   mut vi = sys_malloc(48)
   memset(vi, 0, 48)
   store32(vi, 19, 0)
   store32(vi, 1, 20)
   store64_raw(vi, to_int(binding_desc), 24)
   store32(vi, 3, 32)
   store64_raw(vi, to_int(attr_desc), 40)

   ;; Common States
   mut viewport_state = sys_malloc(48)
   memset(viewport_state, 0, 48)
   store32(viewport_state, 22, 0)
   store32(viewport_state, 1, 20) ;; viewportCount
   store32(viewport_state, 1, 32) ;; scissorCount
   mut rs = sys_malloc(64)
   memset(rs, 0, 64)
   store32(rs, 23, 0)
   store32(rs, 0, 20)
   store32(rs, 0, 24)
   store32(rs, 0, 28) ;; polygonMode = FILL
   store32(rs, 0, 32) ;; cullMode = NONE
   store32(rs, 0, 36) ;; frontFace
   store32(rs, 0, 40) ;; depthBiasEnable
   store32_f32(rs, 1.0, 56) ;; lineWidth
   mut ms = sys_malloc(64)
   memset(ms, 0, 64)
   store32(ms, 24, 0)
   store32(ms, 1, 20) ;; rasterizationSamples
   mut cba = sys_malloc(32)
   memset(cba, 0, 32)
   store32(cba, 1, 0)  ;; blendEnable
   store32(cba, 6, 4)  ;; srcColorBlendFactor = SRC_ALPHA
   store32(cba, 7, 8)  ;; dstColorBlendFactor = ONE_MINUS_SRC_ALPHA
   store32(cba, 0, 12) ;; colorBlendOp = ADD
   store32(cba, 1, 16) ;; srcAlphaBlendFactor = ONE
   store32(cba, 0, 20) ;; dstAlphaBlendFactor = ZERO
   store32(cba, 0, 24) ;; alphaBlendOp = ADD
   store32(cba, 15, 28) ;; colorWriteMask RGBA
   mut cb = sys_malloc(64)
   memset(cb, 0, 64)
   store32(cb, 26, 0)
   store32(cb, 0, 20)
   store32(cb, 0, 24)
   store32(cb, 1, 28)
   store64_raw(cb, to_int(cba), 32)
   mut dyn_states = sys_malloc(8)
   store32(dyn_states, 0, 0)
   store32(dyn_states, 1, 4)
   mut ds = sys_malloc(32)
   memset(ds, 0, 32)
   store32(ds, 27, 0)
   store32(ds, 2, 20)
   store64_raw(ds, to_int(dyn_states), 24)

   ;; Pipeline
   mut stages = sys_malloc(96)
   memset(stages, 0, 96)
   mut main_str = sys_malloc(8)
   store8(main_str, 109, 0) ;; m
   store8(main_str, 97, 1)  ;; a
   store8(main_str, 105, 2) ;; i
   store8(main_str, 110, 3) ;; n
   store8(main_str, 0, 4)

   store32(stages, 18, 0) ;; VERTEX, sType 18
   store32(stages, 0, 8) store32(stages, 0, 12) ;; pNext
   store32(stages, 0, 16) ;; flags
   store32(stages, 1, 20) ;; stage
   store64_raw(stages, vk_get(VK_CTX_VERT_MODULE), 24) ;; module
   store64_raw(stages, to_int(main_str), 32) ;; pName
   store32(stages, 0, 40) store32(stages, 0, 44) ;; pSpecializationInfo

   store32(stages, 18, 48) ;; FRAGMENT, sType 18
   store32(stages, 0, 56) store32(stages, 0, 60) ;; pNext
   store32(stages, 0, 64) ;; flags
   store32(stages, 16, 68) ;; stage
   store64_raw(stages, vk_get(VK_CTX_FRAG_MODULE), 72) ;; module
   store64_raw(stages, to_int(main_str), 80) ;; pName
   store32(stages, 0, 88) store32(stages, 0, 92) ;; pSpecializationInfo

   mut ia = sys_malloc(32)
   memset(ia, 0, 32)
   store32(ia, 20, 0)
   store32(ia, 3, 20) ;; topology = TRIANGLE_LIST
   store32(ia, 0, 24)

   mut ci = sys_malloc(144)
   memset(ci, 0, 144)
   store32(ci, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0)
   store32(ci, 2, 20) ;; stageCount
   store64_raw(ci, to_int(stages), 24) ;; pStages
   store64_raw(ci, to_int(vi), 32) ;; pVertexInputState
   store64_raw(ci, to_int(ia), 40) ;; pInputAssemblyState
   store32(ci, 0, 48) store32(ci, 0, 52) ;; pTessellationState
   store64_raw(ci, to_int(viewport_state), 56) ;; pViewportState
   store64_raw(ci, to_int(rs), 64) ;; pRasterizationState
   store64_raw(ci, to_int(ms), 72) ;; pMultisampleState

   mut dss = sys_malloc(128)
   memset(dss, 0, 128)
   store32(dss, 25, 0) ;; VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
   store32(dss, 0, 20) ;; depthTestEnable
   store32(dss, 0, 24) ;; depthWriteEnable
   store32(dss, 7, 28) ;; depthCompareOp ALWAYS (7)
   store32(dss, 0, 32) ;; depthBoundsTestEnable
   store32(dss, 0, 36) ;; stencilTestEnable

   store64_raw(ci, to_int(dss), 80) ;; pDepthStencilState
   store64_raw(ci, to_int(cb), 88) ;; pColorBlendState
   store64_raw(ci, to_int(ds), 96) ;; pDynamicState
   store64_raw(ci, vk_get(VK_CTX_PIPELINE_LAYOUT), 104)
   store64_raw(ci, vk_get(VK_CTX_RENDER_PASS), 112)
   store32(ci, 0, 120) ;; subpass
   store32(ci, 0, 128) store32(ci, 0, 132)
   store32(ci, -1, 136)
   mut pipe_ptr = sys_malloc(8)
   store32(pipe_ptr, 0, 0) store32(pipe_ptr, 0, 4)
    def gp_res = create_graphics_pipelines(vk_get(VK_CTX_DEVICE), 0, 1, ci, 0, pipe_ptr)
    def h_low = load32(pipe_ptr, 0)
    def h_high = load32(pipe_ptr, 4)
    if(_is_debug()){ print(f"Vulkan: vkCreateGraphicsPipelines res={gp_res} low={h_low} high={h_high}") }
    if(gp_res != 0 || (!h_low && !h_high)){
       if(_is_debug()){ print(f"Vulkan: vkCreateGraphicsPipelines failed or NULL pipeline") }
       return false
    }
   vk_set(VK_CTX_PIPELINE, load64(pipe_ptr, 0))
   if(_is_debug()){ _dbg_handle("pipeline", vk_get(VK_CTX_PIPELINE)) }
   true
}
