;; Keywords: encoding pairing math crypto
;; Encoding routines for Cantor pairing and unpairing.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc7468
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.pairing(cantor_pair, cantor_unpair, cantor_unpair_leaves)
use std.core
use std.math.nt
use std.math.bin

fn cantor_pair(any x, any y) any {
   "Cantor-pair two non-negative integers into one integer."
   def s = Z(x) + Z(y)
   ((s * (s + Z(1))) / Z(2)) + Z(y)
}

fn cantor_unpair(any z) list {
   "Inverse Cantor pairing. Returns `[x, y]` for a non-negative integer."
   def zz = Z(z)
   def w = (isqrt(Z(8) * zz + Z(1)) - Z(1)) / Z(2)
   def t = (w * (w + Z(1))) / Z(2)
   def y = zz - t
   def x = w - y
   [x, y]
}

fn cantor_unpair_leaves(any z, int leaf_limit=256) list {
   "Decode a recursively Cantor-paired integer into leaves below `leaf_limit`.
   This mirrors recursive encoders that stop at byte-sized leaves."
   mut stack = [Z(z)]
   mut out = []
   while stack.len > 0 {
      def cur = stack[stack.len - 1]
      stack = slice(stack, 0, stack.len - 1)
      if cur < Z(leaf_limit) {
         out = out.append(int(cur))
      } else {
         def xy = cantor_unpair(cur)
         stack = stack.append(xy[1])
         stack = stack.append(xy[0])
      }
   }
   out
}

#main {
   def p = cantor_pair(17, 29)
   assert(cantor_unpair(p) == [Z(17), Z(29)], "Cantor unpair roundtrip")
   def nested = cantor_pair(cantor_pair(65, 66), cantor_pair(67, 68))
   def leaves = cantor_unpair_leaves(nested)
   assert(leaves == [65, 66, 67, 68], "nested Cantor leaves")
   assert(leaves.text == "ABCD", "nested Cantor text")
   print("CANTOR_PAIRING_OK")
   print("✓ std.math.crypto.encoding.pairing self-test passed")
}
