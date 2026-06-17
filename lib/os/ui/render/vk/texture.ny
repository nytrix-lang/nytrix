;; Keywords: render vulkan gpu texture os ui
;; Vulkan texture upload, cache, sampling, and image-resource lifetime management.
;; References:
;; - std.os.ui.render.vk
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.vk.texture(create_texture, update_texture_rect, bind_texture, bind_default_texture, texture_size, texture_format, texture_descriptor, material_descriptor, destroy_texture, read_framebuffer, blit_buffer, _record_image_readback_to_buffer, _ensure_readback_slab, create_texture_ex, _create_default_texture, _texture_meta, set_texture_debug_meta, set_texture_protected, create_cubemap, draw_skybox, bindless_sync_texture_slot, last_created_texture_id, texture_count, tex_job_make, tex_job_queue_make, tex_job_queue_push, tex_job_queue_pop, tex_job_result_make, tex_job_cache_key, tex_job_worker_plan, tex_job_upload_plan, resize, rgba_mip_level_count, rgba_mip_total_bytes, generate_rgba_mips, RECT_PACK_HEURISTIC_BL, RECT_PACK_HEURISTIC_BF, rect_pack_init, rect_pack, srgb_to_linear_chan, linear_to_srgb_chan, linear_to_srgb_u8, image_sample_linear_rgb_uv, env_dir_to_uv, generate_spec_env_slab, generate_env_image, generate_neutral_env_image, generate_compare_visible_env_image, generate_compare_reflect_env_image, generate_studio_env_image, scene_prefers_studio_env, scene_prefers_neutral_env, scene_prefers_compare_reflect_env, scene_prefers_compare_visible_env, scene_prefers_optical_spec_env, scene_prefers_black_visible_env, scene_prefers_gray_proof_bg)
use std.core
use std.core.mem
use std.math
use std.os.ui.render.vk.state
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.vk.vulkan
use std.os.ui.render.vk.buffers (_find_memory_type,
   _ensure_upload_cb,
   _begin_upload_cb,

_submit_upload_cb)

use std.os.ui.render.matrix (mat4_identity)
use std.os.ui.render.vk.draw (_draw_raw_stream_current_material, draw_rect_tex)
use std.os.ui.render.vk.pipeline (bind_pipeline, _ensure_skybox_pipeline)
use std.os.ui.render.vk.renderer (_sync_pc, _flush, set_unlit, set_ortho, set_mvp)
use std.os.ui.render.vk.utils (__vkr_push_vertex, _vkr_bgra_to_rgba_if_needed)
use std.os.ui.render.texture as render_texture
use std.core.str as str
use std.core.common as common

mut _last_synced_skybox_tex = -1
mut _last_synced_skybox_view = 0
mut _last_synced_skybox_sampler = 0
mut _last_created_texture_id = -1

comptime table VkTextureWrapMode {
   33648 -> 1
   33071 -> 2
}

fn _tex_alloc(any size) any {
   def p = zalloc(size)
   if !p { panic("vulkan texture allocation failed") }
   p
}

fn _max_textures_value() int { 4096 }

fn _vk_stype_sampler_create_info() int { 31 }

fn _vk_stype_write_descriptor_set() int { 35 }

fn _vk_stype_image_create_info() int { 14 }

fn _vk_stype_memory_allocate_info() int { 5 }

fn _vk_stype_image_view_create_info() int { 15 }

fn _vk_stype_descriptor_set_allocate_info() int { 34 }

fn _vk_stype_command_buffer_allocate_info() int { 40 }

fn _vk_stype_command_buffer_begin_info() int { 42 }

fn _vk_stype_submit_info() int { 4 }

fn _vk_stype_buffer_create_info() int { 12 }

fn _vk_stype_fence_create_info() int { 8 }

fn _vk_image_aspect_color() int { 0x00000001 }

fn _vk_pipeline_top_of_pipe() int { 0x00000001 }

fn _vk_pipeline_color_attachment_output() int { 0x00000400 }

fn _vk_pipeline_transfer() int { 0x00001000 }

fn _vk_pipeline_fragment_shader() int { 0x00000080 }

fn _vk_access_color_attachment_write() int { 0x00000100 }

fn _vk_access_transfer_read() int { 0x00000800 }

fn _vk_access_transfer_write() int { 0x00001000 }

fn _vk_access_shader_read() int { 0x00000020 }

fn _vk_layout_undefined() int { 0 }

fn _vk_layout_color_attachment_optimal() int { 2 }

fn _vk_layout_shader_read_only() int { 5 }

fn _vk_layout_transfer_src() int { 6 }

fn _vk_layout_transfer_dst() int { 7 }

fn _vk_layout_present_src() int { 1000001002 }

fn _vk_format_r16g16b16a16_sfloat() int { 97 }

fn _vk_image_usage_transfer_src() int { 0x00000001 }

fn _vk_image_usage_transfer_dst() int { 0x00000002 }

fn _vk_image_usage_sampled() int { 0x00000004 }

fn _vk_image_create_cube_compatible() int { 0x00000010 }

fn _vk_buffer_usage_transfer_src() int { 0x00000001 }

fn _vk_sharing_mode_exclusive() int { 0 }

fn _vk_memory_device_local() int { 0x00000001 }

fn _vk_memory_host_visible_coherent() int { 0x00000006 }

fn _vk_descriptor_combined_image_sampler() int { 1 }

fn _vk_command_buffer_one_time_submit() int { 0x00000001 }

fn _copy_upload_bytes(any dst, any src, int n) bool {
   if !dst || !src || n <= 0 { return false }
   __copy_mem(dst, src, n)
   true
}

fn _tex_trace_enabled() bool {
   ui_profile.env_truthy_cached("NY_UI_TEX_TRACE")
}

fn _tex_log(any line) bool {
   ui_profile.print_text(line)
}

fn _tex_debug(any line) bool {
   if !_debug_gfx_enabled { return false }
   _tex_log(line)
}

fn _tex_trace(any line) bool {
   if !_tex_trace_enabled() { return false }
   _tex_log(line)
}

fn _tex_debug_or_trace(any line) bool {
   if !_debug_gfx_enabled && !_tex_trace_enabled() { return false }
   _tex_log(line)
}

fn _tex_fb_trace_enabled() bool {
   ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP") || _debug_gfx_enabled
}

fn _has_live_surface() bool {
   if !_surface { return false }
   def raw = load64_h(_surface, 0)
   raw != 0 && raw != 0x8000000000
}

fn _record_image_readback_to_buffer(any cb,
   any src_image,
   int old_layout,
   any dst_buffer,
   int w,
   int h,
   any barrier,
   any region,
   int restore_stage=_vk_pipeline_color_attachment_output()) bool {
   if !cb || !src_image || !dst_buffer || w <= 0 || h <= 0 || !barrier || !region { return false }
   def src_stage = _vk_pipeline_color_attachment_output() | _vk_pipeline_transfer()
   def src_access = _vk_access_color_attachment_write() | _vk_access_transfer_write()
   VkImageMemoryBarrierColor(barrier,
      src_image,
      src_access,
      _vk_access_transfer_read(),
      old_layout,
   _vk_layout_transfer_src())
   cmd_pipeline_barrier(cb,
      src_stage,
      _vk_pipeline_transfer(),
      0,
      0,
      0,
      0,
      0,
      1,
   barrier)
   memset(region, 0, 56)
   store32(region, _vk_image_aspect_color(), 16)
   store32(region, 1, 28)
   store32(region, w, 44)
   store32(region, h, 48)
   store32(region, 1, 52)
   cmd_copy_image_to_buffer(cb, src_image, _vk_layout_transfer_src(), dst_buffer, 1, region)
   VkImageMemoryBarrierColor(barrier,
      src_image,
      _vk_access_transfer_read(),
      0,
      _vk_layout_transfer_src(),
   old_layout)
   cmd_pipeline_barrier(cb, _vk_pipeline_transfer(), restore_stage, 0, 0, 0, 0, 0, 1, barrier)
   true
}

fn _ensure_readback_slab() bool {
   if !_readback_slab {
      _readback_slab = malloc(272)
      if _readback_slab {
         memset(_readback_slab, 0, 272)
         _readback_ai = _readback_slab
         _readback_cb_p = _readback_slab + 32
         _readback_bi = _readback_slab + 40
         _readback_barrier = _readback_slab + 72
         _readback_region = _readback_slab + 144
         _readback_s_info = _readback_slab + 200
      }
   }
   _readback_slab != 0
}

