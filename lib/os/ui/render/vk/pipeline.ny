;; Keywords: render vulkan gpu pipeline
;; Vulkan pipeline creation, binding, cache, and shader-stage setup.
module std.os.ui.render.vk.pipeline(compile_glsl_to_spirv, create_shader_module_from_source, create_pipeline, bind_pipeline, push_constants, shader_pc_bytes, _get_default_pipeline, _get_nocull_pipeline, _get_unlit_nocull_pipeline, _get_flip_pipeline, _get_flip_unlit_pipeline, _get_mesh_opaque_pipeline, _get_mesh_opaque_nocull_pipeline, _get_mesh_opaque_nocull_flip_pipeline, _get_mesh_opaque_unlit_pipeline, _get_mesh_opaque_unlit_nocull_pipeline, _get_mesh_opaque_unlit_nocull_flip_pipeline, _get_mesh_fast_opaque_pipeline, _get_mesh_fast_opaque_nocull_pipeline, _get_mesh_fast_opaque_flip_pipeline, _get_mesh_fast_opaque_nocull_flip_pipeline, _get_mesh_fast_env_opaque_pipeline, _get_mesh_fast_env_opaque_nocull_pipeline, _get_mesh_fast_env_opaque_flip_pipeline, _get_mesh_fast_env_opaque_nocull_flip_pipeline, _get_mesh_alpha_pipeline, _get_mesh_alpha_nocull_pipeline, _get_mesh_alpha_nocull_flip_pipeline, _get_mesh_alpha_unlit_pipeline, _get_mesh_alpha_unlit_nocull_pipeline, _get_mesh_alpha_unlit_nocull_flip_pipeline, _get_mesh_alpha_flip_pipeline, _get_mesh_alpha_unlit_flip_pipeline, _create_shader_module, _ensure_shader_binaries, _create_graphics_pipeline, _ensure_nocull_pipeline, _ensure_line_pipeline, _ensure_point_pipeline, _ensure_wire_pipeline, _ensure_circle_pipeline, _ensure_ring_pipeline, _ensure_rounded_rect_pipeline, _ensure_skybox_pipeline)
use std.core
use std.core.mem
use std.os
use std.os.path as ospath
use std.os.fs as osfs
use std.os.sys as sys
use std.os.process as proc
use std.os.ui.profile as ui_profile
use std.os.ui.render.vk.state
use std.os.ui.render.vk.vulkan
use std.os.ui.render.vk.renderer (_flush)
use std.os.ui.render.vk.utils (_dbg_handle)
use std.core.common as common
use std.core.str (to_hex)

def _SHADER_PC_BYTES = 256
def _VK_TOPO_POINTS = 0
def _VK_TOPO_LINES = 1
def _VK_TOPO_TRIANGLES = 3
def _VK_CULL_NONE = 0
def _VK_CULL_BACK = 0x00000002
def _VK_FRONT_DEFAULT = 0
def _VK_FRONT_FLIPPED = 1
def _VK_POLYGON_FILL = 0
def _VK_POLYGON_LINE = 1
def _PIPE_BLEND_OPAQUE = 0
def _PIPE_BLEND_UI = 1
def _PIPE_BLEND_ALPHA = 2

fn _max_textures_value(): int { 4096 }

fn _vk_descriptor_combined_image_sampler(): int { 1 }

fn _vk_descriptor_uniform_buffer(): int { 6 }

fn _vk_shader_stage_vertex(): int { 0x00000001 }

fn _vk_shader_stage_fragment(): int { 0x00000010 }

fn shader_pc_bytes(): int {
   "Return the Vulkan shader push-constant byte size."
   256
}

fn _pipe_alloc(int: size): ?ptr {
   def p = zalloc(size)
   if(!p){ panic("vulkan pipeline allocation failed") }
   p
}

fn _write_default_vertex_binding_desc(any: binding_desc): any {
   store32(binding_desc, 0, 0)
   store32(binding_desc, _VKR_VERT_STRIDE, 4)
   store32(binding_desc, 0, 8)
}

fn _write_vertex_attr(any: attr_desc, int: loc, int: format, int: off): any {
   def base = loc * 16
   store32(attr_desc, loc, base)
   store32(attr_desc, 0, base + 4)
   store32(attr_desc, format, base + 8)
   store32(attr_desc, off, base + 12)
}

fn _write_default_vertex_attr_desc(any: attr_desc): any {
   _write_vertex_attr(attr_desc, 0, 106, _VKR_OFF_X)
   _write_vertex_attr(attr_desc, 1, 103, _VKR_OFF_U)
   _write_vertex_attr(attr_desc, 2, 37, _VKR_OFF_C)
   _write_vertex_attr(attr_desc, 3, 98, _VKR_OFF_TEX)
   _write_vertex_attr(attr_desc, 4, 106, _VKR_OFF_NX)
   _write_vertex_attr(attr_desc, 5, 109, _VKR_OFF_TX)
   _write_vertex_attr(attr_desc, 6, 103, _VKR_OFF_U2)
}

mut _shader_ui_source_root_cache = ""

fn _pipe_deep_trace_enabled(): bool {
   ui_profile.debug_deep_enabled()
}

fn _pipe_log_ms(str: stage, any: t0): f64 {
   if(!_pipe_deep_trace_enabled()){ return 0.0 }
   def ms = ui_profile.elapsed_ms(t0)
   ui_profile.print_line("vk:pipe", stage + "=" + to_str(ms) + "ms")
   ms
}

fn _shader_ui_dir(): str { "etc/assets/shaders/ui" }

fn _shader_ui_source_root(): str {
   if(is_str(_shader_ui_source_root_cache) && _shader_ui_source_root_cache.len > 0){ return _shader_ui_source_root_cache }
   def env_dir = common.env_trim("NY_UI_SHADER_DIR")
   if(env_dir.len > 0 && osfs.is_dir(env_dir)){
      _shader_ui_source_root_cache = ospath.normalize(env_dir)
      return _shader_ui_source_root_cache
   }
   def repo_dir = ospath.resolve_repo_asset(_shader_ui_dir())
   if(osfs.is_dir(repo_dir)){
      _shader_ui_source_root_cache = ospath.normalize(repo_dir)
      return _shader_ui_source_root_cache
   }
   _shader_ui_source_root_cache = repo_dir
   _shader_ui_source_root_cache
}

fn _shader_ui_source_path(str: name): str { ospath.join(_shader_ui_source_root(), name) }

fn _shader_hash32(any: x): int {
   if(!is_str(x)){ return 0 }
   mut h = 2166136261
   def n = x.len
   mut i = 0
   while(i < n){
      h = band(bxor(h, load8(x, i)) * 16777619, 2147483647)
      i += 1
   }
   h
}

