;; Keywords: render vulkan gpu utils
;; Vulkan utility routines for handles, memory, layout transitions, and debug formatting.
module std.os.ui.render.vk.utils(__vkr_push_rect_tex_fast, __vkr_push_quad_xyuv_fast, __vkr_push_rect_outline_fast, _vkr_color_u32, __vkr_pack_color, _vkr_store_vertex, __vkr_push_vertex, __vkr_push_rect_tex, _init_quad_template, __vkr_push_rect, __vkr_push_line, __vkr_push_rect_sdf, _check_debug_env, _dbg_handle, _get_vertex_offset, _get_local_vertex_map, _advance_vertex_offset, _vkr_bind_dynamic_vertex_buffer, _vkr_bind_pipeline_if_needed, _pack_color, _push_vertex, _vkr_safe_f32_limit, _vkr_bgra_to_rgba_if_needed, store_mat4_cm_raw, pack_emissive_u32, pack_normal_tex_word, pack_rgba_u32, pack_material_scalar_u32, pack_alpha_cutoff_u32, gltf_anim_apply_uv_pointer_override, gltf_anim_apply_material_pointer_overrides, gltf_expand_indexed_vertices, gltf_rewind_triangle_vertices, gltf_sync_drawable_part_from_raw, gltf_sync_drawable_parts_from_raw, pack_bsdf0_u32, pack_bsdf1_u32, pack_bsdf2_u32, pack_bsdf3_u32, pack_bsdf4_u32, pack_bsdf5_u32, pack_bsdf_ext_slab, pack_material_slab)
use std.core
use std.core.mem
use std.math
use std.math.float as fmath
use std.os
use std.os.ui.profile as ui_profile
use std.os.ui.render.vk.state
use std.os.ui.render.vk.vulkan (cmd_bind_vertex_buffers, cmd_bind_pipeline)
use std.core.common as common
use std.core.str (to_hex)
use std.math.crypto.encoding.bytes

mut _cached_ubo_env = -1
mut _cached_renderdoc_env = -1
def _VKR_SANITY_LIMIT = 1048576.0

comptime table GltfAnimUvSlotGroup {
   "baseColorTexture" -> "base"
   "normalTexture", "clearcoatNormalTexture" -> "normal"
   "metallicRoughnessTexture", "transmissionTexture", "sheenRoughnessTexture",
   "iridescenceThicknessTexture", "anisotropyTexture", "clearcoatRoughnessTexture" -> "mr"
   "occlusionTexture", "specularTexture", "clearcoatTexture", "thicknessTexture",
   "iridescenceTexture" -> "occlusion"
   "emissiveTexture", "specularColorTexture", "sheenColorTexture",
   "diffuseTransmissionColorTexture" -> "emissive"
}

comptime table GltfAnimPointerKind {
   "baseColorFactor" -> 1
   "emissiveFactor" -> 2
   "metallicFactor" -> 3
   "roughnessFactor" -> 4
   "alphaCutoff" -> 5
   "uvOffset" -> 6
   "uvScale" -> 7
   "uvRotation" -> 8
}

layout VkrMaterialSlab pack(4){
   i32: base_color_u32,
   i32: material_u32,
   i32: emissive_u32,
   i32: emissive_tex_id,
   i32: emissive_uv_set,
   i32: tex_id,
   i32: alpha_u32,
   i32: occlusion_id,
   i32: occlusion_uv_set,
   i32: bsdf0_u32,
   i32: bsdf1_u32,
   i32: bsdf2_u32,
   i32: bsdf3_u32,
   i32: base_uv_xf0,
   i32: base_uv_xf1,
   i32: normal_uv_xf0,
   i32: normal_uv_xf1,
   i32: mr_uv_xf0,
   i32: mr_uv_xf1,
   i32: occlusion_uv_xf0,
   i32: occlusion_uv_xf1,
   i32: emissive_uv_xf0,
   i32: emissive_uv_xf1,
   i32: visible,
   i32: node_idx,
   i32: normal_tex_word,
   ptr: mesh,
   ptr: model,
   i32: is_lines,
   f32: width,
   i32: unlit,
   ptr: bsdf_ext_slab,
   i32: is_points,
   i32: bsdf4_u32,
   i32: bsdf5_u32,
   i32: ext2_tex_word,
   i32: flip_winding
}

fn _vkr_safe_f32(any: v, f64: fallback=0.0): f64 { _vkr_safe_f32_limit(v, fallback, 1048576.0) }

fn _vkr_safe_f32_limit(any: v, f64: fallback=0.0, f64: limit=1048576.0): f64 {
   def fv = fmath.float(v)
   if(fmath.is_nan(fv) || fmath.is_inf(fv)){ return fallback }
   if(fv > limit){ return limit }
   if(fv < 0.0 - limit){ return 0.0 - limit }
   fv
}

fn store_mat4_cm_raw(any: dst, any: mat, bool: allow_plain16=false): bool {
   "Stores tagged column-major mat4, optionally accepting a plain 16-float list."
   if(!dst || !is_list(mat)){ return false }
   def n = mat.len
   if(n == 18){
      if(int(__load_item_fast(mat, 0)) != 4 || int(__load_item_fast(mat, 1)) != 4){ return false }
      mut i = 0
      while(i < 16){
         store32_f32(dst, __load_item_fast(mat, 2 + i), i * 4)
         i += 1
      }
      return true
   }
   if(allow_plain16 && n == 16){
      mut i = 0
      while(i < 16){
         store32_f32(dst, __load_item_fast(mat, i), i * 4)
         i += 1
      }
      return true
   }
   false
}

fn pack_normal_tex_word(int: normal_tex_id, int: normal_uv_set, f64: normal_scale=1.0, bool: clearcoat_only=false, bool: mirrored_double_sided=false, bool: double_sided=false): int {
   def tid = band(int(normal_tex_id), 0xffff)
   mut word = tid < MAX_TEXTURES ? tid : 0xffff
   if(band(int(normal_uv_set), 1) != 0){ word = bor(word, 0x10000) }
   if(mirrored_double_sided){ word = bor(word, 0x20000) }
   if(double_sided){ word = bor(word, 0x40000) }
   mut scl = float(normal_scale)
   if(scl >= 0.0){
      if(scl > 2.0){ scl = 2.0 }
      def scale_u7 = band(int((scl / 2.0) * 127.0 + 0.5), 127)
      word = bor(word, bshl(scale_u7, 24))
   }
   if(clearcoat_only){ word = bor(word, 0x80000000) }
   word
}

fn pack_bsdf0_u32(dict: minfo): int {
   "Packs specular/sheen-roughness/transmission/iridescence factors into u32."
   def spec = int((clamp01(float(minfo.get("specular_factor", 1.0))) * 255.0)) & 255
   def sheen_r = int((clamp01(float(minfo.get("sheen_roughness_factor", 0.0))) * 255.0)) & 255
   def trans = int((clamp01(float(minfo.get("transmission_factor", 0.0))) * 255.0)) & 255
   def iri = int((clamp01(float(minfo.get("iridescence_factor", 0.0))) * 255.0)) & 255
   spec | (sheen_r << 8) | (trans << 16) | (iri << 24)
}

