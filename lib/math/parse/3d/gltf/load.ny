;; Keywords: 3d gltf glb parse
;; Submodule: load
module std.math.parse.3d.gltf.load(_gltf_stage_binary_blob, _gltf_data_uri_decode, _gltf_load_buffer_uri, _gltf_parse_glb, _gltf_strip_top_level_key, _gltf_strip_asset_field, _gltf_parse_gltf_with_fallbacks, gltf_free_data, _gltf_sparse_accessor_meta, _gltf_sparse_buffer_ptr, _gltf_sparse_index_value, _gltf_sparse_materialize, _gltf_resolve_accessor_data, _gltf_release_accessor_data, _gltf_primary_data_ptr, load_gltf, load_gltf_file, parse_gltf_str)
use std.core
use std.math.bin
use std.math
use std.os (file_exists, file_read, file_write)
use std.os.interact
use std.math.parse.data.json
use std.math.float (is_nan, is_inf)
use std.math.crypto.hash as lib_hash
use std.os.path as ospath
use std.core.str as str
use std.math.crypto.encoding.base as str_base
use std.core.common as common
use std.core.cache as cache
use std.math.parse.3d.gltf.math as gltf_math
use std.math.parse.3d.gltf.shared as shr

fn _gltf_stage_binary_blob(any raw) any {
   if !raw { return 0 }
   mut raw_len = 0
   if is_dict(raw) {
      raw_len = int(raw.get("len", 0))
      raw = raw.get("ptr", 0)
   } elif is_str(raw) {
      raw_len = raw.len
   }
   if !raw || raw_len <= 0 { return 0 }
   def buf = malloc(raw_len)
   if !buf { return 0 }
   memcpy(buf, raw, raw_len)
   return {"ptr": buf, "len": raw_len, "kind": "buf"}
}

fn _gltf_data_uri_decode(any uri) any {
   if !is_str(uri) || uri.len == 0 { return 0 }
   def comma = str.find(uri, ",")
   if comma < 0 { return 0 }
   def payload = shr._gltf_copy_bytes(uri, comma + 1, uri.len - (comma + 1))
   if str.find(uri, ";base64,") >= 0 { return _gltf_stage_binary_blob(str_base.decode64(payload)) }
   _gltf_stage_binary_blob(payload)
}

fn _gltf_load_buffer_uri(any uri, str base_path="") any {
   if !is_str(uri) || uri.len == 0 { return 0 }
   if str.find(uri, "data:") == 0 { return _gltf_data_uri_decode(uri) }
   def full_path = ospath.join(base_path, shr._gltf_url_decode(uri))
   def res = file_read(full_path)
   if is_err(res) { return 0 }
   def raw = unwrap(res)
   _gltf_stage_binary_blob(raw)
}

fn _gltf_parse_glb(any data, str base_path="") dict {
   if !is_str(data) || data.len < 20 { return {"error": "Invalid GLB payload"} }
   if u32le(data, 0) != _GLB_MAGIC { return {"error": "Invalid GLB magic"} }
   mut off = 12
   mut json_chunk = 0
   mut bin_chunk = 0
   while off + 8 <= data.len {
      def chunk_len = u32le(data, off)
      def chunk_type = u32le(data, off + 4)
      def chunk_data_off = off + 8
      if chunk_data_off + chunk_len > data.len { break }
      if chunk_type == _GLB_CHUNK_JSON && !json_chunk { json_chunk = shr._gltf_copy_bytes(data, chunk_data_off, chunk_len) } elif chunk_type == _GLB_CHUNK_BIN && !bin_chunk { bin_chunk = shr._gltf_copy_bytes(data, chunk_data_off, chunk_len) }
      off = chunk_data_off + chunk_len
   }
   if !is_str(json_chunk) || json_chunk.len == 0 { return {"error": "Missing GLB JSON chunk"} }
   mut json_len = json_chunk.len
   while json_len > 0 {
      if shr._gltf_is_json_ws(load8(json_chunk, json_len - 1)) { json_len -= 1 }
      else { break }
   }
   def json_clean = (json_len == json_chunk.len) ? json_chunk : shr._gltf_copy_bytes(json_chunk, 0, json_len)
   mut bin_data = _gltf_stage_binary_blob(bin_chunk)
   _gltf_parse_gltf_with_fallbacks(json_clean, base_path, bin_data)
}

