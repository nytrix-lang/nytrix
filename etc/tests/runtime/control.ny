use std.core
use std.core.test

fn test_break_semantics() {
   print("Testing break semantics...")
   ; Test 1: Simple break
   def i = 0
   while (i < 10) {
      if (i == 5) { break }
      i = i + 1
   }
   assert(i == 5, "simple break")
   ; Test 2: Break with variable update before it
   i = 0
   def flag = 0
   while (i < 10) {
      if (i == 5) {
         flag = 1
         break
      }
      i = i + 1
   }
   assert(flag == 1, "var update before break")
   assert(i == 5, "break stops loop")
   ; Test 3: Nested loops
   i = 0
   def j = 0
   def count = 0
   while (i < 3) {
      j = 0
      while (j < 3) {
         if (j == 1) { break } ; Should break inner only
         count = count + 1
         j = j + 1
      }
      i = i + 1
   }
   assert(count == 3, "nested break inner")
}

fn test_break_bug_repro() {
   print("Testing break/assignment bug repro...")
   def m = 1
   def matches = 0
   def i = 0
   while (i < 6) {
      def is_match = 1
      def j = 0
      while (j < 1) {
         def char_match = 0
         if (i == 1) { char_match = 1 }
         if (i == 3) { char_match = 1 }
         if (i == 5) { char_match = 1 }
         if (char_match == 0) {
            is_match = 0
            break
         }
         j = j + 1
      }
      if (is_match == 1) {
         matches = matches + 1
         i = i + m
      } else {
         i = i + 1
      }
   }
   assert(matches == 3, "match count simulation")
}

fn test_main() {
   test_break_semantics()
   test_break_bug_repro()
   print("✓ Runtime control flow tests passed")
}

test_main()

print("Testing paren-less control flow...")

fn test_if_paren(){
   if (0) { panic("if(0) taken") } else { print("if(0) else works") }
   if (1) { print("if(1) works") } else { panic("if(1) else taken") }
}

fn test_if_noparen(){
   if 0 { print("if 0 else") } else { print("if 0 else works") }
   if 1 { print("if 1 works") } else { panic("if 1 else taken") }
}

fn test_while_paren(){
   def i = 0
   while (i < 3) { i = i + 1 }
   assert(i == 3, "while paren")
}

fn test_while_noparen(){
   def i = 0
   while i < 3 { i = i + 1 }
   assert(i == 3, "while noparen")
}

fn test_for_paren(){
   def s = 0
   for (x in [1,2,3]) { s = s + x }
   assert(s == 6, "for paren")
}

fn test_for_noparen(){
   def s = 0
   for x in [1,2,3] { s = s + x }
   assert(s == 6, "for noparen")
}

test_if_paren()
test_if_noparen()
test_while_paren()
test_while_noparen()
test_for_paren()
test_for_noparen()

print("\u2713 paren-less control flow passed")
