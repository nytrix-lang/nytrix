;; Keywords: rsa non-coprime roots nthroot crt plaintext candidates ctf
;; Compact helpers for RSA plaintext recovery when e is not coprime to phi(n).
module std.math.crypto.rsa.roots(
   rsa_crt_pq,
   rsa_plaintexts_from_pq,
   rsa_ascii_hits_from_pq,
   rsa_find_flag_from_pq
)

use std.math.nt
use std.math.crypto.support.tools (
   bytes_ascii,
   bytes_contains,
   bytes_has_prefix,
   bytes_is_printable_ascii,
   extract_flag_bytes
)

fn rsa_crt_pq(any xp, any xq, any p, any q) any {
   "Combine x = xp mod p and x = xq mod q."
   def pp = Z(p)
   def qq = Z(q)
   def inv_q = inverse_mod(qq, pp)
   if inv_q == Z(0) { return nil }
   mod(Z(xq) + mod((Z(xp) - Z(xq)) * inv_q, pp) * qq, pp * qq)
}

fn rsa_plaintexts_from_pq(any c, any e, any p, any q) list {
   "Return all plaintext candidates for c = m^e mod p*q using known p,q."
   def pp = Z(p)
   def qq = Z(q)
   def rp = mod_nth_roots_prime(mod(c, pp), e, pp)
   def rq = mod_nth_roots_prime(mod(c, qq), e, qq)
   mut out = []
   mut i = 0
   while i < rp.len {
      mut j = 0
      while j < rq.len {
         def m = rsa_crt_pq(rp.get(i), rq.get(j), pp, qq)
         if m != nil { out = out.append(m) }
         j += 1
      }
      i += 1
   }
   out
}

fn _rsa_hit_bytes(list bs, list prefixes, bool allow_brace, bool allow_printable) bool {
   mut i = 0
   while i < prefixes.len {
      if bytes_has_prefix(bs, prefixes.get(i)) { return true }
      i += 1
   }
   if allow_brace && bytes_contains(bs, str_to_bytes("{")) { return true }
   if allow_printable && bytes_is_printable_ascii(bs, 1, 100) { return true }
   false
}

fn rsa_ascii_hits_from_pq(
   any c,
   any e,
   any p,
   any q,
   list prefixes=["CU", "flag", "crypto", "CTF"],
   bool allow_brace=true,
   bool allow_printable=true
) list {
   "Return ASCII-looking plaintext candidates, similar to quick Python CTF filters."
   def ms = rsa_plaintexts_from_pq(c, e, p, q)
   mut out = []
   mut i = 0
   while i < ms.len {
      def bs = long_to_bytes(ms.get(i))
      if _rsa_hit_bytes(bs, prefixes, allow_brace, allow_printable) {
         out = out.append(bytes_ascii(bs))
      }
      i += 1
   }
   out
}

fn rsa_find_flag_from_pq(any c, any e, any p, any q, str prefix="FlagY{", str suffix="}") str {
   "Return first prefix...suffix flag found among RSA root candidates."
   def ms = rsa_plaintexts_from_pq(c, e, p, q)
   mut i = 0
   while i < ms.len {
      def flag = extract_flag_bytes(long_to_bytes(ms.get(i)), prefix, suffix)
      if flag != "" { return flag }
      i += 1
   }
   ""
}