fn pack_bsdf1_u32(dict: minfo): int {
   "Packs specular color RGB + IOR into u32."
   def spc = minfo.get("specular_color_factor", [1.0, 1.0, 1.0])
   mut sp_r, sp_g = float(spc.get(0, 1.0)), float(spc.get(1, 1.0))
   mut sp_b = float(spc.get(2, 1.0))
   if(sp_r < 0.0){ sp_r = 0.0 }
   if(sp_g < 0.0){ sp_g = 0.0 }
   if(sp_b < 0.0){ sp_b = 0.0 }
   mut peak = max(sp_r, max(sp_g, sp_b))
   if(peak < 1.0){ peak = 1.0 }
   def r, g = int((clamp01(sp_r / peak) * 255.0)) & 255, int((clamp01(sp_g / peak) * 255.0)) & 255
   def b = int((clamp01(sp_b / peak) * 255.0)) & 255
   def ior_u8 = int((clamp01((float(minfo.get("ior", 1.5)) - 1.0) / 1.5) * 255.0)) & 255
   r | (g << 8) | (b << 16) | (ior_u8 << 24)
}

fn pack_bsdf2_u32(dict: minfo): int {
   "Packs sheen color RGB + thickness into u32."
   def shc = minfo.get("sheen_color_factor", [0.0, 0.0, 0.0])
   def r = int((clamp01(float(shc.get(0, 0.0))) * 255.0)) & 255
   def g = int((clamp01(float(shc.get(1, 0.0))) * 255.0)) & 255
   def b = int((clamp01(float(shc.get(2, 0.0))) * 255.0)) & 255
   mut thickness_v = float(minfo.get("thickness_factor", 0.0))
   if(thickness_v < 0.0){ thickness_v = 0.0 } elif(thickness_v > 4.0){ thickness_v = 4.0 }
   mut t_u8 = int((thickness_v / 4.0) * 255.0 + 0.5) & 255
   if(thickness_v > 0.0 && t_u8 == 0){ t_u8 = 1 }
   r | (g << 8) | (b << 16) | (t_u8 << 24)
}

fn pack_bsdf3_u32(dict: minfo): int {
   "Packs attenuation color RGB + distance into u32."
   def att = minfo.get("attenuation_color", [1.0, 1.0, 1.0])
   mut att_r, att_g = clamp01(float(att.get(0, 1.0))), clamp01(float(att.get(1, 1.0)))
   mut att_b = clamp01(float(att.get(2, 1.0)))
   mut d_u8 = 255
   def spec_hdr_scale = float(minfo.get("specular_hdr_scale", 1.0))
   def raw_d = float(minfo.get("attenuation_distance", 0.0))
   def use_iri_pack =
   spec_hdr_scale <= 1.001 &&
   raw_d <= 0.0 &&
   float(minfo.get("iridescence_factor", 0.0)) > 0.0 &&
   abs(att_r - 1.0) <= 0.000001 &&
   abs(att_g - 1.0) <= 0.000001 &&
   abs(att_b - 1.0) <= 0.000001
   if(use_iri_pack){
      mut iri_ior = float(minfo.get("iridescence_ior", 1.3))
      mut iri_min = float(minfo.get("iridescence_thickness_min", 100.0))
      mut iri_max = float(minfo.get("iridescence_thickness_max", 400.0))
      if(iri_ior < 1.0){ iri_ior = 1.0 } elif(iri_ior > 3.0){ iri_ior = 3.0 }
      if(iri_min < 0.0){ iri_min = 0.0 } elif(iri_min > 800.0){ iri_min = 800.0 }
      if(iri_max < 0.0){ iri_max = 0.0 } elif(iri_max > 800.0){ iri_max = 800.0 }
      def iri_r = int((clamp01((iri_ior - 1.0) / 2.0) * 255.0 + 0.5)) & 255
      def iri_g = int((clamp01(iri_min / 800.0) * 255.0 + 0.5)) & 255
      def iri_b = int((clamp01(iri_max / 800.0) * 255.0 + 0.5)) & 255
      return iri_r | (iri_g << 8) | (iri_b << 16) | (254 << 24)
   }
   def r, g = int(att_r * 255.0) & 255, int(att_g * 255.0) & 255
   def b = int(att_b * 255.0) & 255
   if(spec_hdr_scale > 1.001){
      def clipped = min(spec_hdr_scale, 16.0)
      d_u8 = int((clamp01((clipped - 1.0) / 15.0) * 255.0 + 0.5)) & 255
   } else {
      if(raw_d <= 0.0){ d_u8 = 255 } else {
         d_u8 = int((sqrt(clamp01(raw_d / 10.0)) * 253.0 + 0.5)) & 255
         if(d_u8 < 1){ d_u8 = 1 }
         if(d_u8 > 253){ d_u8 = 253 }
      }
   }
   r | (g << 8) | (b << 16) | (d_u8 << 24)
}

fn pack_bsdf4_u32(dict: minfo): int {
   "Packs clearcoat/roughness/anisotropy/dispersion into u32."
   def cc = int((clamp01(float(minfo.get("clearcoat_factor", 0.0))) * 255.0)) & 255
   def ccr = int((clamp01(float(minfo.get("clearcoat_roughness_factor", 0.0))) * 255.0)) & 255
   def an = int((clamp01(float(minfo.get("anisotropy_strength", 0.0))) * 255.0)) & 255
   def dp = int((clamp01(float(minfo.get("dispersion", 0.0)) / 10.0) * 255.0)) & 255
   cc | (ccr << 8) | (an << 16) | (dp << 24)
}

fn pack_bsdf5_u32(dict: minfo): int {
   "Packs diffuse transmission/refraction/subsurface/alpha coverage into u32."
   def dt = int((clamp01(float(minfo.get("diffuse_transmission_factor", 0.0))) * 255.0)) & 255
   def rf = int((clamp01(float(minfo.get("refraction_factor", 0.0))) * 255.0)) & 255
   def ss = int((clamp01(float(minfo.get("subsurface_factor", 0.0))) * 255.0)) & 255
   def ac = int((clamp01(float(minfo.get("alpha_coverage", 1.0))) * 255.0)) & 255
   dt | (rf << 8) | (ss << 16) | (ac << 24)
}

fn pack_bsdf_ext_slab(any: minfo): any {
   "Allocates a 64-byte future-material slab."
   def slab = malloc(64)
   if(!slab){ return 0 }
   memset(slab, 0, 64)
   store32(slab, pack_bsdf4_u32(minfo), 0)
   store32(slab, pack_bsdf5_u32(minfo), 4)
   store32_f32(slab, float(minfo.get("anisotropy_rotation", 0.0)), 8)
   store32_f32(slab, float(minfo.get("iridescence_thickness_min", 100.0)), 12)
   store32_f32(slab, float(minfo.get("iridescence_thickness_max", 400.0)), 16)
   store32_f32(slab, float(minfo.get("refraction_roughness", 0.0)), 20)
   def dtc = minfo.get("diffuse_transmission_color_factor", [1.0, 1.0, 1.0])
   store32_f32(slab, float(dtc.get(0, 1.0)), 24)
   store32_f32(slab, float(dtc.get(1, 1.0)), 28)
   store32_f32(slab, float(dtc.get(2, 1.0)), 32)
   def ssc = minfo.get("subsurface_color_factor", [1.0, 1.0, 1.0])
   store32_f32(slab, float(ssc.get(0, 1.0)), 36)
   store32_f32(slab, float(ssc.get(1, 1.0)), 40)
   store32_f32(slab, float(ssc.get(2, 1.0)), 44)
   slab
}

