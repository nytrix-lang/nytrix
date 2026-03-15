;; Keywords: 3d gltf glb
;; glTF 2.0 loader with proper indexed primitive expansion
module std.parse.3d.gltf(load_gltf, load_gltf_file, parse_gltf_str, gltf_mesh_count, gltf_get_mesh, gltf_mesh_to_vertices, gltf_material_infos, gltf_material_infos_limited, gltf_material_info, gltf_material_extension_info, gltf_extensions_report, gltf_supported_extension_caps, gltf_material_texture_uris, gltf_material_variant_mappings, gltf_node_instancing_info, gltf_material_feature_mask, gltf_material_texture_slot_table, gltf_scene_feature_summary, gltf_required_extension_failures, gltf_animation_pointer_bindings, gltf_extension_payloads, gltf_mesh_to_vertex_buffers, gltf_to_vertex_buffer, gltf_to_mesh_group_indexed, gltf_scene_punctual_lights, gltf_camera_count, gltf_camera_info, gltf_camera_instances, gltf_skin_info, gltf_warm_runtime, gltf_free_data, gltf_skin_count, gltf_morph_target_count, gltf_animation_count, gltf_animation_info, gltf_sample_animation, gltf_rebuild_animated_mats, gltf_skin_joint_mats, gltf_apply_skinning, gltf_free_skin_mats_cache, gltf_has_node_visibility, gltf_resolve_node_visibility, gltf_apply_morph_weights, gltf_sample_animation_merged, _gltf_read_f32_fast, _gltf_comp_size, _gltf_type_count, GLTF_COMP_NONE, GLTF_COMP_BYTE, GLTF_COMP_UBYTE, GLTF_COMP_SHORT, GLTF_COMP_USHORT, GLTF_COMP_UINT, GLTF_COMP_FLOAT, GLTF_TYPE_SCALAR, GLTF_TYPE_VEC2, GLTF_TYPE_VEC3, GLTF_TYPE_VEC4, GLTF_TYPE_MAT2, GLTF_TYPE_MAT3, GLTF_TYPE_MAT4)
use std.core
use std.os
use std.os.interact
use std.parse.data.json
use std.math.bin
use std.math
use std.math.float (is_nan, is_inf)
use std.math.crypto.hash as lib_hash
use std.parse.3d.meshopt
use std.os.path as ospath
use std.core.str as str
use std.math.crypto.encoding.base as str_base
use std.core.common as common
use std.core.cache as cache
use std.parse.3d.gltf_math as gltf_math

def GLTF_COMP_NONE = 0
def GLTF_COMP_BYTE = 5120
def GLTF_COMP_UBYTE = 5121
def GLTF_COMP_SHORT = 5122
def GLTF_COMP_USHORT = 5123
def GLTF_COMP_UINT = 5125
def GLTF_COMP_FLOAT = 5126
def GLTF_TYPE_SCALAR = "SCALAR"
def GLTF_TYPE_VEC2 = "VEC2"
def GLTF_TYPE_VEC3 = "VEC3"
def GLTF_TYPE_VEC4 = "VEC4"
def GLTF_TYPE_MAT2 = "MAT2"
def GLTF_TYPE_MAT3 = "MAT3"
def GLTF_TYPE_MAT4 = "MAT4"
def _GLB_MAGIC = 0x46546C67
def _GLB_CHUNK_JSON = 0x4E4F534A
def _GLB_CHUNK_BIN = 0x004E4942

fn _gltf_is_json_ws(int: c): bool {
   case c {
      0, 9, 10, 11, 12, 13, 32 -> true
      _ -> false
   }
}

comptime table GltfCompSize {
   GLTF_COMP_BYTE, GLTF_COMP_UBYTE -> 1
   GLTF_COMP_SHORT, GLTF_COMP_USHORT -> 2
   GLTF_COMP_UINT, GLTF_COMP_FLOAT -> 4
}

comptime table GltfTypeCount {
   GLTF_TYPE_SCALAR -> 1
   GLTF_TYPE_VEC2 -> 2
   GLTF_TYPE_VEC3 -> 3
   GLTF_TYPE_VEC4 -> 4
   GLTF_TYPE_MAT2 -> 4
   GLTF_TYPE_MAT3 -> 9
   GLTF_TYPE_MAT4 -> 16
}

comptime table GltfTypeCols {
   GLTF_TYPE_SCALAR, GLTF_TYPE_VEC2, GLTF_TYPE_VEC3, GLTF_TYPE_VEC4 -> 1
   GLTF_TYPE_MAT2 -> 2
   GLTF_TYPE_MAT3 -> 3
   GLTF_TYPE_MAT4 -> 4
}

comptime table GltfTypeRows {
   GLTF_TYPE_SCALAR -> 1
   GLTF_TYPE_VEC2, GLTF_TYPE_MAT2 -> 2
   GLTF_TYPE_VEC3, GLTF_TYPE_MAT3 -> 3
   GLTF_TYPE_VEC4, GLTF_TYPE_MAT4 -> 4
}

comptime table GltfImageMimeSupported {
   "image/png", "image/jpeg", "image/webp", "image/ktx2" -> true
}

comptime table GltfImageExtFromMime {
   "image/png" -> ".png"
   "image/jpeg", "image/jpg" -> ".jpg"
   "image/webp" -> ".webp"
   "image/ktx2" -> ".ktx2"
   "image/bmp" -> ".bmp"
}

comptime table GltfExtensionStatus {
   "KHR_texture_transform", "KHR_materials_unlit", "KHR_materials_emissive_strength" -> "parse+shader"
   "KHR_materials_specular", "KHR_materials_ior", "KHR_materials_sheen", "KHR_materials_clearcoat",
   "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_iridescence",
   "KHR_materials_anisotropy", "KHR_materials_dispersion", "KHR_materials_refraction",
   "KHR_materials_subsurface" -> "parse+packed"
   "KHR_meshopt_compression", "EXT_meshopt_compression", "EXT_texture_webp" -> "parse+decode"
   "KHR_texture_basisu" -> "fallback-or-todo"
   "KHR_draco_mesh_compression" -> "todo"
   "KHR_materials_volume_scatter", "KHR_materials_diffuse_transmission", "KHR_materials_pbrSpecularGlossiness",
   "KHR_materials_variants", "KHR_lights_punctual", "KHR_node_visibility", "KHR_animation_pointer",
   "EXT_mesh_gpu_instancing", "KHR_materials_alpha_coverage", "MSFT_lod", "KHR_mesh_quantization", "KHR_xmp", "KHR_xmp_json_ld" -> "parse"
}

mut _gltf_img_uri_cache = dict(128)
mut _gltf_material_infos_cache = dict(32)
mut _gltf_anim_info_cache = dict(64)
mut _gltf_acc_res_cache = dict(256)
mut _gltf_anim_sample_cache = dict(64)
mut _gltf_node_local_mats_cache = dict(32)
mut _gltf_skin_inv_bind_cache = dict(32)
mut _gltf_mesh_inv_cache = dict(32)
mut _gltf_visibility_flag_cache = dict(64)
mut _gltf_scene_feature_summary_cache = dict(32)
mut _gltf_anim_trace_cache = -1
mut _gltf_debug_cache = -1
mut _gltf_autolod_cache = -1

fn _gltf_ensure_caches(): any {
   if(!is_dict(_gltf_img_uri_cache)){ _gltf_img_uri_cache = dict(128) }
   if(!is_dict(_gltf_material_infos_cache)){ _gltf_material_infos_cache = dict(32) }
   if(!is_dict(_gltf_anim_info_cache)){ _gltf_anim_info_cache = dict(64) }
   if(!is_dict(_gltf_acc_res_cache)){ _gltf_acc_res_cache = dict(256) }
   if(!is_dict(_gltf_anim_sample_cache)){ _gltf_anim_sample_cache = dict(64) }
   if(!is_dict(_gltf_node_local_mats_cache)){ _gltf_node_local_mats_cache = dict(32) }
   if(!is_dict(_gltf_skin_inv_bind_cache)){ _gltf_skin_inv_bind_cache = dict(32) }
   if(!is_dict(_gltf_mesh_inv_cache)){ _gltf_mesh_inv_cache = dict(32) }
   if(!is_dict(_gltf_visibility_flag_cache)){ _gltf_visibility_flag_cache = dict(64) }
   if(!is_dict(_gltf_scene_feature_summary_cache)){ _gltf_scene_feature_summary_cache = dict(32) }
}

fn _gltf_cache_key_from_g(any: g): str {
   if(!is_dict(g)){ return "gltf:none" }
   def explicit = to_str(g.get("_ny_cache_key", ""))
   if(explicit.len > 0){ return explicit }
   def base_path = to_str(g.get("_ny_base_path", ""))
   def materials = g.get("materials", [])
   def meshes = g.get("meshes", [])
   def nodes = g.get("nodes", [])
   def buffers = g.get("buffers", [])
   def images = g.get("images", [])
   def accessors = g.get("accessors", [])
   base_path
   + "|mat=" + to_str(is_list(materials) ? materials.len : 0)
   + "|mesh=" + to_str(is_list(meshes) ? meshes.len : 0)
   + "|node=" + to_str(is_list(nodes) ? nodes.len : 0)
   + "|buf=" + to_str(is_list(buffers) ? buffers.len : 0)
   + "|img=" + to_str(is_list(images) ? images.len : 0)
   + "|acc=" + to_str(is_list(accessors) ? accessors.len : 0)
}

fn _gltf_cache_key_from_data(any: gltf_data): str {
   if(!is_dict(gltf_data)){ return "gltf:data:none" }
   def source_path = to_str(gltf_data.get("source_path", ""))
   if(source_path.len > 0){ return source_path }
   def g = gltf_data.get("gltf", 0)
   _gltf_cache_key_from_g(g)
}

fn _gltf_stamp_cache_key(any: gltf_data, any: key): bool {
   if(!is_dict(gltf_data)){ return false }
   def g = gltf_data.get("gltf", 0)
   if(is_dict(g)){
      g["_ny_cache_key"] = to_str(key)
      gltf_data["gltf"] = g
   }
   true
}

mut _gltf_skin_trace_printed = 0
def _GLTF_CACHE_LIMIT_SMALL = 2048
def _GLTF_CACHE_LIMIT_MED = 8192
def _GLTF_CACHE_LIMIT_BIG = 16384
def GLTF_MODE_POINTS = 0
def GLTF_MODE_LINES = 1
def GLTF_MODE_LINE_LOOP = 2
def GLTF_MODE_LINE_STRIP = 3
def GLTF_MODE_TRIANGLES = 4
def GLTF_MODE_TRIANGLE_STRIP = 5
def GLTF_MODE_TRIANGLE_FAN = 6
def _GLTF_VTX_STRIDE = 64
def _GLTF_VTX_OFF_X = 0
def _GLTF_VTX_OFF_Y = 4
def _GLTF_VTX_OFF_Z = 8
def _GLTF_VTX_OFF_U = 12
def _GLTF_VTX_OFF_V = 16
def _GLTF_VTX_OFF_C = 20
def _GLTF_VTX_OFF_NX = 24
def _GLTF_VTX_OFF_NY = 28
def _GLTF_VTX_OFF_NZ = 32
def _GLTF_VTX_OFF_TX = 36
def _GLTF_VTX_OFF_TY = 40
def _GLTF_VTX_OFF_TZ = 44
def _GLTF_VTX_OFF_TW = 48
def _GLTF_VTX_OFF_U2 = 52
def _GLTF_VTX_OFF_V2 = 56
def _GLTF_VTX_OFF_TEX = 60
def _GLTF_INV_255 = 0.00392156862745098
def _GLTF_INV_127 = 0.007874015748031496
def _GLTF_INV_65535 = 0.000015259021896696422
def _GLTF_INV_32767 = 0.00003051850947599719

fn _gltf_copy_bytes(any: data, int: start, int: count): str {
   if(!is_str(data) || count <= 0){ return "" }
   mut n = count
   if(start < 0){ start = 0 }
   def total = data.len
   if(start >= total){ return "" }
   if(start + n > total){ n = total - start }
   if(n <= 0){ return "" }
   def out = malloc(n + 1)
   if(!out){ return "" }
   init_str(out, n)
   memcpy(out, data + start, n)
   store8(out, 0, n)
   out
}

fn _gltf_copy_blob_bytes(any: data, int: start, int: count): str {
   if(is_str(data)){ return _gltf_copy_bytes(data, start, count) }
   if(count <= 0){ return "" }
   mut src = 0
   mut total = 0
   if(is_dict(data)){
      src = data.get("ptr", 0)
      total = int(data.get("len", 0))
   } else {
      src = data
   }
   if(!src){ return "" }
   mut off = int(start)
   if(off < 0){ off = 0 }
   mut n = int(count)
   if(total > 0){
      if(off >= total){ return "" }
      if(off + n > total){ n = total - off }
   }
   if(n <= 0){ return "" }
   def out = malloc(n + 1)
   if(!out){ return "" }
   init_str(out, n)
   memcpy(out, ptr_add(src, off), n)
   store8(out, 0, n)
   out
}

fn _gltf_blob_ptr(any: v): any {
   if(is_dict(v)){ return v.get("ptr", 0) }
   v
}

fn _gltf_is_path_sep(int: c): bool { c == 47 || c == 92 }

fn _gltf_path_dirname(any: path): str {
   if(!is_str(path)){ return "." }
   def n = path.len
   if(n <= 0){ return "." }
   mut end = n
   while(end > 1 && _gltf_is_path_sep(load8(path, end - 1))){ end -= 1 }
   mut j = end - 1
   while(j >= 0 && !_gltf_is_path_sep(load8(path, j))){ j -= 1 }
   if(j < 0){ return "." }
   case j {
      0 -> _gltf_copy_bytes(path, 0, 1)
      2 if path.len >= 3 && load8(path, 1) == 58 -> _gltf_copy_bytes(path, 0, 3)
      _ -> _gltf_copy_bytes(path, 0, j)
   }
}

fn _gltf_url_decode(any: s): str {
   if(!is_str(s)){ return "" }
   if(str.find(s, "%") < 0 && str.find(s, "+") < 0){ return s }
   def n = s.len
   def out = malloc(n + 1)
   if(!out){ return "" }
   init_str(out, n)
   mut i, o = 0, 0
   while(i < n){
      def c = load8(s, i)
      case c {
         37 if i + 2 < n -> { ;; '%'
            def hi = str.hex_val(load8(s, i + 1))
            def lo = str.hex_val(load8(s, i + 2))
            if(hi >= 0 && lo >= 0){
               store8(out, hi * 16 + lo, o)
               o += 1
               i = i + 3
            } else {
               store8(out, 37, o)
               o += 1
               i += 1
            }
         }
         43 -> { ;; '+'
            store8(out, 32, o)
            o += 1
            i += 1
         }
         _ -> {
            store8(out, c, o)
            o += 1
            i += 1
         }
      }
   }
   store8(out, 0, o)
   out
}

fn _gltf_stage_binary_blob(any: raw): any {
   if(!raw){ return 0 }
   mut raw_len = 0
   if(is_dict(raw)){
      raw_len = int(raw.get("len", 0))
      raw = raw.get("ptr", 0)
   } elif(is_str(raw)){
      raw_len = raw.len
   }
   if(!raw || raw_len <= 0){ return 0 }
   def buf = malloc(raw_len)
   if(!buf){ return 0 }
   memcpy(buf, raw, raw_len)
   return {"ptr": buf, "len": raw_len, "kind": "buf"}
}

fn _gltf_data_uri_decode(any: uri): any {
   if(!is_str(uri) || uri.len == 0){ return 0 }
   def comma = str.find(uri, ",")
   if(comma < 0){ return 0 }
   def payload = _gltf_copy_bytes(uri, comma + 1, uri.len - (comma + 1))
   if(str.find(uri, ";base64,") >= 0){ return _gltf_stage_binary_blob(str_base.decode64(payload)) }
   _gltf_stage_binary_blob(payload)
}

fn _gltf_load_buffer_uri(any: uri, str: base_path=""): any {
   if(!is_str(uri) || uri.len == 0){ return 0 }
   if(str.find(uri, "data:") == 0){ return _gltf_data_uri_decode(uri) }
   def full_path = ospath.join(base_path, _gltf_url_decode(uri))
   def res = file_read(full_path)
   if(is_err(res)){ return 0 }
   def raw = unwrap(res)
   _gltf_stage_binary_blob(raw)
}

fn _gltf_parse_glb(any: data, str: base_path=""): dict {
   if(!is_str(data) || data.len < 20){ return {"error": "Invalid GLB payload"} }
   if(u32le(data, 0) != _GLB_MAGIC){ return {"error": "Invalid GLB magic"} }
   mut off = 12
   mut json_chunk = 0
   mut bin_chunk = 0
   while(off + 8 <= data.len){
      def chunk_len = u32le(data, off)
      def chunk_type = u32le(data, off + 4)
      def chunk_data_off = off + 8
      if(chunk_data_off + chunk_len > data.len){ break }
      if(chunk_type == _GLB_CHUNK_JSON && !json_chunk){ json_chunk = _gltf_copy_bytes(data, chunk_data_off, chunk_len) } elif(chunk_type == _GLB_CHUNK_BIN && !bin_chunk){ bin_chunk = _gltf_copy_bytes(data, chunk_data_off, chunk_len) }
      off = chunk_data_off + chunk_len
   }
   if(!is_str(json_chunk) || json_chunk.len == 0){ return {"error": "Missing GLB JSON chunk"} }
   mut json_len = json_chunk.len
   while(json_len > 0){
      if(_gltf_is_json_ws(load8(json_chunk, json_len - 1))){ json_len -= 1 }
      else { break }
   }
   def json_clean = (json_len == json_chunk.len) ? json_chunk : _gltf_copy_bytes(json_chunk, 0, json_len)
   mut bin_data = _gltf_stage_binary_blob(bin_chunk)
   _gltf_parse_gltf_with_fallbacks(json_clean, base_path, bin_data)
}

fn _gltf_strip_top_level_key(any: json_str, any: key_name): any {
   if(!is_str(json_str)){ return json_str }
   def key_pat = "\"" + to_str(key_name) + "\""
   def pos = str.find(json_str, key_pat)
   if(pos < 0){ return json_str }
   mut colon = pos + key_pat.len
   while(colon < json_str.len && load8(json_str, colon) != 58){ colon += 1 }
   if(colon >= json_str.len){ return json_str }
   mut end = colon + 1
   mut depth = 0
   mut in_str = false
   mut esc = false
   while(end < json_str.len){
      def c = load8(json_str, end)
      if(in_str){
         if(esc){ esc = false }
         elif(c == 92){ esc = true }
         elif(c == 34){ in_str = false }
      } else {
         if(c == 34){ in_str = true }
         elif(c == 123 || c == 91){ depth += 1 }
         elif(c == 125 || c == 93){
            depth -= 1
            if(depth <= 0){ end += 1 break }
         }
      }
      end += 1
   }
   mut start = pos
   while(start > 0){
      if(_gltf_is_json_ws(load8(json_str, start - 1))){ start -= 1 }
      else { break }
   }
   if(start > 0 && load8(json_str, start - 1) == 44){ start -= 1 }
   mut remove_end = end
   while(remove_end < json_str.len){
      if(_gltf_is_json_ws(load8(json_str, remove_end))){ remove_end += 1 }
      else { break }
   }
   if(remove_end < json_str.len && load8(json_str, remove_end) == 44){ remove_end += 1 }
   def left = start > 0 ? _gltf_copy_bytes(json_str, 0, start) : ""
   def right = remove_end < json_str.len ? _gltf_copy_bytes(json_str, remove_end, json_str.len - remove_end) : ""
   left + right
}

fn _gltf_strip_asset_field(any: json_str, any: field_name): any {
   if(!is_str(json_str)){ return json_str }
   def asset_pat = "\"asset\""
   def pos = str.find(json_str, asset_pat)
   if(pos < 0){ return json_str }
   mut colon = pos + asset_pat.len
   while(colon < json_str.len && load8(json_str, colon) != 58){ colon += 1 }
   if(colon >= json_str.len){ return json_str }
   mut start = colon + 1
   while(start < json_str.len){
      if(_gltf_is_json_ws(load8(json_str, start))){ start += 1 }
      else { break }
   }
   if(start >= json_str.len || load8(json_str, start) != 123){ return json_str }
   mut end = start + 1
   mut depth = 1
   mut in_str = false
   mut esc = false
   while(end < json_str.len){
      def c = load8(json_str, end)
      if(in_str){
         if(esc){ esc = false }
         elif(c == 92){ esc = true }
         elif(c == 34){ in_str = false }
      } else {
         if(c == 34){ in_str = true }
         elif(c == 123){ depth += 1 }
         elif(c == 125){
            depth -= 1
            if(depth <= 0){ end += 1 break }
         }
      }
      end += 1
   }
   if(end <= start){ return json_str }
   def asset_json = _gltf_copy_bytes(json_str, start, end - start)
   def stripped_asset = _gltf_strip_top_level_key(asset_json, field_name)
   if(stripped_asset == asset_json){ return json_str }
   def left = start > 0 ? _gltf_copy_bytes(json_str, 0, start) : ""
   def right = end < json_str.len ? _gltf_copy_bytes(json_str, end, json_str.len - end) : ""
   left + stripped_asset + right
}

fn _gltf_parse_gltf_with_fallbacks(any: json_str, str: base_path="", any: binary_override=0): any {
   def direct = parse_gltf_str(json_str, base_path, binary_override)
   if(is_dict(direct) && !direct.contains("error") && !_gltf_doc_has_invalid_node_trs(direct)){ return direct }
   if(
      !is_dict(direct) ||
      !str.startswith(to_str(direct.get("error", "")), "Failed to parse glTF JSON")
   ){
      return direct
   }
   mut cur = json_str
   def retry_keys = ["copyright", "generator", "name", "extras"]
   def retry_keys_n = retry_keys.len
   mut i = 0
   while(i < retry_keys_n){
      cur = _gltf_strip_asset_field(cur, to_str(retry_keys[i]))
      def retry = parse_gltf_str(cur, base_path, binary_override)
      if(is_dict(retry) && !retry.contains("error")){ return retry }
      i += 1
   }
   def retry_top_keys = ["extensions", "extensionsUsed", "extensionsRequired"]
   def retry_top_keys_n = retry_top_keys.len
   i = 0
   while(i < retry_top_keys_n){
      cur = _gltf_strip_top_level_key(cur, to_str(retry_top_keys[i]))
      def retry = parse_gltf_str(cur, base_path, binary_override)
      if(is_dict(retry) && !retry.contains("error")){ return retry }
      i += 1
   }
   cur = _gltf_strip_asset_field(cur, "extensions")
   def retry_asset_ext = parse_gltf_str(cur, base_path, binary_override)
   if(is_dict(retry_asset_ext) && !retry_asset_ext.contains("error")){ return retry_asset_ext }
   direct
}

fn _gltf_float_bad(any: v): bool {
   if(!is_int(v) && !is_float(v)){ return true }
   if(is_nan(v) || is_inf(v)){ return true }
   abs(0.0 + v) > 1000000.0
}

fn _gltf_num_or(any: v, f64: d): f64 {
   if(!is_int(v) && !is_float(v)){ return d }
   if(is_nan(v) || is_inf(v)){ return d }
   def out = 0.0 + v
   if(abs(out) > 1000000.0){ return d }
   out
}

fn _gltf_vec3(any: v, f64: x, f64: y, f64: z): list {
   if(!is_list(v)){ return [float(x), float(y), float(z)] }
   [
      _gltf_num_or(v.get(0, x), x),
      _gltf_num_or(v.get(1, y), y),
      _gltf_num_or(v.get(2, z), z)
   ]
}

fn _gltf_vec4(any: v, f64: x, f64: y, f64: z, f64: w): list {
   if(!is_list(v)){ return [float(x), float(y), float(z), float(w)] }
   [
      _gltf_num_or(v.get(0, x), x),
      _gltf_num_or(v.get(1, y), y),
      _gltf_num_or(v.get(2, z), z),
      _gltf_num_or(v.get(3, w), w)
   ]
}

fn _gltf_anim_duration_valid(any: v): bool {
   if(!is_int(v) && !is_float(v)){ return false }
   if(is_nan(v) || is_inf(v)){ return false }
   def out = 0.0 + v
   out > 0.0001 && out < 3600.0
}

fn _gltf_doc_has_invalid_node_trs(any: doc): bool {
   mut g0 = 0
   if(is_dict(doc)){ g0 = doc.get("gltf", 0) }
   def g = is_dict(g0) ? g0 : doc
   if(!is_dict(g)){ return false }
   def nodes = g.get("nodes", 0)
   if(!is_list(nodes)){ return false }
   def nodes_n = nodes.len
   mut ni = 0
   while(ni < nodes_n){
      def node = nodes[ni]
      if(is_dict(node)){
         def mat = node.get("matrix", 0)
         if(is_list(mat) && mat.len >= 16){
            mut mi = 0
            while(mi < 16){
               def v = float(mat[mi])
               if(_gltf_float_bad(v)){ return true }
               mi += 1
            }
         } else {
            def tr = node.get("translation", 0)
            def ro = node.get("rotation", 0)
            def sc = node.get("scale", 0)
            mut ai = 0
            while(is_list(tr) && ai < tr.len){
               def v = float(tr[ai])
               if(_gltf_float_bad(v)){ return true }
               ai += 1
            }
            ai = 0
            while(is_list(ro) && ai < ro.len){
               def v = float(ro[ai])
               if(_gltf_float_bad(v)){ return true }
               ai += 1
            }
            ai = 0
            while(is_list(sc) && ai < sc.len){
               def v = float(sc[ai])
               if(_gltf_float_bad(v)){ return true }
               ai += 1
            }
         }
      }
      ni += 1
   }
   false
}

@jit
fn _gltf_align_up(int: v, int: a): int {
   if(a <= 1){ return v }
   ((v + a - 1) / a) * a
}

@jit
fn _gltf_elem_size(int: comp_size, str: type_str): int {
   def cols = _gltf_type_cols(type_str)
   def rows = _gltf_type_rows(type_str)
   if(cols <= 0 || rows <= 0){ return 0 }
   if(cols == 1){ return comp_size * rows }
   cols * _gltf_align_up(rows * comp_size, 4)
}

fn _gltf_read_f32_fast(any: data, int: offset, int: comp_type): f64 {
   case comp_type {
      GLTF_COMP_FLOAT -> f32le(data, offset)
      GLTF_COMP_UBYTE -> load8(data, offset) * _GLTF_INV_255
      GLTF_COMP_BYTE -> {
         def raw = load8(data, offset)
         def sval = raw >= 128 ? raw - 256 : raw
         def fv = sval * _GLTF_INV_127
         fv < -1.0 ? -1.0 : fv
      }
      GLTF_COMP_USHORT -> u16le(data, offset) * _GLTF_INV_65535
      GLTF_COMP_SHORT -> {
         def raw = u16le(data, offset)
         def sval = raw >= 32768 ? raw - 65536 : raw
         def fv = sval * _GLTF_INV_32767
         fv < -1.0 ? -1.0 : fv
      }
      GLTF_COMP_UINT -> u32le(data, offset) * 1.0
      _ -> 0.0
   }
}

fn _gltf_read_f32_acc(any: data_ptr, int: offset, int: comp_type, bool: normalized=false): f64 {
   case comp_type {
      GLTF_COMP_FLOAT -> f32le(data_ptr, offset)
      GLTF_COMP_UBYTE -> {
         def raw = load8(data_ptr, offset)
         normalized ? raw * _GLTF_INV_255 : raw * 1.0
      }
      GLTF_COMP_BYTE -> {
         def raw = load8(data_ptr, offset)
         def sval = raw >= 128 ? raw - 256 : raw
         if(normalized){
            def fv = sval * _GLTF_INV_127
            return fv < -1.0 ? -1.0 : fv
         }
         sval * 1.0
      }
      GLTF_COMP_USHORT -> {
         def raw = u16le(data_ptr, offset)
         normalized ? raw * _GLTF_INV_65535 : raw * 1.0
      }
      GLTF_COMP_SHORT -> {
         def raw = u16le(data_ptr, offset)
         def sval = raw >= 32768 ? raw - 65536 : raw
         if(normalized){
            def fv = sval * _GLTF_INV_32767
            return fv < -1.0 ? -1.0 : fv
         }
         sval * 1.0
      }
      GLTF_COMP_UINT -> {
         def raw = u32le(data_ptr, offset)
         normalized ? raw * 0.00000000023283064370807974 : raw * 1.0
      }
      _ -> 0.0
   }
}

@jit
fn _gltf_read_index_acc(any: data_ptr, int: offset, int: comp_type): int {
   case comp_type {
      GLTF_COMP_USHORT -> u16le(data_ptr, offset)
      GLTF_COMP_UINT -> u32le(data_ptr, offset)
      GLTF_COMP_UBYTE -> load8(data_ptr, offset)
      _ -> 0
   }
}

@jit
fn _gltf_get_index_fast(any: data, int: base_off, int: idx, int: comp_type): int {
   def off = base_off + idx * _gltf_comp_size(comp_type)
   case comp_type {
      GLTF_COMP_USHORT -> u16le(data, off)
      GLTF_COMP_UINT -> u32le(data, off)
      GLTF_COMP_UBYTE -> load8(data, off)
      _ -> 0
   }
}

fn _gltf_node_local_mats(any: g): list {
   _gltf_ensure_caches()
   if(!is_dict(g)){ return [] }
   def nodes = g.get("nodes", 0)
   if(!is_list(nodes)){ return [] }
   def nodes_n = nodes.len
   def key = _gltf_cache_key_from_g(g) + ":nodes:" + to_str(nodes_n)
   def cached = _gltf_node_local_mats_cache.get(key, 0)
   if(is_list(cached) && cached.len == nodes_n){ return cached }
   mut mats = list(nodes_n)
   mut i = 0
   while(i < nodes_n){
      mats = mats.append(gltf_math.node_local_matrix(nodes[i]))
      i += 1
   }
   _gltf_node_local_mats_cache = cache.cache_put_reset(
      _gltf_node_local_mats_cache,
      key,
      mats,
      _GLTF_CACHE_LIMIT_SMALL,
      32
   )
   mats
}

fn _gltf_collect_mesh_nodes(any: g, int: node_idx, list: parent_m, int: mesh_idx, list: out): list {
   def nodes = g.get("nodes")
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return out }
   def node = nodes.get(node_idx)
   def world_m = gltf_math.mat4_mul(parent_m, gltf_math.node_local_matrix(node))
   if(node.get("mesh", -1) == mesh_idx){ out = out.append(world_m) }
   def children = node.get("children")
   if(is_list(children)){
      def children_n = children.len
      mut i = 0
      while(i < children_n){
         out = _gltf_collect_mesh_nodes(g, int(children[i]), world_m, mesh_idx, out)
         i += 1
      }
   }
   out
}

fn _gltf_read_mat4_accessor_value(any: res, int: idx): list {
   if(!is_dict(res)){ return gltf_math.mat4_identity() }
   def ptr = res.get("ptr", 0)
   if(!ptr){ return gltf_math.mat4_identity() }
   def stride = int(res.get("stride", 64))
   def off = idx * stride
   [
      load32_f32(ptr, off + 0), load32_f32(ptr, off + 4), load32_f32(ptr, off + 8), load32_f32(ptr, off + 12),
      load32_f32(ptr, off + 16), load32_f32(ptr, off + 20), load32_f32(ptr, off + 24), load32_f32(ptr, off + 28),
      load32_f32(ptr, off + 32), load32_f32(ptr, off + 36), load32_f32(ptr, off + 40), load32_f32(ptr, off + 44),
      load32_f32(ptr, off + 48), load32_f32(ptr, off + 52), load32_f32(ptr, off + 56), load32_f32(ptr, off + 60),
      "mat4", 400
   ]
}

fn _gltf_mat4_transpose(any: m): list {
   if(!is_list(m) || m.len < 16){ return gltf_math.mat4_identity() }
   [
      m.get(0, 1.0), m.get(4, 0.0), m.get(8, 0.0), m.get(12, 0.0),
      m.get(1, 0.0), m.get(5, 1.0), m.get(9, 0.0), m.get(13, 0.0),
      m.get(2, 0.0), m.get(6, 0.0), m.get(10, 1.0), m.get(14, 0.0),
      m.get(3, 0.0), m.get(7, 0.0), m.get(11, 0.0), m.get(15, 1.0),
      "mat4", 400
   ]
}

fn _gltf_pack_skin_sidecars(dict: g, any: data, int: joints_acc_idx, int: weights_acc_idx, int: count): any {
   if(count <= 0){ return 0 }
   def joints_res = _gltf_resolve_accessor_data(g, joints_acc_idx, data)
   def weights_res = _gltf_resolve_accessor_data(g, weights_acc_idx, data)
   if(!is_dict(joints_res) || !is_dict(weights_res)){
      _gltf_release_accessor_data(joints_res)
      _gltf_release_accessor_data(weights_res)
      return 0
   }
   def joints_cnt = int(joints_res.get("count", 0))
   def weights_cnt = int(weights_res.get("count", 0))
   def source_limit = min(joints_cnt, weights_cnt)
   def joints_ptr = joints_res.get("ptr", 0)
   def weights_ptr = weights_res.get("ptr", 0)
   def joints_comp = int(joints_res.get("comp", 0))
   def weights_comp = int(weights_res.get("comp", 0))
   def joints_stride = int(joints_res.get("stride", 0))
   def weights_stride = int(weights_res.get("stride", 0))
   def weights_norm = weights_res.get("normalized", false)
   def use_count = min(count, source_limit)
   if(!joints_ptr || !weights_ptr || use_count <= 0 || source_limit <= 0){
      if(_gltf_is_debug()){
         print("[gltf] skin sidecar fail ptrs/size joints=" + to_str(joints_ptr != 0) +
            " weights=" + to_str(weights_ptr != 0) +
            " use_count=" + to_str(use_count) +
            " source_limit=" + to_str(source_limit) +
            " joints_cnt=" + to_str(joints_cnt) +
         " weights_cnt=" + to_str(weights_cnt))
      }
      _gltf_release_accessor_data(joints_res)
      _gltf_release_accessor_data(weights_res)
      return 0
   }
   def joints_sidecar = malloc(use_count * 16)
   def weights_sidecar = malloc(use_count * 16)
   if(!joints_sidecar || !weights_sidecar){
      if(_gltf_is_debug()){
         print("[gltf] skin sidecar fail alloc joints=" + to_str(joints_sidecar != 0) +
            " weights=" + to_str(weights_sidecar != 0) +
         " use_count=" + to_str(use_count))
      }
      if(joints_sidecar){ free(joints_sidecar) }
      if(weights_sidecar){ free(weights_sidecar) }
      _gltf_release_accessor_data(joints_res)
      _gltf_release_accessor_data(weights_res)
      return 0
   }
   memset(joints_sidecar, 0, use_count * 16)
   memset(weights_sidecar, 0, use_count * 16)
   mut vi = 0
   while(vi < use_count){
      def joff, woff = vi * joints_stride, vi * weights_stride
      mut k = 0
      while(k < 4){
         mut jv = 0
         if(joints_comp == GLTF_COMP_UBYTE){ jv = load8(joints_ptr, joff + k) }
         elif(joints_comp == GLTF_COMP_USHORT){ jv = u16le(joints_ptr, joff + k * 2) }
         elif(joints_comp == GLTF_COMP_UINT){ jv = u32le(joints_ptr, joff + k * 4) }
         elif(joints_comp == GLTF_COMP_BYTE){
            def raw = int(load8(joints_ptr, joff + k))
            jv = raw >= 128 ? raw - 256 : raw
         } else {
            jv = 0
         }
         store32(joints_sidecar, int(jv), vi * 16 + k * 4)
         def wv = _gltf_read_f32_acc(
            weights_ptr,
            woff + k * max(1, _gltf_comp_size(weights_comp)),
            weights_comp,
            weights_norm
         )
         store32_f32(weights_sidecar, wv, vi * 16 + k * 4)
         k += 1
      }
      vi += 1
   }
   _gltf_release_accessor_data(joints_res)
   _gltf_release_accessor_data(weights_res)
   return {"joints_ptr": joints_sidecar, "weights_ptr": weights_sidecar, "count": use_count}
}