fn _shader_ui_source_text(str: name): str {
   def path = _shader_ui_source_path(name)
   def trace_shader = _shader_trace_enabled()
   if(file_exists(path)){
      def res = file_read(path)
      if(is_ok(res)){
         def txt = unwrap(res)
         if(is_str(txt) && txt.len > 0){
            if(trace_shader){ ui_profile.print_line("vk:shader", "source=" + path + " bytes=" + to_str(txt.len) + " hash=0x" + to_hex(_shader_hash32(txt))) }
            return txt
         }
      }
   }
   ui_profile.print_line("vk:shader", "missing source=" + path + " root=" + _shader_ui_source_root())
   ""
}

fn _shader_trace_enabled(): bool {
   ui_profile.env_truthy_cached("NY_UI_SHADER_TRACE")
}

comptime template _shader_cache_path_getter(name, file_name){
   fn ${name}(): str { ospath.join(ospath.cache_dir(), file_name) }
}

comptime emit _shader_cache_path_getter(_shader_points_spv, "nytrix_lit.vert.spv")
comptime emit _shader_cache_path_getter(_shader_frag_spv, "nytrix_lit.frag.spv")
comptime emit _shader_cache_path_getter(_shader_fast_frag_spv, "nytrix_lit_fast.frag.spv")
comptime emit _shader_cache_path_getter(_shader_fast_env_frag_spv, "nytrix_lit_fast_env.frag.spv")
comptime emit _shader_cache_path_getter(_shader_sdf_spv, "nytrix_sdf.vert.spv")
comptime emit _shader_cache_path_getter(_shader_sky_vert_spv, "nytrix_sky.vert.spv")
comptime emit _shader_cache_path_getter(_shader_sky_frag_spv, "nytrix_sky.frag.spv")
comptime emit _shader_cache_path_getter(_shader_circle_spv, "nytrix_circle.frag.spv")
comptime emit _shader_cache_path_getter(_shader_ring_spv, "nytrix_ring.frag.spv")
comptime emit _shader_cache_path_getter(_shader_rounded_rect_spv, "nytrix_rounded_rect.frag.spv")

