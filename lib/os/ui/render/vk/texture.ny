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
use std.math.crypto.hash as hash
use std.math.float (is_nan, is_inf)
use std.core.str as str
use std.core.common as common
use std.os.prim (env)

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
   if(!p){ panic("vulkan texture allocation failed") }
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
   if(!dst || !src || n <= 0){ return false }
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
   if(!_debug_gfx_enabled){ return false }
   _tex_log(line)
}

fn _tex_trace(any line) bool {
   if(!_tex_trace_enabled()){ return false }
   _tex_log(line)
}

fn _tex_debug_or_trace(any line) bool {
   if(!_debug_gfx_enabled && !_tex_trace_enabled()){ return false }
   _tex_log(line)
}

fn _tex_fb_trace_enabled() bool {
   ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP") || _debug_gfx_enabled
}

fn _has_live_surface() bool {
   if(!_surface){ return false }
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
   if(!cb || !src_image || !dst_buffer || w <= 0 || h <= 0 || !barrier || !region){ return false }
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
   if(!_readback_slab){
      _readback_slab = malloc(272)
      if(_readback_slab){
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
   if(!is_int(filter) || filter < 0){ return _cfg_filter ? 1 : 0 }
   return filter ? 1 : 0
}

fn _destroy_texture_image_resources(any view, any image, any memory) any {
   if(view){ destroy_image_view(_device, view, 0) }
   if(image){ destroy_image(_device, image, 0) }
   if(memory){ free_memory(_device, memory, 0) }
   0
}

fn _normalize_wrap_mode(any wrap) int {
   if(!is_int(wrap)){ return 0 }
   def mode = comptime match VkTextureWrapMode(int(wrap), 0)
   return mode
}

fn _ensure_texture_state_lists() any {
   if(!is_list(_textures)){ _textures = [] }
   if(!is_list(_texture_ds_cache)){ _texture_ds_cache = [] }
   if(!is_list(_free_texture_ids)){ _free_texture_ids = [] }
   0
}

fn _alloc_texture_slot() int {
   _ensure_texture_state_lists()
   while(_free_texture_ids.len > 0){
      def tex_id = int(_free_texture_ids.pop())
      if(tex_id >= 0 && tex_id < _textures.len){ return tex_id }
   }
   if(_textures.len >= _max_textures_value()){
      if(!_bindless_overflow_warned){
         _bindless_overflow_warned = true
         _tex_log("[gfx:vulkan] texture slot overflow live=" + to_str(_textures.len) + " max=" + to_str(_max_textures_value()))
      }
      return -1
   }
   return _textures.len
}

fn _store_texture_slot(int tex_id, any tex, any ds, int format) bool {
   _ensure_texture_state_lists()
   if(tex_id < 0 || tex_id >= _max_textures_value()){ return false }
   if(tex_id < _textures.len){
      _textures[tex_id] = tex
      _texture_ds_cache[tex_id] = ds
   } else {
      _textures = _textures.append(tex)
      _texture_ds_cache = _texture_ds_cache.append(ds)
   }
   if(_texture_fmt_cache != 0){ store8(_texture_fmt_cache, format, tex_id) }
   true
}

fn rgba_mip_level_count(int w, int h) int {
   "Runs the rgba mip level count operation."
   mut levels = 1
   mut cw = max(1, int(w))
   mut ch = max(1, int(h))
   while(cw > 1 || ch > 1){
      cw, ch = max(1, cw >> 1), max(1, ch >> 1)
      levels += 1
   }
   levels
}

fn rgba_mip_total_bytes(int w, int h) int {
   "Runs the rgba mip total bytes operation."
   mut total = 0
   mut cw = max(1, int(w))
   mut ch = max(1, int(h))
   while(cw > 0 && ch > 0){
      total += cw * ch * 4
      if(cw == 1 && ch == 1){ break }
      cw, ch = max(1, cw >> 1), max(1, ch >> 1)
   }
   total
}

fn generate_rgba_mips(ptr src_pixels, int w, int h, bool copy_single=false) any {
   "Generates generate rgba mips."
   def iw, ih = int(w), int(h)
   if(!src_pixels || iw <= 0 || ih <= 0){ return 0 }
   def levels = rgba_mip_level_count(iw, ih)
   if(levels <= 1){
      if(!copy_single){ return src_pixels }
      def single_bytes = iw * ih * 4
      def copy = malloc(single_bytes)
      if(!copy){ return 0 }
      memcpy(copy, src_pixels, single_bytes)
      return copy
   }
   def total = rgba_mip_total_bytes(iw, ih)
   mut dst = malloc(total)
   if(!dst){ return 0 }
   memcpy(dst, src_pixels, iw * ih * 4)
   mut src_off = 0
   mut dst_off = iw * ih * 4
   mut prev_w = iw
   mut prev_h = ih
   mut i = 1
   while(i < levels){
      mut next_w, next_h = prev_w >> 1, prev_h >> 1
      if(next_w < 1){ next_w = 1 }
      if(next_h < 1){ next_h = 1 }
      mut y = 0
      while(y < next_h){
         mut x = 0
         while(x < next_w){
            mut sx0, sy0 = x << 1, y << 1
            if(sx0 >= prev_w){ sx0 = prev_w - 1 }
            if(sy0 >= prev_h){ sy0 = prev_h - 1 }
            mut sx1, sy1 = sx0 + 1, sy0 + 1
            if(sx1 >= prev_w){ sx1 = prev_w - 1 }
            if(sy1 >= prev_h){ sy1 = prev_h - 1 }
            def p00, p10 = src_off + (sy0 * prev_w + sx0) * 4, src_off + (sy0 * prev_w + sx1) * 4
            def p01, p11 = src_off + (sy1 * prev_w + sx0) * 4, src_off + (sy1 * prev_w + sx1) * 4
            def dp = dst_off + (y * next_w + x) * 4
            mut c = 0
            while(c < 4){
               def sum = int(load8(dst, p00 + c)) + int(load8(dst, p10 + c)) + int(load8(dst, p01 + c)) + int(load8(dst, p11 + c))
               store8(dst, (sum + 2) / 4, dp + c)
               c += 1
            }
            x += 1
         }
         y += 1
      }
      src_off = dst_off
      dst_off += next_w * next_h * 4
      prev_w, prev_h = next_w, next_h
      i += 1
   }
   dst
}

fn resize(dict img, int new_w, int new_h) any {
   "Resizes an image dictionary using bilinear interpolation."
   if(!is_dict(img)){ return 0 }
   def w, h = img.get("width"), img.get("height")
   def pixels = img.get("data")
   def new_pixels = malloc(new_w * new_h * 4)
   if(!new_pixels){ return 0 }
   def x_ratio, y_ratio = float(w - 1) / float(new_w), float(h - 1) / float(new_h)
   mut y = 0
   while(y < new_h){
      mut x = 0
      while(x < new_w){
         def px, py = float(x) * x_ratio, float(y) * y_ratio
         def x_l, x_h = int(floor(px)), int(ceil(px))
         def y_l, y_h = int(floor(py)), int(ceil(py))
         def x_weight, y_weight = px - float(x_l), py - float(y_l)
         mut c = 0
         while(c < 4){
            def a, b = float(load8(pixels, (y_l * w + x_l) * 4 + c)), float(load8(pixels, (y_l * w + x_h) * 4 + c))
            def d, e = float(load8(pixels, (y_h * w + x_l) * 4 + c)), float(load8(pixels, (y_h * w + x_h) * 4 + c))
            def val = a * (1 - x_weight) * (1 - y_weight) +
            b * x_weight * (1 - y_weight) +
            d * y_weight * (1 - x_weight) +
            e * x_weight * y_weight
            store8(new_pixels, int(val), (y * new_w + x) * 4 + c)
            c += 1
         }
         x += 1
      }
      y += 1
   }
   return {"data": new_pixels, "width": new_w, "height": new_h, "channels": 4}
}

def RECT_PACK_HEURISTIC_BL = 0
def RECT_PACK_HEURISTIC_BF = 1

fn _node_new(list nodes, any x, any y, any nxt) int {
   def d = [x, y, nxt]
   def idx = nodes.len
   nodes.append(d)
   idx
}

fn _nx(list nodes, any idx) any { nodes.get(idx).get(0) }

fn _ny(list nodes, any idx) any { nodes.get(idx).get(1) }

fn _nn(list nodes, any idx) any { nodes.get(idx).get(2) }

fn _set_nxt(list nodes, any idx, any v) any { nodes.get(idx).set(2, v) }

fn _set_x(list nodes, any idx, any v) any { nodes.get(idx).set(0, v) }

fn rect_pack_init(any width, any height, any heuristic=0) dict {
   "Creates a new texture-atlas rect-pack context for a bin of `width`×`height`."
   def nodes = list(width + 4)
   _node_new(nodes, 0,     0,        1)
   _node_new(nodes, width, 0x3FFFFFFF, -1)
   return {
      "w": width,
      "h": height,
      "heur": heuristic,
      "nodes": nodes,
      "active": 0,
      "free": -1,
      "align": 1
   }
}

fn _skyline_find_min_y(list nodes, any first_idx, any x0, any width) list {
   def x1 = x0 + width
   mut node_idx = first_idx
   mut min_y = 0
   mut waste = 0
   mut visited_w = 0
   while(_nx(nodes, node_idx) < x1){
      def ny = _ny(nodes, node_idx)
      def nn_idx = _nn(nodes, node_idx)
      def nx2 = _nx(nodes, nn_idx)
      if(ny > min_y){
         waste += visited_w * (ny - min_y)
         min_y = ny
         if(_nx(nodes, node_idx) < x0){ visited_w += nx2 - x0 } else { visited_w += nx2 - _nx(nodes, node_idx) }
      } else {
         mut under_w = nx2 - _nx(nodes, node_idx)
         if(under_w + visited_w > width){ under_w = width - visited_w }
         waste += under_w * (min_y - ny)
         visited_w += under_w
      }
      node_idx = nn_idx
   }
   [min_y, waste]
}

fn _skyline_find_best_pos(dict ctx, any w, any h) list {
   def nodes  = ctx.get("nodes")
   def cw     = ctx.get("w")
   def ch     = ctx.get("h")
   def heur   = ctx.get("heur")
   def align  = ctx.get("align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   if(aw > cw || h > ch){ return [0, 0, 0, -1] }
   mut best_waste = 0x3FFFFFFF
   mut best_x = 0
   mut best_y = 0x3FFFFFFF
   mut best_prev = -2
   mut prev_idx = -1
   mut node_idx = ctx.get("active")
   while(_nx(nodes, node_idx) + aw <= cw){
      def res = _skyline_find_min_y(nodes, node_idx, _nx(nodes, node_idx), aw)
      def y   = res.get(0)
      def wst = res.get(1)
      if(heur == RECT_PACK_HEURISTIC_BL){
         if(y < best_y){
            best_y    = y
            best_prev = prev_idx
            best_x    = _nx(nodes, node_idx)
         }
      } else {
         if(y + h <= ch){
            if(y < best_y || (y == best_y && wst < best_waste)){
               best_y    = y
               best_waste = wst
               best_prev = prev_idx
               best_x    = _nx(nodes, node_idx)
            }
         }
      }
      prev_idx = node_idx
      node_idx = _nn(nodes, node_idx)
   }
   if(heur == RECT_PACK_HEURISTIC_BF){
      mut tail_idx = ctx.get("active")
      mut pv2 = -1
      mut nd2 = ctx.get("active")
      while(_nx(nodes, tail_idx) < aw){ tail_idx = _nn(nodes, tail_idx) }
      while(tail_idx != -1){
         def xpos = _nx(nodes, tail_idx) - aw
         if(xpos < 0){ tail_idx = _nn(nodes, tail_idx) }
         while(_nx(nodes, _nn(nodes, nd2)) <= xpos){
            pv2 = nd2
            nd2 = _nn(nodes, nd2)
         }
         def res = _skyline_find_min_y(nodes, nd2, xpos, aw)
         def y   = res.get(0)
         def wst = res.get(1)
         if(y + h <= ch && y <= best_y){
            if(y < best_y || wst < best_waste || (wst == best_waste && xpos < best_x)){
               best_x, best_y = xpos, y
               best_waste = wst
               best_prev = pv2
            }
         }
         tail_idx = _nn(nodes, tail_idx)
      }
   }
   if(best_prev == -2){ return [0, 0, 0, -1] }
   [1, best_x, best_y, best_prev]
}

fn _skyline_pack_one(dict ctx, any w, any h) list {
   def nodes   = ctx.get("nodes")
   def ch      = ctx.get("h")
   def align   = ctx.get("align")
   mut aw = (w + align - 1)
   aw -= (aw % align)
   def res = _skyline_find_best_pos(ctx, w, h)
   def found   = res.get(0)
   def rx      = res.get(1)
   def ry      = res.get(2)
   def prev_idx = res.get(3)
   if(!found || ry + h > ch){ return [0, 0, 0] }
   def new_idx = _node_new(nodes, rx, ry + h, -1)
   def active = ctx.get("active")
   def cur_idx = (prev_idx == -1) ? active : _nn(nodes, prev_idx)
   if(_nx(nodes, cur_idx) < rx){
      def after = _nn(nodes, cur_idx)
      _set_nxt(nodes, cur_idx, new_idx)
      _set_nxt(nodes, new_idx, after)
      mut scan = after
      while(scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw){
         def next = _nn(nodes, scan)
         scan = next
      }
      _set_nxt(nodes, new_idx, scan)
   } else {
      if(prev_idx == -1){ ctx.set("active", new_idx) } else { _set_nxt(nodes, prev_idx, new_idx) }
      _set_nxt(nodes, new_idx, cur_idx)
      mut scan = cur_idx
      while(scan != -1 && _nn(nodes, scan) != -1 && _nx(nodes, _nn(nodes, scan)) <= rx + aw){ scan = _nn(nodes, scan) }
      _set_nxt(nodes, new_idx, scan)
      if(scan != -1 && _nx(nodes, scan) < rx + aw){ _set_x(nodes, scan, rx + aw) }
   }
   [1, rx, ry]
}

fn _sort_by_height(list rects) any {
   def n = rects.len
   mut i = 1
   while(i < n){
      def key = rects.get(i)
      def kh = key.get("h")
      def kw = key.get("w")
      mut j = i - 1
      while(j >= 0){
         def rj = rects.get(j)
         def rjh = rj.get("h")
         def rjw = rj.get("w")
         if(rjh > kh || (rjh == kh && rjw >= kw)){ break }
         rects.set(j + 1, rj)
         j -= 1
      }
      rects.set(j + 1, key)
      i += 1
   }
}

fn rect_pack(dict ctx, list rects) int {
   "Packs texture-atlas rect dicts {id, w, h} into ctx. Sets x, y, packed on each."
   def n = rects.len
   mut i = 0
   while(i < n){
      rects.get(i).set("_ord", i)
      i += 1
   }
   _sort_by_height(rects)
   mut all_packed = 1
   i = 0
   while(i < n){
      def r  = rects.get(i)
      def rw = r.get("w")
      def rh = r.get("h")
      if(rw == 0 || rh == 0){
         r.set("x", 0)
         r.set("y", 0)
         r.set("packed", 1)
      } else {
         def res = _skyline_pack_one(ctx, rw, rh)
         if(res.get(0)){
            r.set("x", res.get(1))
            r.set("y", res.get(2))
            r.set("packed", 1)
         } else {
            r.set("x", 0)
            r.set("y", 0)
            r.set("packed", 0)
            all_packed = 0
         }
      }
      i += 1
   }
   def sorted = list(n)
   i = 0
   while(i < n){ sorted.append(0) i += 1 }
   i = 0
   while(i < n){
      def r = rects.get(i)
      sorted.set(r.get("_ord"), r)
      i += 1
   }
   i = 0
   while(i < n){ rects.set(i, sorted.get(i)) i += 1 }
   all_packed
}

@jit
fn _v3_norm(any v) list {
   def x = float(v.get(0, 0.0))
   def y = float(v.get(1, 0.0))
   def z = float(v.get(2, 0.0))
   def l = sqrt(x * x + y * y + z * z)
   if(l <= 0.000000001){ return [0.0, 0.0, 0.0] }
   [x / l, y / l, z / l]
}

@jit
fn _v3_dot(any a, any b) f64 {
   float(a.get(0, 0.0)) * float(b.get(0, 0.0)) +
   float(a.get(1, 0.0)) * float(b.get(1, 0.0)) +
   float(a.get(2, 0.0)) * float(b.get(2, 0.0))
}

@jit
fn _v3_cross(any a, any b) list {
   def ax, ay = float(a.get(0, 0.0)), float(a.get(1, 0.0))
   def az = float(a.get(2, 0.0))
   def bx, by = float(b.get(0, 0.0)), float(b.get(1, 0.0))
   def bz = float(b.get(2, 0.0))
   [ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx]
}

@jit
fn srgb_to_linear_chan(any x) f64 {
   "Runs the srgb to linear chan operation."
   def c = clamp(float(x), 0.0, 1.0)
   if(c <= 0.04045){ return c / 12.92 }
   pow((c + 0.055) / 1.055, 2.4)
}

@jit
fn linear_to_srgb_chan(any x) f64 {
   "Runs the linear to srgb chan operation."
   mut c = float(x)
   if(is_nan(c)){ c = 0.0 }
   if(c < 0.0){ c = 0.0 } elif(c > 1.0){ c = 1.0 }
   if(c <= 0.0031308){ return c * 12.92 }
   1.055 * pow(c, 1.0 / 2.4) - 0.055
}

@jit
fn linear_to_srgb_u8(any x) int {
   "Runs the linear to srgb u8 operation."
   def y = linear_to_srgb_chan(x)
   if(is_nan(y)){ return 0 }
   clamp(int(y * 255.0 + 0.5), 0, 255)
}

@jit
fn image_sample_linear_rgb_uv(any im, any u, any v) list {
   "Runs the image sample linear rgb uv operation."
   if(!is_dict(im)){ return [0.0, 0.0, 0.0] }
   def data = im.get("data", 0)
   def w = int(im.get("width", 0))
   def h = int(im.get("height", 0))
   if(!data || !is_str(data) || w <= 0 || h <= 0){ return [0.0, 0.0, 0.0] }
   mut uu = float(u) - floor(float(u))
   mut vv = clamp(float(v), 0.0, 1.0)
   def fx, fy = uu * float(w) - 0.5, vv * float(h) - 0.5
   mut x0, y0 = int(floor(fx)), int(floor(fy))
   mut x1, y1 = x0 + 1, y0 + 1
   def tx, ty = fx - float(x0), fy - float(y0)
   while(x0 < 0){ x0 += w }
   while(x1 < 0){ x1 += w }
   x0, x1 = x0 % w, x1 % w
   if(y0 < 0){ y0 = 0 }
   if(y1 < 0){ y1 = 0 }
   if(y0 >= h){ y0 = h - 1 }
   if(y1 >= h){ y1 = h - 1 }
   def i00, i10 = ((y0 * w) + x0) * 4, ((y0 * w) + x1) * 4
   def i01, i11 = ((y1 * w) + x0) * 4, ((y1 * w) + x1) * 4
   def c00 = [
      srgb_to_linear_chan(float(load8(data, i00 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i00 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i00 + 2) & 255) / 255.0)
   ]
   def c10 = [
      srgb_to_linear_chan(float(load8(data, i10 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i10 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i10 + 2) & 255) / 255.0)
   ]
   def c01 = [
      srgb_to_linear_chan(float(load8(data, i01 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i01 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i01 + 2) & 255) / 255.0)
   ]
   def c11 = [
      srgb_to_linear_chan(float(load8(data, i11 + 0) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i11 + 1) & 255) / 255.0),
      srgb_to_linear_chan(float(load8(data, i11 + 2) & 255) / 255.0)
   ]
   def a0 = [
      c00.get(0, 0.0) + (c10.get(0, 0.0) - c00.get(0, 0.0)) * tx,
      c00.get(1, 0.0) + (c10.get(1, 0.0) - c00.get(1, 0.0)) * tx,
      c00.get(2, 0.0) + (c10.get(2, 0.0) - c00.get(2, 0.0)) * tx
   ]
   def a1 = [
      c01.get(0, 0.0) + (c11.get(0, 0.0) - c01.get(0, 0.0)) * tx,
      c01.get(1, 0.0) + (c11.get(1, 0.0) - c01.get(1, 0.0)) * tx,
      c01.get(2, 0.0) + (c11.get(2, 0.0) - c01.get(2, 0.0)) * tx
   ]
   [
      a0.get(0, 0.0) + (a1.get(0, 0.0) - a0.get(0, 0.0)) * ty,
      a0.get(1, 0.0) + (a1.get(1, 0.0) - a0.get(1, 0.0)) * ty,
      a0.get(2, 0.0) + (a1.get(2, 0.0) - a0.get(2, 0.0)) * ty
   ]
}

@jit
fn env_dir_to_uv(any d) list {
   "Runs the env dir to uv operation."
   def n = _v3_norm(d)
   def x = float(n.get(0, 0.0))
   def y, z = float(n.get(1, 0.0)), float(n.get(2, 0.0))
   def u_raw = 0.5 + atan2(x, 0.0 - z) / 6.283185307179586
   def v = clamp(0.5 - asin(clamp(y, -1.0, 1.0)) / 3.141592653589793, 0.00001, 0.99999)
   [u_raw - floor(u_raw), v]
}

@jit
fn _radical_inverse_vdc32(any bits0) f64 {
   mut bits = int(bits0)
   bits = ((bits << 16) | ((bits >> 16) & 0xffff)) & 0xffffffff
   bits = (((bits & 0x55555555) << 1) | ((bits & 0xaaaaaaaa) >> 1)) & 0xffffffff
   bits = (((bits & 0x33333333) << 2) | ((bits & 0xcccccccc) >> 2)) & 0xffffffff
   bits = (((bits & 0x0f0f0f) << 4) | ((bits & 0xf0f0f0) >> 4)) & 0xffffffff
   bits = (((bits & 0x00ff00ff) << 8) | ((bits & 0xff00ff00) >> 8)) & 0xffffffff
   float(bits) * 2.3283064365386963e-10
}

@jit
fn _importance_sample_ggx(any xi_x, any xi_y, any roughness, any N) list {
   def a = roughness * roughness
   def a2 = a * a
   def phi = 6.283185307179586 * xi_x
   def cos_theta = sqrt((1.0 - xi_y) / max(1.0 + (a2 - 1.0) * xi_y, 1e-6))
   def sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0))
   def H = [cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta]
   def Nz = float(N.get(2, 0.0))
   def up = (abs(Nz) < 0.999) ? [0.0, 0.0, 1.0] : [1.0, 0.0, 0.0]
   def tangent = _v3_norm(_v3_cross(up, N))
   def bitangent = _v3_cross(N, tangent)
   _v3_norm([
         tangent.get(0, 0.0) * H.get(0, 0.0) + bitangent.get(0, 0.0) * H.get(1, 0.0) + float(N.get(0, 0.0)) * H.get(2, 0.0),
         tangent.get(1, 0.0) * H.get(0, 0.0) + bitangent.get(1, 0.0) * H.get(1, 0.0) + float(N.get(1, 0.0)) * H.get(2, 0.0),
         tangent.get(2, 0.0) * H.get(0, 0.0) + bitangent.get(2, 0.0) * H.get(1, 0.0) + float(N.get(2, 0.0)) * H.get(2, 0.0)
   ])
}

