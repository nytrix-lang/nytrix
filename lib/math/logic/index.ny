;; Keywords: logic proof certificate persistent index cache stale digest
;; Persistent certificate index with strict version and dependency invalidation.
module std.math.logic.index(
   open, lookup, put, save, size)

use std.core
use std.os
use std.os.fs as fs
use std.math.parse.data.json as json
use std.math.logic.certificate as cert

fn _fresh(str path, str module_version, str dependency_digest,
   str checker_version) dict {
   {"format":"ny-proof-index-v1", "path":path,
      "module_version":module_version,
      "dependency_digest":dependency_digest,
      "checker_version":checker_version, "entries":dict(64)}
}

fn _matches(dict index, any value) bool {
   is_dict(value) && value.get("format", "") == "ny-proof-index-v1" &&
      value.get("module_version", "") == index.get("module_version") &&
      value.get("dependency_digest", "") == index.get("dependency_digest") &&
      value.get("checker_version", "") == index.get("checker_version") &&
      is_dict(value.get("entries", nil))
}

;; Returns the result of the `open` operation.
fn open(str path, str module_version, str dependency_digest,
   str checker_version="logic-kernel-v1") dict {
   mut index = _fresh(path, module_version, dependency_digest, checker_version)
   if !fs.is_file(path) { return index }
   def loaded = json.json_decode(unwrap(file_read(path)))
   if !_matches(index, loaded) { return index }
   index["entries"] = loaded.get("entries")
   index
}

fn _probe(dict index, str canonical) dict {
   cert.envelope(canonical, index.get("module_version"),
      index.get("dependency_digest"), index.get("checker_version"))
}

;; Returns the result of the `lookup` operation.
fn lookup(dict index, str canonical, int max_variables=16,
   int max_nodes=100000, int max_depth=128, int max_steps=1000000,
   int max_memory=100000) any {
   def probe = _probe(index, canonical)
   def value = index.get("entries").get(cert.key(probe), nil)
   if value == nil || !cert.check(value, max_variables, max_nodes,
      max_depth, max_steps, max_memory) {
      return nil
   }
   value
}

;; Returns the result of the `put` operation.
fn put(dict index, dict certificate, int max_variables=16,
   int max_nodes=100000, int max_depth=128, int max_steps=1000000,
   int max_memory=100000) bool {
   if certificate.get("module_version", "") != index.get("module_version") ||
      certificate.get("dependency_digest", "") != index.get("dependency_digest") ||
      certificate.get("checker_version", "") != index.get("checker_version") ||
      !cert.check(certificate, max_variables, max_nodes, max_depth, max_steps,
         max_memory) {
      return false
   }
   index.get("entries")[cert.key(certificate)] = certificate
   true
}

;; Returns true when save.
fn save(dict index) bool {
   def path = index.get("path", "")
   if path.len == 0 { return false }
   def tmp = path + ".tmp-" + to_str(pid()) + "-" + to_str(ticks())
   match file_write(tmp, json.json_encode({
      "format":index.get("format"),
      "module_version":index.get("module_version"),
      "dependency_digest":index.get("dependency_digest"),
      "checker_version":index.get("checker_version"),
      "entries":index.get("entries")})) {
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
fn size(dict index) int { index.get("entries", {}).len }
