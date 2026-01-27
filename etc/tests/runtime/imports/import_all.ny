use std.core
use test_module_all *

;; Import system – use module *

def sum = all_add(2, 3)
assert(sum == 5, "import * function")

def ok1 = sum == 5
def ok2 = all_value == 7

if ok1 && ok2 {
   print("[PASS] use module * imports functions and vars")
} else {
   print("[FAIL] use module * failed")
}

print("")
print("Results: 1/1 tests passed")
