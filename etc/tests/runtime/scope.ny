use std.core
use std.core.test

print("Testing block scoping...")

;; Test 1: Outer visible in
mut x = 10
{
   t_assert(x == 10, "Outer visible in")
}

;; Test 2: Shadowing
mut y = 20
{
   mut y = 30
   t_assert(y == 30, "Inner shadows outer")
}
t_assert_eq(y, 20, "Outer restored after block")

;; Test 3: Mutation
mut z = 40
{
   z = 50
   t_assert(z == 50, "Inner modifies outer")
}
t_assert_eq(z, 50, "Outer keeps modification")

;; Test 4: Nested blocks
mut a = 1
{
   mut b = 2
   {
      mut c = 3
      t_assert(a == 1, "Nested: outer visible")
      t_assert(b == 2, "Nested: middle visible")
      t_assert(c == 3, "Nested: inner visible")
   }
   ;; c should not be visible here, but we can't easily test compilation failure.
   t_assert(a == 1, "Nested: outer visible in middle")
   t_assert(b == 2, "Nested: middle visible in middle")
}

print("✓ Scope tests passed")
