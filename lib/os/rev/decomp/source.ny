;; Keywords: source paths files bytes normalization
;; Source normalization for decompiler paths, bytes, and analysis records.
module std.os.rev.decomp.source(_read, _source_data, _looks_path)

use std.core
use std.os (file_exists, file_read)

fn _read(str p) dict {
   if !file_exists(p) { return {"ok": false, "path": p, "reason": "missing"} }
   def r = file_read(p)
   if is_err(r) { return {"ok": false, "path": p, "reason": "read_failed", "error": r} }
   {"ok": true, "path": p, "data": unwrap(r)}
}

fn _source_data(any source) str {
   if is_dict(source) {
      if source.contains("data") { return source.get("data", "") }
      if source.contains("path") { return _read(source.get("path", "")).get("data", "") }
   }
   if is_bytes(source) { return source }
   if is_str(source) {
      if _looks_path(source) && file_exists(source) { return _read(source).get("data", "") }
      return source
   }
   ""
}

fn _looks_path(str s) bool {
   if s.len == 0 || s.len > 4096 { return false }
   mut i = 0
   while i < s.len {
      def c = load8(s, i)
      if c == 0 || (c < 32 && c != 9 && c != 10 && c != 13) { return false }
      i += 1
   }
   true
}

#main {
   assert(_source_data({"data": "ELF"}) == "ELF" && !_looks_path("a\x00b"), "source normalization")
   print("✓ std.os.rev.decomp.source self-test passed")
}
