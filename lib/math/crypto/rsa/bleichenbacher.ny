;; Keywords: rsa bleichenbacher math crypto
;; Bleichenbacher RSA padding-oracle attack routines.
;; Reference:
;; - Bleichenbacher D., "Chosen Ciphertext Attacks Against Protocols Based on the RSA Encryption Standard PKCS #1"
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.bleichenbacher(bleichenbacher_attack, bleichenbacher_signature_suffix_forgery)
use std.math.nt
use std.math.crypto.rsa.op (compute_phi, compute_d)
use std.core.error

fn _floor_div(any a, any b) any { a / b }

fn _ceil_div(any a, any b) any { a / b + ((a % b) != 0 ? 1 : 0) }

fn _insert_interval(list M, any a, any b) list {
   mut i = 0
   while i < M.len {
      def cur = M[i]
      def a0 = cur[0]
      def b0 = cur[1]
      if a0 <= b && a <= b0 {
         M[i] = [min(a, a0), max(b, b0)]
         return M
      }
      i += 1
   }
   M.append([a, b])
}

fn _bb_step1(any padding_oracle, any n, any e, any c) any {
   mut s0, c0 = 1, c
   if padding_oracle(c0) { return [s0, c0] }
   mut s = 2
   while s < n {
      c0 = mod(c * power_mod(s, e, n), n)
      if padding_oracle(c0) { return [s, c0] }
      s += 1
   }
   nil
}

fn _bb_step2a(any padding_oracle, any n, any e, any c0, any B) any {
   mut s = _ceil_div(n, 3 * B)
   while !padding_oracle(mod(c0 * power_mod(s, e, n), n)) { s += 1 }
   s
}

fn _bb_step2b(any padding_oracle, any n, any e, any c0, any s) any {
   mut ss = s + 1
   while !padding_oracle(mod(c0 * power_mod(ss, e, n), n)) { ss += 1 }
   ss
}

fn _bb_step2c(any padding_oracle, any n, any e, any c0, any B, any s, any a, any b) any {
   mut r = _ceil_div(2 * (b * s - 2 * B), n)
   while true {
      def left = _ceil_div(2 * B + r * n, b)
      def right = _floor_div(3 * B + r * n, a)
      mut ss = left
      while ss <= right {
         if padding_oracle(mod(c0 * power_mod(ss, e, n), n)) { return ss }
         ss += 1
      }
      r += 1
   }
   0
}

fn _bb_step3(any n, any B, any s, list M) list {
   mut M2 = []
   mut mi = 0
   while mi < M.len {
      def cur = M[mi]
      def a = cur[0]
      def b = cur[1]
      def left = _ceil_div(a * s - 3 * B + 1, n)
      def right = _floor_div(b * s - 2 * B, n)
      mut r = left
      while r <= right {
         def a2, b2 = max(a, _ceil_div(2 * B + r * n, s)), min(b, _floor_div(3 * B - 1 + r * n, s))
         M2 = _insert_interval(M2, a2, b2)
         r += 1
      }
      mi += 1
   }
   M2
}

fn bleichenbacher_attack(any padding_oracle, any n, any e, any c) any {
   "Recover plaintext with PKCS#1 v1.5-style oracle returning true when plaintext lies in [2B, 3B)."
   def k, B = _ceil_div(bit_length(n), 8), 1 << (8 * (k - 2))
   def step1 = _bb_step1(padding_oracle, n, e, c)
   if step1 == nil { return nil }
   def s0, c0 = step1[0], step1[1]
   mut M, s = [[2 * B, 3 * B - 1]], _bb_step2a(padding_oracle, n, e, c0, B)
   M = _bb_step3(n, B, s, M)
   while true {
      if M.len > 1 { s = _bb_step2b(padding_oracle, n, e, c0, s) } else {
         def iv = M[0]
         def a = iv[0]
         def b = iv[1]
         if a == b { return mod(a * inverse_mod(s0, n), n) }
         s = _bb_step2c(padding_oracle, n, e, c0, B, s, a, b)
      }
      M = _bb_step3(n, B, s, M)
   }
   nil
}

fn bleichenbacher_signature_suffix_forgery(any suffix, int suffix_bit_length) any {
   "Return s such that s^3 ends with the provided odd suffix.
   suffix_bit_length is the number of low bits that must match."
   assert((suffix % 2) == 1, "target suffix must be odd")
   mut s, i = 1, 0
   while i < suffix_bit_length {
      if (((s * s * s) >> i) & 1) != ((suffix >> i) & 1) { s = s | (1 << i) }
      i += 1
   }
   s
}
