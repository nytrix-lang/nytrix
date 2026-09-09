use std.core

;; Dynamic shifts must retain values that no longer fit the tagged-int payload.
assert(to_str(__shl(0x78, 56)) == "8646911284551352320",
   "left shift preserves high bits beyond tagged range")
assert(to_str(__shl(-1, 63)) == "-9223372036854775808",
   "negative left shift promotes without changing sign")
assert(__shl(1, -1) == 1, "negative shift count leaves value unchanged")
assert(to_str(__shl(1, 64)) == "18446744073709551616",
   "large shift count promotes")
print("✓ shift promotion tests passed")
