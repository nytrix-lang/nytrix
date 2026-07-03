;; Keywords: rsa hastad math crypto
;; RSA Hastad broadcast attack routines.
;; CRT combines e ciphertexts to get M^e over Z, then takes the eth root.
;; Works because M^e < product of moduli when e is small.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.hastad(hastad_solve, hastad_find, hastad_attack, hastad_attack_report, hastads_attack)
use std.math.nt

fn _hastad_exp(any e) int { int(e) }

fn hastad_solve(list ns, list cs, any e) any {
   "Recover plaintext m from Hastad's broadcast attack.
   ns: list of distinct moduli n_i(pairwise coprime).
   cs: list of ciphertexts c_i = m^e mod n_i.
   e: public exponent(typically 3).
   Uses CRT to compute M^e over the integers, then extracts the eth root.
   Returns m as an integer, or nil if recovery fails."
   def ee = _hastad_exp(e)
   def k = ns.len
   def kc = cs.len
   if k == 0 || kc != k || ee <= 0 { return nil }
   mut rems = []
   mut mods = []
   mut i = 0
   while i < k {
      rems = rems.append(cs.get(i))
      mods = mods.append(ns.get(i))
      i += 1
   }
   def me = crt(rems, mods)
   if me == nil { return nil }
   def m = nth_root(me, ee)
   mut check = Z(1)
   mut j = 0
   while j < ee {
      check = check * m
      j += 1
   }
   if check == me { return m }
   nil
}

fn _hastad_next_combo(list combo, int n, int k) bool {
   mut i = k - 1
   while i >= 0 {
      if combo.get(i) < n - k + i {
         combo[i] = combo.get(i) + 1
         mut j = i + 1
         while j < k {
            combo[j] = combo.get(j - 1) + 1
            j += 1
         }
         return true
      }
      i -= 1
   }
   false
}

fn _hastad_subset(list ns, list cs, list combo) list {
   mut sub_ns, sub_cs = [], []
   mut i = 0
   while i < combo.len {
      def idx = combo.get(i)
      sub_ns, sub_cs = sub_ns.append(ns.get(idx)), sub_cs.append(cs.get(idx))
      i += 1
   }
   [sub_ns, sub_cs]
}

fn hastad_find(list ns, list cs, any e) any {
   "Scan e-sized recipient subsets for a valid Hastad broadcast recovery.
   Returns [m, indices] or nil."
   def ee = _hastad_exp(e)
   if ns.len != cs.len || ns.len < ee { return nil }
   mut combo = []
   mut i = 0
   while i < ee {
      combo = combo.append(i)
      i += 1
   }
   while true {
      def sub = _hastad_subset(ns, cs, combo)
      def m = hastad_solve(sub.get(0), sub.get(1), ee)
      if m != nil { return [m, clone(combo)] }
      if !_hastad_next_combo(combo, ns.len, ee) { break }
   }
   nil
}

fn hastad_attack(list ns, any e, list cs) any {
   "Hastad broadcast attack entrypoint."
   hastad_solve(ns, cs, e)
}

fn hastad_attack_report(list ns, any e, list cs) dict {
   "Explain a Hastad broadcast recovery attempt.
   Returns a report with sample counts, selected indices, recovered plaintext
   when successful, and a short reason when recovery fails."
   def ee = _hastad_exp(e)
   mut reason = ""
   mut m = nil
   mut indices = []
   if ee <= 1 {
      reason = "exponent must be greater than one"
   } else if ns.len != cs.len {
      reason = "modulus and ciphertext lists must have the same length"
   } else if ns.len < ee {
      reason = "not enough samples for exponent"
   } else {
      def hit = hastad_find(ns, cs, ee)
      if hit == nil {
         reason = "no exact broadcast root found"
      } else {
         m = hit.get(0)
         indices = hit.get(1)
      }
   }
   {
      "ok": m != nil,
      "plaintext": m,
      "indices": indices,
      "reason": reason,
      "exponent": ee,
      "sample_count": ns.len,
      "required_samples": ee,
   }
}

fn hastads_attack(list ns, any e, list cs) any {
   "Pluralized alias for multi-key broadcast inputs."
   hastad_attack(ns, e, cs)
}
