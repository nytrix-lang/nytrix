use std.core
use std.core.test

print("Testing block scoping...")

;; Test 1: Outer visible in
mut x = 10
{
   assert(x == 10, "Outer visible in")
}

;; Test 2: Shadowing
mut y = 20
{
   mut y = 30
   assert(y == 30, "Inner shadows outer")
}

assert_eq(y, 20, "Outer restored after block")

;; Test 3: Mutation
mut z = 40
{
   z = 50
   assert(z == 50, "Inner modifies outer")
}

assert_eq(z, 50, "Outer keeps modification")

;; Test 4: Nested blocks
mut a = 1
{
   mut b = 2
   {
      mut c = 3
      assert(a == 1, "Nested: outer visible")
      assert(b == 2, "Nested: middle visible")
      assert(c == 3, "Nested: inner visible")
   }
   ;; c should not be visible here, but we can't easily test compilation failure.
   assert(a == 1, "Nested: outer visible in middle")
   assert(b == 2, "Nested: middle visible in middle")
}

print("✓ Scope tests passed")