fn generate_spec_env_slab(any im, int base_w=256) any {
   "Generates generate spec env slab."
   if(!is_dict(im)){ return 0 }
   def src_w, src_h = int(im.get("width", 0)), int(im.get("height", 0))
   if(src_w <= 0 || src_h <= 0){ return 0 }
   mut w0 = clamp(int(base_w), 64, 512)
   if(w0 > src_w){ w0 = src_w }
   def h0 = max(1, w0 / 2)
   mut levels = 1
   mut tw = w0
   mut th = h0
   mut total = 0
   while(true){
      total += tw * th * 4
      if(tw <= 1 && th <= 1){ break }
      tw, th = max(1, tw >> 1), max(1, th >> 1)
      levels += 1
   }
   def slab = malloc(total)
   if(!slab){ return 0 }
   mut off = 0
   mut level = 0
   while(level < levels){
      def w, h = max(1, w0 >> level), max(1, h0 >> level)
      def roughness = (levels > 1) ? float(level) / float(levels - 1) : 0.0
      mut sample_count = 1
      if(level > 0){
         if(roughness < 0.15){ sample_count = 64 }
         elif(roughness < 0.5){ sample_count = 32 }
         else { sample_count = 16 }
      }
      mut y = 0
      while(y < h){
         def vv = (float(y) + 0.5) / float(h)
         def elev = (0.5 - vv) * 3.141592653589793
         def sin_e = sin(elev)
         def cos_e = cos(elev)
         mut x = 0
         while(x < w){
            def uu = (float(x) + 0.5) / float(w)
            def phi = (uu - 0.5) * 6.283185307179586
            def N = _v3_norm([cos_e * cos(phi), sin_e, cos_e * sin(phi)])
            mut c0, c1 = 0.0, 0.0
            mut c2 = 0.0
            mut weight = 0.0
            if(roughness <= 0.0 || sample_count <= 1){
               def uv = env_dir_to_uv(N)
               def s = image_sample_linear_rgb_uv(im, uv.get(0, 0.0), uv.get(1, 0.0))
               c0, c1 = s.get(0, 0.0), s.get(1, 0.0)
               c2 = s.get(2, 0.0)
               weight = 1.0
            } else {
               mut i = 0
               while(i < sample_count){
                  def xi_x, xi_y = float(i) / float(sample_count), _radical_inverse_vdc32(i)
                  def H = _importance_sample_ggx(xi_x, xi_y, roughness, N)
                  def VoH = max(_v3_dot(N, H), 0.0)
                  def L = _v3_norm([
                        2.0 * VoH * H.get(0, 0.0) - N.get(0, 0.0),
                        2.0 * VoH * H.get(1, 0.0) - N.get(1, 0.0),
                        2.0 * VoH * H.get(2, 0.0) - N.get(2, 0.0)
                  ])
                  def NoL = max(_v3_dot(N, L), 0.0)
                  if(NoL > 0.0){
                     def uv = env_dir_to_uv(L)
                     def s = image_sample_linear_rgb_uv(im, uv.get(0, 0.0), uv.get(1, 0.0))
                     c0 += s.get(0, 0.0) * NoL
                     c1 += s.get(1, 0.0) * NoL
                     c2 += s.get(2, 0.0) * NoL
                     weight += NoL
                  }
                  i += 1
               }
            }
            if(weight > 0.0){
               c0, c1 = c0 / weight, c1 / weight
               c2 = c2 / weight
            }
            def dp = off + ((y * w) + x) * 4
            store8(slab, linear_to_srgb_u8(c0), dp + 0)
            store8(slab, linear_to_srgb_u8(c1), dp + 1)
            store8(slab, linear_to_srgb_u8(c2), dp + 2)
            store8(slab, 255, dp + 3)
            x += 1
         }
         y += 1
      }
      off += w * h * 4
      level += 1
   }
   mut out = dict(8)
   out["pixels"] = slab
   out["width"] = w0
   out["height"] = h0
   out["levels"] = levels
   out["bytes"] = total
   out
}