fn _gltf_strip_top_level_key(any json_str, any key_name) any {
   if !is_str(json_str) { return json_str }
   def key_pat = "\"" + to_str(key_name) + "\""
   def pos = str.find(json_str, key_pat)
   if pos < 0 { return json_str }
   mut colon = pos + key_pat.len
   while colon < json_str.len && load8(json_str, colon) != 58 { colon += 1 }
   if colon >= json_str.len { return json_str }
   mut end = colon + 1
   mut depth = 0
   mut in_str = false
   mut esc = false
   while end < json_str.len {
      def c = load8(json_str, end)
      if in_str {
         if esc { esc = false }
         elif c == 92 { esc = true }
         elif c == 34 { in_str = false }
      } else {
         if c == 34 { in_str = true }
         elif c == 123 || c == 91 { depth += 1 }
         elif c == 125 || c == 93 {
            depth -= 1
            if depth <= 0 { end += 1 break }
         }
      }
      end += 1
   }
   mut start = pos
   while start > 0 {
      if shr._gltf_is_json_ws(load8(json_str, start - 1)) { start -= 1 }
      else { break }
   }
   if start > 0 && load8(json_str, start - 1) == 44 { start -= 1 }
   mut remove_end = end
   while remove_end < json_str.len {
      if shr._gltf_is_json_ws(load8(json_str, remove_end)) { remove_end += 1 }
      else { break }
   }
   if remove_end < json_str.len && load8(json_str, remove_end) == 44 { remove_end += 1 }
   def left = start > 0 ? shr._gltf_copy_bytes(json_str, 0, start) : ""
   def right = remove_end < json_str.len ? shr._gltf_copy_bytes(json_str, remove_end, json_str.len - remove_end) : ""
   left + right
}

fn _gltf_strip_asset_field(any json_str, any field_name) any {
   if !is_str(json_str) { return json_str }
   def asset_pat = "\"asset\""
   def pos = str.find(json_str, asset_pat)
   if pos < 0 { return json_str }
   mut colon = pos + asset_pat.len
   while colon < json_str.len && load8(json_str, colon) != 58 { colon += 1 }
   if colon >= json_str.len { return json_str }
   mut start = colon + 1
   while start < json_str.len {
      if shr._gltf_is_json_ws(load8(json_str, start)) { start += 1 }
      else { break }
   }
   if start >= json_str.len || load8(json_str, start) != 123 { return json_str }
   mut end = start + 1
   mut depth = 1
   mut in_str = false
   mut esc = false
   while end < json_str.len {
      def c = load8(json_str, end)
      if in_str {
         if esc { esc = false }
         elif c == 92 { esc = true }
         elif c == 34 { in_str = false }
      } else {
         if c == 34 { in_str = true }
         elif c == 123 { depth += 1 }
         elif c == 125 {
            depth -= 1
            if depth <= 0 { end += 1 break }
         }
      }
      end += 1
   }
   if end <= start { return json_str }
   def asset_json = shr._gltf_copy_bytes(json_str, start, end - start)
   def stripped_asset = _gltf_strip_top_level_key(asset_json, field_name)
   if stripped_asset == asset_json { return json_str }
   def left = start > 0 ? shr._gltf_copy_bytes(json_str, 0, start) : ""
   def right = end < json_str.len ? shr._gltf_copy_bytes(json_str, end, json_str.len - end) : ""
   left + stripped_asset + right
}

