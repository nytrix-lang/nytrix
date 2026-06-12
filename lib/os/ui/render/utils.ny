;; Keywords: render utils gltf material packing mesh os ui
;; Backend-neutral render utility routines for material packing and glTF CPU helpers.
module std.os.ui.render.utils(
   pack_emissive_u32, pack_normal_tex_word, pack_rgba_u32,
   pack_material_scalar_u32, pack_alpha_cutoff_u32,
   gltf_anim_apply_uv_pointer_override,
   gltf_anim_apply_material_pointer_overrides,
   gltf_expand_indexed_vertices, gltf_rewind_triangle_vertices,
   gltf_sync_drawable_part_from_raw, gltf_sync_drawable_parts_from_raw,
   pack_bsdf0_u32, pack_bsdf1_u32, pack_bsdf2_u32, pack_bsdf3_u32,
   pack_bsdf4_u32, pack_bsdf5_u32, pack_bsdf_ext_slab, pack_material_slab
)

use std.core
use std.core.mem
use std.math
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.shared as render_shared
use std.core.common as common
use std.math.crypto.encoding.bytes

def MAX_TEXTURES = render_shared.MAX_TEXTURES

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

fn pack_normal_tex_word(int normal_tex_id, int normal_uv_set, f64 normal_scale=1.0, bool clearcoat_only=false, bool mirrored_double_sided=false, bool double_sided=false) int {
   "Packs normal texture id, uv set, scale, and sidedness flags."
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

fn pack_bsdf0_u32(dict minfo) int {
   "Packs specular/sheen-roughness/transmission/iridescence factors into u32."
   def spec = int((clamp01(float(minfo.get("specular_factor", 1.0))) * 255.0)) & 255
   def sheen_r = int((clamp01(float(minfo.get("sheen_roughness_factor", 0.0))) * 255.0)) & 255
   def trans = int((clamp01(float(minfo.get("transmission_factor", 0.0))) * 255.0)) & 255
   def iri = int((clamp01(float(minfo.get("iridescence_factor", 0.0))) * 255.0)) & 255
   spec | (sheen_r << 8) | (trans << 16) | (iri << 24)
}

fn pack_bsdf1_u32(dict minfo) int {
   "Packs specular color RGB + IOR into u32."
   def spc = minfo.get("specular_color_factor", [1.0, 1.0, 1.0])
   def r = int((clamp01(float(spc.get(0, 1.0))) * 255.0)) & 255
   def g = int((clamp01(float(spc.get(1, 1.0))) * 255.0)) & 255
   def b = int((clamp01(float(spc.get(2, 1.0))) * 255.0)) & 255
   def ior_u8 = int((clamp01((float(minfo.get("ior", 1.5)) - 1.0) / 1.5) * 255.0)) & 255
   r | (g << 8) | (b << 16) | (ior_u8 << 24)
}

fn pack_bsdf2_u32(dict minfo) int {
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

fn pack_bsdf3_u32(dict minfo) int {
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

fn pack_bsdf4_u32(dict minfo) int {
   "Packs clearcoat/roughness/anisotropy/dispersion into u32."
   def cc = int((clamp01(float(minfo.get("clearcoat_factor", 0.0))) * 255.0)) & 255
   def ccr = int((clamp01(float(minfo.get("clearcoat_roughness_factor", 0.0))) * 255.0)) & 255
   def an = int((clamp01(float(minfo.get("anisotropy_strength", 0.0))) * 255.0)) & 255
   def dp = int((clamp01(float(minfo.get("dispersion", 0.0)) / 10.0) * 255.0)) & 255
   cc | (ccr << 8) | (an << 16) | (dp << 24)
}

fn pack_bsdf5_u32(dict minfo) int {
   "Packs diffuse transmission/refraction/subsurface/alpha coverage into u32."
   def dt = int((clamp01(float(minfo.get("diffuse_transmission_factor", 0.0))) * 255.0)) & 255
   def rf = int((clamp01(float(minfo.get("refraction_factor", 0.0))) * 255.0)) & 255
   def ss = int((clamp01(float(minfo.get("subsurface_factor", 0.0))) * 255.0)) & 255
   def ac = int((clamp01(float(minfo.get("alpha_coverage", 1.0))) * 255.0)) & 255
   dt | (rf << 8) | (ss << 16) | (ac << 24)
}

fn pack_bsdf_ext_slab(any minfo) any {
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

fn pack_emissive_u32(any emissive_factor, f64 emissive_strength=1.0) int {
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

fn pack_rgba_u32(list v) int {
   "Packs animated glTF RGBA floats into 0xAARRGGBB."
   def r = int(clamp01(float(v.get(0, 1.0))) * 255.0 + 0.5) & 255
   def g = int(clamp01(float(v.get(1, 1.0))) * 255.0 + 0.5) & 255
   def b = int(clamp01(float(v.get(2, 1.0))) * 255.0 + 0.5) & 255
   def a = int(clamp01(float(v.get(3, 1.0))) * 255.0 + 0.5) & 255
   r | (g << 8) | (b << 16) | (a << 24)
}

fn pack_material_scalar_u32(int cur_mat, str kind, any v) int {
   "Packs material scalar animation override into the material word."
   def mr_word = band(bshr(int(cur_mat), 16), 0xffff)
   def u8 = int(clamp01(float(v)) * 255.0 + 0.5) & 255
   mut metallic_u8 = int(cur_mat) & 255
   mut rough_u8 = (int(cur_mat) >> 8) & 255
   if(kind == "metallicFactor"){ metallic_u8 = u8 } elif(kind == "roughnessFactor"){ rough_u8 = u8 }
   metallic_u8 | (rough_u8 << 8) | (mr_word << 16)
}

fn pack_alpha_cutoff_u32(int cur_alpha, any v) int {
   "Packs alpha cutoff animation override into the alpha word."
   def cutoff_u8 = int(clamp01(float(v)) * 255.0 + 0.5) & 255
   (int(cur_alpha) & 0xffff00ff) | (cutoff_u8 << 8)
}

fn _gltf_anim_decode_uv_offset16(int q) f64 { -8.0 + (float(int(q) & 0xffff) / 65535.0) * 16.0 }

fn _gltf_anim_decode_uv_scale11(int q) f64 {
   def v = int(q) & 2047
   if(v == 0){ return 1.0 }
   (float(v) / 2047.0) * 64.0 - 32.0
}

fn _gltf_anim_decode_uv_rot8(int q) f64 { (float(int(q) & 255) / 255.0) * (2.0 * PI) - PI }

fn _gltf_anim_pack_uv_offset16(f64 v) int {
   def n = clamp01((float(v) + 8.0) / 16.0)
   int(n * 65535.0 + 0.5) & 0xffff
}

fn _gltf_anim_pack_uv_scale11(f64 v) int {
   if(abs(float(v) - 1.0) <= 0.000001){ return 0 }
   mut n = (float(v) + 32.0) / 64.0
   if(n < 0.0){ n = 0.0 }
   if(n > 1.0){ n = 1.0 }
   int(n * 2047.0 + 0.5) & 2047
}

fn _gltf_anim_pack_uv_rot8(f64 v) int {
   def n = clamp01((float(v) + PI) / (2.0 * PI))
   int(n * 255.0 + 0.5) & 255
}

fn _gltf_anim_unpack_uv_xf(int word0, int word1) dict {
   return {
      "offset": [_gltf_anim_decode_uv_offset16(word0), _gltf_anim_decode_uv_offset16(int(word0) >> 16)],
      "scale": [_gltf_anim_decode_uv_scale11(word1), _gltf_anim_decode_uv_scale11(int(word1) >> 11)],
      "rotation": _gltf_anim_decode_uv_rot8(int(word1) >> 22),
      "uv_set": (int(word1) >> 30) & 1
   }
}

fn _gltf_anim_pack_uv_xf_state(dict st) list {
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

fn _gltf_anim_uv_slot_group(str slot) str { comptime match GltfAnimUvSlotGroup(slot, "") }

fn gltf_anim_apply_uv_pointer_override(any out, any mesh, any slab, str slot, str kind, any val) list {
   "Applies KHR_animation_pointer UV transform overrides to a CPU material slab."
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

fn gltf_anim_apply_material_pointer_overrides(any part, any ptr_overrides) any {
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

fn gltf_sync_drawable_part_from_raw(any part, any raw_part, bool update_part_tex=true, bool update_part_material=true) any {
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
   if(raw_part.contains("model")){
      ;; Keep raw glTF node transforms in sync too.  The previous helper only
      ;; refreshed vertex/index buffers, so any caller that rebuilt raw glTF
      ;; parts for an animated pose could keep drawing the old part model.
      ;; Keep raw glTF node transforms in sync for animated/rebuilt parts.
      part["model"] = raw_part.get("model", part.get("model", 0))
   }
   if(raw_part.contains("node_idx")){ part["node_idx"] = int(raw_part.get("node_idx", part.get("node_idx", -1))) }
   if(raw_part.contains("visible")){ part["visible"] = raw_part.get("visible", part.get("visible", true)) ? true : false }
   if(update_part_material && raw_part.contains("material_slab")){ part["material_slab"] = raw_part.get("material_slab", 0) }
   part
}

fn gltf_sync_drawable_parts_from_raw(any existing_parts, any raw_parts, bool update_part_tex=true, bool update_part_material=true) any {
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

fn gltf_expand_indexed_vertices(?ptr vptr, int vcnt, ?ptr iptr, int icnt, bool idx_u32=false) ?ptr {
   "Expands indexed packed glTF vertices into a linear CPU vertex buffer."
   if(!vptr || vcnt <= 0 || !iptr || icnt <= 0){ return 0 }
   mut ?ptr: out = malloc(icnt * render_shared.VERTEX_STRIDE)
   if(!out){ return 0 }
   memset(out, 0, render_shared.VERTEX_STRIDE)
   def trace_expand = common.env_truthy("NY_RENDER_INDEX_TRACE") || common.env_truthy("NY_GLTF_INDEX_TRACE") || common.env_truthy("NY_VK_CAPTURE_TRACE")
   def idx_step = idx_u32 ? 4 : 2
   def idx_bytes_n = icnt * idx_step
   def idx_bytes = bytes(idx_bytes_n)
   if(!idx_bytes){
      free(out)
      return 0
   }
   __copy_mem(idx_bytes, iptr, idx_bytes_n)
   if(trace_expand){
      ui_profile.print_text("[render:expand:bytes] len=" + to_str(len(idx_bytes)) +
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
         ui_profile.print_text("[render:expand] i=" + to_str(i) +
            " vi=" + to_str(vi) +
            " idx_off=" + to_str(idx_off) +
         " vcnt=" + to_str(vcnt))
      }
      def dst_ptr = out + dst_off
      if(vi >= 0 && vi < vcnt){ __copy_mem(dst_ptr, vptr + vi * render_shared.VERTEX_STRIDE, render_shared.VERTEX_STRIDE) } else { __copy_mem(dst_ptr, out, render_shared.VERTEX_STRIDE) }
      idx_off += idx_step
      dst_off += render_shared.VERTEX_STRIDE
      i += 1
   }
   out
}

fn gltf_rewind_triangle_vertices(?ptr vptr, int vcnt) ?ptr {
   "Copies a linear triangle-list vertex buffer with every triangle winding reversed."
   if(!vptr || vcnt <= 0){ return 0 }
   mut ?ptr: out = malloc(vcnt * render_shared.VERTEX_STRIDE)
   if(!out){ return 0 }
   mut i = 0
   while(i + 2 < vcnt){
      __copy_mem(out + (i + 0) * render_shared.VERTEX_STRIDE, vptr + (i + 0) * render_shared.VERTEX_STRIDE, render_shared.VERTEX_STRIDE)
      __copy_mem(out + (i + 1) * render_shared.VERTEX_STRIDE, vptr + (i + 2) * render_shared.VERTEX_STRIDE, render_shared.VERTEX_STRIDE)
      __copy_mem(out + (i + 2) * render_shared.VERTEX_STRIDE, vptr + (i + 1) * render_shared.VERTEX_STRIDE, render_shared.VERTEX_STRIDE)
      i += 3
   }
   while(i < vcnt){
      __copy_mem(out + i * render_shared.VERTEX_STRIDE, vptr + i * render_shared.VERTEX_STRIDE, render_shared.VERTEX_STRIDE)
      i += 1
   }
   out
}

fn pack_material_slab(any part) any {
   "Packs a material record into the shared native material slab layout."
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
