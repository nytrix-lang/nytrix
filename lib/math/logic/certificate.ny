;; Keywords: logic proof certificate checker canonical digest budget
;; Compact certificate envelope shared by self-hosted reasoning modules.
module std.math.logic.certificate(
   envelope, check, key)

use std.core

;; Returns the result of the `envelope` operation.
fn envelope(str canonical, str module_version, str dependency_digest,
   str checker_version="logic-kernel-v1") dict {
   def digest = __proof_cert_digest(canonical, module_version,
      dependency_digest, checker_version)
   return {"format":"ny-proof-v1", "canonical":canonical,
      "digest":digest, "module_version":module_version,
      "dependency_digest":dependency_digest,
      "checker_version":checker_version}
}

;; Returns the result of the `key` operation.
fn key(dict certificate) str {
   to_str(certificate.get("digest", -1)) + ":" +
      certificate.get("module_version", "") + ":" +
      certificate.get("dependency_digest", "") + ":" +
      certificate.get("checker_version", "")
}

;; Returns the result of the `check` operation.
fn check(any certificate, int max_variables=16, int max_nodes=100000,
   int max_depth=128, int max_steps=1000000, int max_memory=100000) bool {
   if !is_dict(certificate) || certificate.get("format", "") != "ny-proof-v1" ||
      !is_str(certificate.get("canonical", nil)) ||
      !is_int(certificate.get("digest", nil)) ||
      !is_str(certificate.get("module_version", nil)) ||
      !is_str(certificate.get("dependency_digest", nil)) ||
      !is_str(certificate.get("checker_version", nil)) {
      return false
   }
   if max_variables < 0 || max_nodes <= 0 || max_depth <= 0 || max_steps <= 0 ||
      max_memory <= 0 {
      return false
   }
   __proof_cert_check(certificate.get("canonical"),
      certificate.get("digest"), certificate.get("module_version"),
      certificate.get("dependency_digest"),
      certificate.get("checker_version"), max_variables, max_nodes,
      max_depth, max_steps, max_memory)
}
