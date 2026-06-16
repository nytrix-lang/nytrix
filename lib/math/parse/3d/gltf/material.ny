;; Keywords: 3d gltf glb parse
;; Submodule: material
module std.math.parse.3d.gltf.material(_gltf_extract_embedded_image, _gltf_texture_uv_props, _gltf_scene_bounds_new, _gltf_scene_bounds_accum, _gltf_scene_bounds_result, _gltf_indexed_material_slots, _gltf_part_world_bounds, _gltf_scene_bounds_accum_part, _gltf_image_name_lower, _gltf_name_has_any, _gltf_name_is_bad_for_slot, _gltf_name_good_for_slot, _gltf_find_image_uri_by_slot, _gltf_resolve_image_uri, _gltf_texture_filter_from_sampler, _gltf_texture_sampler_info, _gltf_decode_mr_tex_id, _gltf_tex_uri_uv, _gltf_tex_info, _gltf_ext_dict, _gltf_ext_float, _gltf_ext_vec3, _gltf_ext_tex_uv, _gltf_ext_tex_info, _gltf_material_apply_extensions, gltf_material_info, _gltf_u8, _gltf_u8_round, _gltf_pack_u8x4, _gltf_pack_vec3_lane, _gltf_pack_bsdf0, _gltf_pack_bsdf1, _gltf_vec3_needs_factor, _gltf_pack_bsdf2, _gltf_pack_bsdf3, _gltf_pack_bsdf4, _gltf_pack_bsdf5, _gltf_keep_bsdf_record_value, _gltf_keep_ext2_record_value, _gltf_pack_alpha_ext_flags, _gltf_pack_emissive_word, gltf_material_infos, gltf_material_infos_limited, _gltf_indexed_material_fast_record, _gltf_indexed_base_color_state, _gltf_indexed_mr_state, _gltf_indexed_uv_xforms, _gltf_indexed_material_info_record, _gltf_indexed_material_records, _gltf_indexed_copy_i32_fields, _gltf_keep_indexed_bsdf_fields, _gltf_indexed_part_material_defaults, _gltf_indexed_part_material_apply_record, _gltf_indexed_part_material_apply_texrec, _gltf_indexed_part_force_uv_xform, _gltf_indexed_part_material_force_uv0, _gltf_indexed_prim_material_state, _gltf_indexed_mesh_opts, _gltf_indexed_default_color, _gltf_indexed_material_record_setup, _gltf_has_uri, _gltf_material_uri_feature_mask, _gltf_material_gt_feature_mask, _gltf_material_ne_feature_mask, _gltf_material_bool_feature_mask, gltf_material_feature_mask)
use std.core
use std.math.bin
use std.math
use std.os (file_exists, file_read, file_write)
use std.os.path as ospath
use std.math.float (is_nan, is_inf)
use std.core.str as str
use std.core.common as common
use std.core.cache as cache
use std.math.crypto.hash as lib_hash
use std.math.parse.3d.gltf.math as gltf_math
use std.math.parse.3d.gltf.shared as shr
use std.math.parse.3d.gltf.load as ld

fn _gltf_extract_embedded_image(any gltf_data, int img_idx) str {
   shr._gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return "" }
   def images = g.get("images")
   if !is_list(images) || img_idx < 0 || img_idx >= images.len { return "" }
   def img = images[img_idx]
   def base_path = to_str(gltf_data.get("base_path", ""))
   def source_path = to_str(gltf_data.get("source_path", ""))
   def cache_root = (source_path.len > 0) ? source_path : base_path
   def cache_key = cache_root + "|" + to_str(img_idx)
   if _gltf_img_uri_cache.contains(cache_key) { return to_str(_gltf_img_uri_cache.get(cache_key, "")) }
   mut out_path = ""
   mut uri = to_str(img.get("uri", ""))
   if uri.len > 0 {
      if str.find(uri, "data:") == 0 {
         def decoded = ld._gltf_data_uri_decode(uri)
         if !is_str(decoded) || decoded.len <= 0 { return "" }
         mut mime = to_str(img.get("mimeType", ""))
         if mime.len == 0 {
            def semi = str.find_from(uri, ";", 5)
            def comma = str.find_from(uri, ",", 5)
            if semi > 5 { mime = shr._gltf_copy_bytes(uri, 5, semi - 5) }
            elif comma > 5 { mime = shr._gltf_copy_bytes(uri, 5, comma - 5) }
         }
         out_path = ospath.join(ospath.cache_dir(), "ny_gltf_img_" + to_str(lib_hash.xxh32(base_path + "|" + to_str(img_idx) + "|" + mime + "|data")) + shr._gltf_image_ext_from_mime(mime))
         if !file_exists(out_path) { _ = file_write(out_path, decoded) }
      } else {
         out_path = ospath.join(base_path, shr._gltf_url_decode(uri))
      }
      _gltf_img_uri_cache = cache.cache_put_reset(shr._gltf_img_uri_cache, cache_key, out_path, shr._GLTF_CACHE_LIMIT_MED, 128)
      return out_path
   }
   def bv_list = g.get("bufferViews")
   def bv_idx = int(img.get("bufferView", -1))
   if !is_list(bv_list) || bv_idx < 0 || bv_idx >= bv_list.len { return "" }
   def bv = bv_list[bv_idx]
   def off = int(bv.get("byteOffset", 0))
   def blen = int(bv.get("byteLength", 0))
   if blen <= 0 { return "" }
   def buffer_data = gltf_data.get("buffer_data", 0)
   def buf_idx = int(bv.get("buffer", 0))
   mut bin_blob = 0
   if is_list(buffer_data) && buf_idx >= 0 && buf_idx < buffer_data.len {
      def b = buffer_data[buf_idx]
      bin_blob = b
   }
   if !bin_blob {
      def binary_data = gltf_data.get("binary_data", 0)
      bin_blob = binary_data
   }
   if !bin_blob { return "" }
   def bytes = shr._gltf_copy_blob_bytes(bin_blob, off, blen)
   if !is_str(bytes) || bytes.len <= 0 { return "" }
   def mime = to_str(img.get("mimeType", "application/octet-stream"))
   out_path = ospath.join(ospath.cache_dir(), "ny_gltf_img_" + to_str(lib_hash.xxh32(cache_root + "|" + to_str(img_idx) + "|" + to_str(off) + "|" + to_str(blen) + "|" + mime)) + shr._gltf_image_ext_from_mime(mime))
   if !file_exists(out_path) { _ = file_write(out_path, bytes) }
   _gltf_img_uri_cache = cache.cache_put_reset(shr._gltf_img_uri_cache, cache_key, out_path, shr._GLTF_CACHE_LIMIT_MED, 128)
   out_path
}

fn _gltf_texture_uv_props(dict out, str prefix, any tex_info) dict {
   mut o = out
   mut texcoord = is_dict(tex_info) ? int(tex_info.get("texCoord", 0)) : 0
   mut uv_offset = [0.0, 0.0]
   mut uv_scale = [1.0, 1.0]
   mut uv_rotation = 0.0
   def exts = is_dict(tex_info) ? tex_info.get("extensions", 0) : 0
   def xform = is_dict(exts) ? exts.get("KHR_texture_transform", 0) : 0
   if is_dict(xform) {
      def off = xform.get("offset", uv_offset)
      def scl = xform.get("scale", uv_scale)
      uv_offset = [
         shr._gltf_num_or(is_list(off) ? off.get(0, 0.0) : 0.0, 0.0),
         shr._gltf_num_or(is_list(off) ? off.get(1, 0.0) : 0.0, 0.0)
      ]
      uv_scale = [
         shr._gltf_num_or(is_list(scl) ? scl.get(0, 1.0) : 1.0, 1.0),
         shr._gltf_num_or(is_list(scl) ? scl.get(1, 1.0) : 1.0, 1.0)
      ]
      uv_rotation = shr._gltf_num_or(xform.get("rotation", 0.0), 0.0)
      texcoord = int(xform.get("texCoord", texcoord))
   }
   def uv_words = shr._gltf_pack_uv_xform_words_from_values(uv_offset, uv_scale, uv_rotation, texcoord)
   o[prefix + "_texcoord"] = texcoord
   o[prefix + "_uv_xf0"] = int(uv_words.get(0, 0))
   o[prefix + "_uv_xf1"] = int(uv_words.get(1, 0))
   if is_dict(xform) {
      o[prefix + "_uv_offset"] = uv_offset
      o[prefix + "_uv_scale"] = uv_scale
      o[prefix + "_uv_rotation"] = uv_rotation
   }
   o
}

fn _gltf_scene_bounds_new() list { [1e9, 1e9, 1e9, -1e9, -1e9, -1e9] }

