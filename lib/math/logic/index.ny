;; Keywords: logic proof certificate persistent index cache stale digest
;; Persistent certificate index with strict version and dependency invalidation.
module core.math.logic.index(
   open, lookup, put, save, size)
use core
use core.os as os
use core.math.parse.data.json (json_decode, json_try_decode, json_encode)
use core.math.logic.certificate as cert

fn _fresh(str path, str module_version, str dependency_digest,
   str checker_version) dict {
   {"format":"ny-proof-index-v1", "path":path,
      "module_version":module_version,
      "dependency_digest":dependency_digest,
      "checker_version":checker_version, "entries":dict(64)}
}

fn _matches(dict index, any value) bool {
   if !is_dict(value) { return false }
   def str format = value.get("format", "")
   def str module_version = value.get("module_version", "")
   def str dependency_digest = value.get("dependency_digest", "")
   def str checker_version = value.get("checker_version", "")
   def str expected_module_version = index.get("module_version", "")
   def str expected_dependency_digest = index.get("dependency_digest", "")
   def str expected_checker_version = index.get("checker_version", "")
   def entries = value.get("entries", nil)
   format == "ny-proof-index-v1" &&
   module_version == expected_module_version &&
   dependency_digest == expected_dependency_digest &&
   checker_version == expected_checker_version && is_dict(entries)
}

;; Returns the result of the `open` operation.
fn open(str path, str module_version, str dependency_digest,
   str checker_version="logic-kernel-v1") dict {
   mut index = _fresh(path, module_version, dependency_digest, checker_version)
   if !os.file_exists(path) { return index }
   ;; Keep the file payload at its declared string representation.  The
   ;; generic `unwrap` result is `any`; passing it directly into the JSON
   ;; parser can preserve the tagged Result payload as an integer word in
   ;; native lowering, making a valid saved index look like malformed JSON.
   mut str source = ""
   match os.file_read(path) {
      ok(content) -> { source = content }
      err(_) -> { return index }
   }
   def decoded = json_try_decode(source)
   if !decoded.get("ok", false) { return index }
   def loaded = decoded.get("value", 0)
   if !_matches(index, loaded) { return index }
   def source_entries = loaded.get("entries", dict(0))
   index = dict_set(index, "entries", source_entries)
   index
}

;; Returns the result of the `lookup` operation.
fn lookup(dict index, str canonical, int max_variables=16,
   int max_nodes=100000, int max_depth=128, int max_steps=1000000,
   int max_memory=100000) any {
   def str module_version = index.get("module_version", "")
   def str dependency_digest = index.get("dependency_digest", "")
   def str checker_version = index.get("checker_version", "")
   def probe = cert.envelope(canonical, module_version, dependency_digest,
      checker_version)
   def str probe_key = cert.key(probe)
   def dict entries = index.get("entries", dict(0))
   def any value = entries.get(probe_key, nil)
   if value == nil { return nil }
   if !cert.check(value) { return nil }
   value
}

fn _put_impl(dict index, dict certificate, int max_variables=16,
   int max_nodes=100000, int max_depth=128, int max_steps=1000000,
   int max_memory=100000) bool {
   def module_match = certificate.get("module_version", "") ==
   index.get("module_version")
   def dependency_match = certificate.get("dependency_digest", "") ==
   index.get("dependency_digest")
   def checker_match = certificate.get("checker_version", "") ==
   index.get("checker_version")
   def checked = cert.check(certificate, max_variables, max_nodes, max_depth,
      max_steps, max_memory)
   if module_match == false || dependency_match == false ||
   checker_match == false || checked == false {
      return false
   }
   mut entries = index.get("entries", dict(0))
   def str key = cert.key(certificate)
   entries = dict_set(entries, key, certificate)
   ;; `put` promises an in-place update of the caller-owned index. Rebinding
   ;; this local dictionary only changed the callee's handle, so `save(index)`
   ;; serialized the original empty entries table after a successful put.
   index["entries"] = entries
   true
}

;; Returns the result of the `put` operation.
fn put(dict index, dict certificate, int max_variables=16,
   int max_nodes=100000, int max_depth=128, int max_steps=1000000,
   int max_memory=100000) bool {
   return _put_impl(index, certificate, max_variables, max_nodes, max_depth,
      max_steps, max_memory)
}

;; Returns true when save.
fn save(dict index) bool {
   def str path = index.get("path", "")
   if path.len == 0 { return false }
   def tmp = path + ".tmp-" + to_str(pid()) + "-" + to_str(ticks())
   def str payload = json_encode({
         "format":index.get("format"),
         "module_version":index.get("module_version"),
         "dependency_digest":index.get("dependency_digest"),
         "checker_version":index.get("checker_version"),
         "entries":index.get("entries")})
   match file_write(tmp, payload) {
      ok(written) -> {
         written
         match fs.rename(tmp, path) {
            ok(renamed) -> { renamed  return true }
            err(rename_error) -> { rename_error  file_remove(tmp)  return false }
         }
      }
      err(write_error) -> { write_error  return false }
   }
}

;; Returns the result of the `size` operation.
fn size(dict index) int {
   dict_len(index.get("entries", dict(0)))
}