fn pack_emissive_u32(any: emissive_factor, f64: emissive_strength=1.0): int {
   "Packs emissive factor * strength into RGB + shared scale."
   def ef = is_list(emissive_factor) ? emissive_factor : [0.0, 0.0, 0.0]
   mut strength = float(emissive_strength)
   if(strength < 0.0){ strength = 0.0 }
   mut r, g = float(ef.get(0, 0.0)) * strength, float(ef.get(1, 0.0)) * strength
   mut b = float(ef.get(2, 0.0)) * strength
   if(r < 0.0){ r = 0.0 }
   if(g < 0.0){ g = 0.0 }
   if(b < 0.0){ b = 0.0 }
   mut peak = max(r, max(g, b))
   if(peak <= 0.000001){ return 0 }
   if(peak > 64.0){ peak = 64.0 }
   def scale_u8 = int((peak / 64.0) * 255.0 + 0.5) & 255
   def rn = int((clamp01(r / peak) * 255.0 + 0.5)) & 255
   def gn = int((clamp01(g / peak) * 255.0 + 0.5)) & 255
   def bn = int((clamp01(b / peak) * 255.0 + 0.5)) & 255
   rn | (gn << 8) | (bn << 16) | (scale_u8 << 24)
}

fn pack_rgba_u32(list: v): int {
   "Packs animated glTF RGBA floats into 0xAARRGGBB."
   def r = int(clamp01(float(v.get(0, 1.0))) * 255.0 + 0.5) & 255
   def g = int(clamp01(float(v.get(1, 1.0))) * 255.0 + 0.5) & 255
   def b = int(clamp01(float(v.get(2, 1.0))) * 255.0 + 0.5) & 255
   def a = int(clamp01(float(v.get(3, 1.0))) * 255.0 + 0.5) & 255
   r | (g << 8) | (b << 16) | (a << 24)
}

fn pack_material_scalar_u32(int: cur_mat, str: kind, any: v): int {
   def mr_word = band(bshr(int(cur_mat), 16), 0xffff)
   def u8 = int(clamp01(float(v)) * 255.0 + 0.5) & 255
   mut metallic_u8 = int(cur_mat) & 255
   mut rough_u8 = (int(cur_mat) >> 8) & 255
   if(kind == "metallicFactor"){ metallic_u8 = u8 } elif(kind == "roughnessFactor"){ rough_u8 = u8 }
   metallic_u8 | (rough_u8 << 8) | (mr_word << 16)
}

fn pack_alpha_cutoff_u32(int: cur_alpha, any: v): int {
   def cutoff_u8 = int(clamp01(float(v)) * 255.0 + 0.5) & 255
   (int(cur_alpha) & 0xffff00ff) | (cutoff_u8 << 8)
}

fn _gltf_anim_decode_uv_offset16(int: q): f64 { -8.0 + (float(int(q) & 0xffff) / 65535.0) * 16.0 }

fn _gltf_anim_decode_uv_scale11(int: q): f64 {
   def v = int(q) & 2047
   if(v == 0){ return 1.0 }
   (float(v) / 2047.0) * 64.0 - 32.0
}

fn _gltf_anim_decode_uv_rot8(int: q): f64 { (float(int(q) & 255) / 255.0) * (2.0 * PI) - PI }

fn _gltf_anim_pack_uv_offset16(f64: v): int {
   def n = clamp01((float(v) + 8.0) / 16.0)
   int(n * 65535.0 + 0.5) & 0xffff
}

fn _gltf_anim_pack_uv_scale11(f64: v): int {
   if(abs(float(v) - 1.0) <= 0.000001){ return 0 }
   mut n = (float(v) + 32.0) / 64.0
   if(n < 0.0){ n = 0.0 }
   if(n > 1.0){ n = 1.0 }
   int(n * 2047.0 + 0.5) & 2047
}

fn _gltf_anim_pack_uv_rot8(f64: v): int {
   def n = clamp01((float(v) + PI) / (2.0 * PI))
   int(n * 255.0 + 0.5) & 255
}

fn _gltf_anim_unpack_uv_xf(int: word0, int: word1): dict {
   return {
      "offset": [_gltf_anim_decode_uv_offset16(word0), _gltf_anim_decode_uv_offset16(int(word0) >> 16)],
      "scale": [_gltf_anim_decode_uv_scale11(word1), _gltf_anim_decode_uv_scale11(int(word1) >> 11)],
      "rotation": _gltf_anim_decode_uv_rot8(int(word1) >> 22),
      "uv_set": (int(word1) >> 30) & 1
   }
}

fn _gltf_anim_pack_uv_xf_state(dict: st): list {
   def off = st.get("offset", [0.0, 0.0])
   def scl = st.get("scale", [1.0, 1.0])
   def rot = float(st.get("rotation", 0.0))
   def uv_set = int(st.get("uv_set", 0))
   def word0 = _gltf_anim_pack_uv_offset16(off.get(0, 0.0)) | (_gltf_anim_pack_uv_offset16(off.get(1, 0.0)) << 16)
   mut word1 = _gltf_anim_pack_uv_scale11(scl.get(0, 1.0)) | (_gltf_anim_pack_uv_scale11(scl.get(1, 1.0)) << 11)
   word1 = word1 | (_gltf_anim_pack_uv_rot8(rot) << 22)
   if(uv_set == 1){ word1 = word1 | 0x40000000 }
   [word0, word1]
}

fn _gltf_anim_uv_slot_group(str: slot): str { comptime match GltfAnimUvSlotGroup(slot, "") }

fn gltf_anim_apply_uv_pointer_override(any: out, any: mesh, any: slab, str: slot, str: kind, any: val): list {
   def grp = _gltf_anim_uv_slot_group(slot)
   if(grp == ""){ return [out, mesh] }
   def kind_code = comptime match GltfAnimPointerKind(kind, 0)
   def k0 = grp + "_uv_xf0"
   def k1 = grp + "_uv_xf1"
   def slab_off0 = grp == "base" ? 52 : (grp == "normal" ? 60 : (grp == "mr" ? 68 : (grp == "occlusion" ? 76 : 84)))
   def slab_off1 = slab_off0 + 4
   mut mesh_cur0, mesh_cur1 = 0, 0
   if(is_dict(mesh)){ mesh_cur0, mesh_cur1 = int(mesh.get(k0, 0)), int(mesh.get(k1, 0)) }
   def cur0, cur1 = int(out.get(k0, mesh_cur0)), int(out.get(k1, mesh_cur1))
   mut st = _gltf_anim_unpack_uv_xf(cur0, cur1)
   if(kind_code == 6 && is_list(val) && val.len >= 2){ st["offset"] = [float(val.get(0, 0.0)), float(val.get(1, 0.0))] } elif(kind_code == 7 && is_list(val) && val.len >= 2){
      st["scale"] = [float(val.get(0, 1.0)), float(val.get(1, 1.0))]
   } elif(kind_code == 8 && is_list(val) && val.len > 0){
      st["rotation"] = float(val.get(0, 0.0))
   } else {
      return [out, mesh]
   }
   def words = _gltf_anim_pack_uv_xf_state(st)
   def next0 = int(words.get(0, cur0))
   def next1 = int(words.get(1, cur1))
   out[k0] = next0
   out[k1] = next1
   if(is_dict(mesh)){
      mesh[k0] = next0
      mesh[k1] = next1
   }
   if(slab){
      store32(slab, next0, slab_off0)
      store32(slab, next1, slab_off1)
   }
   [out, mesh]
}