fn _gltf_scene_bounds_accum(
   list state, f64 wmin_x, f64 wmin_y, f64 wmin_z, f64 wmax_x, f64 wmax_y, f64 wmax_z
) list {
   if state.len < 6 { state = _gltf_scene_bounds_new() }
   if shr._gltf_float_bad(wmin_x) || shr._gltf_float_bad(wmin_y) || shr._gltf_float_bad(wmin_z) ||
   shr._gltf_float_bad(wmax_x) || shr._gltf_float_bad(wmax_y) || shr._gltf_float_bad(wmax_z){
      return state
   }
   wmin_x, wmin_y = 0.0 + wmin_x, 0.0 + wmin_y
   wmin_z = 0.0 + wmin_z
   wmax_x = 0.0 + wmax_x
   wmax_y = 0.0 + wmax_y
   wmax_z = 0.0 + wmax_z
   if wmin_x > wmax_x || wmin_y > wmax_y || wmin_z > wmax_z { return state }
   if wmin_x < float(state.get(0, 1e9)) { state[0] = wmin_x }
   if wmin_y < float(state.get(1, 1e9)) { state[1] = wmin_y }
   if wmin_z < float(state.get(2, 1e9)) { state[2] = wmin_z }
   if wmax_x > float(state.get(3, -1e9)) { state[3] = wmax_x }
   if wmax_y > float(state.get(4, -1e9)) { state[4] = wmax_y }
   if wmax_z > float(state.get(5, -1e9)) { state[5] = wmax_z }
   state
}

fn _gltf_scene_bounds_result(list state) list {
   mut pmin = [float(state.get(0, 1e9)), float(state.get(1, 1e9)), float(state.get(2, 1e9))]
   mut pmax = [float(state.get(3, -1e9)), float(state.get(4, -1e9)), float(state.get(5, -1e9))]
   if pmin.get(0, 1e9) > 1e8 { pmin = [-1.0, -1.0, -1.0] }
   if pmax.get(0, -1e9) < -1e8 { pmax = [1.0, 1.0, 1.0] }
   [pmin, pmax]
}

fn _gltf_indexed_material_slots(
   dict material_state, int tex_id, int normal_tex_id, int normal_uv_set,
   int emissive_tex_id, int emissive_uv_set, int occlusion_tex_id, int occlusion_uv_set,
   int material_u32, int uv_set, int met_rough_uv_set
) dict {
   def base_slot = shr._gltf_make_material_slot(tex_id, uv_set, int(material_state.get("base_uv_xf0", 0)), int(material_state.get("base_uv_xf1", 0)))
   {
      "base_color": base_slot,
      "base": base_slot,
      "normal": shr._gltf_make_material_slot(normal_tex_id, normal_uv_set, int(material_state.get("normal_uv_xf0", 0)), int(material_state.get("normal_uv_xf1", 0))),
      "metallic_roughness": shr._gltf_make_material_slot(_gltf_decode_mr_tex_id(band(bshr(material_u32, 16), 0x7fff)), met_rough_uv_set, int(material_state.get("mr_uv_xf0", 0)), int(material_state.get("mr_uv_xf1", 0))),
      "occlusion": shr._gltf_make_material_slot(occlusion_tex_id, occlusion_uv_set, int(material_state.get("occlusion_uv_xf0", 0)), int(material_state.get("occlusion_uv_xf1", 0))),
      "emissive": shr._gltf_make_material_slot(emissive_tex_id, emissive_uv_set, int(material_state.get("emissive_uv_xf0", 0)), int(material_state.get("emissive_uv_xf1", 0)))
   }
}

fn _gltf_part_world_bounds(any lbs, any model) list {
   mut wmin_x, wmin_y, wmin_z = 0.0, 0.0, 0.0
   mut wmax_x, wmax_y, wmax_z = 0.0, 0.0, 0.0
   if is_list(lbs) && is_list(model) && model.len >= 16 {
      def wb = shr._gltf_transform_aabb(lbs.get(0), lbs.get(1), model)
      if is_list(wb) && wb.len >= 2 {
         def wmin = wb.get(0)
         def wmax = wb.get(1)
         wmin_x, wmin_y = float(wmin.get(0, 0.0)), float(wmin.get(1, 0.0))
         wmin_z = float(wmin.get(2, 0.0))
         wmax_x = float(wmax.get(0, 0.0))
         wmax_y = float(wmax.get(1, 0.0))
         wmax_z = float(wmax.get(2, 0.0))
      }
   }
   [wmin_x, wmin_y, wmin_z, wmax_x, wmax_y, wmax_z]
}

fn _gltf_scene_bounds_accum_part(list state, list world_bounds) list {
   _gltf_scene_bounds_accum(state,
      float(world_bounds.get(0, 0.0)),
      float(world_bounds.get(1, 0.0)),
      float(world_bounds.get(2, 0.0)),
      float(world_bounds.get(3, 0.0)),
      float(world_bounds.get(4, 0.0)),
   float(world_bounds.get(5, 0.0)))
}

fn _gltf_image_name_lower(any img, str resolved="") str {
   mut name = str.lower(to_str(img.get("name", "")))
   def uri = str.lower(to_str(img.get("uri", "")))
   if uri.len > 0 { name = name + " " + uri }
   if resolved.len > 0 { name = name + " " + str.lower(ospath.basename(resolved)) }
   name
}

fn _gltf_name_has_any(str s, list needles) bool {
   mut i = 0
   while i < needles.len {
      if str.find(s, to_str(needles.get(i, ""))) >= 0 { return true }
      i += 1
   }
   false
}

fn _gltf_name_is_bad_for_slot(str slot, str name) bool {
   if slot == "base_color" || slot == "diffuse" {
      return _gltf_name_has_any(name, ["normal", "nrm", "rough", "metal", "orm", "occlusion", "ao"])
   }
   if slot == "normal" { return _gltf_name_has_any(name, ["base", "color", "albedo", "diffuse", "rough", "metal", "orm", "occlusion", "ao"]) }
   if slot == "metallic_roughness" { return _gltf_name_has_any(name, ["base", "color", "albedo", "diffuse", "normal", "nrm"]) }
   if slot == "occlusion" { return _gltf_name_has_any(name, ["base", "color", "albedo", "diffuse", "normal", "nrm"]) }
   false
}

fn _gltf_name_good_for_slot(str slot, str name) bool {
   if slot == "base_color" || slot == "diffuse" {
      return _gltf_name_has_any(name, ["basecolor", "base_color", "base color", "albedo", "diffuse", "color"]) &&
      !_gltf_name_has_any(name, ["normal", "nrm", "rough", "metal", "orm", "occlusion", "ao"])
   }
   if slot == "normal" { return _gltf_name_has_any(name, ["normal", "nrm"]) }
   if slot == "metallic_roughness" { return _gltf_name_has_any(name, ["metallicroughness", "metallic_roughness", "roughnessmetallic", "roughness_metallic", "rough", "metal", "orm"]) }
   if slot == "occlusion" { return _gltf_name_has_any(name, ["occlusion", "ao", "orm"]) }
   false
}

fn _gltf_find_image_uri_by_slot(any gltf_data, str slot) str {
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return "" }
   def images = g.get("images")
   if !is_list(images) { return "" }
   mut i = 0
   while i < images.len {
      def img = images.get(i, 0)
      if is_dict(img) {
         def resolved = _gltf_extract_embedded_image(gltf_data, i)
         def name = _gltf_image_name_lower(img, resolved)
         if _gltf_name_good_for_slot(slot, name) { return resolved }
      }
      i += 1
   }
   ""
}

fn _gltf_resolve_image_uri(any gltf_data, any tex_info, str slot="") str {
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) || !is_dict(tex_info) { return "" }
   def textures = g.get("textures")
   def images = g.get("images")
   if !is_list(textures) || !is_list(images) { return "" }
   def tex_idx = int(tex_info.get("index", -1))
   if tex_idx < 0 || tex_idx >= textures.len { return "" }
   def tex = textures.get(tex_idx)
   if !is_dict(tex) { return "" }
   mut src_idx = int(tex.get("source", -1))
   ;; Prefer the core glTF source when present. Compressed extension sources are
   ;; alternatives and may be unsupported or point at ORM/normal payloads in some
   ;; converted samples. Only use extension source when the core source is absent.
   if src_idx < 0 && is_dict(tex.get("extensions", 0)) {
      def exts = tex.get("extensions", 0)
      def webp = exts.get("EXT_texture_webp", 0)
      if is_dict(webp) { src_idx = int(webp.get("source", -1)) }
      if src_idx < 0 {
         def basisu = exts.get("KHR_texture_basisu", 0)
         if is_dict(basisu) { src_idx = int(basisu.get("source", -1)) }
      }
   }
   if src_idx < 0 || src_idx >= images.len { return "" }
   def resolved = _gltf_extract_embedded_image(gltf_data, src_idx)
   ;; Safety net for material slots: prefer a slot-named image whenever the
   ;; chosen source is clearly wrong OR too generic to prove it belongs to this
   ;; slot.  Some exporters/converted samples keep texture.source indexes but
   ;; expose image names only through the image list; in that case a baseColor
   ;; material can otherwise land on an ORM/roughness-looking grayscale map in VK.
   if slot.len > 0 && resolved.len > 0 {
      def img = images.get(src_idx, 0)
      def name = is_dict(img) ? _gltf_image_name_lower(img, resolved) : str.lower(ospath.basename(resolved))
      def alt = _gltf_find_image_uri_by_slot(gltf_data, slot)
      if alt.len > 0 && (_gltf_name_is_bad_for_slot(slot, name) || !_gltf_name_good_for_slot(slot, name)) {
         return alt
      }
   }
   resolved
}