fn gltf_skin_joint_mats(any: gltf_data, int: skin_idx, dict: node_world_mats, any: mesh_node_world=0): list {
   "Builds mesh-local skin joint matrices for the given skin."
   def skin = gltf_skin_info(gltf_data, skin_idx)
   if(!is_dict(skin)){ return [] }
   def joints = skin.get("joints", [])
   if(!is_list(joints) || joints.len == 0){ return [] }
   def joints_n = joints.len
   def inv_bind_mats = _gltf_skin_inv_bind_mats(gltf_data, skin_idx)
   def skin_no_mesh_inv = common.env_truthy("NY_GLTF_SKIN_NO_MESH_INV")
   def skin_transpose_inv_bind = common.env_truthy("NY_GLTF_SKIN_TRANSPOSE_INV_BIND")
   def skin_invbind_first = common.env_truthy("NY_GLTF_SKIN_INVBIND_FIRST")
   def skin_trace = common.env_truthy("NY_GLTF_SKIN_TRACE")
   def mesh_inv = skin_no_mesh_inv ? gltf_math.mat4_identity() : _gltf_mesh_inv_cached(mesh_node_world)
   mut out = list(0)
   mut ji = 0
   while(ji < joints_n){
      def joint_idx = int(joints.get(ji, -1))
      def joint_world = node_world_mats.get(joint_idx, gltf_math.mat4_identity())
      mut inv_bind = inv_bind_mats.get(ji, gltf_math.mat4_identity())
      if(skin_transpose_inv_bind){ inv_bind = _gltf_mat4_transpose(inv_bind) }
      mut jm = skin_invbind_first ? gltf_math.mat4_mul(inv_bind, joint_world) : gltf_math.mat4_mul(joint_world, inv_bind)
      jm = gltf_math.mat4_mul(mesh_inv, jm)
      if(skin_trace && _gltf_skin_trace_printed < 12 && (ji == 0 || ji == 1 || ji == 2 || ji == 6)){
         print("[gltf:skinmat] skin=" + to_str(skin_idx) +
            " ji=" + to_str(ji) +
            " node=" + to_str(joint_idx) +
            " mesh_t=[" + to_str(mesh_node_world.get(12, 0.0)) + "," + to_str(mesh_node_world.get(13, 0.0)) + "," + to_str(mesh_node_world.get(14, 0.0)) + "]" +
            " jw_t=[" + to_str(joint_world.get(12, 0.0)) + "," + to_str(joint_world.get(13, 0.0)) + "," + to_str(joint_world.get(14, 0.0)) + "]" +
            " ib_t=[" + to_str(inv_bind.get(12, 0.0)) + "," + to_str(inv_bind.get(13, 0.0)) + "," + to_str(inv_bind.get(14, 0.0)) + "]" +
         " jm_t=[" + to_str(jm.get(12, 0.0)) + "," + to_str(jm.get(13, 0.0)) + "," + to_str(jm.get(14, 0.0)) + "]")
      }
      out = out.append(jm)
      ji += 1
   }
   out
}

fn _gltf_skin_inv_bind_mats(any: gltf_data, int: skin_idx): list {
   _gltf_ensure_caches()
   def skin = gltf_skin_info(gltf_data, skin_idx)
   if(!is_dict(skin)){ return [] }
   def joints = skin.get("joints", [])
   def joints_n = is_list(joints) ? joints.len : 0
   if(joints_n <= 0){ return [] }
   def g = gltf_data.get("gltf", 0)
   def data = _gltf_primary_data_ptr(gltf_data)
   def inv_bind_acc = int(skin.get("inverse_bind_accessor", -1))
   def source_key = to_str(gltf_data.get("source_path", "")) + "|" + to_str(gltf_data.get("base_path", ""))
   def key = source_key + ":" + to_str(to_int(data)) + ":" + to_str(int(skin_idx)) + ":" + to_str(inv_bind_acc) + ":" + to_str(joints_n)
   def cached = _gltf_skin_inv_bind_cache.get(key, 0)
   if(is_list(cached) && cached.len == joints_n){ return cached }
   def inv_bind_res = _gltf_resolve_accessor_data(g, inv_bind_acc, data)
   def skin_trace = common.env_truthy("NY_GLTF_SKIN_TRACE")
   if(skin_trace && _gltf_skin_trace_printed < 12){
      print("[gltf:skinib] skin=" + to_str(skin_idx) +
         " acc=" + to_str(inv_bind_acc) +
         " count=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("count", 0)) : -1) +
         " stride=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("stride", -1)) : -1) +
         " elem=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("elem_size", -1)) : -1) +
         " type_count=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("type_count", -1)) : -1) +
         " cols=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("cols", -1)) : -1) +
         " rows=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("rows", -1)) : -1) +
         " comp=" + to_str(is_dict(inv_bind_res) ? int(inv_bind_res.get("comp", -1)) : -1) +
      " type=" + to_str(is_dict(inv_bind_res) ? inv_bind_res.get("type_str", "") : ""))
   }
   mut mats = list(joints_n)
   mut ji = 0
   while(ji < joints_n){
      if(is_dict(inv_bind_res) && ji < int(inv_bind_res.get("count", 0))){ mats = mats.append(_gltf_read_mat4_accessor_value(inv_bind_res, ji)) } else { mats = mats.append(gltf_math.mat4_identity()) }
      ji += 1
   }
   _gltf_release_accessor_data(inv_bind_res)
   _gltf_skin_inv_bind_cache = cache.cache_put_reset(_gltf_skin_inv_bind_cache, key, mats, _GLTF_CACHE_LIMIT_SMALL, 32)
   mats
}

fn _gltf_mesh_inv_key(any: mesh_node_world): str {
   mut key = "mesh_inv"
   mut i = 0
   while(i < 16){
      key = key + ":" + to_str(float(mesh_node_world.get(i, (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0 : 0.0)))
      i += 1
   }
   key
}

fn _gltf_mesh_inv_cached(any: mesh_node_world): list {
   _gltf_ensure_caches()
   if(!is_list(mesh_node_world) || mesh_node_world.len < 16){ return gltf_math.mat4_identity() }
   def key = _gltf_mesh_inv_key(mesh_node_world)
   def cached = _gltf_mesh_inv_cache.get(key, 0)
   if(is_list(cached)){ return cached }
   def inv = gltf_math.mat4_inverse_affine(mesh_node_world)
   _gltf_mesh_inv_cache = cache.cache_put_reset(_gltf_mesh_inv_cache, key, inv, _GLTF_CACHE_LIMIT_SMALL, 32)
   inv
}

fn _gltf_pack_skin_mat_slab(any: skin_mats): any {
   if(!is_list(skin_mats)){ return 0 }
   def count = skin_mats.len
   if(count <= 0){ return 0 }
   def slab = malloc(count * 64)
   if(!slab){ return 0 }
   if(!_gltf_write_skin_mat_slab(skin_mats, slab)){
      free(slab)
      return 0
   }
   slab
}

fn _gltf_write_skin_mat_slab(any: skin_mats, any: slab): bool {
   if(!is_list(skin_mats) || !slab){ return false }
   def count = skin_mats.len
   mut i = 0
   while(i < count){
      def m = skin_mats[i]
      def base = slab + i * 64
      if(is_list(m)){
         mut j = 0
         while(j < 16){
            store32_f32(base, float(m.get(j, (j == 0 || j == 5 || j == 10 || j == 15) ? 1.0 : 0.0)), j * 4)
            j += 1
         }
      } else {
         memset(base, 0, 64)
         store32_f32(base, 1.0, 0)
         store32_f32(base, 1.0, 20)
         store32_f32(base, 1.0, 40)
         store32_f32(base, 1.0, 60)
      }
      i += 1
   }
   true
}

fn _gltf_skin_mats_cache_record(any: gltf_data, int: skin_idx, dict: node_world_mats, any: mesh_bind_world, any: skin_mats_cache): any {
   def cache_key = to_str(skin_idx) + ":" + to_str(to_int(mesh_bind_world))
   if(is_dict(skin_mats_cache)){
      def cached = skin_mats_cache.get(cache_key, 0)
      if(is_list(cached)){ return cached }
   }
   def skin_raw_off = common.env_truthy("NY_GLTF_SKIN_RAW_OFF")
   def skin_no_mesh_inv = common.env_truthy("NY_GLTF_SKIN_NO_MESH_INV")
   def skin_transpose_inv_bind = common.env_truthy("NY_GLTF_SKIN_TRANSPOSE_INV_BIND")
   def skin_invbind_first = common.env_truthy("NY_GLTF_SKIN_INVBIND_FIRST")
   if(!skin_raw_off
      && !skin_no_mesh_inv
      && !skin_transpose_inv_bind
      && !skin_invbind_first){
      def runtime_skin = gltf_skin_info(gltf_data, skin_idx)
      def runtime_joints = is_dict(runtime_skin) ? runtime_skin.get("joints", []) : []
      def runtime_count = is_list(runtime_joints) ? runtime_joints.len : 0
      if(runtime_count > 0){
         def runtime_inv_bind = _gltf_skin_inv_bind_mats(gltf_data, skin_idx)
         if(is_list(runtime_inv_bind) && runtime_inv_bind.len >= runtime_count){
            def runtime_mesh_inv = _gltf_mesh_inv_cached(mesh_bind_world)
            def slab = malloc(runtime_count * 64)
            if(slab){
               def runtime_world_list = node_world_mats.get("__world_list", 0)
               if(is_list(runtime_world_list)){
                  __gltf_skin_mats_store_raw(
                     slab,
                     runtime_joints,
                     runtime_world_list,
                     runtime_inv_bind,
                     runtime_mesh_inv,
                     runtime_count
                  )
               } else {
                  mut rji = 0
                  while(rji < runtime_count){
                     def joint_idx = int(runtime_joints.get(rji, -1))
                     def joint_world = node_world_mats.get(joint_idx, gltf_math.mat4_identity())
                     def inv_bind = runtime_inv_bind.get(rji, gltf_math.mat4_identity())
                     __gltf_skin_mat_store_raw(slab, rji, joint_world, inv_bind, runtime_mesh_inv)
                     rji += 1
                  }
               }
               def rec = [0, slab, runtime_count]
               if(is_dict(skin_mats_cache)){
                  skin_mats_cache[cache_key] = rec
                  mut keys = skin_mats_cache.get("__keys", [])
                  keys = keys.append(cache_key)
                  skin_mats_cache["__keys"] = keys
               }
               return rec
            }
         }
      }
   }
   def skin_mats = gltf_skin_joint_mats(gltf_data, skin_idx, node_world_mats, mesh_bind_world)
   if(!is_list(skin_mats) || skin_mats.len == 0){ return 0 }
   def slab = _gltf_pack_skin_mat_slab(skin_mats)
   def rec = [skin_mats, slab, skin_mats.len]
   if(is_dict(skin_mats_cache)){
      skin_mats_cache[cache_key] = rec
      mut keys = skin_mats_cache.get("__keys", [])
      keys = keys.append(cache_key)
      skin_mats_cache["__keys"] = keys
   }
   rec
}

fn gltf_free_skin_mats_cache(any: skin_mats_cache): bool {
   if(!is_dict(skin_mats_cache)){ return false }
   def keys = skin_mats_cache.get("__keys", [])
   if(!is_list(keys)){ return false }
   def keys_n = keys.len
   mut i = 0
   while(i < keys_n){
      def key = keys[i]
      def rec = skin_mats_cache.get(key, 0)
      if(is_list(rec)){
         def slab = rec.get(1, 0)
         if(slab){ free(slab) }
      }
      i += 1
   }
   skin_mats_cache["__keys"] = []
   true
}

@jit
fn _gltf_apply_skinning_slab(any: vptr, any: bind_vptr, any: joints_ptr, any: weights_ptr, int: vcnt, any: skin_slab, int: mat_count): bool {
   if(!vptr || !bind_vptr || !joints_ptr || !weights_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0){ return false }
   __gltf_skin_apply_raw(vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, mat_count)
}

@jit
fn _gltf_apply_skinning_one_slab(any: vptr, any: bind_vptr, any: joints_ptr, int: vcnt, any: skin_slab, int: mat_count): bool {
   if(!vptr || !bind_vptr || !joints_ptr || !skin_slab || vcnt <= 0 || mat_count <= 0){ return false }
   __gltf_skin_apply_one_raw(vptr, bind_vptr, joints_ptr, vcnt, skin_slab, mat_count)
}

fn _gltf_apply_part_skin_slab(dict: part, any: vptr, any: bind_vptr, any: joints_ptr, any: weights_ptr, int: vcnt, any: skin_slab, int: mat_count): bool {
   if(part.get("skin_single_influence", false)){ return _gltf_apply_skinning_one_slab(vptr, bind_vptr, joints_ptr, vcnt, skin_slab, mat_count) }
   _gltf_apply_skinning_slab(vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, mat_count)
}

fn _gltf_part_runtime_skin_slab(dict: part, any: gltf_data, dict: node_world_mats, int: skin_idx, any: mesh_bind_world): any {
   def runtime_skin = gltf_skin_info(gltf_data, skin_idx)
   def runtime_joints = is_dict(runtime_skin) ? runtime_skin.get("joints", []) : []
   def runtime_count = is_list(runtime_joints) ? runtime_joints.len : 0
   if(runtime_count <= 0){ return 0 }
   def runtime_inv_bind = _gltf_skin_inv_bind_mats(gltf_data, skin_idx)
   if(!is_list(runtime_inv_bind) || runtime_inv_bind.len < runtime_count){ return 0 }
   def runtime_mesh_inv = _gltf_mesh_inv_cached(mesh_bind_world)
   mut runtime_slab = part.get("skin_runtime_slab", 0)
   mut runtime_slab_count = int(part.get("skin_runtime_slab_count", 0))
   if(!runtime_slab || runtime_slab_count < runtime_count){
      if(runtime_slab){ free(runtime_slab) }
      runtime_slab = malloc(runtime_count * 64)
      if(!runtime_slab){ return 0 }
      runtime_slab_count = runtime_count
      part["skin_runtime_slab"] = runtime_slab
      part["skin_runtime_slab_count"] = runtime_slab_count
   }
   def runtime_world_list = node_world_mats.get("__world_list", 0)
   if(is_list(runtime_world_list)){
      __gltf_skin_mats_store_raw(
         runtime_slab,
         runtime_joints,
         runtime_world_list,
         runtime_inv_bind,
         runtime_mesh_inv,
         runtime_count
      )
   } else {
      mut rji = 0
      while(rji < runtime_count){
         def joint_idx = int(runtime_joints.get(rji, -1))
         def joint_world = node_world_mats.get(joint_idx, gltf_math.mat4_identity())
         def inv_bind = runtime_inv_bind.get(rji, gltf_math.mat4_identity())
         __gltf_skin_mat_store_raw(runtime_slab, rji, joint_world, inv_bind, runtime_mesh_inv)
         rji += 1
      }
   }
   [runtime_slab, runtime_count]
}

fn _gltf_skin_weighted_mat4_vec3(
   any: jm0, any: jm1, any: jm2, any: jm3,
   f64: ew0, f64: ew1, f64: ew2, f64: ew3, f64: inv_w,
   f64: x, f64: y, f64: z, bool: translate
): list {
   def x0 = (0.0 + jm0.get(0, 1.0)) * x + (0.0 + jm0.get(4, 0.0)) * y + (0.0 + jm0.get(8, 0.0)) * z + (translate ? (0.0 + jm0.get(12, 0.0)) : 0.0)
   def y0 = (0.0 + jm0.get(1, 0.0)) * x + (0.0 + jm0.get(5, 1.0)) * y + (0.0 + jm0.get(9, 0.0)) * z + (translate ? (0.0 + jm0.get(13, 0.0)) : 0.0)
   def z0 = (0.0 + jm0.get(2, 0.0)) * x + (0.0 + jm0.get(6, 0.0)) * y + (0.0 + jm0.get(10, 1.0)) * z + (translate ? (0.0 + jm0.get(14, 0.0)) : 0.0)
   def x1 = (0.0 + jm1.get(0, 1.0)) * x + (0.0 + jm1.get(4, 0.0)) * y + (0.0 + jm1.get(8, 0.0)) * z + (translate ? (0.0 + jm1.get(12, 0.0)) : 0.0)
   def y1 = (0.0 + jm1.get(1, 0.0)) * x + (0.0 + jm1.get(5, 1.0)) * y + (0.0 + jm1.get(9, 0.0)) * z + (translate ? (0.0 + jm1.get(13, 0.0)) : 0.0)
   def z1 = (0.0 + jm1.get(2, 0.0)) * x + (0.0 + jm1.get(6, 0.0)) * y + (0.0 + jm1.get(10, 1.0)) * z + (translate ? (0.0 + jm1.get(14, 0.0)) : 0.0)
   def x2 = (0.0 + jm2.get(0, 1.0)) * x + (0.0 + jm2.get(4, 0.0)) * y + (0.0 + jm2.get(8, 0.0)) * z + (translate ? (0.0 + jm2.get(12, 0.0)) : 0.0)
   def y2 = (0.0 + jm2.get(1, 0.0)) * x + (0.0 + jm2.get(5, 1.0)) * y + (0.0 + jm2.get(9, 0.0)) * z + (translate ? (0.0 + jm2.get(13, 0.0)) : 0.0)
   def z2 = (0.0 + jm2.get(2, 0.0)) * x + (0.0 + jm2.get(6, 0.0)) * y + (0.0 + jm2.get(10, 1.0)) * z + (translate ? (0.0 + jm2.get(14, 0.0)) : 0.0)
   def x3 = (0.0 + jm3.get(0, 1.0)) * x + (0.0 + jm3.get(4, 0.0)) * y + (0.0 + jm3.get(8, 0.0)) * z + (translate ? (0.0 + jm3.get(12, 0.0)) : 0.0)
   def y3 = (0.0 + jm3.get(1, 0.0)) * x + (0.0 + jm3.get(5, 1.0)) * y + (0.0 + jm3.get(9, 0.0)) * z + (translate ? (0.0 + jm3.get(13, 0.0)) : 0.0)
   def z3 = (0.0 + jm3.get(2, 0.0)) * x + (0.0 + jm3.get(6, 0.0)) * y + (0.0 + jm3.get(10, 1.0)) * z + (translate ? (0.0 + jm3.get(14, 0.0)) : 0.0)
   [
      (x0 * ew0 + x1 * ew1 + x2 * ew2 + x3 * ew3) * inv_w,
      (y0 * ew0 + y1 * ew1 + y2 * ew2 + y3 * ew3) * inv_w,
      (z0 * ew0 + z1 * ew1 + z2 * ew2 + z3 * ew3) * inv_w
   ]
}

fn _gltf_apply_skinning_fallback(
   dict: part, int: skin_idx, int: node_idx, list: skin_mats, bool: skin_trace,
   any: bind_vptr, any: joints_ptr, any: weights_ptr, any: vptr, int: vcnt
): any {
   memcpy(vptr, bind_vptr, vcnt * _GLTF_VTX_STRIDE)
   mut vi = 0
   while(vi < vcnt){
      def boff = vi * _GLTF_VTX_STRIDE
      def px = load32_f32(bind_vptr, boff + _GLTF_VTX_OFF_X)
      def py = load32_f32(bind_vptr, boff + _GLTF_VTX_OFF_Y)
      def pz = load32_f32(bind_vptr, boff + _GLTF_VTX_OFF_Z)
      def nx0 = load32_f32(bind_vptr, boff + _GLTF_VTX_OFF_NX)
      def ny0 = load32_f32(bind_vptr, boff + _GLTF_VTX_OFF_NY)
      def nz0 = load32_f32(bind_vptr, boff + _GLTF_VTX_OFF_NZ)
      def has_norm = (nx0 * nx0 + ny0 * ny0 + nz0 * nz0) > 0.000001
      def side_off = vi * 16
      def j0 = int(load32(joints_ptr, side_off + 0))
      def j1 = int(load32(joints_ptr, side_off + 4))
      def j2 = int(load32(joints_ptr, side_off + 8))
      def j3 = int(load32(joints_ptr, side_off + 12))
      def w0 = load32_f32(weights_ptr, side_off + 0)
      def w1 = load32_f32(weights_ptr, side_off + 4)
      def w2 = load32_f32(weights_ptr, side_off + 8)
      def w3 = load32_f32(weights_ptr, side_off + 12)
      def use0 = w0 > 0.000001 && j0 >= 0 && j0 < skin_mats.len
      def use1 = w1 > 0.000001 && j1 >= 0 && j1 < skin_mats.len
      def use2 = w2 > 0.000001 && j2 >= 0 && j2 < skin_mats.len
      def use3 = w3 > 0.000001 && j3 >= 0 && j3 < skin_mats.len
      def ew0 = use0 ? w0 : 0.0
      def ew1 = use1 ? w1 : 0.0
      def ew2 = use2 ? w2 : 0.0
      def ew3 = use3 ? w3 : 0.0
      def jm0 = use0 ? skin_mats.get(j0, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def jm1 = use1 ? skin_mats.get(j1, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def jm2 = use2 ? skin_mats.get(j2, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def jm3 = use3 ? skin_mats.get(j3, gltf_math.mat4_identity()) : gltf_math.mat4_identity()
      def wsum = ew0 + ew1 + ew2 + ew3
      if(wsum > 0.000001){
         def inv_w = 1.0 / wsum
         def skin_pos = _gltf_skin_weighted_mat4_vec3(jm0, jm1, jm2, jm3, ew0, ew1, ew2, ew3, inv_w, px, py, pz, true)
         def sx, sy = float(skin_pos.get(0, px)), float(skin_pos.get(1, py))
         def sz = float(skin_pos.get(2, pz))
         if(vi == 0 && skin_trace && _gltf_skin_trace_printed < 12){
            print("[gltf:skin] skin=" + to_str(skin_idx) +
               " node=" + to_str(node_idx) +
               " vcnt=" + to_str(vcnt) +
               " p=[" + to_str(px) + "," + to_str(py) + "," + to_str(pz) + "]" +
               " j=[" + to_str(j0) + "," + to_str(j1) + "," + to_str(j2) + "," + to_str(j3) + "]" +
               " w=[" + to_str(w0) + "," + to_str(w1) + "," + to_str(w2) + "," + to_str(w3) + "]" +
               " out=[" + to_str(sx) + "," + to_str(sy) + "," + to_str(sz) + "]" +
               " jm0t=[" + to_str(jm0.get(12, 0.0)) + "," + to_str(jm0.get(13, 0.0)) + "," + to_str(jm0.get(14, 0.0)) + "]" +
            " jm1t=[" + to_str(jm1.get(12, 0.0)) + "," + to_str(jm1.get(13, 0.0)) + "," + to_str(jm1.get(14, 0.0)) + "]")
            _gltf_skin_trace_printed += 1
         }
         if(_gltf_float_bad(sx) || _gltf_float_bad(sy) || _gltf_float_bad(sz)){
            store32_f32(vptr, px, boff + _GLTF_VTX_OFF_X)
            store32_f32(vptr, py, boff + _GLTF_VTX_OFF_Y)
            store32_f32(vptr, pz, boff + _GLTF_VTX_OFF_Z)
         } else {
            store32_f32(vptr, sx, boff + _GLTF_VTX_OFF_X)
            store32_f32(vptr, sy, boff + _GLTF_VTX_OFF_Y)
            store32_f32(vptr, sz, boff + _GLTF_VTX_OFF_Z)
         }
         if(has_norm){
            def skin_norm = _gltf_skin_weighted_mat4_vec3(jm0, jm1, jm2, jm3, ew0, ew1, ew2, ew3, inv_w, nx0, ny0, nz0, false)
            def nnx, nny = float(skin_norm.get(0, nx0)), float(skin_norm.get(1, ny0))
            def nnz = float(skin_norm.get(2, nz0))
            def nl = sqrt(nnx * nnx + nny * nny + nnz * nnz)
            if(nl > 0.000001 && !_gltf_float_bad(nl)){
               def inv_n = 1.0 / nl
               store32_f32(vptr, nnx * inv_n, boff + _GLTF_VTX_OFF_NX)
               store32_f32(vptr, nny * inv_n, boff + _GLTF_VTX_OFF_NY)
               store32_f32(vptr, nnz * inv_n, boff + _GLTF_VTX_OFF_NZ)
            } else {
               store32_f32(vptr, nx0, boff + _GLTF_VTX_OFF_NX)
               store32_f32(vptr, ny0, boff + _GLTF_VTX_OFF_NY)
               store32_f32(vptr, nz0, boff + _GLTF_VTX_OFF_NZ)
            }
         }
      } else {
         store32_f32(vptr, px, boff + _GLTF_VTX_OFF_X)
         store32_f32(vptr, py, boff + _GLTF_VTX_OFF_Y)
         store32_f32(vptr, pz, boff + _GLTF_VTX_OFF_Z)
         if(has_norm){
            store32_f32(vptr, nx0, boff + _GLTF_VTX_OFF_NX)
            store32_f32(vptr, ny0, boff + _GLTF_VTX_OFF_NY)
            store32_f32(vptr, nz0, boff + _GLTF_VTX_OFF_NZ)
         }
      }
      vi += 1
   }
   part
}

fn gltf_apply_skinning(any: part, any: gltf_data, any: node_world_mats, any: skin_mats_cache=0): any {
   "Applies CPU skinning in-place to a loaded part if it carries skin sidecars."
   if(!is_dict(part) || !is_dict(gltf_data) || !is_dict(node_world_mats)){ return part }
   def skin_idx = int(part.get("skin_idx", -1))
   if(skin_idx < 0){ return part }
   if(common.env_truthy("NY_GLTF_DISABLE_SKINNING")){ return part }
   def skin_raw_off = common.env_truthy("NY_GLTF_SKIN_RAW_OFF")
   def skin_trace = common.env_truthy("NY_GLTF_SKIN_TRACE")
   def bind_vptr = part.get("skin_bind_vptr", 0)
   def joints_ptr = part.get("skin_joints_ptr", 0)
   def weights_ptr = part.get("skin_weights_ptr", 0)
   def vptr = part.get("vptr", 0)
   if(!bind_vptr || !joints_ptr || !weights_ptr || !vptr){ return part }
   def vcnt = int(part.get("skin_vcnt", part.get("vcnt", 0)))
   if(vcnt <= 0){ return part }
   def node_idx = int(part.get("node_idx", -1))
   mut mesh_bind_world = node_idx >= 0 ? node_world_mats.get(node_idx, part.get("model", 0)) : part.get("model", 0)
   if(!is_list(mesh_bind_world)|| mesh_bind_world.len < 16){ mesh_bind_world = part.get("skin_mesh_bind_world", part.get("model", 0)) }
   if(!is_dict(skin_mats_cache)){
      def runtime_rec = _gltf_part_runtime_skin_slab(part, gltf_data, node_world_mats, skin_idx, mesh_bind_world)
      if(!is_list(runtime_rec)){ return part }
      _gltf_apply_part_skin_slab(part, vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, runtime_rec.get(0, 0), int(runtime_rec.get(1, 0)))
      return part
   }
   def skin_rec = _gltf_skin_mats_cache_record(gltf_data, skin_idx, node_world_mats, mesh_bind_world, skin_mats_cache)
   mut skin_mats = is_list(skin_rec) ? skin_rec.get(0, 0) : 0
   def skin_slab = is_list(skin_rec) ? skin_rec.get(1, 0) : 0
   def skin_mat_count = is_list(skin_rec) ? int(skin_rec.get(2, 0)) : 0
   if(skin_slab && skin_mat_count > 0 && !skin_raw_off){
      _gltf_apply_part_skin_slab(part, vptr, bind_vptr, joints_ptr, weights_ptr, vcnt, skin_slab, skin_mat_count)
      return part
   }
   if(!is_list(skin_mats) || skin_mats.len == 0){ return part }
   _gltf_apply_skinning_fallback(part, skin_idx, node_idx, skin_mats, skin_trace, bind_vptr, joints_ptr, weights_ptr, vptr, vcnt)
}

fn _gltf_mesh_instance_mats(any: g, int: mesh_idx): list {
   mut out = list(0)
   def scenes = g.get("scenes")
   def scene_idx = _gltf_active_scene_idx(g, 0)
   mut scene_scoped = false
   if(is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len){
      def scene = scenes.get(scene_idx)
      def roots = scene.get("nodes")
      if(is_list(roots)){
         scene_scoped = true
         def roots_n = roots.len
         def id = gltf_math.mat4_identity()
         mut i = 0
         while(i < roots_n){
            out = _gltf_collect_mesh_nodes(g, int(roots[i]), id, mesh_idx, out)
            i += 1
         }
      }
   }
   if(out.len == 0 && !scene_scoped){ out = out.append(gltf_math.mat4_identity()) }
   out
}

fn _gltf_node_visit_key(int: node_idx): str {
   "Dict key used only for recursion guards. Keep it disjoint from integer node indices."
   "__visit_node_" + to_str(int(node_idx))
}

@jit
fn _gltf_build_node_world_mats(any: g, int: node_idx, list: parent_m, dict: node_world_mats): dict {
   def nodes = g.get("nodes")
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return node_world_mats }
   def visit_key = _gltf_node_visit_key(node_idx)
   if(node_world_mats.get(visit_key, false)){ return node_world_mats }
   if(node_world_mats.contains(node_idx)){ return node_world_mats }
   def node = nodes[node_idx]
   if(!is_dict(node)){ return node_world_mats }
   node_world_mats[visit_key] = true
   def local_m = gltf_math.node_local_matrix(node)
   def world_m = gltf_math.mat4_mul(parent_m, local_m)
   node_world_mats[node_idx] = world_m
   def children = node.get("children")
   if(is_list(children)){
      def children_n = children.len
      mut i = 0
      while(i < children_n){
         def child_idx = int(children[i])
         if(child_idx >= 0 && child_idx != node_idx){ node_world_mats = _gltf_build_node_world_mats(g, child_idx, world_m, node_world_mats) }
         i += 1
      }
   }
   node_world_mats = node_world_mats.delete(visit_key)
   node_world_mats
}

fn _gltf_root_relevant_for_mesh_limit(any: g, int: node_idx, int: mesh_limit): bool {
   if(mesh_limit <= 0){ return true }
   def nodes = g.get("nodes", 0)
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return false }
   def node = nodes.get(node_idx, 0)
   if(!is_dict(node)){ return false }
   def mesh_ref = int(node.get("mesh", -1))
   if(mesh_ref >= 0 && mesh_ref < mesh_limit){ return true }
   def children = node.get("children", [])
   if(is_list(children) && children.len > 0){ return true }
   if(mesh_ref >= 0){ return false }
   true
}

fn gltf_scene_punctual_lights(any: gltf_data, any: overrides=0): list {
   "Returns a list of world-space KHR_lights_punctual lights for the active scene.
   When `overrides` are provided, light transforms and visibility follow animated TRS/visibility."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   def exts = g.get("extensions", 0)
   def khr_lights = is_dict(exts) ? exts.get("KHR_lights_punctual", 0) : 0
   def lights = is_dict(khr_lights) ? khr_lights.get("lights", 0) : 0
   def nodes = g.get("nodes", 0)
   if(!is_list(lights) || !is_list(nodes)){ return [] }
   def nodes_n = nodes.len
   def lights_n = lights.len
   mut node_world_mats = dict(max(16, nodes_n * 2))
   def vis_map = gltf_resolve_node_visibility(gltf_data, overrides)
   def scenes = g.get("scenes")
   def scene_idx = _gltf_active_scene_idx(g, gltf_data)
   if(is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len){
      def scene = scenes.get(scene_idx)
      def roots = scene.get("nodes")
      if(is_list(roots)){
         def roots_n = roots.len
         def id = gltf_math.mat4_identity()
         mut ri = 0
         while(ri < roots_n){
            if(is_dict(overrides)){
               node_world_mats = _gltf_build_node_world_mats_animated(
                  g,
                  int(roots[ri]),
                  id,
                  node_world_mats,
                  overrides
               )
            } else {
               node_world_mats = _gltf_build_node_world_mats(g, int(roots[ri]), id, node_world_mats)
            }
            ri += 1
         }
      }
   }
   mut out = []
   mut i = 0
   while(i < nodes_n){
      def node = nodes[i]
      if(!vis_map.get(i, true)){ i += 1 continue }
      def node_ext = is_dict(node) ? node.get("extensions", 0) : 0
      def lref = is_dict(node_ext) ? node_ext.get("KHR_lights_punctual", 0) : 0
      def light_idx = is_dict(lref) ? int(lref.get("light", -1)) : -1
      if(light_idx >= 0 && light_idx < lights_n){
         def light = lights.get(light_idx, 0)
         def world_m = node_world_mats.get(i, gltf_math.mat4_identity())
         def color = light.get("color", [1.0, 1.0, 1.0])
         def intensity = float(light.get("intensity", 1.0))
         def range = float(light.get("range", 0.0))
         def ltype = to_str(light.get("type", "point"))
         def spot = light.get("spot", 0)
         def outer_angle = is_dict(spot) ? float(spot.get("outerConeAngle", 0.785398)) : 0.785398
         def rec = {
            "type": ltype, "position": gltf_math.mat4_transform_point(world_m, [0.0, 0.0, 0.0]),
            "direction": gltf_math.mat4_transform_dir(world_m, [0.0, 0.0, -1.0]),
            "color": [float(color.get(0,1.0)), float(color.get(1,1.0)), float(color.get(2,1.0))],
            "intensity": intensity, "range": range, "outer_cone_cos": cos(outer_angle), "node_idx": i
         }
         out = out.append(rec)
      }
      i += 1
   }
   out
}

fn _gltf_compute_mesh_instance_mats_fast(any: g, any: node_world_mats, int: mesh_limit=0): dict {
   def nodes = g.get("nodes")
   if(!is_list(nodes)){ return dict(0) }
   def nodes_n = nodes.len
   def scene_scoped = is_dict(node_world_mats) && node_world_mats.len > 0
   mut mesh_map = dict(max(16, nodes_n * 2))
   mut node_i = 0
   while(node_i < nodes_n){
      def node = nodes.get(node_i)
      def mesh_ref = int(node.get("mesh", -1))
      if(mesh_limit > 0 && mesh_ref >= mesh_limit){
         node_i += 1
         continue
      }
      if(mesh_ref >= 0){
         mut wm = node_world_mats.get(node_i, 0)
         if(scene_scoped && (!is_list(wm) || wm.len < 16)){
            node_i += 1
            continue
         }
         if(!is_list(wm) || wm.len < 16){ wm = gltf_math.mat4_identity() }
         def node_ext = node.get("extensions", 0)
         def gpu_ins = is_dict(node_ext) ? node_ext.get("EXT_mesh_gpu_instancing", 0) : 0
         if(is_dict(gpu_ins)){
            def attrs = gpu_ins.get("attributes", 0)
            if(is_dict(attrs)){
               def existing = mesh_map.get(mesh_ref, 0)
               def entry = [wm, node_i]
               if(is_list(existing)){ mesh_map[mesh_ref] = existing.append(entry) }
               else { mesh_map[mesh_ref] = [entry] }
            }
         } else {
            def existing = mesh_map.get(mesh_ref, 0)
            def entry = [wm, node_i]
            if(is_list(existing)){ mesh_map[mesh_ref] = existing.append(entry) }
            else { mesh_map[mesh_ref] = [entry] }
         }
      }
      node_i += 1
   }
   mesh_map
}

fn _gltf_node_default_visibility(any: g, int: node_idx): bool {
   def nodes = g.get("nodes", 0)
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return true }
   def node = nodes[node_idx]
   if(!is_dict(node)){ return true }
   def exts = node.get("extensions", 0)
   def nv = is_dict(exts) ? exts.get("KHR_node_visibility", 0) : 0
   if(is_dict(nv) && nv.contains("visible")){
      def v = nv.get("visible", true)
      if(is_int(v)){ return v != 0 }
      if(is_float(v)){ return float(v) >= 0.5 }
      return !!v
   }
   true
}

fn _gltf_pointer_node_visibility_idx(any: pointer): int {
   def p = to_str(pointer)
   if(p.len == 0){ return -1 }
   def parts = str.split(p, "/")
   if(!is_list(parts) || parts.len < 6){ return -1 }
   if(to_str(parts[1]) != "nodes"){ return -1 }
   def node_txt = to_str(parts[2])
   if(node_txt.len == 0){ return -1 }
   mut i = 0
   mut out = 0
   while(i < node_txt.len){
      def ch = load8(node_txt, i)
      if(ch < 48 || ch > 57){ return -1 }
      out = out * 10 + (ch - 48)
      i += 1
   }
   if(to_str(parts[3]) != "extensions"){ return -1 }
   if(to_str(parts[4]) != "KHR_node_visibility"){ return -1 }
   if(to_str(parts[5]) != "visible"){ return -1 }
   out
}

fn _gltf_pointer_material_target(any: pointer): any {
   def p = to_str(pointer)
   if(str.find(p, "/materials/") != 0){ return 0 }
   def parts = str.split(p, "/")
   if(parts.len < 4){ return 0 }
   if(to_str(parts[1]) != "materials"){ return 0 }
   def mat_idx = int(parts[2])
   if(mat_idx < 0){ return 0 }
   if(parts.len >= 5
      && to_str(parts[3]) == "pbrMetallicRoughness"
      && to_str(parts[4]) == "baseColorFactor"){
      return {"material": mat_idx, "kind": "baseColorFactor"}
   }
   if(parts.len >= 5
      && to_str(parts[3]) == "pbrMetallicRoughness"
      && to_str(parts[4]) == "metallicFactor"){
      return {"material": mat_idx, "kind": "metallicFactor"}
   }
   if(parts.len >= 5
      && to_str(parts[3]) == "pbrMetallicRoughness"
      && to_str(parts[4]) == "roughnessFactor"){
      return {"material": mat_idx, "kind": "roughnessFactor"}
   }
   if(to_str(parts[3]) == "emissiveFactor"){ return {"material": mat_idx, "kind": "emissiveFactor"} }
   if(to_str(parts[3]) == "alphaCutoff"){ return {"material": mat_idx, "kind": "alphaCutoff"} }
   mut ktt_idx = -1
   def parts_n = parts.len
   mut pi = 0
   while(pi < parts_n){
      if(to_str(parts[pi]) == "KHR_texture_transform"){ ktt_idx = pi }
      pi += 1
   }
   if(ktt_idx >= 5 && ktt_idx + 1 < parts_n){
      def slot = to_str(parts[ktt_idx - 2])
      def op = to_str(parts[ktt_idx + 1])
      if(slot != "" && (op == "offset" || op == "scale" || op == "rotation")){
         def kind = (op == "offset") ? "uvOffset" : ((op == "scale") ? "uvScale" : "uvRotation")
         return {"material": mat_idx, "kind": kind, "slot": slot}
      }
   }
   0
}

fn _gltf_anim_override_for_node(any: overrides, int: node_idx): any {
   if(!is_dict(overrides)){ return 0 }
   def ov_num = overrides.get(int(node_idx), 0)
   if(is_dict(ov_num)){ return ov_num }
   def ov_str = overrides.get(to_str(node_idx), 0)
   if(is_dict(ov_str)){ return ov_str }
   def ovs = overrides.get("__nodes", 0)
   if(is_list(ovs)){
      def ovs_n = ovs.len
      mut oi = 0
      while(oi < ovs_n){
         def rec = ovs[oi]
         if(is_dict(rec) && int(rec.get("node", -1)) == int(node_idx)){ return rec }
         oi += 1
      }
   }
   0
}

fn _gltf_node_local_visibility(any: g, int: node_idx, any: overrides=0): bool {
   mut vis = _gltf_node_default_visibility(g, node_idx)
   def ov = _gltf_anim_override_for_node(overrides, node_idx)
   if(is_dict(ov) && ov.contains("VIS")){
      def vv = ov.get("VIS", true)
      if(is_int(vv)){ vis = vv != 0 }
      elif(is_float(vv)){ vis = float(vv) >= 0.5 }
      else { vis = !!vv }
   }
   vis
}

fn _gltf_build_node_visibility_map(any: g, int: node_idx, bool: parent_visible, dict: vis_map, any: overrides=0): dict {
   def nodes = g.get("nodes", 0)
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return vis_map }
   def local_vis = _gltf_node_local_visibility(g, node_idx, overrides)
   def eff_vis = parent_visible && local_vis
   vis_map[node_idx] = eff_vis
   def node = nodes[node_idx]
   def children = is_dict(node) ? node.get("children", 0) : 0
   if(is_list(children)){
      def children_n = children.len
      mut ci = 0
      while(ci < children_n){
         vis_map = _gltf_build_node_visibility_map(g, int(children[ci]), eff_vis, vis_map, overrides)
         ci += 1
      }
   }
   vis_map
}

