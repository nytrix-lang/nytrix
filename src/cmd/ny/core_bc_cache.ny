;; Keywords: compiler cache bitcode
;; Validates a shared core bitcode artifact against its exact build inputs.
module core.compiler.tools.core_bc_cache
use core
use core.os (args, file_exists, file_read, exit)
use core.math.crypto.hash as hash
use core.str as str

def _CORE_BC_SIGNATURE_PREFIX =
"artifact=core.bc\nselector=lib\nformat=nyir-bodies-v2\nentry=no\ncompiler-source="

fn _read_text(str path) str {
   match file_read(path) {
      ok(content) -> content
      err(_) -> ""
   }
}

fn _valid_signature(str signature) bool {
   if !str.startswith(signature, _CORE_BC_SIGNATURE_PREFIX) ||
   signature.len != _CORE_BC_SIGNATURE_PREFIX.len + 65 {
      return false
   }
   mut int i = _CORE_BC_SIGNATURE_PREFIX.len
   while i < signature.len - 1 {
      def int value = str.hex_val(load8(signature, i))
      if value < 0 || value > 15 { return false }
      i += 1
   }
   load8(signature, signature.len - 1) == 10
}

fn _matches(str bitcode, str core_source, str signature_path,
   str inputs_path) bool {
   if !file_exists(bitcode) || !file_exists(core_source) ||
   !file_exists(signature_path) || !file_exists(inputs_path) {
      return false
   }
   def signature = _read_text(signature_path)
   if !_valid_signature(signature) { return false }
   def inputs = _read_text(inputs_path)
   def expected = hash.sha256_hex(_read_text(core_source)) + "\n" +
   hash.sha256_hex(signature) + "\n"
   inputs == expected
}

#main {
   def ag = args()
   if ag.len != 5 { exit(2) }
   def bitcode = to_str(ag.get(1))
   def core_source = to_str(ag.get(2))
   def signature = to_str(ag.get(3))
   def inputs = to_str(ag.get(4))
   if _matches(bitcode, core_source, signature, inputs) { exit(0) }
   exit(1)
}
