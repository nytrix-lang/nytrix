use std.io.fmt
use std.core.error

fn test_format(){
   print("Testing format...")
   def s = format("Hello {}!", "World")
   assert(eq(s, "Hello World!"), "format string")
   s = format("{} + {} = {}", 1, 2, 3)
   assert(eq(s, "1 + 2 = 3"), "format ints")
   print("Format passed")
}

fn test_main(){
   test_format()
   print("✓ std.io.fmt passed")
}

test_main()