fn gltf_has_node_visibility(any: gltf_data, any: overrides=0): bool {
   "Returns true when visibility defaults or animation overrides can affect nodes."
   _gltf_ensure_caches()
   if(is_dict(overrides)){
      def ov_nodes = overrides.get("__nodes", 0)
      if(is_list(ov_nodes)){
         mut oi = 0
         def ov_n = ov_nodes.len
         while(oi < ov_n){
            def rec = ov_nodes[oi]
            if(is_dict(rec) && rec.contains("VIS")){ return true }
            oi += 1
         }
      }
   }
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return false }
   def key = _gltf_cache_key_from_data(gltf_data) + ":visibility"
   if(_gltf_visibility_flag_cache.contains(key)){ return _gltf_visibility_flag_cache.get(key, false) ? true : false }
   def nodes = g.get("nodes", 0)
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut ni = 0
   while(ni < nodes_n){
      def node = nodes[ni]
      def exts = is_dict(node) ? node.get("extensions", 0) : 0
      if(is_dict(exts) && is_dict(exts.get("KHR_node_visibility", 0))){
         _gltf_visibility_flag_cache = cache.cache_put_reset(_gltf_visibility_flag_cache,
            key,
            true,
            _GLTF_CACHE_LIMIT_MED,
         64)
         return true
      }
      ni += 1
   }
   _gltf_visibility_flag_cache = cache.cache_put_reset(_gltf_visibility_flag_cache,
      key,
      false,
      _GLTF_CACHE_LIMIT_MED,
   64)
   false
}

fn gltf_resolve_node_visibility(any: gltf_data, any: overrides=0): dict {
   "Returns {node_idx: bool} effective visibility map for active scene roots."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return dict(0) }
   def nodes = g.get("nodes", 0)
   mut vis_map = dict(is_list(nodes) ? max(16, nodes.len * 2) : 16)
   def scenes = g.get("scenes", 0)
   def scene_idx = _gltf_active_scene_idx(g, gltf_data)
   if(is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len){
      def scene = scenes[scene_idx]
      def roots = is_dict(scene) ? scene.get("nodes", 0) : 0
      if(is_list(roots)){
         def roots_n = roots.len
         mut ri = 0
         while(ri < roots_n){
            vis_map = _gltf_build_node_visibility_map(g, int(roots[ri]), true, vis_map, overrides)
            ri += 1
         }
      }
   }
   vis_map
}

fn gltf_free_data(any: gltf_data): bool {
   "Frees binary data buffers held by a gltf_data dict. Call after GPU mesh upload."
   _gltf_ensure_caches()
   if(!is_dict(gltf_data)){ return false }
   _gltf_acc_res_cache = dict(256)
   _gltf_anim_sample_cache = dict(64)
   def buffer_data = gltf_data.get("buffer_data", 0)
   if(is_list(buffer_data)){
      def buffer_data_n = buffer_data.len
      mut bi = 0
      while(bi < buffer_data_n){
         def b = buffer_data[bi]
         if(is_dict(b)){
            def ptr = b.get("ptr", 0)
            def kind = to_str(b.get("kind", ""))
            if((kind == "str" || kind == "buf") && ptr){ free(ptr) }
         } elif(is_str(b)){
            free(b)
         }
         bi += 1
      }
      return true
   }
   def binary_data = gltf_data.get("binary_data", 0)
   if(is_dict(binary_data)){
      def ptr = binary_data.get("ptr", 0)
      def kind = to_str(binary_data.get("kind", ""))
      if((kind == "str" || kind == "buf") && ptr){ free(ptr) }
   } elif(is_str(binary_data)){
      free(binary_data)
   }
   true
}

@jit
fn parse_gltf_str(any: json_str, str: base_path="", any: binary_override=0): dict {
   mut gltf = json_decode(json_str)
   if(!is_dict(gltf)){ return {"error": "Failed to parse glTF JSON"} }
   mut errors = []
   mut warnings = []
   def asset = gltf.get("asset", 0)
   if(!is_dict(asset)){ errors = errors.append("missing asset object") } else {
      def ver = to_str(asset.get("version", ""))
      if(ver.len == 0){ errors = errors.append("asset.version is required") }
      def minv = to_str(asset.get("minVersion", ""))
      if(minv.len > 0 && ver.len > 0 && minv > ver){ errors = errors.append("asset.minVersion > asset.version") }
   }
   def used = gltf.get("extensionsUsed", [])
   def req = gltf.get("extensionsRequired", [])
   if(is_list(req)){
      def caps = gltf_supported_extension_caps()
      def req_n = req.len
      def used_n = is_list(used) ? used.len : 0
      mut ri = 0
      while(ri < req_n){
         def name = to_str(req[ri])
         mut found = false
         mut ui = 0
         while(ui < used_n){
            if(to_str(used[ui]) == name){ found = true }
            ui += 1
         }
         if(!found){ errors = errors.append("extensionsRequired contains " + name + " not present in extensionsUsed") }
         def st = to_str(caps.get(name, "unknown"))
         if(st == "unknown" || st == "todo" || st == "fallback-or-todo"){ errors = errors.append("extensionsRequired contains unsupported " + name + " (" + st + ")") }
         ri += 1
      }
   }
   def buffers = gltf.get("buffers")
   mut binary_data = binary_override
   mut buffer_data = []
   if(is_list(buffers) && buffers.len > 0){
      def buffers_n = buffers.len
      mut bi = 0
      while(bi < buffers_n){
         def buf = buffers[bi]
         def uri = buf.get("uri", "")
         mut loaded = 0
         if(bi == 0 && binary_override){ loaded = binary_override } elif(is_str(uri) && uri.len > 0){ loaded = _gltf_load_buffer_uri(uri, base_path) }
         buffer_data = buffer_data.append(loaded)
         bi += 1
      }
      if(buffer_data.len > 0){ binary_data = buffer_data[0] }
   }
   gltf["_ny_buffer_data"] = buffer_data
   gltf["_ny_base_path"] = base_path
   gltf["_ny_cache_key"] = _gltf_cache_key_from_g(gltf) + "|json=" + to_str(lib_hash.xxh32(json_str))
   def images = gltf.get("images", [])
   if(is_list(images)){
      def images_n = images.len
      mut ii = 0
      while(ii < images_n){
         def img = images[ii]
         if(is_dict(img)){
            def uri = img.get("uri", "")
            def has_uri = is_str(uri) && uri.len > 0
            def has_bv = int(img.get("bufferView", -1)) >= 0
            def mime = to_str(img.get("mimeType", ""))
            if(has_uri && has_bv){ errors = errors.append("image[" + to_str(ii) + "] has both uri and bufferView") }
            if(has_bv && mime.len == 0){ errors = errors.append("image[" + to_str(ii) + "] bufferView requires mimeType") }
            if(has_bv && !_gltf_image_mime_supported(mime)){ errors = errors.append("image[" + to_str(ii) + "] unsupported mimeType " + mime) }
         }
         ii += 1
      }
   }
   mut out = {
      "gltf": gltf, "binary_data": binary_data, "buffer_data": buffer_data,
      "base_path": base_path, "errors": errors, "warnings": warnings
   }
   if(errors.len > 0){ out["error"] = "Invalid glTF: " + str.join(errors, "; ") }
   out
}

fn load_gltf_file(str: path): any {
   def dir = _gltf_path_dirname(path)
   def res = file_read(path)
   if(is_err(res)){ return {"error": "Failed to read glTF file: " + path} }
   def raw = unwrap(res)
   if(is_str(raw) && raw.len >= 4 && u32le(raw, 0) == _GLB_MAGIC){
      mut glb = _gltf_parse_glb(raw, dir)
      if(is_dict(glb)){
         glb["source_path"] = path
         _gltf_stamp_cache_key(glb, path)
         glb["anim_duration_hint"] = 0.0
      }
      return glb
   }
   mut parsed = _gltf_parse_gltf_with_fallbacks(raw, dir)
   if(is_dict(parsed)){
      parsed["source_path"] = path
      _gltf_stamp_cache_key(parsed, path)
      parsed["anim_duration_hint"] = 0.0
   }
   parsed
}

fn _gltf_anim_trace_enabled(): bool {
   _gltf_anim_trace_cache = common.cached_env_truthy(_gltf_anim_trace_cache, "NY_GLTF_ANIM_TRACE")
   _gltf_anim_trace_cache == 1
}

fn _gltf_extract_embedded_image(any: gltf_data, int: img_idx): str {
   _gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return "" }
   def images = g.get("images")
   if(!is_list(images) || img_idx < 0 || img_idx >= images.len){ return "" }
   def img = images[img_idx]
   def base_path = to_str(gltf_data.get("base_path", ""))
   def source_path = to_str(gltf_data.get("source_path", ""))
   def cache_root = (source_path.len > 0) ? source_path : base_path
   def cache_key = cache_root + "|" + to_str(img_idx)
   if(_gltf_img_uri_cache.contains(cache_key)){ return to_str(_gltf_img_uri_cache.get(cache_key, "")) }
   mut out_path = ""
   mut uri = to_str(img.get("uri", ""))
   if(uri.len > 0){
      if(str.find(uri, "data:") == 0){
         def decoded = _gltf_data_uri_decode(uri)
         if(!is_str(decoded) || decoded.len <= 0){ return "" }
         mut mime = to_str(img.get("mimeType", ""))
         if(mime.len == 0){
            def semi = str.find_from(uri, ";", 5)
            def comma = str.find_from(uri, ",", 5)
            if(semi > 5){ mime = _gltf_copy_bytes(uri, 5, semi - 5) }
            elif(comma > 5){ mime = _gltf_copy_bytes(uri, 5, comma - 5) }
         }
         out_path = ospath.join(ospath.cache_dir(), "ny_gltf_img_" + to_str(lib_hash.xxh32(base_path + "|" + to_str(img_idx) + "|" + mime + "|data")) + _gltf_image_ext_from_mime(mime))
         if(!file_exists(out_path)){ _ = file_write(out_path, decoded) }
      } else {
         out_path = ospath.join(base_path, _gltf_url_decode(uri))
      }
      _gltf_img_uri_cache = cache.cache_put_reset(_gltf_img_uri_cache, cache_key, out_path, _GLTF_CACHE_LIMIT_MED, 128)
      return out_path
   }
   def bv_list = g.get("bufferViews")
   def bv_idx = int(img.get("bufferView", -1))
   if(!is_list(bv_list) || bv_idx < 0 || bv_idx >= bv_list.len){ return "" }
   def bv = bv_list[bv_idx]
   def off = int(bv.get("byteOffset", 0))
   def blen = int(bv.get("byteLength", 0))
   if(blen <= 0){ return "" }
   def buffer_data = gltf_data.get("buffer_data", 0)
   def buf_idx = int(bv.get("buffer", 0))
   mut bin_blob = 0
   if(is_list(buffer_data) && buf_idx >= 0 && buf_idx < buffer_data.len){
      def b = buffer_data[buf_idx]
      bin_blob = b
   }
   if(!bin_blob){
      def binary_data = gltf_data.get("binary_data", 0)
      bin_blob = binary_data
   }
   if(!bin_blob){ return "" }
   def bytes = _gltf_copy_blob_bytes(bin_blob, off, blen)
   if(!is_str(bytes) || bytes.len <= 0){ return "" }
   def mime = to_str(img.get("mimeType", "application/octet-stream"))
   out_path = ospath.join(ospath.cache_dir(), "ny_gltf_img_" + to_str(lib_hash.xxh32(cache_root + "|" + to_str(img_idx) + "|" + to_str(off) + "|" + to_str(blen) + "|" + mime)) + _gltf_image_ext_from_mime(mime))
   if(!file_exists(out_path)){ _ = file_write(out_path, bytes) }
   _gltf_img_uri_cache = cache.cache_put_reset(_gltf_img_uri_cache, cache_key, out_path, _GLTF_CACHE_LIMIT_MED, 128)
   out_path
}

fn _gltf_texture_uv_props(dict: out, str: prefix, any: tex_info): dict {
   mut o = out
   mut texcoord = is_dict(tex_info) ? int(tex_info.get("texCoord", 0)) : 0
   mut uv_offset = [0.0, 0.0]
   mut uv_scale = [1.0, 1.0]
   mut uv_words = [0, 0]
   def exts = is_dict(tex_info) ? tex_info.get("extensions", 0) : 0
   def xform = is_dict(exts) ? exts.get("KHR_texture_transform", 0) : 0
   if(is_dict(xform)){
      def off = xform.get("offset", [0.0, 0.0])
      def scl = xform.get("scale", [1.0, 1.0])
      def uv_offset_v = [_gltf_num_or(off.get(0, 0.0), 0.0), _gltf_num_or(off.get(1, 0.0), 0.0)]
      def uv_scale_v = [_gltf_num_or(scl.get(0, 1.0), 1.0), _gltf_num_or(scl.get(1, 1.0), 1.0)]
      def uv_rotation_raw = xform.get("rotation", 0.0)
      def uv_rotation_v = _gltf_num_or(uv_rotation_raw, 0.0)
      def texcoord_v = int(xform.get("texCoord", texcoord))
      uv_offset = uv_offset_v
      uv_scale = uv_scale_v
      texcoord = texcoord_v
      uv_words = _gltf_pack_uv_xform_words_from_values(uv_offset_v, uv_scale_v, uv_rotation_v, texcoord_v)
   } else {
      uv_words = _gltf_pack_uv_xform_words_from_values(uv_offset, uv_scale, 0.0, texcoord)
   }
   o[prefix + "_texcoord"] = texcoord
   o[prefix + "_uv_offset"] = uv_offset
   o[prefix + "_uv_scale"] = uv_scale
   o[prefix + "_uv_rotation"] = is_dict(xform) ? xform.get("rotation", 0.0) : 0.0
   o[prefix + "_uv_xf0"] = int(uv_words.get(0, 0))
   o[prefix + "_uv_xf1"] = int(uv_words.get(1, 0))
   o
}

fn _gltf_is_debug(): bool {
   if(_gltf_debug_cache != -1){ return _gltf_debug_cache == 1 }
   _gltf_debug_cache = (common.env_truthy("NY_GLTF_DEBUG") || common.env_truthy("NY_GLTF_MODEL_DEBUG") || common.env_truthy("NY_DEBUG_DEEP")) ? 1 : 0
   _gltf_debug_cache == 1
}

fn _gltf_autolod_enabled(): bool {
   _gltf_autolod_cache = common.cached_env_truthy(_gltf_autolod_cache, "NY_GLTF_AUTOLOD")
   _gltf_autolod_cache == 1
}

fn _gltf_warn_unhandled_material_maps(any: info, int: material_idx): bool {
   if(!_gltf_is_debug() || !is_dict(info)){ return false }
   false
}

fn _gltf_make_material_slot(int: tex_id, int: uv_set, int: xf0, int: xf1): dict { return {"tex_id": int(tex_id), "uv_set": int(uv_set), "xf0": int(xf0), "xf1": int(xf1)} }

fn _gltf_resolve_part_skin(dict: gltf_data, dict: g, any: data, list: nodes, int: part_node_idx, dict: meta, any: vptr, int: vcnt): dict {
   if(part_node_idx < 0 || part_node_idx >= nodes.len){
      return {"skin_idx": -1,
         "skin_bind_vptr": 0,
         "skin_joints_ptr": 0,
         "skin_weights_ptr": 0,
         "skin_vcnt": vcnt,
         "skin_joints": [],
      "skin_inv_bind_accessor": -1}
   }
   def node_obj = nodes.get(part_node_idx, 0)
   def skin_idx = is_dict(node_obj) ? int(node_obj.get("skin", -1)) : -1
   def out = {"skin_idx": skin_idx, "skin_bind_vptr": 0, "skin_joints_ptr": 0, "skin_weights_ptr": 0, "skin_vcnt": vcnt, "skin_joints": [], "skin_inv_bind_accessor": -1}
   if(!(skin_idx >= 0 && meta.get("is_skinned_candidate", false) && vptr && vcnt > 0)){ return out }
   def skin_bind_vptr = malloc(vcnt * _GLTF_VTX_STRIDE)
   if(!skin_bind_vptr){ return out }
   memcpy(skin_bind_vptr, vptr, vcnt * _GLTF_VTX_STRIDE)
   def joints_acc_idx = int(meta.get("joints_acc_idx", -1))
   def weights_acc_idx = int(meta.get("weights_acc_idx", -1))
   def skin_side = _gltf_pack_skin_sidecars(g, data, joints_acc_idx, weights_acc_idx, vcnt)
   if(!is_dict(skin_side)){
      free(skin_bind_vptr)
      return out
   }
   def skin_info = gltf_skin_info(gltf_data, skin_idx)
   def skin_joints = is_dict(skin_info) ? skin_info.get("joints", []) : []
   return {
      "skin_idx": skin_idx,
      "skin_bind_vptr": skin_bind_vptr,
      "skin_joints_ptr": skin_side.get("joints_ptr", 0),
      "skin_weights_ptr": skin_side.get("weights_ptr", 0),
      "skin_vcnt": int(skin_side.get("count", vcnt)),
      "skin_joints": skin_joints,
      "skin_inv_bind_accessor": is_dict(skin_info) ? int(skin_info.get("inverse_bind_accessor", -1)) : -1
   }
}

fn _gltf_scene_bounds_new(): list { [1e9, 1e9, 1e9, -1e9, -1e9, -1e9] }

fn _gltf_scene_bounds_accum(
   list: state, f64: wmin_x, f64: wmin_y, f64: wmin_z, f64: wmax_x, f64: wmax_y, f64: wmax_z
): list {
   if(state.len < 6){ state = _gltf_scene_bounds_new() }
   if(_gltf_float_bad(wmin_x) || _gltf_float_bad(wmin_y) || _gltf_float_bad(wmin_z) ||
      _gltf_float_bad(wmax_x) || _gltf_float_bad(wmax_y) || _gltf_float_bad(wmax_z)){
      return state
   }
   wmin_x, wmin_y = 0.0 + wmin_x, 0.0 + wmin_y
   wmin_z = 0.0 + wmin_z
   wmax_x = 0.0 + wmax_x
   wmax_y = 0.0 + wmax_y
   wmax_z = 0.0 + wmax_z
   if(wmin_x > wmax_x || wmin_y > wmax_y || wmin_z > wmax_z){ return state }
   if(wmin_x < float(state.get(0, 1e9))){ state[0] = wmin_x }
   if(wmin_y < float(state.get(1, 1e9))){ state[1] = wmin_y }
   if(wmin_z < float(state.get(2, 1e9))){ state[2] = wmin_z }
   if(wmax_x > float(state.get(3, -1e9))){ state[3] = wmax_x }
   if(wmax_y > float(state.get(4, -1e9))){ state[4] = wmax_y }
   if(wmax_z > float(state.get(5, -1e9))){ state[5] = wmax_z }
   state
}

fn _gltf_scene_bounds_result(list: state): list {
   mut pmin = [float(state.get(0, 1e9)), float(state.get(1, 1e9)), float(state.get(2, 1e9))]
   mut pmax = [float(state.get(3, -1e9)), float(state.get(4, -1e9)), float(state.get(5, -1e9))]
   if(pmin.get(0, 1e9) > 1e8){ pmin = [-1.0, -1.0, -1.0] }
   if(pmax.get(0, -1e9) < -1e8){ pmax = [1.0, 1.0, 1.0] }
   [pmin, pmax]
}

fn _gltf_alpha_mode_code(str: alpha_mode): int {
   case alpha_mode {
      "MASK" -> 1
      "BLEND" -> 2
      _ -> 0
   }
}

fn _gltf_prim_mode_expands_to_vertices(int: prim_mode): bool {
   case prim_mode {
      0, 1, 2, 3 -> true
      _ -> false
   }
}

fn _gltf_apply_prim_mode_opts(dict: mesh_opts, int: prim_mode, bool: has_normals): dict {
   case prim_mode {
      0 -> {
         mesh_opts["is_points"] = true
         if(!has_normals){ mesh_opts["unlit"] = true }
      }
      1, 2, 3 -> {
         mesh_opts["is_lines"] = true
         if(!has_normals){ mesh_opts["unlit"] = true }
      }
      _ -> {}
   }
   mesh_opts
}

fn _gltf_accessor_view_meta(any: data, dict: bv, dict: acc): dict {
   def comp = acc.get("componentType", 0)
   def typ = acc.get("type", "")
   mut stride = bv.get("byteStride", 0)
   if(stride <= 0){ stride = _gltf_comp_size(comp) * _gltf_type_count(typ) }
   {
      "ptr": ptr_add(ptr_add(data, int(bv.get("byteOffset", 0))), int(acc.get("byteOffset", 0))),
      "count": acc.get("count", 0),
      "comp": comp,
      "normalized": acc.get("normalized", false),
      "stride": stride,
      "type_count": _gltf_type_count(typ)
   }
}

fn _gltf_legacy_attr_meta(any: data, any: accs, any: bv_list, int: acc_idx, int: accs_n, int: bv_n): dict {
   mut out = {"bv_off": 0, "acc_off": 0, "elem": 0, "cnt": 0, "comp": 0, "view": 0}
   if(acc_idx < 0 || acc_idx >= accs_n){ return out }
   def acc = accs.get(acc_idx)
   def bv_idx = acc.get("bufferView", -1)
   if(bv_idx < 0 || bv_idx >= bv_n){ return out }
   def bv = bv_list.get(bv_idx)
   mut elem = bv.get("byteStride", 0)
   if(elem <= 0){ elem = _gltf_comp_size(acc.get("componentType", 0)) * _gltf_type_count(acc.get("type", "")) }
   out["bv_off"] = bv.get("byteOffset", 0)
   out["acc_off"] = acc.get("byteOffset", 0)
   out["elem"] = elem
   out["cnt"] = acc.get("count", 0)
   out["comp"] = acc.get("componentType", 0)
   out["view"] = _gltf_accessor_view_meta(data, bv, acc)
   out
}

fn _gltf_legacy_index_meta(any: accs, any: bv_list, int: idx_acc, int: accs_n, int: bv_n): dict {
   mut out = {"base": 0, "count": 0, "comp": 0, "has": false}
   if(idx_acc < 0 || idx_acc >= accs_n){ return out }
   def acc = accs.get(idx_acc)
   def bv_idx = acc.get("bufferView", -1)
   if(bv_idx < 0 || bv_idx >= bv_n){ return out }
   def bv = bv_list.get(bv_idx)
   out["base"] = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
   out["count"] = acc.get("count", 0)
   out["comp"] = acc.get("componentType", 0)
   out["has"] = true
   out
}

fn _gltf_legacy_prim_state(
   dict: prim, any: data, any: accs, any: bv_list, int: accs_n, int: bv_n,
   any: material_tex_ids, int: material_tex_ids_n
): dict {
   def attrs = prim.get("attributes")
   def mat_idx = int(prim.get("material", -1))
   mut prim_tex_id = 0
   if(mat_idx >= 0 && mat_idx < material_tex_ids_n){ prim_tex_id = int(material_tex_ids.get(mat_idx, 0)) }
   def pos = is_dict(attrs) ? _gltf_legacy_attr_meta(data, accs, bv_list, int(attrs.get("POSITION", -1)), accs_n, bv_n) : _gltf_legacy_attr_meta(data, accs, bv_list, -1, accs_n, bv_n)
   def uv = is_dict(attrs) ? _gltf_legacy_attr_meta(data, accs, bv_list, int(attrs.get("TEXCOORD_0", -1)), accs_n, bv_n) : _gltf_legacy_attr_meta(data, accs, bv_list, -1, accs_n, bv_n)
   def norm = is_dict(attrs) ? _gltf_legacy_attr_meta(data, accs, bv_list, int(attrs.get("NORMAL", -1)), accs_n, bv_n) : _gltf_legacy_attr_meta(data, accs, bv_list, -1, accs_n, bv_n)
   def color = is_dict(attrs) ? _gltf_legacy_attr_meta(data, accs, bv_list, int(attrs.get("COLOR_0", -1)), accs_n, bv_n) : _gltf_legacy_attr_meta(data, accs, bv_list, -1, accs_n, bv_n)
   def idx = _gltf_legacy_index_meta(accs, bv_list, int(prim.get("indices", -1)), accs_n, bv_n)
   def has_idx = idx.get("has", false)
   def prim_count = has_idx ? int(idx.get("count", 0)) : int(pos.get("cnt", 0))
   {
      "mat_idx": mat_idx, "prim_tex_id": prim_tex_id,
      "pos_total": int(pos.get("bv_off", 0)) + int(pos.get("acc_off", 0)),
      "pos_elem": int(pos.get("elem", 0)), "pos_cnt": int(pos.get("cnt", 0)),
      "pos_comp": int(pos.get("comp", 0)), "pos_cs": _gltf_comp_size(int(pos.get("comp", 0))),
      "uv_total": int(uv.get("bv_off", 0)) + int(uv.get("acc_off", 0)),
      "uv_elem": int(uv.get("elem", 0)), "uv_cnt": int(uv.get("cnt", 0)),
      "uv_comp": int(uv.get("comp", 0)), "uv_cs": _gltf_comp_size(int(uv.get("comp", 0))),
      "norm_total": int(norm.get("bv_off", 0)) + int(norm.get("acc_off", 0)),
      "norm_elem": int(norm.get("elem", 0)), "norm_cnt": int(norm.get("cnt", 0)),
      "norm_comp": int(norm.get("comp", 0)), "norm_cs": _gltf_comp_size(int(norm.get("comp", 0))),
      "c_res": color.get("view", 0),
      "idx_base": int(idx.get("base", 0)), "idx_count": int(idx.get("count", 0)),
      "idx_comp": int(idx.get("comp", 0)), "has_idx": has_idx, "prim_count": prim_count
   }
}

fn _gltf_indexed_material_slots(
   dict: material_state, int: tex_id, int: normal_tex_id, int: normal_uv_set,
   int: emissive_tex_id, int: emissive_uv_set, int: occlusion_tex_id, int: occlusion_uv_set,
   int: material_u32, int: uv_set, int: met_rough_uv_set
): dict {
   def base_slot = _gltf_make_material_slot(tex_id, uv_set, int(material_state.get("base_uv_xf0", 0)), int(material_state.get("base_uv_xf1", 0)))
   {
      "base_color": base_slot,
      "base": base_slot,
      "normal": _gltf_make_material_slot(normal_tex_id, normal_uv_set, int(material_state.get("normal_uv_xf0", 0)), int(material_state.get("normal_uv_xf1", 0))),
      "metallic_roughness": _gltf_make_material_slot(_gltf_decode_mr_tex_id(band(bshr(material_u32, 16), 0x7fff)), met_rough_uv_set, int(material_state.get("mr_uv_xf0", 0)), int(material_state.get("mr_uv_xf1", 0))),
      "occlusion": _gltf_make_material_slot(occlusion_tex_id, occlusion_uv_set, int(material_state.get("occlusion_uv_xf0", 0)), int(material_state.get("occlusion_uv_xf1", 0))),
      "emissive": _gltf_make_material_slot(emissive_tex_id, emissive_uv_set, int(material_state.get("emissive_uv_xf0", 0)), int(material_state.get("emissive_uv_xf1", 0)))
   }
}

