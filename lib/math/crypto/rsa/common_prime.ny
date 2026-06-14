;; Keywords: rsa common-prime math crypto
;; RSA common-prime detection and recovery routines.
;; Factor faulty RSA moduli that accidentally reuse a prime.
;; Reference:
;; - common-prime attack literature and standard batch-GCD observation
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.common_prime(common_prime_factor_pair, common_prime_recover_pair, common_prime_scan, common_prime_scan_fast, common_prime_factor_all, common_factors_attack, common_factors_factor_all)
use std.math.nt
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn _z(any x) any { is_bigint(x) ? x : Z(x) }

fn _z_moduli(list moduli) list {
   def n = moduli.len
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, _z(moduli[i]))
      i += 1
   }
   out
}

fn _common_prime_private(any e, any p, any q) any { compute_d(e, compute_phi(p, q)) }

fn common_prime_factor_pair(any n1, any n2) any {
   "Return [p, q1, q2] if n1 and n2 share a non-trivial prime, nil otherwise."
   def p = gcd(n1, n2)
   if p <= 1 || p == n1 || p == n2 { return nil }
   if n1 % p != 0 || n2 % p != 0 { return nil }
   [p, n1 / p, n2 / p]
}

fn common_prime_recover_pair(any n1, any e1, any c1, any n2, any e2, any c2) any {
   "Recover [m1, m2, p, q1, q2, d1, d2] from two RSA ciphertexts if the moduli share a prime."
   def facs = common_prime_factor_pair(n1, n2)
   if facs == nil { return nil }
   def p = facs[0]
   def q1 = facs[1]
   def q2 = facs[2]
   def d1 = _common_prime_private(e1, p, q1)
   def d2 = _common_prime_private(e2, p, q2)
   if d1 == nil || d2 == nil || d1 == 0 || d2 == 0 { return nil }
   def m1, m2 = power_mod(c1, d1, n1), power_mod(c2, d2, n2)
   [m1, m2, p, q1, q2, d1, d2]
}

fn common_prime_scan(list moduli) list {
   "Scan a list of moduli and return all shared-prime hits as [i, j, p, qi, qj]."
   mut hits = []
   mut i = 0
   while i < moduli.len {
      def ni = moduli[i]
      mut j = i + 1
      while j < moduli.len {
         def nj = moduli[j]
         def facs = common_prime_factor_pair(ni, nj)
         if facs != nil { hits = hits.append([i, j, facs[0], facs[1], facs[2]]) }
         j += 1
      }
      i += 1
   }
   hits
}

fn _prefix_products(list moduli) list {
   def n = moduli.len
   mut pref = list(n + 1)
   __list_set_len(pref, n + 1)
   __store_item_fast(pref, 0, Z(1))
   mut i = 0
   while i < n {
      __store_item_fast(pref, i + 1, pref[i] * moduli[i])
      i += 1
   }
   pref
}

fn _suffix_products(list moduli) list {
   def n = moduli.len
   mut suf = list(n + 1)
   __list_set_len(suf, n + 1)
   mut i = 0
   while i <= n {
      __store_item_fast(suf, i, Z(1))
      i += 1
   }
   i = n - 1
   while i >= 0 {
      __store_item_fast(suf, i, suf[i + 1] * moduli[i])
      i -= 1
   }
   suf
}

fn _common_prime_product_tree(list zmods) list {
   mut layers = list(4)
   layers = layers.append(zmods)
   mut layer = zmods
   while layer.len > 1 {
      mut next = list((layer.len + 1) / 2)
      mut i = 0
      while i < layer.len {
         if i + 1 < layer.len {
            next = next.append(layer[i] * layer[i + 1])
         } else {
            next = next.append(layer[i])
         }
         i += 2
      }
      layers = layers.append(next)
      layer = next
   }
   layers
}

