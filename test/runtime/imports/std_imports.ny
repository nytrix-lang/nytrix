use std.core

fn test_std_explicit() {
	print("[PASS] Explicit std import works")
	return true
}

fn run_all_tests() {
	print("Import System: Standard Library Tests")
	def passed = 0
	def total = 1
	if test_std_explicit() { passed = passed + 1 }
	print(f"Results: {passed}/{total} tests passed")
}

run_all_tests()