fn _normalize_filter(any filter) int {
   if !is_int(filter) || filter < 0 { return _cfg_filter ? 1 : 0 }
   return filter ? 1 : 0
}

fn _destroy_texture_image_resources(any view, any image, any memory) any {
   if view { destroy_image_view(_device, view, 0) }
   if image { destroy_image(_device, image, 0) }
   if memory { free_memory(_device, memory, 0) }
   0
}

fn _normalize_wrap_mode(any wrap) int {
   if !is_int(wrap) { return 0 }
   def mode = comptime match VkTextureWrapMode(int(wrap), 0)
   return mode
}

fn _ensure_texture_state_lists() any {
   if !is_list(_textures) { _textures = [] }
   if !is_list(_texture_ds_cache) { _texture_ds_cache = [] }
   if !is_list(_free_texture_ids) { _free_texture_ids = [] }
   0
}

fn _alloc_texture_slot() int {
   _ensure_texture_state_lists()
   while _free_texture_ids.len > 0 {
      def tex_id = int(_free_texture_ids.pop())
      if tex_id >= 0 && tex_id < _textures.len { return tex_id }
   }
   if _textures.len >= _max_textures_value() {
      if !_bindless_overflow_warned {
         _bindless_overflow_warned = true
         _tex_log("[gfx:vulkan] texture slot overflow live=" + to_str(_textures.len) + " max=" + to_str(_max_textures_value()))
      }
      return -1
   }
   return _textures.len
}

fn _store_texture_slot(int tex_id, any tex, any ds, int format) bool {
   _ensure_texture_state_lists()
   if tex_id < 0 || tex_id >= _max_textures_value() { return false }
   if tex_id < _textures.len {
      _textures[tex_id] = tex
      _texture_ds_cache[tex_id] = ds
   } else {
      _textures = _textures.append(tex)
      _texture_ds_cache = _texture_ds_cache.append(ds)
   }
   if _texture_fmt_cache != 0 { store8(_texture_fmt_cache, format, tex_id) }
   true
}

fn rgba_mip_level_count(int w, int h) int { render_texture.rgba_mip_level_count(w, h) }

fn rgba_mip_total_bytes(int w, int h) int { render_texture.rgba_mip_total_bytes(w, h) }

fn generate_rgba_mips(ptr src_pixels, int w, int h, bool copy_single=false) any { render_texture.generate_rgba_mips(src_pixels, w, h, copy_single) }

fn resize(dict img, int new_w, int new_h) any { render_texture.resize(img, new_w, new_h) }
def RECT_PACK_HEURISTIC_BL = render_texture.RECT_PACK_HEURISTIC_BL
def RECT_PACK_HEURISTIC_BF = render_texture.RECT_PACK_HEURISTIC_BF

fn rect_pack_init(any width, any height, any heuristic=0) dict { render_texture.rect_pack_init(width, height, heuristic) }

fn rect_pack(dict ctx, list rects) int { render_texture.rect_pack(ctx, rects) }

fn srgb_to_linear_chan(any x) f64 { render_texture.srgb_to_linear_chan(x) }

fn linear_to_srgb_chan(any x) f64 { render_texture.linear_to_srgb_chan(x) }

fn linear_to_srgb_u8(any x) int { render_texture.linear_to_srgb_u8(x) }

fn image_sample_linear_rgb_uv(any im, any u, any v) list { render_texture.image_sample_linear_rgb_uv(im, u, v) }

fn env_dir_to_uv(any d) list { render_texture.env_dir_to_uv(d) }

fn generate_spec_env_slab(any im, int base_w=256) any { render_texture.generate_spec_env_slab(im, base_w) }

fn generate_env_image(int kind=0, int w=1024, int h=512) any { render_texture.generate_env_image(kind, w, h) }

fn generate_neutral_env_image(int w=1024, int h=512) any { render_texture.generate_neutral_env_image(w, h) }

fn generate_compare_visible_env_image(int w=1024, int h=512) any { render_texture.generate_compare_visible_env_image(w, h) }

fn generate_compare_reflect_env_image(int w=1024, int h=512) any { render_texture.generate_compare_reflect_env_image(w, h) }

fn generate_studio_env_image(int w=1024, int h=512) any { render_texture.generate_studio_env_image(w, h) }

fn scene_prefers_studio_env(any name) bool { render_texture.scene_prefers_studio_env(name) }

fn scene_prefers_neutral_env(any name) bool { render_texture.scene_prefers_neutral_env(name) }

fn scene_prefers_compare_reflect_env(any name) bool { render_texture.scene_prefers_compare_reflect_env(name) }

fn scene_prefers_compare_visible_env(any name) bool { render_texture.scene_prefers_compare_visible_env(name) }

fn scene_prefers_optical_spec_env(any name) bool { render_texture.scene_prefers_optical_spec_env(name) }

fn scene_prefers_black_visible_env(any name) bool { render_texture.scene_prefers_black_visible_env(name) }

fn scene_prefers_gray_proof_bg(any name) bool { render_texture.scene_prefers_gray_proof_bg(name) }

fn _mip_level_count(int w, int h) int { rgba_mip_level_count(w, h) }

fn _generate_rgba_mips(any src_pixels, int w, int h) any { generate_rgba_mips(src_pixels, w, h, false) }

fn _mip_byte_count_rgba(int w, int h) int { rgba_mip_total_bytes(w, h) }

fn _ensure_sampler(any filter, any wrap_s=10497, any wrap_t=10497) any {
   def norm = _normalize_filter(filter)
   def uw = _normalize_wrap_mode(wrap_s)
   def vw = _normalize_wrap_mode(wrap_t)
   if uw == 0 && vw == 0 {
      def cur = norm ? _linear_sampler : _nearest_sampler
      if cur { return cur }
   }
   mut sampler_ci = _tex_alloc(80)
   store32(sampler_ci, _vk_stype_sampler_create_info(), 0)
   store32(sampler_ci, 0, 16)
   store32(sampler_ci, norm, 20)
   store32(sampler_ci, norm, 24)
   store32(sampler_ci, norm, 28)
   store32(sampler_ci, uw, 32)
   store32(sampler_ci, vw, 36)
   store32(sampler_ci, uw, 40)
   store32_f32(sampler_ci, 0.0, 64)
   store32_f32(sampler_ci, 16.0, 68)
   mut sampler_ptr = _tex_alloc(8)
   if create_sampler(_device, sampler_ci, 0, sampler_ptr) != 0 {
      free(sampler_ci) free(sampler_ptr)
      return 0
   }
   def sampler = load64_h(sampler_ptr, 0)
   if uw == 0 && vw == 0 {
      if norm { _linear_sampler = sampler }
      else { _nearest_sampler = sampler }
   }
   free(sampler_ci) free(sampler_ptr)
   sampler
}

fn _texture_sampler(any filter, any wrap_s=10497, any wrap_t=10497) any {
   def sampler = _ensure_sampler(filter, wrap_s, wrap_t)
   sampler
}

fn _upload_image_region(any image, int x, int y, int w, int h, int old_layout) bool {
   if !_upload_cb_ptr || !_upload_cb_arr || !_upload_bi || !_upload_bar1 || !_upload_bar2 || !_upload_region || !_upload_si { return false }
   def cb = _ensure_upload_cb()
   if !cb { return false }
   if !_begin_upload_cb(cb) { return false }
   mut src_access = 0
   mut src_stage = _vk_pipeline_top_of_pipe()
   if old_layout == _vk_layout_shader_read_only() {
      src_access = _vk_access_shader_read()
      src_stage = _vk_pipeline_fragment_shader()
   }
   VkImageMemoryBarrierColor(_upload_bar1,
      image,
      src_access,
      _vk_access_transfer_write(),
      old_layout,
   _vk_layout_transfer_dst())
   cmd_pipeline_barrier(cb, src_stage, _vk_pipeline_transfer(), 0, 0, 0, 0, 0, 1, _upload_bar1)
   memset(_upload_region, 0, 56)
   store32(_upload_region, _vk_image_aspect_color(), 16)
   store32(_upload_region, 0, 20)
   store32(_upload_region, 0, 24)
   store32(_upload_region, 1, 28)
   store32(_upload_region, x, 32)
   store32(_upload_region, y, 36)
   store32(_upload_region, 0, 40)
   store32(_upload_region, w, 44)
   store32(_upload_region, h, 48)
   store32(_upload_region, 1, 52)
   cmd_copy_buffer_to_image(cb, _staging_buffer, image, _vk_layout_transfer_dst(), 1, _upload_region)
   _record_upload_shader_read_barrier(cb, image)
   if end_command_buffer(cb) != 0 { return false }
   _submit_upload_cb(cb)
}

