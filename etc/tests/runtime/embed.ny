use std.str *
use std.core.reflect *

;; Perform the embed on itself to avoid external fixture dependencies
def RES = embed("etc/tests/runtime/embed.ny")

if(len(RES) > 0 && str_contains(RES, "embed test passed")){
    print("✓ embed test passed")
} else {
    print("✗ embed test failed")
    if(len(RES) == 0){
        print("  Error: RES is empty")
    } else {
        print("  Error: 'embed test passed' not found in RES")
    }
}
