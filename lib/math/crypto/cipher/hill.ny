;; Keywords: cipher hill math crypto
;; Hill cipher and modular-matrix decoding routines.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.hill(hill_encrypt, hill_decrypt, find_mod_inverse_26)
use std.core
use std.math.nt
use std.math.matrix
use std.core.str

fn _hill_builder_take(list b) str { def out = builder_to_str(b) builder_free(b) out }

fn find_mod_inverse_26(int n) int {
   "Find the modular multiplicative inverse of n mod 26, returning -1 if no inverse exists."
   def inv = inverse_mod(n, 26)
   inv == 0 ? -1 : int(inv)
}

fn _hill_key_matrix(any key_mat) any {
   if is_matrix(key_mat) { return key_mat }
   Matrix(key_mat)
}

fn _hill_apply_matrix(str msg, any key) str {
   mut result = Builder(msg.len + 8)
   mut i = 0
   while i < msg.len {
      def c1, c2 = ord(msg[i]) - 65, ord(msg[i + 1]) - 65
      def r1 = (c1 * mat_get(key, 0, 0) + c2 * mat_get(key, 0, 1)) % 26
      def r2 = (c1 * mat_get(key, 1, 0) + c2 * mat_get(key, 1, 1)) % 26
      result = builder_append(result, chr(r1 + 65))
      result = builder_append(result, chr(r2 + 65))
      i = i + 2
   }
   _hill_builder_take(result)
}

fn hill_encrypt(str msg, any key_mat) str {
   "Encrypt a message using the Hill cipher with a 2x2 key matrix, padding with 'X' if needed."
   def key = _hill_key_matrix(key_mat)
   mut m = msg
   if m.len % 2 != 0 { m = m + "X" }
   _hill_apply_matrix(m, key)
}

fn hill_decrypt(str msg, any key_mat) any {
   "Decrypt a Hill cipher message using the inverse of the 2x2 key matrix, returning nil if the key is not invertible."
   def key = _hill_key_matrix(key_mat)
   def det = (mat_get(key, 0, 0) * mat_get(key, 1, 1) - mat_get(key, 0, 1) * mat_get(key, 1, 0)) % 26
   def inv_det = find_mod_inverse_26((det + 26) % 26)
   if inv_det == -1 { return nil }
   def inv_mat = Matrix([
         [
            (mat_get(key, 1, 1) * inv_det) % 26,
            ((0 - mat_get(key, 0, 1)) * inv_det % 26 + 26) % 26
         ],
         [
            ((0 - mat_get(key, 1, 0)) * inv_det % 26 + 26) % 26,
            (mat_get(key, 0, 0) * inv_det) % 26
         ]
   ])
   _hill_apply_matrix(msg, inv_mat)
}
