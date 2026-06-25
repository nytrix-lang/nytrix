;; Keywords: 3d gltf glb parse
module std.math.parse.3d.gltf.shared(GLTF_COMP_NONE, GLTF_COMP_BYTE, GLTF_COMP_UBYTE, GLTF_COMP_SHORT, GLTF_COMP_USHORT, GLTF_COMP_UINT, GLTF_COMP_FLOAT, GLTF_TYPE_SCALAR, GLTF_TYPE_VEC2, GLTF_TYPE_VEC3, GLTF_TYPE_VEC4, GLTF_TYPE_MAT2, GLTF_TYPE_MAT3, GLTF_TYPE_MAT4, _GLB_MAGIC, _GLB_CHUNK_JSON, _GLB_CHUNK_BIN, _gltf_is_json_ws, GltfCompSize, GltfTypeCount, GltfTypeCols, GltfTypeRows, GltfImageMimeSupported, GltfImageExtFromMime, GltfExtensionStatus, _gltf_img_uri_cache, _gltf_material_infos_cache, _gltf_anim_info_cache, _gltf_acc_res_cache, _gltf_anim_sample_cache, _gltf_node_local_mats_cache, _gltf_skin_inv_bind_cache, _gltf_mesh_inv_cache, _gltf_visibility_flag_cache, _gltf_disable_skinning_mode, _gltf_skin_raw_off_mode, _gltf_skin_validate_mode, _gltf_skin_no_mesh_inv_mode, _gltf_skin_transpose_inv_bind_mode, _gltf_skin_invbind_first_mode, _gltf_anim_fast_mode, _gltf_anim_fast_skin_mode, _gltf_ensure_caches, _gltf_env_truthy_flag, _gltf_env_toggle_flag, _gltf_disable_skinning_enabled, _gltf_skin_raw_off_enabled, _gltf_skin_validate_enabled, _gltf_skin_no_mesh_inv_enabled, _gltf_skin_transpose_inv_bind_enabled, _gltf_skin_invbind_first_enabled, _gltf_anim_fast_enabled, _gltf_anim_fast_skin_enabled, _gltf_cache_key_from_g, _gltf_cache_key_from_data, _gltf_stamp_cache_key, _GLTF_CACHE_LIMIT_SMALL, _GLTF_CACHE_LIMIT_MED, _GLTF_CACHE_LIMIT_BIG, GLTF_MODE_TRIANGLES, _GLTF_VTX_STRIDE, _GLTF_VTX_OFF_X, _GLTF_VTX_OFF_Y, _GLTF_VTX_OFF_Z, _GLTF_VTX_OFF_U, _GLTF_VTX_OFF_V, _GLTF_VTX_OFF_C, _GLTF_VTX_OFF_NX, _GLTF_VTX_OFF_NY, _GLTF_VTX_OFF_NZ, _GLTF_VTX_OFF_TX, _GLTF_VTX_OFF_TY, _GLTF_VTX_OFF_TZ, _GLTF_VTX_OFF_TW, _GLTF_VTX_OFF_U2, _GLTF_VTX_OFF_V2, _GLTF_VTX_OFF_TEX, _GLTF_INV_255, _GLTF_INV_127, _GLTF_INV_65535, _GLTF_INV_32767, _gltf_copy_bytes, _gltf_copy_blob_bytes, _gltf_blob_ptr, _gltf_is_path_sep, _gltf_path_dirname, _gltf_url_decode, _gltf_float_bad, _gltf_float3_bad, _gltf_float6_bad, _gltf_list_has_bad_float, _gltf_mat3x4_bad, _gltf_num_or, _gltf_vec3, _gltf_vec4, _gltf_anim_duration_valid, _gltf_doc_has_invalid_node_trs, _gltf_align_up, _gltf_elem_size, _gltf_read_f32_fast, _gltf_read_f32_acc, _gltf_read_index_acc, _gltf_node_local_mats, _gltf_node_visit_key, _gltf_make_material_slot, _gltf_alpha_mode_code, _gltf_prim_mode_expands_to_vertices, _gltf_apply_prim_mode_opts, _gltf_active_scene_idx, _gltf_vertex_color_u32, _gltf_make_uv_xform, _gltf_mat3x4_num, _gltf_mat3_num, _gltf_pack_uv_xform_words_from_values, _gltf_pack_uv_xform_words, _gltf_uv_xf_force_uv0, _gltf_pick_primary_uv_props, _gltf_extension_status, _gltf_meshes, _gltf_transform_aabb, _gltf_comp_size, _gltf_type_count)
use std.core
use std.math.bin
use std.math
use std.math.float (is_nan, is_inf)
use std.core.str as str
use std.core.common as common
use std.core.cache as cache
use std.os.path as ospath
use std.math.parse.data.json
use std.math.crypto.hash as lib_hash
use std.os (file_exists, file_read)
use std.math.parse.3d.gltf.math as gltf_math

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

