use std.core
use test_module (local_add as add2, local_greet)

fn test_import_list() {
	def sum = add2(10, 5)
	if sum != 15 {
		print("[FAIL] use module (list) failed")
		return false
	}
	def greeting = local_greet("Tester")
	print(f"[PASS] import list rename works: {greeting}")
	return true
}

fn run_all_tests() {
	print("Import System: use module (list)")
	def passed = 0
	def total = 1
	if test_import_list() { passed = passed + 1 }
	print("")
	print(f"Results: {passed}/{total} tests passed")
}

run_all_tests()
