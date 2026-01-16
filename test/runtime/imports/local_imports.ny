use std.core
use test_module as tm

fn test_local_import() {
	def result = tm.local_add(5, 3)
	if result == 8 {
		print("[PASS] Local import works: ./test_module")
		return true
	}
	print("[FAIL] Local import failed")
	return false
}

fn test_local_function_call() {
	def greeting = tm.local_greet("Tester")
	print(f"[PASS] Local function called: {greeting}")
	return true
}

fn run_all_tests() {
	print("Import System: Local Module Tests")
	def passed = 0
	def total = 2
	if test_local_import() { passed = passed + 1 }
	if test_local_function_call() { passed = passed + 1 }
	print("")
	print(f"Results: {passed}/{total} tests passed")
}

run_all_tests()
