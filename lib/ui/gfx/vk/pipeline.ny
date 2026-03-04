;; Keywords: ui gfx vulkan renderer pipeline

module std.ui.gfx.vk.pipeline (
   compile_glsl_to_spirv, create_shader_module_from_source, create_pipeline, bind_pipeline, push_constants,
   _get_default_pipeline, _get_nocull_pipeline, _create_shader_module, _ensure_shader_binaries, _create_graphics_pipeline
)

use std.core *
use std.core.mem *
use std.os.process as proc
use std.ui.gfx.vk.state *
use std.ui.gfx.vk.vulkan *
use std.util.common as common

fn compile_glsl_to_spirv(source, stage_ext){
   "Compiles GLSL source string to SPIR-V bytes using glslc."
   def tmp_src = f"/build/cache/ny_shader_custom_{to_str(ticks())}.{stage_ext}"
   def tmp_spv = f"{tmp_src}.spv"
   unwrap(file_write(tmp_src, source))
   if(proc.run("glslc", ["glslc", tmp_src, "-o", tmp_spv]) != 0){ return 0 }
   def res = file_read(tmp_spv)
   if(is_err(res)){ return 0 }
   unwrap(res)
}

fn create_shader_module_from_source(source, stage_ext){
   "Compiles GLSL source and creates a Vulkan shader module."
   def spirv = compile_glsl_to_spirv(source, stage_ext)
   if(!spirv){ return 0 }

   def size = len(spirv)
   mut ci = sys_malloc(128)
   memset(ci, 0, 128)
   store32(ci, 16, 0) ; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store64_h(ci, size, 24)
   store64_h(ci, spirv, 32)

   mut mod_ptr = sys_malloc(8)
   if(create_shader_module(_device, ci, 0, mod_ptr) != 0){ return 0 }
   def sm_low = load32(mod_ptr, 0)
   def sm_high = load32(mod_ptr, 4)
   (sm_high * 4294967296) + (sm_low & 0xFFFFFFFF)
}

fn create_pipeline(vert_mod, frag_mod, topology=3, depth_test=1, depth_write=1, cull_mode=0, front_face=0, depth_bias=0, depth_clamp=0, line_width=1.0){
   "Creates a custom graphics pipeline. topology: 3=TRI_LIST, 1=LINE_LIST."
   mut main_str = sys_malloc(8)
   strcpy(main_str, "main")

   def s1 = VkPipelineShaderStageCreateInfo(1, vert_mod, main_str)
   def s2 = VkPipelineShaderStageCreateInfo(16, frag_mod, main_str)
   mut stages = sys_malloc(96)
   memcpy(stages, s1, 48)
   memcpy(stages + 48, s2, 48)

   def pipe_layout = _pipeline_layout
   mut binding_desc = sys_malloc(12)
   store32(binding_desc, 0, 0)
   store32(binding_desc, _VKR_VERT_STRIDE, 4)
   store32(binding_desc, 0, 8)

   mut attr_desc = sys_malloc(80)
   store32(attr_desc, 0, 0)  store32(attr_desc, 0, 4)  store32(attr_desc, 106, 8) store32(attr_desc, 0, 12)
   store32(attr_desc, 1, 16) store32(attr_desc, 0, 20) store32(attr_desc, 103, 24) store32(attr_desc, 12, 28)
   store32(attr_desc, 2, 32) store32(attr_desc, 0, 36) store32(attr_desc, 37, 40) store32(attr_desc, 20, 44)
   store32(attr_desc, 3, 48) store32(attr_desc, 0, 52) store32(attr_desc, 98, 56) store32(attr_desc, 24, 60)
   store32(attr_desc, 4, 64) store32(attr_desc, 0, 68) store32(attr_desc, 106, 72) store32(attr_desc, 28, 76)
   def vi = VkPipelineVertexInputStateCreateInfo(1, binding_desc, 5, attr_desc)

   def ia = VkPipelineInputAssemblyStateCreateInfo(topology, 0)
   def viewport_state = VkPipelineViewportStateCreateInfo(1, 0, 1, 0)
   ; Default to CCW (0) and zero bias unless enabled.
   def rs = VkPipelineRasterizationStateCreateInfo(depth_clamp, 0, 0, cull_mode, front_face, depth_bias, 0.0, 0.0, 0.0, float(line_width))
   def ms = VkPipelineMultisampleStateCreateInfo(_cfg_msaa, 0, 0.0, 0, 0, 0)
   ; blendEnable=1, srcColor=ONE (1), dstColor=ONE_MINUS_SRC_ALPHA (7), colorOp=ADD (0)
   ; srcAlpha=ONE (1), dstAlpha=ONE_MINUS_SRC_ALPHA (7), alphaOp=ADD (0), mask=(15)
   def cba = VkPipelineColorBlendAttachmentState(1, 1, 7, 0, 1, 7, 0, 15)
   def cb = VkPipelineColorBlendStateCreateInfo(0, 0, 1, cba, 0)
   def dss = VkPipelineDepthStencilStateCreateInfo(depth_test, depth_write, 3, 0, 0, 0, 0, 0.0, 1.0)

   mut dyn_states = sys_malloc(12)
   store32(dyn_states, 0, 0)
   store32(dyn_states, 1, 4)
   store32(dyn_states, 2, 8) ; line width
   def ds = VkPipelineDynamicStateCreateInfo(3, dyn_states)

   def ci = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs, ms, dss, cb, ds, pipe_layout, _render_pass, 0, 0, -1)
   mut pipe_ptr = sys_malloc(8)
   if(create_graphics_pipelines(_device, 0, 1, ci, 0, pipe_ptr) != 0){ return 0 }
   load64(pipe_ptr, 0)
}