fn _compile_shader_spv(str: source, str: stage_ext, str: out_spv): bool {
   def tmp_src = ospath.join(ospath.temp_dir(), f"ny_shader_auto_{to_str(ticks())}_{stage_ext}.{stage_ext}")
   if(!_write_tmp_text_file(tmp_src, source)){ return false }
   if(file_exists(out_spv)){ _ = proc.run("rm", ["rm", "-f", out_spv]) }
   def rc = proc.run("glslc", ["glslc", "-fshader-stage=" + stage_ext, tmp_src, "-o", out_spv])
   match file_remove(tmp_src){ ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
   if(rc != 0){ return false }
   file_exists(out_spv)
}

fn _shader_recompile_forced(): bool {
   ui_profile.env_truthy_cached("NY_UI_SHADER_RECOMPILE")
}

fn _shader_compile_cached(str: source, str: stage_ext, str: out_spv): bool {
   def sig = stage_ext + ":" + to_hex(_shader_hash32(source))
   def sig_path = out_spv + ".hash"
   if(!_shader_recompile_forced() && file_exists(out_spv) && file_exists(sig_path)){
      def res = file_read(sig_path)
      if(is_ok(res) && unwrap(res) == sig){ return true }
   }
   if(!_compile_shader_spv(source, stage_ext, out_spv)){ return false }
   _ = file_write(sig_path, sig)
   true
}

fn _ensure_sdf_shader_binary(): bool {
   def _t_sdf = _pipe_deep_trace_enabled() ? ticks() : 0
   def vert_src_sdf = _shader_ui_source_text("sdf.vert.glsl")
   if(vert_src_sdf.len <= 0){ return false }
   if(!_shader_compile_cached(vert_src_sdf, "vert", _shader_sdf_spv())){ return false }
   _pipe_log_ms("compile_sdf_vert", _t_sdf)
   true
}

fn _ensure_sky_shader_binaries(): bool {
   def vert_src_sky = _shader_ui_source_text("sky.vert.glsl")
   if(vert_src_sky.len <= 0){ return false }
   def _t_sky_vert = _pipe_deep_trace_enabled() ? ticks() : 0
   if(!_shader_compile_cached(vert_src_sky, "vert", _shader_sky_vert_spv())){ return false }
   _pipe_log_ms("compile_sky_vert", _t_sky_vert)
   def frag_src_sky = _shader_ui_source_text("sky.frag.glsl")
   if(frag_src_sky.len <= 0){ return false }
   def _t_sky_frag = _pipe_deep_trace_enabled() ? ticks() : 0
   if(!_shader_compile_cached(frag_src_sky, "frag", _shader_sky_frag_spv())){ return false }
   _pipe_log_ms("compile_sky_frag", _t_sky_frag)
   true
}

fn _write_tmp_text_file(any: path, any: content): bool {
   if(!path || !is_str(path)){ return false }
   def fd_res = sys.sys_open(path, bor(bor(1, 64), 512), 420)
   if(is_err(fd_res)){ return false }
   def fd = unwrap(fd_res)
   mut ok = true
   if(content && is_str(content)){
      def n = content.len
      def wr = sys.sys_write(fd, content, n)
      ok = is_ok(wr) && unwrap_or(wr, -1) == n
   }
   match sys.sys_close(fd){
      ok(ignoredok) -> { ignoredok }
      err(ignorederr) -> { ignorederr }
   }
   ok
}

fn compile_glsl_to_spirv(str: source, str: stage_ext): any {
   "Compiles GLSL source string to SPIR-V bytes using glslc."
   def tmp_src = ospath.join(ospath.temp_dir(), f"ny_shader_custom_{to_str(ticks())}.{stage_ext}")
   def tmp_spv = f"{tmp_src}.spv"
   if(!_write_tmp_text_file(tmp_src, source)){ return 0 }
   def rc = proc.run("glslc", ["glslc", f"-fshader-stage={stage_ext}", tmp_src, "-o", tmp_spv])
   match file_remove(tmp_src){ ok(ignoredok) -> { ignoredok } err(ignorederr) -> { ignorederr } }
   if(rc != 0){ return 0 }
   def res = file_read(tmp_spv)
   match file_remove(tmp_spv){ ok(ignoredok2) -> { ignoredok2 } err(ignorederr2) -> { ignorederr2 } }
   if(is_err(res)){ return 0 }
   unwrap(res)
}

fn create_shader_module_from_source(str: source, str: stage_ext): any {
   "Compiles GLSL source and creates a Vulkan shader module."
   def spirv = compile_glsl_to_spirv(source, stage_ext)
   if(!spirv){ return 0 }
   def size = spirv.len
   mut ci = _pipe_alloc(128)
   store32(ci, 16, 0) ; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store64_h(ci, size, 24)
   store64_h(ci, spirv, 32)
   mut mod_ptr = _pipe_alloc(8)
   if(create_shader_module(_device, ci, 0, mod_ptr) != 0){ return 0 }
   load64(mod_ptr, 0)
}

fn _create_pipeline_ex(any: vert_mod, any: frag_mod, int: topology=3, int: depth_test=1, int: depth_write=1, int: cull_mode=0, int: front_face=0, int: depth_bias=0, int: depth_clamp=0, f64: line_width=1.0, int: blend_enable=1, int: polygon_mode=0): any {
   "Creates a custom graphics pipeline. topology: 3=TRI_LIST, 1=LINE_LIST, 0=POINT_LIST."
   mut main_str = _pipe_alloc(8)
   strcpy(main_str, "main")
   def s1 = VkPipelineShaderStageCreateInfo(1, vert_mod, main_str)
   def s2 = VkPipelineShaderStageCreateInfo(16, frag_mod, main_str)
   mut stages = _pipe_alloc(96)
   memcpy(stages, s1, 48)
   memcpy(stages + 48, s2, 48)
   def pipe_layout = _pipeline_layout
   mut binding_desc = _pipe_alloc(12)
   _write_default_vertex_binding_desc(binding_desc)
   mut attr_desc = _pipe_alloc(112)
   _write_default_vertex_attr_desc(attr_desc)
   def vi = VkPipelineVertexInputStateCreateInfo(1, binding_desc, 7, attr_desc)
   def ia = VkPipelineInputAssemblyStateCreateInfo(topology, 0)
   def viewport_state = VkPipelineViewportStateCreateInfo(1, 0, 1, 0)
   ; Callers pass the front-face convention explicitly. Dedicated glTF mesh
   ; pipelines below account for the negative Vulkan viewport height.
   def rs = VkPipelineRasterizationStateCreateInfo(depth_clamp, 0, polygon_mode, cull_mode, front_face, depth_bias, 0.0, 0.0, 0.0, float(line_width))
   def ms = VkPipelineMultisampleStateCreateInfo(_cfg_msaa, 0, 0.0, 0, 0, 0)
   ; When blend is disabled this behaves like a normal opaque write path.
   mut cba = 0
   if(blend_enable == 0){ cba = VkPipelineColorBlendAttachmentState(0, 1, 7, 0, 1, 7, 0, 15) }
   elif(blend_enable == 2){ cba = VkPipelineColorBlendAttachmentState(1, 6, 7, 0, 1, 7, 0, 15) } else {
      cba = VkPipelineColorBlendAttachmentState(blend_enable, 6, 7, 0, 1, 7, 0, 15)
   }
   def cb = VkPipelineColorBlendStateCreateInfo(0, 0, 1, cba, 0)
   def dss = VkPipelineDepthStencilStateCreateInfo(depth_test, depth_write, 3, 0, 0, 0, 0, 0.0, 1.0)
   mut dyn_states = _pipe_alloc(12)
   store32(dyn_states, 0, 0)
   store32(dyn_states, 1, 4)
   store32(dyn_states, 2, 8) ; line width
   def ds = VkPipelineDynamicStateCreateInfo(3, dyn_states)
   def ci = VkGraphicsPipelineCreateInfo(2,
      stages,
      vi,
      ia,
      0,
      viewport_state,
      rs,
      ms,
      dss,
      cb,
      ds,
      pipe_layout,
      _render_pass,
      0,
      0,
   -1)
   mut pipe_ptr = _pipe_alloc(8)
   if(_shader_trace_enabled()){
      ui_profile.print_line("vk:pipe", "ci_stype=" + to_str(load32(ci, 0)) +
         " vert=" + to_str(vert_mod) +
         " frag=" + to_str(frag_mod) +
         " stage0=" + to_str(load32(stages, 0)) +
         " stage1=" + to_str(load32(stages + 48, 0)) +
         " vi=" + to_str(load32(vi, 0)) +
         " ia=" + to_str(load32(ia, 0)) +
         " vp=" + to_str(load32(viewport_state, 0)) +
         " rs=" + to_str(load32(rs, 0)) +
         " ms=" + to_str(load32(ms, 0)) +
         " dss=" + to_str(load32(dss, 0)) +
         " blend=" + to_str(load32(cb, 0)) +
      " dyn=" + to_str(load32(ds, 0)))
   }
   if(create_graphics_pipelines(_device, 0, 1, ci, 0, pipe_ptr) != 0){ return 0 }
   load64(pipe_ptr, 0)
}

fn create_pipeline(any: vert_mod, any: frag_mod, int: topology=3, int: depth_test=1, int: depth_write=1, int: cull_mode=0, int: front_face=0, int: depth_bias=0, int: depth_clamp=0, f64: line_width=1.0): any {
   "Create a Vulkan graphics pipeline from compiled shader modules."
   _create_pipeline_ex(vert_mod,
      frag_mod,
      topology,
      depth_test,
      depth_write,
      cull_mode,
      front_face,
      depth_bias,
      depth_clamp,
      line_width,
   1)
}

fn _vk_eager_pipelines(): bool { common.env_truthy("NY_VK_EAGER_PIPELINES") }

fn _create_mesh_pipeline_with_frag(any: frag_mod, bool: alpha, bool: nocull=false, bool: flip=false): any {
   def cull = nocull ? 0 : 2
   def front = flip ? 1 : 0
   if(alpha){ return _create_pipeline_ex(_vert_module, frag_mod, 3, 1, 0, cull, front, 0, 0, 1.0, 2) }
   _create_pipeline_ex(_vert_module, frag_mod, 3, 1, 1, cull, front, 0, 0, 1.0, 0)
}

fn _create_mesh_pipeline(bool: alpha, bool: nocull=false, bool: flip=false): any { _create_mesh_pipeline_with_frag(_frag_module, alpha, nocull, flip) }

fn _mesh_pipeline_ready_basic(): bool { _device && _pipeline_layout && _render_pass && _vert_module && _frag_module }

fn _ensure_unlit_nocull_pipeline(): bool {
   if(_unlit_nocull_pipeline){ return _unlit_nocull_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _unlit_pipeline = create_pipeline(_vert_module, _frag_module, 3, 0, 0, 0, 0, 0, 0)
   _unlit_nocull_pipeline = _unlit_pipeline
   _unlit_nocull_pipeline != 0
}

fn _ensure_flip_pipeline(): bool {
   if(_flip_pipeline){ return _flip_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _flip_pipeline = create_pipeline(_vert_module, _frag_module, 3, 1, 1, 2, 1, 0, 0)
   if(!_flip_pipeline){ _flip_pipeline = _pipeline }
   _flip_pipeline != 0
}

comptime template _mesh_pipeline_ensure_fn(name, slot, alpha, nocull, flip){
   fn ${name}(): bool {
      if(slot){ return true }
      if(!_mesh_pipeline_ready_basic()){ return false }
      slot = _create_mesh_pipeline(alpha, nocull, flip)
      slot != 0
   }
}

comptime emit _mesh_pipeline_ensure_fn(_ensure_mesh_opaque_pipeline, _mesh_opaque_pipeline, false, false, false)
comptime emit _mesh_pipeline_ensure_fn(_ensure_mesh_opaque_nocull_pipeline, _mesh_opaque_nocull_pipeline, false, true, false)
comptime emit _mesh_pipeline_ensure_fn(_ensure_mesh_opaque_nocull_flip_pipeline, _mesh_opaque_nocull_flip_pipeline, false, true, true)
comptime emit _mesh_pipeline_ensure_fn(_ensure_mesh_alpha_pipeline, _mesh_alpha_pipeline, true, false, false)

fn _ensure_mesh_alpha_flip_pipeline(): bool {
   if(_mesh_alpha_flip_pipeline){ return _mesh_alpha_flip_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _mesh_alpha_flip_pipeline = _create_mesh_pipeline(true, false, true)
   if(!_mesh_alpha_flip_pipeline){ _mesh_alpha_flip_pipeline = _get_mesh_alpha_pipeline() }
   _mesh_alpha_flip_pipeline != 0
}

comptime emit _mesh_pipeline_ensure_fn(_ensure_mesh_alpha_nocull_pipeline, _mesh_alpha_nocull_pipeline, true, true, false)
comptime emit _mesh_pipeline_ensure_fn(_ensure_mesh_alpha_nocull_flip_pipeline, _mesh_alpha_nocull_flip_pipeline, true, true, true)

fn _pipe_eq(any: a, any: b): bool { to_int(a) == to_int(b) }

fn _engine_pipeline_handle(any: p): bool {
   _pipe_eq(p, _pipeline) ||
   _pipe_eq(p, _nocull_pipeline) ||
   _pipe_eq(p, _unlit_pipeline) ||
   _pipe_eq(p, _unlit_nocull_pipeline) ||
   _pipe_eq(p, _flip_pipeline) ||
   _pipe_eq(p, _flip_unlit_pipeline) ||
   _pipe_eq(p, _line_pipeline) ||
   _pipe_eq(p, _point_pipeline) ||
   _pipe_eq(p, _wire_pipeline) ||
   _pipe_eq(p, _circle_pipeline) ||
   _pipe_eq(p, _ring_pipeline) ||
   _pipe_eq(p, _rounded_rect_pipeline) ||
   _pipe_eq(p, _skybox_pipeline) ||
   _pipe_eq(p, _mesh_opaque_pipeline) ||
   _pipe_eq(p, _mesh_opaque_nocull_pipeline) ||
   _pipe_eq(p, _mesh_opaque_nocull_flip_pipeline) ||
   _pipe_eq(p, _mesh_opaque_unlit_pipeline) ||
   _pipe_eq(p, _mesh_opaque_unlit_nocull_pipeline) ||
   _pipe_eq(p, _mesh_opaque_unlit_nocull_flip_pipeline) ||
   _pipe_eq(p, _mesh_fast_opaque_pipeline) ||
   _pipe_eq(p, _mesh_fast_opaque_nocull_pipeline) ||
   _pipe_eq(p, _mesh_fast_opaque_flip_pipeline) ||
   _pipe_eq(p, _mesh_fast_opaque_nocull_flip_pipeline) ||
   _pipe_eq(p, _mesh_fast_env_opaque_pipeline) ||
   _pipe_eq(p, _mesh_fast_env_opaque_nocull_pipeline) ||
   _pipe_eq(p, _mesh_fast_env_opaque_flip_pipeline) ||
   _pipe_eq(p, _mesh_fast_env_opaque_nocull_flip_pipeline) ||
   _pipe_eq(p, _mesh_alpha_pipeline) ||
   _pipe_eq(p, _mesh_alpha_nocull_pipeline) ||
   _pipe_eq(p, _mesh_alpha_nocull_flip_pipeline) ||
   _pipe_eq(p, _mesh_alpha_unlit_pipeline) ||
   _pipe_eq(p, _mesh_alpha_unlit_nocull_pipeline) ||
   _pipe_eq(p, _mesh_alpha_unlit_nocull_flip_pipeline) ||
   _pipe_eq(p, _mesh_alpha_flip_pipeline) ||
   _pipe_eq(p, _mesh_alpha_unlit_flip_pipeline)
}

fn bind_pipeline(any: pipe): any {
   "Selects a graphics pipeline for subsequent draws. Pass 0 to restore default."
   if(!_frame_open){ return 0 }
   mut p = pipe
   if(p == 0){ p = _pipeline }
   if(p == _target_pipeline){ return 0 }
   if(_vertex_offset != _last_flush_offset){
      _flush_reason = 2
      _flush()
   }
   def is_custom = p != 0 && !_engine_pipeline_handle(p)
   _use_custom_pc = is_custom ? 1 : 0
   _target_pipeline = p
   _pc_dirty = true
}

fn push_constants(any: data_ptr, int: size, int: offset=0): any {
   "Pushes raw data to the current pipeline's push constants and caches it for flushes."
   if(!_frame_open || !data_ptr || size <= 0){ return 0 }
   if(offset + size > shader_pc_bytes()){ return 0 }
   def pc_ptr = _use_custom_pc ? _pc_buffer_custom : _pc_buffer
   memcpy(pc_ptr + offset, data_ptr, size)
   _pc_dirty = true
   def cb = load64(_cmd_bufs_slab, _current_frame * 8)
   cmd_push_constants(cb, _pipeline_layout, 1 | 16, offset, size, data_ptr)
}

fn use_custom_push_constants(any: enabled): any {
   "Enables/disables custom push constant mode for custom pipelines."
   _use_custom_pc = enabled ? 1 : 0
}

fn set_custom_push_constants(any: data_ptr, int: size, int: offset=0): any {
   "Sets custom push constant data(call after bind_pipeline with custom pipeline)."
   if(!_frame_open || !data_ptr || size <= 0){ return 0 }
   if(offset + size > shader_pc_bytes()){ return 0 }
   memcpy(_pc_buffer_custom + offset, data_ptr, size)
   _pc_dirty = true
   def cb = load64(_cmd_bufs_slab, _current_frame * 8)
   cmd_push_constants(cb, _pipeline_layout, 1 | 16, offset, size, data_ptr)
}

fn _create_shader_module(str: path): any {
   def res = file_read(path)
   if(is_err(res)){ return 0 }
   def code = unwrap(res)
   def size = code.len
   mut ci = _pipe_alloc(128)
   store32(ci, 16, 0) ; VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
   store32(ci, 0, 8) store32(ci, 0, 12)
   store32(ci, 0, 16)
   store64_h(ci, size, 24)
   store64_h(ci, code, 32) ; pCode
   mut mod_ptr = _pipe_alloc(8)
   def vk_res = create_shader_module(_device, ci, 0, mod_ptr)
   if(vk_res != 0){ return 0 }
   def mod = load64(mod_ptr, 0)
   if(_shader_trace_enabled()){
      ui_profile.print_line("vk:shader", "module path=" + path + " size=" + to_str(size) + " handle=" + to_str(mod))
   }
   mod
}

fn _ensure_shader_binaries(): bool {
   def _t_total = _pipe_deep_trace_enabled() ? ticks() : 0
   def vert_spv = _shader_points_spv()
   def frag_spv = _shader_frag_spv()
   if(is_str(_cfg_vert_spv) && file_exists(_cfg_vert_spv)){ proc.run("cp", ["cp", _cfg_vert_spv, vert_spv]) } else {
      def vert_src = _shader_ui_source_text("lit.vert.glsl")
      if(vert_src.len <= 0){ return false }
      def _t_vert = _pipe_deep_trace_enabled() ? ticks() : 0
      if(!_shader_compile_cached(vert_src, "vert", vert_spv)){ return false }
      _pipe_log_ms("compile_main_vert", _t_vert)
      if(common.env_truthy("NY_VK_EAGER_AUX_SHADERS")){
         if(!_ensure_sdf_shader_binary()){ return false }
         if(!_ensure_sky_shader_binaries()){ return false }
      }
   }
   if(is_str(_cfg_frag_spv) && file_exists(_cfg_frag_spv)){ proc.run("cp", ["cp", _cfg_frag_spv, frag_spv]) } else {
      def frag_src = _shader_ui_source_text("lit.frag.glsl")
      if(frag_src.len <= 0){ return false }
      def _t_frag = _pipe_deep_trace_enabled() ? ticks() : 0
      if(!_shader_compile_cached(frag_src, "frag", frag_spv)){ return false }
      _pipe_log_ms("compile_main_frag", _t_frag)
   }
   _pipe_log_ms("ensure_shader_binaries_total", _t_total)
   file_exists(vert_spv) && file_exists(frag_spv)
}

fn _ensure_fast_frag_shader_binary(): bool {
   def _t0 = _pipe_deep_trace_enabled() ? ticks() : 0
   def fast_src = _shader_ui_source_text("lit_fast.frag.glsl")
   if(fast_src.len <= 0){ return false }
   if(!_shader_compile_cached(fast_src, "frag", _shader_fast_frag_spv())){ return false }
   _pipe_log_ms("compile_fast_frag", _t0)
   true
}

fn _ensure_fast_env_frag_shader_binary(): bool {
   def _t0 = _pipe_deep_trace_enabled() ? ticks() : 0
   def fast_src = replace(_shader_ui_source_text("lit_fast.frag.glsl"), "#version 450", "#version 450\n#define NY_FAST_ENV_ONLY 1")
   if(fast_src.len <= 0){ return false }
   if(!_shader_compile_cached(fast_src, "frag", _shader_fast_env_frag_spv())){ return false }
   _pipe_log_ms("compile_fast_env_frag", _t0)
   true
}

fn _ensure_fast_frag_module(): bool {
   if(_frag_fast_module){ return _frag_fast_module != 0 }
   if(!_device){ return false }
   if(!_ensure_fast_frag_shader_binary()){ return false }
   _frag_fast_module = _create_shader_module(_shader_fast_frag_spv())
   _frag_fast_module != 0
}

fn _ensure_fast_env_frag_module(): bool {
   if(_frag_fast_env_module){ return _frag_fast_env_module != 0 }
   if(!_device){ return false }
   if(!_ensure_fast_env_frag_shader_binary()){ return false }
   _frag_fast_env_module = _create_shader_module(_shader_fast_env_frag_spv())
   _frag_fast_env_module != 0
}

fn _mesh_pipeline_with_frag_ready(): bool { _device && _pipeline_layout && _render_pass && _vert_module }

comptime template _mesh_frag_pipeline_ensure_fn(name, slot, ensure_frag, frag_mod, alpha, nocull, flip){
   fn ${name}(): bool {
      if(slot){ return true }
      if(!_mesh_pipeline_with_frag_ready() || !ensure_frag()){ return false }
      slot = _create_mesh_pipeline_with_frag(frag_mod, alpha, nocull, flip)
      slot != 0
   }
}

comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_opaque_pipeline, _mesh_fast_opaque_pipeline,
_ensure_fast_frag_module, _frag_fast_module, false, false, false)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_opaque_nocull_pipeline, _mesh_fast_opaque_nocull_pipeline,
_ensure_fast_frag_module, _frag_fast_module, false, true, false)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_opaque_flip_pipeline, _mesh_fast_opaque_flip_pipeline,
_ensure_fast_frag_module, _frag_fast_module, false, false, true)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_opaque_nocull_flip_pipeline, _mesh_fast_opaque_nocull_flip_pipeline,
_ensure_fast_frag_module, _frag_fast_module, false, true, true)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_env_opaque_pipeline, _mesh_fast_env_opaque_pipeline,
_ensure_fast_env_frag_module, _frag_fast_env_module, false, false, false)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_env_opaque_nocull_pipeline, _mesh_fast_env_opaque_nocull_pipeline,
_ensure_fast_env_frag_module, _frag_fast_env_module, false, true, false)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_env_opaque_flip_pipeline, _mesh_fast_env_opaque_flip_pipeline,
_ensure_fast_env_frag_module, _frag_fast_env_module, false, false, true)
comptime emit _mesh_frag_pipeline_ensure_fn(
   _ensure_mesh_fast_env_opaque_nocull_flip_pipeline, _mesh_fast_env_opaque_nocull_flip_pipeline,
_ensure_fast_env_frag_module, _frag_fast_env_module, false, true, true)

