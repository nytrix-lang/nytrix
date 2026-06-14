use std.core.str
use std.core.reflect

;; Perform the embed on itself to avoid external fixture dependencies
def RES = embed("etc/tests/rt/embed.ny")

if RES.len > 0 && RES.contains("embed test passed") {
   print("✓ embed test passed")
} else {
   print("✗ embed test failed")
   if RES.len == 0 {
      print("  Error: RES is empty")
   } else {
      print("  Error: 'embed test passed' not found in RES")
   }
}