fn _gltf_texture_filter_from_sampler(any s) int {
   if !is_dict(s) { return -1 }
   if s.get("mag_linear", false) || s.get("min_linear", false) || s.get("min_uses_mips", false) { return 1 }
   if s.get("mag_nearest", false) || s.get("min_nearest", false) { return 0 }
   -1
}

fn _gltf_texture_sampler_info(any gltf_data, any tex_info) any {
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) || !is_dict(tex_info) { return 0 }
   def textures = g.get("textures", [])
   def samplers = g.get("samplers", [])
   def tex_idx = int(tex_info.get("index", -1))
   if !is_list(textures) || tex_idx < 0 || tex_idx >= textures.len { return 0 }
   def tex = textures.get(tex_idx)
   if !is_dict(tex) { return 0 }
   def sampler_idx = int(tex.get("sampler", -1))
   mut mag = -1
   mut minf = -1
   mut wrap_s = 10497
   mut wrap_t = 10497
   if is_list(samplers) && sampler_idx >= 0 && sampler_idx < samplers.len {
      def sampler = samplers.get(sampler_idx)
      if is_dict(sampler) {
         mag = int(sampler.get("magFilter", -1))
         minf = int(sampler.get("minFilter", -1))
         wrap_s = int(sampler.get("wrapS", 10497))
         wrap_t = int(sampler.get("wrapT", 10497))
      }
   }
   {
      "mag_filter": mag, "min_filter": minf, "wrap_s": wrap_s, "wrap_t": wrap_t,
      "mag_linear": mag == 9729, "mag_nearest": mag == 9728,
      "min_uses_mips": (minf == 9984 || minf == 9985 || minf == 9986 || minf == 9987),
      "min_linear": (minf == 9729 || minf == 9985 || minf == 9987),
      "min_nearest": (minf == 9728 || minf == 9984 || minf == 9986)
   }
}

fn _pack_material_word(any metallic_u8, any rough_u8, any mr_word) int {
   bor(
      bor(band(int(metallic_u8), 255), bshl(band(int(rough_u8), 255), 8)),
      bshl(band(int(mr_word), 0xffff), 16)
   )
}

fn _gltf_decode_mr_tex_id(int mr_word) int {
   def enc = band(mr_word, 0x7fff)
   enc > 0 ? enc - 1 : -1
}

fn _gltf_tex_uri_uv(dict out, str prefix, any gltf_data, any tex_info) dict {
   ;; glTF TextureInfo is an object.  Do not treat a missing texture as index 0:
   ;; that binds image 0 into base/normal/MR/occlusion slots and makes VK sample
   ;; the wrong material texture (classic gray/striped Avocado failure).
   if !is_dict(tex_info) { return out }
   def sampler = _gltf_texture_sampler_info(gltf_data, tex_info)
   out[prefix + "_uri"] = _gltf_resolve_image_uri(gltf_data, tex_info, prefix)
   out[prefix + "_filter"] = _gltf_texture_filter_from_sampler(sampler)
   out[prefix + "_sampler"] = sampler
   _gltf_texture_uv_props(out, prefix, tex_info)
}

fn _gltf_tex_info(dict out, str prefix, any gltf_data, any tex_info) dict {
   ;; Missing textureInfo must remain missing.  Earlier code accepted the default
   ;; 0 and resolved texture/image 0, so absent normal/MR/occlusion/emissive maps
   ;; could alias the base image and break material binding in Vulkan.
   if !is_dict(tex_info) { return out }
   def sampler = _gltf_texture_sampler_info(gltf_data, tex_info)
   out[prefix + "_uri"] = _gltf_resolve_image_uri(gltf_data, tex_info, prefix)
   out[prefix + "_filter"] = _gltf_texture_filter_from_sampler(sampler)
   out[prefix + "_sampler"] = sampler
   out = _gltf_texture_uv_props(out, prefix, tex_info)
   out
}

fn _gltf_ext_dict(any ext, str key) any {
   if !is_dict(ext) { return 0 }
   def v = ext.get(key, 0)
   is_dict(v) ? v : 0
}

fn _gltf_ext_float(dict out, any ext, str out_key, str src_key, f64 default) dict {
   out[out_key] = float(ext.get(src_key, default))
   out
}

fn _gltf_ext_vec3(dict out, any ext, str out_key, str src_key, f64 x, f64 y, f64 z) dict {
   out[out_key] = shr._gltf_vec3(ext.get(src_key, [x, y, z]), x, y, z)
   out
}

fn _gltf_ext_tex_uv(dict out, any ext, any gltf_data, str prefix, str tex_key) dict {
   _gltf_tex_uri_uv(out, prefix, gltf_data, ext.get(tex_key, 0))
}

fn _gltf_ext_tex_info(dict out, any ext, any gltf_data, str prefix, str tex_key) dict {
   _gltf_tex_info(out, prefix, gltf_data, ext.get(tex_key, 0))
}

