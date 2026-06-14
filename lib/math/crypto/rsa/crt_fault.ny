;; Keywords: rsa crt-fault math crypto
;; RSA CRT fault attack routines.
;; References:
;; - Boneh, DeMillo, Lipton, "On the Importance of Checking Cryptographic Protocols for Faults"
module std.math.crypto.rsa.crt_fault(crt_fault_known_message, crt_fault_unknown_message, crt_fault_attack, crt_fault_attack_known_message)
use std.math.nt

fn _normalize_pair(any p, any q) list { bigint_le(p, q) ? [p, q] : [q, p] }

fn crt_fault_known_message(any n, any e, any m, any faulty_sig) any {
   "Recover [p, q] from a known message and a faulty CRT-RSA signature."
   def g = gcd(Z(m) - power_mod(Z(faulty_sig), Z(e), Z(n)), Z(n))
   if bigint_eq(g, Z(1)) || bigint_eq(g, Z(0)) || bigint_eq(g, Z(n)) { return nil }
   _normalize_pair(g, Z(n) / g)
}

fn crt_fault_unknown_message(any n, any e, any valid_sig, any faulty_sig) any {
   "Recover [p, q] from a valid/faulty CRT-RSA signature pair on the same unknown message."
   if Z(valid_sig) == Z(faulty_sig) { return nil }
   def g = gcd(Z(valid_sig) - Z(faulty_sig), Z(n))
   if bigint_eq(g, Z(1)) || bigint_eq(g, Z(0)) || bigint_eq(g, Z(n)) { return nil }
   _normalize_pair(g, Z(n) / g)
}

fn crt_fault_attack(any n, any e, any valid_sig, any faulty_sig) any {
   "Valid/faulty CRT-RSA signature attack entrypoint."
   crt_fault_unknown_message(n, e, valid_sig, faulty_sig)
}

fn crt_fault_attack_known_message(any n, any e, any m, any faulty_sig) any {
   "Known-message CRT-RSA fault attack entrypoint."
   crt_fault_known_message(n, e, m, faulty_sig)
}