fn _ensure_wire_pipeline(): bool {
   if(_wire_pipeline){ return _wire_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _wire_pipeline = _create_pipeline_ex(_vert_module,
      _frag_module,
      3,
      1,
      1,
      0,
      0,
      0,
      0,
      1.0,
      1,
   1)
   _wire_pipeline != 0
}

fn _ensure_nocull_pipeline(): bool {
   if(_nocull_pipeline){ return _nocull_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _nocull_pipeline = create_pipeline(_vert_module, _frag_module, 3, 1, 1, 0, 0, 0, 0)
   _nocull_pipeline != 0
}

fn _ensure_line_pipeline(): bool {
   if(_line_pipeline){ return _line_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _line_pipeline = create_pipeline(_vert_module, _frag_module, 1, 1, 1, 0, 0, 0, 0)
   _line_pipeline != 0
}

fn _ensure_point_pipeline(): bool {
   if(_point_pipeline){ return _point_pipeline != 0 }
   if(!_mesh_pipeline_ready_basic()){ return false }
   _point_pipeline = create_pipeline(_vert_module, _frag_module, 0, 1, 1, 0, 0, 0, 0)
   _point_pipeline != 0
}

fn _ensure_circle_pipeline(): bool {
   if(_circle_pipeline){ return _circle_pipeline != 0 }
   if(!_device || !_pipeline_layout || !_render_pass){ return false }
   if(!_ensure_sdf_shader_binary()){ return false }
   def frag_circle_src = _shader_ui_source_text("circle.frag.glsl")
   if(frag_circle_src.len <= 0){ return false }
   if(!_shader_compile_cached(frag_circle_src, "frag", _shader_circle_spv())){ return false }
   def vert_sdf_mod = _create_shader_module(_shader_sdf_spv())
   def frag_circle_mod = _create_shader_module(_shader_circle_spv())
   if(!vert_sdf_mod || !frag_circle_mod){ return false }
   _circle_pipeline = create_pipeline(vert_sdf_mod, frag_circle_mod, 3, 1, 1, 0, 0, 0, 0)
   _circle_pipeline != 0
}

fn _ensure_ring_pipeline(): bool {
   if(_ring_pipeline){ return _ring_pipeline != 0 }
   if(!_device || !_pipeline_layout || !_render_pass){ return false }
   if(!_ensure_sdf_shader_binary()){ return false }
   def _t0 = _pipe_deep_trace_enabled() ? ticks() : 0
   def frag_ring_src = _shader_ui_source_text("ring.frag.glsl")
   if(frag_ring_src.len <= 0){ return false }
   if(!_shader_compile_cached(frag_ring_src, "frag", _shader_ring_spv())){ return false }
   def vert_sdf_mod = _create_shader_module(_shader_sdf_spv())
   def frag_ring_mod = _create_shader_module(_shader_ring_spv())
   if(!vert_sdf_mod || !frag_ring_mod){ return false }
   _ring_pipeline = create_pipeline(vert_sdf_mod, frag_ring_mod, 3, 1, 1, 0, 0, 0, 0)
   _pipe_log_ms("ensure_ring_pipeline", _t0)
   _ring_pipeline != 0
}

fn _ensure_rounded_rect_pipeline(): bool {
   if(_rounded_rect_pipeline){ return _rounded_rect_pipeline != 0 }
   if(!_device || !_pipeline_layout || !_render_pass){ return false }
   if(!_ensure_sdf_shader_binary()){ return false }
   def _t0 = _pipe_deep_trace_enabled() ? ticks() : 0
   def frag_src = _shader_ui_source_text("rounded_rect.frag.glsl")
   if(frag_src.len <= 0){ return false }
   if(!_shader_compile_cached(frag_src, "frag", _shader_rounded_rect_spv())){ return false }
   def vert_sdf_mod = _create_shader_module(_shader_sdf_spv())
   def frag_mod = _create_shader_module(_shader_rounded_rect_spv())
   if(!vert_sdf_mod || !frag_mod){ return false }
   _rounded_rect_pipeline = create_pipeline(vert_sdf_mod, frag_mod, 3, 1, 1, 0, 0, 0, 0)
   _pipe_log_ms("ensure_rounded_rect_pipeline", _t0)
   _rounded_rect_pipeline != 0
}

fn _ensure_skybox_pipeline(): bool {
   if(_skybox_pipeline){ return _skybox_pipeline != 0 }
   if(!_device || !_pipeline_layout || !_render_pass){ return false }
   def _t0 = _pipe_deep_trace_enabled() ? ticks() : 0
   if(!_ensure_sky_shader_binaries()){ return false }
   def sky_v_mod = _create_shader_module(_shader_sky_vert_spv())
   def sky_f_mod = _create_shader_module(_shader_sky_frag_spv())
   if(!sky_v_mod || !sky_f_mod){ return false }
   _skybox_pipeline = create_pipeline(sky_v_mod, sky_f_mod, 3, 1, 0, 0, 0, 0, 0)
   _pipe_log_ms("ensure_skybox_pipeline", _t0)
   _skybox_pipeline != 0
}

fn _create_graphics_layouts(): bool {
   def scratch = _pipe_alloc(64)
   def dsl_ptr = scratch
   def ubo_ptr = scratch + 8
   def pc_range = scratch + 16
   def dsl_arr = scratch + 32
   def layout_ptr = scratch + 48
   def tex_binding = VkDescriptorSetLayoutBinding(0,
      _vk_descriptor_combined_image_sampler(),
      _max_textures_value(),
      _vk_shader_stage_fragment(),
   0)
   def tex_ci = VkDescriptorSetLayoutCreateInfo(1, tex_binding)
   if(_shader_trace_enabled()){
      ui_profile.print_line("vk:layout", "tex_ci_stype=" + to_str(load32(tex_ci, 0)) +
      " tex_count=" + to_str(load32(tex_binding, 8)))
   }
   if(create_descriptor_set_layout(_device, tex_ci, 0, dsl_ptr) != 0){ free(scratch) return false }
   _descriptor_set_layout = load64(dsl_ptr, 0)
   def ubo_binding = VkDescriptorSetLayoutBinding(0,
      _vk_descriptor_uniform_buffer(),
      1,
      _vk_shader_stage_vertex() | _vk_shader_stage_fragment(),
   0)
   def ubo_ci = VkDescriptorSetLayoutCreateInfo(1, ubo_binding)
   if(_shader_trace_enabled()){
      ui_profile.print_line("vk:layout", "ubo_ci_stype=" + to_str(load32(ubo_ci, 0)) +
         " ubo_type=" + to_str(load32(ubo_binding, 4)) +
      " ubo_stages=" + to_str(load32(ubo_binding, 12)))
   }
   if(create_descriptor_set_layout(_device, ubo_ci, 0, ubo_ptr) != 0){ free(scratch) return false }
   _descriptor_set_layout_ubo = load64(ubo_ptr, 0)
   store32(pc_range, 1 | 16, 0) ; STAGE_VERTEX | STAGE_FRAGMENT
   store32(pc_range, 0, 4)
   store32(pc_range, shader_pc_bytes(), 8) ; push-constant bytes
   store64_h(dsl_arr, _descriptor_set_layout, 0)
   store64_h(dsl_arr, _descriptor_set_layout_ubo, 8)
   def layout_ci = VkPipelineLayoutCreateInfo(2, dsl_arr, 1, pc_range)
   if(_shader_trace_enabled()){
      ui_profile.print_line("vk:layout", "layout_ci_stype=" + to_str(load32(layout_ci, 0)) +
         " pc_stage=" + to_str(load32(pc_range, 0)) +
         " pc_off=" + to_str(load32(pc_range, 4)) +
      " pc_size=" + to_str(load32(pc_range, 8)))
   }
   def pl_res = create_pipeline_layout(_device, layout_ci, 0, layout_ptr)
   if(pl_res != 0){ free(scratch) return false }
   _pipeline_layout = load64(layout_ptr, 0)
   free(scratch)
   true
}

fn _create_default_lit_pipeline(): any {
   _create_pipeline_ex(_vert_module,
      _frag_module,
      3,
      1,
      1,
      2,
      0,
      0,
      0,
      1.0,
   1)
}

fn _create_eager_core_pipelines(): bool {
   mut _t = _pipe_deep_trace_enabled() ? ticks() : 0
   _flip_pipeline = _create_pipeline_ex(_vert_module,
      _frag_module,
      3,
      1,
      1,
      2,
      1,
      0,
      0,
      1.0,
   1)
   if(!_flip_pipeline){ _flip_pipeline = _pipeline }
   _pipe_log_ms("create_flip_pipeline", _t)
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   _nocull_pipeline = create_pipeline(_vert_module, _frag_module, 3, 1, 1, 0, 0, 0, 0)
   _pipe_log_ms("create_nocull_pipeline", _t)
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   _unlit_pipeline = create_pipeline(_vert_module, _frag_module, 3, 0, 0, 0, 0, 0, 0)
   if(_unlit_pipeline){ _unlit_nocull_pipeline = _unlit_pipeline }
   _flip_unlit_pipeline = _create_pipeline_ex(_vert_module,
      _frag_module,
      3,
      1,
      1,
      2,
      1,
      0,
      0,
      1.0,
   1)
   if(!_flip_unlit_pipeline){ _flip_unlit_pipeline = _unlit_pipeline }
   _pipe_log_ms("create_unlit_pipeline", _t)
   true
}

fn _create_eager_mesh_pipelines(): bool {
   def _t = _pipe_deep_trace_enabled() ? ticks() : 0
   _mesh_opaque_pipeline = _create_mesh_pipeline(false, false, false)
   _mesh_opaque_nocull_pipeline = _create_mesh_pipeline(false, true, false)
   _mesh_opaque_nocull_flip_pipeline = _create_mesh_pipeline(false, true, true)
   _mesh_opaque_unlit_pipeline = _create_mesh_pipeline(false, false, false)
   _mesh_opaque_unlit_nocull_pipeline = _create_mesh_pipeline(false, true, false)
   _mesh_opaque_unlit_nocull_flip_pipeline = _create_mesh_pipeline(false, true, true)
   _mesh_alpha_pipeline = _create_mesh_pipeline(true, false, false)
   _mesh_alpha_flip_pipeline = _create_mesh_pipeline(true, false, true)
   if(!_mesh_alpha_flip_pipeline){ _mesh_alpha_flip_pipeline = _mesh_alpha_pipeline }
   _mesh_alpha_nocull_pipeline = _create_mesh_pipeline(true, true, false)
   _mesh_alpha_nocull_flip_pipeline = _create_mesh_pipeline(true, true, true)
   _mesh_alpha_unlit_pipeline = _create_mesh_pipeline(true, false, false)
   _mesh_alpha_unlit_flip_pipeline = _create_mesh_pipeline(true, false, true)
   if(!_mesh_alpha_unlit_flip_pipeline){ _mesh_alpha_unlit_flip_pipeline = _mesh_alpha_unlit_pipeline }
   _mesh_alpha_unlit_nocull_pipeline = _create_mesh_pipeline(true, true, false)
   _mesh_alpha_unlit_nocull_flip_pipeline = _create_mesh_pipeline(true, true, true)
   _pipe_log_ms("create_mesh_alpha_opaque_pipelines", _t)
   true
}

fn _create_eager_primitive_pipelines(): bool {
   mut _t = _pipe_deep_trace_enabled() ? ticks() : 0
   def _ignored_line = _ensure_line_pipeline()
   _pipe_log_ms("create_line_pipeline", _t)
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   def _ignored_point = _ensure_point_pipeline()
   _pipe_log_ms("create_point_pipeline", _t)
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   def _ignored_wire = _ensure_wire_pipeline()
   _pipe_log_ms("create_wire_pipeline", _t)
   true
}

fn _create_graphics_pipeline(): bool {
   def _t_total = _pipe_deep_trace_enabled() ? ticks() : 0
   mut _t = _pipe_deep_trace_enabled() ? ticks() : 0
   if(!_ensure_shader_binaries()){
      ui_profile.print_line("gfx:vulkan", "shader binaries failed")
      return false
   }
   _pipe_log_ms("ensure_shader_binaries", _t)
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   _vert_module = _create_shader_module(_shader_points_spv())
   _frag_module = _create_shader_module(_shader_frag_spv())
   _pipe_log_ms("create_main_shader_modules", _t)
   if(!_vert_module || !_frag_module){ return false }
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   if(!_create_graphics_layouts()){ return false }
   _pipe_log_ms("create_layouts", _t)
   _t = _pipe_deep_trace_enabled() ? ticks() : 0
   _pipeline = _create_default_lit_pipeline()
   _pipe_log_ms("create_lit_pipeline", _t)
   if(!_pipeline){ return false }
   if(_debug_gfx_enabled){ _dbg_handle("pipeline", _pipeline) }
   if(!_vk_eager_pipelines()){
      _pipe_log_ms("create_graphics_pipeline_total", _t_total)
      return true
   }
   def _ignored_eager_core = _create_eager_core_pipelines()
   def _ignored_eager_mesh = _create_eager_mesh_pipelines()
   def _ignored_eager_primitive = _create_eager_primitive_pipelines()
   _pipe_log_ms("create_graphics_pipeline_total", _t_total)
   true
}

fn _get_default_pipeline(): any { _pipeline }

fn _get_nocull_pipeline(): any {
   if(!_nocull_pipeline){ _ = _ensure_nocull_pipeline() }
   _nocull_pipeline
}

fn _pipeline_or(any: pipe, any: fallback): any {
   if(pipe){ return pipe }
   fallback
}

fn _get_unlit_nocull_pipeline(): any {
   if(!_unlit_nocull_pipeline){ _ = _ensure_unlit_nocull_pipeline() }
   _pipeline_or(_unlit_nocull_pipeline, _unlit_pipeline)
}

fn _get_flip_pipeline(): any {
   if(!_flip_pipeline){ _ = _ensure_flip_pipeline() }
   _pipeline_or(_flip_pipeline, _pipeline)
}

fn _get_flip_unlit_pipeline(): any {
   if(!_flip_unlit_pipeline){ _flip_unlit_pipeline = _get_flip_pipeline() }
   _pipeline_or(_flip_unlit_pipeline, _get_unlit_nocull_pipeline())
}

fn _fallback_default_pipeline(): any { _pipeline }

fn _fallback_nocull_pipeline(): any { _pipeline_or(_get_nocull_pipeline(), _pipeline) }

fn _fallback_flip_pipeline(): any { _pipeline_or(_get_flip_pipeline(), _pipeline) }

fn _fallback_mesh_alpha_nocull_flip_pipeline(): any { _pipeline_or(_get_mesh_alpha_flip_pipeline(), _get_mesh_alpha_pipeline()) }

fn _get_mesh_opaque_pipeline(): any {
   if(!_mesh_opaque_pipeline){ _ = _ensure_mesh_opaque_pipeline() }
   _pipeline_or(_mesh_opaque_pipeline, _fallback_default_pipeline())
}

fn _get_mesh_opaque_nocull_pipeline(): any {
   if(!_mesh_opaque_nocull_pipeline){ _ = _ensure_mesh_opaque_nocull_pipeline() }
   _pipeline_or(_mesh_opaque_nocull_pipeline, _fallback_nocull_pipeline())
}

fn _get_mesh_opaque_nocull_flip_pipeline(): any {
   if(!_mesh_opaque_nocull_flip_pipeline){ _ = _ensure_mesh_opaque_nocull_flip_pipeline() }
   _pipeline_or(_mesh_opaque_nocull_flip_pipeline, _fallback_flip_pipeline())
}

fn _get_mesh_opaque_unlit_pipeline(): any { _get_mesh_opaque_pipeline() }

fn _get_mesh_opaque_unlit_nocull_pipeline(): any { _get_mesh_opaque_nocull_pipeline() }

fn _get_mesh_opaque_unlit_nocull_flip_pipeline(): any { _get_mesh_opaque_nocull_flip_pipeline() }

fn _get_mesh_fast_opaque_pipeline(): any {
   if(!_mesh_fast_opaque_pipeline){ _ = _ensure_mesh_fast_opaque_pipeline() }
   _pipeline_or(_mesh_fast_opaque_pipeline, _get_mesh_opaque_pipeline())
}

fn _get_mesh_fast_opaque_nocull_pipeline(): any {
   if(!_mesh_fast_opaque_nocull_pipeline){ _ = _ensure_mesh_fast_opaque_nocull_pipeline() }
   _pipeline_or(_mesh_fast_opaque_nocull_pipeline, _get_mesh_opaque_nocull_pipeline())
}

fn _get_mesh_fast_opaque_flip_pipeline(): any {
   if(!_mesh_fast_opaque_flip_pipeline){ _ = _ensure_mesh_fast_opaque_flip_pipeline() }
   _pipeline_or(_mesh_fast_opaque_flip_pipeline, _get_flip_pipeline())
}

fn _get_mesh_fast_opaque_nocull_flip_pipeline(): any {
   if(!_mesh_fast_opaque_nocull_flip_pipeline){ _ = _ensure_mesh_fast_opaque_nocull_flip_pipeline() }
   _pipeline_or(_mesh_fast_opaque_nocull_flip_pipeline, _get_mesh_opaque_nocull_flip_pipeline())
}

fn _get_mesh_fast_env_opaque_pipeline(): any {
   if(!_mesh_fast_env_opaque_pipeline){ _ = _ensure_mesh_fast_env_opaque_pipeline() }
   _pipeline_or(_mesh_fast_env_opaque_pipeline, _get_mesh_fast_opaque_pipeline())
}

fn _get_mesh_fast_env_opaque_nocull_pipeline(): any {
   if(!_mesh_fast_env_opaque_nocull_pipeline){ _ = _ensure_mesh_fast_env_opaque_nocull_pipeline() }
   _pipeline_or(_mesh_fast_env_opaque_nocull_pipeline, _get_mesh_fast_opaque_nocull_pipeline())
}

fn _get_mesh_fast_env_opaque_flip_pipeline(): any {
   if(!_mesh_fast_env_opaque_flip_pipeline){ _ = _ensure_mesh_fast_env_opaque_flip_pipeline() }
   _pipeline_or(_mesh_fast_env_opaque_flip_pipeline, _get_mesh_fast_opaque_flip_pipeline())
}

fn _get_mesh_fast_env_opaque_nocull_flip_pipeline(): any {
   if(!_mesh_fast_env_opaque_nocull_flip_pipeline){ _ = _ensure_mesh_fast_env_opaque_nocull_flip_pipeline() }
   _pipeline_or(_mesh_fast_env_opaque_nocull_flip_pipeline, _get_mesh_fast_opaque_nocull_flip_pipeline())
}

fn _get_mesh_alpha_pipeline(): any {
   if(!_mesh_alpha_pipeline){ _ = _ensure_mesh_alpha_pipeline() }
   _pipeline_or(_mesh_alpha_pipeline, _fallback_default_pipeline())
}

fn _get_mesh_alpha_nocull_pipeline(): any {
   if(!_mesh_alpha_nocull_pipeline){ _ = _ensure_mesh_alpha_nocull_pipeline() }
   _pipeline_or(_mesh_alpha_nocull_pipeline, _fallback_nocull_pipeline())
}

fn _get_mesh_alpha_nocull_flip_pipeline(): any {
   if(!_mesh_alpha_nocull_flip_pipeline){ _ = _ensure_mesh_alpha_nocull_flip_pipeline() }
   _pipeline_or(_mesh_alpha_nocull_flip_pipeline, _fallback_mesh_alpha_nocull_flip_pipeline())
}

fn _get_mesh_alpha_unlit_pipeline(): any { _get_mesh_alpha_pipeline() }

fn _get_mesh_alpha_unlit_nocull_pipeline(): any { _get_mesh_alpha_nocull_pipeline() }

fn _get_mesh_alpha_unlit_nocull_flip_pipeline(): any { _get_mesh_alpha_nocull_flip_pipeline() }

fn _get_mesh_alpha_flip_pipeline(): any {
   if(!_mesh_alpha_flip_pipeline){ _ = _ensure_mesh_alpha_flip_pipeline() }
   _pipeline_or(_mesh_alpha_flip_pipeline, _fallback_flip_pipeline())
}

fn _get_mesh_alpha_unlit_flip_pipeline(): any { _get_mesh_alpha_flip_pipeline() }
