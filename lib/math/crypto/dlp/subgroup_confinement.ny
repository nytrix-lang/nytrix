;; Keywords: dlp discrete-log group-theory subgroup-confinement math crypto
;; Subgroup-confinement attack routines for discrete-log protocols.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
;; References:
;; - std.math.crypto.dlp
;; - std.math.crypto
module std.math.crypto.dlp.subgroup_confinement(dh_small_subgroup_element, dh_small_subgroup_confinement)
use std.math.nt

fn _sg_pow_int(any base, any exp) any {
   mut out = Z(1)
   mut b = Z(base)
   mut e = int(exp)
   while e > 0 {
      case e % 2 {
         1 -> { out = out * b }
         _ -> {}
      }
      b, e = b * b, e / 2
   }
   out
}

fn dh_small_subgroup_element(any g, any p, any subgroup_order) any {
   "Construct an element whose order divides subgroup_order."
   if subgroup_order == nil || subgroup_order <= 0 { return nil }
   def full_order = Z(p) - Z(1)
   if full_order % Z(subgroup_order) != Z(0) { return nil }
   def h = power_mod(Z(g), full_order / Z(subgroup_order), Z(p))
   if h == Z(1) { return nil }
   h
}

fn dh_small_subgroup_confinement(fnptr oracle_fn, any g, any p, list subgroup_factors, any upper_bound=nil) any {
   "Recover a DH secret exponent x modulo the product of chosen subgroup
   orders using a confinement oracle. oracle_fn(h) should return the victim's
   shared secret h^x mod p for the chosen subgroup element h."
   mut rems = []
   mut mods = []
   mut covered = Z(1)
   mut i = 0
   while i < subgroup_factors.len {
      def ent = subgroup_factors[i]
      def q = ent[0]
      def e = ent[1]
      def qpow = _sg_pow_int(q, e)
      def h = dh_small_subgroup_element(g, p, qpow)
      if h != nil {
         def target = Z(oracle_fn(h))
         mut r = Z(0)
         mut found = nil
         while r < qpow {
            if power_mod(h, r, p) == target {
               found = r
               r = qpow
            } else {
               r = r + Z(1)
            }
         }
         if found != nil {
            rems = rems.append(found)
            mods = mods.append(qpow)
            covered = covered * qpow
            if upper_bound != nil && covered >= Z(upper_bound) { i = subgroup_factors.len } else { i += 1 }
         } else {
            return nil
         }
      } else {
         i += 1
      }
   }
   if rems.len == 0 { return nil }
   crt(rems, mods)
}