fn gltf_anim_apply_material_pointer_overrides(any: part, any: ptr_overrides): any {
   "Applies KHR_animation_pointer material overrides to a CPU render part/material slab."
   if(!is_dict(part) || !is_list(ptr_overrides) || ptr_overrides.len == 0){ return part }
   mut mesh = part.get("mesh", 0)
   mut mat_idx = int(part.get("mat_idx", int(part.get("material_idx", -1))))
   if(mat_idx < 0 && is_dict(mesh)){ mat_idx = int(mesh.get("mat_idx", int(mesh.get("material_idx", -1)))) }
   if(mat_idx < 0){ return part }
   mut slab = part.get("material_slab", 0)
   if(!slab && is_dict(mesh)){ slab = mesh.get("material_slab", 0) }
   if(!is_dict(mesh) && !slab){ return part }
   mut out = part
   mut changed = false
   mut mesh_changed = false
   mut oi = 0
   def overrides_n = ptr_overrides.len
   while(oi < overrides_n){
      def pr = ptr_overrides.get(oi, 0)
      if(is_dict(pr) && int(pr.get("material", -2)) == mat_idx){
         def kind = to_str(pr.get("kind", ""))
         def kind_code = comptime match GltfAnimPointerKind(kind, 0)
         def val = pr.get("value", [])
         if(kind_code == 1 && is_list(val)){
            def rgba = pack_rgba_u32(val)
            if(int(out.get("base_color_u32", -1)) != rgba || !out.get("animated_color_override", false)){
               out["base_color_u32"] = rgba
               out["animated_color_override"] = true
               changed = true
               if(slab){ store32(slab, rgba, 0) }
            }
            if(is_dict(mesh)
               && (int(mesh.get("base_color_u32", -1)) != rgba
               || !mesh.get("animated_color_override", false))){
               mesh["base_color_u32"] = rgba
               mesh["animated_color_override"] = true
               mesh_changed = true
            }
         } elif(kind_code == 2 && is_list(val)){
            def em = pack_emissive_u32(val)
            if(int(out.get("emissive_u32", -1)) != em){
               out["emissive_u32"] = em
               changed = true
               if(slab){ store32(slab, em, 8) }
            }
            if(is_dict(mesh) && int(mesh.get("emissive_u32", -1)) != em){
               mesh["emissive_u32"] = em
               mesh_changed = true
            }
         } elif((kind_code == 3 || kind_code == 4) && is_list(val) && val.len > 0){
            mut mesh_mat = 0x0000ff00
            if(is_dict(mesh)){ mesh_mat = int(mesh.get("material_u32", 0x0000ff00)) }
            def cur_mat = int(out.get("material_u32", mesh_mat))
            def next_mat = pack_material_scalar_u32(cur_mat, kind, val.get(0, 0.0))
            if(cur_mat != next_mat){
               out["material_u32"] = next_mat
               changed = true
               if(slab){ store32(slab, next_mat, 4) }
            }
            if(is_dict(mesh) && int(mesh.get("material_u32", 0x0000ff00)) != next_mat){
               mesh["material_u32"] = next_mat
               mesh_changed = true
            }
         } elif(kind_code == 5 && is_list(val) && val.len > 0){
            mut mesh_alpha = 0
            if(is_dict(mesh)){ mesh_alpha = int(mesh.get("alpha_u32", 0)) }
            def cur_alpha = int(out.get("alpha_u32", mesh_alpha))
            def next_alpha = pack_alpha_cutoff_u32(cur_alpha, val.get(0, 0.5))
            if(cur_alpha != next_alpha){
               out["alpha_u32"] = next_alpha
               changed = true
               if(slab){ store32(slab, next_alpha, 24) }
            }
            if(is_dict(mesh) && int(mesh.get("alpha_u32", 0)) != next_alpha){
               mesh["alpha_u32"] = next_alpha
               mesh_changed = true
            }
         } elif(kind_code >= 6 && kind_code <= 8 && is_list(val)){
            def uv_res = gltf_anim_apply_uv_pointer_override(out, mesh, slab, to_str(pr.get("slot", "")), kind, val)
            def next_out = uv_res.get(0, out)
            def next_mesh = uv_res.get(1, mesh)
            if(to_int(next_out) != to_int(out)){
               out = next_out
               changed = true
            }
            if(to_int(next_mesh) != to_int(mesh)){
               mesh = next_mesh
               mesh_changed = true
            }
         }
      }
      oi += 1
   }
   if(is_dict(mesh) && mesh_changed){
      out["mesh"] = mesh
      changed = true
   }
   if(!changed){ return part }
   out
}

fn gltf_sync_drawable_part_from_raw(any: part, any: raw_part, bool: update_part_tex=true, bool: update_part_material=true): any {
   "Updates a drawable part's mesh pointers/counts from a morphed raw glTF part."
   if(!is_dict(part) || !is_dict(raw_part)){ return part }
   def vptr = raw_part.get("vptr", 0)
   def vcnt = int(raw_part.get("vcnt", 0))
   def iptr = raw_part.get("iptr", 0)
   def icnt = int(raw_part.get("icnt", 0))
   mut mesh = part.get("mesh", 0)
   if(!is_dict(mesh) || !vptr || vcnt <= 0){ return part }
   mesh["ptr"] = vptr
   mesh["count"] = vcnt
   mesh["draw_count"] = vcnt
   if(iptr && icnt > 0){
      mesh["idx_ptr"] = iptr
      mesh["index_count"] = icnt
      mesh["draw_index_count"] = icnt
   }
   mesh["tex_id"] = int(raw_part.get("tex_id", mesh.get("tex_id", -1)))
   if(raw_part.contains("material_slab")){ mesh["material_slab"] = raw_part.get("material_slab", 0) }
   part["mesh"] = mesh
   part["vptr"] = vptr
   part["vcnt"] = vcnt
   if(update_part_tex){ part["tex_id"] = int(raw_part.get("tex_id", part.get("tex_id", -1))) }
   if(iptr && icnt > 0){
      part["iptr"] = iptr
      part["icnt"] = icnt
   }
   if(update_part_material && raw_part.contains("material_slab")){ part["material_slab"] = raw_part.get("material_slab", 0) }
   part
}