fn _gltf_parse_gltf_with_fallbacks(any json_str, str base_path="", any binary_override=0) any {
   def direct = parse_gltf_str(json_str, base_path, binary_override)
   if is_dict(direct) && !direct.contains("error") && !shr._gltf_doc_has_invalid_node_trs(direct) { return direct }
   if
   !is_dict(direct) ||
   !str.startswith(to_str(direct.get("error", "")), "Failed to parse glTF JSON")
   {
      return direct
   }
   mut cur = json_str
   def retry_keys = ["copyright", "generator", "name", "extras"]
   def retry_keys_n = retry_keys.len
   mut i = 0
   while i < retry_keys_n {
      cur = _gltf_strip_asset_field(cur, to_str(retry_keys[i]))
      def retry = parse_gltf_str(cur, base_path, binary_override)
      if is_dict(retry) && !retry.contains("error") { return retry }
      i += 1
   }
   def retry_top_keys = ["extensions", "extensionsUsed", "extensionsRequired"]
   def retry_top_keys_n = retry_top_keys.len
   i = 0
   while i < retry_top_keys_n {
      cur = _gltf_strip_top_level_key(cur, to_str(retry_top_keys[i]))
      def retry = parse_gltf_str(cur, base_path, binary_override)
      if is_dict(retry) && !retry.contains("error") { return retry }
      i += 1
   }
   cur = _gltf_strip_asset_field(cur, "extensions")
   def retry_asset_ext = parse_gltf_str(cur, base_path, binary_override)
   if is_dict(retry_asset_ext) && !retry_asset_ext.contains("error") { return retry_asset_ext }
   direct
}

fn gltf_free_data(any gltf_data) bool {
   "Frees binary data buffers held by a gltf_data dict. Call after GPU mesh upload."
   shr._gltf_ensure_caches()
   if !is_dict(gltf_data) { return false }
   _gltf_acc_res_cache = dict(256)
   _gltf_anim_sample_cache = dict(64)
   def buffer_data = gltf_data.get("buffer_data", 0)
   if is_list(buffer_data) {
      def buffer_data_n = buffer_data.len
      mut bi = 0
      while bi < buffer_data_n {
         def b = buffer_data[bi]
         if is_dict(b) {
            def ptr = b.get("ptr", 0)
            def kind = to_str(b.get("kind", ""))
            if (kind == "str" || kind == "buf") && ptr { free(ptr) }
         } elif is_str(b) {
            free(b)
         }
         bi += 1
      }
      return true
   }
   def binary_data = gltf_data.get("binary_data", 0)
   if is_dict(binary_data) {
      def ptr = binary_data.get("ptr", 0)
      def kind = to_str(binary_data.get("kind", ""))
      if (kind == "str" || kind == "buf") && ptr { free(ptr) }
   } elif is_str(binary_data) {
      free(binary_data)
   }
   true
}

