use std.core
use test_module_all *

fn test_import_all() {
	def sum = test_module_all.all_add(2, 3)
	def ok1 = sum == 5
	def ok2 = all_value == 7
	if ok1 && ok2 {
		print("[PASS] use module * imports functions and vars")
		return true
	}
	print("[FAIL] use module * failed")
	return false
}

fn run_all_tests() {
	print("Import System: use module *")
	def passed = 0
	def total = 1
	if test_import_all() { passed = passed + 1 }
	print("")
	print(f"Results: {passed}/{total} tests passed")
}

run_all_tests()