fn gltf_sync_drawable_parts_from_raw(any: existing_parts, any: raw_parts, bool: update_part_tex=true, bool: update_part_material=true): any {
   "Updates drawable mesh parts with morphed raw vertex/index buffers."
   if(!is_list(existing_parts) || !is_list(raw_parts)){ return existing_parts }
   if(existing_parts.len == 0 || raw_parts.len == 0){ return existing_parts }
   mut out = existing_parts
   mut i = 0
   while(i < existing_parts.len && i < raw_parts.len){
      def part = gltf_sync_drawable_part_from_raw(existing_parts.get(i, 0), raw_parts.get(i,
         0),
         update_part_tex,
      update_part_material)
      out[i] = part
      i += 1
   }
   out
}

fn gltf_expand_indexed_vertices(?ptr: vptr, int: vcnt, ?ptr: iptr, int: icnt, bool: idx_u32=false): ?ptr {
   "Expands indexed packed glTF vertices into a linear CPU vertex buffer."
   if(!vptr || vcnt <= 0 || !iptr || icnt <= 0){ return 0 }
   mut ?ptr: out = malloc(icnt * VERTEX_STRIDE)
   if(!out){ return 0 }
   memset(out, 0, VERTEX_STRIDE)
   def trace_expand = common.env_truthy("NY_GLTF_INDEX_TRACE") || common.env_truthy("NY_VK_CAPTURE_TRACE")
   def idx_step = idx_u32 ? 4 : 2
   def idx_bytes_n = icnt * idx_step
   def idx_bytes = bytes(idx_bytes_n)
   if(!idx_bytes){
      free(out)
      return 0
   }
   __copy_mem(idx_bytes, iptr, idx_bytes_n)
   if(trace_expand){
      ui_profile.print_text("[vk:expand:bytes] len=" + to_str(len(idx_bytes)) +
         " raw16=" + to_str(load16(iptr, 0)) + "," + to_str(load16(iptr, 2)) + "," + to_str(load16(iptr, 4)) + "," + to_str(load16(iptr, 6)) +
         " raw8=" + to_str(load8(iptr, 0)) + "," + to_str(load8(iptr, 1)) + "," + to_str(load8(iptr, 2)) + "," + to_str(load8(iptr, 3)) +
         " bidx=" + to_str(idx_bytes[0]) + "," + to_str(idx_bytes[1]) + "," + to_str(idx_bytes[2]) + "," + to_str(idx_bytes[3]) +
      " bload=" + to_str(load8(idx_bytes, 0)) + "," + to_str(load8(idx_bytes, 1)) + "," + to_str(load8(idx_bytes, 2)) + "," + to_str(load8(idx_bytes, 3)))
   }
   mut idx_off = 0
   mut dst_off = 0
   mut i = 0
   while(i < icnt){
      mut vi = 0
      if(idx_u32){
         vi = int(idx_bytes[idx_off]) |
         (int(idx_bytes[idx_off + 1]) << 8) |
         (int(idx_bytes[idx_off + 2]) << 16) |
         (int(idx_bytes[idx_off + 3]) << 24)
      } else {
         vi = int(idx_bytes[idx_off]) | (int(idx_bytes[idx_off + 1]) << 8)
      }
      if(trace_expand && i < 16){
         ui_profile.print_text("[vk:expand] i=" + to_str(i) +
            " vi=" + to_str(vi) +
            " idx_off=" + to_str(idx_off) +
         " vcnt=" + to_str(vcnt))
      }
      def dst_ptr = out + dst_off
      if(vi >= 0 && vi < vcnt){ __copy_mem(dst_ptr, vptr + vi * VERTEX_STRIDE, VERTEX_STRIDE) } else { __copy_mem(dst_ptr, out, VERTEX_STRIDE) }
      idx_off += idx_step
      dst_off += VERTEX_STRIDE
      i += 1
   }
   out
}

fn gltf_rewind_triangle_vertices(?ptr: vptr, int: vcnt): ?ptr {
   "Copies a linear triangle-list vertex buffer with every triangle winding reversed."
   if(!vptr || vcnt <= 0){ return 0 }
   mut ?ptr: out = malloc(vcnt * VERTEX_STRIDE)
   if(!out){ return 0 }
   mut i = 0
   while(i + 2 < vcnt){
      __copy_mem(out + (i + 0) * VERTEX_STRIDE, vptr + (i + 0) * VERTEX_STRIDE, VERTEX_STRIDE)
      __copy_mem(out + (i + 1) * VERTEX_STRIDE, vptr + (i + 2) * VERTEX_STRIDE, VERTEX_STRIDE)
      __copy_mem(out + (i + 2) * VERTEX_STRIDE, vptr + (i + 1) * VERTEX_STRIDE, VERTEX_STRIDE)
      i += 3
   }
   while(i < vcnt){
      __copy_mem(out + i * VERTEX_STRIDE, vptr + i * VERTEX_STRIDE, VERTEX_STRIDE)
      i += 1
   }
   out
}