fn _gltf_is_json_ws(int c) bool {
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
   "EXT_texture_webp" -> "parse+decode"
   "KHR_meshopt_compression", "EXT_meshopt_compression" -> "fallback-or-todo"
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
mut _gltf_disable_skinning_mode = -1
mut _gltf_skin_raw_off_mode = -1
mut _gltf_skin_validate_mode = -1
mut _gltf_skin_no_mesh_inv_mode = -1
mut _gltf_skin_transpose_inv_bind_mode = -1
mut _gltf_skin_invbind_first_mode = -1
mut _gltf_anim_fast_mode = -1
mut _gltf_anim_fast_skin_mode = -1

fn _gltf_ensure_caches() any {
   if !is_dict(_gltf_img_uri_cache) { _gltf_img_uri_cache = dict(128) }
   if !is_dict(_gltf_material_infos_cache) { _gltf_material_infos_cache = dict(32) }
   if !is_dict(_gltf_anim_info_cache) { _gltf_anim_info_cache = dict(64) }
   if !is_dict(_gltf_acc_res_cache) { _gltf_acc_res_cache = dict(256) }
   if !is_dict(_gltf_anim_sample_cache) { _gltf_anim_sample_cache = dict(64) }
   if !is_dict(_gltf_node_local_mats_cache) { _gltf_node_local_mats_cache = dict(32) }
   if !is_dict(_gltf_skin_inv_bind_cache) { _gltf_skin_inv_bind_cache = dict(32) }
   if !is_dict(_gltf_mesh_inv_cache) { _gltf_mesh_inv_cache = dict(32) }
   if !is_dict(_gltf_visibility_flag_cache) { _gltf_visibility_flag_cache = dict(64) }
}

fn _gltf_env_truthy_flag(int flag, str name) int {
   flag != -1 ? flag : (common.env_truthy(name) ? 1 : 0)
}

fn _gltf_env_toggle_flag(int flag, str name, bool default_value=false) int {
   flag != -1 ? flag : (common.env_toggle(name, default_value) ? 1 : 0)
}

fn _gltf_disable_skinning_enabled() bool {
   _gltf_disable_skinning_mode = _gltf_env_truthy_flag(_gltf_disable_skinning_mode, "NY_GLTF_DISABLE_SKINNING")
   _gltf_disable_skinning_mode == 1
}

fn _gltf_skin_raw_off_enabled() bool {
   _gltf_skin_raw_off_mode = _gltf_env_truthy_flag(_gltf_skin_raw_off_mode, "NY_GLTF_SKIN_RAW_OFF")
   _gltf_skin_raw_off_mode == 1
}

fn _gltf_skin_validate_enabled() bool {
   _gltf_skin_validate_mode = _gltf_env_truthy_flag(_gltf_skin_validate_mode, "NY_GLTF_SKIN_VALIDATE")
   _gltf_skin_validate_mode == 1
}

fn _gltf_skin_no_mesh_inv_enabled() bool {
   _gltf_skin_no_mesh_inv_mode = _gltf_env_truthy_flag(_gltf_skin_no_mesh_inv_mode, "NY_GLTF_SKIN_NO_MESH_INV")
   _gltf_skin_no_mesh_inv_mode == 1
}

fn _gltf_skin_transpose_inv_bind_enabled() bool {
   _gltf_skin_transpose_inv_bind_mode = _gltf_env_truthy_flag(_gltf_skin_transpose_inv_bind_mode, "NY_GLTF_SKIN_TRANSPOSE_INV_BIND")
   _gltf_skin_transpose_inv_bind_mode == 1
}

fn _gltf_skin_invbind_first_enabled() bool {
   _gltf_skin_invbind_first_mode = _gltf_env_truthy_flag(_gltf_skin_invbind_first_mode, "NY_GLTF_SKIN_INVBIND_FIRST")
   _gltf_skin_invbind_first_mode == 1
}

fn _gltf_anim_fast_enabled() bool {
   _gltf_anim_fast_mode = _gltf_env_toggle_flag(_gltf_anim_fast_mode, "NY_GLTF_ANIM_FAST", false)
   _gltf_anim_fast_mode == 1
}

fn _gltf_anim_fast_skin_enabled() bool {
   _gltf_anim_fast_skin_mode = _gltf_env_toggle_flag(_gltf_anim_fast_skin_mode, "NY_GLTF_ANIM_FAST_SKIN", true)
   _gltf_anim_fast_skin_mode == 1
}

fn _gltf_cache_key_from_g(any g) str {
   if !is_dict(g) { return "gltf:none" }
   def explicit = to_str(g.get("_ny_cache_key", ""))
   if explicit.len > 0 { return explicit }
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

fn _gltf_cache_key_from_data(any gltf_data) str {
   if !is_dict(gltf_data) { return "gltf:data:none" }
   def source_path = to_str(gltf_data.get("source_path", ""))
   if source_path.len > 0 { return source_path }
   def g = gltf_data.get("gltf", 0)
   _gltf_cache_key_from_g(g)
}

fn _gltf_stamp_cache_key(any gltf_data, any key) bool {
   if !is_dict(gltf_data) { return false }
   def g = gltf_data.get("gltf", 0)
   if is_dict(g) {
      g["_ny_cache_key"] = to_str(key)
      gltf_data["gltf"] = g
   }
   true
}

def _GLTF_CACHE_LIMIT_SMALL = 2048
def _GLTF_CACHE_LIMIT_MED = 8192
def _GLTF_CACHE_LIMIT_BIG = 16384
def GLTF_MODE_TRIANGLES = 4
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

fn _gltf_copy_bytes(any data, int start, int count) str {
   if !is_str(data) || count <= 0 { return "" }
   mut n = count
   if start < 0 { start = 0 }
   def total = data.len
   if start >= total { return "" }
   if start + n > total { n = total - start }
   if n <= 0 { return "" }
   def out = malloc(n + 1)
   if !out { return "" }
   init_str(out, n)
   memcpy(out, data + start, n)
   store8(out, 0, n)
   out
}

fn _gltf_copy_blob_bytes(any data, int start, int count) str {
   if is_str(data) { return _gltf_copy_bytes(data, start, count) }
   if count <= 0 { return "" }
   mut src = 0
   mut total = 0
   if is_dict(data) {
      src = data.get("ptr", 0)
      total = int(data.get("len", 0))
   } else {
      src = data
   }
   if !src { return "" }
   mut off = int(start)
   if off < 0 { off = 0 }
   mut n = int(count)
   if total > 0 {
      if off >= total { return "" }
      if off + n > total { n = total - off }
   }
   if n <= 0 { return "" }
   def out = malloc(n + 1)
   if !out { return "" }
   init_str(out, n)
   memcpy(out, ptr_add(src, off), n)
   store8(out, 0, n)
   out
}

fn _gltf_blob_ptr(any v) any {
   if is_dict(v) { return v.get("ptr", 0) }
   v
}

fn _gltf_is_path_sep(int c) bool { c == 47 || c == 92 }

fn _gltf_path_dirname(any path) str {
   if !is_str(path) { return "." }
   def n = path.len
   if n <= 0 { return "." }
   mut end = n
   while end > 1 && _gltf_is_path_sep(load8(path, end - 1)) { end -= 1 }
   mut j = end - 1
   while j >= 0 && !_gltf_is_path_sep(load8(path, j)) { j -= 1 }
   if j < 0 { return "." }
   case j {
      0 -> _gltf_copy_bytes(path, 0, 1)
      2 if path.len >= 3 && load8(path, 1) == 58 -> _gltf_copy_bytes(path, 0, 3)
      _ -> _gltf_copy_bytes(path, 0, j)
   }
}

fn _gltf_url_decode(any s) str {
   if !is_str(s) { return "" }
   if str.find(s, "%") < 0 && str.find(s, "+") < 0 { return s }
   def n = s.len
   def out = malloc(n + 1)
   if !out { return "" }
   init_str(out, n)
   mut i, o = 0, 0
   while i < n {
      def c = load8(s, i)
      case c {
         37 if i + 2 < n -> {
            def hi = str.hex_val(load8(s, i + 1))
            def lo = str.hex_val(load8(s, i + 2))
            if hi >= 0 && lo >= 0 {
               store8(out, hi * 16 + lo, o)
               o += 1
               i = i + 3
            } else {
               store8(out, 37, o)
               o += 1
               i += 1
            }
         }
         43 -> {
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

fn _gltf_float_bad(any v) bool {
   if !is_int(v) && !is_float(v) { return true }
   if is_nan(v) || is_inf(v) { return true }
   abs(0.0 + v) > 1000000.0
}

fn _gltf_float3_bad(any x, any y, any z) bool {
   _gltf_float_bad(x) || _gltf_float_bad(y) || _gltf_float_bad(z)
}

fn _gltf_float6_bad(any x1, any y1, any z1, any x2, any y2, any z2) bool {
   _gltf_float3_bad(x1, y1, z1) || _gltf_float3_bad(x2, y2, z2)
}

fn _gltf_list_has_bad_float(any xs, int limit=-1) bool {
   if !is_list(xs) { return false }
   def n = (limit >= 0 && limit < xs.len) ? limit : xs.len
   mut i = 0
   while i < n {
      if _gltf_float_bad(xs[i]) { return true }
      i += 1
   }
   false
}

fn _gltf_mat3x4_bad(any xs) bool {
   _gltf_list_has_bad_float(xs, 12)
}

fn _gltf_num_or(any v, f64 d) f64 {
   if !is_int(v) && !is_float(v) { return d }
   if is_nan(v) || is_inf(v) { return d }
   def out = 0.0 + v
   if abs(out) > 1000000.0 { return d }
   out
}

fn _gltf_vec3(any v, f64 x, f64 y, f64 z) list {
   if !is_list(v) { return [float(x), float(y), float(z)] }
   [
      _gltf_num_or(v.get(0, x), x),
      _gltf_num_or(v.get(1, y), y),
      _gltf_num_or(v.get(2, z), z)
   ]
}

fn _gltf_vec4(any v, f64 x, f64 y, f64 z, f64 w) list {
   if !is_list(v) { return [float(x), float(y), float(z), float(w)] }
   [
      _gltf_num_or(v.get(0, x), x),
      _gltf_num_or(v.get(1, y), y),
      _gltf_num_or(v.get(2, z), z),
      _gltf_num_or(v.get(3, w), w)
   ]
}

fn _gltf_anim_duration_valid(any v) bool {
   if !is_int(v) && !is_float(v) { return false }
   if is_nan(v) || is_inf(v) { return false }
   def out = 0.0 + v
   out > 0.0001 && out < 3600.0
}

fn _gltf_doc_has_invalid_node_trs(any doc) bool {
   mut g0 = 0
   if is_dict(doc) { g0 = doc.get("gltf", 0) }
   def g = is_dict(g0) ? g0 : doc
   if !is_dict(g) { return false }
   def nodes = g.get("nodes", 0)
   if !is_list(nodes) { return false }
   def nodes_n = nodes.len
   mut ni = 0
   while ni < nodes_n {
      def node = nodes[ni]
      if is_dict(node) {
         def mat = node.get("matrix", 0)
         if is_list(mat) && mat.len >= 16 {
            if _gltf_list_has_bad_float(mat, 16) { return true }
         } else {
            def tr = node.get("translation", 0)
            def ro = node.get("rotation", 0)
            def sc = node.get("scale", 0)
            if _gltf_list_has_bad_float(tr) { return true }
            if _gltf_list_has_bad_float(ro) { return true }
            if _gltf_list_has_bad_float(sc) { return true }
         }
      }
      ni += 1
   }
   false
}

fn _gltf_align_up(int v, int a) int {
   if a <= 1 { return v }
   ((v + a - 1) / a) * a
}

fn _gltf_elem_size(int comp_size, str type_str) int {
   def cols = _gltf_type_cols(type_str)
   def rows = _gltf_type_rows(type_str)
   if cols <= 0 || rows <= 0 { return 0 }
   if cols == 1 { return comp_size * rows }
   cols * _gltf_align_up(rows * comp_size, 4)
}

fn _gltf_read_f32_fast(any data, int offset, int comp_type) f64 {
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

fn _gltf_read_f32_acc(any data_ptr, int offset, int comp_type, bool normalized=false) f64 {
   case comp_type {
      GLTF_COMP_FLOAT -> f32le(data_ptr, offset)
      GLTF_COMP_UBYTE -> {
         def raw = load8(data_ptr, offset)
         normalized ? raw * _GLTF_INV_255 : raw * 1.0
      }
      GLTF_COMP_BYTE -> {
         def raw = load8(data_ptr, offset)
         def sval = raw >= 128 ? raw - 256 : raw
         if normalized {
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
         if normalized {
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

fn _gltf_read_index_acc(any data_ptr, int offset, int comp_type) int {
   case comp_type {
      GLTF_COMP_USHORT -> u16le(data_ptr, offset)
      GLTF_COMP_UINT -> u32le(data_ptr, offset)
      GLTF_COMP_UBYTE -> load8(data_ptr, offset)
      _ -> 0
   }
}

fn _gltf_node_local_mats(any g) list {
   _gltf_ensure_caches()
   if !is_dict(g) { return [] }
   def nodes = g.get("nodes", 0)
   if !is_list(nodes) { return [] }
   def nodes_n = nodes.len
   def key = _gltf_cache_key_from_g(g) + ":nodes:" + to_str(nodes_n)
   def cached = _gltf_node_local_mats_cache.get(key, 0)
   if is_list(cached) && cached.len == nodes_n { return cached }
   mut mats = []
   mut i = 0
   while i < nodes_n {
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

fn _gltf_node_visit_key(int node_idx) str {
   "Dict key used only for recursion guards. Keep it disjoint from integer node indices."
   "__visit_node_" + to_str(int(node_idx))
}

fn _gltf_make_material_slot(int tex_id, int uv_set, int xf0, int xf1) dict { return {"tex_id": int(tex_id), "uv_set": int(uv_set), "xf0": int(xf0), "xf1": int(xf1)} }

fn _gltf_alpha_mode_code(str alpha_mode) int {
   def m = str.upper(str.strip(to_str(alpha_mode)))
   case m {
      "MASK" -> 1
      "BLEND" -> 2
      _ -> 0
   }
}

fn _gltf_prim_mode_expands_to_vertices(int prim_mode) bool {
   case prim_mode {
      0, 1, 2, 3 -> true
      _ -> false
   }
}

fn _gltf_apply_prim_mode_opts(dict mesh_opts, int prim_mode, bool has_normals) dict {
   case prim_mode {
      0 -> {
         mesh_opts["is_points"] = true
         if !has_normals { mesh_opts["unlit"] = true }
      }
      1, 2, 3 -> {
         mesh_opts["is_lines"] = true
         if !has_normals { mesh_opts["unlit"] = true }
      }
      _ -> {}
   }
   mesh_opts
}

fn _gltf_active_scene_idx(any g, any gltf_data=0) int {
   if !is_dict(g) { return 0 }
   if is_dict(gltf_data) && gltf_data.contains("scene_index") { return int(gltf_data.get("scene_index", 0)) }
   int(g.get("scene", 0))
}

fn _gltf_vertex_color_u32(any color_res, int vi, int fallback_u32) int {
   if !is_dict(color_res) { return fallback_u32 }
   def ptr = color_res.get("ptr", 0)
   def cnt = int(color_res.get("count", 0))
   if !ptr || vi < 0 || vi >= cnt { return fallback_u32 }
   def stride = int(color_res.get("stride", 0))
   def comp = int(color_res.get("comp", 0))
   def cs = _gltf_comp_size(comp)
   if cs <= 0 || stride <= 0 { return fallback_u32 }
   def type_count = int(color_res.get("type_count", 4))
   if type_count != 3 && type_count != 4 { return fallback_u32 }
   def norm = color_res.get("normalized", false) || comp != GLTF_COMP_FLOAT
   def off = vi * stride
   if comp == GLTF_COMP_UBYTE && norm {
      def ir, ig = load8(ptr, off + 0), load8(ptr, off + 1)
      def ib, ia = load8(ptr, off + 2), type_count == 4 ? load8(ptr, off + 3) : 255
      return ir | (ig << 8) | (ib << 16) | (ia << 24)
   }
   mut r = clamp01(_gltf_read_f32_acc(ptr, off + cs * 0, comp, norm))
   mut g = clamp01(_gltf_read_f32_acc(ptr, off + cs * 1, comp, norm))
   mut b = clamp01(_gltf_read_f32_acc(ptr, off + cs * 2, comp, norm))
   mut a = 1.0
   if type_count == 4 { a = clamp01(_gltf_read_f32_acc(ptr, off + cs * 3, comp, norm)) }
   def ir, ig = band(int(r * 255.0 + 0.5), 255), band(int(g * 255.0 + 0.5), 255)
   def ib, ia = band(int(b * 255.0 + 0.5), 255), band(int(a * 255.0 + 0.5), 255)
   ir | (ig << 8) | (ib << 16) | (ia << 24)
}

fn _gltf_make_uv_xform(any info, str prefix) dict {
   {
      "offset": is_dict(info) ? info.get(prefix + "_uv_offset", [0.0, 0.0]) : [0.0, 0.0],
      "scale": is_dict(info) ? info.get(prefix + "_uv_scale", [1.0, 1.0]) : [1.0, 1.0],
      "rotation": is_dict(info) ? _gltf_num_or(info.get(prefix + "_uv_rotation", 0.0), 0.0) : 0.0
   }
}

fn _gltf_mat3x4_num(any m) list {
   [
      _gltf_num_or(m.get(0, 1.0), 1.0), _gltf_num_or(m.get(4, 0.0), 0.0), _gltf_num_or(m.get(8, 0.0), 0.0), _gltf_num_or(m.get(12, 0.0), 0.0),
      _gltf_num_or(m.get(1, 0.0), 0.0), _gltf_num_or(m.get(5, 1.0), 1.0), _gltf_num_or(m.get(9, 0.0), 0.0), _gltf_num_or(m.get(13, 0.0), 0.0),
      _gltf_num_or(m.get(2, 0.0), 0.0), _gltf_num_or(m.get(6, 0.0), 0.0), _gltf_num_or(m.get(10, 1.0), 1.0), _gltf_num_or(m.get(14, 0.0), 0.0)
   ]
}

fn _gltf_mat3_num(any m) list {
   [
      float(m.get(0, 1.0)), float(m.get(4, 0.0)), float(m.get(8, 0.0)),
      float(m.get(1, 0.0)), float(m.get(5, 1.0)), float(m.get(9, 0.0)),
      float(m.get(2, 0.0)), float(m.get(6, 0.0)), float(m.get(10, 1.0))
   ]
}

fn _gltf_pack_uv_xform_words_from_values(any uv_off, any uv_scl, any uv_rot, any uv_set_raw) list {
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
   if !has_non_identity_xform {
      mut word1_id = 0
      if uv_set != 0 { word1_id = (1 << 30) }
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
   if uv_set != 0 { uv_set_bit = 1 }
   def word1 = bor(bor(scl_x_q, bshl(scl_y_q, 11)), bor(bshl(rot_q, 22), bshl(uv_set_bit, 30)))
   [word0, word1]
}

fn _gltf_pack_uv_xform_words(any info, str prefix) list {
   "Packs glTF texture-transform state into two u32 words for the shader.
   word0: offset.x/offet.y as 2x16-bit signed-fixed over [-8, 8]
   word1: scale.x/scale.y as 2x11-bit fixed over [-32, 32], rotation as 8-bit fixed over [-PI, PI], uvSet bit at 30."
   if is_dict(info) && info.contains(prefix + "_uv_xf0") && info.contains(prefix + "_uv_xf1") { return [int(info.get(prefix + "_uv_xf0", 0)), int(info.get(prefix + "_uv_xf1", 0))] }
   def uv_off = is_dict(info) ? info.get(prefix + "_uv_offset", [0.0, 0.0]) : [0.0, 0.0]
   def uv_scl = is_dict(info) ? info.get(prefix + "_uv_scale", [1.0, 1.0]) : [1.0, 1.0]
   def uv_rot = is_dict(info) ? _gltf_num_or(info.get(prefix + "_uv_rotation", 0.0), 0.0) : 0.0
   def uv_set = is_dict(info) ? int(info.get(prefix + "_texcoord", 0)) : 0
   _gltf_pack_uv_xform_words_from_values(uv_off, uv_scl, uv_rot, uv_set)
}

fn _gltf_uv_xf_force_uv0(any xf0, any xf1) list { [int(xf0), band(int(xf1), 0xbfffffff)] }

fn _gltf_pick_primary_uv_props(any info, any texrec=0) list {
   "Chooses the best available texcoord set/xform for the currently-rendered material path.
   Preference order follows visible shading inputs under the current renderer."
   mut prefix = "base_color"
   def has_base = (is_dict(texrec) && (int(texrec.get("base", -1)) >= 0 || int(texrec.get("base_color", -1)) >= 0)) || (is_dict(info) && to_str(info.get("base_color_uri", "")) != "")
   def has_emissive = (is_dict(texrec) && int(texrec.get("emissive", -1)) >= 0) || (is_dict(info) && to_str(info.get("emissive_uri", "")) != "")
   def has_normal = (is_dict(texrec) && int(texrec.get("normal", -1)) >= 0) || (is_dict(info) && to_str(info.get("normal_uri", "")) != "")
   def has_mr = (is_dict(texrec) && int(texrec.get("metallic_roughness", -1)) >= 0) || (is_dict(info) && to_str(info.get("metallic_roughness_uri", "")) != "")
   def has_occ = (is_dict(texrec) && int(texrec.get("occlusion", -1)) >= 0) || (is_dict(info) && to_str(info.get("occlusion_uri", "")) != "")
   if has_base { prefix = "base_color" }
   elif has_emissive { prefix = "emissive" }
   elif has_normal { prefix = "normal" }
   elif has_mr { prefix = "metallic_roughness" }
   elif has_occ { prefix = "occlusion" }
   def uv_set = is_dict(info) ? int(info.get(prefix + "_texcoord", 0)) : 0
   [uv_set, _gltf_make_uv_xform(info, prefix)]
}

fn _gltf_extension_status(str name) str { comptime match GltfExtensionStatus(name, "unknown") }

fn _gltf_meshes(any gltf_data) any {
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def meshes = g.get("meshes")
   is_list(meshes) ? meshes : 0
}

fn _gltf_transform_aabb(any minv, any maxv, any m) list {
   def x1 = _gltf_num_or(minv.get(0, 0.0), 0.0) def y1 = _gltf_num_or(minv.get(1, 0.0), 0.0) def z1 = _gltf_num_or(minv.get(2, 0.0), 0.0)
   def x2 = _gltf_num_or(maxv.get(0, 0.0), 0.0) def y2 = _gltf_num_or(maxv.get(1, 0.0), 0.0) def z2 = _gltf_num_or(maxv.get(2, 0.0), 0.0)
   if _gltf_float6_bad(x1, y1, z1, x2, y2, z2) { return [] }
   if x1 > x2 || y1 > y2 || z1 > z2 { return [] }
   def mm = _gltf_mat3x4_num(m)
   def m00, m01 = mm.get(0), mm.get(1)
   def m02, m03 = mm.get(2), mm.get(3)
   def m10, m11 = mm.get(4), mm.get(5)
   def m12, m13 = mm.get(6), mm.get(7)
   def m20, m21 = mm.get(8), mm.get(9)
   def m22, m23 = mm.get(10), mm.get(11)
   if _gltf_mat3x4_bad(mm) { return [] }
   mut wmin_x, wmin_y, wmin_z = 1e9, 1e9, 1e9
   mut wmax_x, wmax_y, wmax_z = -1e9, -1e9, -1e9
   mut used = 0
   mut ci = 0
   while ci < 8 {
      def px, py = (band(ci, 1) != 0) ? x2 : x1, (band(ci, 2) != 0) ? y2 : y1
      def pz = (band(ci, 4) != 0) ? z2 : z1
      def wx = 0.0 + (m00 * px + m01 * py + m02 * pz + m03)
      def wy = 0.0 + (m10 * px + m11 * py + m12 * pz + m13)
      def wz = 0.0 + (m20 * px + m21 * py + m22 * pz + m23)
      if !_gltf_float3_bad(wx, wy, wz) {
         if wx < wmin_x { wmin_x = 0.0 + wx } if wx > wmax_x { wmax_x = 0.0 + wx }
         if wy < wmin_y { wmin_y = 0.0 + wy } if wy > wmax_y { wmax_y = 0.0 + wy }
         if wz < wmin_z { wmin_z = 0.0 + wz } if wz > wmax_z { wmax_z = 0.0 + wz }
         used += 1
      }
      ci += 1
   }
   if used <= 0 || wmin_x > wmax_x || wmin_y > wmax_y || wmin_z > wmax_z { return [] }
   return [[wmin_x, wmin_y, wmin_z], [wmax_x, wmax_y, wmax_z]]
}

fn _gltf_anim_override_for_node(any overrides, int node_idx) any {
   if !is_dict(overrides) { return 0 }
   def ov_num = overrides.get(int(node_idx), 0)
   if is_dict(ov_num) { return ov_num }
   def ov_str = overrides.get(to_str(node_idx), 0)
   if is_dict(ov_str) { return ov_str }
   def ovs = overrides.get("__nodes", 0)
   if is_list(ovs) {
      def ovs_n = ovs.len
      mut oi = 0
      while oi < ovs_n {
         def rec = ovs[oi]
         if is_dict(rec) && int(rec.get("node", -1)) == int(node_idx) { return rec }
         oi += 1
      }
   }
   0
}

fn _gltf_pointer_node_visibility_idx(any pointer) int {
   def p = to_str(pointer)
   if p.len == 0 { return -1 }
   def parts = str.split(p, "/")
   if !is_list(parts) || parts.len < 6 { return -1 }
   if to_str(parts[1]) != "nodes" { return -1 }
   def node_txt = to_str(parts[2])
   if node_txt.len == 0 { return -1 }
   mut i = 0
   mut out = 0
   while i < node_txt.len {
      def ch = load8(node_txt, i)
      if ch < 48 || ch > 57 { return -1 }
      out = out * 10 + (ch - 48)
      i += 1
   }
   if to_str(parts[3]) != "extensions" { return -1 }
   if to_str(parts[4]) != "KHR_node_visibility" { return -1 }
   if to_str(parts[5]) != "visible" { return -1 }
   out
}

fn _gltf_pointer_material_target(any pointer) any {
   def p = to_str(pointer)
   if str.find(p, "/materials/") != 0 { return 0 }
   def parts = str.split(p, "/")
   if parts.len < 4 { return 0 }
   if to_str(parts[1]) != "materials" { return 0 }
   def mat_idx = int(parts[2])
   if mat_idx < 0 { return 0 }
   if parts.len >= 5
   && to_str(parts[3]) == "pbrMetallicRoughness"
   && to_str(parts[4]) == "baseColorFactor"{
      return {"material": mat_idx, "kind": "baseColorFactor"}
   }
   if parts.len >= 5
   && to_str(parts[3]) == "pbrMetallicRoughness"
   && to_str(parts[4]) == "metallicFactor"{
      return {"material": mat_idx, "kind": "metallicFactor"}
   }
   if parts.len >= 5
   && to_str(parts[3]) == "pbrMetallicRoughness"
   && to_str(parts[4]) == "roughnessFactor"{
      return {"material": mat_idx, "kind": "roughnessFactor"}
   }
   if to_str(parts[3]) == "emissiveFactor" { return {"material": mat_idx, "kind": "emissiveFactor"} }
   if to_str(parts[3]) == "alphaCutoff" { return {"material": mat_idx, "kind": "alphaCutoff"} }
   mut ktt_idx = -1
   def parts_n = parts.len
   mut pi = 0
   while pi < parts_n {
      if to_str(parts[pi]) == "KHR_texture_transform" { ktt_idx = pi }
      pi += 1
   }
   if ktt_idx >= 5 && ktt_idx + 1 < parts_n {
      def slot = to_str(parts[ktt_idx - 2])
      def op = to_str(parts[ktt_idx + 1])
      if slot != "" && (op == "offset" || op == "scale" || op == "rotation" || op == "texCoord") {
         def kind = (op == "offset") ? "uvOffset" : ((op == "scale") ? "uvScale" : ((op == "rotation") ? "uvRotation" : "uvTexCoord"))
         return {"material": mat_idx, "kind": kind, "slot": slot}
      }
   }
   0
}