fn parse_gltf_str(any json_str, str base_path="", any binary_override=0) dict {
   "Parses parse gltf str."
   mut gltf = json_decode(json_str)
   if !is_dict(gltf) { return {"error": "Failed to parse glTF JSON"} }
   mut errors = []
   mut warnings = []
   def asset = gltf.get("asset", 0)
   if !is_dict(asset) { errors = errors.append("missing asset object") } else {
      def ver = to_str(asset.get("version", ""))
      if ver.len == 0 { errors = errors.append("asset.version is required") }
      def minv = to_str(asset.get("minVersion", ""))
      if minv.len > 0 && ver.len > 0 && minv > ver { errors = errors.append("asset.minVersion > asset.version") }
   }
   def used = gltf.get("extensionsUsed", [])
   def req = gltf.get("extensionsRequired", [])
   if is_list(req) {
      def req_n = req.len
      def used_n = is_list(used) ? used.len : 0
      mut ri = 0
      while ri < req_n {
         def name = to_str(req[ri])
         mut found = false
         mut ui = 0
         while ui < used_n {
            if to_str(used[ui]) == name { found = true }
            ui += 1
         }
         if !found { errors = errors.append("extensionsRequired contains " + name + " not present in extensionsUsed") }
         def st = shr._gltf_extension_status(name)
         if st == "unknown" || st == "todo" || st == "fallback-or-todo" { errors = errors.append("extensionsRequired contains unsupported " + name + " (" + st + ")") }
         ri += 1
      }
   }
   def buffers = gltf.get("buffers")
   mut binary_data = binary_override
   mut buffer_data = []
   if is_list(buffers) && buffers.len > 0 {
      def buffers_n = buffers.len
      mut bi = 0
      while bi < buffers_n {
         def buf = buffers[bi]
         def uri = buf.get("uri", "")
         mut loaded = 0
         if bi == 0 && binary_override { loaded = binary_override } elif is_str(uri) && uri.len > 0 { loaded = _gltf_load_buffer_uri(uri, base_path) }
         buffer_data = buffer_data.append(loaded)
         bi += 1
      }
      if buffer_data.len > 0 { binary_data = buffer_data[0] }
   }
   gltf["_ny_buffer_data"] = buffer_data
   gltf["_ny_base_path"] = base_path
   gltf["_ny_cache_key"] = shr._gltf_cache_key_from_g(gltf) + "|json=" + to_str(lib_hash.xxh32(json_str))
   def images = gltf.get("images", [])
   if is_list(images) {
      def images_n = images.len
      mut ii = 0
      while ii < images_n {
         def img = images[ii]
         if is_dict(img) {
            def uri = img.get("uri", "")
            def has_uri = is_str(uri) && uri.len > 0
            def has_bv = int(img.get("bufferView", -1)) >= 0
            def mime = to_str(img.get("mimeType", ""))
            if has_uri && has_bv { errors = errors.append("image[" + to_str(ii) + "] has both uri and bufferView") }
            if has_bv && mime.len == 0 { errors = errors.append("image[" + to_str(ii) + "] bufferView requires mimeType") }
            if has_bv && !shr._gltf_image_mime_supported(mime) { errors = errors.append("image[" + to_str(ii) + "] unsupported mimeType " + mime) }
         }
         ii += 1
      }
   }
   mut out = {
      "gltf": gltf, "binary_data": binary_data, "buffer_data": buffer_data,
      "base_path": base_path, "errors": errors, "warnings": warnings
   }
   if errors.len > 0 { out["error"] = "Invalid glTF: " + str.join(errors, "; ") }
   out
}

fn load_gltf_file(str path) any {
   "Loads load gltf file."
   def dir = shr._gltf_path_dirname(path)
   def res = file_read(path)
   if is_err(res) { return {"error": "Failed to read glTF file: " + path} }
   def raw = unwrap(res)
   if is_str(raw) && raw.len >= 4 && u32le(raw, 0) == _GLB_MAGIC {
      mut glb = _gltf_parse_glb(raw, dir)
      if is_dict(glb) {
         glb["source_path"] = path
         shr._gltf_stamp_cache_key(glb, path)
         glb["anim_duration_hint"] = 0.0
      }
      return glb
   }
   mut parsed = _gltf_parse_gltf_with_fallbacks(raw, dir)
   if is_dict(parsed) {
      parsed["source_path"] = path
      shr._gltf_stamp_cache_key(parsed, path)
      parsed["anim_duration_hint"] = 0.0
   }
   parsed
}