fn pack_material_slab(any: part): any {
   "Packs a material record into the native Vulkan material slab layout."
   if(!is_dict(part)){ return 0 }
   def slab = malloc(160)
   if(!slab){ return 0 }
   memset(slab, 0, 160)
   def base_color_u32 = int(part.get("base_color_u32", 0xffffffff))
   def material_u32 = int(part.get("material_u32", 0x0000ff00))
   def emissive_u32 = int(part.get("emissive_u32", 0))
   def emissive_tex_id = int(part.get("emissive_tex_id", -1))
   def emissive_uv_set = int(part.get("emissive_uv_set", 0))
   def tex_id = int(part.get("tex_id", -1))
   def alpha_u32 = int(part.get("alpha_u32", 0))
   def occlusion_id = int(part.get("occlusion", -1))
   def occlusion_uv_set = int(part.get("occlusion_uv_set", 0))
   def normal_tex_id = int(part.get("normal_tex_id", -1))
   def normal_uv_set = int(part.get("normal_uv_set", 0))
   def bsdf0_u32 = int(part.get("bsdf0_u32", 0))
   def bsdf1_u32 = int(part.get("bsdf1_u32", 0))
   def bsdf2_u32 = int(part.get("bsdf2_u32", 0))
   def bsdf3_u32 = int(part.get("bsdf3_u32", 0))
   def bsdf4_u32 = int(part.get("bsdf4_u32", 0))
   def bsdf5_u32 = int(part.get("bsdf5_u32", 0))
   def ext2_tex_word = int(part.get("ext2_tex_word", 0x80000000))
   def base_uv_xf0 = int(part.get("base_uv_xf0", 0))
   def base_uv_xf1 = int(part.get("base_uv_xf1", 0))
   def normal_uv_xf0 = int(part.get("normal_uv_xf0", 0))
   def normal_uv_xf1 = int(part.get("normal_uv_xf1", 0))
   def mr_uv_xf0 = int(part.get("mr_uv_xf0", 0))
   def mr_uv_xf1 = int(part.get("mr_uv_xf1", 0))
   def occlusion_uv_xf0 = int(part.get("occlusion_uv_xf0", 0))
   def occlusion_uv_xf1 = int(part.get("occlusion_uv_xf1", 0))
   def emissive_uv_xf0 = int(part.get("emissive_uv_xf0", 0))
   def emissive_uv_xf1 = int(part.get("emissive_uv_xf1", 0))
   def visible = part.get("visible", true) ? 1 : 0
   def node_idx = int(part.get("node_idx", -1))
   def mesh = part.get("mesh", 0)
   def model = part.get("model", 0)
   def part_opts = part.get("opts", 0)
   def part_opts_is_dict = is_dict(part_opts)
   mut opt_is_lines = false
   mut opt_is_points = false
   mut opt_unlit = false
   if(part_opts_is_dict){
      opt_is_lines, opt_is_points = part_opts.get("is_lines", false), part_opts.get("is_points", false)
      opt_unlit = part_opts.get("unlit", false)
   }
   def is_lines = part.get("is_lines", opt_is_lines) ? 1 : 0
   def is_points = part.get("is_points", opt_is_points) ? 1 : 0
   def width = float(part.get("width", 1.0))
   def unlit = part.get("unlit", opt_unlit) ? 1 : 0
   mut flip_winding_bool = part.get("flip_winding", false)
   mut double_sided_bool = part.get("double_sided", false)
   if(part_opts_is_dict){
      if(!flip_winding_bool){ flip_winding_bool = part_opts.get("flip_winding", false) }
      if(!double_sided_bool){ double_sided_bool = part_opts.get("double_sided", false) }
   }
   def flip_winding = flip_winding_bool ? 1 : 0
   def double_sided = double_sided_bool ? 1 : 0
   def bsdf_ext_slab = part.get("bsdf_ext_slab", 0)
   mut normal_tex_word = 0
   if(part.contains("normal_tex_word")){ normal_tex_word = int(part.get("normal_tex_word", 0)) }
   else { normal_tex_word = pack_normal_tex_word(normal_tex_id, normal_uv_set, -1.0) }
   normal_tex_word = bor(normal_tex_word, 0x80000)
   if(flip_winding != 0){ normal_tex_word = bor(normal_tex_word, 0x20000) }
   if(double_sided != 0){ normal_tex_word = bor(normal_tex_word, 0x40000) }
   store_layout(slab, "VkrMaterialSlab", base_color_u32, material_u32, emissive_u32, emissive_tex_id, emissive_uv_set, tex_id,
      alpha_u32, occlusion_id, occlusion_uv_set, bsdf0_u32, bsdf1_u32, bsdf2_u32, bsdf3_u32,
      base_uv_xf0, base_uv_xf1, normal_uv_xf0, normal_uv_xf1, mr_uv_xf0, mr_uv_xf1,
      occlusion_uv_xf0, occlusion_uv_xf1, emissive_uv_xf0, emissive_uv_xf1, visible,
      node_idx, normal_tex_word, mesh, model, is_lines, width, unlit, bsdf_ext_slab,
   is_points, bsdf4_u32, bsdf5_u32, ext2_tex_word, flip_winding)
   slab
}

@inline
fn __vkr_push_quad_xyuv_fast(any: p, any: x0, any: y0, any: x1, any: y1, any: u1, any: v1, any: u2, any: v2, any: color_u32, any: tex_id=0): any {
   if(!p){ return 0 }
   _vkr_write_quad_xyuv_fast(p, x0, y0, x1, y1, u1, v1, u2, v2, _vkr_color_u32(color_u32), tex_id)
   p
}

fn __vkr_push_rect_tex_fast(any: p, any: x, any: y, any: w, any: h, any: u1, any: v1, any: u2, any: v2, any: color_u32, any: tex_id=0): any {
   if(!p){ return 0 }
   def x0, y0 = _vkr_safe_f32(x), _vkr_safe_f32(y)
   def x1, y1 = _vkr_safe_f32(x0 + _vkr_safe_f32(w)), _vkr_safe_f32(y0 + _vkr_safe_f32(h))
   _vkr_write_quad_xyuv_fast(p, x0, y0, x1, y1, u1, v1, u2, v2, _vkr_color_u32(color_u32), tex_id)
   p
}

@jit
fn __vkr_push_rect_outline_fast(any: p, any: x, any: y, any: w, any: h, any: color_u32, any: tex_id=0): any {
   if(!p){ return 0 }
   def x0, y0 = _vkr_safe_f32(x), _vkr_safe_f32(y)
   def x1, y1 = _vkr_safe_f32(x0 + _vkr_safe_f32(w)), _vkr_safe_f32(y0 + _vkr_safe_f32(h))
   def c = _vkr_color_u32(color_u32)
   _vkr_write_quad_xyuv_fast(p + 0 * _VKR_VERT_STRIDE * 6, x0, y0, x1, _vkr_safe_f32(y0 + 1.0), 0.0, 0.0, 0.0, 0.0, c, tex_id)
   _vkr_write_quad_xyuv_fast(p + 1 * _VKR_VERT_STRIDE * 6, x0, _vkr_safe_f32(y1 - 1.0), x1, y1, 0.0, 0.0, 0.0, 0.0, c, tex_id)
   _vkr_write_quad_xyuv_fast(p + 2 * _VKR_VERT_STRIDE * 6, x0, y0, _vkr_safe_f32(x0 + 1.0), y1, 0.0, 0.0, 0.0, 0.0, c, tex_id)
   _vkr_write_quad_xyuv_fast(p + 3 * _VKR_VERT_STRIDE * 6, _vkr_safe_f32(x1 - 1.0), y0, x1, y1, 0.0, 0.0, 0.0, 0.0, c, tex_id)
   p
}

@jit
fn _vkr_push_quad_vertex_fast(any: v, any: x, any: y, any: u, any: uv, int: color_u32, any: tex_id): any {
   store32_f32(v, _vkr_safe_f32(x), _VKR_OFF_X)
   store32_f32(v, _vkr_safe_f32(y), _VKR_OFF_Y)
   store32_f32(v, 0.0, _VKR_OFF_Z)
   store32_f32(v, _vkr_safe_f32(u), _VKR_OFF_U)
   store32_f32(v, _vkr_safe_f32(uv), _VKR_OFF_V)
   store32(v, color_u32, _VKR_OFF_C)
   store32_f32(v, 0.0, _VKR_OFF_NX)
   store32_f32(v, 0.0, _VKR_OFF_NY)
   store32_f32(v, 1.0, _VKR_OFF_NZ)
   store32_f32(v, 1.0, _VKR_OFF_TX)
   store32_f32(v, 0.0, _VKR_OFF_TY)
   store32_f32(v, 0.0, _VKR_OFF_TZ)
   store32_f32(v, 1.0, _VKR_OFF_TW)
   store32_f32(v, 0.0, _VKR_OFF_U2)
   store32_f32(v, 0.0, _VKR_OFF_V2)
   store32(v, tex_id, _VKR_OFF_TEX)
}