fn _record_upload_shader_read_barrier(any cb, any image, int level_count=1) any {
   VkImageMemoryBarrierColor(_upload_bar2,
      image,
      _vk_access_transfer_write(),
      _vk_access_shader_read(),
      _vk_layout_transfer_dst(),
      _vk_layout_shader_read_only(),
      0,
      1,
   level_count)
   cmd_pipeline_barrier(cb,
      _vk_pipeline_transfer(),
      _vk_pipeline_fragment_shader(),
      0,
      0,
      0,
      0,
      0,
      1,
   _upload_bar2)
   0
}

fn bindless_sync_texture_slot(any tex_id) bool {
   "Runs the bindless sync texture slot operation."
   if !_bindless_ds { return false }
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { return false }
   mut tex_obj = _textures.get(tex_id, 0)
   if !is_dict(tex_obj) { return false }
   def tex_view = tex_obj.get("view", 0)
   if !tex_view { return false }
   def tex_sampler = tex_obj.get("sampler", _default_sampler)
   def synced = tex_obj.get("bindless_synced", false)
   def synced_view = tex_obj.get("bindless_synced_view", 0)
   def synced_sampler = tex_obj.get("bindless_synced_sampler", 0)
   def sync_changed = !synced || synced_view != tex_view || synced_sampler != tex_sampler
   if sync_changed || ui_profile.env_truthy_cached("NY_UI_TEX_TRACE_SYNC_ALL") {
      _tex_trace("[vk:tex] sync tex=" + to_str(tex_id) +
         " view=0x" + str.to_hex(tex_view) +
         " sampler=0x" + str.to_hex(tex_sampler) +
         " synced=" + to_str(synced ? 1 : 0) +
         " w=" + to_str(int(tex_obj.get("width", 0))) +
         " h=" + to_str(int(tex_obj.get("height", 0))) +
         " fmt=" + to_str(int(tex_obj.get("format", 0))) +
      " path=" + to_str(tex_obj.get("path", "")))
   }
   if synced && synced_view == tex_view && synced_sampler == tex_sampler { return true }
   mut im_info = _tex_alloc(24)
   mut write = _tex_alloc(64)
   memset(im_info, 0, 24)
   memset(write, 0, 64)
   store64_h(im_info, tex_sampler, 0)
   store64_h(im_info, tex_view, 8)
   store32(im_info, 5, 16)
   store32(write, _vk_stype_write_descriptor_set(), 0)
   store64_h(write, _bindless_ds, 16)
   store32(write, 0, 24)
   store32(write, tex_id, 28)
   store32(write, 1, 32)
   store32(write, _vk_descriptor_combined_image_sampler(), 36)
   store64_h(write, im_info, 40)
   update_descriptor_sets(_device, 1, write, 0, 0)
   free(im_info, write)
   tex_obj["bindless_synced"] = true
   tex_obj["bindless_synced_view"] = tex_view
   tex_obj["bindless_synced_sampler"] = tex_sampler
   _textures[tex_id] = tex_obj
   true
}

fn tex_job_make(int index, any uri, any mime="", any sampler=0, int material=-1, any slot="") dict { render_texture.tex_job_make(index, uri, mime, sampler, material, slot) }

fn tex_job_queue_make() dict { render_texture.tex_job_queue_make() }

fn tex_job_queue_push(any q, any job) dict { render_texture.tex_job_queue_push(q, job) }

fn tex_job_queue_pop(any q) any { render_texture.tex_job_queue_pop(q) }

fn tex_job_result_make(any job, int width, int height, any rgba_or_mips, bool ok=true, any err="") dict { render_texture.tex_job_result_make(job, width, height, rgba_or_mips, ok, err) }

fn tex_job_cache_key(any uri, any mime="", int flags=0) str { render_texture.tex_job_cache_key(uri, mime, flags) }

fn tex_job_worker_plan(int worker_count=4) dict { render_texture.tex_job_worker_plan(worker_count) }

fn tex_job_upload_plan(any results) list { render_texture.tex_job_upload_plan(results) }

fn material_descriptor(any base_tex_id, any normal_tex_id=0, any mr_tex_id=0, any occ_tex_id=0, any emissive_tex_id=0) any {
   "Bindless-only path: returns the shared descriptor set."
   _bindless_ds
}

fn last_created_texture_id() int { return int(_last_created_texture_id) }

fn texture_count() int {
   "Runs the count operation."
   return is_list(_textures) ? int(_textures.len) : 0
}