fn _gltf_sparse_accessor_meta(any sparse, any bv_list) list {
   if !is_dict(sparse) || !is_list(bv_list) { return [false] }
   def s_count = int(sparse.get("count", 0))
   def s_indices = sparse.get("indices", 0)
   def s_values = sparse.get("values", 0)
   if s_count < 0 || !is_dict(s_indices) || !is_dict(s_values) { return [false] }
   def sbv_idx = int(s_indices.get("bufferView", -1))
   def vbv_idx = int(s_values.get("bufferView", -1))
   if sbv_idx < 0 || sbv_idx >= bv_list.len || vbv_idx < 0 || vbv_idx >= bv_list.len { return [false] }
   def sbv = bv_list.get(sbv_idx)
   def vbv = bv_list.get(vbv_idx)
   def idx_comp = int(s_indices.get("componentType", 0))
   if idx_comp != 5121 && idx_comp != 5123 && idx_comp != 5125 { return [false] }
   [
      true,
      s_count,
      int(sbv.get("byteOffset", 0)) + int(s_indices.get("byteOffset", 0)),
      idx_comp,
      int(vbv.get("byteOffset", 0)) + int(s_values.get("byteOffset", 0)),
      int(sbv.get("buffer", 0)),
      int(vbv.get("buffer", 0))
   ]
}

fn _gltf_sparse_buffer_ptr(any buffers_data, any fallback, int buf_idx) any {
   if is_list(buffers_data) && buf_idx >= 0 && buf_idx < buffers_data.len {
      def blob = buffers_data.get(buf_idx, 0)
      def p = shr._gltf_blob_ptr(blob)
      if p { return p }
   }
   fallback
}

fn _gltf_sparse_index_value(any data, int off, int comp, int i) int {
   case comp {
      5121 -> load8(data, off + i)
      5123 -> u16le(data, off + i * 2)
      5125 -> u32le(data, off + i * 4)
      _ -> -1
   }
}

fn _gltf_sparse_materialize(
   any sparse, any bv_list, any buffers_data, any data,
   int count, int elem_size, any base_ptr, int base_stride
) any {
   def meta = _gltf_sparse_accessor_meta(sparse, bv_list)
   if !is_list(meta) || !bool(meta.get(0, false)) { return 0 }
   def sparse_count = int(meta.get(1, 0))
   def idx_off = int(meta.get(2, 0))
   def idx_comp = int(meta.get(3, 0))
   def val_off = int(meta.get(4, 0))
   def idx_data = _gltf_sparse_buffer_ptr(buffers_data, data, int(meta.get(5, 0)))
   def val_data = _gltf_sparse_buffer_ptr(buffers_data, data, int(meta.get(6, 0)))
   if !idx_data || !val_data || count < 0 || elem_size <= 0 { return 0 }
   def out = malloc(count * elem_size)
   if !out { return 0 }
   if base_ptr && base_stride >= elem_size {
      if base_stride == elem_size {
         memcpy(out, base_ptr, count * elem_size)
      } else {
         mut i = 0
         while i < count {
            memcpy(ptr_add(out, i * elem_size), ptr_add(base_ptr, i * base_stride), elem_size)
            i += 1
         }
      }
   } else {
      memset(out, 0, count * elem_size)
   }
   mut si = 0
   while si < sparse_count {
      def dst_idx = _gltf_sparse_index_value(idx_data, idx_off, idx_comp, si)
      if dst_idx >= 0 && dst_idx < count {
         memcpy(ptr_add(out, dst_idx * elem_size), ptr_add(val_data, val_off + si * elem_size), elem_size)
      }
      si += 1
   }
   out
}