fn _gltf_part_world_bounds(any: lbs, any: model): list {
   mut wmin_x, wmin_y, wmin_z = 0.0, 0.0, 0.0
   mut wmax_x, wmax_y, wmax_z = 0.0, 0.0, 0.0
   if(is_list(lbs) && is_list(model) && model.len >= 16){
      def wb = _gltf_transform_aabb(lbs.get(0), lbs.get(1), model)
      if(is_list(wb) && wb.len >= 2){
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

fn _gltf_scene_bounds_accum_part(list: state, list: world_bounds): list {
   _gltf_scene_bounds_accum(state,
      float(world_bounds.get(0, 0.0)),
      float(world_bounds.get(1, 0.0)),
      float(world_bounds.get(2, 0.0)),
      float(world_bounds.get(3, 0.0)),
      float(world_bounds.get(4, 0.0)),
   float(world_bounds.get(5, 0.0)))
}

fn _gltf_make_indexed_part_from_state(
   any: vptr, int: vcnt, any: iptr, int: icnt, dict: part_opts,
   dict: material_state, int: mat_idx, int: uv_set, any: model,
   int: part_node_idx, dict: skin_state, bool: part_visible,
   bool: has_gpu_instancing, int: prim_mode, any: lod_data, list: world_bounds,
   int: packed_color
): dict {
   def tex_id = int(material_state.get("tex_id", -1))
   def material_u32 = int(material_state.get("material_u32", 0x0000ff00))
   def normal_tex_id = int(material_state.get("normal_tex_id", -1))
   def normal_uv_set = int(material_state.get("normal_uv_set", 0))
   def emissive_tex_id = int(material_state.get("emissive_tex_id", -1))
   def emissive_uv_set = int(material_state.get("emissive_uv_set", 0))
   def occlusion_tex_id = int(material_state.get("occlusion_tex_id", -1))
   def occlusion_uv_set = int(material_state.get("occlusion_uv_set", 0))
   def met_rough_uv_set = int(material_state.get("met_rough_uv_set", 0))
   def skin_joints = skin_state.get("skin_joints", [])
   def bsdf2_u32 = _gltf_scale_bsdf2_thickness(int(material_state.get("bsdf2_u32", 0)), _gltf_model_mean_scale(model))
   {
      "vptr": vptr,
      "vcnt": vcnt,
      "iptr": iptr,
      "icnt": icnt,
      "opts": part_opts,
      "index_type_u32": part_opts.get("index_type_u32", false),
      "unlit": part_opts.get("unlit", false),
      "no_cull": part_opts.get("no_cull", false),
      "double_sided": part_opts.get("double_sided", false),
      "flip_winding": part_opts.get("flip_winding", false),
      "mat_idx": mat_idx,
      "tex_id": tex_id,
      "base_color_u32": int(material_state.get("prim_packed_color", packed_color)),
      "material_u32": material_u32,
      "base_uv_xf0": int(material_state.get("base_uv_xf0", 0)),
      "base_uv_xf1": int(material_state.get("base_uv_xf1", 0)),
      "normal_uv_xf0": int(material_state.get("normal_uv_xf0", 0)),
      "normal_uv_xf1": int(material_state.get("normal_uv_xf1", 0)),
      "mr_uv_xf0": int(material_state.get("mr_uv_xf0", 0)),
      "mr_uv_xf1": int(material_state.get("mr_uv_xf1", 0)),
      "occlusion_uv_xf0": int(material_state.get("occlusion_uv_xf0", 0)),
      "occlusion_uv_xf1": int(material_state.get("occlusion_uv_xf1", 0)),
      "emissive_uv_xf0": int(material_state.get("emissive_uv_xf0", 0)),
      "emissive_uv_xf1": int(material_state.get("emissive_uv_xf1", 0)),
      "is_points": part_opts.get("is_points", false),
      "is_lines": part_opts.get("is_lines", false),
      "vc_mode": int(material_state.get("prim_vc_mode", 0)),
      "normal_tex_id": normal_tex_id,
      "normal_uv_set": normal_uv_set,
      "normal_tex_word": int(material_state.get("normal_tex_word", 0x80000000)),
      "emissive_tex_id": emissive_tex_id,
      "emissive_u32": int(material_state.get("emissive_u32", 0)),
      "emissive_uv_set": emissive_uv_set,
      "alpha_u32": int(material_state.get("alpha_u32", 0)),
      "occlusion": occlusion_tex_id,
      "occlusion_uv_set": occlusion_uv_set,
      "bsdf0_u32": int(material_state.get("bsdf0_u32", 0)),
      "bsdf1_u32": int(material_state.get("bsdf1_u32", 0)),
      "bsdf2_u32": bsdf2_u32,
      "bsdf3_u32": int(material_state.get("bsdf3_u32", 0)),
      "bsdf4_u32": int(material_state.get("bsdf4_u32", 0)),
      "bsdf5_u32": int(material_state.get("bsdf5_u32", 0)),
      "ext2_tex_word": int(material_state.get("ext2_tex_word", 0x80000000)),
      "material_slots": _gltf_indexed_material_slots(material_state, tex_id, normal_tex_id, normal_uv_set, emissive_tex_id, emissive_uv_set, occlusion_tex_id, occlusion_uv_set, material_u32, uv_set, met_rough_uv_set),
      "model": model,
      "node_idx": part_node_idx,
      "skin_idx": int(skin_state.get("skin_idx", -1)),
      "skin_bind_vptr": skin_state.get("skin_bind_vptr", 0),
      "skin_joints_ptr": skin_state.get("skin_joints_ptr", 0),
      "skin_weights_ptr": skin_state.get("skin_weights_ptr", 0),
      "skin_vcnt": int(skin_state.get("skin_vcnt", vcnt)),
      "skin_mesh_bind_world": model,
      "skin_joint_nodes": skin_joints,
      "skin_joint_count": is_list(skin_joints) ? skin_joints.len : 0,
      "skin_inv_bind_accessor": int(skin_state.get("skin_inv_bind_accessor", -1)),
      "visible": part_visible,
      "instanced_part": has_gpu_instancing,
      "primitive_mode": prim_mode,
      "has_normals": part_opts.get("has_normals", false),
      "lod_hierarchy": lod_data,
      "min": [float(world_bounds.get(0, 0.0)), float(world_bounds.get(1, 0.0)), float(world_bounds.get(2, 0.0))],
      "max": [float(world_bounds.get(3, 0.0)), float(world_bounds.get(4, 0.0)), float(world_bounds.get(5, 0.0))]
   }
}

fn _gltf_active_scene_idx(any: g, any: gltf_data=0): int {
   if(!is_dict(g)){ return 0 }
   if(is_dict(gltf_data) && gltf_data.contains("scene_index")){ return int(gltf_data.get("scene_index", 0)) }
   int(g.get("scene", 0))
}

@jit
fn _gltf_vertex_color_u32(any: color_res, int: vi, int: fallback_u32): int {
   if(!is_dict(color_res)){ return fallback_u32 }
   def ptr = color_res.get("ptr", 0)
   def cnt = int(color_res.get("count", 0))
   if(!ptr || vi < 0 || vi >= cnt){ return fallback_u32 }
   def stride = int(color_res.get("stride", 0))
   def comp = int(color_res.get("comp", 0))
   def cs = _gltf_comp_size(comp)
   if(cs <= 0 || stride <= 0){ return fallback_u32 }
   def type_count = int(color_res.get("type_count", 4))
   if(type_count != 3 && type_count != 4){ return fallback_u32 }
   ; glTF vertex colors are FLOAT or normalized integer formats.
   ; For robustness, keep your integer-as-normalized behavior.
   def norm = color_res.get("normalized", false) || comp != GLTF_COMP_FLOAT
   def off = vi * stride
   if(comp == GLTF_COMP_UBYTE && norm){
      def ir, ig = load8(ptr, off + 0), load8(ptr, off + 1)
      def ib, ia = load8(ptr, off + 2), type_count == 4 ? load8(ptr, off + 3) : 255
      return ir | (ig << 8) | (ib << 16) | (ia << 24)
   }
   mut r = clamp01(_gltf_read_f32_acc(ptr, off + cs * 0, comp, norm))
   mut g = clamp01(_gltf_read_f32_acc(ptr, off + cs * 1, comp, norm))
   mut b = clamp01(_gltf_read_f32_acc(ptr, off + cs * 2, comp, norm))
   mut a = 1.0
   if(type_count == 4){ a = clamp01(_gltf_read_f32_acc(ptr, off + cs * 3, comp, norm)) }
   def ir, ig = band(int(r * 255.0 + 0.5), 255), band(int(g * 255.0 + 0.5), 255)
   def ib, ia = band(int(b * 255.0 + 0.5), 255), band(int(a * 255.0 + 0.5), 255)
   ir | (ig << 8) | (ib << 16) | (ia << 24)
}

fn _gltf_make_uv_xform(any: info, str: prefix): dict {
   {
      "offset": is_dict(info) ? info.get(prefix + "_uv_offset", [0.0, 0.0]) : [0.0, 0.0],
      "scale": is_dict(info) ? info.get(prefix + "_uv_scale", [1.0, 1.0]) : [1.0, 1.0],
      "rotation": is_dict(info) ? _gltf_num_or(info.get(prefix + "_uv_rotation", 0.0), 0.0) : 0.0
   }
}

fn _gltf_legacy_mesh_total_vertices(list: primitives, any: accs, int: accs_n): int {
   mut total = 0
   mut pi = 0
   while(pi < primitives.len){
      def prim = primitives.get(pi)
      def idx_acc = prim.get("indices", -1)
      if(idx_acc >= 0){
         if(idx_acc < accs_n){
            def acc = accs.get(idx_acc)
            total += acc.get("count", 0)
         }
      } else {
         def pos_attr = prim.get("attributes")
         if(is_dict(pos_attr)){
            def pos_acc_idx = pos_attr.get("POSITION", -1)
            if(pos_acc_idx >= 0 && pos_acc_idx < accs_n){
               def pos_acc = accs.get(pos_acc_idx, 0)
               total += is_dict(pos_acc) ? pos_acc.get("count", 0) : 0
            }
         }
      }
      pi += 1
   }
   total
}

fn _gltf_store_legacy_vertex(any: dst, int: byte_off, any: px, any: py, any: pz, any: u, any: v_uv, int: color_u32, any: nx, any: ny, any: nz, int: prim_tex_id): any {
   store32_f32(dst, px, byte_off + _GLTF_VTX_OFF_X)
   store32_f32(dst, py, byte_off + _GLTF_VTX_OFF_Y)
   store32_f32(dst, pz, byte_off + _GLTF_VTX_OFF_Z)
   store32_f32(dst, u, byte_off + _GLTF_VTX_OFF_U)
   store32_f32(dst, v_uv, byte_off + _GLTF_VTX_OFF_V)
   store32(dst, color_u32, byte_off + _GLTF_VTX_OFF_C)
   store32_f32(dst, nx, byte_off + _GLTF_VTX_OFF_NX)
   store32_f32(dst, ny, byte_off + _GLTF_VTX_OFF_NY)
   store32_f32(dst, nz, byte_off + _GLTF_VTX_OFF_NZ)
   store32_f32(dst, 0.0, byte_off + _GLTF_VTX_OFF_U2)
   store32_f32(dst, 0.0, byte_off + _GLTF_VTX_OFF_V2)
   store32(dst, prim_tex_id, byte_off + _GLTF_VTX_OFF_TEX)
}

fn _gltf_mat3x4_num(any: m): list {
   [
      _gltf_num_or(m.get(0, 1.0), 1.0), _gltf_num_or(m.get(4, 0.0), 0.0), _gltf_num_or(m.get(8, 0.0), 0.0), _gltf_num_or(m.get(12, 0.0), 0.0),
      _gltf_num_or(m.get(1, 0.0), 0.0), _gltf_num_or(m.get(5, 1.0), 1.0), _gltf_num_or(m.get(9, 0.0), 0.0), _gltf_num_or(m.get(13, 0.0), 0.0),
      _gltf_num_or(m.get(2, 0.0), 0.0), _gltf_num_or(m.get(6, 0.0), 0.0), _gltf_num_or(m.get(10, 1.0), 1.0), _gltf_num_or(m.get(14, 0.0), 0.0)
   ]
}

fn _gltf_mat3_num(any: m): list {
   [
      float(m.get(0, 1.0)), float(m.get(4, 0.0)), float(m.get(8, 0.0)),
      float(m.get(1, 0.0)), float(m.get(5, 1.0)), float(m.get(9, 0.0)),
      float(m.get(2, 0.0)), float(m.get(6, 0.0)), float(m.get(10, 1.0))
   ]
}

fn _gltf_emit_legacy_vertex(any: dst, int: byte_off, any: data, any: ps, any: mesh_m, int: vi, int: prim_tex_id, bool: normalize_normals=false): list {
   def pos_total = int(ps.get("pos_total", 0))
   def pos_elem = int(ps.get("pos_elem", 0))
   def pos_comp = int(ps.get("pos_comp", 0))
   def pos_cs = int(ps.get("pos_cs", 0))
   def uv_total = int(ps.get("uv_total", 0))
   def uv_elem = int(ps.get("uv_elem", 0))
   def uv_cnt = int(ps.get("uv_cnt", 0))
   def uv_comp = int(ps.get("uv_comp", 0))
   def uv_cs = int(ps.get("uv_cs", 0))
   def norm_total = int(ps.get("norm_total", 0))
   def norm_elem = int(ps.get("norm_elem", 0))
   def norm_cnt = int(ps.get("norm_cnt", 0))
   def norm_comp = int(ps.get("norm_comp", 0))
   def norm_cs = int(ps.get("norm_cs", 0))
   def c_res = ps.get("c_res", 0)
   def m00 = float(mesh_m.get(0, 1.0))
   def m10 = float(mesh_m.get(1, 0.0))
   def m20 = float(mesh_m.get(2, 0.0))
   def m01 = float(mesh_m.get(4, 0.0))
   def m11 = float(mesh_m.get(5, 1.0))
   def m21 = float(mesh_m.get(6, 0.0))
   def m02 = float(mesh_m.get(8, 0.0))
   def m12 = float(mesh_m.get(9, 0.0))
   def m22 = float(mesh_m.get(10, 1.0))
   def m03 = float(mesh_m.get(12, 0.0))
   def m13 = float(mesh_m.get(13, 0.0))
   def m23 = float(mesh_m.get(14, 0.0))
   def base = pos_total + vi * pos_elem
   def px0 = _gltf_read_f32_fast(data, base, pos_comp)
   def py0 = _gltf_read_f32_fast(data, base + pos_cs, pos_comp)
   def pz0 = _gltf_read_f32_fast(data, base + pos_cs * 2, pos_comp)
   def px = m00 * px0 + m01 * py0 + m02 * pz0 + m03
   def py = m10 * px0 + m11 * py0 + m12 * pz0 + m13
   def pz = m20 * px0 + m21 * py0 + m22 * pz0 + m23
   mut u = 0.0
   mut v_uv = 0.0
   if(uv_cnt > 0 && uv_elem > 0 && vi < uv_cnt){
      u = _gltf_read_f32_fast(data, uv_total + vi * uv_elem, uv_comp)
      v_uv = _gltf_read_f32_fast(data, uv_total + vi * uv_elem + uv_cs, uv_comp)
   }
   mut nx, ny = 0.0, 0.0
   mut nz = 0.0
   if(norm_cnt > 0 && norm_elem > 0 && vi < norm_cnt){
      def nbase = norm_total + vi * norm_elem
      def nx0 = _gltf_read_f32_fast(data, nbase, norm_comp)
      def ny0 = _gltf_read_f32_fast(data, nbase + norm_cs, norm_comp)
      def nz0 = _gltf_read_f32_fast(data, nbase + norm_cs * 2, norm_comp)
      nx, ny = m00 * nx0 + m01 * ny0 + m02 * nz0, m10 * nx0 + m11 * ny0 + m12 * nz0
      nz = m20 * nx0 + m21 * ny0 + m22 * nz0
      if(normalize_normals){
         def nl = sqrt(nx * nx + ny * ny + nz * nz)
         if(nl > 0.000001){
            nx, ny = nx / nl, ny / nl
            nz = nz / nl
         }
      }
   }
   _gltf_store_legacy_vertex(dst, byte_off, px, py, pz, u, v_uv, _gltf_vertex_color_u32(c_res, vi, 0xffffffff), nx, ny, nz, prim_tex_id)
   [px, py, pz]
}

fn _gltf_pack_uv_xform_words_from_values(any: uv_off, any: uv_scl, any: uv_rot, any: uv_set_raw): list {
   def off_x0, off_y0 = _gltf_num_or(uv_off.get(0, 0.0), 0.0), _gltf_num_or(uv_off.get(1, 0.0), 0.0)
   def scl_x0, scl_y0 = _gltf_num_or(uv_scl.get(0, 1.0), 1.0), _gltf_num_or(uv_scl.get(1, 1.0), 1.0)
   def rot0 = _gltf_num_or(uv_rot, 0.0)
   def uv_set = int(uv_set_raw)
   def has_non_identity_xform =
   abs(off_x0) > 0.000001 ||
   abs(off_y0) > 0.000001 ||
   abs(scl_x0 - 1.0) > 0.000001 ||
   abs(scl_y0 - 1.0) > 0.000001 ||
   abs(rot0) > 0.000001
   if(!has_non_identity_xform){
      mut word1_id = 0
      if(uv_set != 0){ word1_id = (1 << 30) }
      return [0, word1_id]
   }
   def off_x, off_y = min(8.0, max(-8.0, off_x0)), min(8.0, max(-8.0, off_y0))
   def scl_x, scl_y = min(32.0, max(-32.0, scl_x0)), min(32.0, max(-32.0, scl_y0))
   def local_pi = 3.141592653589793
   def local_two_pi = 6.283185307179586
   def rot_turns = floor((rot0 + local_pi) / local_two_pi)
   def rot_wrapped = rot0 - rot_turns * local_two_pi
   def rot = min(local_pi, max(-local_pi, rot_wrapped))
   def off_x_q = band(int(((off_x + 8.0) / 16.0) * 65535.0 + 0.5), 0xffff)
   def off_y_q = band(int(((off_y + 8.0) / 16.0) * 65535.0 + 0.5), 0xffff)
   def scl_x_q = abs(scl_x0 - 1.0) <= 0.000001 ? 0 : band(int(((scl_x + 32.0) / 64.0) * 2047.0 + 0.5), 0x7ff)
   def scl_y_q = abs(scl_y0 - 1.0) <= 0.000001 ? 0 : band(int(((scl_y + 32.0) / 64.0) * 2047.0 + 0.5), 0x7ff)
   def rot_q = band(int(((rot + local_pi) / local_two_pi) * 255.0 + 0.5), 0xff)
   def word0 = bor(off_x_q, bshl(off_y_q, 16))
   mut uv_set_bit = 0
   if(uv_set != 0){ uv_set_bit = 1 }
   def word1 = bor(bor(scl_x_q, bshl(scl_y_q, 11)), bor(bshl(rot_q, 22), bshl(uv_set_bit, 30)))
   [word0, word1]
}

fn _gltf_pack_uv_xform_words(any: info, str: prefix): list {
   "Packs glTF texture-transform state into two u32 words for the shader.
   word0: offset.x/offet.y as 2x16-bit signed-fixed over [-8, 8]
   word1: scale.x/scale.y as 2x11-bit fixed over [-32, 32], rotation as 8-bit fixed over [-PI, PI], uvSet bit at 30."
   if(is_dict(info) && info.contains(prefix + "_uv_xf0") && info.contains(prefix + "_uv_xf1")){ return [int(info.get(prefix + "_uv_xf0", 0)), int(info.get(prefix + "_uv_xf1", 0))] }
   def uv_off = is_dict(info) ? info.get(prefix + "_uv_offset", [0.0, 0.0]) : [0.0, 0.0]
   def uv_scl = is_dict(info) ? info.get(prefix + "_uv_scale", [1.0, 1.0]) : [1.0, 1.0]
   def uv_rot = is_dict(info) ? _gltf_num_or(info.get(prefix + "_uv_rotation", 0.0), 0.0) : 0.0
   def uv_set = is_dict(info) ? int(info.get(prefix + "_texcoord", 0)) : 0
   _gltf_pack_uv_xform_words_from_values(uv_off, uv_scl, uv_rot, uv_set)
}

fn _gltf_uv_xf_force_uv0(any: xf0, any: xf1): list { [int(xf0), band(int(xf1), 0xbfffffff)] }

fn _gltf_pick_primary_uv_props(any: info, any: texrec=0): list {
   "Chooses the best available texcoord set/xform for the currently-rendered material path.
   Preference order follows visible shading inputs under the current renderer."
   mut prefix = "base_color"
   def has_base = (is_dict(texrec) && (int(texrec.get("base", -1)) >= 0 || int(texrec.get("base_color", -1)) >= 0)) || (is_dict(info) && to_str(info.get("base_color_uri", "")) != "")
   def has_emissive = (is_dict(texrec) && int(texrec.get("emissive", -1)) >= 0) || (is_dict(info) && to_str(info.get("emissive_uri", "")) != "")
   def has_normal = (is_dict(texrec) && int(texrec.get("normal", -1)) >= 0) || (is_dict(info) && to_str(info.get("normal_uri", "")) != "")
   def has_mr = (is_dict(texrec) && int(texrec.get("metallic_roughness", -1)) >= 0) || (is_dict(info) && to_str(info.get("metallic_roughness_uri", "")) != "")
   def has_occ = (is_dict(texrec) && int(texrec.get("occlusion", -1)) >= 0) || (is_dict(info) && to_str(info.get("occlusion_uri", "")) != "")
   if(has_base){ prefix = "base_color" }
   elif(has_emissive){ prefix = "emissive" }
   elif(has_normal){ prefix = "normal" }
   elif(has_mr){ prefix = "metallic_roughness" }
   elif(has_occ){ prefix = "occlusion" }
   def uv_set = is_dict(info) ? int(info.get(prefix + "_texcoord", 0)) : 0
   [uv_set, _gltf_make_uv_xform(info, prefix)]
}

fn _gltf_resolve_image_uri(any: gltf_data, any: tex_info): str {
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g) || !is_dict(tex_info)){ return "" }
   def textures = g.get("textures")
   def images = g.get("images")
   if(!is_list(textures) || !is_list(images)){ return "" }
   def tex_idx = int(tex_info.get("index", -1))
   if(tex_idx < 0 || tex_idx >= textures.len){ return "" }
   def tex = textures.get(tex_idx)
   mut src_idx = int(tex.get("source", -1))
   if(src_idx < 0 && is_dict(tex.get("extensions", 0))){
      def exts = tex.get("extensions", 0)
      def basisu = exts.get("KHR_texture_basisu", 0)
      if(is_dict(basisu)){ src_idx = int(basisu.get("source", -1)) }
      if(src_idx < 0){
         def webp = exts.get("EXT_texture_webp", 0)
         if(is_dict(webp)){ src_idx = int(webp.get("source", -1)) }
      }
   }
   if(src_idx < 0 || src_idx >= images.len){ return "" }
   _gltf_extract_embedded_image(gltf_data, src_idx)
}

fn _gltf_texture_filter_from_sampler(any: s): int {
   if(!is_dict(s)){ return -1 }
   if(s.get("mag_linear", false) || s.get("min_linear", false) || s.get("min_uses_mips", false)){ return 1 }
   if(s.get("mag_nearest", false) || s.get("min_nearest", false)){ return 0 }
   -1
}

fn _gltf_texture_sampler_info(any: gltf_data, any: tex_info): any {
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g) || !is_dict(tex_info)){ return 0 }
   def textures = g.get("textures", [])
   def samplers = g.get("samplers", [])
   def tex_idx = int(tex_info.get("index", -1))
   if(!is_list(textures) || tex_idx < 0 || tex_idx >= textures.len){ return 0 }
   def tex = textures.get(tex_idx)
   if(!is_dict(tex)){ return 0 }
   def sampler_idx = int(tex.get("sampler", -1))
   mut mag = -1
   mut minf = -1
   mut wrap_s = 10497
   mut wrap_t = 10497
   if(is_list(samplers) && sampler_idx >= 0 && sampler_idx < samplers.len){
      def sampler = samplers.get(sampler_idx)
      if(is_dict(sampler)){
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

fn _pack_material_word(any: metallic_u8, any: rough_u8, any: mr_word): int {
   bor(
      bor(band(int(metallic_u8), 255), bshl(band(int(rough_u8), 255), 8)),
      bshl(band(int(mr_word), 0xffff), 16)
   )
}

fn _gltf_decode_mr_tex_id(int: mr_word): int {
   def enc = band(mr_word, 0x7fff)
   enc > 0 ? enc - 1 : -1
}

fn _gltf_tex_uri_uv(dict: out, str: prefix, any: gltf_data, any: tex_info): dict {
   def sampler = _gltf_texture_sampler_info(gltf_data, tex_info)
   out[prefix + "_uri"] = _gltf_resolve_image_uri(gltf_data, tex_info)
   out[prefix + "_filter"] = _gltf_texture_filter_from_sampler(sampler)
   out[prefix + "_sampler"] = sampler
   _gltf_texture_uv_props(out, prefix, tex_info)
}

fn _gltf_tex_info(dict: out, str: prefix, any: gltf_data, any: tex_info): dict {
   def sampler = _gltf_texture_sampler_info(gltf_data, tex_info)
   out[prefix + "_uri"] = _gltf_resolve_image_uri(gltf_data, tex_info)
   out[prefix + "_filter"] = _gltf_texture_filter_from_sampler(sampler)
   out[prefix + "_sampler"] = sampler
   out = _gltf_texture_uv_props(out, prefix, tex_info)
   out
}

fn _gltf_extension_status(str: name): str { comptime match GltfExtensionStatus(name, "unknown") }

fn _gltf_extension_report_item(str: name): dict {
   def status = _gltf_extension_status(name)
   {"name": name, "status": status, "known": status != "unknown"}
}

fn gltf_supported_extension_caps(): dict {
   "Returns parser/renderer support status for common glTF extensions. status: parse, partial, shader, todo."
   {
      "KHR_texture_transform": "parse+shader", "KHR_materials_unlit": "parse+shader", "KHR_materials_emissive_strength": "parse+shader",
      "KHR_materials_specular": "parse+packed", "KHR_materials_ior": "parse+packed", "KHR_materials_sheen": "parse+packed", "KHR_materials_clearcoat": "parse+packed",
      "KHR_materials_transmission": "parse+packed", "KHR_materials_volume": "parse+packed", "KHR_materials_volume_scatter": "parse",
      "KHR_materials_iridescence": "parse+packed", "KHR_materials_anisotropy": "parse+packed", "KHR_materials_dispersion": "parse+packed",
      "KHR_materials_diffuse_transmission": "parse", "KHR_materials_refraction": "parse+packed", "KHR_materials_subsurface": "parse+packed",
      "KHR_materials_pbrSpecularGlossiness": "parse", "KHR_materials_variants": "parse", "KHR_lights_punctual": "parse",
      "KHR_node_visibility": "parse", "KHR_animation_pointer": "parse", "EXT_mesh_gpu_instancing": "parse",
      "KHR_meshopt_compression": "parse+decode", "EXT_meshopt_compression": "parse+decode", "KHR_draco_mesh_compression": "todo",
      "KHR_texture_basisu": "fallback-or-todo", "EXT_texture_webp": "parse+decode", "KHR_materials_alpha_coverage": "parse",
      "MSFT_lod": "parse", "KHR_mesh_quantization": "parse", "KHR_xmp": "parse", "KHR_xmp_json_ld": "parse"
   }
}

fn gltf_extensions_report(any: gltf_data): dict {
   "Reports extensionsUsed/extensionsRequired and whether this loader parses them."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return dict(0) }
   mut used_out = list(0)
   def used = g.get("extensionsUsed", [])
   mut i = 0
   while(is_list(used) && i < used.len){
      def name = to_str(used[i])
      used_out = used_out.append(_gltf_extension_report_item(name))
      i += 1
   }
   mut req_out = list(0)
   def req = g.get("extensionsRequired", [])
   i = 0
   while(is_list(req) && i < req.len){
      def name = to_str(req[i])
      req_out = req_out.append(_gltf_extension_report_item(name))
      i += 1
   }
   return {"used": used_out, "required": req_out}
}

fn gltf_material_variant_mappings(any: gltf_data, int: mesh_idx=-1, int: prim_idx=-1): list {
   "Parses KHR_materials_variants mappings from primitives. Returned records are not yet bound by renderer."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   def meshes = g.get("meshes", [])
   mut out = list(0)
   mut mi = 0
   while(is_list(meshes) && mi < meshes.len){
      if(mesh_idx < 0 || mesh_idx == mi){
         def mesh = meshes[mi]
         def prims = is_dict(mesh) ? mesh.get("primitives", []) : []
         mut pi = 0
         while(is_list(prims) && pi < prims.len){
            if(prim_idx < 0 || prim_idx == pi){
               def prim = prims[pi]
               def ext = is_dict(prim) ? prim.get("extensions", 0) : 0
               def mv = is_dict(ext) ? ext.get("KHR_materials_variants", 0) : 0
               def maps = is_dict(mv) ? mv.get("mappings", []) : []
               mut k = 0
               while(is_list(maps) && k < maps.len){
                  def m = maps[k]
                  if(is_dict(m)){ out = out.append({"mesh": mi, "primitive": pi, "material": int(m.get("material", -1)), "variants": m.get("variants", [])}) }
                  k += 1
               }
            }
            pi += 1
         }
      }
      mi += 1
   }
   out
}

fn gltf_node_instancing_info(any: gltf_data, int: node_idx): any {
   "Parses EXT_mesh_gpu_instancing accessor indices for a node. The renderer can later expand or draw indirect from these accessors."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def nodes = g.get("nodes", [])
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return 0 }
   def node = nodes[node_idx]
   def ext = is_dict(node) ? node.get("extensions", 0) : 0
   def inst = is_dict(ext) ? ext.get("EXT_mesh_gpu_instancing", 0) : 0
   if(!is_dict(inst)){ return 0 }
   def attrs = inst.get("attributes", 0)
   {
      "translation_accessor": is_dict(attrs) ? int(attrs.get("TRANSLATION", -1)) : -1,
      "rotation_accessor": is_dict(attrs) ? int(attrs.get("ROTATION", -1)) : -1,
      "scale_accessor": is_dict(attrs) ? int(attrs.get("SCALE", -1)) : -1,
      "_attributes": attrs
   }
}

fn gltf_material_extension_info(any: gltf_data, int: material_idx): any {
   "Returns the raw parsed extension payload for one material plus normalized texture slots. Safe scaffolding for next renderer pass."
   def info = gltf_material_info(gltf_data, material_idx)
   if(!is_dict(info)){ return 0 }
   mut out = dict(64)
   def keys = ["clearcoat_factor", "clearcoat_roughness_factor", "anisotropy_strength", "anisotropy_rotation", "dispersion", "transmission_factor", "ior", "sheen_roughness_factor", "iridescence_factor", "iridescence_ior", "iridescence_thickness_min", "iridescence_thickness_max", "thickness_factor", "attenuation_distance", "specular_factor", "diffuse_transmission_factor", "diffuse_transmission_color_factor", "alpha_coverage"]
   def keys_n = keys.len
   mut i = 0
   while(i < keys_n){
      def k = to_str(keys[i])
      if(info.contains(k)){ out[k] = info.get(k, 0) }
      i += 1
   }
   out["material"] = material_idx
   out["all"] = info
   out
}

fn _gltf_ext_dict(any: ext, str: key): any {
   if(!is_dict(ext)){ return 0 }
   def v = ext.get(key, 0)
   is_dict(v) ? v : 0
}

fn _gltf_ext_float(dict: out, any: ext, str: out_key, str: src_key, f64: default): dict {
   out[out_key] = float(ext.get(src_key, default))
   out
}

fn _gltf_ext_vec3(dict: out, any: ext, str: out_key, str: src_key, f64: x, f64: y, f64: z): dict {
   out[out_key] = _gltf_vec3(ext.get(src_key, [x, y, z]), x, y, z)
   out
}

fn _gltf_ext_tex_uv(dict: out, any: ext, any: gltf_data, str: prefix, str: tex_key): dict {
   _gltf_tex_uri_uv(out, prefix, gltf_data, ext.get(tex_key, 0))
}

fn _gltf_ext_tex_info(dict: out, any: ext, any: gltf_data, str: prefix, str: tex_key): dict {
   _gltf_tex_info(out, prefix, gltf_data, ext.get(tex_key, 0))
}