fn _rgba_image_result(any pixels, int w, int h) dict {
   mut out = dict(8)
   out["data"] = init_str(pixels, w * h * 4)
   out["width"] = w
   out["height"] = h
   out["channels"] = 4
   out
}

fn generate_env_image(int kind=0, int w=1024, int h=512) any {
   "Generates generate env image."
   def iw, ih = max(1, int(w)), max(1, int(h))
   def pixels = malloc(iw * ih * 4)
   if(!pixels){ return 0 }
   def fw, fh = float(iw), float(ih)
   mut y = 0
   while(y < ih){
      def v = (float(y) + 0.5) / fh
      def elev = (0.5 - v) * 3.141592653589793
      def dy = sin(elev)
      def sky_t = clamp(dy * 0.5 + 0.5, 0.0, 1.0)
      def top_t = clamp(dy, 0.0, 1.0)
      def floor_t = clamp(-dy, 0.0, 1.0)
      def row = y * iw * 4
      mut x = 0
      while(x < iw){
         def u = (float(x) + 0.5) / fw
         mut c0, c1 = 0.0, 0.0
         mut c2 = 0.0
         if(kind == 0){
            c0, c1 = (0.58 * (1.0 - sky_t)) + ((0.78 + 0.035 * top_t) * sky_t), (0.60 * (1.0 - sky_t)) + ((0.80 + 0.036 * top_t) * sky_t)
            c2 = (0.66 * (1.0 - sky_t)) + ((0.86 + 0.040 * top_t) * sky_t)
            def key1_dx, key1_dy = (u - 0.74) / 0.055, (v - 0.27) / 0.085
            def key1 = exp(-(key1_dx * key1_dx + key1_dy * key1_dy))
            def key2_dx = (u - 0.28) / 0.070
            def key2_dy = (v - 0.30) / 0.095
            def key2 = exp(-(key2_dx * key2_dx + key2_dy * key2_dy))
            def top_strip = exp(-pow((v - 0.17) / 0.040, 2.0)) * exp(-pow((u - 0.50) / 0.32, 6.0))
            def horizon = exp(-pow((v - 0.50) / 0.11, 2.0)) * 0.012
            c0 += key1 * 0.10 + key2 * 0.08 + top_strip * 0.034 + horizon
            c1 += key1 * 0.10 + key2 * 0.08 + top_strip * 0.034 + horizon
            c2 += key1 * 0.11 + key2 * 0.09 + top_strip * 0.038 + horizon
         } elif(kind == 1){
            c0, c1 = 0.70 + 0.15 * top_t - 0.08 * floor_t, 0.66 + 0.14 * top_t - 0.07 * floor_t
            c2 = 0.74 + 0.16 * top_t - 0.04 * floor_t
            def broad1_dx, broad1_dy = (u - 0.72) / 0.18, (v - 0.24) / 0.16
            def broad1 = exp(-(broad1_dx * broad1_dx + broad1_dy * broad1_dy))
            def broad2_dx = (u - 0.28) / 0.18
            def broad2_dy = (v - 0.30) / 0.18
            def broad2 = exp(-(broad2_dx * broad2_dx + broad2_dy * broad2_dy))
            def top_strip = exp(-pow((v - 0.16) / 0.040, 2.0)) * exp(-pow((u - 0.50) / 0.34, 6.0))
            def horizon = exp(-pow((v - 0.56) / 0.14, 2.0))
            def warm_floor = exp(-pow((v - 0.82) / 0.16, 2.0))
            c0 += broad1 * 0.16 + broad2 * 0.12 + top_strip * 0.08 + horizon * 0.06 + warm_floor * 0.05
            c1 += broad1 * 0.12 + broad2 * 0.10 + top_strip * 0.06 + horizon * 0.04 + warm_floor * 0.03
            c2 += broad1 * 0.18 + broad2 * 0.16 + top_strip * 0.10 + horizon * 0.08 + warm_floor * 0.05
         } elif(kind == 2){
            mut l = 0.30 + 0.42 * top_t + 0.02 * floor_t
            def key_left_dx, key_left_dy = (u - 0.22) / 0.090, (v - 0.23) / 0.085
            def key_left = exp(-(key_left_dx * key_left_dx + key_left_dy * key_left_dy))
            def key_right_dx = (u - 0.78) / 0.095
            def key_right_dy = (v - 0.24) / 0.090
            def key_right = exp(-(key_right_dx * key_right_dx + key_right_dy * key_right_dy))
            def top_strip = exp(-pow((v - 0.15) / 0.070, 2.0)) * exp(-pow((u - 0.50) / 0.42, 4.0))
            def mid_soft = exp(-pow((v - 0.44) / 0.16, 2.0)) * exp(-pow((u - 0.50) / 0.36, 4.0))
            def floor_soft = exp(-pow((v - 0.82) / 0.18, 2.0))
            def center_shadow_x = max(abs((u - 0.50) / 0.25), abs((v - 0.63) / 0.30))
            def center_shadow = exp(-pow(center_shadow_x, 4.0))
            l += key_left * 1.05 + key_right * 1.00 + top_strip * 0.24 + mid_soft * 0.10 + floor_soft * 0.035
            l -= center_shadow * 0.035
            l = max(l, 0.075)
            def warm = key_left * 0.035 + floor_t * 0.018
            def cool = key_right * 0.025 + top_t * 0.020
            c0, c1 = l * 1.01 + warm, l * 1.00
            c2 = l * 1.02 + cool
         } else {
            c0, c1 = 0.18 + 0.08 * sky_t - 0.02 * floor_t, 0.18 + 0.08 * sky_t - 0.02 * floor_t
            c2 = 0.20 + 0.09 * sky_t - 0.02 * floor_t
            def soft1_dx, soft1_dy = (u - 0.24) / 0.060, (v - 0.23) / 0.055
            def soft1 = exp(-(soft1_dx * soft1_dx + soft1_dy * soft1_dy))
            def soft2_dx = (u - 0.76) / 0.060
            def soft2_dy = (v - 0.23) / 0.055
            def soft2 = exp(-(soft2_dx * soft2_dx + soft2_dy * soft2_dy))
            def fill_dx = (u - 0.50) / 0.20
            def fill_dy = (v - 0.19) / 0.08
            def fill = exp(-(fill_dx * fill_dx + fill_dy * fill_dy))
            def warm_dx = (u - 0.50) / 0.30
            def warm_dy = (v - 0.74) / 0.16
            def warm = exp(-(warm_dx * warm_dx + warm_dy * warm_dy))
            def horizon = exp(-pow((v - 0.50) / 0.090, 2.0))
            c0 += soft1 * 1.14 + soft2 * 1.14 + fill * 0.30 + warm * 0.04 + horizon * 0.03
            c1 += soft1 * 1.14 + soft2 * 1.14 + fill * 0.30 + warm * 0.04 + horizon * 0.03
            c2 += soft1 * 1.16 + soft2 * 1.16 + fill * 0.31 + warm * 0.03 + horizon * 0.04
         }
         def p = row + x * 4
         store8(pixels, linear_to_srgb_u8(c0), p + 0)
         store8(pixels, linear_to_srgb_u8(c1), p + 1)
         store8(pixels, linear_to_srgb_u8(c2), p + 2)
         store8(pixels, 255, p + 3)
         x += 1
      }
      y += 1
   }
   _rgba_image_result(pixels, iw, ih)
}