fn _gltf_resolve_accessor_data(any g, int acc_idx, any data) any {
   shr._gltf_ensure_caches()
   def accs = g.get("accessors", [])
   if acc_idx < 0 || acc_idx >= accs.len { return 0 }
   def acc = accs.get(acc_idx)
   def sparse = acc.get("sparse", 0)
   def cacheable = !is_dict(sparse)
   def cache_key = cacheable ? (shr._gltf_cache_key_from_g(g) + ":data:" + to_str(to_int(data)) + ":acc:" + to_str(int(acc_idx))) : ""
   if cacheable && _gltf_acc_res_cache.contains(cache_key) { return _gltf_acc_res_cache.get(cache_key, 0) }
   def count = acc.get("count", 0)
   def comp = acc.get("componentType", 0)
   def type_str = acc.get("type", "")
   def type_cnt = shr._gltf_type_count(type_str)
   def comp_size = shr._gltf_comp_size(comp)
   def elem_size = shr._gltf_elem_size(comp_size, type_str)
   def normalized = acc.get("normalized", false) ? true : false
   def bv_list = g.get("bufferViews", [])
   def buffers_data = g.get("_ny_buffer_data", 0)
   def bv_idx = acc.get("bufferView", -1)
   mut ptr = 0
   mut stride = 0
   mut owned = false
   if bv_idx >= 0 && bv_idx < bv_list.len {
      def bv = bv_list.get(bv_idx)
      def bv_ext = is_dict(bv) ? bv.get("extensions", 0) : 0
      def meshopt_ext = is_dict(bv_ext) ? bv_ext.get("EXT_meshopt_compression", bv_ext.get("KHR_meshopt_compression", 0)) : 0
      if is_dict(meshopt_ext) && !common.env_truthy("NY_GLTF_UNSAFE_UNDECODED_MESHOPT") { return 0 }
      def buf_idx = int(bv.get("buffer", 0))
      mut base_ptr = data
      if is_list(buffers_data) && buf_idx >= 0 && buf_idx < buffers_data.len {
         def b = buffers_data.get(buf_idx, 0)
         def bp = shr._gltf_blob_ptr(b)
         if bp { base_ptr = bp }
      }
      if base_ptr {
         ptr = ptr_add(ptr_add(base_ptr, int(bv.get("byteOffset", 0))), int(acc.get("byteOffset", 0)))
         stride = bv.get("byteStride", 0)
         if stride == 0 { stride = elem_size }
      }
   }
   if is_dict(sparse) {
      ptr = _gltf_sparse_materialize(sparse, bv_list, buffers_data, data, count, elem_size, ptr, stride)
      if !ptr { return 0 }
      stride = elem_size
      owned = true
   }
   def cols = shr._gltf_type_cols(type_str)
   def rows = shr._gltf_type_rows(type_str)
   mut out = {
      "ptr": ptr, "count": count, "stride": stride, "comp": comp,
      "normalized": normalized, "type_count": type_cnt, "type_str": type_str,
      "cols": cols, "rows": rows,
      "elem_size": elem_size, "owned": owned
   }
   if cacheable && !owned { _gltf_acc_res_cache = cache.cache_put_reset(shr._gltf_acc_res_cache, cache_key, out, shr._GLTF_CACHE_LIMIT_BIG, 256) }
   out
}

fn _gltf_release_accessor_data(any acc_res) int {
   if is_dict(acc_res) && acc_res.get("owned", false) {
      def ptr = acc_res.get("ptr", 0)
      if ptr { free(ptr) }
   }
   0
}

fn _gltf_primary_data_ptr(any gltf_data) any {
   if !is_dict(gltf_data) { return 0 }
   def binary_data = gltf_data.get("binary_data", 0)
   if is_dict(binary_data) {
      def ptr = binary_data.get("ptr", 0)
      if ptr { return ptr }
   } elif is_str(binary_data) && binary_data.len > 0 {
      return binary_data
   }
   def buffer_data = gltf_data.get("buffer_data", 0)
   if is_list(buffer_data) && buffer_data.len > 0 {
      def b0 = buffer_data.get(0, 0)
      def ptr0 = shr._gltf_blob_ptr(b0)
      if ptr0 { return ptr0 }
   }
   def g = gltf_data.get("gltf", 0)
   mut ny_bufs = 0
   if is_dict(g) { ny_bufs = g.get("_ny_buffer_data", 0) }
   if is_list(ny_bufs) && ny_bufs.len > 0 {
      def b0 = ny_bufs.get(0, 0)
      def ptr0 = shr._gltf_blob_ptr(b0)
      if ptr0 { return ptr0 }
   }
   0
}