@jit
fn _vkr_write_quad_xyuv_fast(any: p, any: x0, any: y0, any: x1, any: y1, any: u1, any: v1, any: u2, any: v2, int: color_u32, any: tex_id): any {
   _vkr_push_quad_vertex_fast(p + 0 * _VKR_VERT_STRIDE, x0, y0, u1, v1, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 1 * _VKR_VERT_STRIDE, x0, y1, u1, v2, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 2 * _VKR_VERT_STRIDE, x1, y1, u2, v2, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 3 * _VKR_VERT_STRIDE, x1, y1, u2, v2, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 4 * _VKR_VERT_STRIDE, x1, y0, u2, v1, color_u32, tex_id)
   _vkr_push_quad_vertex_fast(p + 5 * _VKR_VERT_STRIDE, x0, y0, u1, v1, color_u32, tex_id)
}

fn _vkr_color_u32(any: c): int {
   if(is_int(c)){ return c }
   if(is_float(c)){ return __flt_to_int(c) }
   if(!is_list(c)){ return 0xFFFFFFFF }
   _pack_color(c.get(0, 1.0), c.get(1, 1.0), c.get(2, 1.0), c.get(3, 1.0))
}

@pure
@jit
fn __vkr_pack_color(any: r, any: g, any: b, any: a): int {
   def r8, g8 = __flt_to_int(float(r) * 255.0) & 255, __flt_to_int(float(g) * 255.0) & 255
   def b8, a8 = __flt_to_int(float(b) * 255.0) & 255, __flt_to_int(float(a) * 255.0) & 255
   (a8 << 24) | (r8 << 16) | (g8 << 8) | b8
}

@jit
fn _vkr_store_vertex(any: base, int: idx, any: x, any: y, any: z, any: u, any: v, any: color, any: tex_id=0, any: nx=0.0, any: ny=0.0, any: nz=1.0): any {
   def off = base + idx * _VKR_VERT_STRIDE
   store32_f32(off, _vkr_safe_f32(x), _VKR_OFF_X)
   store32_f32(off, _vkr_safe_f32(y), _VKR_OFF_Y)
   store32_f32(off, _vkr_safe_f32(z), _VKR_OFF_Z)
   store32_f32(off, _vkr_safe_f32(u), _VKR_OFF_U)
   store32_f32(off, _vkr_safe_f32(v), _VKR_OFF_V)
   store32(off, _vkr_color_u32(color), _VKR_OFF_C)
   store32(off, tex_id, _VKR_OFF_TEX)
   store32_f32(off, _vkr_safe_f32(nx), _VKR_OFF_NX)
   store32_f32(off, _vkr_safe_f32(ny), _VKR_OFF_NY)
   store32_f32(off, _vkr_safe_f32(nz, 1.0), _VKR_OFF_NZ)
   store32_f32(off, 0.0, _VKR_OFF_U2)
   store32_f32(off, 0.0, _VKR_OFF_V2)
}

@jit
fn __vkr_push_vertex(any: p, any: x, any: y, any: z, any: u, any: v, any: color, any: tex_id=0, any: nx=0.0, any: ny=0.0, any: nz=1.0): any {
   if(!p){ return 0 }
   _vkr_store_vertex(p, 0, x, y, z, u, v, color, tex_id, nx, ny, nz)
}