fn create_texture_ex(int width,
   int height,
   any pixels,
   int format=37,
   any filter=-1,
   any wrap_s=10497,
   any wrap_t=10497,
   bool use_mipmaps=false,
   int prebaked_mip_bytes=0) int {
   "Creates a GPU texture. Format 37=RGBA8, 9=R8."
   _last_created_texture_id = -1
   if width <= 0 || height <= 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex skip invalid size w=" + to_str(width) + " h=" + to_str(height) + " fmt=" + to_str(format))
      return -1
   }
   mut bpp = 4
   if format == 9 { bpp = 1 }
   if format == _vk_format_r16g16b16a16_sfloat() { bpp = 8 }
   mut use_mipmaps_live = use_mipmaps
   if use_mipmaps_live && format != 9 {
      def base_bytes = width * height * bpp
      def mip_bytes = _mip_byte_count_rgba(width, height)
      if mip_bytes > _staging_capacity && base_bytes <= _staging_capacity {
         _tex_debug_or_trace("[gfx:vulkan] mip upload too large; using base level only w=" + to_str(width) +
            " h=" + to_str(height) +
            " mip_bytes=" + to_str(mip_bytes) +
         " staging_bytes=" + to_str(_staging_capacity))
         use_mipmaps_live = false
      }
   }
   def tex_filter = _normalize_filter(filter)
   def tex_sampler = _texture_sampler(tex_filter, wrap_s, wrap_t)
   def mip_levels = use_mipmaps_live ? _mip_level_count(width, height) : 1
   mut img_ci = malloc(88)
   mut img_ptr = malloc(8)
   if !img_ci || !img_ptr { return -1 }
   memset(img_ci, 0, 88)
   store32(img_ci, _vk_stype_image_create_info(), 0)
   store32(img_ci, 0, 16)
   store32(img_ci, 1, 20)
   store32(img_ci, format, 24)
   store32(img_ci, width, 28)
   store32(img_ci, height, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, mip_levels, 40)
   store32(img_ci, 1, 44)
   store32(img_ci, 1, 48)
   store32(img_ci, 0, 52)
   store32(img_ci,
      _vk_image_usage_transfer_dst() | _vk_image_usage_sampled() | (use_mipmaps_live ? _vk_image_usage_transfer_src() : 0),
   56)
   store32(img_ci, _vk_sharing_mode_exclusive(), 60)
   store32(img_ci, 0, 80)
   def r1 = create_image(_device, img_ci, 0, img_ptr)
   if r1 != 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex create_image failed res=" + to_str(r1) + " w=" + to_str(width) + " h=" + to_str(height) + " fmt=" + to_str(format))
      free(img_ci)
      free(img_ptr)
      return -1
   }
   def image = load64_h(img_ptr, 0)
   mut mem_req = malloc(24)
   if !mem_req { return -1 }
   get_image_memory_requirements(_device, image, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   if size <= 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex invalid mem requirements size=" + to_str(int(size)) + " w=" + to_str(width) + " h=" + to_str(height) + " fmt=" + to_str(format))
      destroy_image(_device, image, 0)
      _free_image_create_allocs(img_ci, img_ptr, mem_req)
      return -1
   }
   def mem_idx = _find_memory_type(type_bits, _vk_memory_device_local())
   if mem_idx < 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex no mem type type_bits=0x" + str.to_hex(type_bits) + " req=0x" + str.to_hex(_vk_memory_device_local()))
      destroy_image(_device, image, 0)
      _free_image_create_allocs(img_ci, img_ptr, mem_req)
      return -1
   }
   mut alloc_info = malloc(64)
   memset(alloc_info, 0, 64)
   store32(alloc_info, _vk_stype_memory_allocate_info(), 0)
   store64_h(alloc_info, size, 16)
   store32(alloc_info, mem_idx, 24)
   mut mem_ptr = malloc(8)
   if !alloc_info || !mem_ptr { return -1 }
   _tex_debug("[gfx:vulkan] create_texture_ex alloc mem size=" + to_str(int(size)) + " type=" + to_str(int(mem_idx)))
   def alloc_res = allocate_memory(_device, alloc_info, 0, mem_ptr)
   if alloc_res != 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex allocate_memory failed res=" + to_str(int(alloc_res)) + " bytes=" + to_str(int(size)))
      destroy_image(_device, image, 0)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
      return -1
   }
   def memory = load64_h(mem_ptr, 0)
   def bind_res = bind_image_memory(_device, image, memory, 0)
   if bind_res != 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex bind_image_memory failed res=" + to_str(int(bind_res)))
      _destroy_texture_image_resources(0, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
      return -1
   }
   mut view_ci = _tex_alloc(80)
   store32(view_ci, _vk_stype_image_view_create_info(), 0)
   store64_h(view_ci, image, 24)
   store32(view_ci, 1, 32)
   store32(view_ci, format, 36)
   store32(view_ci, _vk_image_aspect_color(), 56)
   store32(view_ci, mip_levels, 64)
   store32(view_ci, 1, 72)
   mut view_ptr = _tex_alloc(8)
   def r3 = create_image_view(_device, view_ci, 0, view_ptr)
   if r3 != 0 {
      _tex_debug("[gfx:vulkan] create_texture_ex image_view failed res=" + to_str(r3))
      return -1
   }
   def view = load64_h(view_ptr, 0)
   def img_size = width * height * bpp
   mut mip_pixels = 0
   mut upload_pixels = pixels
   mut prebaked_total = int(prebaked_mip_bytes)
   mut expected_mip_total = 0
   if use_mipmaps_live && format != 9 && pixels {
      expected_mip_total = _mip_byte_count_rgba(width, height)
      if prebaked_total >= expected_mip_total {
         mip_pixels = pixels
         upload_pixels = pixels
      } else {
         prebaked_total = 0
         mip_pixels = _generate_rgba_mips(pixels, width, height)
         if mip_pixels { upload_pixels = mip_pixels }
      }
   }
   if pixels && _staging_map {
      if use_mipmaps_live && mip_pixels {
         mut total = prebaked_total
         if total <= 0 { total = expected_mip_total }
         if total > _staging_capacity {
            _tex_debug(f"[gfx:vulkan] staging too small mip_bytes={total} staging_bytes={_staging_capacity}")
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         if !_copy_upload_bytes(_staging_map, upload_pixels, total) {
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         def cb = _ensure_upload_cb()
         if !cb {
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         if !_begin_upload_cb(cb) {
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         VkImageMemoryBarrierColor(_upload_bar1,
            image,
            0,
            _vk_access_transfer_write(),
            _vk_layout_undefined(),
            _vk_layout_transfer_dst(),
            0,
            1,
         mip_levels)
         cmd_pipeline_barrier(cb,
            _vk_pipeline_top_of_pipe(),
            _vk_pipeline_transfer(),
            0,
            0,
            0,
            0,
            0,
            1,
         _upload_bar1)
         def region = _upload_region
         if !region {
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         mut off = 0
         mut cw = width
         mut ch = height
         mut level = 0
         while level < mip_levels {
            memset(region, 0, 56)
            store64_h(region, off, 0)
            store32(region, _vk_image_aspect_color(), 16)
            store32(region, level, 20)
            store32(region, 0, 24)
            store32(region, 1, 28)
            store32(region, 0, 32)
            store32(region, 0, 36)
            store32(region, 0, 40)
            store32(region, cw, 44)
            store32(region, ch, 48)
            store32(region, 1, 52)
            cmd_copy_buffer_to_image(cb, _staging_buffer, image, _vk_layout_transfer_dst(), 1, region)
            off += cw * ch * 4
            cw, ch = max(1, cw >> 1), max(1, ch >> 1)
            level += 1
         }
         _record_upload_shader_read_barrier(cb, image, mip_levels)
         end_command_buffer(cb)
         if !_submit_upload_cb(cb) {
            _tex_debug("[gfx:vulkan] create_texture_ex upload submit failed")
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
      } else {
         if img_size > _staging_capacity {
            _tex_debug(f"[gfx:vulkan] staging too small image_bytes={img_size} staging_bytes={_staging_capacity}")
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         if !_copy_upload_bytes(_staging_map, upload_pixels, img_size) {
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
         if !_upload_image_region(image, 0, 0, width, height, _vk_layout_undefined()) {
            _tex_debug("[gfx:vulkan] create_texture_ex upload_image_region failed w=" + to_str(width) + " h=" + to_str(height) + " bytes=" + to_str(img_size))
            if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
            return -1
         }
      }
   } else {
      if !_upload_image_region(image, 0, 0, width, height, _vk_layout_undefined()) {
         _tex_debug("[gfx:vulkan] create_texture_ex upload_image_region failed empty pixels w=" + to_str(width) + " h=" + to_str(height))
         if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
         return -1
      }
   }
   mut ds = 0
   def tex = {
      "image": image, "view": view, "memory": memory, "ds": ds,
      "width": width, "height": height, "format": format, "bpp": bpp,
      "filter": tex_filter, "sampler": tex_sampler, "bindless_synced": false
   }
   def tex_id = _alloc_texture_slot()
   if tex_id < 0 {
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
      return -1
   }
   if !_store_texture_slot(tex_id, tex, ds, format) {
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
      return -1
   }
   def ret_tex_id = int(tex_id)
   _last_created_texture_id = ret_tex_id
   _tex_trace("[vk:tex] create tex=" + to_str(tex_id) +
      " " + to_str(width) + "x" + to_str(height) +
      " fmt=" + to_str(format) +
      " bpp=" + to_str(bpp) +
      " view=0x" + str.to_hex(view) +
   " sampler=0x" + str.to_hex(tex_sampler))
   if _bindless_ds { bindless_sync_texture_slot(tex_id) }
   _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
   if mip_pixels && mip_pixels != pixels { free(mip_pixels) }
   return ret_tex_id
}

fn update_texture_rect(int tex_id, int x, int y, int w, int h, any pixels) bool {
   "Partially updates a texture's pixel data. Uses pre-allocated buffers."
   if tex_id < 0 || tex_id >= _textures.len { return false }
   def tex_obj = _textures.get(tex_id)
   def image = tex_obj.get("image")
   def bpp = tex_obj.get("bpp", 4)
   if bpp <= 0 { return false }
   def img_size = w * h * bpp
   if img_size > _staging_capacity { return false }
   if !_copy_upload_bytes(_staging_map, pixels, img_size) { return false }
   _upload_image_region(image, x, y, w, h, _vk_layout_shader_read_only())
   if _bindless_ds { bindless_sync_texture_slot(tex_id) }
   true
}

fn create_texture(int width, int height, any pixels) int {
   "Creates a GPU texture from raw pixel data(RGBA8)."
   def raw = create_texture_ex(width, height, pixels, 37, -1)
   def stable = last_created_texture_id()
   return stable >= 0 ? stable : raw
}

fn _init_bindless_descriptor_set(any default_view) bool {
   if _bindless_ds { return true }
   mut dsl_ptr = _tex_alloc(8)
   store64_h(dsl_ptr, _descriptor_set_layout, 0)
   mut alloc_ds = _tex_alloc(40)
   store32(alloc_ds, _vk_stype_descriptor_set_allocate_info(), 0)
   store64_h(alloc_ds, _descriptor_pool, 16)
   store32(alloc_ds, 1, 24)
   store64_h(alloc_ds, dsl_ptr, 32)
   mut ds_ptr = _tex_alloc(8)
   if allocate_descriptor_sets(_device, alloc_ds, ds_ptr) != 0 {
      free(dsl_ptr, alloc_ds, ds_ptr)
      return false
   }
   _bindless_ds = load64_h(ds_ptr, 0)
   mut infos = _tex_alloc(24 * _max_textures_value())
   mut i = 0
   while i < _max_textures_value() {
      def off = infos + i * 24
      store64_h(off, _default_sampler, 0)
      store64_h(off, default_view, 8)
      store32(off, 5, 16)
      i += 1
   }
   mut write = _tex_alloc(64)
   mut im_info = _tex_alloc(24)
   memset(write, 0, 64)
   store32(write, _vk_stype_write_descriptor_set(), 0)
   store64_h(write, _bindless_ds, 16)
   store32(write, 0, 24)
   store32(write, 0, 28)
   store32(write, _max_textures_value(), 32)
   store32(write, _vk_descriptor_combined_image_sampler(), 36)
   store64_h(write, infos, 40)
   update_descriptor_sets(_device, 1, write, 0, 0)
   i = 0
   def textures_n = _textures.len
   while i < textures_n {
      mut tex_obj = _textures.get(i, 0)
      if is_dict(tex_obj) {
         def tex_view = tex_obj.get("view", 0)
         def tex_sampler = tex_obj.get("sampler", _default_sampler)
         if tex_view {
            store64_h(im_info, tex_sampler, 0)
            store64_h(im_info, tex_view, 8)
            store32(im_info, 5, 16)
            store32(write, i, 28)
            store32(write, 1, 32)
            store64_h(write, im_info, 40)
            update_descriptor_sets(_device, 1, write, 0, 0)
            tex_obj["bindless_synced"] = true
            _textures[i] = tex_obj
         }
      }
      i += 1
   }
   free(infos, write, im_info, dsl_ptr, alloc_ds, ds_ptr)
   true
}

fn _create_default_texture() bool {
   _default_sampler = _texture_sampler(_cfg_filter)
   if !_default_sampler { return false }
   def pixels = _tex_alloc(4)
   store32(pixels, 0xFFFFFFFF, 0)
   def tex_id = create_texture_ex(1, 1, pixels, 37, _cfg_filter)
   if tex_id == -1 { return false }
   _default_texture = tex_id
   def normal_pixels = _tex_alloc(4)
   store8(normal_pixels, 128, 0)
   store8(normal_pixels, 128, 1)
   store8(normal_pixels, 255, 2)
   store8(normal_pixels, 255, 3)
   def normal_tex_id = create_texture_ex(1, 1, normal_pixels, 37, _cfg_filter)
   if normal_tex_id == -1 { return false }
   _default_normal_texture = normal_tex_id
   def black_pixels = _tex_alloc(4)
   store8(black_pixels, 0, 0)
   store8(black_pixels, 0, 1)
   store8(black_pixels, 0, 2)
   store8(black_pixels, 255, 3)
   def black_tex_id = create_texture_ex(1, 1, black_pixels, 37, _cfg_filter)
   if black_tex_id == -1 { return false }
   _default_black_texture = black_tex_id
   _current_texture_id = tex_id
   _current_tex_index = tex_id
   _batch_texture_id = tex_id
   _batch_tex_index = _current_tex_index
   def tex_obj = _textures.get(tex_id, 0)
   def view = tex_obj.get("view", 0)
   if !view || !_init_bindless_descriptor_set(view) { return false }
   true
}

mut _mvp_dirty = true
mut _model_dirty = true

fn bind_texture(any tex_id) any {
   "Binds bind texture."
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { tex_id = _default_texture }
   if tex_id == _current_texture_id { return 0 }
   if _vertex_offset != _last_flush_offset {
      _flush_reason = 1
      _flush()
   }
   _current_texture_id = tex_id
   _current_tex_index = tex_id
   if _vertex_offset == _last_flush_offset {
      _batch_texture_id = tex_id
      _batch_tex_index = tex_id
   }
   if _bindless_ds { bindless_sync_texture_slot(tex_id) }
   0
}

fn bind_default_texture() any {
   "Binds the renderer's default 1x1 white texture."
   bind_texture(_default_texture)
}

fn texture_size(any tex_id) any {
   "Returns [width, height] for a texture ID, or 0 if invalid."
   if !is_int(tex_id) { tex_id = _default_texture } elif tex_id < 0 || tex_id >= _max_textures_value() {
      if !_bindless_overflow_warned {
         _bindless_overflow_warned = true
      }
      tex_id = _default_texture
   }
   def tex = _textures.get(tex_id, 0)
   if !tex || !is_dict(tex) { return 0 }
   [tex.get("width", 0), tex.get("height", 0)]
}

fn _texture_meta(any tex_id, any key, any fallback) any {
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { return fallback }
   def tex_obj = _textures.get(tex_id, 0)
   if !tex_obj || !is_dict(tex_obj) { return fallback }
   tex_obj.get(key, fallback)
}

fn set_texture_debug_meta(any tex_id, any path="", any cache_key="") bool {
   "Attaches debug-only source metadata to a live texture slot."
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { return false }
   mut tex_obj = _textures.get(tex_id, 0)
   if !tex_obj || !is_dict(tex_obj) { return false }
   if is_str(path) && path.len > 0 { tex_obj["path"] = path }
   if is_str(cache_key) && cache_key.len > 0 { tex_obj["cache_key"] = cache_key }
   _textures[tex_id] = tex_obj
   true
}

fn set_texture_protected(any tex_id, bool protected=true) bool {
   "Marks a texture slot as owned by a long-lived atlas/default resource."
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { return false }
   mut tex_obj = _textures.get(tex_id, 0)
   if !tex_obj || !is_dict(tex_obj) { return false }
   tex_obj["protected"] = protected
   _textures[tex_id] = tex_obj
   true
}

fn texture_format(any tex_id) int {
   "Returns the format of a texture."
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { return 37 }
   if _texture_fmt_cache != 0 { return load8(_texture_fmt_cache, tex_id) }
   def t = _textures.get(tex_id)
   if !t { return 37 }
   t.get("format", 37)
}

fn texture_descriptor(any tex_id) any {
   "Returns the descriptor set for a texture."
   _bindless_ds
}

fn destroy_texture(any tex_id) any {
   "Destroys a texture and frees its GPU resources."
   if !is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len { return 0 }
   if tex_id == _default_texture || tex_id == _default_normal_texture || tex_id == _default_black_texture { return 0 }
   def tex = _textures.get(tex_id, 0)
   if !tex || !is_dict(tex) { return 0 }
   if tex.get("protected", false) { return 0 }
   if _bindless_ds && _default_texture >= 0 {
      def def_tex = _textures.get(_default_texture, 0)
      def def_view = def_tex.get("view", 0)
      if def_view {
         mut im_info = _tex_alloc(24)
         store64_h(im_info, _default_sampler, 0)
         store64_h(im_info, def_view, 8)
         store32(im_info, 5, 16)
         mut write = _tex_alloc(64)
         store32(write, _vk_stype_write_descriptor_set(), 0)
         store64_h(write, _bindless_ds, 16)
         store32(write, 0, 24)
         store32(write, tex_id, 28)
         store32(write, 1, 32)
         store32(write, _vk_descriptor_combined_image_sampler(), 36)
         store64_h(write, im_info, 40)
         update_descriptor_sets(_device, 1, write, 0, 0)
         free(im_info) free(write)
      }
   }
   def img = tex.get("image", 0)
   def view = tex.get("view", 0)
   def mem = tex.get("memory", 0)
   if view { destroy_image_view(_device, view, 0) }
   if img { destroy_image(_device, img, 0) }
   if mem { free_memory(_device, mem, 0) }
   if _current_texture_id == tex_id {
      _current_texture_id = -1
      _current_tex_index = 0
   }
   if _batch_texture_id == tex_id {
      _batch_texture_id = _default_texture
      _batch_tex_index = (_default_texture >= 0) ? _default_texture : 0
   }
   _textures[tex_id] = 0
   _texture_ds_cache[tex_id] = 0
   if _texture_fmt_cache != 0 && tex_id >= 0 && tex_id < _max_textures_value() { store8(_texture_fmt_cache, 0, tex_id) }
   _free_texture_ids = _free_texture_ids.append(tex_id)
   _material_ds_cache = dict(64)
   0
}

fn read_framebuffer() any {
   "Reads the current swapchain image back to CPU memory. Returns {data, width, height, channels} or 0."
   def force_fresh = common.env_truthy("NY_VK_SNAPSHOT_FRESH")
   if !force_fresh && _capture_ready && _capture_pixels && _capture_w > 0 && _capture_h > 0 {
      def res = {"data": _capture_pixels, "width": _capture_w, "height": _capture_h, "bpp": 4}
      _capture_ready = false
      _capture_pixels = 0
      _capture_w = 0
      _capture_h = 0
      return res
   }
   ;; The synchronous fallback path allocates a one-shot command buffer and
   ;; calls vkDeviceWaitIdle from outside the frame graph.  That is fragile on
   ;; X11/Vulkan during resize/close and was the source of trace-time crashes.
   ;; Normal snapshots should call request_frame_capture(), then read the cached
   ;; result on the following frame.  Keep the old path only as an explicit
   ;; diagnostic escape hatch.
   if !common.env_truthy("NY_VK_READBACK_BLOCKING") {
      _capture_request = true
      return 0
   }
   if _frame_open {
      _capture_request = true
      return 0
   }
   if !_device || !_swapchain || _image_index < 0 { return 0 }
   def w, h = _swapchain_extent_w, _swapchain_extent_h
   if w <= 0 || h <= 0 { return 0 }
   def size = w * h * 4
   if size <= 0 || size > (512 * 1024 * 1024) { return 0 }
   if !_staging_buffer || !_staging_map || _staging_capacity < size { return 0 }
   def readback_buf = _staging_buffer
   def mapped_data = _staging_map
   mut cb = 0
   if !_ensure_readback_slab() { return 0 }
   def ai = _readback_ai
   def cb_p = _readback_cb_p
   def bi = _readback_bi
   def barrier = _readback_barrier
   def region = _readback_region
   def s_info = _readback_s_info
   if !ai || !cb_p || !bi || !barrier || !region || !s_info { return 0 }
   memset(ai, 0, 32)
   store32(ai, _vk_stype_command_buffer_allocate_info(), 0)
   store64_h(ai, _command_pool, 16)
   store32(ai, 1, 28)
   memset(cb_p, 0, 8)
   if allocate_command_buffers(_device, ai, cb_p) != 0 { return 0 }
   cb = load64(cb_p, 0)
   if !cb {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   memset(bi, 0, 32)
   store32(bi, _vk_stype_command_buffer_begin_info(), 0)
   store32(bi, _vk_command_buffer_one_time_submit(), 16)
   if begin_command_buffer(cb, bi) != 0 {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   mut old_layout = _vk_layout_present_src()
   if !_has_live_surface() { old_layout = _vk_layout_color_attachment_optimal() }
   def src_image = _swapchain_images.get(_image_index)
   if !_record_image_readback_to_buffer(cb, src_image, old_layout, readback_buf, w, h, barrier, region) {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   if end_command_buffer(cb) != 0 {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   memset(s_info, 0, 72)
   store32(s_info, _vk_stype_submit_info(), 0)
   store32(s_info, 1, 40)
   store64_h(s_info, cb_p, 48)
   if queue_submit(_graphics_queue, 1, s_info, 0) != 0 || device_wait_idle(_device) != 0 {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   if !mapped_data {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   def pixels = malloc(size)
   if !pixels {
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   __copy_mem(pixels, mapped_data, size)
   free_command_buffers(_device, _command_pool, 1, cb_p)
   _vkr_bgra_to_rgba_if_needed(pixels, size, _swapchain_format)
   if _tex_fb_trace_enabled() {
      def p0 = size >= 4 ? (load8(pixels, 0) | (load8(pixels, 1) << 8) | (load8(pixels, 2) << 16) | (load8(pixels, 3) << 24)) : 0
      def pc = size >= 4 ? (((h / 2) * w + (w / 2)) * 4) : 0
      def c0 = (pc >= 0 && pc + 3 < size) ? (load8(pixels, pc) | (load8(pixels, pc + 1) << 8) | (load8(pixels, pc + 2) << 16) | (load8(pixels, pc + 3) << 24)) : 0
      _tex_log("[vk:fb] format=" + to_str(_swapchain_format) +
         " size=" + to_str(w) + "x" + to_str(h) +
         " img=" + to_str(_image_index) +
         " p0=0x" + str.to_hex(p0) +
      " pc=0x" + str.to_hex(c0))
   }
   def res = {"data": pixels, "width": w, "height": h, "bpp": 4}
   res
}

fn blit_buffer(any pixels, int w, int h) any {
   "Blits a raw RGBA8 pixel buffer to the full window."
   if !_frame_open { return 0 }
   def blit_sz = (_blit_tex_id == -1) ? 0 : texture_size(_blit_tex_id)
   if _blit_tex_id == -1 || !is_list(blit_sz) || blit_sz.get(0, 0) != w || blit_sz.get(1, 0) != h {
      if _blit_tex_id != -1 { destroy_texture(_blit_tex_id) }
      _blit_tex_id = create_texture(w, h, pixels)
   } else {
      update_texture_rect(_blit_tex_id, 0, 0, w, h, pixels)
   }
   def last_unlit = _current_is_unlit
   set_unlit(true)
   def ws_w, ws_h = float(_swapchain_extent_w), float(_swapchain_extent_h)
   mut old_mvp = malloc(64)
   if _current_mvp { memcpy(old_mvp, _current_mvp, 64) } else { memset(old_mvp, 0, 64) }
   set_ortho(0.0, ws_w, 0.0, ws_h, -1.0, 1.0)
   draw_rect_tex(0.0, 0.0, ws_w, ws_h, _blit_tex_id, 1.0, 1.0, 1.0, 1.0)
   _flush()
   memcpy(_current_mvp, old_mvp, 64)
   free(old_mvp)
   _mvp_dirty = true
   _pc_dirty = true
   set_unlit(last_unlit != 0)
   0
}

mut _skybox_cube = 0
mut _skybox_cube_tex_id = -1

fn _upload_cubemap_face_dedicated(any image, int face_size, any pixels, int layer) bool {
   if !_graphics_queue || !_command_pool { return false }
   def face_sz = face_size * face_size * 4
   if !is_str(pixels) || pixels.len < face_sz { return false }
   mut buf_ci = _tex_alloc(88)
   store32(buf_ci, _vk_stype_buffer_create_info(), 0)
   store64_h(buf_ci, face_sz, 24)
   store32(buf_ci, _vk_buffer_usage_transfer_src(), 32)
   store32(buf_ci, _vk_sharing_mode_exclusive(), 36)
   mut staging_buf_ptr = _tex_alloc(8)
   def res = create_buffer(_device, buf_ci, 0, staging_buf_ptr)
   if res != 0 {
      free(buf_ci, staging_buf_ptr)
      return false
   }
   def staging_buf = load64_h(staging_buf_ptr, 0)
   mut mem_req = _tex_alloc(24)
   get_buffer_memory_requirements(_device, staging_buf, mem_req)
   def mem_sz = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   def mem_type = _find_memory_type(type_bits,
   _vk_memory_host_visible_coherent())
   mut alloc_info = _tex_alloc(64)
   store32(alloc_info, _vk_stype_memory_allocate_info(), 0)
   store64_h(alloc_info, mem_sz, 16)
   store32(alloc_info, mem_type, 24)
   mut mem_ptr = _tex_alloc(8)
   if allocate_memory(_device, alloc_info, 0, mem_ptr) != 0 {
      free(buf_ci, staging_buf_ptr, mem_req, alloc_info, mem_ptr)
      return false
   }
   def staging_mem = load64_h(mem_ptr, 0)
   bind_buffer_memory(_device, staging_buf, staging_mem, 0)
   mut mapped = _tex_alloc(8)
   if map_memory(_device, staging_mem, 0, face_sz, 0, mapped) != 0 {
      free(buf_ci, staging_buf_ptr, mem_req, alloc_info, mem_ptr, mapped)
      return false
   }
   def map_ptr = load64(mapped, 0)
   __copy_mem(map_ptr, pixels, face_sz)
   unmap_memory(_device, staging_mem)
   free(mapped)
   mut cb_alloc = _tex_alloc(32)
   store32(cb_alloc, _vk_stype_command_buffer_allocate_info(), 0)
   store64_h(cb_alloc, _command_pool, 16)
   store32(cb_alloc, 1, 28)
   mut cb_ptr = _tex_alloc(8)
   allocate_command_buffers(_device, cb_alloc, cb_ptr)
   def cb = load64(cb_ptr, 0)
   mut begin_info = _tex_alloc(32)
   store32(begin_info, _vk_stype_command_buffer_begin_info(), 0)
   store32(begin_info, 1, 16)
   begin_command_buffer(cb, begin_info)
   mut bar = _tex_alloc(72)
   VkImageMemoryBarrierColor(bar,
      image,
      0,
      _vk_access_transfer_write(),
      _vk_layout_undefined(),
      _vk_layout_transfer_dst(),
      layer,
   1)
   cmd_pipeline_barrier(cb, _vk_pipeline_top_of_pipe(), _vk_pipeline_transfer(), 0, 0, 0, 0, 0, 1, bar)
   mut copy_region = _tex_alloc(56)
   store32(copy_region, _vk_image_aspect_color(), 16)
   store32(copy_region, 0, 20)
   store32(copy_region, layer, 24)
   store32(copy_region, 1, 28)
   store32(copy_region, 0, 32)
   store32(copy_region, 0, 36)
   store32(copy_region, 0, 40)
   store32(copy_region, face_size, 44)
   store32(copy_region, face_size, 48)
   store32(copy_region, 1, 52)
   cmd_copy_buffer_to_image(cb, staging_buf, image, _vk_layout_transfer_dst(), 1, copy_region)
   VkImageMemoryBarrierColor(bar,
      image,
      _vk_access_transfer_write(),
      _vk_access_shader_read(),
      _vk_layout_transfer_dst(),
      _vk_layout_shader_read_only(),
      layer,
   1)
   cmd_pipeline_barrier(cb,
      _vk_pipeline_transfer(),
      _vk_pipeline_fragment_shader(),
      0,
      0,
      0,
      0,
      0,
      1,
   bar)
   end_command_buffer(cb)
   mut submit_info = _tex_alloc(72)
   store32(submit_info, _vk_stype_submit_info(), 0)
   store32(submit_info, 1, 40)
   store64_h(submit_info, cb_ptr, 48)
   mut fence_ci = _tex_alloc(24)
   store32(fence_ci, _vk_stype_fence_create_info(), 0)
   mut fence_ptr = _tex_alloc(8)
   create_fence(_device, fence_ci, 0, fence_ptr)
   def fence = load64_h(fence_ptr, 0)
   ;; Check the submit return value and use a finite fence timeout. The
   ;; previous code called wait_for_fences with UINT64_MAX, so a lost device
   ;; or OOM on submit would hang the application forever with no diagnostic.
   def submit_res = queue_submit(_graphics_queue, 1, submit_info, fence)
   if submit_res != 0 {
      _tex_debug("cubemap face submit failed code=" + to_str(submit_res))
      destroy_fence(_device, fence, 0)
      free_command_buffers(_device, _command_pool, 1, cb_ptr)
      free_memory(_device, staging_mem, 0)
      destroy_buffer(_device, staging_buf, 0)
      free(fence_ci, fence_ptr, submit_info, begin_info, cb_alloc, cb_ptr, mem_req, alloc_info, mem_ptr, buf_ci, staging_buf_ptr, bar, copy_region)
      return false
   }
   def wait_res = wait_for_fences(_device, 1, fence_ptr, 1, 5_000_000_000)
   if wait_res != 0 {
      _tex_debug("cubemap face fence wait failed code=" + to_str(wait_res))
      destroy_fence(_device, fence, 0)
      free_command_buffers(_device, _command_pool, 1, cb_ptr)
      free_memory(_device, staging_mem, 0)
      destroy_buffer(_device, staging_buf, 0)
      free(fence_ci, fence_ptr, submit_info, begin_info, cb_alloc, cb_ptr, mem_req, alloc_info, mem_ptr, buf_ci, staging_buf_ptr, bar, copy_region)
      return false
   }
   destroy_fence(_device, fence, 0)
   free_command_buffers(_device, _command_pool, 1, cb_ptr)
   free_memory(_device, staging_mem, 0)
   destroy_buffer(_device, staging_buf, 0)
   free(fence_ci, fence_ptr, submit_info, begin_info, cb_alloc, cb_ptr, mem_req, alloc_info, mem_ptr, buf_ci, staging_buf_ptr, bar, copy_region)
   true
}

fn _free_image_create_allocs(any img_ci, any img_ptr, any mem_req=0, any alloc_info=0, any mem_ptr=0, any view_ci=0, any view_ptr=0) any {
   if img_ci { free(img_ci) }
   if img_ptr { free(img_ptr) }
   if mem_req { free(mem_req) }
   if alloc_info { free(alloc_info) }
   if mem_ptr { free(mem_ptr) }
   if view_ci { free(view_ci) }
   if view_ptr { free(view_ptr) }
   0
}

fn create_cubemap(int face_size, list face_pixels_list) int {
   "Creates create cubemap."
   if face_pixels_list.len != 6 { return -1 }
   def format = 37
   def _face_sz = face_size * face_size * 4
   mut img_ci = _tex_alloc(88)
   store32(img_ci, _vk_stype_image_create_info(), 0)
   store32(img_ci, _vk_image_create_cube_compatible(), 16)
   store32(img_ci, 1, 20)
   store32(img_ci, format, 24)
   store32(img_ci, face_size, 28)
   store32(img_ci, face_size, 32)
   store32(img_ci, 1, 36)
   store32(img_ci, 1, 40)
   store32(img_ci, 6, 44)
   store32(img_ci, 1, 48)
   store32(img_ci, 0, 52)
   store32(img_ci, _vk_image_usage_transfer_dst() | _vk_image_usage_sampled(), 56)
   store32(img_ci, _vk_sharing_mode_exclusive(), 60)
   store32(img_ci, 0, 80)
   mut img_ptr = _tex_alloc(8)
   def r1 = create_image(_device, img_ci, 0, img_ptr)
   if r1 != 0 {
      _free_image_create_allocs(img_ci, img_ptr)
      return -1
   }
   def image = load64_h(img_ptr, 0)
   mut mem_req = _tex_alloc(24)
   get_image_memory_requirements(_device, image, mem_req)
   def mem_sz = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   def mem_idx = _find_memory_type(type_bits, _vk_memory_device_local())
   mut alloc_info = _tex_alloc(64)
   store32(alloc_info, _vk_stype_memory_allocate_info(), 0)
   store64_h(alloc_info, mem_sz, 16)
   store32(alloc_info, mem_idx, 24)
   mut mem_ptr = _tex_alloc(8)
   if allocate_memory(_device, alloc_info, 0, mem_ptr) != 0 {
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
      return -1
   }
   def memory = load64_h(mem_ptr, 0)
   if bind_image_memory(_device, image, memory, 0) != 0 {
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
      return -1
   }
   mut face_i = 0
   while face_i < 6 {
      def pixels = face_pixels_list.get(face_i)
      if !_upload_cubemap_face_dedicated(image, face_size, pixels, face_i) {
         face_i = 6
         _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
         return -1
      }
      face_i += 1
   }
   mut view_ci = _tex_alloc(88)
   store32(view_ci, _vk_stype_image_view_create_info(), 0)
   store64_h(view_ci, image, 24)
   store32(view_ci, 3, 32)
   store32(view_ci, format, 36)
   store32(view_ci, _vk_image_aspect_color(), 56)
   store32(view_ci, 0, 60)
   store32(view_ci, 1, 64)
   store32(view_ci, 0, 68)
   store32(view_ci, 6, 72)
   store32(view_ci, 0, 76)
   mut view_ptr = _tex_alloc(8)
   def r3 = create_image_view(_device, view_ci, 0, view_ptr)
   if r3 != 0 {
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      return -1
   }
   def view = load64_h(view_ptr, 0)
   def tex_filter = _normalize_filter(1)
   def tex_sampler = _texture_sampler(tex_filter)
   mut ds = 0
   def tex = {
      "image": image, "view": view, "memory": memory, "ds": ds,
      "width": face_size, "height": face_size, "format": format, "bpp": 4,
      "filter": tex_filter, "sampler": tex_sampler,
      "bindless_synced": false, "is_cubemap": true
   }
   def tex_id = _alloc_texture_slot()
   if tex_id < 0 {
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      return -1
   }
   if !_store_texture_slot(tex_id, tex, ds, format) {
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      return -1
   }
   if _bindless_ds { bindless_sync_texture_slot(tex_id) }
   _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
   return tex_id
}

fn draw_skybox(int tex_id) bool {
   "Draws draw skybox."
   if tex_id < 0 || tex_id >= _textures.len { return false }
   def tex = _textures.get(tex_id)
   if !is_dict(tex) { return false }
   if !_skybox_pipeline && !_ensure_skybox_pipeline() { return false }
   if !_skybox_pipeline { return false }
   def needs_rebuild = (!_skybox_cube || _skybox_cube_tex_id != tex_id || !_skybox_cube.get("cpu_ptr", 0))
   if needs_rebuild {
      if is_dict(_skybox_cube) {
         def old_ptr = _skybox_cube.get("cpu_ptr", 0)
         if old_ptr { free(old_ptr) }
      }
      def n = 36
      def buf = malloc(n * _VKR_VERT_STRIDE)
      if !buf { return false }
      def c = 0xFFFFFFFF
      __vkr_push_vertex(buf +  0*_VKR_VERT_STRIDE,  1.0,-1.0, 1.0, 0.0,0.0, c, tex_id,  0.0, 0.0, 1.0)
      __vkr_push_vertex(buf +  1*_VKR_VERT_STRIDE, -1.0,-1.0, 1.0, 1.0,0.0, c, tex_id,  0.0, 0.0, 1.0)
      __vkr_push_vertex(buf +  2*_VKR_VERT_STRIDE, -1.0, 1.0, 1.0, 1.0,1.0, c, tex_id,  0.0, 0.0, 1.0)
      __vkr_push_vertex(buf +  3*_VKR_VERT_STRIDE,  1.0,-1.0, 1.0, 0.0,0.0, c, tex_id,  0.0, 0.0, 1.0)
      __vkr_push_vertex(buf +  4*_VKR_VERT_STRIDE, -1.0, 1.0, 1.0, 1.0,1.0, c, tex_id,  0.0, 0.0, 1.0)
      __vkr_push_vertex(buf +  5*_VKR_VERT_STRIDE,  1.0, 1.0, 1.0, 0.0,1.0, c, tex_id,  0.0, 0.0, 1.0)
      __vkr_push_vertex(buf +  6*_VKR_VERT_STRIDE, -1.0,-1.0,-1.0, 0.0,0.0, c, tex_id,  0.0, 0.0,-1.0)
      __vkr_push_vertex(buf +  7*_VKR_VERT_STRIDE,  1.0,-1.0,-1.0, 1.0,0.0, c, tex_id,  0.0, 0.0,-1.0)
      __vkr_push_vertex(buf +  8*_VKR_VERT_STRIDE,  1.0, 1.0,-1.0, 1.0,1.0, c, tex_id,  0.0, 0.0,-1.0)
      __vkr_push_vertex(buf +  9*_VKR_VERT_STRIDE, -1.0,-1.0,-1.0, 0.0,0.0, c, tex_id,  0.0, 0.0,-1.0)
      __vkr_push_vertex(buf + 10*_VKR_VERT_STRIDE,  1.0, 1.0,-1.0, 1.0,1.0, c, tex_id,  0.0, 0.0,-1.0)
      __vkr_push_vertex(buf + 11*_VKR_VERT_STRIDE, -1.0, 1.0,-1.0, 0.0,1.0, c, tex_id,  0.0, 0.0,-1.0)
      __vkr_push_vertex(buf + 12*_VKR_VERT_STRIDE,  1.0,-1.0,-1.0, 0.0,0.0, c, tex_id,  1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 13*_VKR_VERT_STRIDE,  1.0,-1.0, 1.0, 1.0,0.0, c, tex_id,  1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 14*_VKR_VERT_STRIDE,  1.0, 1.0, 1.0, 1.0,1.0, c, tex_id,  1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 15*_VKR_VERT_STRIDE,  1.0,-1.0,-1.0, 0.0,0.0, c, tex_id,  1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 16*_VKR_VERT_STRIDE,  1.0, 1.0, 1.0, 1.0,1.0, c, tex_id,  1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 17*_VKR_VERT_STRIDE,  1.0, 1.0,-1.0, 0.0,1.0, c, tex_id,  1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 18*_VKR_VERT_STRIDE, -1.0,-1.0, 1.0, 0.0,0.0, c, tex_id, -1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 19*_VKR_VERT_STRIDE, -1.0,-1.0,-1.0, 1.0,0.0, c, tex_id, -1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 20*_VKR_VERT_STRIDE, -1.0, 1.0,-1.0, 1.0,1.0, c, tex_id, -1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 21*_VKR_VERT_STRIDE, -1.0,-1.0, 1.0, 0.0,0.0, c, tex_id, -1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 22*_VKR_VERT_STRIDE, -1.0, 1.0,-1.0, 1.0,1.0, c, tex_id, -1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 23*_VKR_VERT_STRIDE, -1.0, 1.0, 1.0, 0.0,1.0, c, tex_id, -1.0, 0.0, 0.0)
      __vkr_push_vertex(buf + 24*_VKR_VERT_STRIDE, -1.0, 1.0,-1.0, 0.0,0.0, c, tex_id,  0.0, 1.0, 0.0)
      __vkr_push_vertex(buf + 25*_VKR_VERT_STRIDE,  1.0, 1.0,-1.0, 1.0,0.0, c, tex_id,  0.0, 1.0, 0.0)
      __vkr_push_vertex(buf + 26*_VKR_VERT_STRIDE,  1.0, 1.0, 1.0, 1.0,1.0, c, tex_id,  0.0, 1.0, 0.0)
      __vkr_push_vertex(buf + 27*_VKR_VERT_STRIDE, -1.0, 1.0,-1.0, 0.0,0.0, c, tex_id,  0.0, 1.0, 0.0)
      __vkr_push_vertex(buf + 28*_VKR_VERT_STRIDE,  1.0, 1.0, 1.0, 1.0,1.0, c, tex_id,  0.0, 1.0, 0.0)
      __vkr_push_vertex(buf + 29*_VKR_VERT_STRIDE, -1.0, 1.0, 1.0, 0.0,1.0, c, tex_id,  0.0, 1.0, 0.0)
      __vkr_push_vertex(buf + 30*_VKR_VERT_STRIDE, -1.0,-1.0, 1.0, 0.0,0.0, c, tex_id,  0.0,-1.0, 0.0)
      __vkr_push_vertex(buf + 31*_VKR_VERT_STRIDE,  1.0,-1.0, 1.0, 1.0,0.0, c, tex_id,  0.0,-1.0, 0.0)
      __vkr_push_vertex(buf + 32*_VKR_VERT_STRIDE,  1.0,-1.0,-1.0, 1.0,1.0, c, tex_id,  0.0,-1.0, 0.0)
      __vkr_push_vertex(buf + 33*_VKR_VERT_STRIDE, -1.0,-1.0, 1.0, 0.0,0.0, c, tex_id,  0.0,-1.0, 0.0)
      __vkr_push_vertex(buf + 34*_VKR_VERT_STRIDE,  1.0,-1.0,-1.0, 1.0,1.0, c, tex_id,  0.0,-1.0, 0.0)
      __vkr_push_vertex(buf + 35*_VKR_VERT_STRIDE, -1.0,-1.0,-1.0, 0.0,1.0, c, tex_id,  0.0,-1.0, 0.0)
      _skybox_cube = {
         "cpu_ptr": buf,
         "count": n
      }
      _skybox_cube_tex_id = tex_id
   }
   if !_skybox_cube { return false }
   if !_bindless_ds || !_pipeline_layout { return false }
   def tex_obj = _textures.get(tex_id, 0)
   mut tex_view = 0
   mut tex_sampler = 0
   if is_dict(tex_obj) { tex_view, tex_sampler = tex_obj.get("view", 0), tex_obj.get("sampler", 0) }
   if tex_id >= 0
   && (tex_id != _last_synced_skybox_tex
      || tex_view != _last_synced_skybox_view
      || tex_sampler != _last_synced_skybox_sampler){
      bindless_sync_texture_slot(tex_id)
      _last_synced_skybox_tex = tex_id
      _last_synced_skybox_view = tex_view
      _last_synced_skybox_sampler = tex_sampler
   }
   def cpu_ptr = _skybox_cube.get("cpu_ptr", 0)
   def count = int(_skybox_cube.get("count", 36))
   if !cpu_ptr || count <= 0 { return false }
   if !_draw_raw_stream_current_material(cpu_ptr, count, _skybox_pipeline) { return false }
   _last_bound_pipe = 0
   _pc_dirty = true
   true
}