fn _gltf_material_apply_extensions(dict: out, any: ext, any: ext_sg, bool: use_spec_gloss, any: gltf_data): dict {
   if(!is_dict(ext)){ return out }
   if(use_spec_gloss && is_dict(ext_sg)){
      out = _gltf_ext_vec3(out, ext_sg, "specular_color_factor", "specularFactor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_info(out, ext_sg, gltf_data, "specular_color", "specularGlossinessTexture")
   }
   if(ext.contains("KHR_materials_unlit")){ out["unlit"] = true }
   def ext_es = _gltf_ext_dict(ext, "KHR_materials_emissive_strength")
   if(is_dict(ext_es)){ out = _gltf_ext_float(out, ext_es, "emissive_strength", "emissiveStrength", 1.0) }
   def ext_cc = _gltf_ext_dict(ext, "KHR_materials_clearcoat")
   if(is_dict(ext_cc)){
      out = _gltf_ext_float(out, ext_cc, "clearcoat_factor", "clearcoatFactor", 0.0)
      out = _gltf_ext_float(out, ext_cc, "clearcoat_roughness_factor", "clearcoatRoughnessFactor", 0.0)
      out["clearcoat_normal_scale"] = is_dict(ext_cc.get("clearcoatNormalTexture", 0)) ? float(ext_cc.get("clearcoatNormalTexture", 0).get("scale", 1.0)) : 1.0
      out = _gltf_ext_tex_uv(out, ext_cc, gltf_data, "clearcoat", "clearcoatTexture")
      out = _gltf_ext_tex_uv(out, ext_cc, gltf_data, "clearcoat_roughness", "clearcoatRoughnessTexture")
      out = _gltf_ext_tex_uv(out, ext_cc, gltf_data, "clearcoat_normal", "clearcoatNormalTexture")
   }
   def ext_an = _gltf_ext_dict(ext, "KHR_materials_anisotropy")
   if(is_dict(ext_an)){
      out = _gltf_ext_float(out, ext_an, "anisotropy_strength", "anisotropyStrength", 0.0)
      out = _gltf_ext_float(out, ext_an, "anisotropy_rotation", "anisotropyRotation", 0.0)
      out = _gltf_ext_tex_uv(out, ext_an, gltf_data, "anisotropy", "anisotropyTexture")
   }
   def ext_dp = _gltf_ext_dict(ext, "KHR_materials_dispersion")
   if(is_dict(ext_dp)){ out = _gltf_ext_float(out, ext_dp, "dispersion", "dispersion", 0.0) }
   def ext_tr = _gltf_ext_dict(ext, "KHR_materials_transmission")
   if(is_dict(ext_tr)){
      out = _gltf_ext_float(out, ext_tr, "transmission_factor", "transmissionFactor", 0.0)
      out = _gltf_ext_tex_uv(out, ext_tr, gltf_data, "transmission", "transmissionTexture")
   }
   def ext_ior = _gltf_ext_dict(ext, "KHR_materials_ior")
   if(is_dict(ext_ior)){ out = _gltf_ext_float(out, ext_ior, "ior", "ior", 1.5) }
   def ext_sh = _gltf_ext_dict(ext, "KHR_materials_sheen")
   if(is_dict(ext_sh)){
      out = _gltf_ext_vec3(out, ext_sh, "sheen_color_factor", "sheenColorFactor", 0.0, 0.0, 0.0)
      out = _gltf_ext_float(out, ext_sh, "sheen_roughness_factor", "sheenRoughnessFactor", 0.0)
      out = _gltf_ext_tex_uv(out, ext_sh, gltf_data, "sheen_color", "sheenColorTexture")
      out = _gltf_ext_tex_uv(out, ext_sh, gltf_data, "sheen_roughness", "sheenRoughnessTexture")
   }
   def ext_ir = _gltf_ext_dict(ext, "KHR_materials_iridescence")
   if(is_dict(ext_ir)){
      out = _gltf_ext_float(out, ext_ir, "iridescence_factor", "iridescenceFactor", 0.0)
      out = _gltf_ext_float(out, ext_ir, "iridescence_ior", "iridescenceIor", 1.3)
      out = _gltf_ext_float(out, ext_ir, "iridescence_thickness_min", "iridescenceThicknessMinimum", 100.0)
      out = _gltf_ext_float(out, ext_ir, "iridescence_thickness_max", "iridescenceThicknessMaximum", 400.0)
      out = _gltf_ext_tex_uv(out, ext_ir, gltf_data, "iridescence", "iridescenceTexture")
      out = _gltf_ext_tex_uv(out, ext_ir, gltf_data, "iridescence_thickness", "iridescenceThicknessTexture")
   }
   def ext_vol = _gltf_ext_dict(ext, "KHR_materials_volume")
   if(is_dict(ext_vol)){
      out = _gltf_ext_float(out, ext_vol, "thickness_factor", "thicknessFactor", 0.0)
      out = _gltf_ext_float(out, ext_vol, "attenuation_distance", "attenuationDistance", 0.0)
      out = _gltf_ext_vec3(out, ext_vol, "attenuation_color", "attenuationColor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_uv(out, ext_vol, gltf_data, "thickness", "thicknessTexture")
   }
   def ext_sp = _gltf_ext_dict(ext, "KHR_materials_specular")
   if(is_dict(ext_sp)){
      out = _gltf_ext_float(out, ext_sp, "specular_factor", "specularFactor", 1.0)
      out = _gltf_ext_vec3(out, ext_sp, "specular_color_factor", "specularColorFactor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_uv(out, ext_sp, gltf_data, "specular", "specularTexture")
      out = _gltf_ext_tex_uv(out, ext_sp, gltf_data, "specular_color", "specularColorTexture")
   }
   def ext_dt = _gltf_ext_dict(ext, "KHR_materials_diffuse_transmission")
   if(is_dict(ext_dt)){
      out = _gltf_ext_tex_info(out, ext_dt, gltf_data, "diffuse_transmission", "diffuseTransmissionTexture")
      out = _gltf_ext_tex_info(out, ext_dt, gltf_data, "diffuse_transmission_color", "diffuseTransmissionColorTexture")
      out = _gltf_ext_float(out, ext_dt, "diffuse_transmission_factor", "diffuseTransmissionFactor", 0.0)
      out = _gltf_ext_vec3(out, ext_dt, "diffuse_transmission_color_factor", "diffuseTransmissionColorFactor", 1.0, 1.0, 1.0)
   }
   def ext_vsc = _gltf_ext_dict(ext, "KHR_materials_volume_scatter")
   if(is_dict(ext_vsc)){
      out = _gltf_ext_vec3(out, ext_vsc, "volume_scatter_color_factor", "multiscatterColor", 1.0, 1.0, 1.0)
   }
   def ext_ac = _gltf_ext_dict(ext, "KHR_materials_alpha_coverage")
   if(is_dict(ext_ac)){ out = _gltf_ext_float(out, ext_ac, "alpha_coverage", "alphaCoverage", 1.0) }
   def ext_ref = _gltf_ext_dict(ext, "KHR_materials_refraction")
   if(is_dict(ext_ref)){
      out = _gltf_ext_float(out, ext_ref, "refraction_factor", "refractionFactor", 0.0)
      out = _gltf_ext_float(out, ext_ref, "refraction_roughness", "refractionRoughnessFactor", 0.0)
   }
   def ext_sss = _gltf_ext_dict(ext, "KHR_materials_subsurface")
   if(is_dict(ext_sss)){
      out = _gltf_ext_float(out, ext_sss, "subsurface_factor", "subsurfaceFactor", 0.0)
      out = _gltf_ext_vec3(out, ext_sss, "subsurface_color_factor", "subsurfaceColorFactor", 1.0, 1.0, 1.0)
      out = _gltf_ext_tex_info(out, ext_sss, gltf_data, "subsurface", "subsurfaceTexture")
   }
   out
}

fn gltf_material_info(any: gltf_data, int: material_idx): any {
   "Returns a normalized material record with glTF PBR factors and texture URIs."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def materials = g.get("materials")
   if(!is_list(materials) || material_idx < 0 || material_idx >= materials.len){ return 0 }
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
   if(use_spec_gloss){
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
   out["base_color_factor"] = _gltf_vec4(base_factor, 1.0, 1.0, 1.0, 1.0)
   out["metallic_factor"] = metallic_factor
   out["roughness_factor"] = roughness_factor
   out["specular_glossiness"] = use_spec_gloss
   out["emissive_factor"] = _gltf_vec3(emissive, 0.0, 0.0, 0.0)
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

fn _gltf_u8(any: v): int {
   band(int(clamp01(float(v)) * 255.0), 255)
}

fn _gltf_u8_round(any: v): int {
   band(int(clamp01(float(v)) * 255.0 + 0.5), 255)
}

fn _gltf_pack_u8x4(any: a, any: b, any: c, any: d): int {
   bor(bor(band(int(a), 255), bshl(band(int(b), 255), 8)),
   bor(bshl(band(int(c), 255), 16), bshl(band(int(d), 255), 24)))
}

fn _gltf_pack_vec3_lane(any: c, f64: default, int: lane3): int {
   _gltf_pack_u8x4(
      _gltf_u8(c.get(0, default)),
      _gltf_u8(c.get(1, default)),
      _gltf_u8(c.get(2, default)),
      lane3
   )
}

fn _gltf_pack_bsdf0(any: info): int {
   def spec = _gltf_u8(is_dict(info) ? info.get("specular_factor", 1.0) : 1.0)
   def sheen_r = _gltf_u8(is_dict(info) ? info.get("sheen_roughness_factor", 0.0) : 0.0)
   def _trans_v = is_dict(info) ? float(info.get("transmission_factor", 0.0)) : 0.0
   def trans = _gltf_u8(_trans_v)
   def iri = _gltf_u8(is_dict(info) ? info.get("iridescence_factor", 0.0) : 0.0)
   _gltf_pack_u8x4(spec, sheen_r, trans, iri)
}

fn _gltf_pack_bsdf1(any: info): int {
   def c = is_dict(info) ? info.get("specular_color_factor", [1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0]
   def ior_v = is_dict(info) ? float(info.get("ior", 1.5)) : 1.5
   _gltf_pack_vec3_lane(c, 1.0, _gltf_u8((ior_v - 1.0) / 1.5))
}

fn _gltf_vec3_needs_factor(any: c, f64: default_v): bool {
   if(!is_list(c)){ return false }
   abs(float(c.get(0, default_v)) - default_v) > 0.003 ||
   abs(float(c.get(1, default_v)) - default_v) > 0.003 ||
   abs(float(c.get(2, default_v)) - default_v) > 0.003
}

fn _gltf_pack_bsdf2(any: info): int {
   mut c = is_dict(info) ? info.get("sheen_color_factor", [0.0, 0.0, 0.0]) : [0.0, 0.0, 0.0]
   if(is_dict(info) && float(info.get("diffuse_transmission_factor", 0.0)) > 0.0){
      def dtc = info.get("diffuse_transmission_color_factor", [1.0, 1.0, 1.0])
      if(_gltf_vec3_needs_factor(dtc, 1.0)){ c = dtc }
   }
   mut thickness_v = is_dict(info) ? float(info.get("thickness_factor", 0.0)) : 0.0
   if(thickness_v < 0.0){ thickness_v = 0.0 } elif(thickness_v > 4.0){ thickness_v = 4.0 }
   mut thickness = _gltf_u8_round(thickness_v / 4.0)
   if(thickness_v > 0.0 && thickness == 0){ thickness = 1 }
   _gltf_pack_vec3_lane(c, 0.0, thickness)
}

fn _gltf_pack_bsdf3(any: info): int {
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
   if(use_iri_pack){
      mut iri_ior = float(info.get("iridescence_ior", 1.3))
      mut iri_min = float(info.get("iridescence_thickness_min", 100.0))
      mut iri_max = float(info.get("iridescence_thickness_max", 400.0))
      if(iri_ior < 1.0){ iri_ior = 1.0 } elif(iri_ior > 3.0){ iri_ior = 3.0 }
      if(iri_min < 0.0){ iri_min = 0.0 } elif(iri_min > 800.0){ iri_min = 800.0 }
      if(iri_max < 0.0){ iri_max = 0.0 } elif(iri_max > 800.0){ iri_max = 800.0 }
      def iri_ior_u8 = _gltf_u8_round((iri_ior - 1.0) / 2.0)
      def iri_min_u8 = _gltf_u8_round(iri_min / 800.0)
      def iri_max_u8 = _gltf_u8_round(iri_max / 800.0)
      return _gltf_pack_u8x4(iri_ior_u8, iri_min_u8, iri_max_u8, 254)
   }
   mut att_u8 = 255
   if(att_dist > 0.0){
      att_u8 = band(int(sqrt(clamp01(att_dist / 10.0)) * 253.0 + 0.5), 255)
      if(att_u8 < 1){ att_u8 = 1 }
      if(att_u8 > 253){ att_u8 = 253 }
   }
   _gltf_pack_u8x4(_gltf_u8(att_r), _gltf_u8(att_g), _gltf_u8(att_b), att_u8)
}

fn _gltf_pack_bsdf4(any: info): int {
   def clearcoat = _gltf_u8(is_dict(info) ? info.get("clearcoat_factor", 0.0) : 0.0)
   def clearcoat_rough = _gltf_u8(is_dict(info) ? info.get("clearcoat_roughness_factor", 0.0) : 0.0)
   def anisotropy = _gltf_u8(is_dict(info) ? info.get("anisotropy_strength", 0.0) : 0.0)
   def dispersion = _gltf_u8(is_dict(info) ? (float(info.get("dispersion", 0.0)) / 10.0) : 0.0)
   _gltf_pack_u8x4(clearcoat, clearcoat_rough, anisotropy, dispersion)
}

fn _gltf_pack_bsdf5(any: info): int {
   def diffuse_trans = _gltf_u8(is_dict(info) ? info.get("diffuse_transmission_factor", 0.0) : 0.0)
   def refraction = _gltf_u8(is_dict(info) ? info.get("refraction_factor", 0.0) : 0.0)
   def subsurface = _gltf_u8(is_dict(info) ? info.get("subsurface_factor", 0.0) : 0.0)
   def alpha_coverage = _gltf_u8(is_dict(info) ? info.get("alpha_coverage", 1.0) : 1.0)
   _gltf_pack_u8x4(diffuse_trans, refraction, subsurface, alpha_coverage)
}

fn _gltf_keep_bsdf_record_value(int: record_word, int: material_word): int {
   record_word != 0 ? record_word : material_word
}

fn _gltf_keep_ext2_record_value(int: record_word, int: material_word): int{ record_word != 0x80000000 ? record_word : material_word }

fn _gltf_pack_alpha_ext_flags(any: info): int {
   if(!is_dict(info)){ return 0 }
   def dt = float(info.get("diffuse_transmission_factor", 0.0))
   def dtc = info.get("diffuse_transmission_color_factor", [1.0, 1.0, 1.0])
   mut flags = 0
   if(dt > 0.0 && _gltf_vec3_needs_factor(dtc, 1.0)){ flags = bor(flags, 0x80000000) }
   flags
}

fn _gltf_pack_emissive_word(any: info): int {
   mut ef = [0.0, 0.0, 0.0]
   mut strength = 1.0
   if(is_dict(info)){
      ef = info.get("emissive_factor", ef)
      strength = float(info.get("emissive_strength", 1.0))
   }
   if(strength < 0.0){ strength = 0.0 }
   mut r, g = float(ef.get(0, 0.0)) * strength, float(ef.get(1, 0.0)) * strength
   mut b = float(ef.get(2, 0.0)) * strength
   if(r < 0.0){ r = 0.0 }
   if(g < 0.0){ g = 0.0 }
   if(b < 0.0){ b = 0.0 }
   mut peak = max(r, max(g, b))
   if(peak <= 0.000001){ return 0 }
   if(peak > 64.0){ peak = 64.0 }
   def scale_u8 = band(int((peak / 64.0) * 255.0 + 0.5), 255)
   def rn = band(int((clamp01(r / peak) * 255.0 + 0.5)), 255)
   def gn = band(int((clamp01(g / peak) * 255.0 + 0.5)), 255)
   def bn = band(int((clamp01(b / peak) * 255.0 + 0.5)), 255)
   bor(bor(rn, bshl(gn, 8)), bor(bshl(bn, 16), bshl(scale_u8, 24)))
}

fn gltf_material_infos(any: gltf_data): list {
   "Returns normalized glTF material records suitable for future PBR/IBL pipeline binding."
   _gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   def materials = g.get("materials")
   if(!is_list(materials)){ return [] }
   def materials_n = materials.len
   mut cache_key = _gltf_cache_key_from_data(gltf_data)
   if(_gltf_material_infos_cache.contains(cache_key)){ return _gltf_material_infos_cache.get(cache_key, []) }
   mut out = list(materials_n)
   mut i = 0
   while(i < materials_n){
      out = out.append(gltf_material_info(gltf_data, i))
      i += 1
   }
   _gltf_material_infos_cache = cache.cache_put_reset(_gltf_material_infos_cache,
      cache_key,
      out,
      _GLTF_CACHE_LIMIT_SMALL,
   32)
   out
}

fn gltf_material_infos_limited(any: gltf_data, int: limit=0): list {
   "Returns normalized glTF material records, capped to `limit` when positive."
   _gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   def materials = g.get("materials")
   if(!is_list(materials)){ return [] }
   def materials_n = materials.len
   if(limit <= 0 || limit >= materials_n){ return gltf_material_infos(gltf_data) }
   def lim = max(0, limit)
   mut cache_key = _gltf_cache_key_from_data(gltf_data)
   cache_key = cache_key + "|limit=" + to_str(lim)
   if(_gltf_material_infos_cache.contains(cache_key)){ return _gltf_material_infos_cache.get(cache_key, []) }
   mut out = list(lim)
   mut i = 0
   while(i < lim){
      out = out.append(gltf_material_info(gltf_data, i))
      i += 1
   }
   _gltf_material_infos_cache = cache.cache_put_reset(_gltf_material_infos_cache,
      cache_key,
      out,
      _GLTF_CACHE_LIMIT_SMALL,
   32)
   out
}

fn gltf_material_texture_uris(any: gltf_data): list {
   "Returns base-color texture URIs for each material, resolved relative to the glTF base path."
   def infos = gltf_material_infos(gltf_data)
   if(!is_list(infos)){ return [] }
   def info_n = infos.len
   mut out = list(info_n)
   mut i = 0
   while(i < info_n){
      def info = infos[i]
      out = out.append(is_dict(info) ? to_str(info.get("base_color_uri", "")) : "")
      i += 1
   }
   out
}

fn gltf_skin_count(any: gltf_data): int {
   "Returns number of skin objects in the glTF asset."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def skins = g.get("skins", 0)
   is_list(skins) ? skins.len : 0
}

fn gltf_skin_info(any: gltf_data, int: skin_idx): any {
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def skins = g.get("skins", [])
   if(!is_list(skins) || skin_idx < 0 || skin_idx >= skins.len){ return 0 }
   def skin = skins.get(skin_idx, 0)
   if(!is_dict(skin)){ return 0 }
   {
      "index": skin_idx,
      "name": to_str(skin.get("name", "")),
      "skeleton": int(skin.get("skeleton", -1)),
      "joints": skin.get("joints", []),
      "inverse_bind_accessor": int(skin.get("inverseBindMatrices", -1))
   }
}

fn gltf_camera_count(any: gltf_data): int {
   def g = gltf_data.get("gltf", 0)
   def cams = is_dict(g) ? g.get("cameras", []) : []
   is_list(cams) ? cams.len : 0
}

fn gltf_camera_info(any: gltf_data, int: cam_idx): any {
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def cams = g.get("cameras", [])
   if(!is_list(cams) || cam_idx < 0 || cam_idx >= cams.len){ return 0 }
   def cam = cams[cam_idx]
   if(!is_dict(cam)){ return 0 }
   def typ = to_str(cam.get("type", ""))
   if(eq(typ, "perspective")){
      def p = cam.get("perspective", 0)
      def aspect_ratio = is_dict(p) ? p.get("aspectRatio", 0) : 0
      def yfov = is_dict(p) ? float(p.get("yfov", 0.0)) : 0.0
      def znear = is_dict(p) ? float(p.get("znear", 0.0)) : 0.0
      def zfar = is_dict(p) ? p.get("zfar", 0) : 0
      return {"index": cam_idx,
         "name": to_str(cam.get("name",
         "")),
         "type": typ,
         "aspect_ratio": aspect_ratio,
         "yfov": yfov,
         "znear": znear,
      "zfar": zfar}
   }
   if(eq(typ, "orthographic")){
      def o = cam.get("orthographic", 0)
      def xmag = is_dict(o) ? float(o.get("xmag", 0.0)) : 0.0
      def ymag = is_dict(o) ? float(o.get("ymag", 0.0)) : 0.0
      def znear = is_dict(o) ? float(o.get("znear", 0.0)) : 0.0
      def zfar = is_dict(o) ? float(o.get("zfar", 0.0)) : 0.0
      return {"index": cam_idx,
         "name": to_str(cam.get("name",
         "")),
         "type": typ,
         "xmag": xmag,
         "ymag": ymag,
         "znear": znear,
      "zfar": zfar}
   }
   {"index": cam_idx, "name": to_str(cam.get("name", "")), "type": typ}
}

fn gltf_camera_instances(any: gltf_data): list {
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   def nodes = g.get("nodes", [])
   if(!is_list(nodes)){ return [] }
   def nodes_n = nodes.len
   mut node_world_mats = dict(max(16, nodes_n * 2))
   def scenes = g.get("scenes", [])
   def scene_idx = _gltf_active_scene_idx(g, gltf_data)
   if(is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len){
      def scene = scenes[scene_idx]
      def roots = is_dict(scene) ? scene.get("nodes", []) : []
      def roots_n = is_list(roots) ? roots.len : 0
      mut ri = 0
      while(ri < roots_n){
         node_world_mats = _gltf_build_node_world_mats(g, int(roots[ri]), gltf_math.mat4_identity(), node_world_mats)
         ri += 1
      }
   }
   mut out = []
   mut i = 0
   while(i < nodes_n){
      def node = nodes[i]
      def cam_idx = is_dict(node) ? int(node.get("camera", -1)) : -1
      if(cam_idx >= 0){ out = out.append({"node_idx": i, "camera_idx": cam_idx, "camera": gltf_camera_info(gltf_data, cam_idx), "world_matrix": node_world_mats.get(i, gltf_math.mat4_identity())}) }
      i += 1
   }
   out
}

fn gltf_morph_target_count(any: gltf_data): int {
   "Returns the maximum morph target count found on any primitive."
   def meshes = _gltf_meshes(gltf_data)
   if(!meshes){ return 0 }
   def meshes_n = meshes.len
   mut best = 0
   mut mi = 0
   while(mi < meshes_n){
      def mesh = meshes[mi]
      def prims = is_dict(mesh) ? mesh.get("primitives", 0) : 0
      if(is_list(prims)){
         def prims_n = prims.len
         mut pi = 0
         while(pi < prims_n){
            def prim = prims[pi]
            def targets = is_dict(prim) ? prim.get("targets", 0) : 0
            if(is_list(targets)){
               def targets_n = targets.len
               if(targets_n > best){ best = targets_n }
            }
            pi += 1
         }
      }
      mi += 1
   }
   best
}

fn _gltf_meshes(any: gltf_data): any {
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def meshes = g.get("meshes")
   is_list(meshes) ? meshes : 0
}

fn gltf_mesh_count(any: gltf_data): int {
   def meshes = _gltf_meshes(gltf_data)
   meshes ? meshes.len : 0
}

fn gltf_get_mesh(any: gltf_data, int: mesh_idx): any {
   def meshes = _gltf_meshes(gltf_data)
   if(!meshes || mesh_idx < 0 || mesh_idx >= meshes.len){ return 0 }
   meshes.get(mesh_idx)
}

fn gltf_mesh_to_vertices(any: gltf_data, int: mesh_idx): dict { {"vertices": list(0), "indices": list(0), "vertex_count": 0, "index_count": 0} }

fn gltf_mesh_to_vertex_buffers(any: gltf_data, int: mesh_idx, any: color=0, any: material_tex_ids=0): list {
   "Builds one expanded vertex buffer per glTF primitive/material."
   def g = gltf_data.get("gltf", 0)
   def data = _gltf_primary_data_ptr(gltf_data)
   if(!is_dict(g) || !data){ return [] }
   def meshes = g.get("meshes")
   if(!is_list(meshes) || mesh_idx < 0 || mesh_idx >= meshes.len){ return [] }
   def mesh = meshes.get(mesh_idx)
   def primitives = mesh.get("primitives")
   def instance_mats = _gltf_mesh_instance_mats(g, mesh_idx)
   if(!is_list(primitives) || primitives.len == 0 || !is_list(instance_mats) || instance_mats.len == 0){ return [] }
   def primitives_n = primitives.len
   def instance_mats_n = instance_mats.len
   def accs = g.get("accessors")
   def bv_list = g.get("bufferViews")
   def accs_n = is_list(accs) ? accs.len : 0
   def bv_n = is_list(bv_list) ? bv_list.len : 0
   def material_tex_ids_n = is_list(material_tex_ids) ? material_tex_ids.len : 0
   mut out = list(0)
   mut pi = 0
   while(pi < primitives_n){
      def prim = primitives.get(pi)
      def ps = _gltf_legacy_prim_state(prim, data, accs, bv_list, accs_n, bv_n, material_tex_ids, material_tex_ids_n)
      def mat_idx = int(ps.get("mat_idx", -1))
      def prim_tex_id = int(ps.get("prim_tex_id", 0))
      def pos_total = int(ps.get("pos_total", 0))
      def pos_elem = int(ps.get("pos_elem", 0))
      def pos_cnt = int(ps.get("pos_cnt", 0))
      def pos_comp = int(ps.get("pos_comp", 0))
      def pos_cs = int(ps.get("pos_cs", 0))
      def uv_total = int(ps.get("uv_total", 0))
      def uv_elem = int(ps.get("uv_elem", 0))
      def uv_cnt = int(ps.get("uv_cnt", 0))
      def uv_comp = int(ps.get("uv_comp", 0))
      def uv_cs = int(ps.get("uv_cs", 0))
      def norm_total = int(ps.get("norm_total", 0))
      def norm_elem = int(ps.get("norm_elem", 0))
      def norm_cnt = int(ps.get("norm_cnt", 0))
      def norm_comp = int(ps.get("norm_comp", 0))
      def norm_cs = int(ps.get("norm_cs", 0))
      def c_res = ps.get("c_res", 0)
      def idx_base = int(ps.get("idx_base", 0))
      def idx_comp = int(ps.get("idx_comp", 0))
      def has_idx = ps.get("has_idx", false)
      def prim_count = int(ps.get("prim_count", 0))
      def total_verts = prim_count * instance_mats_n
      if(total_verts <= 0){
         pi += 1
         continue
      }
      def buf = malloc(total_verts * _GLTF_VTX_STRIDE)
      if(!buf){
         pi += 1
         continue
      }
      mut vert_off = 0
      mut min_x, min_y, min_z = 1e9, 1e9, 1e9
      mut max_x, max_y, max_z = -1e9, -1e9, -1e9
      mut mi = 0
      while(mi < instance_mats_n){
         def mesh_m = instance_mats.get(mi)
         mut src_i = 0
         while(src_i < prim_count){
            mut vi = src_i
            if(has_idx){ vi = _gltf_get_index_fast(data, idx_base, src_i, idx_comp) }
            if(vi >= 0 && vi < pos_cnt){
               def sample = _gltf_emit_legacy_vertex(buf, vert_off * _GLTF_VTX_STRIDE, data, ps, mesh_m, vi, prim_tex_id, false)
               def px, py = sample.get(0), sample.get(1)
               def pz = sample.get(2)
               if(px < min_x){ min_x = px } if(py < min_y){ min_y = py } if(pz < min_z){ min_z = pz }
               if(px > max_x){ max_x = px } if(py > max_y){ max_y = py } if(pz > max_z){ max_z = pz }
               vert_off += 1
            }
            src_i += 1
         }
         mi += 1
      }
      if(vert_off > 0){
         out = out.append({
               "ptr": buf,
               "count": vert_off,
               "min": [min_x, min_y, min_z],
               "max": [max_x, max_y, max_z],
               "tex_id": prim_tex_id,
               "material_idx": mat_idx
         })
      } else {
         free(buf)
      }
      pi += 1
   }
   out
}

fn gltf_to_vertex_buffer(any: gltf_data, int: mesh_idx, any: color=0, any: material_tex_ids=0): dict {
   def g = gltf_data.get("gltf", 0)
   def data = _gltf_primary_data_ptr(gltf_data)
   if(!is_dict(g) || !data){ return {"error": "Invalid glTF data"} }
   def meshes = g.get("meshes")
   if(!is_list(meshes) || mesh_idx < 0 || mesh_idx >= meshes.len){ return {"error": "Invalid mesh index"} }
   def mesh = meshes.get(mesh_idx)
   def primitives = mesh.get("primitives")
   def instance_mats = _gltf_mesh_instance_mats(g, mesh_idx)
   if(!is_list(primitives) || primitives.len == 0){ return {"error": "No primitives in mesh"} }
   def primitives_n = primitives.len
   def instance_mats_n = instance_mats.len
   def accs = g.get("accessors")
   def bv_list = g.get("bufferViews")
   def accs_n = is_list(accs) ? accs.len : 0
   def bv_n = is_list(bv_list) ? bv_list.len : 0
   def material_tex_ids_n = is_list(material_tex_ids) ? material_tex_ids.len : 0
   mut total_verts = _gltf_legacy_mesh_total_vertices(primitives, accs, accs_n)
   total_verts = total_verts * instance_mats_n
   if(total_verts <= 0){ return {"error": "No vertices"} }
   def buf = malloc(total_verts * _GLTF_VTX_STRIDE)
   if(!buf){ return {"error": "Vertex buffer allocation failed"} }
   mut vert_off = 0
   mut min_x, min_y, min_z = 1e9, 1e9, 1e9
   mut max_x, max_y, max_z = -1e9, -1e9, -1e9
   mut mi = 0
   while(mi < instance_mats_n){
      def mesh_m = instance_mats.get(mi)
      mut pi = 0
      while(pi < primitives_n){
         def prim = primitives.get(pi)
         def ps = _gltf_legacy_prim_state(prim, data, accs, bv_list, accs_n, bv_n, material_tex_ids, material_tex_ids_n)
         def prim_tex_id = int(ps.get("prim_tex_id", 0))
         def pos_cnt = int(ps.get("pos_cnt", 0))
         def idx_base = int(ps.get("idx_base", 0))
         def idx_comp = int(ps.get("idx_comp", 0))
         def has_idx = ps.get("has_idx", false)
         def prim_count = int(ps.get("prim_count", 0))
         mut src_i = 0
         while(src_i < prim_count){
            mut vi = src_i
            if(has_idx){ vi = _gltf_get_index_fast(data, idx_base, src_i, idx_comp) }
            if(vi >= 0 && vi < pos_cnt){
               def sample = _gltf_emit_legacy_vertex(buf, vert_off, data, ps, mesh_m, vi, prim_tex_id, true)
               def px, py = sample.get(0), sample.get(1)
               def pz = sample.get(2)
               if(px < min_x){ min_x = px } if(px > max_x){ max_x = px }
               if(py < min_y){ min_y = py } if(py > max_y){ max_y = py }
               if(pz < min_z){ min_z = pz } if(pz > max_z){ max_z = pz }
               vert_off += _GLTF_VTX_STRIDE
            }
            src_i += 1
         }
         pi += 1
      }
      mi += 1
   }
   def out = {"ptr": buf, "count": total_verts, "min": [min_x, min_y, min_z], "max": [max_x, max_y, max_z]}
   out
}

fn _gltf_accessor_local_bounds(any: acc): any {
   if(!is_dict(acc)){ return 0 }
   def mn, mx = acc.get("min", 0), acc.get("max", 0)
   if(is_list(mn) && is_list(mx) && mn.len >= 3 && mx.len >= 3){
      def x1 = _gltf_num_or(mn.get(0, 0.0), 0.0) def y1 = _gltf_num_or(mn.get(1, 0.0), 0.0) def z1 = _gltf_num_or(mn.get(2, 0.0), 0.0)
      def x2 = _gltf_num_or(mx.get(0, 0.0), 0.0) def y2 = _gltf_num_or(mx.get(1, 0.0), 0.0) def z2 = _gltf_num_or(mx.get(2, 0.0), 0.0)
      if(_gltf_float_bad(x1) || _gltf_float_bad(y1) || _gltf_float_bad(z1) ||
      _gltf_float_bad(x2) || _gltf_float_bad(y2) || _gltf_float_bad(z2)){ return 0 }
      if(x1 > x2 || y1 > y2 || z1 > z2){ return 0 }
      return [[x1, y1, z1], [x2, y2, z2]]
   }
   0
}

fn _gltf_transform_aabb(any: minv, any: maxv, any: m): list {
   def x1 = _gltf_num_or(minv.get(0, 0.0), 0.0) def y1 = _gltf_num_or(minv.get(1, 0.0), 0.0) def z1 = _gltf_num_or(minv.get(2, 0.0), 0.0)
   def x2 = _gltf_num_or(maxv.get(0, 0.0), 0.0) def y2 = _gltf_num_or(maxv.get(1, 0.0), 0.0) def z2 = _gltf_num_or(maxv.get(2, 0.0), 0.0)
   if(_gltf_float_bad(x1) || _gltf_float_bad(y1) || _gltf_float_bad(z1) ||
   _gltf_float_bad(x2) || _gltf_float_bad(y2) || _gltf_float_bad(z2)){ return [] }
   if(x1 > x2 || y1 > y2 || z1 > z2){ return [] }
   def mm = _gltf_mat3x4_num(m)
   def m00, m01 = mm.get(0), mm.get(1)
   def m02, m03 = mm.get(2), mm.get(3)
   def m10, m11 = mm.get(4), mm.get(5)
   def m12, m13 = mm.get(6), mm.get(7)
   def m20, m21 = mm.get(8), mm.get(9)
   def m22, m23 = mm.get(10), mm.get(11)
   if(_gltf_float_bad(m00) || _gltf_float_bad(m01) || _gltf_float_bad(m02) || _gltf_float_bad(m03) ||
      _gltf_float_bad(m10) || _gltf_float_bad(m11) || _gltf_float_bad(m12) || _gltf_float_bad(m13) ||
   _gltf_float_bad(m20) || _gltf_float_bad(m21) || _gltf_float_bad(m22) || _gltf_float_bad(m23)){ return [] }
   mut wmin_x, wmin_y, wmin_z = 1e9, 1e9, 1e9
   mut wmax_x, wmax_y, wmax_z = -1e9, -1e9, -1e9
   mut used = 0
   mut ci = 0
   while(ci < 8){
      def px, py = (band(ci, 1) != 0) ? x2 : x1, (band(ci, 2) != 0) ? y2 : y1
      def pz = (band(ci, 4) != 0) ? z2 : z1
      def wx = 0.0 + (m00 * px + m01 * py + m02 * pz + m03)
      def wy = 0.0 + (m10 * px + m11 * py + m12 * pz + m13)
      def wz = 0.0 + (m20 * px + m21 * py + m22 * pz + m23)
      if(!_gltf_float_bad(wx) && !_gltf_float_bad(wy) && !_gltf_float_bad(wz)){
         if(wx < wmin_x){ wmin_x = 0.0 + wx } if(wx > wmax_x){ wmax_x = 0.0 + wx }
         if(wy < wmin_y){ wmin_y = 0.0 + wy } if(wy > wmax_y){ wmax_y = 0.0 + wy }
         if(wz < wmin_z){ wmin_z = 0.0 + wz } if(wz > wmax_z){ wmax_z = 0.0 + wz }
         used += 1
      }
      ci += 1
   }
   if(used <= 0 || wmin_x > wmax_x || wmin_y > wmax_y || wmin_z > wmax_z){ return [] }
   return [[wmin_x, wmin_y, wmin_z], [wmax_x, wmax_y, wmax_z]]
}

@jit
fn _gltf_model_has_negative_det(any: m): bool {
   if(!is_list(m) || m.len < 16){ return false }
   def mm = _gltf_mat3_num(m)
   def m00, m01 = mm.get(0), mm.get(1)
   def m02 = mm.get(2)
   def m10, m11 = mm.get(3), mm.get(4)
   def m12 = mm.get(5)
   def m20, m21 = mm.get(6), mm.get(7)
   def m22 = mm.get(8)
   def det = m00 * (m11 * m22 - m12 * m21) -
   m01 * (m10 * m22 - m12 * m20) +
   m02 * (m10 * m21 - m11 * m20)
   det < 0.0
}

@jit
fn _gltf_model_mean_scale(any: m): f64 {
   if(!is_list(m) || m.len < 16){ return 1.0 }
   def mm = _gltf_mat3_num(m)
   def m00, m01 = mm.get(0), mm.get(1)
   def m02 = mm.get(2)
   def m10, m11 = mm.get(3), mm.get(4)
   def m12 = mm.get(5)
   def m20, m21 = mm.get(6), mm.get(7)
   def m22 = mm.get(8)
   def sx, sy = sqrt(m00 * m00 + m10 * m10 + m20 * m20), sqrt(m01 * m01 + m11 * m11 + m21 * m21)
   def sz = sqrt(m02 * m02 + m12 * m12 + m22 * m22)
   mut s = (sx + sy + sz) / 3.0
   if(_gltf_float_bad(s) || s <= 0.0){ return 1.0 }
   if(s < 0.001){ s = 0.001 } elif(s > 64.0){ s = 64.0 }
   s
}

@jit
fn _gltf_scale_bsdf2_thickness(any: bsdf2_u32, any: volume_scale): int {
   def word = int(bsdf2_u32)
   def thickness_u8 = band(bshr(word, 24), 255)
   if(thickness_u8 <= 0){ return word }
   mut scale = float(volume_scale)
   if(_gltf_float_bad(scale) || scale <= 0.0){ return word }
   if(scale > 0.997 && scale < 1.003){ return word }
   if(scale < 0.001){ scale = 0.001 } elif(scale > 64.0){ scale = 64.0 }
   mut scaled_u8 = int(float(thickness_u8) * scale + 0.5)
   if(scaled_u8 <= 0){ scaled_u8 = 1 } elif(scaled_u8 > 255){ scaled_u8 = 255 }
   bor(band(word, 0x00ffffff), bshl(scaled_u8, 24))
}

fn _gltf_prim_meta(dict: prim, list: accs): dict {
   def attrs = prim.get("attributes")
   def idx_acc = prim.get("indices", -1)
   def mat_idx = int(prim.get("material", -1))
   mut pos_acc_idx, pos_cnt = -1, 0
   mut uv_acc_idx, uv_cnt = -1, 0
   mut uv1_acc_idx, uv1_cnt = -1, 0
   mut c_acc_idx, c_cnt = -1, 0
   mut n_acc_idx, n_cnt = -1, 0
   mut i_acc_idx, i_cnt = -1, 0
   mut has_idx = false
   mut local_bounds = 0
   def prim_mode = int(prim.get("mode", GLTF_MODE_TRIANGLES))
   if(is_dict(attrs)){
      pos_acc_idx = attrs.get("POSITION", -1)
      if(pos_acc_idx >= 0 && pos_acc_idx < accs.len){
         def acc = accs.get(pos_acc_idx)
         pos_cnt = acc.get("count", 0)
         local_bounds = _gltf_accessor_local_bounds(acc)
      }
      uv_acc_idx = attrs.get("TEXCOORD_0", -1)
      if(uv_acc_idx >= 0 && uv_acc_idx < accs.len){
         def acc = accs.get(uv_acc_idx)
         uv_cnt = acc.get("count", 0)
      }
      uv1_acc_idx = attrs.get("TEXCOORD_1", -1)
      if(uv1_acc_idx >= 0 && uv1_acc_idx < accs.len){
         def acc = accs.get(uv1_acc_idx)
         uv1_cnt = acc.get("count", 0)
      }
      c_acc_idx = attrs.get("COLOR_0", -1)
      if(_gltf_is_debug()){
         print("[gltf:color] c_acc_idx=" + to_str(c_acc_idx) +
            " c_cnt=" + to_str(c_cnt) +
         " pos_cnt=" + to_str(pos_cnt))
      }
      if(c_acc_idx >= 0 && c_acc_idx < accs.len){
         def acc = accs.get(c_acc_idx)
         c_cnt = acc.get("count", 0)
         def c_type = to_str(acc.get("type", ""))
         def c_comp = int(acc.get("componentType", 0))
         if(_gltf_is_debug()){
            print("[gltf:color] type=" + c_type +
               " comp=" + to_str(c_comp) +
            " normalized=" + to_str(acc.get("normalized", false)))
         }
      }
      n_acc_idx = attrs.get("NORMAL", -1)
      if(n_acc_idx >= 0 && n_acc_idx < accs.len){
         def acc = accs.get(n_acc_idx)
         n_cnt = acc.get("count", 0)
      }
   }
   def texcoord_sets = _gltf_collect_indexed_semantics(attrs, "TEXCOORD")
   def color_sets = _gltf_collect_indexed_semantics(attrs, "COLOR")
   def joints_sets = _gltf_collect_indexed_semantics(attrs, "JOINTS")
   def weights_sets = _gltf_collect_indexed_semantics(attrs, "WEIGHTS")
   mut is_skinned_candidate = false
   def has_joints_sets = joints_sets.len > 0
   def has_weights_sets = weights_sets.len > 0
   if(has_joints_sets){ if(has_weights_sets){ is_skinned_candidate = true } }
   def t_acc_idx = is_dict(attrs) ? int(attrs.get("TANGENT", -1)) : -1
   mut t_cnt = 0
   if(t_acc_idx >= 0 && t_acc_idx < accs.len){
      def acc = accs.get(t_acc_idx)
      t_cnt = acc.get("count", 0)
   }
   if(idx_acc >= 0 && idx_acc < accs.len){
      def acc = accs.get(idx_acc)
      i_acc_idx = idx_acc
      i_cnt = acc.get("count", 0)
      has_idx = true
   }
   {
      "pos_acc_idx": pos_acc_idx,
      "pos_cnt": pos_cnt,
      "uv_acc_idx": uv_acc_idx,
      "uv_cnt": uv_cnt,
      "uv1_acc_idx": uv1_acc_idx,
      "uv1_cnt": uv1_cnt,
      "c_acc_idx": c_acc_idx,
      "c_cnt": c_cnt,
      "n_acc_idx": n_acc_idx,
      "n_cnt": n_cnt,
      "t_acc_idx": t_acc_idx,
      "t_cnt": t_cnt,
      "texcoord_sets": texcoord_sets,
      "color_sets": color_sets,
      "joints_sets": joints_sets,
      "weights_sets": weights_sets,
      "is_skinned_candidate": is_skinned_candidate,
      "joints_acc_idx": _gltf_first_set_acc(joints_sets, 0),
      "weights_acc_idx": _gltf_first_set_acc(weights_sets, 0),
      "i_acc_idx": i_acc_idx,
      "i_cnt": i_cnt,
      "has_idx": has_idx,
      "mat_idx": mat_idx,
      "local_bounds": local_bounds,
      "mode": prim_mode
   }
}

fn _gltf_mesh_morph_weights(any: g, int: mesh_idx, int: target_count): list {
   mut out = []
   mut i = 0
   while(i < target_count){ out = out.append(0.0) i += 1 }
   if(target_count <= 0){ return out }
   def meshes = g.get("meshes")
   if(!is_list(meshes) || mesh_idx < 0 || mesh_idx >= meshes.len){ return out }
   def mesh = meshes[mesh_idx]
   def weights = is_dict(mesh) ? mesh.get("weights", 0) : 0
   if(!is_list(weights)){ return out }
   def weights_n = weights.len
   i = 0
   while(i < target_count && i < weights_n){
      out[i] = float(weights[i])
      i += 1
   }
   out
}

fn _gltf_collect_indexed_semantics(any: attrs, str: prefix): list {
   mut out = []
   if(!is_dict(attrs)){ return out }
   mut idx = 0
   while(true){
      def key = prefix + "_" + to_str(idx)
      if(!attrs.contains(key)){ break }
      mut rec = dict(4)
      rec["set"] = idx
      rec["semantic"] = key
      rec["accessor"] = int(attrs.get(key, -1))
      out = out.append(rec)
      idx += 1
   }
   out
}

fn _gltf_first_set_acc(any: sets, int: set_idx): int {
   def sets_n = is_list(sets) ? sets.len : 0
   mut i = 0
   while(i < sets_n){
      def rec = sets[i]
      if(is_dict(rec) && int(rec.get("set", -1)) == set_idx){ return int(rec.get("accessor", -1)) }
      i += 1
   }
   -1
}

fn _gltf_release_vertex_pack_accessors(any: pos_res, any: uv0_res, any: uv1_res, any: c_res, any: n_res, any: t_res): int {
   _gltf_release_accessor_data(pos_res)
   _gltf_release_accessor_data(uv0_res)
   _gltf_release_accessor_data(uv1_res)
   _gltf_release_accessor_data(c_res)
   _gltf_release_accessor_data(n_res)
   _gltf_release_accessor_data(t_res)
   0
}

fn _gltf_collect_morph_targets(dict: g, any: data, any: targets, any: morph_weights): list {
   mut morph_targets = list(0)
   if(!is_list(targets)){ return morph_targets }
   def targets_n = targets.len
   mut ti = 0
   while(ti < targets_n){
      def target = targets[ti]
      def weight = is_list(morph_weights) ? float(morph_weights.get(ti, 0.0)) : 0.0
      if(is_dict(target) && (weight > 0.000001 || weight < -0.000001)){
         def t_pos_res = _gltf_resolve_accessor_data(g, int(target.get("POSITION", -1)), data)
         def t_norm_res = _gltf_resolve_accessor_data(g, int(target.get("NORMAL", -1)), data)
         if(is_dict(t_pos_res) || is_dict(t_norm_res)){
            morph_targets = morph_targets.append({"weight": weight, "pos_res": t_pos_res, "norm_res": t_norm_res})
         } else {
            _gltf_release_accessor_data(t_pos_res)
            _gltf_release_accessor_data(t_norm_res)
         }
      }
      ti += 1
   }
   morph_targets
}

fn _gltf_release_morph_targets(list: morph_targets): bool {
   def morph_targets_n = morph_targets.len
   mut mti = 0
   while(mti < morph_targets_n){
      def mt = morph_targets.get(mti, 0)
      _gltf_release_accessor_data(mt.get("pos_res", 0))
      _gltf_release_accessor_data(mt.get("norm_res", 0))
      mti += 1
   }
   true
}

fn _gltf_try_pack_vertices_pnc_raw(
   any: buf, int: count, any: pos_ptr, int: pos_comp, bool: pos_norm, int: pos_stride,
   bool: uv0_valid, int: uv0_comp, bool: uv0_norm, bool: uv1_valid,
   bool: n_valid, any: n_ptr, int: n_cnt, int: n_comp, bool: n_norm, int: n_stride,
   bool: t_valid, int: t_comp, bool: t_norm,
   bool: c_valid, any: c_ptr, int: c_cnt, int: c_stride, int: c_comp, int: c_type_count, bool: c_norm,
   int: tex_id, int: morph_targets_n
): bool {
   if(!pos_ptr || pos_comp != GLTF_COMP_FLOAT || pos_norm){ return false }
   if(uv0_valid && (uv0_comp != GLTF_COMP_FLOAT || uv0_norm)){ return false }
   if(n_valid && (n_comp != GLTF_COMP_FLOAT || n_norm)){ return false }
   if(t_valid && (t_comp != GLTF_COMP_FLOAT || t_norm)){ return false }
   if(common.env_truthy("NY_GLTF_PACK_NATIVE_OFF")){ return false }
   if(morph_targets_n != 0 || pos_stride < 12 || uv0_valid || uv1_valid || t_valid){ return false }
   if(n_valid && n_stride < 12){ return false }
   __gltf_pack_vertices_pnc_raw(buf,
      count,
      pos_ptr,
      pos_stride,
      n_valid ? n_ptr : 0,
      n_valid ? n_cnt : 0,
      n_valid ? n_stride : 0,
      c_valid ? c_ptr : 0,
      c_valid ? c_cnt : 0,
      c_valid ? c_stride : 0,
      c_valid ? c_comp : 0,
      c_valid ? c_type_count : 4,
      c_valid ? c_norm : false,
   tex_id)
}

fn _gltf_apply_morph_vec3(list: morph_targets, int: morph_targets_n, int: vi, f64: x, f64: y, f64: z, str: res_key): list {
   mut ox, oy = x, y
   mut oz, mi = z, 0
   while(mi < morph_targets_n){
      def mt = morph_targets[mi]
      def mt_w = float(mt.get("weight", 0.0))
      def mt_res = mt.get(res_key, 0)
      if(is_dict(mt_res) && vi < int(mt_res.get("count", 0))){
         def mt_ptr = mt_res.get("ptr", 0)
         def mt_comp = mt_res.get("comp", GLTF_COMP_FLOAT)
         def mt_norm = mt_res.get("normalized", false)
         def mt_stride = mt_res.get("stride", 0)
         def mt_cs = _gltf_comp_size(mt_comp)
         def mt_off = vi * mt_stride
         ox += _gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 0, mt_comp, mt_norm) * mt_w
         oy += _gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 1, mt_comp, mt_norm) * mt_w
         oz += _gltf_read_f32_acc(mt_ptr, mt_off + mt_cs * 2, mt_comp, mt_norm) * mt_w
      }
      mi += 1
   }
   [ox, oy, oz]
}

fn _gltf_pack_unique_vertices(dict: g, any: data, dict: meta, int: packed_color, int: tex_id, int: uv_set=0, int: uv_xform=0): any {
   def pos_res = _gltf_resolve_accessor_data(g, meta.get("pos_acc_idx", -1), data)
   def uv0_idx = meta.get("uv_acc_idx", -1)
   def uv1_idx = meta.get("uv1_acc_idx", -1)
   def uv0_res = _gltf_resolve_accessor_data(g, uv0_idx, data)
   def uv1_res = (uv1_idx >= 0 && uv1_idx != uv0_idx) ? _gltf_resolve_accessor_data(g, uv1_idx, data) : 0
   def c_res = _gltf_resolve_accessor_data(g, meta.get("c_acc_idx", -1), data)
   def n_res = _gltf_resolve_accessor_data(g, meta.get("n_acc_idx", -1), data)
   def t_res = _gltf_resolve_accessor_data(g, meta.get("t_acc_idx", -1), data)
   mut count = 0
   if(is_dict(pos_res)){ count = pos_res.get("count", 0) }
   if(count <= 0){
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      return 0
   }
   def buf = malloc(count * _GLTF_VTX_STRIDE)
   if(!buf){
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      return 0
   }
   def pos_ptr = pos_res.get("ptr", 0)
   def pos_comp = pos_res.get("comp", 0)
   def pos_norm = pos_res.get("normalized", false)
   def pos_stride = pos_res.get("stride", 0)
   mut uv0_ptr, uv0_cnt, uv0_comp, uv0_norm, uv0_stride = 0, 0, 0, false, 0
   if(is_dict(uv0_res)){
      uv0_ptr, uv0_cnt = uv0_res.get("ptr", 0), uv0_res.get("count", 0)
      uv0_comp, uv0_norm = uv0_res.get("comp", 0), uv0_res.get("normalized", false)
      uv0_stride = uv0_res.get("stride", 0)
   }
   mut uv1_ptr, uv1_cnt, uv1_comp, uv1_norm, uv1_stride = 0, 0, GLTF_COMP_FLOAT, false, 0
   if(is_dict(uv1_res)){
      uv1_ptr, uv1_cnt = uv1_res.get("ptr", 0), uv1_res.get("count", 0)
      uv1_comp, uv1_norm = uv1_res.get("comp", GLTF_COMP_FLOAT), uv1_res.get("normalized", false)
      uv1_stride = uv1_res.get("stride", 0)
   }
   def uv1_cs = _gltf_comp_size(uv1_comp)
   mut n_ptr, n_cnt, n_comp, n_norm, n_stride = 0, 0, 0, false, 0
   if(is_dict(n_res)){
      n_ptr, n_cnt = n_res.get("ptr", 0), n_res.get("count", 0)
      n_comp, n_norm = n_res.get("comp", 0), n_res.get("normalized", false)
      n_stride = n_res.get("stride", 0)
   }
   mut t_ptr, t_cnt, t_comp, t_norm, t_stride = 0, 0, 0, false, 0
   if(is_dict(t_res)){
      t_ptr, t_cnt = t_res.get("ptr", 0), t_res.get("count", 0)
      t_comp, t_norm = t_res.get("comp", 0), t_res.get("normalized", false)
      t_stride = t_res.get("stride", 0)
   }
   if(!pos_ptr){
      free(buf)
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      return 0
   }
   def morph_targets = _gltf_collect_morph_targets(g, data, meta.get("targets", 0), meta.get("morph_weights", 0))
   def morph_targets_n = morph_targets.len
   def pos_cs = _gltf_comp_size(pos_comp)
   def uv0_cs = _gltf_comp_size(uv0_comp)
   def n_cs = _gltf_comp_size(n_comp)
   def t_cs = _gltf_comp_size(t_comp)
   def uv0_valid = is_dict(uv0_res) && uv0_cnt > 0 && uv0_stride > 0 && uv0_cs > 0
   def uv1_valid = is_dict(uv1_res) && uv1_cnt > 0 && uv1_stride > 0 && uv1_cs > 0
   def n_valid = is_dict(n_res) && n_cnt > 0 && n_stride > 0 && n_cs > 0
   def t_valid = is_dict(t_res) && t_cnt > 0 && t_stride > 0 && t_cs > 0
   mut c_ptr, c_cnt, c_comp, c_stride, c_type_count, c_norm = 0, 0, 0, 0, 4, false
   if(is_dict(c_res)){
      c_ptr = c_res.get("ptr", 0)
      c_cnt = int(c_res.get("count", 0))
      c_comp = int(c_res.get("comp", 0))
      c_stride = int(c_res.get("stride", 0))
      c_type_count = int(c_res.get("type_count", 4))
      c_norm = c_res.get("normalized", false) || c_comp != GLTF_COMP_FLOAT
   }
   def c_valid = c_ptr && c_cnt > 0 && c_stride > 0 && (c_type_count == 3 || c_type_count == 4)
   if(_gltf_try_pack_vertices_pnc_raw(buf, count, pos_ptr, pos_comp, pos_norm, pos_stride,
         uv0_valid, uv0_comp, uv0_norm, uv1_valid, n_valid, n_ptr, n_cnt, n_comp, n_norm, n_stride,
         t_valid, t_comp, t_norm, c_valid, c_ptr, c_cnt, c_stride, c_comp, c_type_count, c_norm,
      tex_id, morph_targets_n)){
      _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
      mut out = dict(4)
      out["ptr"] = buf
      out["count"] = count
      return out
   }
   mut vi = 0
   if(vi < count){
      while(vi < count){
         def pbase = vi * pos_stride
         def px = _gltf_read_f32_acc(pos_ptr, pbase + pos_cs * 0, pos_comp, pos_norm)
         def py = _gltf_read_f32_acc(pos_ptr, pbase + pos_cs * 1, pos_comp, pos_norm)
         def pz = _gltf_read_f32_acc(pos_ptr, pbase + pos_cs * 2, pos_comp, pos_norm)
         def morph_pos = _gltf_apply_morph_vec3(morph_targets, morph_targets_n, vi, px, py, pz, "pos_res")
         def mx, my = float(morph_pos.get(0, px)), float(morph_pos.get(1, py))
         def mz = float(morph_pos.get(2, pz))
         mut nx, ny = 0.0, 0.0
         mut nz = 0.0
         if(n_valid && vi < n_cnt){
            def nbase = vi * n_stride
            nx = _gltf_read_f32_acc(n_ptr, nbase + n_cs * 0, n_comp, n_norm)
            ny = _gltf_read_f32_acc(n_ptr, nbase + n_cs * 1, n_comp, n_norm)
            nz = _gltf_read_f32_acc(n_ptr, nbase + n_cs * 2, n_comp, n_norm)
         }
         def morph_norm = _gltf_apply_morph_vec3(morph_targets, morph_targets_n, vi, nx, ny, nz, "norm_res")
         nx, ny = float(morph_norm.get(0, nx)), float(morph_norm.get(1, ny))
         nz = float(morph_norm.get(2, nz))
         def nl = sqrt(nx * nx + ny * ny + nz * nz)
         if(nl > 0.00001){ nx /= nl ny /= nl nz /= nl }
         mut tx, ty = 0.0, 0.0
         mut tz, tw = 0.0, 1.0
         if(t_valid && vi < t_cnt){
            def tbase = vi * t_stride
            tx = _gltf_read_f32_acc(t_ptr, tbase + t_cs * 0, t_comp, t_norm)
            ty = _gltf_read_f32_acc(t_ptr, tbase + t_cs * 1, t_comp, t_norm)
            tz = _gltf_read_f32_acc(t_ptr, tbase + t_cs * 2, t_comp, t_norm)
            tw = _gltf_read_f32_acc(t_ptr, tbase + t_cs * 3, t_comp, t_norm)
         }
         def off = ptr_add(buf, vi * _GLTF_VTX_STRIDE)
         def color_u32 = _gltf_vertex_color_u32(c_res, vi, 0xffffffff)
         store32_f32(off, mx, _GLTF_VTX_OFF_X)
         store32_f32(off, my, _GLTF_VTX_OFF_Y)
         store32_f32(off, mz, _GLTF_VTX_OFF_Z)
         store32_f32(off, 0.0, _GLTF_VTX_OFF_U)
         store32_f32(off, 0.0, _GLTF_VTX_OFF_V)
         if(uv0_valid && vi < uv0_cnt){
            def ubase = vi * uv0_stride
            store32_f32(off, _gltf_read_f32_acc(uv0_ptr, ubase + uv0_cs * 0, uv0_comp, uv0_norm), _GLTF_VTX_OFF_U)
            store32_f32(off, _gltf_read_f32_acc(uv0_ptr, ubase + uv0_cs * 1, uv0_comp, uv0_norm), _GLTF_VTX_OFF_V)
         }
         store32(off, color_u32, _GLTF_VTX_OFF_C)
         store32_f32(off, nx, _GLTF_VTX_OFF_NX)
         store32_f32(off, ny, _GLTF_VTX_OFF_NY)
         store32_f32(off, nz, _GLTF_VTX_OFF_NZ)
         store32_f32(off, tx, _GLTF_VTX_OFF_TX)
         store32_f32(off, ty, _GLTF_VTX_OFF_TY)
         store32_f32(off, tz, _GLTF_VTX_OFF_TZ)
         store32_f32(off, tw, _GLTF_VTX_OFF_TW)
         store32_f32(off, 0.0, _GLTF_VTX_OFF_U2)
         store32_f32(off, 0.0, _GLTF_VTX_OFF_V2)
         if(uv1_valid && vi < uv1_cnt){
            def sbase = vi * uv1_stride
            store32_f32(off, _gltf_read_f32_acc(uv1_ptr, sbase + uv1_cs * 0, uv1_comp, uv1_norm), _GLTF_VTX_OFF_U2)
            store32_f32(off, _gltf_read_f32_acc(uv1_ptr, sbase + uv1_cs * 1, uv1_comp, uv1_norm), _GLTF_VTX_OFF_V2)
         }
         store32(off, tex_id, _GLTF_VTX_OFF_TEX)
         vi += 1
      }
   }
   _gltf_release_vertex_pack_accessors(pos_res, uv0_res, uv1_res, c_res, n_res, t_res)
   _gltf_release_morph_targets(morph_targets)
   mut out = dict(4)
   out["ptr"] = buf
   out["count"] = count
   out
}

@jit
@inline
fn _gltf_store_index(any: out, bool: use_u32, int: v, int: out_cnt): int {
   if(use_u32){ store32(out, v, out_cnt * 4) } else { store16(out, v, out_cnt * 2) }
   out_cnt + 1
}

@jit
@inline
fn _gltf_store_index2(any: out, bool: use_u32, int: a, int: b, int: out_cnt): int {
   out_cnt = _gltf_store_index(out, use_u32, a, out_cnt)
   _gltf_store_index(out, use_u32, b, out_cnt)
}

@jit
@inline
fn _gltf_store_index3(any: out, bool: use_u32, int: a, int: b, int: c, int: out_cnt): int {
   out_cnt = _gltf_store_index(out, use_u32, a, out_cnt)
   out_cnt = _gltf_store_index(out, use_u32, b, out_cnt)
   _gltf_store_index(out, use_u32, c, out_cnt)
}

@jit
@inline
fn _gltf_pack_index_value(bool: has_idx, any: idx_ptr, int: idx_stride, int: idx_comp, int: i): int {
   if(has_idx){ return _gltf_read_index_acc(idx_ptr, i * idx_stride, idx_comp) }
   i
}

@jit
fn _gltf_pack_point_indices(any: out, bool: use_u32, bool: has_idx, any: idx_ptr, int: idx_stride, int: idx_comp, int: src_count): int {
   mut out_cnt, i = 0, 0
   while(i < src_count){
      out_cnt = _gltf_store_index(out, use_u32, _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i), out_cnt)
      i += 1
   }
   out_cnt
}

@jit
fn _gltf_pack_line_indices(any: out, bool: use_u32, bool: has_idx, any: idx_ptr, int: idx_stride, int: idx_comp, int: src_count, int: step, bool: close_loop): int {
   mut out_cnt, i = 0, 0
   while(i + 1 < src_count){
      def a = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i)
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 1)
      out_cnt = _gltf_store_index2(out, use_u32, a, b, out_cnt)
      i += step
   }
   if(close_loop && src_count > 1){
      def a = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, src_count - 1)
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, 0)
      out_cnt = _gltf_store_index2(out, use_u32, a, b, out_cnt)
   }
   out_cnt
}