@jit
fn __vkr_push_rect_tex(any: p, any: x, any: y, any: w, any: h, any: u1, any: v1, any: u2, any: v2, any: color, any: tex_id=0, any: nz=1.0): any {
   if(!p){ return 0 }
   __copy_mem(p, _quad_template, _VKR_VERT_STRIDE * 6)
   def c = _vkr_color_u32(color)
   x, y = _vkr_safe_f32(x), _vkr_safe_f32(y)
   u1, v1 = _vkr_safe_f32(u1), _vkr_safe_f32(v1)
   u2, v2 = _vkr_safe_f32(u2), _vkr_safe_f32(v2)
   def x2, y2 = _vkr_safe_f32(x + _vkr_safe_f32(w)), _vkr_safe_f32(y + _vkr_safe_f32(h))
   mut bv = p
   store32_f32(bv, x, _VKR_OFF_X)
   store32_f32(bv, y, _VKR_OFF_Y)
   store32_f32(bv, u1, _VKR_OFF_U)
   store32_f32(bv, v1, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32(bv,
      tex_id,
   _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, u1, _VKR_OFF_U)
   store32_f32(bv, v2, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32(bv,
      tex_id,
   _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, u2, _VKR_OFF_U)
   store32_f32(bv, v2, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32(bv,
      tex_id,
   _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, u2, _VKR_OFF_U)
   store32_f32(bv, v2, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32(bv,
      tex_id,
   _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y, _VKR_OFF_Y)
   store32_f32(bv, u2, _VKR_OFF_U)
   store32_f32(bv, v1, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32(bv,
      tex_id,
   _VKR_OFF_TEX)
   bv += _VKR_VERT_STRIDE
   store32_f32(bv, x, _VKR_OFF_X)
   store32_f32(bv, y, _VKR_OFF_Y)
   store32_f32(bv, u1, _VKR_OFF_U)
   store32_f32(bv, v1, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32(bv,
      tex_id,
   _VKR_OFF_TEX)
   0
}

fn _init_quad_template(): any {
   if(!_quad_template){ return 0 }
   mut i = 0 while(i < 6){
      def off = _quad_template + i * _VKR_VERT_STRIDE
      store32_f32(off, 0.0, _VKR_OFF_Z) ; Z
      store32(off, 0, _VKR_OFF_TEX) ; Tex index
      store32_f32(off, 0.0, _VKR_OFF_NX) ; NX
      store32_f32(off, 0.0, _VKR_OFF_NY) ; NY
      store32_f32(off, 1.0, _VKR_OFF_NZ) ; NZ
      store32_f32(off, 1.0, _VKR_OFF_TX)
      store32_f32(off, 0.0, _VKR_OFF_TY)
      store32_f32(off, 0.0, _VKR_OFF_TZ)
      store32_f32(off, 1.0, _VKR_OFF_TW)
      store32_f32(off, 0.0, _VKR_OFF_U2)
      store32_f32(off, 0.0, _VKR_OFF_V2)
      i += 1
   }
}

@jit
fn __vkr_push_rect(any: p, any: x, any: y, any: w, any: h, any: color): any {
   if(!p){ return 0 }
   def c = _vkr_color_u32(color)
   def x0, y0 = _vkr_safe_f32(x), _vkr_safe_f32(y)
   def x1, y1 = _vkr_safe_f32(x0 + _vkr_safe_f32(w)), _vkr_safe_f32(y0 + _vkr_safe_f32(h))
   if(_quad_template){
      _vkr_write_quad_xyuv_fast(p, x0, y0, x1, y1, 0.0, 0.0, 0.0, 0.0, c, _current_tex_index)
      return p
   }
   _vkr_store_vertex(p, 0, x0, y0, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 1, x0, y1, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 2, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 3, x1, y1, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 4, x1, y0, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 5, x0, y0, 0.0, 0.0, 0.0, c, _current_tex_index, 0.0, 0.0, 1.0)
}

fn __vkr_push_line(any: p, any: x1, any: y1, any: x2, any: y2, any: thickness, any: color): any {
   if(!p){ return 0 }
   def dx, dy = float(x2) - float(x1), float(y2) - float(y1)
   def l = sqrt(dx*dx + dy*dy)
   if(l == 0.0){ return 0 }
   def th = float(thickness) * 0.5
   def nx = -dy / l * th
   def ny =  dx / l * th
   _vkr_store_vertex(p, 0, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 1, float(x1) - nx, float(y1) - ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 2, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 3, float(x1) + nx, float(y1) + ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 4, float(x2) - nx, float(y2) - ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
   _vkr_store_vertex(p, 5, float(x2) + nx, float(y2) + ny, 0.0, 0.0, 0.0, color, _current_tex_index, 0.0, 0.0, 1.0)
}

@jit
fn __vkr_push_rect_sdf(any: p, any: x, any: y, any: w, any: h, any: c, any: nx, any: ny, any: nz): any {
   if(!p){ return 0 }
   __copy_mem(p, _quad_template, _VKR_VERT_STRIDE * 6)
   def x2, y2 = float(x) + float(w), float(y) + float(h)
   mut bv = p
   ; Vert 1
   store32_f32(bv, float(x), _VKR_OFF_X)
   store32_f32(bv, float(y), _VKR_OFF_Y)
   store32_f32(bv, 0.0, _VKR_OFF_U)
   store32_f32(bv, 0.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   ; Vert 2
   store32_f32(bv, float(x), _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, 0.0, _VKR_OFF_U)
   store32_f32(bv, 1.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   ; Vert 3
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, 1.0, _VKR_OFF_U)
   store32_f32(bv, 1.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   ; Vert 4
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, y2, _VKR_OFF_Y)
   store32_f32(bv, 1.0, _VKR_OFF_U)
   store32_f32(bv, 1.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   ; Vert 5
   store32_f32(bv, x2, _VKR_OFF_X)
   store32_f32(bv, float(y), _VKR_OFF_Y)
   store32_f32(bv, 1.0, _VKR_OFF_U)
   store32_f32(bv, 0.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv, nz, _VKR_OFF_NZ)
   bv += _VKR_VERT_STRIDE
   ; Vert 6
   store32_f32(bv, float(x), _VKR_OFF_X)
   store32_f32(bv, float(y), _VKR_OFF_Y)
   store32_f32(bv, 0.0, _VKR_OFF_U)
   store32_f32(bv, 0.0, _VKR_OFF_V)
   store32(bv, c, _VKR_OFF_C)
   store32_f32(bv, nx, _VKR_OFF_NX)
   store32_f32(bv, ny, _VKR_OFF_NY)
   store32_f32(bv,
      nz,
   _VKR_OFF_NZ)
}

fn _check_debug_env(): any {
   if(_cached_ubo_env < 0){
      case ui_profile.env_lower_cached("NYTRIX_UBO"){
         "1", "true", "on", "yes" -> { _cached_ubo_env = 1 }
         "0", "false", "off", "no" -> { _cached_ubo_env = 2 }
         _ -> { _cached_ubo_env = 0 }
      }
   }
   if(_cached_renderdoc_env < 0){ _cached_renderdoc_env = (common.env_present("RENDERDOC") || common.env_present("RENDERDOC_CAPTUREOPTS") || common.env_present("RENDERDOC_CMD")) ? 1 : 0 }
   if(ui_profile.debug_enabled()){ _debug_gfx_enabled = true }
   if(_cached_ubo_env == 1){
      if(ui_profile.env_truthy_cached("NYTRIX_UBO_FORCE")){ _ubo_enabled = true } else {
         _ubo_enabled = false
         if(_debug_gfx_enabled){ ui_profile.print_text("[gfx:vulkan] UBO requested but disabled(use NYTRIX_UBO_FORCE=1 to force)") }
      }
   }
   if(_cached_ubo_env == 2){ _ubo_enabled = false }
   if(_cached_renderdoc_env == 1){ if(_debug_gfx_enabled){ ui_profile.print_text("[gfx:vulkan] RenderDoc detected; bindless remains enabled by design.") } }
}

fn _dbg_handle(any: label, any: h): int {
   if(_debug_gfx_enabled){ ui_profile.print_text("[gfx:vulkan] " + label + " h=0x" + to_hex(h)) }
   0
}

mut _cfg_msaa = 1

fn _get_vertex_offset(): int { _vertex_offset }

fn _get_local_vertex_map(): any { _local_vertex_map }

fn _advance_vertex_offset(any: bytes): any { _vertex_offset += bytes }

@inline
fn _vkr_bind_dynamic_vertex_buffer(any: cb): any {
   if(!_dynamic_vbo_bound){
      store64_h(_flush_off, _current_frame_vertex_offset, 0)
      if(_vertex_buffer_raw){ __copy_mem(_flush_buf, _vertex_buffer_raw, 8) }
      else { store64(_flush_buf, _vertex_buffer, 0) }
      cmd_bind_vertex_buffers(cb, 0, 1, _flush_buf, _flush_off)
      _dynamic_vbo_bound = true
   }
}

@inline
fn _vkr_bind_pipeline_if_needed(any: cb, any: target): any {
   if(_last_bound_pipe != target){
      cmd_bind_pipeline(cb, 0, target)
      _last_bound_pipe = target
      _pipeline_bind_count += 1
   }
}

fn _vkr_bgra_to_rgba_if_needed(any: pixels, int: size, int: format): any {
   if(!pixels || size <= 0){ return 0 }
   if(format < 44 || format > 52){ return 0 }
   mut b = 0
   while(b < size){
      def blue = load8(pixels, b)
      def red  = load8(pixels, b + 2)
      store8(pixels, red, b)
      store8(pixels, blue, b + 2)
      b += 4
   }
}

@pure
@jit
fn _pack_color(any: r, any: g, any: b, any: a): int { (int(r * 255.0) & 0xFF) | ((int(g * 255.0) & 0xFF) << 8) | ((int(b * 255.0) & 0xFF) << 16) | ((int(a * 255.0) & 0xFF) << 24) }

@jit
fn _push_vertex(any: x, any: y, any: z, any: u, any: v, any: r, any: g, any: b, any: a, any: tex_id=0): any {
   def off = _local_vertex_map + _vertex_offset
   ; Ensure we use raw floats to avoid object tagging artifacts in the buffer.
   store32_f32(off, _vkr_safe_f32(x), _VKR_OFF_X)
   store32_f32(off, _vkr_safe_f32(y), _VKR_OFF_Y)
   store32_f32(off, _vkr_safe_f32(z), _VKR_OFF_Z)
   store32_f32(off, _vkr_safe_f32(u), _VKR_OFF_U)
   store32_f32(off, _vkr_safe_f32(v), _VKR_OFF_V)
   store32(off, _pack_color(r, g, b, a), _VKR_OFF_C)
   store32(off, tex_id, _VKR_OFF_TEX)
   store32_f32(off, 0.0, _VKR_OFF_NX)
   store32_f32(off, 0.0, _VKR_OFF_NY)
   store32_f32(off, 1.0, _VKR_OFF_NZ)
   store32_f32(off, 0.0, _VKR_OFF_U2)
   store32_f32(off, 0.0, _VKR_OFF_V2)
   _vertex_offset += _VKR_VERT_STRIDE
}
