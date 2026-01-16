;; Compound assignment operator tests

fn main() {
   def x = 10
   x += 5
   assert(x == 15, "x += 5 failed")
   x -= 3
   assert(x == 12, "x -= 3 failed")
   x *= 2
   assert(x == 24, "x *= 2 failed")
   x /= 4
   assert(x == 6, "x /= 4 failed")
   x %= 5
   assert(x == 1, "x %= 5 failed")
   ; Test chaining
   def y = 100
   y += 10
   y -= 5
   y *= 2
   y /= 3
   y %= 50
   assert(y == 20, "chain failed")
   print("✓ Compound assignment tests passed")
   return 0
}