@jit
fn _gltf_pack_triangle_strip_indices(any: out, bool: use_u32, bool: has_idx, any: idx_ptr, int: idx_stride, int: idx_comp, int: src_count): int {
   mut out_cnt, i = 0, 0
   while(i + 2 < src_count){
      def a = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i)
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 1)
      def c = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 2)
      if((i & 1) == 0){ out_cnt = _gltf_store_index3(out, use_u32, a, b, c, out_cnt) }
      else { out_cnt = _gltf_store_index3(out, use_u32, b, a, c, out_cnt) }
      i += 1
   }
   out_cnt
}

@jit
fn _gltf_pack_triangle_fan_indices(any: out, bool: use_u32, bool: has_idx, any: idx_ptr, int: idx_stride, int: idx_comp, int: src_count): int {
   mut out_cnt, i = 0, 1
   def base = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, 0)
   while(i + 1 < src_count){
      def b = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i)
      def c = _gltf_pack_index_value(has_idx, idx_ptr, idx_stride, idx_comp, i + 1)
      out_cnt = _gltf_store_index3(out, use_u32, base, b, c, out_cnt)
      i += 1
   }
   out_cnt
}

@jit
fn _gltf_pack_triangle_indices(any: out, bool: use_u32, bool: has_idx, any: idx_ptr, int: idx_stride, int: idx_comp, int: src_count): int {
   mut out_cnt, i = 0, 0
   if(has_idx){
      while(i < src_count){
         out_cnt = _gltf_store_index(out, use_u32, _gltf_pack_index_value(true, idx_ptr, idx_stride, idx_comp, i), out_cnt)
         i += 1
      }
      return out_cnt
   }
   while(i + 2 < src_count){
      out_cnt = _gltf_store_index3(out, use_u32, i, i + 1, i + 2, out_cnt)
      i += 3
   }
   out_cnt
}

@jit
fn _gltf_pack_indices(any: g, any: data, any: meta): any {
   "Packs index buffer. Returns {ptr, count, u32}. Uses u32 if > 65535 verts.
   For non-indexed primitives(has_idx=false) generates a sequential [0..n-1] index buffer."
   def has_idx = meta.get("has_idx", false)
   def pos_cnt = meta.get("pos_cnt", 0)
   def prim_mode = int(meta.get("mode", GLTF_MODE_TRIANGLES))
   mut idx_res = 0
   mut i_cnt, i_comp, idx_ptr, idx_stride = 0, 0, 0, 0
   if(has_idx){
      idx_res = _gltf_resolve_accessor_data(g, meta.get("i_acc_idx", -1), data)
      if(is_dict(idx_res)){
         i_cnt, i_comp = idx_res.get("count", 0), idx_res.get("comp", 0)
         idx_ptr, idx_stride = idx_res.get("ptr", 0), idx_res.get("stride", 0)
      }
      if(i_cnt <= 0){
         _gltf_release_accessor_data(idx_res)
         return 0
      }
   }
   def src_count = has_idx ? i_cnt : pos_cnt
   def use_u32 = pos_cnt > 65535
   def max_out = src_count * 3
   def esize = use_u32 ? 4 : 2
   def out = malloc(max_out * esize)
   if(!out){
      if(has_idx){ _gltf_release_accessor_data(idx_res) }
      return 0
   }
   def out_cnt = case prim_mode {
      0 -> _gltf_pack_point_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
      1 -> _gltf_pack_line_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count, 2, false)
      2 -> _gltf_pack_line_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count, 1, true)
      3 -> _gltf_pack_line_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count, 1, false)
      5 -> _gltf_pack_triangle_strip_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
      6 -> _gltf_pack_triangle_fan_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
      _ -> _gltf_pack_triangle_indices(out, use_u32, has_idx, idx_ptr, idx_stride, i_comp, src_count)
   }
   if(has_idx){ _gltf_release_accessor_data(idx_res) }
   return {"ptr": out, "count": out_cnt, "u32": use_u32, "mode": prim_mode}
}

fn _gltf_index_value(any: iptr, bool: idx_u32, int: idx): int{ idx_u32 ? load32(iptr, idx * 4) : load16(iptr, idx * 2) }

fn _gltf_expand_primitive_vertices(?ptr: vptr, int: vcnt, ?ptr: iptr, int: icnt, bool: idx_u32=false): any {
   if(!vptr || vcnt <= 0 || !iptr || icnt <= 0){ return 0 }
   def out = malloc(icnt * _GLTF_VTX_STRIDE)
   if(!out){ return 0 }
   mut i = 0
   while(i < icnt){
      def vi = _gltf_index_value(iptr, idx_u32, i)
      if(vi < 0 || vi >= vcnt){
         free(out)
         return 0
      }
      memcpy(ptr_add(out, i * _GLTF_VTX_STRIDE), ptr_add(vptr, vi * _GLTF_VTX_STRIDE), _GLTF_VTX_STRIDE)
      i += 1
   }
   {"ptr": out, "count": icnt}
}

fn _gltf_copy_part_opts(any: opts): dict {
   mut out = dict(8)
   if(!is_dict(opts)){ return out }
   def flags = ["index_type_u32", "is_points", "is_lines", "unlit", "no_cull", "double_sided", "flip_winding"]
   mut i = 0
   while(i < flags.len){
      def key = to_str(flags.get(i))
      if(opts.get(key, false)){ out[key] = true }
      i += 1
   }
   def storage = opts.get("storage", "")
   if(is_str(storage) && storage.len > 0){ out["storage"] = storage }
   out
}

fn _gltf_pack_cacheable_meta(any: meta): bool {
   if(!is_dict(meta)){ return false }
   def targets = meta.get("targets", 0)
   !is_list(targets) || targets.len == 0
}

fn _gltf_pack_vertex_cache_key(any: meta, int: packed_color, int: tex_id, int: uv_set, any: uv_xform): str {
   to_str(int(meta.get("pos_acc_idx", -1))) + "|" +
   to_str(int(meta.get("uv_acc_idx", -1))) + "|" +
   to_str(int(meta.get("uv1_acc_idx", -1))) + "|" +
   to_str(int(meta.get("c_acc_idx", -1))) + "|" +
   to_str(int(meta.get("n_acc_idx", -1))) + "|" +
   to_str(int(meta.get("t_acc_idx", -1))) + "|" +
   to_str(int(packed_color)) + "|" +
   to_str(int(tex_id)) + "|" +
   to_str(int(uv_set)) + "|" +
   to_str(to_int(uv_xform))
}

fn _gltf_pack_index_cache_key(any: meta): str {
   to_str(int(meta.get("i_acc_idx", -1))) + "|" +
   to_str(int(meta.get("pos_cnt", 0))) + "|" +
   to_str(int(meta.get("mode", GLTF_MODE_TRIANGLES)))
}

fn _gltf_pack_primitive_buffers(any: g, any: data, any: meta, int: packed_color, int: tex_id, int: uv_set, any: uv_xform, bool: pack_cache_enabled, any: vertex_pack_cache, any: index_pack_cache, any: pack_stats): list {
   mut verts = 0
   mut inds = 0
   mut vcache = vertex_pack_cache
   mut icache = index_pack_cache
   mut stats = pack_stats
   def use_pack_cache = pack_cache_enabled && _gltf_pack_cacheable_meta(meta)
   if(use_pack_cache){
      def vk = _gltf_pack_vertex_cache_key(meta, packed_color, tex_id, uv_set, uv_xform)
      verts = vcache.get(vk, 0)
      if(!is_dict(verts)){
         stats["vertex_misses"] = int(stats.get("vertex_misses", 0)) + 1
         verts = _gltf_pack_unique_vertices(g, data, meta, packed_color, tex_id, uv_set, uv_xform)
         if(is_dict(verts)){ vcache[vk] = verts }
      } else {
         stats["vertex_hits"] = int(stats.get("vertex_hits", 0)) + 1
      }
      def ik = _gltf_pack_index_cache_key(meta)
      inds = icache.get(ik, 0)
      if(!is_dict(inds)){
         stats["index_misses"] = int(stats.get("index_misses", 0)) + 1
         inds = _gltf_pack_indices(g, data, meta)
         if(is_dict(inds)){ icache[ik] = inds }
      } else {
         stats["index_hits"] = int(stats.get("index_hits", 0)) + 1
      }
   } else {
      verts = _gltf_pack_unique_vertices(g, data, meta, packed_color, tex_id, uv_set, uv_xform)
      inds = _gltf_pack_indices(g, data, meta)
   }
   [verts, inds, vcache, icache, stats]
}

fn _gltf_build_lod_data(any: vptr, int: vcnt, any: iptr, int: icnt, bool: idx_u32, int: prim_mode_lod): any {
   if(_gltf_is_debug()){ print("[gltf:lod] check vptr=" + to_str(vptr != 0) + " iptr=" + to_str(iptr != 0) + " icnt=" + to_str(icnt) + " mode=" + to_str(prim_mode_lod)) }
   mut lod_data = 0
   if(_gltf_is_debug()){ print("[gltf:lod] enabled=" + to_str(_gltf_autolod_enabled()) + " entering maybe-build") }
   if(_gltf_autolod_enabled() && vptr && iptr && icnt >= 24 && prim_mode_lod == GLTF_MODE_TRIANGLES){
      mut lod_indices = []
      if(idx_u32){
         mut ii = 0
         while(ii < icnt){
            lod_indices = lod_indices.append(u32le(iptr, ii * 4))
            ii += 1
         }
      } else {
         mut ii = 0
         while(ii < icnt){
            lod_indices = lod_indices.append(u16le(iptr, ii * 2))
            ii += 1
         }
      }
      def max_lod = icnt > 10000 ? 6 : (icnt > 2000 ? 4 : 2)
      lod_data = meshopt_process_mesh(lod_indices, vcnt, vptr, 64, max_lod)
      if(_gltf_is_debug()){
         mut level_count = -1
         if(is_dict(lod_data)){
            def levels = lod_data.get("levels", [])
            if(is_list(levels)){ level_count = levels.len }
         }
         print("[gltf:lod] built levels=" + to_str(level_count))
      }
   }
   if(_gltf_is_debug()){ print("[gltf:lod] after maybe-build") }
   lod_data
}