fn generate_neutral_env_image(int w=1024, int h=512) any { generate_env_image(0, w, h) }

fn generate_compare_visible_env_image(int w=1024, int h=512) any { generate_env_image(1, w, h) }

fn generate_compare_reflect_env_image(int w=1024, int h=512) any { generate_env_image(2, w, h) }

fn generate_studio_env_image(int w=1024, int h=512) any { generate_env_image(3, w, h) }

fn _scene_key(any name) str { str.lower(str.strip(to_str(name))) }

fn _scene_has_any(str s, list words) bool {
   mut i = 0
   while(i < words.len){
      def w = str.lower(str.strip(to_str(words.get(i, ""))))
      if(w.len > 0 && str.str_contains(s, w)){ return true }
      i += 1
   }
   false
}

fn _scene_env_match(any name, str env_name) bool {
   def raw = _scene_key(env(env_name))
   if(raw.len == 0){ return false }
   if(raw == "*"){ return true }
   def s = _scene_key(name)
   if(s.len == 0){ return false }
   def parts = str.split(raw, ",")
   mut i = 0
   while(i < parts.len){
      def p = str.strip(to_str(parts.get(i, "")))
      if(p.len > 0 && (s == p || str.str_contains(s, p))){ return true }
      i += 1
   }
   false
}