fn bind_pipeline(pipe){
   "Binds a custom graphics pipeline for subsequent draw calls. Pass 0 to restore default."
   if(!_frame_open){ return }
   mut p = pipe
   if(p == 0){ p = _pipeline }
   if(p == _last_bound_pipe){ _target_pipeline = p return }
   _flush()

   ;; Enable custom push constant mode for non-engine pipelines
   def is_custom = (p != _pipeline && p != _nocull_pipeline && p != _unlit_pipeline && p != _wire_pipeline && p != 0)
   if(is_custom){ _use_custom_pc = 1 } else { _use_custom_pc = 0 }

   def cb = get(_command_buffers, _current_frame)
   cmd_bind_pipeline(cb, 0, p)
   _last_bound_pipe = p
   _target_pipeline = p
   _pc_dirty = true ;; force push constants for new pipeline
}

fn push_constants(ptr, size, offset=0){
   "Pushes raw data to the current pipeline's push constants and caches it for flushes."
   if(!_frame_open || !ptr || size <= 0){ return }
   if(offset + size > 160){ return }

   ;; Use custom push constant buffer for custom pipelines
   def pc_ptr = _use_custom_pc ? _pc_buffer_custom : _pc_buffer

   ;; Cache in push constant buffer so automatic _flush doesn't clobber it
   memcpy(pc_ptr + offset, ptr, size)
   _pc_dirty = true

   def cb = get(_command_buffers, _current_frame)
   cmd_push_constants(cb, _pipeline_layout, 1 | 16, offset, size, ptr)
}

fn use_custom_push_constants(enabled){
   "Enables/disables custom push constant mode for custom pipelines."
   _use_custom_pc = enabled ? 1 : 0
}

fn set_custom_push_constants(ptr, size, offset=0){
   "Sets custom push constant data (call after bind_pipeline with custom pipeline)."
   if(!_frame_open || !ptr || size <= 0){ return }
   if(offset + size > 160){ return }

   memcpy(_pc_buffer_custom + offset, ptr, size)
   _pc_dirty = true

   def cb = get(_command_buffers, _current_frame)
   cmd_push_constants(cb, _pipeline_layout, 1 | 16, offset, size, ptr)
}

fn _get_default_pipeline(){
   "Internal: returns the default triangle pipeline handle."
   _pipeline
}

fn _create_shader_module(path){
   "Internal: Loads a SPIR-V shader file and creates a Vulkan shader module handle."
   def res = file_read(path)
   if(is_err(res)){
      return 0
   }
   def code = unwrap(res)
   def size = len(code)
   mut ci = sys_malloc(128)
   memset(ci, 0, 128)
   store32(ci, 16, 0) ; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store32(ci, 0, 8) store32(ci, 0, 12)
   store32(ci, 0, 16)
   store64_h(ci, size, 24)
   store64_h(ci, code, 32) ; pCode
   mut mod_ptr = sys_malloc(8)
   def vk_res = create_shader_module(_device, ci, 0, mod_ptr)
   if(vk_res != 0){
      return 0
   }
   def sm_low = load32(mod_ptr, 0)
   def sm_high = load32(mod_ptr, 4)
   (sm_high * 4294967296) + (sm_low & 0xFFFFFFFF)
}