fn _gltf_indexed_material_fast_record(any: texrec_fast, any: info_fast): dict {
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

fn _gltf_indexed_base_color_state(any: info): dict {
   def f = is_dict(info) ? info.get("base_color_factor", [1.0, 1.0, 1.0, 1.0]) : [1.0, 1.0, 1.0, 1.0]
   def r = int((clamp01(float(f.get(0, 1.0))) * 255.0))
   def g_val = int((clamp01(float(f.get(1, 1.0))) * 255.0))
   def b = int((clamp01(float(f.get(2, 1.0))) * 255.0))
   def alpha_mode = is_dict(info) ? to_str(info.get("alpha_mode", "OPAQUE")) : "OPAQUE"
   def is_blend = alpha_mode == "BLEND"
   def alpha_mode_code = _gltf_alpha_mode_code(alpha_mode)
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

fn _gltf_indexed_mr_state(any: info, any: texrec_uv): dict {
   mut metallic_u8 = band(int((clamp01(is_dict(info) ? float(info.get("metallic_factor", 1.0)) : 1.0) * 255.0)), 255)
   def rough_u8 = band(int((clamp01(is_dict(info) ? float(info.get("roughness_factor", 1.0)) : 1.0) * 255.0)), 255)
   if(is_dict(info) && info.get("specular_glossiness", false)){ metallic_u8 = 0 }
   def mr_tex_id = is_dict(texrec_uv) ? int(texrec_uv.get("metallic_roughness", -1)) : -1
   def mr_uv_set = is_dict(info) ? int(info.get("metallic_roughness_texcoord", 0)) : 0
   mut mr_tid = 0
   if(mr_tex_id >= 0){ mr_tid = band(mr_tex_id + 1, 0x7fff) }
   mut mr_word = mr_tid
   if(mr_uv_set != 0){ mr_word = bor(mr_word, 0x8000) }
   {
      "material_word": _pack_material_word(metallic_u8, rough_u8, mr_word),
      "mr_uv_set": mr_uv_set,
      "metallic_u8": metallic_u8,
      "rough_u8": rough_u8
   }
}

fn _gltf_indexed_uv_xforms(any: info): dict {
   {
      "base": _gltf_pack_uv_xform_words(info, "base_color"),
      "normal": _gltf_pack_uv_xform_words(info, "normal"),
      "mr": _gltf_pack_uv_xform_words(info, "metallic_roughness"),
      "occlusion": _gltf_pack_uv_xform_words(info, "occlusion"),
      "emissive": _gltf_pack_uv_xform_words(info, "emissive")
   }
}

fn _gltf_indexed_material_info_record(any: info, any: texrec_uv): dict {
   def base = _gltf_indexed_base_color_state(info)
   def mr = _gltf_indexed_mr_state(info, texrec_uv)
   def emit_tex_id = is_dict(texrec_uv) ? int(texrec_uv.get("emissive", -1)) : -1
   def uv_props = _gltf_pick_primary_uv_props(info, texrec_uv)
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
      "ext2_tex_word": int(info.get("ext2_tex_word", 0x80000000)),
      "debug_emit_tex_id": emit_tex_id, "debug_alpha_mode": base.get("alpha_mode"),
      "debug_metallic_u8": mr.get("metallic_u8"), "debug_rough_u8": mr.get("rough_u8")
   }
}

fn _gltf_indexed_material_records(list: mat_infos, any: material_tex_ids, int: material_tex_ids_n, int: mat_count_pre, bool: use_fast_core_records, bool: is_debug): list {
   mut records = list(mat_count_pre)
   mut mj = 0
   while(mj < mat_count_pre){
      if(use_fast_core_records){
         def texrec_fast = material_tex_ids.get(mj, 0)
         if(is_dict(texrec_fast) && bool(texrec_fast.get("fast_core_pbr", false))){
            def info_fast = (mj >= 0 && mj < mat_infos.len) ? mat_infos.get(mj) : 0
            records = records.append(_gltf_indexed_material_fast_record(texrec_fast, info_fast))
            mj += 1
            continue
         }
      }
      def info = mat_infos.get(mj)
      def texrec_uv = (mj >= 0 && mj < material_tex_ids_n) ? material_tex_ids.get(mj, 0) : 0
      if(is_debug && mj == 0 && is_dict(texrec_uv)){
         print("[gltf] mat_texrec0 keys base=" + to_str(texrec_uv.get("base", -999)) +
            " base_color=" + to_str(texrec_uv.get("base_color", -999)) +
            " mr=" + to_str(texrec_uv.get("metallic_roughness", -999)) +
         " slot=" + to_str(texrec_uv.get("slot", "")))
      }
      def rec = _gltf_indexed_material_info_record(info, texrec_uv)
      records = records.append(rec)
      if(is_debug){
         print(
            "[gltf] mat[" + to_str(mj) + "]" +
            " base_rgba=0x" + to_hex(int(rec.get("packed_color", 0))) +
            " material=0x" + to_hex(int(rec.get("material_u32", 0))) +
            " metallic=" + to_str(int(rec.get("debug_metallic_u8", 0))) +
            " rough=" + to_str(int(rec.get("debug_rough_u8", 0))) +
            " emissive_tex=" + to_str(int(rec.get("debug_emit_tex_id", -1))) +
            " alphaMode=" + to_str(rec.get("debug_alpha_mode", "OPAQUE")) +
            " ds=" + to_str(bool(rec.get("double_sided", false)))
         )
      }
      _gltf_warn_unhandled_material_maps(info, mj)
      mj += 1
   }
   records
}

fn _gltf_indexed_copy_i32_fields(dict: st, dict: src, list: rows): dict {
   mut i = 0
   while(i < rows.len){
      def row = rows.get(i)
      def dst = to_str(row.get(0))
      def src_key = to_str(row.get(1, dst))
      st[dst] = int(src.get(src_key, st.get(dst, 0)))
      i += 1
   }
   st
}

fn _gltf_keep_indexed_bsdf_fields(dict: st, dict: src): dict {
   mut i = 0
   while(i < 6){
      def key = "bsdf" + to_str(i) + "_u32"
      st[key] = _gltf_keep_bsdf_record_value(int(src.get(key, 0)), int(st.get(key, 0)))
      i += 1
   }
   st
}

fn _gltf_indexed_part_material_defaults(int: packed_color): dict {
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

fn _gltf_indexed_part_material_apply_record(dict: st, any: mat_state): dict {
   if(!is_dict(mat_state)){ return st }
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

fn _gltf_indexed_part_material_apply_texrec(dict: st, dict: texrec, bool: is_debug, int: mesh_idx, int: pi, int: mat_idx): dict {
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
   if(band(tex_material_u32, 0x0000ffff) == 0 && band(material_u32, 0x0000ffff) != 0){ tex_material_u32 = bor(band(tex_material_u32, 0xffff0000), band(material_u32, 0x0000ffff)) }
   def tex_mr_raw = int(texrec.get("metallic_roughness", texrec.get("mr", band(bshr(tex_material_u32, 16), 0x7fff))))
   mut tex_mr_id = 0
   if(tex_mr_raw >= 0){ tex_mr_id = band(tex_mr_raw + 1, 0x7fff) }
   if(tex_mr_raw >= 0 || int(st.get("met_rough_uv_set", 0)) != 0){
      def tex_metallic_u8 = band(tex_material_u32, 255)
      def tex_rough_u8 = band(bshr(tex_material_u32, 8), 255)
      mut tex_mr_word = tex_mr_id
      if(int(st.get("met_rough_uv_set", 0)) != 0){ tex_mr_word = bor(tex_mr_word, 0x8000) }
      tex_material_u32 = _pack_material_word(tex_metallic_u8, tex_rough_u8, tex_mr_word)
   }
   if(is_debug && pi == 0){
      print("[gltf] prim matdbg mat=" + to_str(mat_idx) +
         " tex_mr_raw=" + to_str(tex_mr_raw) +
         " tex_mr_id=" + to_str(tex_mr_id) +
      " tex_mat_u32=0x" + to_hex(tex_material_u32))
   }
   st["material_u32"] = tex_material_u32
   if(is_debug){
      print(
         "[gltf] prim mesh=" + to_str(mesh_idx) +
         " prim=" + to_str(pi) +
         " mat=" + to_str(mat_idx) +
         " texrec=dict" +
         " base=" + to_str(texrec.get("base_color", texrec.get("base", 0))) +
         " mr=" + to_str(texrec.get("metallic_roughness", 0)) +
         " normal=" + to_str(texrec.get("normal", 0)) +
         " occlusion=" + to_str(texrec.get("occlusion", 0)) +
         " emissive=" + to_str(texrec.get("emissive", 0)) +
         " material_u32=0x" + to_hex(tex_material_u32)
      )
   }
   st
}

fn _gltf_indexed_part_force_uv_xform(dict: st, str: k0, str: k1): dict {
   if(band(int(st.get(k1, 0)), 0x40000000) != 0){
      def xf = _gltf_uv_xf_force_uv0(int(st.get(k0, 0)), int(st.get(k1, 0)))
      st[k0] = int(xf.get(0, st.get(k0, 0)))
      st[k1] = int(xf.get(1, st.get(k1, 0)))
   }
   st
}

fn _gltf_indexed_part_material_force_uv0(dict: st): dict {
   st["normal_uv_set"] = 0
   st["emissive_uv_set"] = 0
   st["met_rough_uv_set"] = 0
   st["occlusion_uv_set"] = 0
   st = _gltf_indexed_part_force_uv_xform(st, "base_uv_xf0", "base_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "normal_uv_xf0", "normal_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "mr_uv_xf0", "mr_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "occlusion_uv_xf0", "occlusion_uv_xf1")
   st = _gltf_indexed_part_force_uv_xform(st, "emissive_uv_xf0", "emissive_uv_xf1")
   if(band(int(st.get("material_u32", 0)), 0x80000000) != 0){ st["material_u32"] = band(int(st.get("material_u32", 0)), 0x7fffffff) }
   st["uv_set"] = 0
   st
}

fn _gltf_indexed_prim_material_state(
   any: material_tex_ids, int: material_tex_ids_n, list: mat_records, int: packed_color,
   dict: meta, int: mesh_idx, int: pi, bool: is_debug
): dict {
   def mat_idx = int(meta.get("mat_idx", 0))
   def mat_record = (mat_idx >= 0 && mat_idx < mat_records.len) ? mat_records.get(mat_idx, 0) : 0
   mut material_state = _gltf_indexed_part_material_apply_record(_gltf_indexed_part_material_defaults(packed_color), mat_record)
   if(mat_idx >= 0 && mat_idx < material_tex_ids_n){
      def texrec = material_tex_ids.get(mat_idx, 0)
      if(is_dict(texrec)){
         material_state = _gltf_indexed_part_material_apply_texrec(material_state, texrec, is_debug, mesh_idx, pi, mat_idx)
      } else {
         material_state["tex_id"] = int(texrec)
         if(is_debug){
            print(
               "[gltf] prim mesh=" + to_str(mesh_idx) +
               " prim=" + to_str(pi) +
               " mat=" + to_str(mat_idx) +
               " texrec=int tex_id=" + to_str(material_state.get("tex_id", -1))
            )
         }
      }
   } elif(is_debug){
      print(
         "[gltf] prim mesh=" + to_str(mesh_idx) +
         " prim=" + to_str(pi) +
         " mat=" + to_str(mat_idx) +
         " no material_tex_ids entry" +
         " tex_id=" + to_str(material_state.get("tex_id", -1)) +
         " vcol_cnt=" + to_str(int(meta.get("c_cnt", 0))) +
         " material_u32=0x" + to_hex(int(material_state.get("material_u32", 0)))
      )
   }
   def has_uv1 = int(meta.get("uv1_cnt", 0)) > 0
   if(!has_uv1){ material_state = _gltf_indexed_part_material_force_uv0(material_state) }
   if(!has_uv1 && int(material_state.get("uv_set", 0)) != 0){ material_state["uv_set"] = 0 }
   if(int(meta.get("c_acc_idx", -1)) >= 0 && int(meta.get("c_cnt", 0)) > 0){
      material_state["prim_vc_mode"] = bor(int(material_state.get("prim_vc_mode", 0)), 4)
   }
   material_state
}

fn _gltf_indexed_mesh_opts(dict: material_state, bool: idx_u32, dict: meta, int: prim_mode, str: use_static, bool: is_debug): dict {
   mut mesh_opts = dict(4)
   if(idx_u32){ mesh_opts["index_type_u32"] = true }
   def prim_has_normals = int(meta.get("n_cnt", 0)) > 0
   mesh_opts["has_normals"] = prim_has_normals
   mesh_opts = _gltf_apply_prim_mode_opts(mesh_opts, prim_mode, prim_has_normals)
   if(use_static != "static"){ mesh_opts["storage"] = use_static }
   if(material_state.get("is_unlit", false)){ mesh_opts["unlit"] = true }
   if(material_state.get("is_nocull", false)){ mesh_opts["no_cull"] = true }
   if(material_state.get("is_double_sided", false)){ mesh_opts["double_sided"] = true }
   if(is_debug){ print("[gltf:part] mesh_opts ready prim_mode=" + to_str(prim_mode) + " idx_u32=" + to_str(idx_u32)) }
   mesh_opts
}

fn _gltf_indexed_append_instance_parts(
   list: parts, list: scene_bounds, dict: gltf_data, dict: g, any: data, any: nodes,
   list: instance_mats, dict: node_vis_map, dict: meta, any: vptr, int: vcnt,
   any: iptr, int: icnt, dict: mesh_opts, dict: material_state, int: mat_idx,
   int: mesh_idx, int: pi, int: uv_set, int: packed_color, int: prim_mode, any: lbs, bool: is_debug
): list {
   def tex_id = int(material_state.get("tex_id", -1))
   def material_u32 = int(material_state.get("material_u32", 0x0000ff00))
   def instance_mats_n = instance_mats.len
   mut mi = 0
   while(mi < instance_mats_n){
      def inst_pair = instance_mats.get(mi)
      def is_mat_pair = is_list(inst_pair) && inst_pair.len == 2 && is_list(inst_pair.get(0))
      def base_model = is_mat_pair ? inst_pair.get(0) : inst_pair
      def part_node_idx = is_mat_pair ? int(inst_pair.get(1, -1)) : -1
      if(is_debug){ print("[gltf:part] mi=" + to_str(mi) + " part_node=" + to_str(part_node_idx) + " mat_pair=" + to_str(is_mat_pair)) }
      mut sub_mats = 0
      mut has_gpu_instancing = false
      if(part_node_idx >= 0){
         def node_obj = nodes.get(part_node_idx)
         sub_mats = _gltf_resolve_instancing_mats(g, node_obj, data, base_model)
         has_gpu_instancing = is_list(sub_mats) && sub_mats.len > 0
      }
      if(is_debug){ print("[gltf:part] sub_mats list=" + to_str(is_list(sub_mats)) + " count=" + to_str(is_list(sub_mats) ? sub_mats.len : -1)) }
      if(!is_list(sub_mats) || sub_mats.len == 0){ sub_mats = [base_model] }
      def sub_mats_n = sub_mats.len
      mut smi = 0
      while(smi < sub_mats_n){
         def model = gltf_math.safe_model_mat4(sub_mats.get(smi))
         mut part_visible = true
         if(part_node_idx >= 0){ part_visible = node_vis_map.get(part_node_idx, true) ? true : false }
         if(is_debug){ print("[gltf:part] smi=" + to_str(smi) + " model_list=" + to_str(is_list(model)) + " model_len=" + to_str(is_list(model) ? model.len : -1)) }
         mut part_opts = _gltf_copy_part_opts(mesh_opts)
         if(is_debug){ print("[gltf:part] before det") }
         def flip_winding = _gltf_model_has_negative_det(model)
         if(is_debug){ print("[gltf:part] after det flip=" + to_str(flip_winding)) }
         if(flip_winding){ part_opts["flip_winding"] = true }
         if(is_debug){ print("[gltf:part] before bounds") }
         def world_bounds = _gltf_part_world_bounds(lbs, model)
         if(is_debug){ print("[gltf:part] after bounds") }
         if(part_visible){ scene_bounds = _gltf_scene_bounds_accum_part(scene_bounds, world_bounds) }
         if(is_debug){
            print(
               "[gltf] part mesh=" + to_str(mesh_idx) +
               " prim=" + to_str(pi) +
               " mat=" + to_str(mat_idx) +
               " node=" + to_str(part_node_idx) +
               " tex=" + to_str(tex_id) +
               " vcnt=" + to_str(vcnt) +
               " icnt=" + to_str(icnt) +
               " flip=" + to_str(flip_winding) +
               " ds=" + to_str(material_state.get("is_double_sided", false)) +
               " unlit=" + to_str(material_state.get("is_unlit", false)) +
               " nocull=" + to_str(material_state.get("is_nocull", false)) +
               " material=0x" + to_hex(material_u32)
            )
         }
         if(is_debug){ print("[gltf:part] pre-skin node=" + to_str(part_node_idx) + " smi=" + to_str(smi) + " flip=" + to_str(flip_winding)) }
         def skin_state = _gltf_resolve_part_skin(gltf_data, g, data, nodes, part_node_idx, meta, vptr, vcnt)
         if(is_debug){ print("[gltf:part] post-skin skin_idx=" + to_str(int(skin_state.get("skin_idx", -1))) + " bind=" + to_str(skin_state.get("skin_bind_vptr", 0) != 0) + " gpu_inst=" + to_str(has_gpu_instancing)) }
         if(is_debug){ print("[gltf:part] pre-state visible=" + to_str(part_visible) + " bounds_ready=" + to_str(is_list(lbs))) }
         if(is_debug){ print("[gltf:part] before append helper") }
         parts = parts.append(_gltf_make_indexed_part_from_state(vptr,
               vcnt,
               iptr,
               icnt,
               part_opts,
               material_state,
               mat_idx,
               uv_set,
               model,
               part_node_idx,
               skin_state,
               part_visible,
               has_gpu_instancing,
               prim_mode,
               0,
               world_bounds,
         packed_color))
         if(is_debug){ print("[gltf:part] appended") }
         smi += 1
      }
      mi += 1
   }
   [parts, scene_bounds]
}

fn _gltf_indexed_default_color(any: gltf_data): int {
   mut packed_color = int(gltf_data.get("color", 0))
   if(packed_color != 0){ return packed_color }
   def r = int((clamp01(gltf_data.get("r", 1.0)) * 255.0))
   def gr = int((clamp01(gltf_data.get("g", 1.0)) * 255.0))
   def b = int((clamp01(gltf_data.get("b", 1.0)) * 255.0))
   def a = int((clamp01(gltf_data.get("a", 1.0)) * 255.0))
   (a << 24) | (b << 16) | (gr << 8) | r
}

fn _gltf_indexed_material_record_setup(any: gltf_data, any: material_tex_ids, int: mesh_limit, bool: is_debug): list {
   def material_tex_ids_n = is_list(material_tex_ids) ? material_tex_ids.len : 0
   def fast_core_input_records = material_tex_ids_n > 0 && is_dict(material_tex_ids.get(0, 0)) && bool(material_tex_ids.get(0, 0).get("fast_core_pbr", false))
   def limited_mat_infos = gltf_data.get("__material_infos_limited", 0)
   def mat_infos = is_list(limited_mat_infos) ? limited_mat_infos : gltf_material_infos_limited(gltf_data, mesh_limit)
   def mat_count_pre = fast_core_input_records ? material_tex_ids_n : mat_infos.len
   def use_fast_core_records = fast_core_input_records && material_tex_ids_n == mat_count_pre
   [material_tex_ids_n, _gltf_indexed_material_records(mat_infos, material_tex_ids, material_tex_ids_n, mat_count_pre, use_fast_core_records, is_debug)]
}

fn _gltf_indexed_scene_world_mats(dict: g, any: gltf_data, int: mesh_limit, int: nodes_n): dict {
   def node_cap = (mesh_limit > 0 && mesh_limit < nodes_n) ? (mesh_limit + 64) : nodes_n
   mut node_world_mats = dict(max(16, node_cap * 2))
   def scenes = g.get("scenes")
   def scene_idx = _gltf_active_scene_idx(g, gltf_data)
   if(is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len){
      def scene = scenes.get(scene_idx)
      def roots = scene.get("nodes")
      if(is_list(roots)){
         def roots_n = roots.len
         def id = gltf_math.mat4_identity()
         mut ri = 0
         while(ri < roots_n){
            def root_idx = int(roots[ri])
            if(_gltf_root_relevant_for_mesh_limit(g, root_idx, mesh_limit)){
               node_world_mats = _gltf_build_node_world_mats(g, root_idx, id, node_world_mats)
            }
            ri += 1
         }
      }
   }
   node_world_mats
}

fn _gltf_indexed_finish_result(list: parts, list: scene_bounds, int: total_v_packed, int: total_i_packed, dict: pack_stats, bool: is_debug): dict {
   def final_bounds = _gltf_scene_bounds_result(scene_bounds)
   if(is_debug){
      print(
         "[gltf] Finished. Parts: " + to_str(parts.len) +
         " Verts: " + to_str(total_v_packed) +
         " Inds: " + to_str(total_i_packed)
      )
   }
   if(common.env_truthy("NY_GLTF_PACK_CACHE_TRACE")){
      print("[gltf:pack_cache] vertex_hits=" + to_str(int(pack_stats.get("vertex_hits", 0))) +
         " vertex_misses=" + to_str(int(pack_stats.get("vertex_misses", 0))) +
         " index_hits=" + to_str(int(pack_stats.get("index_hits", 0))) +
      " index_misses=" + to_str(int(pack_stats.get("index_misses", 0))))
   }
   {"parts": parts,
      "min": final_bounds.get(0),
      "max": final_bounds.get(1),
   "loader_bounds_ready": true}
}

fn gltf_to_mesh_group_indexed(any: gltf_data, any: color=0, any: material_tex_ids=0): any {
   "Loads glTF as indexed raw vertex/index data.
   Returns {parts, min, max} where each part has {vptr, vcnt, iptr, icnt, opts, mat_idx, tex_id, model}.
   Caller must upload to GPU via mesh_create_indexed for each part x instance."
   def is_debug = _gltf_is_debug()
   if(!is_dict(gltf_data)){ return 0 }
   def g = gltf_data.get("gltf", 0)
   def data = _gltf_primary_data_ptr(gltf_data)
   if(!is_dict(g) || !data){ return 0 }
   def meshes = g.get("meshes")
   def nodes = g.get("nodes")
   if(!is_list(meshes)){ return 0 }
   mut meshes_n = meshes.len
   def mesh_limit = int(gltf_data.get("__mesh_limit", 0))
   if(mesh_limit > 0 && mesh_limit < meshes_n){
      if(is_debug){ print("[gltf:proof] mesh loop cap=" + to_str(mesh_limit) + " of " + to_str(meshes_n)) }
      meshes_n = mesh_limit
   }
   def nodes_n = is_list(nodes) ? nodes.len : 0
   def accs = g.get("accessors")
   def bv_list = g.get("bufferViews")
   if(!is_list(accs) || !is_list(bv_list)){
      if(is_debug){ print("[gltf] Missing accessors or bufferViews") }
      return 0
   }
   def packed_color = _gltf_indexed_default_color(gltf_data)
   def mat_setup = _gltf_indexed_material_record_setup(gltf_data, material_tex_ids, mesh_limit, is_debug)
   def material_tex_ids_n = int(mat_setup.get(0, 0))
   def mat_records = mat_setup.get(1, [])
   mut parts = []
   mut scene_bounds = _gltf_scene_bounds_new()
   mut use_static = "static"
   if(is_dict(gltf_data)){ use_static = gltf_data.get("storage", "static") }
   def node_world_mats = _gltf_indexed_scene_world_mats(g, gltf_data, mesh_limit, nodes_n)
   def mesh_instance_map = _gltf_compute_mesh_instance_mats_fast(g, node_world_mats, mesh_limit)
   def scene_scoped_instances = is_dict(node_world_mats) && node_world_mats.len > 0
   def node_vis_map = gltf_resolve_node_visibility(gltf_data, 0)
   def pack_cache_enabled = !common.env_truthy("NY_GLTF_PACK_CACHE_OFF")
   mut vertex_pack_cache = dict(64)
   mut index_pack_cache = dict(64)
   mut pack_stats = {"vertex_hits": 0, "vertex_misses": 0, "index_hits": 0, "index_misses": 0}
   mut mesh_idx = 0
   mut total_v_packed = 0
   mut total_i_packed = 0
   while(mesh_idx < meshes_n){
      def mesh = meshes.get(mesh_idx)
      def primitives = mesh.get("primitives", [])
      def primitives_n = is_list(primitives) ? primitives.len : 0
      if(is_debug && primitives_n > 1){ print("[gltf] mesh " + to_str(mesh_idx) + ": " + to_str(primitives_n) + " primitives") }
      mut instance_mats = mesh_instance_map.get(mesh_idx, 0)
      if(!is_list(instance_mats) || instance_mats.len == 0){
         if(scene_scoped_instances){
            mesh_idx += 1
            continue
         }
         instance_mats = [[gltf_math.mat4_identity(), -1]]
      }
      mut pi = 0
      while(pi < primitives_n){
         def prim = primitives.get(pi)
         mut meta = _gltf_prim_meta(prim, accs)
         if(is_dict(meta)){
            def targets = prim.get("targets", 0)
            def targets_n = is_list(targets) ? targets.len : 0
            if(targets_n > 0){
               meta["targets"] = targets
               meta["morph_weights"] = _gltf_mesh_morph_weights(g, mesh_idx, targets_n)
            }
         }
         if(is_dict(meta)){
            def mat_idx = meta.get("mat_idx", 0)
            def material_state = _gltf_indexed_prim_material_state(material_tex_ids,
               material_tex_ids_n,
               mat_records,
               packed_color,
               meta,
               mesh_idx,
               pi,
            is_debug)
            def uv_set = int(material_state.get("uv_set", 0))
            def uv_xform = material_state.get("uv_xform", 0)
            def tex_id = int(material_state.get("tex_id", -1))
            def prim_packed_color = int(material_state.get("prim_packed_color", packed_color))
            def material_u32 = int(material_state.get("material_u32", 0x0000ff00))
            if(is_debug){
               print(
                  "[gltf] pack prim mesh=" + to_str(mesh_idx) +
                  " prim=" + to_str(pi) +
                  " mat=" + to_str(mat_idx) +
                  " tex=" + to_str(tex_id) +
                  " uv_set=" + to_str(uv_set) +
                  " base_rgba=0x" + to_hex(prim_packed_color) +
                  " material=0x" + to_hex(material_u32)
               )
            }
            def packed_buffers = _gltf_pack_primitive_buffers(g, data, meta, prim_packed_color, tex_id, uv_set, uv_xform, pack_cache_enabled, vertex_pack_cache, index_pack_cache, pack_stats)
            def verts = packed_buffers.get(0, 0)
            def inds = packed_buffers.get(1, 0)
            vertex_pack_cache = packed_buffers.get(2, vertex_pack_cache)
            index_pack_cache = packed_buffers.get(3, index_pack_cache)
            pack_stats = packed_buffers.get(4, pack_stats)
            if(is_dict(verts) && is_dict(inds)){
               mut vptr = verts.get("ptr", 0)
               mut vcnt = verts.get("count", 0)
               mut iptr = inds.get("ptr", 0)
               mut icnt = inds.get("count", 0)
               mut idx_u32 = inds.get("u32", false)
               def prim_mode = int(meta.get("mode", GLTF_MODE_TRIANGLES))
               if(_gltf_prim_mode_expands_to_vertices(prim_mode)){
                  def expanded_prim = _gltf_expand_primitive_vertices(vptr, vcnt, iptr, icnt, idx_u32)
                  if(is_dict(expanded_prim)){
                     vptr = expanded_prim.get("ptr", 0)
                     vcnt = int(expanded_prim.get("count", 0))
                     iptr = 0
                     icnt = 0
                     idx_u32 = false
                  }
               }
               total_v_packed += vcnt
               total_i_packed += icnt
               def prim_mode_lod = int(meta.get("mode", GLTF_MODE_TRIANGLES))
               def lod_data = _gltf_build_lod_data(vptr, vcnt, iptr, icnt, idx_u32, prim_mode_lod)
               def mesh_opts = _gltf_indexed_mesh_opts(material_state, idx_u32, meta, prim_mode, use_static, is_debug)
               def lbs = meta.get("local_bounds", 0)
               def emitted = _gltf_indexed_append_instance_parts(parts, scene_bounds, gltf_data, g, data, nodes, instance_mats,
                  node_vis_map, meta, vptr, vcnt, iptr, icnt, mesh_opts, material_state, mat_idx,
               mesh_idx, pi, uv_set, packed_color, prim_mode, lbs, is_debug)
               parts = emitted.get(0, parts)
               scene_bounds = emitted.get(1, scene_bounds)
            } elif(is_debug){
               print(
                  "[gltf] pack failed mesh=" + to_str(mesh_idx) +
                  " prim=" + to_str(pi) +
                  " verts_ok=" + to_str(is_dict(verts)) +
                  " inds_ok=" + to_str(is_dict(inds))
               )
            }
         }
         pi += 1
      }
      mesh_idx += 1
   }
   _gltf_indexed_finish_result(parts, scene_bounds, total_v_packed, total_i_packed, pack_stats, is_debug)
}

fn gltf_warm_runtime(): bool {
   "Warms the indexed glTF packing helpers so first real scene load does not pay their JIT/setup cost."
   true
}

fn _gltf_resolve_accessor_data(any: g, int: acc_idx, any: data): any {
   _gltf_ensure_caches()
   def accs = g.get("accessors", [])
   if(acc_idx < 0 || acc_idx >= accs.len){ return 0 }
   def acc = accs.get(acc_idx)
   def sparse = acc.get("sparse", 0)
   def cacheable = !is_dict(sparse)
   def cache_key = cacheable ? (_gltf_cache_key_from_g(g) + ":data:" + to_str(to_int(data)) + ":acc:" + to_str(int(acc_idx))) : ""
   if(cacheable && _gltf_acc_res_cache.contains(cache_key)){ return _gltf_acc_res_cache.get(cache_key, 0) }
   def count = acc.get("count", 0)
   def comp = acc.get("componentType", 0)
   def type_str = acc.get("type", "")
   def type_cnt = _gltf_type_count(type_str)
   def comp_size = _gltf_comp_size(comp)
   def elem_size = _gltf_elem_size(comp_size, type_str)
   def normalized = acc.get("normalized", false) ? true : false
   def bv_list = g.get("bufferViews", [])
   def buffers_data = g.get("_ny_buffer_data", 0)
   def bv_idx = acc.get("bufferView", -1)
   mut ptr = 0
   mut stride = 0
   mut owned = false
   if(bv_idx >= 0 && bv_idx < bv_list.len){
      def bv = bv_list.get(bv_idx)
      def buf_idx = int(bv.get("buffer", 0))
      mut base_ptr = data
      if(is_list(buffers_data) && buf_idx >= 0 && buf_idx < buffers_data.len){
         def b = buffers_data.get(buf_idx, 0)
         def bp = _gltf_blob_ptr(b)
         if(bp){ base_ptr = bp }
      }
      if(base_ptr){
         ptr = ptr_add(ptr_add(base_ptr, int(bv.get("byteOffset", 0))), int(acc.get("byteOffset", 0)))
         stride = bv.get("byteStride", 0)
         if(stride == 0){ stride = elem_size }
      }
   }
   if(is_dict(sparse)){
      def m_ptr = malloc(count * elem_size)
      if(!m_ptr){ return 0 }
      if(ptr){
         mut i = 0
         while(i < count){
            memcpy(ptr_add(m_ptr, i * elem_size), ptr_add(ptr, i * stride), elem_size)
            i += 1
         }
      } else {
         memset(m_ptr, 0, count * elem_size)
      }
      def s_count = sparse.get("count", 0)
      def s_indices = sparse.get("indices")
      def s_values = sparse.get("values")
      def sbv_idx = s_indices.get("bufferView")
      if(sbv_idx < 0 || sbv_idx >= bv_list.len){ free(m_ptr) return 0 }
      def sbv = bv_list.get(sbv_idx)
      def idx_off = sbv.get("byteOffset", 0) + s_indices.get("byteOffset", 0)
      def idx_comp = s_indices.get("componentType")
      def vbv_idx = s_values.get("bufferView")
      if(vbv_idx < 0 || vbv_idx >= bv_list.len){ free(m_ptr) return 0 }
      def vbv = bv_list.get(vbv_idx)
      def val_off = vbv.get("byteOffset", 0) + s_values.get("byteOffset", 0)
      def sbuf_idx = int(sbv.get("buffer", 0))
      def vbuf_idx = int(vbv.get("buffer", 0))
      mut sdata, vdata = data, data
      if(is_list(buffers_data) && sbuf_idx >= 0 && sbuf_idx < buffers_data.len){
         def b = buffers_data.get(sbuf_idx, 0)
         def bp = _gltf_blob_ptr(b)
         if(bp){ sdata = bp }
      }
      if(is_list(buffers_data) && vbuf_idx >= 0 && vbuf_idx < buffers_data.len){
         def b = buffers_data.get(vbuf_idx, 0)
         def bp = _gltf_blob_ptr(b)
         if(bp){ vdata = bp }
      }
      if(!sdata || !vdata){ free(m_ptr) return 0 }
      mut si = 0
      while(si < s_count){
         mut idx = 0
         if(idx_comp == 5121){ idx = load8(sdata, idx_off + si) }
         elif(idx_comp == 5123){ idx = u16le(sdata, idx_off + si * 2) }
         elif(idx_comp == 5125){ idx = u32le(sdata, idx_off + si * 4) }
         memcpy(ptr_add(m_ptr, idx * elem_size), ptr_add(vdata, val_off + si * elem_size), elem_size)
         si += 1
      }
      ptr = m_ptr
      stride = elem_size
      owned = true
   }
   def cols = _gltf_type_cols(type_str)
   def rows = _gltf_type_rows(type_str)
   mut out = {
      "ptr": ptr, "count": count, "stride": stride, "comp": comp,
      "normalized": normalized, "type_count": type_cnt, "type_str": type_str,
      "cols": cols, "rows": rows,
      "elem_size": elem_size, "owned": owned
   }
   if(cacheable && !owned){ _gltf_acc_res_cache = cache.cache_put_reset(_gltf_acc_res_cache, cache_key, out, _GLTF_CACHE_LIMIT_BIG, 256) }
   out
}

fn _gltf_release_accessor_data(any: acc_res): int {
   if(is_dict(acc_res) && acc_res.get("owned", false)){
      def ptr = acc_res.get("ptr", 0)
      if(ptr){ free(ptr) }
   }
   0
}

fn _gltf_primary_data_ptr(any: gltf_data): any {
   if(!is_dict(gltf_data)){ return 0 }
   def binary_data = gltf_data.get("binary_data", 0)
   if(is_dict(binary_data)){
      def ptr = binary_data.get("ptr", 0)
      if(ptr){ return ptr }
   } elif(is_str(binary_data) && binary_data.len > 0){
      return binary_data
   }
   def buffer_data = gltf_data.get("buffer_data", 0)
   if(is_list(buffer_data) && buffer_data.len > 0){
      def b0 = buffer_data.get(0, 0)
      def ptr0 = _gltf_blob_ptr(b0)
      if(ptr0){ return ptr0 }
   }
   def g = gltf_data.get("gltf", 0)
   mut ny_bufs = 0
   if(is_dict(g)){ ny_bufs = g.get("_ny_buffer_data", 0) }
   if(is_list(ny_bufs) && ny_bufs.len > 0){
      def b0 = ny_bufs.get(0, 0)
      def ptr0 = _gltf_blob_ptr(b0)
      if(ptr0){ return ptr0 }
   }
   0
}

fn _gltf_resolve_instancing_mats(any: g, any: node, any: data, any: local_to_world): any {
   def ext = node.get("extensions", 0)
   if(!is_dict(ext)){ return 0 }
   def gpu_ins = ext.get("EXT_mesh_gpu_instancing", 0)
   if(!is_dict(gpu_ins)){ return 0 }
   def attrs = gpu_ins.get("attributes", 0)
   if(!is_dict(attrs)){ return 0 }
   def t_acc_idx, r_acc_idx = attrs.get("TRANSLATION", -1), attrs.get("ROTATION", -1)
   def s_acc_idx = attrs.get("SCALE", -1)
   def t_res = _gltf_resolve_accessor_data(g, t_acc_idx, data)
   def r_res = _gltf_resolve_accessor_data(g, r_acc_idx, data)
   def s_res = _gltf_resolve_accessor_data(g, s_acc_idx, data)
   mut count = 1000000
   mut has_any = false
   if(t_res){ count = min(count, int(t_res.get("count", 0))) has_any = true }
   if(r_res){ count = min(count, int(r_res.get("count", 0))) has_any = true }
   if(s_res){ count = min(count, int(s_res.get("count", 0))) has_any = true }
   if(!has_any || count > 1000000){
      _gltf_release_accessor_data(t_res)
      _gltf_release_accessor_data(r_res)
      _gltf_release_accessor_data(s_res)
      return 0
   }
   if(count <= 0){
      _gltf_release_accessor_data(t_res)
      _gltf_release_accessor_data(r_res)
      _gltf_release_accessor_data(s_res)
      return 0
   }
   mut mats = []
   mut i = 0
   while(i < count){
      mut trans = [0.0, 0.0, 0.0]
      if(t_res){
         def ptr = t_res.get("ptr", 0)
         def stride = t_res.get("stride", 0)
         def comp = t_res.get("comp", GLTF_COMP_FLOAT)
         def norm = t_res.get("normalized", false)
         def cs = _gltf_comp_size(comp)
         def off = i * stride
         trans = [
            float(_gltf_read_f32_acc(ptr, off + cs * 0, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 1, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 2, comp, norm))
         ]
      }
      mut rot = [0.0, 0.0, 0.0, 1.0]
      if(r_res){
         def ptr = r_res.get("ptr", 0)
         def stride = r_res.get("stride", 0)
         def comp = r_res.get("comp", GLTF_COMP_FLOAT)
         def norm = r_res.get("normalized", false)
         def cs = _gltf_comp_size(comp)
         def off = i * stride
         rot = [
            float(_gltf_read_f32_acc(ptr, off + cs * 0, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 1, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 2, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 3, comp, norm))
         ]
      }
      mut scale = [1.0, 1.0, 1.0]
      if(s_res){
         def ptr = s_res.get("ptr", 0)
         def stride = s_res.get("stride", 0)
         def comp = s_res.get("comp", GLTF_COMP_FLOAT)
         def norm = s_res.get("normalized", false)
         def cs = _gltf_comp_size(comp)
         def off = i * stride
         scale = [
            float(_gltf_read_f32_acc(ptr, off + cs * 0, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 1, comp, norm)),
            float(_gltf_read_f32_acc(ptr, off + cs * 2, comp, norm))
         ]
      }
      def inst_local = gltf_math.mat4_from_trs(trans, rot, scale)
      mats = mats.append(gltf_math.mat4_mul(local_to_world, inst_local))
      i += 1
   }
   _gltf_release_accessor_data(t_res)
   _gltf_release_accessor_data(r_res)
   _gltf_release_accessor_data(s_res)
   mats
}

fn _gltf_acc_ptr_off(any: acc_res, any: data): int {
   def ptr = acc_res.get("ptr", 0)
   if(!ptr){ return -1 }
   ptr - data
}

fn _gltf_read_acc_f32(any: data, dict: acc_res, int: elem_idx, int: comp_idx): f64 {
   def ptr = acc_res.get("ptr", 0)
   if(!ptr){ return 0.0 }
   def stride = acc_res.get("stride", 4)
   def comp = acc_res.get("comp", GLTF_COMP_FLOAT)
   def norm = acc_res.get("normalized", false)
   def cs = _gltf_comp_size(comp)
   def cols = int(acc_res.get("cols", 1))
   def rows = int(acc_res.get("rows", acc_res.get("type_count", 1)))
   mut byte_off = 0
   if(cols <= 1){ byte_off = elem_idx * stride + comp_idx * cs } else {
      def col = comp_idx / rows
      def row = comp_idx % rows
      def col_size = _gltf_align_up(rows * cs, 4)
      byte_off = elem_idx * stride + col * col_size + row * cs
   }
   _gltf_read_f32_acc(ptr, byte_off, comp, norm)
}

fn _gltf_read_acc_components(any: data, any: acc_res, int: elem_idx, int: n_comp): list {
   mut out = []
   mut i = 0
   while(i < n_comp){
      out = out.append(_gltf_read_acc_f32(data, acc_res, elem_idx, i))
      i += 1
   }
   out
}

fn _gltf_read_acc_scalar_tuple(any: data, any: acc_res, int: tuple_idx, int: n_comp): list {
   "Read n_comp adjacent scalar accessor elements starting at tuple_idx*n_comp.
   Needed for animation outputs like morph weights, which are commonly
   stored as SCALAR accessors with count = keyframes * weight_count."
   mut out = []
   def base_idx = tuple_idx * n_comp
   mut i = 0
   while(i < n_comp){
      out = out.append(_gltf_read_acc_f32(data, acc_res, base_idx + i, 0))
      i += 1
   }
   out
}

fn _gltf_read_anim_tuple(any: data, any: output_res, int: idx, int: n_comp, bool: packed_scalar_tuple): list {
   if(packed_scalar_tuple){ return _gltf_read_acc_scalar_tuple(data, output_res, idx, n_comp) }
   _gltf_read_acc_components(data, output_res, idx, n_comp)
}

fn _gltf_lerp_vec(list: a, list: b, f64: t, int: n): list {
   mut out = []
   mut i = 0
   while(i < n){
      def av, bv = 0.0 + a.get(i, 0.0), 0.0 + b.get(i, 0.0)
      out = out.append(av + (bv - av) * t)
      i += 1
   }
   out
}

fn _gltf_nlerp_quat(list: a, list: b, f64: t): list {
   def ax, ay = 0.0 + a.get(0, 0.0), 0.0 + a.get(1, 0.0)
   def az, aw = 0.0 + a.get(2, 0.0), 0.0 + a.get(3, 1.0)
   mut bx, by = 0.0 + b.get(0, 0.0), 0.0 + b.get(1, 0.0)
   mut bz, bw = 0.0 + b.get(2, 0.0), 0.0 + b.get(3, 1.0)
   def dot = ax * bx + ay * by + az * bz + aw * bw
   if(dot < 0.0){
      bx, by = -bx, -by
      bz, bw = -bz, -bw
   }
   def rx, ry = ax + (bx - ax) * t, ay + (by - ay) * t
   def rz, rw = az + (bz - az) * t, aw + (bw - aw) * t
   def len2 = rx * rx + ry * ry + rz * rz + rw * rw
   def inv_len = len2 > 0.000001 ? 1.0 / sqrt(len2) : 1.0
   [rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len]
}

fn _gltf_normalize_quat(any: q): list {
   def x, y = 0.0 + q.get(0, 0.0), 0.0 + q.get(1, 0.0)
   def z, w = 0.0 + q.get(2, 0.0), 0.0 + q.get(3, 1.0)
   def len2 = x * x + y * y + z * z + w * w
   if(len2 <= 0.000001){ return [0.0, 0.0, 0.0, 1.0] }
   def inv_len = 1.0 / sqrt(len2)
   [x * inv_len, y * inv_len, z * inv_len, w * inv_len]
}

@inline
fn _gltf_read_norm_i16_quat(any: data_ptr, int: off): list {
   mut x = u16le(data_ptr, off + 0)
   mut y = u16le(data_ptr, off + 2)
   mut z = u16le(data_ptr, off + 4)
   mut w = u16le(data_ptr, off + 6)
   if(x >= 32768){ x = x - 65536 }
   if(y >= 32768){ y = y - 65536 }
   if(z >= 32768){ z = z - 65536 }
   if(w >= 32768){ w = w - 65536 }
   mut qx, qy = x * _GLTF_INV_32767, y * _GLTF_INV_32767
   mut qz, qw = z * _GLTF_INV_32767, w * _GLTF_INV_32767
   if(qx < -1.0){ qx = -1.0 }
   if(qy < -1.0){ qy = -1.0 }
   if(qz < -1.0){ qz = -1.0 }
   if(qw < -1.0){ qw = -1.0 }
   [qx, qy, qz, qw]
}

fn _gltf_find_time_bracket(any: data, any: input_res, f64: time_sec): list {
   def count = input_res.get("count", 0)
   if(count <= 0){ return [0, 0, 0.0] }
   if(count == 1){ return [0, 0, 0.0] }
   def t_first = _gltf_read_acc_f32(data, input_res, 0, 0)
   def t_last  = _gltf_read_acc_f32(data, input_res, count - 1, 0)
   if(time_sec <= t_first){ return [0, 0, 0.0] }
   if(time_sec >= t_last){ return [count-1, count-1, 0.0] }
   mut lo = 0
   mut hi = count - 1
   while(hi - lo > 1){
      def mid = (lo + hi) / 2
      def t_mid = _gltf_read_acc_f32(data, input_res, mid, 0)
      if(t_mid <= time_sec){ lo = mid }
      else { hi = mid }
   }
   def t_lo = _gltf_read_acc_f32(data, input_res, lo, 0)
   def t_hi = _gltf_read_acc_f32(data, input_res, hi, 0)
   def dt = t_hi - t_lo
   def alpha = dt > 0.00001 ? (time_sec - t_lo) / dt : 0.0
   [lo, hi, alpha]
}

fn _gltf_sample_channel(any: data, dict: sampler, dict: input_res, dict: output_res, f64: time_sec, int: n_comp, bool: is_rotation, bool: packed_scalar_tuple=false): list {
   def interp = to_str(sampler.get("interpolation", "LINEAR"))
   def bracket = _gltf_find_time_bracket(data, input_res, time_sec)
   def lo = int(bracket.get(0, 0))
   def hi = int(bracket.get(1, 0))
   def t_lo = _gltf_read_acc_f32(data, input_res, lo, 0)
   def t_hi = _gltf_read_acc_f32(data, input_res, hi, 0)
   def dt = t_hi - t_lo
   mut t = 0.0
   if(dt > 0.00001){ t = (time_sec - t_lo) / dt }
   def rot_short_norm =
   is_rotation &&
   n_comp == 4 &&
   output_res.get("comp", 0) == GLTF_COMP_SHORT &&
   output_res.get("normalized", false) &&
   int(output_res.get("cols", 1)) <= 1
   if(rot_short_norm){
      def ptr = output_res.get("ptr", 0)
      def stride = int(output_res.get("stride", 0))
      if(ptr && stride >= 8){
         def alo = lo * stride
         def ahi = hi * stride
         def qa = _gltf_read_norm_i16_quat(ptr, alo)
         if(lo == hi || eq(interp, "STEP")){ return qa }
         def qb = _gltf_read_norm_i16_quat(ptr, ahi)
         def qa0, qa1 = qa.get(0, 0.0), qa.get(1, 0.0)
         def qa2, qa3 = qa.get(2, 0.0), qa.get(3, 1.0)
         mut lbx, lby = qb.get(0, 0.0), qb.get(1, 0.0)
         mut lbz, lbw = qb.get(2, 0.0), qb.get(3, 1.0)
         def dot = qa0 * lbx + qa1 * lby + qa2 * lbz + qa3 * lbw
         if(dot < 0.0){
            lbx, lby = -lbx, -lby
            lbz, lbw = -lbz, -lbw
         }
         def rx, ry = qa0 + (lbx - qa0) * t, qa1 + (lby - qa1) * t
         def rz, rw = qa2 + (lbz - qa2) * t, qa3 + (lbw - qa3) * t
         def len2 = rx * rx + ry * ry + rz * rz + rw * rw
         if(len2 <= 0.000001){ return [0.0, 0.0, 0.0, 1.0] }
         def inv_len = 1.0 / sqrt(len2)
         return [rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len]
      }
   }
   if(lo == hi){
      def base_idx = eq(interp, "CUBICSPLINE") ? lo * 3 + 1 : lo
      return _gltf_read_anim_tuple(data, output_res, base_idx, n_comp, packed_scalar_tuple)
   }
   if(eq(interp, "STEP")){
      return _gltf_read_anim_tuple(data, output_res, lo, n_comp, packed_scalar_tuple)
   }
   if(eq(interp, "CUBICSPLINE")){
      def tk = _gltf_read_acc_f32(data, input_res, lo, 0)
      def tk1 = _gltf_read_acc_f32(data, input_res, hi, 0)
      def td = tk1 - tk
      def vk = _gltf_read_anim_tuple(data, output_res, lo * 3 + 1, n_comp, packed_scalar_tuple)
      def bk = _gltf_read_anim_tuple(data, output_res, lo * 3 + 2, n_comp, packed_scalar_tuple)
      def ak1 = _gltf_read_anim_tuple(data, output_res, hi * 3 + 0, n_comp, packed_scalar_tuple)
      def vk1 = _gltf_read_anim_tuple(data, output_res, hi * 3 + 1, n_comp, packed_scalar_tuple)
      def t2, t3 = t * t, t2 * t
      def h00, h10 = 2.0 * t3 - 3.0 * t2 + 1.0, t3 - 2.0 * t2 + t
      def h01, h11 = -2.0 * t3 + 3.0 * t2, t3 - t2
      mut out = []
      mut i = 0
      while(i < n_comp){
         out = out.append(h00 * (0.0 + vk.get(i, 0.0)) +
            h10 * td * (0.0 + bk.get(i, 0.0)) +
            h01 * (0.0 + vk1.get(i, 0.0)) +
         h11 * td * (0.0 + ak1.get(i, 0.0)))
         i += 1
      }
      if(is_rotation){ return _gltf_normalize_quat(out) }
      return out
   }
   def a_val = _gltf_read_anim_tuple(data, output_res, lo, n_comp, packed_scalar_tuple)
   def b_val = _gltf_read_anim_tuple(data, output_res, hi, n_comp, packed_scalar_tuple)
   if(is_rotation){ return _gltf_nlerp_quat(a_val, b_val, t) }
   _gltf_lerp_vec(a_val, b_val, t, n_comp)
}

fn gltf_animation_count(any: gltf_data): int {
   "Returns the number of animations in the glTF asset."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def anims = g.get("animations", 0)
   is_list(anims) ? anims.len : 0
}

fn _gltf_animation_duration_from_anim(any: doc, int: anim_idx): f64 {
   if(!is_dict(doc)){ return 0.0 }
   def anims = doc.get("animations", 0)
   def accs = doc.get("accessors", 0)
   if(!is_list(anims) || !is_list(accs) || anim_idx < 0 || anim_idx >= anims.len){ return 0.0 }
   def anim = anims[anim_idx]
   if(!is_dict(anim)){ return 0.0 }
   def samplers = anim.get("samplers", [])
   if(!is_list(samplers)){ return 0.0 }
   def samplers_n = samplers.len
   def accs_n = accs.len
   mut duration = 0.0
   mut si = 0
   while(si < samplers_n){
      def samp = samplers[si]
      if(!is_dict(samp)){ si += 1 continue }
      def input_raw = samp.get("input", -1)
      if(!is_int(input_raw) && !is_float(input_raw)){ si += 1 continue }
      def input_idx = int(input_raw)
      if(input_idx >= 0 && input_idx < accs_n){
         def input_acc = accs[input_idx]
         if(is_dict(input_acc)){
            def acc_max = input_acc.get("max", 0)
            if(is_list(acc_max) && acc_max.len > 0){
               def last_t = _gltf_num_or(acc_max.get(0, 0.0), 0.0)
               if(last_t > duration){ duration = last_t }
            }
         }
      }
      si += 1
   }
   duration
}

fn _gltf_animation_duration_from_samples(any: gltf_data, int: anim_idx): f64 {
   if(!is_dict(gltf_data)){ return 0.0 }
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0.0 }
   def data = _gltf_primary_data_ptr(gltf_data)
   if(!data){ return 0.0 }
   def anims = g.get("animations", 0)
   if(!is_list(anims) || anim_idx < 0 || anim_idx >= anims.len){ return 0.0 }
   def anim = anims[anim_idx]
   if(!is_dict(anim)){ return 0.0 }
   def samplers = anim.get("samplers", [])
   if(!is_list(samplers)){ return 0.0 }
   def samplers_n = samplers.len
   mut duration = 0.0
   mut si = 0
   while(si < samplers_n){
      def samp = samplers[si]
      if(!is_dict(samp)){ si += 1 continue }
      def input_idx = int(samp.get("input", -1))
      def input_res = _gltf_resolve_accessor_data(g, input_idx, data)
      if(is_dict(input_res)){
         def count = int(input_res.get("count", 0))
         if(count > 0){
            def last_t = _gltf_read_acc_f32(data, input_res, count - 1, 0)
            if(_gltf_anim_duration_valid(last_t) && last_t > duration){ duration = last_t }
         }
         _gltf_release_accessor_data(input_res)
      }
      si += 1
   }
   duration
}

fn gltf_animation_info(any: gltf_data, int: anim_idx): any {
   "Returns {name, duration} for animation at anim_idx. Duration in seconds."
   _gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def anims = g.get("animations", 0)
   if(!is_list(anims) || anim_idx < 0 || anim_idx >= anims.len){ return 0 }
   def anims_n = anims.len
   def anim_cache_key = to_str(gltf_data.get("source_path", "")) + "|" + to_str(anim_idx) + "|" + to_str(anims_n)
   if(_gltf_anim_info_cache.contains(anim_cache_key)){ return _gltf_anim_info_cache.get(anim_cache_key, 0) }
   def anim = anims.get(anim_idx)
   mut duration = _gltf_animation_duration_from_anim(g, anim_idx)
   if(!_gltf_anim_duration_valid(duration)){ duration = _gltf_animation_duration_from_samples(gltf_data, anim_idx) }
   if(!_gltf_anim_duration_valid(duration)){ duration = float(gltf_data.get("anim_duration_hint", 0.0)) }
   if(!_gltf_anim_duration_valid(duration)){ duration = 0.0 }
   def out = {"name": to_str(anim.get("name", "")), "duration": duration}
   _gltf_anim_info_cache = cache.cache_put_reset(_gltf_anim_info_cache, anim_cache_key, out, _GLTF_CACHE_LIMIT_MED, 64)
   out
}

fn _gltf_anim_fast_records(any: gltf_data, int: anim_idx): any {
   _gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def data = _gltf_primary_data_ptr(gltf_data)
   if(!data){ return 0 }
   def key = _gltf_cache_key_from_data(gltf_data) + ":data:" + to_str(to_int(data)) + ":anim:" + to_str(int(anim_idx))
   if(_gltf_anim_sample_cache.contains(key)){ return _gltf_anim_sample_cache.get(key, 0) }
   def anims = g.get("animations", 0)
   if(!is_list(anims) || anim_idx < 0 || anim_idx >= anims.len){ return 0 }
   def anim = anims.get(anim_idx)
   if(!is_dict(anim)){ return 0 }
   def channels = anim.get("channels", [])
   def samplers = anim.get("samplers", [])
   if(!is_list(channels) || !is_list(samplers)){ return 0 }
   def channels_n = channels.len
   def samplers_n = samplers.len
   mut records = list(0)
   mut ci = 0
   while(ci < channels_n){
      def ch = channels[ci]
      def tgt = is_dict(ch) ? ch.get("target", 0) : 0
      def samp_idx = is_dict(ch) ? int(ch.get("sampler", -1)) : -1
      if(!is_dict(tgt) || samp_idx < 0 || samp_idx >= samplers_n){ return 0 }
      if(is_dict(tgt.get("extensions", 0))){ return 0 }
      def node_idx = int(tgt.get("node", -1))
      def path = to_str(tgt.get("path", ""))
      mut path_code = 0
      mut n_comp = 0
      if(path == "translation"){ path_code = 1 n_comp = 3 }
      elif(path == "rotation"){ path_code = 2 n_comp = 4 }
      elif(path == "scale"){ path_code = 3 n_comp = 3 }
      else { return 0 }
      if(node_idx < 0){ return 0 }
      def samp = samplers[samp_idx]
      if(!is_dict(samp)){ return 0 }
      if(to_str(samp.get("interpolation", "LINEAR")) != "LINEAR"){ return 0 }
      def input_res = _gltf_resolve_accessor_data(g, int(samp.get("input", -1)), data)
      def output_res = _gltf_resolve_accessor_data(g, int(samp.get("output", -1)), data)
      if(!is_dict(input_res) || !is_dict(output_res)){ return 0 }
      if(input_res.get("owned", false) || output_res.get("owned", false)){ return 0 }
      if(int(input_res.get("comp", 0)) != GLTF_COMP_FLOAT || int(output_res.get("comp", 0)) != GLTF_COMP_FLOAT){ return 0 }
      if(int(input_res.get("type_count", 1)) != 1 || int(output_res.get("type_count", 0)) != n_comp){ return 0 }
      def in_ptr = input_res.get("ptr", 0)
      def out_ptr = output_res.get("ptr", 0)
      def count = int(input_res.get("count", 0))
      if(!in_ptr || !out_ptr || count <= 0 || int(output_res.get("count", 0)) < count){ return 0 }
      records = records.append([node_idx, path_code, in_ptr, out_ptr, count, int(input_res.get("stride", 4)), int(output_res.get("stride", n_comp * 4)), n_comp, 0])
      ci += 1
   }
   if(records.len != channels_n){ return 0 }
   _gltf_anim_sample_cache = cache.cache_put_reset(_gltf_anim_sample_cache, key, records, _GLTF_CACHE_LIMIT_MED, 64)
   records
}

fn _gltf_anim_record_bracket(any: rec, f64: time_sec): list {
   def in_ptr = rec.get(2, 0)
   def count = int(rec.get(4, 0))
   def stride = int(rec.get(5, 4))
   if(!in_ptr || count <= 1){ return [0, 0, 0.0] }
   def first_t = f32le(in_ptr, 0)
   if(time_sec <= first_t){
      rec[8] = 0
      return [0, 0, 0.0]
   }
   def last_t = f32le(in_ptr, (count - 1) * stride)
   if(time_sec >= last_t){
      rec[8] = count - 1
      return [count - 1, count - 1, 0.0]
   }
   mut lo = int(rec.get(8, 0))
   if(lo < 0 || lo >= count - 1){ lo = 0 }
   mut t_lo = f32le(in_ptr, lo * stride)
   mut t_hi = f32le(in_ptr, (lo + 1) * stride)
   while(lo > 0 && time_sec < t_lo){
      lo -= 1
      t_lo = f32le(in_ptr, lo * stride)
      t_hi = f32le(in_ptr, (lo + 1) * stride)
   }
   while(lo + 1 < count - 1 && time_sec >= t_hi){
      lo += 1
      t_lo = t_hi
      t_hi = f32le(in_ptr, (lo + 1) * stride)
   }
   rec[8] = lo
   def dt = t_hi - t_lo
   def alpha = dt > 0.00001 ? (time_sec - t_lo) / dt : 0.0
   [lo, lo + 1, alpha]
}

fn _gltf_anim_fast_value(any: rec, f64: time_sec): list {
   def raw = __gltf_anim_fast_value_raw(rec, time_sec)
   if(is_list(raw)){ return raw }
   def br = _gltf_anim_record_bracket(rec, time_sec)
   def lo = int(br.get(0, 0))
   def hi = int(br.get(1, 0))
   def t = float(br.get(2, 0.0))
   def out_ptr = rec.get(3, 0)
   def stride = int(rec.get(6, 0))
   def n_comp = int(rec.get(7, 0))
   def lo_off = lo * stride
   def hi_off = hi * stride
   if(n_comp == 4){
      def ax, ay = f32le(out_ptr, lo_off + 0), f32le(out_ptr, lo_off + 4)
      def az, aw = f32le(out_ptr, lo_off + 8), f32le(out_ptr, lo_off + 12)
      mut bx, by = f32le(out_ptr, hi_off + 0), f32le(out_ptr, hi_off + 4)
      mut bz, bw = f32le(out_ptr, hi_off + 8), f32le(out_ptr, hi_off + 12)
      def dot = ax * bx + ay * by + az * bz + aw * bw
      if(dot < 0.0){ bx = -bx by = -by bz = -bz bw = -bw }
      def rx, ry = ax + (bx - ax) * t, ay + (by - ay) * t
      def rz, rw = az + (bz - az) * t, aw + (bw - aw) * t
      def len2 = rx * rx + ry * ry + rz * rz + rw * rw
      if(len2 <= 0.000001){ return [0.0, 0.0, 0.0, 1.0] }
      def inv_len = 1.0 / sqrt(len2)
      return [rx * inv_len, ry * inv_len, rz * inv_len, rw * inv_len]
   }
   def ax, ay = f32le(out_ptr, lo_off + 0), f32le(out_ptr, lo_off + 4)
   def az = f32le(out_ptr, lo_off + 8)
   def bx = f32le(out_ptr, hi_off + 0)
   def by = f32le(out_ptr, hi_off + 4)
   def bz = f32le(out_ptr, hi_off + 8)
   [ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t]
}

fn _gltf_sample_animation_fast(any: gltf_data, int: anim_idx, f64: time_sec): any {
   if(!common.env_truthy("NY_GLTF_ANIM_FAST")){ return 0 }
   def records = _gltf_anim_fast_records(gltf_data, anim_idx)
   if(!is_list(records)){ return 0 }
   def g = gltf_data.get("gltf", 0)
   def nodes = is_dict(g) ? g.get("nodes", 0) : 0
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut overrides = dict(max(16, nodes_n * 2))
   def rec_n = records.len
   mut i = 0
   while(i < rec_n){
      def rec = records[i]
      def node_idx = int(rec.get(0, -1))
      def path_code = int(rec.get(1, 0))
      def val = _gltf_anim_fast_value(rec, time_sec)
      mut node_ov = overrides.get(node_idx, 0)
      if(!is_dict(node_ov)){ node_ov = {"node": node_idx} }
      if(path_code == 1){ node_ov["T"] = val }
      elif(path_code == 2){ node_ov["R"] = val }
      elif(path_code == 3){ node_ov["S"] = val }
      overrides[node_idx] = node_ov
      i += 1
   }
   overrides["__fast_numeric"] = true
   overrides
}

fn _gltf_anim_sample_component_count(str: path, bool: is_visibility_pointer, any: ptr_mat_target, any: input_res, any: output_res, int: key_mult): int {
   if(is_visibility_pointer){ return 1 }
   if(is_dict(ptr_mat_target)){
      def kind = to_str(ptr_mat_target.get("kind", ""))
      if(kind == "baseColorFactor"){ return 4 }
      if(kind == "emissiveFactor"){ return 3 }
      if(kind == "metallicFactor" || kind == "roughnessFactor" || kind == "alphaCutoff"){ return 1 }
      if(kind == "uvOffset" || kind == "uvScale"){ return 2 }
      if(kind == "uvRotation"){ return 1 }
   }
   if(eq(path, "rotation")){ return 4 }
   if(eq(path, "weights")){ return max(1, int(output_res.get("count", 0)) / max(1, int(input_res.get("count", 1)) * key_mult)) }
   3
}

fn _gltf_anim_trace_rotation(
   any: data, dict: input_res, dict: output_res, int: anim_idx, int: node_idx,
   f64: time_sec, int: input_idx, int: output_idx, any: val
): bool {
   def lo_dbg = _gltf_find_time_bracket(data, input_res, time_sec)
   def lo_i = int(lo_dbg.get(0, 0))
   def hi_i = int(lo_dbg.get(1, 0))
   def a0 = _gltf_read_acc_f32(data, output_res, lo_i, 0)
   def a1 = _gltf_read_acc_f32(data, output_res, lo_i, 1)
   def a2 = _gltf_read_acc_f32(data, output_res, lo_i, 2)
   def a3 = _gltf_read_acc_f32(data, output_res, lo_i, 3)
   def b0 = _gltf_read_acc_f32(data, output_res, hi_i, 0)
   def b1 = _gltf_read_acc_f32(data, output_res, hi_i, 1)
   def b2 = _gltf_read_acc_f32(data, output_res, hi_i, 2)
   def b3 = _gltf_read_acc_f32(data, output_res, hi_i, 3)
   def optr = output_res.get("ptr", 0)
   print(
      "[gltf:anim] clip=" + to_str(anim_idx) +
      " node=" + to_str(node_idx) +
      " t=" + to_str(time_sec) +
      " in=" + to_str(input_idx) +
      " out=" + to_str(output_idx) +
      " icount=" + to_str(input_res.get("count", -1)) +
      " ocount=" + to_str(output_res.get("count", -1)) +
      " stride=" + to_str(output_res.get("stride", -1)) +
      " comp=" + to_str(output_res.get("comp", -1)) +
      " norm=" + to_str(output_res.get("normalized", false)) +
      " typec=" + to_str(output_res.get("type_count", -1)) +
      " in_off=" + to_str(_gltf_acc_ptr_off(input_res, data)) +
      " out_off=" + to_str(_gltf_acc_ptr_off(output_res, data)) +
      " raw16=(" + to_str(optr ? u16le(optr, 0) : -1) + "," + to_str(optr ? u16le(optr, 2) : -1) + "," + to_str(optr ? u16le(optr, 4) : -1) + "," + to_str(optr ? u16le(optr, 6) : -1) + ")" +
      " lo=" + to_str(lo_i) +
      " hi=" + to_str(hi_i) +
      " A=(" + to_str(a0) + "," + to_str(a1) + "," + to_str(a2) + "," + to_str(a3) + ")" +
      " B=(" + to_str(b0) + "," + to_str(b1) + "," + to_str(b2) + "," + to_str(b3) + ")" +
      " val=" + to_str(val)
   )
   true
}

fn _gltf_anim_store_override(
   dict: overrides, bool: is_material_pointer, any: ptr_mat_target,
   bool: is_visibility_pointer, int: ptr_vis_node_idx, int: node_idx, str: path, any: val
): dict {
   if(is_material_pointer){
      mut ptrs = overrides.get("__pointers", [])
      ptrs = ptrs.append({"material": int(ptr_mat_target.get("material", -1)), "kind": to_str(ptr_mat_target.get("kind", "")), "value": val})
      overrides["__pointers"] = ptrs
      return overrides
   }
   def dst_node_idx = is_visibility_pointer ? ptr_vis_node_idx : node_idx
   mut node_ov = _gltf_anim_override_for_node(overrides, dst_node_idx)
   if(!is_dict(node_ov)){ node_ov = dict(4) }
   node_ov["node"] = dst_node_idx
   node_ov["node_key"] = to_str(dst_node_idx)
   def key = is_visibility_pointer ? "VIS" : (eq(path, "translation") ? "T" : (eq(path, "rotation") ? "R" : (eq(path, "scale") ? "S" : "W")))
   if(is_visibility_pointer){
      def visf = 0.0 + val.get(0, 1.0)
      node_ov[key] = visf >= 0.5
   } else {
      node_ov[key] = val
   }
   overrides[dst_node_idx] = node_ov
   overrides[to_str(dst_node_idx)] = node_ov
   mut nodes_ov = overrides.get("__nodes", [])
   mut found = false
   mut ni = 0
   while(is_list(nodes_ov) && ni < nodes_ov.len){
      def rec = nodes_ov[ni]
      if(is_dict(rec) && to_str(rec.get("node_key", "")) == to_str(dst_node_idx)){
         nodes_ov[ni] = node_ov
         found = true
         break
      }
      ni += 1
   }
   if(!found){ nodes_ov = nodes_ov.append(node_ov) }
   overrides["__nodes"] = nodes_ov
   overrides
}

fn gltf_sample_animation(any: gltf_data, int: anim_idx, f64: time_sec): any {
   "Sample animation at time_sec. Returns {node_idx: {T:[x,y,z], R:[x,y,z,w], S:[x,y,z]}} overrides."
   def fast = _gltf_sample_animation_fast(gltf_data, anim_idx, time_sec)
   if(is_dict(fast)){ return fast }
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def anims = g.get("animations", 0)
   if(!is_list(anims) || anim_idx < 0 || anim_idx >= anims.len){ return 0 }
   def anim = anims.get(anim_idx)
   def data = _gltf_primary_data_ptr(gltf_data)
   if(!data){ return 0 }
   def channels = anim.get("channels", [])
   def samplers  = anim.get("samplers", [])
   def nodes = g.get("nodes", 0)
   def channels_n = is_list(channels) ? channels.len : 0
   def samplers_n = is_list(samplers) ? samplers.len : 0
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut overrides = dict(max(16, nodes_n * 2))
   mut ci = 0
   while(ci < channels_n){
      def ch = channels.get(ci)
      def tgt = ch.get("target", 0)
      def samp_idx = int(ch.get("sampler", -1))
      if(is_dict(tgt) && samp_idx >= 0 && samp_idx < samplers_n){
         def tgt_ext = tgt.get("extensions", 0)
         def ptr_ext = is_dict(tgt_ext) ? tgt_ext.get("KHR_animation_pointer", 0) : 0
         def ptr = is_dict(ptr_ext) ? to_str(ptr_ext.get("pointer", "")) : ""
         def node_idx = int(tgt.get("node", -1))
         def path = to_str(tgt.get("path", ""))
         def ptr_vis_node_idx = _gltf_pointer_node_visibility_idx(ptr)
         def is_visibility_pointer = ptr_vis_node_idx >= 0
         def ptr_mat_target = _gltf_pointer_material_target(ptr)
         def is_material_pointer = is_dict(ptr_mat_target)
         if((node_idx >= 0
               && (eq(path, "translation")
                  || eq(path, "rotation")
                  || eq(path, "scale")
            || eq(path, "weights")))
            || is_visibility_pointer
            || is_material_pointer){
            def samp = samplers.get(samp_idx)
            def input_idx  = int(samp.get("input",  -1))
            def output_idx = int(samp.get("output", -1))
            def input_res  = _gltf_resolve_accessor_data(g, input_idx,  data)
            def output_res = _gltf_resolve_accessor_data(g, output_idx, data)
            if(is_dict(input_res) && is_dict(output_res)){
               def key_mult = eq(to_str(samp.get("interpolation", "LINEAR")), "CUBICSPLINE") ? 3 : 1
               def n_comp = _gltf_anim_sample_component_count(path, is_visibility_pointer, ptr_mat_target, input_res, output_res, key_mult)
               def is_rot = eq(path, "rotation")
               def packed_scalar_tuple =
               eq(path, "weights") &&
               int(output_res.get("type_count", 1)) == 1 &&
               n_comp > 1
               def val = _gltf_sample_channel(data, samp, input_res, output_res, time_sec, n_comp, is_rot, packed_scalar_tuple)
               if(_gltf_anim_trace_enabled() && is_rot){ _gltf_anim_trace_rotation(data, input_res, output_res, anim_idx, node_idx, time_sec, input_idx, output_idx, val) }
               overrides = _gltf_anim_store_override(overrides, is_material_pointer, ptr_mat_target, is_visibility_pointer, ptr_vis_node_idx, node_idx, path, val)
            }
            _gltf_release_accessor_data(input_res)
            _gltf_release_accessor_data(output_res)
         }
      }
      ci += 1
   }
   overrides
}

fn gltf_sample_animation_merged(any: gltf_data, f64: time_sec): any {
   "Samples all animation clips at time_sec and merges their overrides."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return 0 }
   def anims = g.get("animations", 0)
   if(!is_list(anims) || anims.len == 0){ return 0 }
   def anims_n = anims.len
   def node_defs = g.get("nodes", 0)
   mut merged = dict(is_list(node_defs) ? max(16, node_defs.len * 2) : 16)
   mut ai = 0
   while(ai < anims_n){
      def clip = gltf_sample_animation(gltf_data, ai, time_sec)
      if(is_dict(clip)){
         def clip_nodes = clip.get("__nodes", [])
         if(is_list(clip_nodes)){
            def clip_nodes_n = clip_nodes.len
            mut ni = 0
            while(ni < clip_nodes_n){
               def rec = clip_nodes[ni]
               if(is_dict(rec)){
                  def node_idx = int(rec.get("node", -1))
                  if(node_idx >= 0){
                     merged[node_idx] = rec
                     merged[to_str(node_idx)] = rec
                     mut merged_nodes = merged.get("__nodes", [])
                     mut found = false
                     mut mi = 0
                     def merged_nodes_n = is_list(merged_nodes) ? merged_nodes.len : 0
                     while(mi < merged_nodes_n){
                        def prev = merged_nodes[mi]
                        if(is_dict(prev) && int(prev.get("node", -1)) == node_idx){
                           merged_nodes[mi] = rec
                           found = true
                           break
                        }
                        mi += 1
                     }
                     if(!found){ merged_nodes = merged_nodes.append(rec) }
                     merged["__nodes"] = merged_nodes
                  }
               }
               ni += 1
            }
         }
         def ptrs = clip.get("__pointers", [])
         if(is_list(ptrs)){
            mut merged_ptrs = merged.get("__pointers", [])
            def ptrs_n = ptrs.len
            mut pi = 0
            while(pi < ptrs_n){
               merged_ptrs = merged_ptrs.append(ptrs[pi])
               pi += 1
            }
            merged["__pointers"] = merged_ptrs
         }
      }
      ai += 1
   }
   merged
}

fn gltf_apply_morph_weights(any: gltf_data, any: overrides): list {
   "Applies sampled node weights overrides onto referenced mesh.weights in gltf_data.
   Returns [updated_gltf_data, changed]."
   if(!is_dict(gltf_data) || !is_dict(overrides)){ return [gltf_data, false] }
   mut g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [gltf_data, false] }
   def nodes = g.get("nodes", 0)
   mut meshes = g.get("meshes", 0)
   if(!is_list(nodes) || !is_list(meshes)){ return [gltf_data, false] }
   def nodes_n = nodes.len
   def meshes_n = meshes.len
   mut changed = false
   mut ni = 0
   while(ni < nodes_n){
      def node = nodes[ni]
      if(is_dict(node)){
         def node_ov = overrides.get(ni, 0)
         if(is_dict(node_ov)){
            def w = node_ov.get("W", 0)
            if(is_list(w)){
               def mesh_idx = int(node.get("mesh", -1))
               if(mesh_idx >= 0 && mesh_idx < meshes_n){
                  mut mesh = meshes[mesh_idx]
                  if(is_dict(mesh)){
                     def w_n = w.len
                     mut out_w = list(w_n)
                     mut wi = 0
                     while(wi < w_n){
                        __store_item_fast(out_w, wi, float(w[wi]))
                        wi += 1
                     }
                     __list_set_len(out_w, w_n)
                     mesh["weights"] = out_w
                     meshes[mesh_idx] = mesh
                     changed = true
                  }
               }
            }
         }
      }
      ni += 1
   }
   if(!changed){ return [gltf_data, false] }
   g["meshes"] = meshes
   gltf_data["gltf"] = g
   [gltf_data, true]
}

fn _gltf_build_node_world_mats_animated(dict: g, int: node_idx, list: parent_m, dict: node_world_mats, dict: overrides): dict {
   def nodes = g.get("nodes")
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return node_world_mats }
   def visit_key = _gltf_node_visit_key(node_idx)
   if(node_world_mats.get(visit_key, false)){ return node_world_mats }
   if(node_world_mats.contains(node_idx)){ return node_world_mats }
   def node = nodes[node_idx]
   if(!is_dict(node)){ return node_world_mats }
   node_world_mats[visit_key] = true
   def anim_ov = _gltf_anim_override_for_node(overrides, node_idx)
   mut local_m = 0
   if(is_dict(anim_ov)){
      def nodes_g = g.get("nodes")
      def orig_node = is_list(nodes_g) ? nodes_g[node_idx] : 0
      def t = anim_ov.get("T", is_dict(orig_node) ? orig_node.get("translation", [0.0,0.0,0.0]) : [0.0,0.0,0.0])
      def r_raw = anim_ov.get("R", is_dict(orig_node) ? orig_node.get("rotation", [0.0,0.0,0.0,1.0]) : [0.0,0.0,0.0,1.0])
      def r = _gltf_normalize_quat(r_raw)
      def s = anim_ov.get("S", is_dict(orig_node) ? orig_node.get("scale",       [1.0,1.0,1.0]) : [1.0,1.0,1.0])
      local_m = gltf_math.mat4_from_trs(t, r, s)
   } else {
      local_m = gltf_math.node_local_matrix(node)
   }
   def world_m = gltf_math.mat4_mul(parent_m, local_m)
   node_world_mats[node_idx] = world_m
   def children = node.get("children")
   if(is_list(children)){
      def children_n = children.len
      mut i = 0
      while(i < children_n){
         def child_idx = int(children[i])
         if(child_idx >= 0 && child_idx != node_idx){ node_world_mats = _gltf_build_node_world_mats_animated(g, child_idx, world_m, node_world_mats, overrides) }
         i += 1
      }
   }
   node_world_mats = node_world_mats.delete(visit_key)
   node_world_mats
}

