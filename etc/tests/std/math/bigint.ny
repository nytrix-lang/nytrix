use std.io
use std.math.bigint
use std.core.error
use std.core.reflect

fn test_bigint_basic(){
   print("Testing bigint basic...")
   def a = bigint_from_str("123456789012345678901234567890")
   def b = bigint_from_str("987654321098765432109876543210")
   def s = bigint_add(a, b)
   assert(eq(bigint_to_str(s), "1111111110111111111011111111100"), "add")
   def d = bigint_sub(b, a)
   assert(eq(bigint_to_str(d), "864197532086419753208641975320"), "sub")
   def m = bigint_mul(bigint_from_str("123456789"), bigint_from_str("987654321"))
   assert(eq(bigint_to_str(m), "121932631112635269"), "mul")
   def q = bigint_div(bigint_from_str("1000000000000"), bigint_from_str("12345"))
   def r = bigint_mod(bigint_from_str("1000000000000"), bigint_from_str("12345"))
   assert(eq(bigint_to_str(q), "81004455"), "div")
   assert(eq(bigint_to_str(r), "3025"), "mod")
   print("bigint basic passed")
}

fn test_bigint_sign(){
   print("Testing bigint sign...")
   def a = bigint_from_str("-999999999999")
   def b = bigint_from_str("2")
   def s = bigint_add(a, b)
   assert(eq(bigint_to_str(s), "-999999999997"), "add sign")
   def p = bigint_mul(a, b)
   assert(eq(bigint_to_str(p), "-1999999999998"), "mul sign")
   def q = bigint_div(a, b)
   assert(eq(bigint_to_str(q), "-499999999999"), "div sign")
   def r = bigint_mod(a, b)
   assert(eq(bigint_to_str(r), "-1"), "mod sign")
   print("bigint sign passed")
}

fn test_main(){
   test_bigint_basic()
   test_bigint_sign()
   print("✓ std.math.bigint passed")
}

test_main()