fn _ensure_shader_binaries(){
   "Internal: Ensures default shader SPIR-V files exist by compiling them from source if necessary."
   def vert_spv = "/build/cache/ny_shader.vert.spv"
   def frag_spv = "/build/cache/ny_shader.frag.spv"

   if(is_str(_cfg_vert_spv) && file_exists(_cfg_vert_spv)){
      proc.run("cp", ["cp", _cfg_vert_spv, vert_spv])
   } else {
      mut vert_src = ""
      if(_ubo_enabled){
         vert_src = "#version 450\nlayout(location=0) in vec3 inPos;\nlayout(location=1) in vec2 inUV;\nlayout(location=2) in vec4 inColor;\nlayout(location=3) in uint inTexIndex;\nlayout(location=4) in vec3 inNormal;\nlayout(set=1, binding=0, std140) uniform UBO { mat4 vp; mat4 model; ivec4 flags; } ubo;\nlayout(location=0) out vec4 vColor;\nlayout(location=1) out vec2 vUV;\nlayout(location=2) out vec3 vNormal;\nlayout(location=3) flat out uint vTexIndex;\nvoid main(){\n  gl_Position = ubo.vp * ubo.model * vec4(inPos, 1.0);\n  vColor = inColor;\n  vUV = inUV;\n  vNormal = mat3(ubo.model) * inNormal;\n  vTexIndex = inTexIndex;\n}\n"
      } else {
         vert_src = "#version 450\nlayout(location=0) in vec3 inPos;\nlayout(location=1) in vec2 inUV;\nlayout(location=2) in vec4 inColor;\nlayout(location=3) in uint inTexIndex;\nlayout(location=4) in vec3 inNormal;\nlayout(push_constant) uniform PC { mat4 vp; mat4 model; int isMask; int isUnlit; } pc;\nlayout(location=0) out vec4 vColor;\nlayout(location=1) out vec2 vUV;\nlayout(location=2) out vec3 vNormal;\nlayout(location=3) flat out uint vTexIndex;\nvoid main(){\n  gl_Position = pc.vp * pc.model * vec4(inPos, 1.0);\n  vColor = inColor;\n  vUV = inUV;\n  vNormal = mat3(pc.model) * inNormal;\n  vTexIndex = inTexIndex;\n}\n"
      }
      unwrap(file_write("/build/cache/ny_shader.vert", vert_src))
      if(proc.run("glslc", ["glslc", "/build/cache/ny_shader.vert", "-o", vert_spv]) != 0 && !file_exists(vert_spv)){ return false }
      ; SDF vertex shader
      def vert_src_sdf = "#version 450\nlayout(location=0) in vec3 inPos;\nlayout(location=1) in vec2 inUV;\nlayout(location=2) in vec4 inColor;\nlayout(location=3) in uint inTexIndex;\nlayout(location=4) in vec3 inNormal;\nlayout(push_constant) uniform PC { mat4 vp; mat4 model; int isMask; int isUnlit; } pc;\nlayout(location=0) out vec4 vColor;\nlayout(location=1) out vec2 vUV;\nlayout(location=2) out vec3 vNormal;\nlayout(location=3) flat out uint vTexIndex;\nvoid main(){\n  gl_Position = pc.vp * pc.model * vec4(inPos, 1.0);\n  vColor = inColor;\n  vUV = inUV;\n  vNormal = inNormal;\n  vTexIndex = inTexIndex;\n}\n"
      unwrap(file_write("/build/cache/ny_shader_sdf.vert", vert_src_sdf))
      proc.run("glslc", ["glslc", "/build/cache/ny_shader_sdf.vert", "-o", "/build/cache/ny_shader_sdf.vert.spv"])
   }
   if(is_str(_cfg_frag_spv) && file_exists(_cfg_frag_spv)){
       proc.run("cp", ["cp", _cfg_frag_spv, frag_spv])
   } else {
      mut frag_src = ""
      mut ubo_decl = ""
      mut mask_line = ""
      mut unlit_line = ""
      if(_ubo_enabled){
         ubo_decl = "layout(set=1, binding=0, std140) uniform UBO { mat4 vp; mat4 model; ivec4 flags; } ubo;\n"
         mask_line = "  if(ubo.flags.x != 0){ tex = vec4(1.0, 1.0, 1.0, tex.r); }\n"
         unlit_line = "  if(ubo.flags.y != 0){\n"
      } else {
         ubo_decl = "layout(push_constant) uniform PC { mat4 vp; mat4 model; int isMask; int isUnlit; } pc;\n"
         mask_line = "  if(pc.isMask != 0){ tex = vec4(1.0, 1.0, 1.0, tex.r); }\n"
         unlit_line = "  if(pc.isUnlit != 0){\n"
      }

      if(_bindless_enabled){
         frag_src = "#version 450\n#extension GL_EXT_nonuniform_qualifier : enable\n" +
         "layout(location=0) in vec4 vColor;\n" +
         "layout(location=1) in vec2 vUV;\n" +
         "layout(location=2) in vec3 vNormal;\n" +
         "layout(location=3) flat in uint vTexIndex;\n" +
         ubo_decl +
         "layout(set=0, binding=0) uniform sampler2D texSamplers[" + to_str(MAX_TEXTURES) + "];\n" +
         "layout(location=0) out vec4 outColor;\n" +
         "void main(){\n" +
         "  vec4 tex = texture(texSamplers[nonuniformEXT(vTexIndex)], vUV);\n" +
         mask_line +
         unlit_line +
         "     vec4 base = vColor * tex;\n" +
         "     outColor = vec4(base.rgb * base.a, base.a);\n" +
         "  } else {\n" +
         "     vec3 normal = vNormal;\n" +
         "     float nl = length(normal);\n" +
         "     if(nl < 1e-5){ normal = vec3(0.0, 0.0, 1.0); }\n" +
         "     else { normal = normal / nl; }\n" +
         "     vec3 l = normalize(vec3(0.5, 1.0, 0.5));\n" +
         "     float diff = max(dot(normal, l), 0.1);\n" +
         "     vec3 skyCol = vec3(0.5, 0.7, 1.0); vec3 groundCol = vec3(0.12, 0.12, 0.15);\n" +
         "     vec3 ambient = mix(groundCol, skyCol, normal.y * 0.5 + 0.5) * 0.4;\n" +
         "     vec4 lit = vColor * tex * vec4(ambient + diff * 0.7, 1.0);\n" +
         "     outColor = vec4(lit.rgb * lit.a, lit.a);\n" +
         "  }\n" +
         "}\n"
      } else {
         frag_src = "#version 450\n" +
         "layout(location=0) in vec4 vColor;\n" +
         "layout(location=1) in vec2 vUV;\n" +
         "layout(location=2) in vec3 vNormal;\n" +
         ubo_decl +
         "layout(set=0, binding=0) uniform sampler2D texSampler;\n" +
         "layout(location=0) out vec4 outColor;\n" +
         "void main(){\n" +
         "  vec4 tex = texture(texSampler, vUV);\n" +
         mask_line +
         unlit_line +
         "     vec4 base = vColor * tex;\n" +
         "     outColor = vec4(base.rgb * base.a, base.a);\n" +
         "  } else {\n" +
         "     vec3 normal = vNormal;\n" +
         "     float nl = length(normal);\n" +
         "     if(nl < 1e-5){ normal = vec3(0.0, 0.0, 1.0); }\n" +
         "     else { normal = normal / nl; }\n" +
         "     vec3 l = normalize(vec3(0.5, 1.0, 0.5));\n" +
         "     float diff = max(dot(normal, l), 0.1);\n" +
         "     vec3 skyCol = vec3(0.5, 0.7, 1.0); vec3 groundCol = vec3(0.12, 0.12, 0.15);\n" +
         "     vec3 ambient = mix(groundCol, skyCol, normal.y * 0.5 + 0.5) * 0.4;\n" +
         "     vec4 lit = vColor * tex * vec4(ambient + diff * 0.7, 1.0);\n" +
         "     outColor = vec4(lit.rgb * lit.a, lit.a);\n" +
         "  }\n" +
         "}\n"
      }
      unwrap(file_write("/build/cache/ny_shader.frag", frag_src))
      if(proc.run("glslc", ["glslc", "/build/cache/ny_shader.frag", "-o", frag_spv]) != 0 && !file_exists(frag_spv)){ return false }
   }
   file_exists(vert_spv) && file_exists(frag_spv)
}