fn _common_prime_tree_collect(list layers, int level, int idx, any rem, list out) list {
   def prod = layers[level][idx]
   if level == 0 {
      def ni = prod
      if ni > 1 {
         def r = rem % (ni * ni)
         def p = gcd(r / ni, ni)
         if p > 1 && p < ni && ni % p == 0 { out = out.append([idx, p, ni / p]) }
      }
      return out
   }
   def prev = layers[level - 1]
   def left = idx * 2
   if left < prev.len {
      def child = prev[left]
      out = _common_prime_tree_collect(layers, level - 1, left, rem % (child * child), out)
   }
   def right = left + 1
   if right < prev.len {
      def child = prev[right]
      out = _common_prime_tree_collect(layers, level - 1, right, rem % (child * child), out)
   }
   out
}

fn _common_prime_factor_all_tree(list zmods) list {
   if zmods.len == 0 { return [] }
   def layers = _common_prime_product_tree(zmods)
   def top = layers.len - 1
   _common_prime_tree_collect(layers, top, 0, layers[top][0], list(zmods.len))
}

fn common_prime_factor_all(any moduli) list {
   "Batch common-factor discovery for many moduli.
   Returns [i, p, q] for entries where n_i = p*q was recovered via shared p."
   if !is_list(moduli) || moduli.len == 0 { return [] }
   def zmods = _z_moduli(moduli)
   if zmods.len >= 96 { return _common_prime_factor_all_tree(zmods) }
   def pref = _prefix_products(zmods)
   def suf = _suffix_products(zmods)
   mut out = list(zmods.len)
   mut i = 0
   while i < zmods.len {
      def ni = zmods[i]
      if ni > 1 {
         def others = pref[i] * suf[i + 1]
         def p = gcd(ni, others)
         if p > 1 && p < ni && ni % p == 0 { out = out.append([i, p, ni / p]) }
      }
      i += 1
   }
   out
}

fn common_prime_scan_fast(any moduli) list {
   "Fast shared-prime scan using batch-GCD over all moduli.
   Returns pair hits as [i, j, p, qi, qj], like common_prime_scan."
   if !is_list(moduli) || moduli.len < 2 { return [] }
   def facts = common_prime_factor_all(moduli)
   if facts.len == 0 { return [] }
   mut out = list((facts.len * (facts.len - 1)) / 2)
   mut i = 0
   while i < facts.len {
      def fi, ii = facts[i], int(fi[0])
      def pi, qi = _z(fi[1]), _z(fi[2])
      mut j = i + 1
      while j < facts.len {
         def fj, jj = facts[j], int(fj[0])
         def pj, qj = _z(fj[1]), _z(fj[2])
         if pi == pj {
            def a, b = ii < jj ? ii : jj, ii < jj ? jj : ii
            def qa, qb = ii < jj ? qi : qj, ii < jj ? qj : qi
            out = out.append([a, b, pi, qa, qb])
         }
         j += 1
      }
      i += 1
   }
   out
}

fn common_factors_attack(any moduli) list {
   "Multi-key common-factors attack entrypoint.
   Returns pair hits [i, j, p, qi, qj]."
   common_prime_scan_fast(moduli)
}

fn common_factors_factor_all(any moduli) list {
   "Return [i, p, q] recovered entries via batch-GCD."
   common_prime_factor_all(moduli)
}

#main {
   def shared = [101, 103, 107, 109, 113, 127, 131, 137]
   mut mods = list(96)
   mut q = 1009
   mut i = 0
   while i < 96 {
      q = int(next_prime(q + 10))
      mods = mods.append(shared[i % shared.len] * q)
      i += 1
   }
   assert(common_prime_factor_all(mods).len == mods.len, "common-prime product-tree shared corpus")
   mut clean = list(96)
   mut a, b = 1009, 1000003
   i = 0
   while i < 96 {
      a = int(next_prime(a + 10))
      b = int(next_prime(b + 10))
      clean = clean.append(a * b)
      i += 1
   }
   assert(common_prime_factor_all(clean).len == 0, "common-prime product-tree clean corpus")
   print("✓ std.math.crypto.rsa.common_prime self-test passed")
}