fn _gltf_build_node_world_mats_animated_fast(list: nodes, any: base_local_mats, list: world_list, int: node_idx, list: parent_m, dict: node_world_mats, dict: overrides): dict {
   if(!is_list(nodes) || node_idx < 0 || node_idx >= nodes.len){ return node_world_mats }
   def node = nodes[node_idx]
   if(!is_dict(node)){ return node_world_mats }
   def anim_ov = overrides.get(int(node_idx), 0)
   mut local_m = 0
   if(is_dict(anim_ov)){
      def t = anim_ov.get("T", node.get("translation", [0.0,0.0,0.0]))
      def r_raw = anim_ov.get("R", node.get("rotation", [0.0,0.0,0.0,1.0]))
      def r = _gltf_normalize_quat(r_raw)
      def s = anim_ov.get("S", node.get("scale", [1.0,1.0,1.0]))
      local_m = gltf_math.mat4_from_trs(t, r, s)
   } else {
      local_m = base_local_mats.get(node_idx, gltf_math.mat4_identity())
   }
   def world_m = gltf_math.mat4_mul(parent_m, local_m)
   node_world_mats[node_idx] = world_m
   if(is_list(world_list) && node_idx >= 0 && node_idx < world_list.len){ world_list[node_idx] = world_m }
   def children = node.get("children")
   if(is_list(children)){
      def children_n = children.len
      mut i = 0
      while(i < children_n){
         def child_idx = int(children[i])
         if(child_idx >= 0 && child_idx != node_idx){
            node_world_mats = _gltf_build_node_world_mats_animated_fast(nodes,
               base_local_mats,
               world_list,
               child_idx,
               world_m,
               node_world_mats,
            overrides)
         }
         i += 1
      }
   }
   node_world_mats
}

fn gltf_rebuild_animated_mats(any: gltf_data, any: overrides): dict {
   "Rebuild world matrices for all nodes applying TRS overrides from gltf_sample_animation.
   Returns {node_idx: mat4} dict. Pass result node_idx lookups to update gpu_parts model matrices."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return dict(0) }
   if(!is_dict(overrides)){ return dict(0) }
   def nodes = g.get("nodes", 0)
   def nodes_n = is_list(nodes) ? nodes.len : 0
   mut node_world_mats = dict(max(16, nodes_n * 2))
   def fast_numeric = overrides.get("__fast_numeric", false) ? true : false
   def base_local_mats = fast_numeric ? _gltf_node_local_mats(g) : 0
   mut world_list = 0
   if(fast_numeric){
      world_list = list(nodes_n)
      mut wi = 0
      while(wi < nodes_n){
         world_list = world_list.append(0)
         wi += 1
      }
   }
   def scenes = g.get("scenes")
   def scene_idx = _gltf_active_scene_idx(g, gltf_data)
   if(is_list(scenes) && scene_idx >= 0 && scene_idx < scenes.len){
      def scene = scenes.get(scene_idx)
      def roots = scene.get("nodes")
      if(is_list(roots)){
         def roots_n = roots.len
         def id = gltf_math.mat4_identity()
         mut ri = 0
         while(ri < roots_n){
            def root_idx = int(roots[ri])
            if(fast_numeric){
               node_world_mats = _gltf_build_node_world_mats_animated_fast(nodes,
                  base_local_mats,
                  world_list,
                  root_idx,
                  id,
                  node_world_mats,
               overrides)
            } else {
               node_world_mats = _gltf_build_node_world_mats_animated(g, root_idx, id, node_world_mats, overrides)
            }
            ri += 1
         }
      }
   }
   if(fast_numeric && is_list(world_list)){ node_world_mats["__world_list"] = world_list }
   node_world_mats
}

fn _gltf_ext_obj(any: obj, str: name): any {
   if(!is_dict(obj)){ return 0 }
   def ex = obj.get("extensions", 0)
   is_dict(ex) ? ex.get(name, 0) : 0
}

fn _gltf_has_uri(any: info, str: slot_key): bool { to_str(info.get(slot_key + "_uri", "")) != "" }

fn _gltf_material_uri_feature_mask(any: info): int {
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
   while(i < n){
      def row = rows[i]
      if(_gltf_has_uri(info, to_str(row[0]))){ m = bor(m, int(row[1])) }
      i += 1
   }
   m
}

fn _gltf_material_gt_feature_mask(any: info): int {
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
   while(i < n){
      def row = rows[i]
      if(float(info.get(to_str(row[0]), row[1])) > float(row[1])){ m = bor(m, int(row[2])) }
      i += 1
   }
   m
}

fn _gltf_material_ne_feature_mask(any: info): int {
   def rows = [
      ["specular_factor", 1.0, 16],
      ["ior", 1.5, 512],
      ["alpha_coverage", 1.0, 16384]
   ]
   mut m = 0
   def n = rows.len
   mut i = 0
   while(i < n){
      def row = rows[i]
      if(float(info.get(to_str(row[0]), row[1])) != float(row[1])){ m = bor(m, int(row[2])) }
      i += 1
   }
   m
}

fn _gltf_material_bool_feature_mask(any: info): int {
   def rows = [
      ["unlit", 131072],
      ["specular_glossiness", 262144]
   ]
   mut m = 0
   def n = rows.len
   mut i = 0
   while(i < n){
      def row = rows[i]
      if(info.get(to_str(row[0]), false)){ m = bor(m, int(row[1])) }
      i += 1
   }
   m
}

fn gltf_material_feature_mask(any: info): int {
   "Returns a feature bitmask for a normalized material info dict. Unwired shader passes can consume this directly."
   if(!is_dict(info)){ return 0 }
   mut m = _gltf_material_uri_feature_mask(info)
   m = bor(m, _gltf_material_gt_feature_mask(info))
   m = bor(m, _gltf_material_ne_feature_mask(info))
   m = bor(m, _gltf_material_bool_feature_mask(info))
   m
}

fn _gltf_slot_rec(any: info, str: slot_key): dict {
   mut r = {
      "slot": slot_key,
      "uri": to_str(info.get(slot_key + "_uri", "")),
      "texcoord": int(info.get(slot_key + "_texcoord", 0)),
      "uv_offset": info.get(slot_key + "_uv_offset", [0.0,0.0]),
      "uv_scale": info.get(slot_key + "_uv_scale", [1.0,1.0]),
      "uv_rotation": _gltf_num_or(info.get(slot_key + "_uv_rotation", 0.0), 0.0)
   }
   if(info.contains(slot_key + "_uv_xf0") && info.contains(slot_key + "_uv_xf1")){
      r["uv_xf0"] = int(info.get(slot_key + "_uv_xf0", 0))
      r["uv_xf1"] = int(info.get(slot_key + "_uv_xf1", 0))
   }
   r["sampler"] = info.get(slot_key + "_sampler", 0)
   r
}

fn gltf_material_texture_slot_table(any: gltf_data, int: material_idx): list {
   "Returns every known glTF material texture slot, including not-yet-bound extension maps."
   def info = gltf_material_info(gltf_data, material_idx)
   if(!is_dict(info)){ return [] }
   def names = ["base_color", "normal", "metallic_roughness", "occlusion", "emissive", "clearcoat", "clearcoat_roughness", "clearcoat_normal", "anisotropy", "transmission", "sheen_color", "sheen_roughness", "iridescence", "iridescence_thickness", "thickness", "specular", "specular_color", "diffuse_transmission", "diffuse_transmission_color", "subsurface"]
   def names_n = names.len
   mut out = list(0)
   mut i = 0
   while(i < names_n){
      def k = to_str(names[i])
      if(to_str(info.get(k + "_uri", "")) != ""){ out = out.append(_gltf_slot_rec(info, k)) }
      i += 1
   }
   out
}

fn gltf_extension_payloads(any: gltf_data): list {
   "Collects scene/node/mesh/primitive/material extension dictionaries for offline validation and later renderer wiring."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   mut out = list(0)
   def buckets = ["materials", "nodes", "meshes", "scenes", "animations"]
   def buckets_n = buckets.len
   mut bi = 0
   while(bi < buckets_n){
      def bname = to_str(buckets[bi])
      def arr = g.get(bname, [])
      def arr_n = is_list(arr) ? arr.len : 0
      mut i = 0
      while(i < arr_n){
         def obj = arr[i]
         def ex = is_dict(obj) ? obj.get("extensions", 0) : 0
         if(is_dict(ex)){
            def rec = {"bucket": bname, "index": i, "extensions": ex}
            out = out.append(rec)
         }
         if(bname == "meshes" && is_dict(obj)){
            def prims = obj.get("primitives", [])
            def prims_n = is_list(prims) ? prims.len : 0
            mut pi = 0
            while(pi < prims_n){
               def prim = prims[pi]
               def pex = is_dict(prim) ? prim.get("extensions", 0) : 0
               if(is_dict(pex)){
                  def prec = {"bucket": "primitive", "mesh": i, "primitive": pi, "extensions": pex}
                  out = out.append(prec)
               }
               pi += 1
            }
         }
         i += 1
      }
      bi += 1
   }
   out
}

fn gltf_required_extension_failures(any: gltf_data): list {
   "Returns required extensions that are unknown/todo for hard validation before rendering."
   def rep = gltf_extensions_report(gltf_data)
   if(!is_dict(rep)){ return [] }
   def req = rep.get("required", [])
   def req_n = is_list(req) ? req.len : 0
   mut out = list(0)
   mut i = 0
   while(i < req_n){
      def r = req[i]
      if(is_dict(r)){
         def st = to_str(r.get("status", "unknown"))
         if(st == "unknown" || st == "todo" || st == "fallback-or-todo"){ out = out.append(r) }
      }
      i += 1
   }
   out
}

fn gltf_animation_pointer_bindings(any: gltf_data): list {
   "Parses KHR_animation_pointer channel targets. It returns raw JSON pointers for later material/texture animation wiring."
   def g = gltf_data.get("gltf", 0)
   if(!is_dict(g)){ return [] }
   def anims = g.get("animations", [])
   def anims_n = is_list(anims) ? anims.len : 0
   mut out = list(0)
   mut ai = 0
   while(ai < anims_n){
      def anim = anims[ai]
      def chans = is_dict(anim) ? anim.get("channels", []) : []
      def chans_n = is_list(chans) ? chans.len : 0
      mut ci = 0
      while(ci < chans_n){
         def ch = chans[ci]
         def target = is_dict(ch) ? ch.get("target", 0) : 0
         def ext = _gltf_ext_obj(target, "KHR_animation_pointer")
         if(is_dict(ext)){
            def rec = {
               "animation": ai,
               "channel": ci,
               "pointer": to_str(ext.get("pointer", "")),
               "sampler": int(ch.get("sampler", -1))
            }
            out = out.append(rec)
         }
         ci += 1
      }
      ai += 1
   }
   out
}

fn gltf_scene_feature_summary(any: gltf_data): dict {
   "One-call support summary for conformance test triage. No renderer state is touched."
   _gltf_ensure_caches()
   def g = gltf_data.get("gltf", 0)
   def cache_key = _gltf_cache_key_from_data(gltf_data) + ":feature-summary"
   if(_gltf_scene_feature_summary_cache.contains(cache_key)){
      def cached = _gltf_scene_feature_summary_cache.get(cache_key, 0)
      if(is_dict(cached)){ return cached }
   }
   def mats = gltf_material_infos(gltf_data)
   mut mat_mask = 0
   def mats_n = is_list(mats) ? mats.len : 0
   mut mi = 0
   while(mi < mats_n){
      mat_mask = bor(mat_mask, gltf_material_feature_mask(mats[mi]))
      mi += 1
   }
   def variants = gltf_material_variant_mappings(gltf_data)
   def anim_ptr = gltf_animation_pointer_bindings(gltf_data)
   def payloads = gltf_extension_payloads(gltf_data)
   mut out = {
      "material_count": is_list(mats) ? mats.len : 0,
      "material_feature_mask": mat_mask,
      "variant_mapping_count": is_list(variants) ? variants.len : 0,
      "animation_pointer_count": is_list(anim_ptr) ? anim_ptr.len : 0,
      "extension_payload_count": is_list(payloads) ? payloads.len : 0,
      "extension_report": gltf_extensions_report(gltf_data),
      "required_failures": gltf_required_extension_failures(gltf_data)
   }
   _gltf_scene_feature_summary_cache = cache.cache_put_reset(_gltf_scene_feature_summary_cache,
      cache_key,
      out,
      _GLTF_CACHE_LIMIT_SMALL,
   32)
   out
}