fn _create_graphics_pipeline(){
   "Internal: Main entry point for initializing all standard graphics pipelines (Lit, Unlit, Line, Wireframe)."
   print("Vulkan step: _ensure_shader_binaries...")
   if(!_ensure_shader_binaries()){
      print("Vulkan: shader binaries failed")
      return false
   }
   print("Vulkan: shader binaries OK")
   _vert_module = _create_shader_module("/build/cache/ny_shader.vert.spv")
   _frag_module = _create_shader_module("/build/cache/ny_shader.frag.spv")
   if(!_vert_module || !_frag_module){ return false }
   mut tex_count = 1
   if(_bindless_enabled){ tex_count = MAX_TEXTURES }
   def tex_binding = VkDescriptorSetLayoutBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, tex_count, VK_SHADER_STAGE_FRAGMENT_BIT, 0)
   def tex_ci = VkDescriptorSetLayoutCreateInfo(1, tex_binding)
   if(_bindless_enabled){
      store32(tex_ci, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 16)
      mut flags = sys_malloc(4)
      store32(flags, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0)
      mut flags_ci = sys_malloc(32)
      memset(flags_ci, 0, 32)
      store32(flags_ci, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0)
      store32(flags_ci, 1, 16)
      store64_h(flags_ci, flags, 24)
      store64_h(tex_ci, flags_ci, 8)
   }

   mut dsl_ptr = sys_malloc(8)
   if(create_descriptor_set_layout(_device, tex_ci, 0, dsl_ptr) != 0){ return false }
   _descriptor_set_layout = load64(dsl_ptr, 0)

   def ubo_binding = VkDescriptorSetLayoutBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0)
   def ubo_ci = VkDescriptorSetLayoutCreateInfo(1, ubo_binding)
   mut ubo_ptr = sys_malloc(8)
   if(create_descriptor_set_layout(_device, ubo_ci, 0, ubo_ptr) != 0){ return false }
   _descriptor_set_layout_ubo = load64(ubo_ptr, 0)
   mut pc_range = sys_malloc(12)
   store32(pc_range, 1 | 16, 0) ; STAGE_VERTEX | STAGE_FRAGMENT
   store32(pc_range, 0, 4)
   store32(pc_range, 160, 8) ; size 160 (aligned)
   mut dsl_arr = sys_malloc(16)
   store64_h(dsl_arr, _descriptor_set_layout, 0)
   store64_h(dsl_arr, _descriptor_set_layout_ubo, 8)

   def pc_count = _ubo_enabled ? 0 : 1
   def pc_ptr = _ubo_enabled ? 0 : pc_range
   def layout_ci = VkPipelineLayoutCreateInfo(2, dsl_arr, pc_count, pc_ptr)

   mut layout_ptr = sys_malloc(8)
   def pl_res = create_pipeline_layout(_device, layout_ci, 0, layout_ptr)
   if(pl_res != 0){
      return false
   }
   _pipeline_layout = load64(layout_ptr, 0)
   mut binding_desc = sys_malloc(12)
   store32(binding_desc, 0, 0) ; binding
   store32(binding_desc, _VKR_VERT_STRIDE, 4)
   store32(binding_desc, 0, 8) ; inputRate VERTEX

   mut attr_desc = sys_malloc(80) ; 5 attributes
   ; 0: Position (vec3) offset 0
   store32(attr_desc, 0, 0) store32(attr_desc, 0, 4) store32(attr_desc, 106, 8) store32(attr_desc, 0, 12)
   ; 1: UV (vec2) offset 12
   store32(attr_desc, 1, 16) store32(attr_desc, 0, 20) store32(attr_desc, 103, 24) store32(attr_desc, 12, 28)
   ; 2: Color (R8G8B8A8_UNORM) offset 20
   store32(attr_desc, 2, 32) store32(attr_desc, 0, 36) store32(attr_desc, 37, 40) store32(attr_desc, 20, 44)
   ; 3: TexIndex (uint) offset 24
   store32(attr_desc, 3, 48) store32(attr_desc, 0, 52) store32(attr_desc, 98, 56) store32(attr_desc, 24, 60)
   ; 4: Normal (vec3) offset 28
   store32(attr_desc, 4, 64) store32(attr_desc, 0, 68) store32(attr_desc, 106, 72) store32(attr_desc, 28, 76)

   def vi = VkPipelineVertexInputStateCreateInfo(1, binding_desc, 5, attr_desc)
   def viewport_state = VkPipelineViewportStateCreateInfo(1, 0, 1, 0)
   def rs_cull = VkPipelineRasterizationStateCreateInfo(0, 0, 0, 2, 0, 0, 0, 0.0, 0.0, 1.0) ; cull=BACK(2), front=CCW(0)
   def rs_nocull = VkPipelineRasterizationStateCreateInfo(0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 1.0) ; cull=NONE(0), front=CCW(0)
   def ms = VkPipelineMultisampleStateCreateInfo(_cfg_msaa, 0, 0.0, 0, 0, 0)
   ;; Use ONE for srcC because we manually pre-multiplied RGB by Alpha in the fragment shader!
   ;; This prevents double-multiplication while keeping the swapchain exactly pre-multiplied for the Compositor.
   def cba = VkPipelineColorBlendAttachmentState(1, 1, 7, 0, 1, 7, 0, 15) ; blend=1, srcC=ONE(1), dstC=ONE_MINUS_SRC_ALPHA(7), srcA=ONE(1), dstA=ONE_MINUS_SRC_ALPHA(7)
   def cb = VkPipelineColorBlendStateCreateInfo(0, 0, 1, cba, 0)

   ; Depth Stencil State (Enabled for 3D)

   mut dyn_states = sys_malloc(12)
   store32(dyn_states, 0, 0) ; VK_DYNAMIC_STATE_VIEWPORT
   store32(dyn_states, 1, 4) ; VK_DYNAMIC_STATE_SCISSOR
   store32(dyn_states, 2, 8) ; VK_DYNAMIC_STATE_LINE_WIDTH
   def ds = VkPipelineDynamicStateCreateInfo(3, dyn_states)
   mut main_str = sys_malloc(8)
   strcpy(main_str, "main")

   def s1 = VkPipelineShaderStageCreateInfo(1, _vert_module, main_str)
   def s2 = VkPipelineShaderStageCreateInfo(16, _frag_module, main_str)
   ; Pack two stage structs contiguously (48 bytes each)
   mut stages = sys_malloc(96)
   memcpy(stages, s1, 48)
   memcpy(stages + 48, s2, 48)

   ; 1. Create Lit Pipeline (with depth test)
   def ia = VkPipelineInputAssemblyStateCreateInfo(3, 0)
   ; Enable robust Depth Testing & Writing for 3D Mesh culling
   def dss = VkPipelineDepthStencilStateCreateInfo(1, 1, 3, 0, 0, 0, 0, 0.0, 1.0)
   common.touch(dss)
   def ci = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_cull, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   mut pipe_ptr = sys_malloc(8)
   store32(pipe_ptr, 0, 0) store32(pipe_ptr, 0, 4)
   if(_debug_gfx_enabled){
      print(f"Vulkan: Creating graphics pipeline with device={_device} layout={_pipeline_layout} pass={_render_pass}")
   }
   def res = create_graphics_pipelines(_device, 0, 1, ci, 0, pipe_ptr)
   if(_debug_gfx_enabled){ print(f"Vulkan: create_graphics_pipelines returned {res}") }
   if(res != 0){ return false }

   if(_debug_gfx_enabled){ print("Vulkan: Loading graphics pipeline handle...") }
   _pipeline = load64(pipe_ptr, 0)
   if(_debug_gfx_enabled){ _dbg_handle("pipeline", _pipeline) }

   ; 2. Create Lit No-Cull Pipeline (depth test on, culling off)
   def ci_nocull = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_nocull, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_nocull, 0, pipe_ptr) == 0){
       _nocull_pipeline = load64(pipe_ptr, 0)
   }

   ; 3. Create Unlit Pipeline (no depth test)
   def dss_unlit = VkPipelineDepthStencilStateCreateInfo(0, 0, 0, 0, 0, 0, 0, 0.0, 1.0)
   def ci_unlit = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_nocull, ms, dss_unlit, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_unlit, 0, pipe_ptr) == 0){
       _unlit_pipeline = load64(pipe_ptr, 0)
   }

   ; 4. Create Line Pipeline (for robust line rendering)
   def ia_line = VkPipelineInputAssemblyStateCreateInfo(1, 0) ; topology=LINE_LIST
   def ci_line = VkGraphicsPipelineCreateInfo(2, stages, vi, ia_line, 0, viewport_state, rs_nocull, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_line, 0, pipe_ptr) == 0){
       _line_pipeline = load64(pipe_ptr, 0)
   }

   ; 5. Create Wireframe Pipeline (PolygonMode=LINE=1, Cull=NONE=0)
   def rs_wire = VkPipelineRasterizationStateCreateInfo(0, 0, 1, 0, 0, 0, 0, 0.0, 0.0, 1.0)
   def ci_wire = VkGraphicsPipelineCreateInfo(2, stages, vi, ia, 0, viewport_state, rs_wire, ms, dss, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
   if(create_graphics_pipelines(_device, 0, 1, ci_wire, 0, pipe_ptr) == 0){
       _wire_pipeline = load64(pipe_ptr, 0)
   }

   ; 6. Create Circle Pipeline (SDF)
   def frag_src_circle = "#version 450\nlayout(location=0) in vec4 vColor; layout(location=1) in vec2 vUV; layout(location=2) in vec3 vNormal; layout(location=3) flat in uint vTexIndex;\nlayout(location=0) out vec4 outColor;\nvoid main(){\n  vec2 uv = vUV * 2.0 - 1.0;\n  float d = length(uv);\n  float alpha = clamp((1.01 - d) / (fwidth(d) + 0.001), 0.0, 1.0);\n  if(alpha <= 0.0) discard;\n  outColor = vec4(vColor.rgb, vColor.a * alpha);\n}\n"
   def frag_circle_mod = create_shader_module_from_source(frag_src_circle, "frag")
   def vert_sdf_mod = _create_shader_module("/build/cache/ny_shader_sdf.vert.spv")
   if(frag_circle_mod && vert_sdf_mod){
      def s1_sdf = VkPipelineShaderStageCreateInfo(1, vert_sdf_mod, main_str)
      def s2_circle = VkPipelineShaderStageCreateInfo(16, frag_circle_mod, main_str)
      mut stages_circle = sys_malloc(96)
      memcpy(stages_circle, s1_sdf, 48)
      memcpy(stages_circle + 48, s2_circle, 48)
      def ci_circle = VkGraphicsPipelineCreateInfo(2, stages_circle, vi, ia, 0, viewport_state, rs_nocull, ms, dss_unlit, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
      if(create_graphics_pipelines(_device, 0, 1, ci_circle, 0, pipe_ptr) == 0){ _circle_pipeline = load64(pipe_ptr, 0) }
   }

   ; 6. Create Ring Pipeline (SDF)
   def frag_src_ring = "#version 450\nlayout(location=0) in vec4 vColor; layout(location=1) in vec2 vUV; layout(location=2) in vec3 vNormal; layout(location=3) flat in uint vTexIndex;\nlayout(location=0) out vec4 outColor;\nvoid main(){\n  vec2 uv = vUV * 2.0 - 1.0;\n  float d = length(uv);\n  float fw = fwidth(d) + 0.001;\n  float outer_alpha = clamp((1.01 - d) / fw, 0.0, 1.0);\n  float inner_ratio = vNormal.x;\n  float inner_alpha = clamp((d - (inner_ratio - 0.01)) / fw, 0.0, 1.0);\n  float alpha = outer_alpha * inner_alpha;\n  if(alpha <= 0.0) discard;\n  outColor = vec4(vColor.rgb, vColor.a * alpha);\n}\n"
   def frag_ring_mod = create_shader_module_from_source(frag_src_ring, "frag")
   if(frag_ring_mod && vert_sdf_mod){
      def s1_sdf = VkPipelineShaderStageCreateInfo(1, vert_sdf_mod, main_str)
      def s2_ring = VkPipelineShaderStageCreateInfo(16, frag_ring_mod, main_str)
      mut stages_ring = sys_malloc(96)
      memcpy(stages_ring, s1_sdf, 48)
      memcpy(stages_ring + 48, s2_ring, 48)
      def ci_ring = VkGraphicsPipelineCreateInfo(2, stages_ring, vi, ia, 0, viewport_state, rs_nocull, ms, dss_unlit, cb, ds, _pipeline_layout, _render_pass, 0, 0, -1)
      if(create_graphics_pipelines(_device, 0, 1, ci_ring, 0, pipe_ptr) == 0){ _ring_pipeline = load64(pipe_ptr, 0) }
   }

   if(_debug_gfx_enabled){ print("Vulkan: Graphics pipeline initialization complete.") }
   true
}

fn _get_nocull_pipeline(){
   "Returns the default lit no-cull pipeline handle."
   _nocull_pipeline
}