fn _gltf_material_apply_extensions(dict out, any ext, any ext_sg, bool use_spec_gloss, any gltf_data) dict {
   if !is_dict(ext) { return out }
   if use_spec_gloss && is_dict(ext_sg) {
      out = _gltf_ext_vec3(out, ext_sg, "specular_color_factor", "specularFactor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_info(out, ext_sg, gltf_data, "specular_color", "specularGlossinessTexture")
   }
   if ext.contains("KHR_materials_unlit") { out["unlit"] = true }
   def ext_es = _gltf_ext_dict(ext, "KHR_materials_emissive_strength")
   if is_dict(ext_es) { out = _gltf_ext_float(out, ext_es, "emissive_strength", "emissiveStrength", 1.0) }
   def ext_cc = _gltf_ext_dict(ext, "KHR_materials_clearcoat")
   if is_dict(ext_cc) {
      out = _gltf_ext_float(out, ext_cc, "clearcoat_factor", "clearcoatFactor", 0.0)
      out = _gltf_ext_float(out, ext_cc, "clearcoat_roughness_factor", "clearcoatRoughnessFactor", 0.0)
      out["clearcoat_normal_scale"] = is_dict(ext_cc.get("clearcoatNormalTexture", 0)) ? float(ext_cc.get("clearcoatNormalTexture", 0).get("scale", 1.0)) : 1.0
      out = _gltf_ext_tex_uv(out, ext_cc, gltf_data, "clearcoat", "clearcoatTexture")
      out = _gltf_ext_tex_uv(out, ext_cc, gltf_data, "clearcoat_roughness", "clearcoatRoughnessTexture")
      out = _gltf_ext_tex_uv(out, ext_cc, gltf_data, "clearcoat_normal", "clearcoatNormalTexture")
   }
   def ext_an = _gltf_ext_dict(ext, "KHR_materials_anisotropy")
   if is_dict(ext_an) {
      out = _gltf_ext_float(out, ext_an, "anisotropy_strength", "anisotropyStrength", 0.0)
      out = _gltf_ext_float(out, ext_an, "anisotropy_rotation", "anisotropyRotation", 0.0)
      out = _gltf_ext_tex_uv(out, ext_an, gltf_data, "anisotropy", "anisotropyTexture")
   }
   def ext_dp = _gltf_ext_dict(ext, "KHR_materials_dispersion")
   if is_dict(ext_dp) { out = _gltf_ext_float(out, ext_dp, "dispersion", "dispersion", 0.0) }
   def ext_tr = _gltf_ext_dict(ext, "KHR_materials_transmission")
   if is_dict(ext_tr) {
      out = _gltf_ext_float(out, ext_tr, "transmission_factor", "transmissionFactor", 0.0)
      out = _gltf_ext_tex_uv(out, ext_tr, gltf_data, "transmission", "transmissionTexture")
   }
   def ext_ior = _gltf_ext_dict(ext, "KHR_materials_ior")
   if is_dict(ext_ior) { out = _gltf_ext_float(out, ext_ior, "ior", "ior", 1.5) }
   def ext_sh = _gltf_ext_dict(ext, "KHR_materials_sheen")
   if is_dict(ext_sh) {
      out = _gltf_ext_vec3(out, ext_sh, "sheen_color_factor", "sheenColorFactor", 0.0, 0.0, 0.0)
      out = _gltf_ext_float(out, ext_sh, "sheen_roughness_factor", "sheenRoughnessFactor", 0.0)
      out = _gltf_ext_tex_uv(out, ext_sh, gltf_data, "sheen_color", "sheenColorTexture")
      out = _gltf_ext_tex_uv(out, ext_sh, gltf_data, "sheen_roughness", "sheenRoughnessTexture")
   }
   def ext_ir = _gltf_ext_dict(ext, "KHR_materials_iridescence")
   if is_dict(ext_ir) {
      out = _gltf_ext_float(out, ext_ir, "iridescence_factor", "iridescenceFactor", 0.0)
      out = _gltf_ext_float(out, ext_ir, "iridescence_ior", "iridescenceIor", 1.3)
      out = _gltf_ext_float(out, ext_ir, "iridescence_thickness_min", "iridescenceThicknessMinimum", 100.0)
      out = _gltf_ext_float(out, ext_ir, "iridescence_thickness_max", "iridescenceThicknessMaximum", 400.0)
      out = _gltf_ext_tex_uv(out, ext_ir, gltf_data, "iridescence", "iridescenceTexture")
      out = _gltf_ext_tex_uv(out, ext_ir, gltf_data, "iridescence_thickness", "iridescenceThicknessTexture")
   }
   def ext_vol = _gltf_ext_dict(ext, "KHR_materials_volume")
   if is_dict(ext_vol) {
      out = _gltf_ext_float(out, ext_vol, "thickness_factor", "thicknessFactor", 0.0)
      out = _gltf_ext_float(out, ext_vol, "attenuation_distance", "attenuationDistance", 0.0)
      out = _gltf_ext_vec3(out, ext_vol, "attenuation_color", "attenuationColor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_uv(out, ext_vol, gltf_data, "thickness", "thicknessTexture")
   }
   def ext_sp = _gltf_ext_dict(ext, "KHR_materials_specular")
   if is_dict(ext_sp) {
      out = _gltf_ext_float(out, ext_sp, "specular_factor", "specularFactor", 1.0)
      out = _gltf_ext_vec3(out, ext_sp, "specular_color_factor", "specularColorFactor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_uv(out, ext_sp, gltf_data, "specular", "specularTexture")
      out = _gltf_ext_tex_uv(out, ext_sp, gltf_data, "specular_color", "specularColorTexture")
   }
   def ext_dt = _gltf_ext_dict(ext, "KHR_materials_diffuse_transmission")
   if is_dict(ext_dt) {
      out = _gltf_ext_tex_info(out, ext_dt, gltf_data, "diffuse_transmission", "diffuseTransmissionTexture")
      out = _gltf_ext_tex_info(out, ext_dt, gltf_data, "diffuse_transmission_color", "diffuseTransmissionColorTexture")
      out = _gltf_ext_float(out, ext_dt, "diffuse_transmission_factor", "diffuseTransmissionFactor", 0.0)
      out = _gltf_ext_vec3(out, ext_dt, "diffuse_transmission_color_factor", "diffuseTransmissionColorFactor", 1.0, 1.0, 1.0)
   }
   def ext_vsc = _gltf_ext_dict(ext, "KHR_materials_volume_scatter")
   if is_dict(ext_vsc) {
      out = _gltf_ext_vec3(out, ext_vsc, "volume_scatter_color_factor", "multiscatterColor", 1.0, 1.0, 1.0)
   }
   def ext_ac = _gltf_ext_dict(ext, "KHR_materials_alpha_coverage")
   if is_dict(ext_ac) { out = _gltf_ext_float(out, ext_ac, "alpha_coverage", "alphaCoverage", 1.0) }
   def ext_ref = _gltf_ext_dict(ext, "KHR_materials_refraction")
   if is_dict(ext_ref) {
      out = _gltf_ext_float(out, ext_ref, "refraction_factor", "refractionFactor", 0.0)
      out = _gltf_ext_float(out, ext_ref, "refraction_roughness", "refractionRoughnessFactor", 0.0)
   }
   def ext_sss = _gltf_ext_dict(ext, "KHR_materials_subsurface")
   if is_dict(ext_sss) {
      out = _gltf_ext_float(out, ext_sss, "subsurface_factor", "subsurfaceFactor", 0.0)
      out = _gltf_ext_vec3(out, ext_sss, "subsurface_color_factor", "subsurfaceColorFactor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_info(out, ext_sss, gltf_data, "subsurface", "subsurfaceTexture")
   }
   out
}

fn gltf_material_info(any gltf_data, int material_idx) any {
   "Returns a normalized material record with glTF PBR factors and texture URIs."
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def materials = g.get("materials")
   if !is_list(materials) || material_idx < 0 || material_idx >= materials.len { return 0 }
   def mat = materials.get(material_idx)
   def pmr = mat.get("pbrMetallicRoughness", 0)
   def ext = mat.get("extensions")
   def ext_sg = is_dict(ext) ? ext.get("KHR_materials_pbrSpecularGlossiness", 0) : 0
   def use_spec_gloss = is_dict(ext_sg)
   mut base_factor = is_dict(pmr) ? pmr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0, 1.0]
   mut metallic_factor = is_dict(pmr) ? float(pmr.get("metallicFactor", 1.0)) : 1.0
   mut roughness_factor = is_dict(pmr) ? float(pmr.get("roughnessFactor", 1.0)) : 1.0
   mut base_tex = is_dict(pmr) ? pmr.get("baseColorTexture", 0) : 0
   mut mr_tex = is_dict(pmr) ? pmr.get("metallicRoughnessTexture", 0) : 0
   if use_spec_gloss {
      base_factor = ext_sg.get("diffuseFactor", [1.0, 1.0, 1.0, 1.0])
      metallic_factor = 0.0
      roughness_factor = 1.0 - clamp01(float(ext_sg.get("glossinessFactor", 1.0)))
      base_tex = ext_sg.get("diffuseTexture", 0)
      mr_tex = ext_sg.get("specularGlossinessTexture", 0)
   }
   def emissive = mat.get("emissiveFactor", [0.0, 0.0, 0.0])
   mut out = dict(192)
   out["index"] = material_idx
   out["name"] = to_str(mat.get("name", ""))
   out["base_color_factor"] = shr._gltf_vec4(base_factor, 1.0, 1.0, 1.0, 1.0)
   out["metallic_factor"] = metallic_factor
   out["roughness_factor"] = roughness_factor
   out["specular_glossiness"] = use_spec_gloss
   out["emissive_factor"] = shr._gltf_vec3(emissive, 0.0, 0.0, 0.0)
   out["alpha_mode"] = to_str(mat.get("alphaMode", "OPAQUE"))
   out["alpha_cutoff"] = float(mat.get("alphaCutoff", 0.5))
   out["double_sided"] = mat.get("doubleSided", false) ? true : false
   def normal_tex = mat.get("normalTexture", 0)
   def occ_tex = mat.get("occlusionTexture", 0)
   def emissive_tex = mat.get("emissiveTexture", 0)
   out = _gltf_tex_info(out, "base_color", gltf_data, base_tex)
   out = _gltf_tex_info(out, "metallic_roughness", gltf_data, mr_tex)
   out = _gltf_tex_info(out, "normal", gltf_data, normal_tex)
   out = _gltf_tex_info(out, "occlusion", gltf_data, occ_tex)
   out = _gltf_tex_info(out, "emissive", gltf_data, emissive_tex)
   out["normal_scale"] = is_dict(normal_tex) ? float(normal_tex.get("scale", 1.0)) : 1.0
   out["occlusion_strength"] = is_dict(occ_tex) ? clamp01(float(occ_tex.get("strength", 1.0))) : 1.0
   _gltf_material_apply_extensions(out, ext, ext_sg, use_spec_gloss, gltf_data)
}

fn _gltf_u8(any v) int {
   band(int(clamp01(float(v)) * 255.0), 255)
}

fn _gltf_u8_round(any v) int {
   band(int(clamp01(float(v)) * 255.0 + 0.5), 255)
}

fn _gltf_pack_u8x4(any a, any b, any c, any d) int {
   bor(bor(band(int(a), 255), bshl(band(int(b), 255), 8)),
   bor(bshl(band(int(c), 255), 16), bshl(band(int(d), 255), 24)))
}

fn _gltf_pack_vec3_lane(any c, f64 default, int lane3) int {
   _gltf_pack_u8x4(
      _gltf_u8(c.get(0, default)),
      _gltf_u8(c.get(1, default)),
      _gltf_u8(c.get(2, default)),
      lane3
   )
}

fn _gltf_pack_bsdf0(any info) int {
   def spec = _gltf_u8(is_dict(info) ? info.get("specular_factor", 1.0) : 1.0)
   def sheen_r = _gltf_u8(is_dict(info) ? info.get("sheen_roughness_factor", 0.0) : 0.0)
   def _trans_v = is_dict(info) ? float(info.get("transmission_factor", 0.0)) : 0.0
   def trans = _gltf_u8(_trans_v)
   def iri = _gltf_u8(is_dict(info) ? info.get("iridescence_factor", 0.0) : 0.0)
   _gltf_pack_u8x4(spec, sheen_r, trans, iri)
}

fn _gltf_pack_bsdf1(any info) int {
   def c = is_dict(info) ? info.get("specular_color_factor", [1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0]
   def ior_v = is_dict(info) ? float(info.get("ior", 1.5)) : 1.5
   _gltf_pack_vec3_lane(c, 1.0, _gltf_u8((ior_v - 1.0) / 1.5))
}

fn _gltf_vec3_needs_factor(any c, f64 default_v) bool {
   if !is_list(c) { return false }
   abs(float(c.get(0, default_v)) - default_v) > 0.003 ||
   abs(float(c.get(1, default_v)) - default_v) > 0.003 ||
   abs(float(c.get(2, default_v)) - default_v) > 0.003
}

fn _gltf_pack_bsdf2(any info) int {
   mut c = is_dict(info) ? info.get("sheen_color_factor", [0.0, 0.0, 0.0]) : [0.0, 0.0, 0.0]
   if is_dict(info) && float(info.get("diffuse_transmission_factor", 0.0)) > 0.0 {
      def dtc = info.get("diffuse_transmission_color_factor", [1.0, 1.0, 1.0])
      if _gltf_vec3_needs_factor(dtc, 1.0) { c = dtc }
   }
   mut thickness_v = is_dict(info) ? float(info.get("thickness_factor", 0.0)) : 0.0
   if thickness_v < 0.0 { thickness_v = 0.0 } elif thickness_v > 4.0 { thickness_v = 4.0 }
   mut thickness = _gltf_u8_round(thickness_v / 4.0)
   if thickness_v > 0.0 && thickness == 0 { thickness = 1 }
   _gltf_pack_vec3_lane(c, 0.0, thickness)
}

fn _gltf_pack_bsdf3(any info) int {
   def c = is_dict(info) ? info.get("attenuation_color", [1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0]
   def att_dist = is_dict(info) ? float(info.get("attenuation_distance", 0.0)) : 0.0
   mut att_r, att_g = clamp01(float(c.get(0, 1.0))), clamp01(float(c.get(1, 1.0)))
   mut att_b = clamp01(float(c.get(2, 1.0)))
   def use_iri_pack =
   is_dict(info) &&
   float(info.get("iridescence_factor", 0.0)) > 0.0 &&
   att_dist <= 0.0 &&
   abs(att_r - 1.0) <= 0.000001 &&
   abs(att_g - 1.0) <= 0.000001 &&
   abs(att_b - 1.0) <= 0.000001
   if use_iri_pack {
      mut iri_ior = float(info.get("iridescence_ior", 1.3))
      mut iri_min = float(info.get("iridescence_thickness_min", 100.0))
      mut iri_max = float(info.get("iridescence_thickness_max", 400.0))
      if iri_ior < 1.0 { iri_ior = 1.0 } elif iri_ior > 3.0 { iri_ior = 3.0 }
      if iri_min < 0.0 { iri_min = 0.0 } elif iri_min > 800.0 { iri_min = 800.0 }
      if iri_max < 0.0 { iri_max = 0.0 } elif iri_max > 800.0 { iri_max = 800.0 }
      def iri_ior_u8 = _gltf_u8_round((iri_ior - 1.0) / 2.0)
      def iri_min_u8 = _gltf_u8_round(iri_min / 800.0)
      def iri_max_u8 = _gltf_u8_round(iri_max / 800.0)
      return _gltf_pack_u8x4(iri_ior_u8, iri_min_u8, iri_max_u8, 254)
   }
   mut att_u8 = 255
   if att_dist > 0.0 {
      att_u8 = band(int(sqrt(clamp01(att_dist / 10.0)) * 253.0 + 0.5), 255)
      if att_u8 < 1 { att_u8 = 1 }
      if att_u8 > 253 { att_u8 = 253 }
   }
   _gltf_pack_u8x4(_gltf_u8(att_r), _gltf_u8(att_g), _gltf_u8(att_b), att_u8)
}

fn _gltf_pack_bsdf4(any info) int {
   def clearcoat = _gltf_u8(is_dict(info) ? info.get("clearcoat_factor", 0.0) : 0.0)
   def clearcoat_rough = _gltf_u8(is_dict(info) ? info.get("clearcoat_roughness_factor", 0.0) : 0.0)
   def anisotropy = _gltf_u8(is_dict(info) ? info.get("anisotropy_strength", 0.0) : 0.0)
   def dispersion = _gltf_u8(is_dict(info) ? (float(info.get("dispersion", 0.0)) / 10.0) : 0.0)
   _gltf_pack_u8x4(clearcoat, clearcoat_rough, anisotropy, dispersion)
}

fn _gltf_pack_bsdf5(any info) int {
   def diffuse_trans = _gltf_u8(is_dict(info) ? info.get("diffuse_transmission_factor", 0.0) : 0.0)
   def refraction = _gltf_u8(is_dict(info) ? info.get("refraction_factor", 0.0) : 0.0)
   def subsurface = _gltf_u8(is_dict(info) ? info.get("subsurface_factor", 0.0) : 0.0)
   def alpha_coverage = _gltf_u8(is_dict(info) ? info.get("alpha_coverage", 1.0) : 1.0)
   _gltf_pack_u8x4(diffuse_trans, refraction, subsurface, alpha_coverage)
}

fn _gltf_keep_bsdf_record_value(int record_word, int material_word) int {
   record_word != 0 ? record_word : material_word
}

fn _gltf_keep_ext2_record_value(int record_word, int material_word) int { record_word != 0x80000000 ? record_word : material_word }

fn _gltf_pack_alpha_ext_flags(any info) int {
   if !is_dict(info) { return 0 }
   def dt = float(info.get("diffuse_transmission_factor", 0.0))
   def dtc = info.get("diffuse_transmission_color_factor", [1.0, 1.0, 1.0])
   mut flags = 0
   if dt > 0.0 && _gltf_vec3_needs_factor(dtc, 1.0) { flags = bor(flags, 0x80000000) }
   flags
}

fn _gltf_pack_emissive_word(any info) int {
   mut ef = [0.0, 0.0, 0.0]
   mut strength = 1.0
   if is_dict(info) {
      ef = info.get("emissive_factor", ef)
      strength = float(info.get("emissive_strength", 1.0))
   }
   if strength < 0.0 { strength = 0.0 }
   mut r, g = float(ef.get(0, 0.0)) * strength, float(ef.get(1, 0.0)) * strength
   mut b = float(ef.get(2, 0.0)) * strength
   if r < 0.0 { r = 0.0 }
   if g < 0.0 { g = 0.0 }
   if b < 0.0 { b = 0.0 }
   mut peak = max(r, max(g, b))
   if peak <= 0.000001 { return 0 }
   if peak > 64.0 { peak = 64.0 }
   def scale_u8 = band(int((peak / 64.0) * 255.0 + 0.5), 255)
   def rn = band(int((clamp01(r / peak) * 255.0 + 0.5)), 255)
   def gn = band(int((clamp01(g / peak) * 255.0 + 0.5)), 255)
   def bn = band(int((clamp01(b / peak) * 255.0 + 0.5)), 255)
   bor(bor(rn, bshl(gn, 8)), bor(bshl(bn, 16), bshl(scale_u8, 24)))
}

fn gltf_material_infos(any gltf_data) list {
   "Returns normalized glTF material records suitable for future PBR/IBL pipeline binding."
   shr._gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return [] }
   def materials = g.get("materials")
   if !is_list(materials) { return [] }
   def materials_n = materials.len
   mut cache_key = shr._gltf_cache_key_from_data(gltf_data)
   if _gltf_material_infos_cache.contains(cache_key) { return _gltf_material_infos_cache.get(cache_key, []) }
   mut out = list(materials_n)
   mut i = 0
   while i < materials_n {
      out = out.append(gltf_material_info(gltf_data, i))
      i += 1
   }
   _gltf_material_infos_cache = cache.cache_put_reset(shr._gltf_material_infos_cache,
      cache_key,
      out,
      shr._GLTF_CACHE_LIMIT_SMALL,
   32)
   out
}

fn gltf_material_infos_limited(any gltf_data, int limit=0) list {
   "Returns normalized glTF material records, capped to `limit` when positive."
   shr._gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return [] }
   def materials = g.get("materials")
   if !is_list(materials) { return [] }
   def materials_n = materials.len
   if limit <= 0 || limit >= materials_n { return gltf_material_infos(gltf_data) }
   def lim = max(0, limit)
   mut cache_key = shr._gltf_cache_key_from_data(gltf_data)
   cache_key = cache_key + "|limit=" + to_str(lim)
   if _gltf_material_infos_cache.contains(cache_key) { return _gltf_material_infos_cache.get(cache_key, []) }
   mut out = list(lim)
   mut i = 0
   while i < lim {
      out = out.append(gltf_material_info(gltf_data, i))
      i += 1
   }
   _gltf_material_infos_cache = cache.cache_put_reset(shr._gltf_material_infos_cache,
      cache_key,
      out,
      shr._GLTF_CACHE_LIMIT_SMALL,
   32)
   out
}

fn _gltf_indexed_material_fast_record(any texrec_fast, any info_fast) dict {
   def double_sided_fast = texrec_fast.get("double_sided", false) ? true : false
   {
      "packed_color": int(texrec_fast.get("base_color_u32", 0xffffffff)),
      "material_u32": int(texrec_fast.get("material_u32", 0x0000ff00)),
      "unlit": false, "nocull": double_sided_fast, "double_sided": double_sided_fast,
      "uv_set": int(texrec_fast.get("base_color_texcoord", 0)), "uv_xform": 0,
      "emit_uv_set": 0, "emissive_u32": 0, "normal_uv_set": 0, "mr_uv_set": 0,
      "alpha_u32": int(texrec_fast.get("alpha_u32", 0)), "occlusion_uv_set": 0,
      "base_uv_xf0": int(texrec_fast.get("base_uv_xf0", 0)), "base_uv_xf1": int(texrec_fast.get("base_uv_xf1", 0)),
      "normal_uv_xf0": 0, "normal_uv_xf1": 0, "mr_uv_xf0": 0, "mr_uv_xf1": 0,
      "occlusion_uv_xf0": 0, "occlusion_uv_xf1": 0, "emissive_uv_xf0": 0, "emissive_uv_xf1": 0,
      "bsdf0_u32": _gltf_keep_bsdf_record_value(int(texrec_fast.get("bsdf0_u32", 0)), _gltf_pack_bsdf0(info_fast)),
      "bsdf1_u32": _gltf_keep_bsdf_record_value(int(texrec_fast.get("bsdf1_u32", 0)), _gltf_pack_bsdf1(info_fast)),
      "bsdf2_u32": _gltf_keep_bsdf_record_value(int(texrec_fast.get("bsdf2_u32", 0)), _gltf_pack_bsdf2(info_fast)),
      "bsdf3_u32": _gltf_keep_bsdf_record_value(int(texrec_fast.get("bsdf3_u32", 0)), _gltf_pack_bsdf3(info_fast)),
      "bsdf4_u32": _gltf_keep_bsdf_record_value(int(texrec_fast.get("bsdf4_u32", 0)), _gltf_pack_bsdf4(info_fast)),
      "bsdf5_u32": _gltf_keep_bsdf_record_value(int(texrec_fast.get("bsdf5_u32", 0)), _gltf_pack_bsdf5(info_fast)),
      "ext2_tex_word": _gltf_keep_ext2_record_value(int(texrec_fast.get("ext2_tex_word", 0x80000000)), int(is_dict(info_fast) ? info_fast.get("ext2_tex_word", 0x80000000) : 0x80000000))
   }
}

fn _gltf_indexed_base_color_state(any info) dict {
   def f = is_dict(info) ? info.get("base_color_factor", [1.0, 1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0, 1.0]
   def r = int((clamp01(float(f.get(0, 1.0))) * 255.0))
   def g_val = int((clamp01(float(f.get(1, 1.0))) * 255.0))
   def b = int((clamp01(float(f.get(2, 1.0))) * 255.0))
   def alpha_mode = is_dict(info) ? to_str(info.get("alpha_mode", "OPAQUE")) : "OPAQUE"
   def is_blend = alpha_mode == "BLEND"
   def alpha_mode_code = shr._gltf_alpha_mode_code(alpha_mode)
   def alpha_cutoff_u8 = band(int((clamp01(is_dict(info) ? float(info.get("alpha_cutoff", 0.5)) : 0.5) * 255.0)), 255)
   def occ_strength_u8 = band(int((clamp01(is_dict(info) ? float(info.get("occlusion_strength", 1.0)) : 1.0) * 255.0)), 255)
   def double_sided = is_dict(info) ? info.get("double_sided", false) : false
   def a = is_blend ? int((clamp01(float(f.get(3, 1.0))) * 255.0)) : 255
   {
      "packed_base": bor(bor(r, bshl(g_val, 8)), bor(bshl(b, 16), bshl(a, 24))),
      "alpha_mode": alpha_mode,
      "alpha_mode_code": alpha_mode_code,
      "alpha_cutoff_u8": alpha_cutoff_u8,
      "occ_strength_u8": occ_strength_u8,
      "double_sided": double_sided
   }
}

fn _gltf_indexed_mr_state(any info, any texrec_uv) dict {
   mut metallic_u8 = band(int((clamp01(is_dict(info) ? float(info.get("metallic_factor", 1.0)) : 1.0) * 255.0)), 255)
   def rough_u8 = band(int((clamp01(is_dict(info) ? float(info.get("roughness_factor", 1.0)) : 1.0) * 255.0)), 255)
   if is_dict(info) && info.get("specular_glossiness", false) { metallic_u8 = 0 }
   def mr_tex_id = is_dict(texrec_uv) ? int(texrec_uv.get("metallic_roughness", -1)) : -1
   def mr_uv_set = is_dict(info) ? int(info.get("metallic_roughness_texcoord", 0)) : 0
   mut mr_tid = 0
   if mr_tex_id >= 0 { mr_tid = band(mr_tex_id + 1, 0x7fff) }
   mut mr_word = mr_tid
   if mr_uv_set != 0 { mr_word = bor(mr_word, 0x8000) }
   {
      "material_word": _pack_material_word(metallic_u8, rough_u8, mr_word),
      "mr_uv_set": mr_uv_set,
      "metallic_u8": metallic_u8,
      "rough_u8": rough_u8
   }
}

fn _gltf_indexed_uv_xforms(any info) dict {
   {
      "base": shr._gltf_pack_uv_xform_words(info, "base_color"),
      "normal": shr._gltf_pack_uv_xform_words(info, "normal"),
      "mr": shr._gltf_pack_uv_xform_words(info, "metallic_roughness"),
      "occlusion": shr._gltf_pack_uv_xform_words(info, "occlusion"),
      "emissive": shr._gltf_pack_uv_xform_words(info, "emissive")
   }
}

fn _gltf_indexed_material_info_record(any info, any texrec_uv) dict {
   def base = _gltf_indexed_base_color_state(info)
   def mr = _gltf_indexed_mr_state(info, texrec_uv)
   def uv_props = shr._gltf_pick_primary_uv_props(info, texrec_uv)
   def xfs = _gltf_indexed_uv_xforms(info)
   def base_uv_xf = xfs.get("base")
   def normal_uv_xf = xfs.get("normal")
   def mr_uv_xf = xfs.get("mr")
   def occ_uv_xf = xfs.get("occlusion")
   def emit_uv_xf = xfs.get("emissive")
   def alpha_base_word = bor(int(base.get("alpha_mode_code", 0)), bor(bshl(int(base.get("alpha_cutoff_u8", 0)), 8), bshl(int(base.get("occ_strength_u8", 0)), 16)))
   def double_sided = base.get("double_sided", false)
   {
      "packed_color": base.get("packed_base"), "material_u32": mr.get("material_word"),
      "unlit": is_dict(info) ? info.get("unlit", false) : false,
      "nocull": double_sided, "double_sided": double_sided,
      "uv_set": int(uv_props.get(0, 0)), "uv_xform": uv_props.get(1, 0),
      "emit_uv_set": is_dict(info) ? int(info.get("emissive_texcoord", 0)) : 0,
      "emissive_u32": _gltf_pack_emissive_word(info),
      "normal_uv_set": is_dict(info) ? int(info.get("normal_texcoord", 0)) : 0,
      "mr_uv_set": is_dict(info) ? int(info.get("metallic_roughness_texcoord", 0)) : 0,
      "alpha_u32": bor(alpha_base_word, _gltf_pack_alpha_ext_flags(info)),
      "occlusion_uv_set": is_dict(info) ? int(info.get("occlusion_texcoord", 0)) : 0,
      "base_uv_xf0": int(base_uv_xf.get(0, 0)), "base_uv_xf1": int(base_uv_xf.get(1, 0)),
      "normal_uv_xf0": int(normal_uv_xf.get(0, 0)), "normal_uv_xf1": int(normal_uv_xf.get(1, 0)),
      "mr_uv_xf0": int(mr_uv_xf.get(0, 0)), "mr_uv_xf1": int(mr_uv_xf.get(1, 0)),
      "occlusion_uv_xf0": int(occ_uv_xf.get(0, 0)), "occlusion_uv_xf1": int(occ_uv_xf.get(1, 0)),
      "emissive_uv_xf0": int(emit_uv_xf.get(0, 0)), "emissive_uv_xf1": int(emit_uv_xf.get(1, 0)),
      "bsdf0_u32": _gltf_pack_bsdf0(info), "bsdf1_u32": _gltf_pack_bsdf1(info),
      "bsdf2_u32": _gltf_pack_bsdf2(info), "bsdf3_u32": _gltf_pack_bsdf3(info),
      "bsdf4_u32": _gltf_pack_bsdf4(info), "bsdf5_u32": _gltf_pack_bsdf5(info),
      "ext2_tex_word": int(info.get("ext2_tex_word", 0x80000000))
   }
}

fn _gltf_indexed_material_records(list mat_infos, any material_tex_ids, int material_tex_ids_n, int mat_count_pre, bool use_fast_core_records) list {
   mut records = list(mat_count_pre)
   mut mj = 0
   while mj < mat_count_pre {
      if use_fast_core_records {
         def texrec_fast = material_tex_ids.get(mj, 0)
         if is_dict(texrec_fast) && bool(texrec_fast.get("fast_core_pbr", false)) {
            def info_fast = (mj >= 0 && mj < mat_infos.len) ? mat_infos.get(mj) : 0
            records = records.append(_gltf_indexed_material_fast_record(texrec_fast, info_fast))
            mj += 1
            continue
         }
      }
      def info = mat_infos.get(mj)
      def texrec_uv = (mj >= 0 && mj < material_tex_ids_n) ? material_tex_ids.get(mj, 0) : 0
      def rec = _gltf_indexed_material_info_record(info, texrec_uv)
      records = records.append(rec)
      mj += 1
   }
   records
}

fn _gltf_indexed_copy_i32_fields(dict st, dict src, list rows) dict {
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      def dst = to_str(row.get(0))
      def src_key = to_str(row.get(1, dst))
      st[dst] = int(src.get(src_key, st.get(dst, 0)))
      i += 1
   }
   st
}

fn _gltf_keep_indexed_bsdf_fields(dict st, dict src) dict {
   mut i = 0
   while i < 6 {
      def key = "bsdf" + to_str(i) + "_u32"
      st[key] = _gltf_keep_bsdf_record_value(int(src.get(key, 0)), int(st.get(key, 0)))
      i += 1
   }
   st
}

fn _gltf_indexed_part_material_defaults(int packed_color) dict {
   {
      "tex_id": -1, "normal_tex_id": -1, "normal_tex_word": 0x80000000,
      "emissive_tex_id": -1, "emissive_u32": 0, "normal_uv_set": 0,
      "emissive_uv_set": 0, "met_rough_uv_set": 0, "alpha_u32": 0,
      "occlusion_tex_id": -1, "occlusion_uv_set": 0,
      "bsdf0_u32": 0, "bsdf1_u32": 0, "bsdf2_u32": 0,
      "bsdf3_u32": 0, "bsdf4_u32": 0, "bsdf5_u32": 0,
      "ext2_tex_word": 0x80000000,
      "base_uv_xf0": 0, "base_uv_xf1": 0,
      "normal_uv_xf0": 0, "normal_uv_xf1": 0,
      "mr_uv_xf0": 0, "mr_uv_xf1": 0,
      "occlusion_uv_xf0": 0, "occlusion_uv_xf1": 0,
      "emissive_uv_xf0": 0, "emissive_uv_xf1": 0,
      "prim_packed_color": packed_color, "material_u32": 0x0000ff00,
      "is_unlit": false, "is_nocull": false, "is_double_sided": false,
      "uv_set": 0, "uv_xform": 0, "prim_vc_mode": 0
   }
}

fn _gltf_indexed_part_material_apply_record(dict st, any mat_state) dict {
   if !is_dict(mat_state) { return st }
   st = _gltf_indexed_copy_i32_fields(st, mat_state, [
         ["prim_packed_color", "packed_color"], ["material_u32"], ["normal_uv_set"],
         ["emissive_uv_set", "emit_uv_set"], ["emissive_u32"], ["met_rough_uv_set", "mr_uv_set"],
         ["alpha_u32"], ["occlusion_uv_set"], ["base_uv_xf0"], ["base_uv_xf1"],
         ["normal_uv_xf0"], ["normal_uv_xf1"], ["mr_uv_xf0"], ["mr_uv_xf1"],
         ["occlusion_uv_xf0"], ["occlusion_uv_xf1"], ["emissive_uv_xf0"], ["emissive_uv_xf1"],
         ["bsdf0_u32"], ["bsdf1_u32"], ["bsdf2_u32"], ["bsdf3_u32"], ["bsdf4_u32"], ["bsdf5_u32"],
         ["ext2_tex_word"], ["uv_set"],
   ])
   st["is_unlit"] = mat_state.get("unlit", false)
   st["is_nocull"] = mat_state.get("nocull", false)
   st["is_double_sided"] = mat_state.get("double_sided", false)
   st["uv_xform"] = mat_state.get("uv_xform", 0)
   st
}

fn _gltf_indexed_part_material_apply_texrec(dict st, dict texrec) dict {
   st = _gltf_indexed_copy_i32_fields(st, texrec, [
         ["prim_packed_color", "base_color_u32"], ["normal_tex_id", "normal"], ["normal_tex_word"],
         ["emissive_tex_id", "emissive"], ["emissive_u32"], ["occlusion_tex_id", "occlusion"],
         ["emissive_uv_set"], ["normal_uv_set"], ["met_rough_uv_set", "metallic_roughness_uv_set"],
         ["occlusion_uv_set"], ["alpha_u32"], ["base_uv_xf0"], ["base_uv_xf1"],
         ["normal_uv_xf0"], ["normal_uv_xf1"], ["mr_uv_xf0"], ["mr_uv_xf1"],
         ["occlusion_uv_xf0"], ["occlusion_uv_xf1"], ["emissive_uv_xf0"], ["emissive_uv_xf1"],
   ])
   st["tex_id"] = int(texrec.get("base_color", texrec.get("base", st.get("tex_id", -1))))
   st = _gltf_keep_indexed_bsdf_fields(st, texrec)
   st["ext2_tex_word"] = _gltf_keep_ext2_record_value(int(texrec.get("ext2_tex_word", 0x80000000)), int(st.get("ext2_tex_word", 0x80000000)))
   st["prim_vc_mode"] = bor(int(st.get("prim_vc_mode", 0)), int(texrec.get("vc_mode", 0)))
   mut tex_material_u32 = int(texrec.get("material_u32", st.get("material_u32", 0x0000ff00)))
   def material_u32 = int(st.get("material_u32", 0x0000ff00))
   if band(tex_material_u32, 0x0000ffff) == 0 && band(material_u32, 0x0000ffff) != 0 { tex_material_u32 = bor(band(tex_material_u32, 0xffff0000), band(material_u32, 0x0000ffff)) }
   def tex_mr_raw = int(texrec.get("metallic_roughness", texrec.get("mr", band(bshr(tex_material_u32, 16), 0x7fff))))
   mut tex_mr_id = 0
   if tex_mr_raw >= 0 { tex_mr_id = band(tex_mr_raw + 1, 0x7fff) }
   if tex_mr_raw >= 0 || int(st.get("met_rough_uv_set", 0)) != 0 {
      def tex_metallic_u8 = band(tex_material_u32, 255)
      def tex_rough_u8 = band(bshr(tex_material_u32, 8), 255)
      mut tex_mr_word = tex_mr_id
      if int(st.get("met_rough_uv_set", 0)) != 0 { tex_mr_word = bor(tex_mr_word, 0x8000) }
      tex_material_u32 = _pack_material_word(tex_metallic_u8, tex_rough_u8, tex_mr_word)
   }
   st["material_u32"] = tex_material_u32
   st
}

fn _gltf_indexed_part_force_uv_xform(dict st, str k0, str k1) dict {
   if band(int(st.get(k1, 0)), 0x40000000) != 0 {
      def xf = shr._gltf_uv_xf_force_uv0(int(st.get(k0, 0)), int(st.get(k1, 0)))
      st[k0] = int(xf.get(0, st.get(k0, 0)))
      st[k1] = int(xf.get(1, st.get(k1, 0)))
   }
   st
}

fn _gltf_indexed_part_material_force_uv0(dict st) dict {
   st["normal_uv_set"] = 0
   st["emissive_uv_set"] = 0
   st["met_rough_uv_set"] = 0
   st["occlusion_uv_set"] = 0
   st = _gltf_indexed_part_force_uv_xform(st, "base_uv_xf0", "base_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "normal_uv_xf0", "normal_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "mr_uv_xf0", "mr_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "occlusion_uv_xf0", "occlusion_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "emissive_uv_xf0", "emissive_uv_xf1")
   if band(int(st.get("material_u32", 0)), 0x80000000) != 0 { st["material_u32"] = band(int(st.get("material_u32", 0)), 0x7fffffff) }
   st["uv_set"] = 0
   st
}

fn _gltf_indexed_prim_material_state(
   any material_tex_ids, int material_tex_ids_n, list mat_records, int packed_color,
   dict meta
) dict {
   def mat_idx = int(meta.get("mat_idx", 0))
   def mat_record = (mat_idx >= 0 && mat_idx < mat_records.len) ? mat_records.get(mat_idx, 0) : 0
   mut material_state = _gltf_indexed_part_material_apply_record(_gltf_indexed_part_material_defaults(packed_color), mat_record)
   if mat_idx >= 0 && mat_idx < material_tex_ids_n {
      def texrec = material_tex_ids.get(mat_idx, 0)
      if is_dict(texrec) {
         material_state = _gltf_indexed_part_material_apply_texrec(material_state, texrec)
      } else {
         material_state["tex_id"] = int(texrec)
      }
   }
   def has_uv1 = int(meta.get("uv1_cnt", 0)) > 0
   if !has_uv1 { material_state = _gltf_indexed_part_material_force_uv0(material_state) }
   if !has_uv1 && int(material_state.get("uv_set", 0)) != 0 { material_state["uv_set"] = 0 }
   ;; Vertex colors are optional in glTF. Keep them opt-in for the viewer until
   ;; the attribute mapper is proven for every sample model. A bad COLOR_0 decode
   ;; multiplies the base-color texture and shows up exactly like gray/striped
   ;; Avocado materials in the Vulkan path.
   if common.env_truthy("NY_GLTF_VERTEX_COLORS") && int(meta.get("c_acc_idx", -1)) >= 0 && int(meta.get("c_cnt", 0)) > 0 {
      material_state["prim_vc_mode"] = bor(int(material_state.get("prim_vc_mode", 0)), 4)
   }
   material_state
}

fn _gltf_indexed_mesh_opts(dict material_state, bool idx_u32, dict meta, int prim_mode, str use_static) dict {
   mut mesh_opts = dict(4)
   if idx_u32 { mesh_opts["index_type_u32"] = true }
   def prim_has_normals = int(meta.get("n_cnt", 0)) > 0
   mesh_opts["has_normals"] = prim_has_normals
   mesh_opts = shr._gltf_apply_prim_mode_opts(mesh_opts, prim_mode, prim_has_normals)
   if use_static != "static" { mesh_opts["storage"] = use_static }
   if material_state.get("is_unlit", false) { mesh_opts["unlit"] = true }
   if material_state.get("is_nocull", false) { mesh_opts["no_cull"] = true }
   if material_state.get("is_double_sided", false) { mesh_opts["double_sided"] = true }
   mesh_opts
}

fn _gltf_indexed_default_color(any gltf_data) int {
   mut packed_color = int(gltf_data.get("color", 0))
   if packed_color != 0 { return packed_color }
   def r = int((clamp01(gltf_data.get("r", 1.0)) * 255.0))
   def gr = int((clamp01(gltf_data.get("g", 1.0)) * 255.0))
   def b = int((clamp01(gltf_data.get("b", 1.0)) * 255.0))
   def a = int((clamp01(gltf_data.get("a", 1.0)) * 255.0))
   (a << 24) | (b << 16) | (gr << 8) | r
}

fn _gltf_indexed_material_record_setup(any gltf_data, any material_tex_ids, int mesh_limit) list {
   def material_tex_ids_n = is_list(material_tex_ids) ? material_tex_ids.len : 0
   def fast_core_input_records = material_tex_ids_n > 0 && is_dict(material_tex_ids.get(0, 0)) && bool(material_tex_ids.get(0, 0).get("fast_core_pbr", false))
   def limited_mat_infos = gltf_data.get("__material_infos_limited", 0)
   def mat_infos = is_list(limited_mat_infos) ? limited_mat_infos : gltf_material_infos_limited(gltf_data, mesh_limit)
   def mat_count_pre = fast_core_input_records ? material_tex_ids_n : mat_infos.len
   def use_fast_core_records = fast_core_input_records && material_tex_ids_n == mat_count_pre
   [material_tex_ids_n, _gltf_indexed_material_records(mat_infos, material_tex_ids, material_tex_ids_n, mat_count_pre, use_fast_core_records)]
}

fn _gltf_has_uri(any info, str slot_key) bool { to_str(info.get(slot_key + "_uri", "")) != "" }

fn _gltf_material_uri_feature_mask(any info) int {
   def rows = [
      ["base_color", 1],
      ["normal", 2],
      ["occlusion", 4],
      ["emissive", 8],
      ["specular", 16],
      ["specular_color", 16],
      ["sheen_color", 32],
      ["sheen_roughness", 32],
      ["clearcoat", 64],
      ["clearcoat_normal", 64],
      ["transmission", 128],
      ["thickness", 256],
      ["iridescence", 1024],
      ["iridescence_thickness", 1024],
      ["anisotropy", 2048],
      ["diffuse_transmission", 8192],
      ["subsurface", 65536]
   ]
   mut m = 0
   def n = rows.len
   mut i = 0
   while i < n {
      def row = rows[i]
      if _gltf_has_uri(info, to_str(row[0])) { m = bor(m, int(row[1])) }
      i += 1
   }
   m
}

fn _gltf_material_gt_feature_mask(any info) int {
   def rows = [
      ["emissive_strength", 1.0, 8],
      ["sheen_roughness_factor", 0.0, 32],
      ["clearcoat_factor", 0.0, 64],
      ["transmission_factor", 0.0, 128],
      ["thickness_factor", 0.0, 256],
      ["iridescence_factor", 0.0, 1024],
      ["anisotropy_strength", 0.0, 2048],
      ["dispersion", 0.0, 4096],
      ["diffuse_transmission_factor", 0.0, 8192],
      ["refraction_factor", 0.0, 32768],
      ["subsurface_factor", 0.0, 65536]
   ]
   mut m = 0
   def n = rows.len
   mut i = 0
   while i < n {
      def row = rows[i]
      if float(info.get(to_str(row[0]), row[1])) > float(row[1]) { m = bor(m, int(row[2])) }
      i += 1
   }
   m
}

fn _gltf_material_ne_feature_mask(any info) int {
   def rows = [
      ["specular_factor", 1.0, 16],
      ["ior", 1.5, 512],
      ["alpha_coverage", 1.0, 16384]
   ]
   mut m = 0
   def n = rows.len
   mut i = 0
   while i < n {
      def row = rows[i]
      if float(info.get(to_str(row[0]), row[1])) != float(row[1]) { m = bor(m, int(row[2])) }
      i += 1
   }
   m
}

fn _gltf_material_bool_feature_mask(any info) int {
   def rows = [
      ["unlit", 131072],
      ["specular_glossiness", 262144]
   ]
   mut m = 0
   def n = rows.len
   mut i = 0
   while i < n {
      def row = rows[i]
      if info.get(to_str(row[0]), false) { m = bor(m, int(row[1])) }
      i += 1
   }
   m
}

fn gltf_material_feature_mask(any info) int {
   "Returns a feature bitmask for a normalized material info dict. Unwired shader passes can consume this directly."
   if !is_dict(info) { return 0 }
   mut m = _gltf_material_uri_feature_mask(info)
   m = bor(m, _gltf_material_gt_feature_mask(info))
   m = bor(m, _gltf_material_ne_feature_mask(info))
   m = bor(m, _gltf_material_bool_feature_mask(info))
   m
}
