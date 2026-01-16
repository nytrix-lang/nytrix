use std.core
use std.io

fn test_modulus_with_inner_parens() {
	def i = 15
	if (i + 1) % 16 == 0 {
		print("[PASS] Modulus with inner parentheses: (i + 1) % 16 == 0")
		return true
	}
	print("[FAIL] Modulus with inner parentheses")
	return false
}

fn test_modulus_with_outer_parens() {
	def i = 15
	if ((i + 1) % 16 == 0) {
		print("[PASS] Modulus with outer parentheses: ((i + 1) % 16 == 0)")
		return true
	}
	print("[FAIL] Modulus with outer parentheses")
	return false
}

fn test_modulus_without_parens() {
	def i = 16
	if i % 8 == 0 {
		print("[PASS] Modulus without parentheses: i % 8 == 0")
		return true
	}
	print("[FAIL] Modulus without parentheses")
	return false
}

fn test_complex_expression() {
	def x = 10
	def y = 5
	if (x + y) * 2 % 10 == 0 {
		print("[PASS] Complex expression: (x + y) * 2 % 10 == 0")
		return true
	}
	print("[FAIL] Complex expression")
	return false
}

fn run_all_tests() {
	print("Parser Operator Precedence Tests")
	def passed = 0
	def total = 4
	if test_modulus_with_inner_parens() { passed = passed + 1 }
	if test_modulus_with_outer_parens() { passed = passed + 1 }
	if test_modulus_without_parens() { passed = passed + 1 }
	if test_complex_expression() { passed = passed + 1 }
	print("")
	print(f"Results: {passed}/{total} tests passed")
	if passed == total {
		print("✓ All tests PASSED")
	} else {
		print(f"✗ {total - passed} tests FAILED")
	}
}

run_all_tests()