fn _scene_env_override(any name, str on_env, str off_env) int {
   if(off_env.len > 0 && _scene_env_match(name, off_env)){ return 0 }
   if(on_env.len > 0 && _scene_env_match(name, on_env)){ return 1 }
   -1
}

fn scene_prefers_studio_env(any name) bool {
   "Runs the prefers studio env operation."
   def ov = _scene_env_override(name, "NY_UI_SCENE_STUDIO_ENV", "NY_UI_SCENE_NO_STUDIO_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   if(s.len == 0){ return false }
   if(str.startswith(s, "compare") || str.endswith(s, "testgrid")){ return false }
   str.endswith(s, "spheres") ||
   _scene_has_any(s, ["metal", "rough", "spec", "gloss", "sheen", "clearcoat", "anisotropy", "iridescence", "pbr", "carpaint", "velvet", "leather"])
}

fn scene_prefers_neutral_env(any name) bool {
   "Runs the prefers neutral env operation."
   def ov = _scene_env_override(name, "NY_UI_SCENE_NEUTRAL_ENV", "NY_UI_SCENE_NO_NEUTRAL_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   str.startswith(s, "compare") ||
   _scene_has_any(s, ["transmission", "volume", "ior", "dispersion", "attenuation", "glass", "scatter", "diffuse", "light", "emissive", "environment", "texture", "uv", "normal", "sheen", "specular", "metallic", "roughness"])
}

fn scene_prefers_compare_reflect_env(any name) bool {
   "Runs the prefers compare reflect env operation."
   def ov = _scene_env_override(name, "NY_UI_SCENE_REFLECT_ENV", "NY_UI_SCENE_NO_REFLECT_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["metal", "rough", "spec", "gloss", "iridescence", "sheen", "clearcoat", "anisotropy", "reflect", "environment", "pbr"])
}

fn scene_prefers_compare_visible_env(any name) bool {
   "Runs the prefers compare visible env operation."
   def ov = _scene_env_override(name, "NY_UI_SCENE_VISIBLE_ENV", "NY_UI_SCENE_NO_VISIBLE_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["transmission", "glass", "visible", "scatter", "sunglass", "transparent", "environment"])
}

fn scene_prefers_optical_spec_env(any name) bool {
   "Runs the prefers optical spec env operation."
   def ov = _scene_env_override(name, "NY_UI_SCENE_OPTICAL_ENV", "NY_UI_SCENE_NO_OPTICAL_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["transmission", "volume", "ior", "dispersion", "attenuation", "glass", "water", "transparent", "iridescence", "optical", "diffuse"])
}

fn scene_prefers_black_visible_env(any name) bool {
   "Runs the prefers black visible env operation."
   _scene_env_match(name, "NY_UI_SCENE_BLACK_VISIBLE_ENV") &&
   !_scene_env_match(name, "NY_UI_SCENE_NO_BLACK_VISIBLE_ENV")
}

fn scene_prefers_gray_proof_bg(any name) bool {
   "Runs the prefers gray proof bg operation."
   def ov = _scene_env_override(name, "NY_UI_SCENE_GRAY_BG", "NY_UI_SCENE_NO_GRAY_BG")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["test", "helmet", "shoe", "lamp", "texture", "rough", "ior", "meshopt", "light", "cloth", "fabric", "carbon", "glass", "transmission", "dispersion", "attenuation"])
}

fn _mip_level_count(int w, int h) int { rgba_mip_level_count(w, h) }

fn _generate_rgba_mips(any src_pixels, int w, int h) any { generate_rgba_mips(src_pixels, w, h, false) }

fn _mip_byte_count_rgba(int w, int h) int { rgba_mip_total_bytes(w, h) }

fn _ensure_sampler(any filter, any wrap_s=10497, any wrap_t=10497) any {
   def norm = _normalize_filter(filter)
   def uw = _normalize_wrap_mode(wrap_s)
   def vw = _normalize_wrap_mode(wrap_t)
   if(uw == 0 && vw == 0){
      def cur = norm ? _linear_sampler : _nearest_sampler
      if(cur){ return cur }
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
   if(create_sampler(_device, sampler_ci, 0, sampler_ptr) != 0){
      free(sampler_ci) free(sampler_ptr)
      return 0
   }
   def sampler = load64_h(sampler_ptr, 0)
   if(uw == 0 && vw == 0){
      if(norm){ _linear_sampler = sampler }
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
   if(!_upload_cb_ptr || !_upload_cb_arr || !_upload_bi || !_upload_bar1 || !_upload_bar2 || !_upload_region || !_upload_si){ return false }
   def cb = _ensure_upload_cb()
   if(!cb){ return false }
   if(!_begin_upload_cb(cb)){ return false }
   mut src_access = 0
   mut src_stage = _vk_pipeline_top_of_pipe()
   if(old_layout == _vk_layout_shader_read_only()){
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
   end_command_buffer(cb)
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
   if(!_bindless_ds){ return false }
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ return false }
   mut tex_obj = _textures.get(tex_id, 0)
   if(!is_dict(tex_obj)){ return false }
   def tex_view = tex_obj.get("view", 0)
   if(!tex_view){ return false }
   def tex_sampler = tex_obj.get("sampler", _default_sampler)
   def synced = tex_obj.get("bindless_synced", false)
   def synced_view = tex_obj.get("bindless_synced_view", 0)
   def synced_sampler = tex_obj.get("bindless_synced_sampler", 0)
   _tex_trace("[vk:tex] sync tex=" + to_str(tex_id) +
      " view=0x" + str.to_hex(tex_view) +
      " sampler=0x" + str.to_hex(tex_sampler) +
      " synced=" + to_str(synced ? 1 : 0) +
      " w=" + to_str(int(tex_obj.get("width", 0))) +
      " h=" + to_str(int(tex_obj.get("height", 0))) +
      " fmt=" + to_str(int(tex_obj.get("format", 0))) +
   " path=" + to_str(tex_obj.get("path", "")))
   if(synced && synced_view == tex_view && synced_sampler == tex_sampler){ return true }
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

fn tex_job_make(int index, any uri, any mime="", any sampler=0, int material=-1, any slot="") dict {
   "Runs the tex job make operation."
   def j = {
      "index": int(index), "uri": to_str(uri), "mime": to_str(mime),
      "sampler": sampler, "material": int(material), "slot": to_str(slot)
   }
   j
}

fn tex_job_queue_make() dict {
   "Runs the tex job queue make operation."
   def q = {"head": 0, "items": list(0)}
   q
}

fn tex_job_queue_push(any q, any job) dict {
   "Runs the tex job queue push operation."
   if(!is_dict(q)){ q = tex_job_queue_make() }
   def items = q.get("items", [])
   q["items"] = items.append(job)
   q
}

fn tex_job_queue_pop(any q) any {
   "Runs the tex job queue pop operation."
   if(!is_dict(q)){ return 0 }
   def items = q.get("items", [])
   mut head = int(q.get("head", 0))
   if(!is_list(items) || head >= items.len){ return 0 }
   def job = items.get(head, 0)
   q["head"] = head + 1
   job
}

fn tex_job_result_make(any job, int width, int height, any rgba_or_mips, bool ok=true, any err="") dict {
   "Runs the tex job result make operation."
   def r = {
      "job": job, "ok": ok ? true : false, "error": to_str(err),
      "width": int(width), "height": int(height), "pixels": rgba_or_mips
   }
   r
}

fn tex_job_cache_key(any uri, any mime="", int flags=0) str { "ntex_" + to_str(hash.fnv1a(to_str(uri) + "|" + to_str(mime) + "|" + to_str(flags))) }

fn tex_job_worker_plan(int worker_count=4) dict {
   "Runs the tex job worker plan operation."
   def p = {
      "workers": int(worker_count), "worker_touches_renderer": false,
      "worker_output": "decoded_rgba_or_prebaked_mip_slab",
      "main_thread_upload": true, "preserve_material_ids": true
   }
   p
}

fn tex_job_upload_plan(any results) list {
   "Runs the tex job upload plan operation."
   if(!is_list(results)){ return [] }
   mut out = list(0)
   mut i = 0
   def results_n = results.len
   while(i < results_n){
      def r = results.get(i, 0)
      if(is_dict(r) && r.get("ok", false)){ out = out.append(r) }
      i += 1
   }
   out
}

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
   if(width <= 0 || height <= 0){
      _tex_debug("[gfx:vulkan] create_texture_ex skip invalid size w=" + to_str(width) + " h=" + to_str(height) + " fmt=" + to_str(format))
      return -1
   }
   mut bpp = 4
   if(format == 9){ bpp = 1 }
   if(format == _vk_format_r16g16b16a16_sfloat()){ bpp = 8 }
   mut use_mipmaps_live = use_mipmaps
   if(use_mipmaps_live && format != 9){
      def base_bytes = width * height * bpp
      def mip_bytes = _mip_byte_count_rgba(width, height)
      if(mip_bytes > _staging_capacity && base_bytes <= _staging_capacity){
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
   if(!img_ci || !img_ptr){ return -1 }
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
   if(r1 != 0){
      _tex_debug("[gfx:vulkan] create_texture_ex create_image failed res=" + to_str(r1) + " w=" + to_str(width) + " h=" + to_str(height) + " fmt=" + to_str(format))
      return -1
   }
   def image = load64_h(img_ptr, 0)
   mut mem_req = malloc(24)
   if(!mem_req){ return -1 }
   get_image_memory_requirements(_device, image, mem_req)
   def size = load64_h(mem_req, 0)
   def type_bits = load32(mem_req, 16)
   if(size <= 0){
      _tex_debug("[gfx:vulkan] create_texture_ex invalid mem requirements size=" + to_str(int(size)) + " w=" + to_str(width) + " h=" + to_str(height) + " fmt=" + to_str(format))
      destroy_image(_device, image, 0)
      _free_image_create_allocs(img_ci, img_ptr, mem_req)
      return -1
   }
   def mem_idx = _find_memory_type(type_bits, _vk_memory_device_local())
   if(mem_idx < 0){
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
   if(!alloc_info || !mem_ptr){ return -1 }
   _tex_debug("[gfx:vulkan] create_texture_ex alloc mem size=" + to_str(int(size)) + " type=" + to_str(int(mem_idx)))
   def alloc_res = allocate_memory(_device, alloc_info, 0, mem_ptr)
   if(alloc_res != 0){
      _tex_debug("[gfx:vulkan] create_texture_ex allocate_memory failed res=" + to_str(int(alloc_res)) + " bytes=" + to_str(int(size)))
      destroy_image(_device, image, 0)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
      return -1
   }
   def memory = load64_h(mem_ptr, 0)
   def bind_res = bind_image_memory(_device, image, memory, 0)
   if(bind_res != 0){
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
   if(r3 != 0){
      _tex_debug("[gfx:vulkan] create_texture_ex image_view failed res=" + to_str(r3))
      return -1
   }
   def view = load64_h(view_ptr, 0)
   def img_size = width * height * bpp
   mut mip_pixels = 0
   mut upload_pixels = pixels
   mut prebaked_total = int(prebaked_mip_bytes)
   mut expected_mip_total = 0
   if(use_mipmaps_live && format != 9 && pixels){
      expected_mip_total = _mip_byte_count_rgba(width, height)
      if(prebaked_total >= expected_mip_total){
         mip_pixels = pixels
         upload_pixels = pixels
      } else {
         prebaked_total = 0
         mip_pixels = _generate_rgba_mips(pixels, width, height)
         if(mip_pixels){ upload_pixels = mip_pixels }
      }
   }
   if(pixels && _staging_map){
      if(use_mipmaps_live && mip_pixels){
         mut total = prebaked_total
         if(total <= 0){ total = expected_mip_total }
         if(total > _staging_capacity){
            _tex_debug(f"[gfx:vulkan] staging too small mip_bytes={total} staging_bytes={_staging_capacity}")
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
         if(!_copy_upload_bytes(_staging_map, upload_pixels, total)){
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
         def cb = _ensure_upload_cb()
         if(!cb){
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
         if(!_begin_upload_cb(cb)){
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
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
         if(!region){
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
         mut off = 0
         mut cw = width
         mut ch = height
         mut level = 0
         while(level < mip_levels){
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
         if(!_submit_upload_cb(cb)){
            _tex_debug("[gfx:vulkan] create_texture_ex upload submit failed")
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
      } else {
         if(img_size > _staging_capacity){
            _tex_debug(f"[gfx:vulkan] staging too small image_bytes={img_size} staging_bytes={_staging_capacity}")
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
         if(!_copy_upload_bytes(_staging_map, upload_pixels, img_size)){
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
         if(!_upload_image_region(image, 0, 0, width, height, _vk_layout_undefined())){
            _tex_debug("[gfx:vulkan] create_texture_ex upload_image_region failed w=" + to_str(width) + " h=" + to_str(height) + " bytes=" + to_str(img_size))
            if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
            return -1
         }
      }
   } else {
      if(!_upload_image_region(image, 0, 0, width, height, _vk_layout_undefined())){
         _tex_debug("[gfx:vulkan] create_texture_ex upload_image_region failed empty pixels w=" + to_str(width) + " h=" + to_str(height))
         if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
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
   if(tex_id < 0){
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
      return -1
   }
   if(!_store_texture_slot(tex_id, tex, ds, format)){
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
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
   if(_bindless_ds){ bindless_sync_texture_slot(tex_id) }
   _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
   if(mip_pixels && mip_pixels != pixels){ free(mip_pixels) }
   return ret_tex_id
}

fn update_texture_rect(int tex_id, int x, int y, int w, int h, any pixels) bool {
   "Partially updates a texture's pixel data. Uses pre-allocated buffers."
   if(tex_id < 0 || tex_id >= _textures.len){ return false }
   def tex_obj = _textures.get(tex_id)
   def image = tex_obj.get("image")
   def bpp = tex_obj.get("bpp", 4)
   def img_size = w * h * bpp
   if(img_size > _staging_capacity){ return false }
   if(!_copy_upload_bytes(_staging_map, pixels, img_size)){ return false }
   _upload_image_region(image, x, y, w, h, _vk_layout_shader_read_only())
   if(_bindless_ds){ bindless_sync_texture_slot(tex_id) }
   true
}

fn create_texture(int width, int height, any pixels) int {
   "Creates a GPU texture from raw pixel data(RGBA8)."
   def raw = create_texture_ex(width, height, pixels, 37, -1)
   def stable = last_created_texture_id()
   return stable >= 0 ? stable : raw
}

fn _init_bindless_descriptor_set(any default_view) bool {
   if(_bindless_ds){ return true }
   mut dsl_ptr = _tex_alloc(8)
   store64_h(dsl_ptr, _descriptor_set_layout, 0)
   mut alloc_ds = _tex_alloc(40)
   store32(alloc_ds, _vk_stype_descriptor_set_allocate_info(), 0)
   store64_h(alloc_ds, _descriptor_pool, 16)
   store32(alloc_ds, 1, 24)
   store64_h(alloc_ds, dsl_ptr, 32)
   mut ds_ptr = _tex_alloc(8)
   if(allocate_descriptor_sets(_device, alloc_ds, ds_ptr) != 0){
      free(dsl_ptr, alloc_ds, ds_ptr)
      return false
   }
   _bindless_ds = load64_h(ds_ptr, 0)
   mut infos = _tex_alloc(24 * _max_textures_value())
   mut i = 0
   while(i < _max_textures_value()){
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
   while(i < textures_n){
      mut tex_obj = _textures.get(i, 0)
      if(is_dict(tex_obj)){
         def tex_view = tex_obj.get("view", 0)
         def tex_sampler = tex_obj.get("sampler", _default_sampler)
         if(tex_view){
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
   if(!_default_sampler){ return false }
   def pixels = _tex_alloc(4)
   store32(pixels, 0xFFFFFFFF, 0)
   def tex_id = create_texture_ex(1, 1, pixels, 37, _cfg_filter)
   if(tex_id == -1){ return false }
   _default_texture = tex_id
   def normal_pixels = _tex_alloc(4)
   store8(normal_pixels, 128, 0)
   store8(normal_pixels, 128, 1)
   store8(normal_pixels, 255, 2)
   store8(normal_pixels, 255, 3)
   def normal_tex_id = create_texture_ex(1, 1, normal_pixels, 37, _cfg_filter)
   if(normal_tex_id == -1){ return false }
   _default_normal_texture = normal_tex_id
   def black_pixels = _tex_alloc(4)
   store8(black_pixels, 0, 0)
   store8(black_pixels, 0, 1)
   store8(black_pixels, 0, 2)
   store8(black_pixels, 255, 3)
   def black_tex_id = create_texture_ex(1, 1, black_pixels, 37, _cfg_filter)
   if(black_tex_id == -1){ return false }
   _default_black_texture = black_tex_id
   _current_texture_id = tex_id
   _current_tex_index = tex_id
   _batch_texture_id = tex_id
   _batch_tex_index = _current_tex_index
   def tex_obj = _textures.get(tex_id, 0)
   def view = tex_obj.get("view", 0)
   if(!view || !_init_bindless_descriptor_set(view)){ return false }
   true
}

mut _mvp_dirty = true
mut _model_dirty = true

fn bind_texture(any tex_id) any {
   "Binds bind texture."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ tex_id = _default_texture }
   if(tex_id == _current_texture_id){ return 0 }
   if(_vertex_offset != _last_flush_offset){
      _flush_reason = 1
      _flush()
   }
   _current_texture_id = tex_id
   _current_tex_index = tex_id
   if(_vertex_offset == _last_flush_offset){
      _batch_texture_id = tex_id
      _batch_tex_index = tex_id
   }
   if(_bindless_ds){ bindless_sync_texture_slot(tex_id) }
   0
}

fn bind_default_texture() any {
   "Binds the renderer's default 1x1 white texture."
   bind_texture(_default_texture)
}

fn texture_size(any tex_id) any {
   "Returns [width, height] for a texture ID, or 0 if invalid."
   if(!is_int(tex_id)){ tex_id = _default_texture } elif(tex_id < 0 || tex_id >= _max_textures_value()){
      if(!_bindless_overflow_warned){
         _bindless_overflow_warned = true
      }
      tex_id = _default_texture
   }
   def tex = _textures.get(tex_id, 0)
   if(!tex || !is_dict(tex)){ return 0 }
   [tex.get("width", 0), tex.get("height", 0)]
}

fn _texture_meta(any tex_id, any key, any fallback) any {
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ return fallback }
   def tex_obj = _textures.get(tex_id, 0)
   if(!tex_obj || !is_dict(tex_obj)){ return fallback }
   tex_obj.get(key, fallback)
}

fn set_texture_debug_meta(any tex_id, any path="", any cache_key="") bool {
   "Attaches debug-only source metadata to a live texture slot."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ return false }
   mut tex_obj = _textures.get(tex_id, 0)
   if(!tex_obj || !is_dict(tex_obj)){ return false }
   if(is_str(path) && path.len > 0){ tex_obj["path"] = path }
   if(is_str(cache_key) && cache_key.len > 0){ tex_obj["cache_key"] = cache_key }
   _textures[tex_id] = tex_obj
   true
}

fn set_texture_protected(any tex_id, bool protected=true) bool {
   "Marks a texture slot as owned by a long-lived atlas/default resource."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ return false }
   mut tex_obj = _textures.get(tex_id, 0)
   if(!tex_obj || !is_dict(tex_obj)){ return false }
   tex_obj["protected"] = protected
   _textures[tex_id] = tex_obj
   true
}

fn texture_format(any tex_id) int {
   "Returns the format of a texture."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ return 37 }
   if(_texture_fmt_cache != 0){ return load8(_texture_fmt_cache, tex_id) }
   def t = _textures.get(tex_id)
   if(!t){ return 37 }
   t.get("format", 37)
}

fn texture_descriptor(any tex_id) any {
   "Returns the descriptor set for a texture."
   _bindless_ds
}

fn destroy_texture(any tex_id) any {
   "Destroys a texture and frees its GPU resources."
   if(!is_int(tex_id) || tex_id < 0 || tex_id >= _textures.len){ return 0 }
   if(tex_id == _default_texture || tex_id == _default_normal_texture || tex_id == _default_black_texture){ return 0 }
   def tex = _textures.get(tex_id, 0)
   if(!tex || !is_dict(tex)){ return 0 }
   if(tex.get("protected", false)){ return 0 }
   if(_bindless_ds && _default_texture >= 0){
      def def_tex = _textures.get(_default_texture, 0)
      def def_view = def_tex.get("view", 0)
      if(def_view){
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
   if(view){ destroy_image_view(_device, view, 0) }
   if(img){ destroy_image(_device, img, 0) }
   if(mem){ free_memory(_device, mem, 0) }
   if(_current_texture_id == tex_id){
      _current_texture_id = -1
      _current_tex_index = 0
   }
   if(_batch_texture_id == tex_id){
      _batch_texture_id = _default_texture
      _batch_tex_index = (_default_texture >= 0) ? _default_texture : 0
   }
   _textures[tex_id] = 0
   _texture_ds_cache[tex_id] = 0
   if(_texture_fmt_cache != 0 && tex_id >= 0 && tex_id < _max_textures_value()){ store8(_texture_fmt_cache, 0, tex_id) }
   _free_texture_ids = _free_texture_ids.append(tex_id)
   _material_ds_cache = dict(64)
   0
}

fn read_framebuffer() any {
   "Reads the current swapchain image back to CPU memory. Returns {data, width, height, channels} or 0."
   def force_fresh = common.env_truthy("NY_VK_SNAPSHOT_FRESH")
   if(!force_fresh && _capture_ready && _capture_pixels && _capture_w > 0 && _capture_h > 0){
      def res = {"data": _capture_pixels, "width": _capture_w, "height": _capture_h, "bpp": 4}
      _capture_ready = false
      _capture_pixels = 0
      _capture_w = 0
      _capture_h = 0
      return res
   }
   if(!_device || !_swapchain || _image_index < 0){ return 0 }
   def w, h = _swapchain_extent_w, _swapchain_extent_h
   if(w <= 0 || h <= 0){ return 0 }
   def size = w * h * 4
   if(!_staging_buffer || !_staging_map || _staging_capacity < size){ return 0 }
   def readback_buf = _staging_buffer
   def mapped_data = _staging_map
   mut cb = 0
   if(!_ensure_readback_slab()){ return 0 }
   def ai = _readback_ai
   def cb_p = _readback_cb_p
   def bi = _readback_bi
   def barrier = _readback_barrier
   def region = _readback_region
   def s_info = _readback_s_info
   if(!ai || !cb_p || !bi || !barrier || !region || !s_info){ return 0 }
   memset(ai, 0, 32)
   store32(ai, _vk_stype_command_buffer_allocate_info(), 0)
   store64_h(ai, _command_pool, 16)
   store32(ai, 1, 28)
   memset(cb_p, 0, 8)
   if(allocate_command_buffers(_device, ai, cb_p) != 0){ return 0 }
   cb = load64(cb_p, 0)
   if(!cb){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   memset(bi, 0, 32)
   store32(bi, _vk_stype_command_buffer_begin_info(), 0)
   store32(bi, _vk_command_buffer_one_time_submit(), 16)
   if(begin_command_buffer(cb, bi) != 0){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   mut old_layout = _vk_layout_present_src()
   if(!_has_live_surface()){ old_layout = _vk_layout_color_attachment_optimal() }
   def src_image = _swapchain_images.get(_image_index)
   if(!_record_image_readback_to_buffer(cb, src_image, old_layout, readback_buf, w, h, barrier, region)){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   if(end_command_buffer(cb) != 0){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   memset(s_info, 0, 72)
   store32(s_info, _vk_stype_submit_info(), 0)
   store32(s_info, 1, 40)
   store64_h(s_info, cb_p, 48)
   if(queue_submit(_graphics_queue, 1, s_info, 0) != 0 || device_wait_idle(_device) != 0){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   if(!mapped_data){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   def pixels = malloc(size)
   if(!pixels){
      free_command_buffers(_device, _command_pool, 1, cb_p)
      return 0
   }
   __copy_mem(pixels, mapped_data, size)
   free_command_buffers(_device, _command_pool, 1, cb_p)
   _vkr_bgra_to_rgba_if_needed(pixels, size, _swapchain_format)
   if(_tex_fb_trace_enabled()){
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
   if(!_frame_open){ return 0 }
   def blit_sz = (_blit_tex_id == -1) ? 0 : texture_size(_blit_tex_id)
   if(_blit_tex_id == -1 || !is_list(blit_sz) || blit_sz.get(0, 0) != w || blit_sz.get(1, 0) != h){
      if(_blit_tex_id != -1){ destroy_texture(_blit_tex_id) }
      _blit_tex_id = create_texture(w, h, pixels)
   } else {
      update_texture_rect(_blit_tex_id, 0, 0, w, h, pixels)
   }
   def last_unlit = _current_is_unlit
   set_unlit(true)
   def ws_w, ws_h = float(_swapchain_extent_w), float(_swapchain_extent_h)
   mut old_mvp = mat4_identity()
   if(_current_mvp){ memcpy(old_mvp + 16, _current_mvp, 128) }
   set_ortho(0.0, ws_w, 0.0, ws_h, -1.0, 1.0)
   draw_rect_tex(0.0, 0.0, ws_w, ws_h, _blit_tex_id, 1.0, 1.0, 1.0, 1.0)
   _flush()
   set_mvp(old_mvp)
   set_unlit(last_unlit != 0)
   0
}

mut _skybox_cube = 0
mut _skybox_cube_tex_id = -1

fn _upload_cubemap_face_dedicated(any image, int face_size, any pixels, int layer) bool {
   if(!_graphics_queue || !_command_pool){ return false }
   def face_sz = face_size * face_size * 4
   if(!is_str(pixels) || pixels.len < face_sz){ return false }
   mut buf_ci = _tex_alloc(88)
   store32(buf_ci, _vk_stype_buffer_create_info(), 0)
   store64_h(buf_ci, face_sz, 24)
   store32(buf_ci, _vk_buffer_usage_transfer_src(), 32)
   store32(buf_ci, _vk_sharing_mode_exclusive(), 36)
   mut staging_buf_ptr = _tex_alloc(8)
   def res = create_buffer(_device, buf_ci, 0, staging_buf_ptr)
   if(res != 0){
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
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){
      free(buf_ci, staging_buf_ptr, mem_req, alloc_info, mem_ptr)
      return false
   }
   def staging_mem = load64_h(mem_ptr, 0)
   bind_buffer_memory(_device, staging_buf, staging_mem, 0)
   mut mapped = _tex_alloc(8)
   if(map_memory(_device, staging_mem, 0, face_sz, 0, mapped) != 0){
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
   queue_submit(_graphics_queue, 1, submit_info, fence)
   wait_for_fences(_device, 1, fence_ptr, 1, 0xFFFFFFFFFFFFFFFF)
   destroy_fence(_device, fence, 0)
   free_command_buffers(_device, _command_pool, 1, cb_ptr)
   free_memory(_device, staging_mem, 0)
   destroy_buffer(_device, staging_buf, 0)
   free(fence_ci, fence_ptr, submit_info, begin_info, cb_alloc, cb_ptr, mem_req, alloc_info, mem_ptr, buf_ci, staging_buf_ptr, bar, copy_region)
   true
}

fn _free_image_create_allocs(any img_ci, any img_ptr, any mem_req=0, any alloc_info=0, any mem_ptr=0, any view_ci=0, any view_ptr=0) any {
   if(img_ci){ free(img_ci) }
   if(img_ptr){ free(img_ptr) }
   if(mem_req){ free(mem_req) }
   if(alloc_info){ free(alloc_info) }
   if(mem_ptr){ free(mem_ptr) }
   if(view_ci){ free(view_ci) }
   if(view_ptr){ free(view_ptr) }
   0
}

fn create_cubemap(int face_size, list face_pixels_list) int {
   "Creates create cubemap."
   if(face_pixels_list.len != 6){ return -1 }
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
   if(r1 != 0){
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
   if(allocate_memory(_device, alloc_info, 0, mem_ptr) != 0){
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr)
      return -1
   }
   def memory = load64_h(mem_ptr, 0)
   bind_image_memory(_device, image, memory, 0)
   mut face_i = 0
   while(face_i < 6){
      def pixels = face_pixels_list.get(face_i)
      if(!_upload_cubemap_face_dedicated(image, face_size, pixels, face_i)){
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
   if(r3 != 0){
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
   if(tex_id < 0){
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      return -1
   }
   if(!_store_texture_slot(tex_id, tex, ds, format)){
      _destroy_texture_image_resources(view, image, memory)
      _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
      return -1
   }
   if(_bindless_ds){ bindless_sync_texture_slot(tex_id) }
   _free_image_create_allocs(img_ci, img_ptr, mem_req, alloc_info, mem_ptr, view_ci, view_ptr)
   return tex_id
}

fn draw_skybox(int tex_id) bool {
   "Draws draw skybox."
   if(tex_id < 0 || tex_id >= _textures.len){ return false }
   def tex = _textures.get(tex_id)
   if(!is_dict(tex)){ return false }
   if(!_skybox_pipeline && !_ensure_skybox_pipeline()){ return false }
   if(!_skybox_pipeline){ return false }
   def needs_rebuild = (!_skybox_cube || _skybox_cube_tex_id != tex_id || !_skybox_cube.get("cpu_ptr", 0))
   if(needs_rebuild){
      if(is_dict(_skybox_cube)){
         def old_ptr = _skybox_cube.get("cpu_ptr", 0)
         if(old_ptr){ free(old_ptr) }
      }
      def n = 36
      def buf = malloc(n * _VKR_VERT_STRIDE)
      if(!buf){ return false }
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
   if(!_skybox_cube){ return false }
   if(!_bindless_ds || !_pipeline_layout){ return false }
   def tex_obj = _textures.get(tex_id, 0)
   mut tex_view = 0
   mut tex_sampler = 0
   if(is_dict(tex_obj)){ tex_view, tex_sampler = tex_obj.get("view", 0), tex_obj.get("sampler", 0) }
   if(tex_id >= 0
      && (tex_id != _last_synced_skybox_tex
         || tex_view != _last_synced_skybox_view
      || tex_sampler != _last_synced_skybox_sampler)){
      bindless_sync_texture_slot(tex_id)
      _last_synced_skybox_tex = tex_id
      _last_synced_skybox_view = tex_view
      _last_synced_skybox_sampler = tex_sampler
   }
   def cpu_ptr = _skybox_cube.get("cpu_ptr", 0)
   def count = int(_skybox_cube.get("count", 36))
   if(!cpu_ptr || count <= 0){ return false }
   if(!_draw_raw_stream_current_material(cpu_ptr, count, _skybox_pipeline)){ return false }
   _last_bound_pipe = 0
   _pc_dirty = true
   true
}
