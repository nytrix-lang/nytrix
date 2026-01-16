use std.core

fn test_print_empty() {
	print()
	return true
}

fn test_print_basic() {
	print("Basic")
	return true
}

fn test_print_multi() {
	print("Vals:", 1, 2, 3)
	return true
}

fn test_print_kwargs() {
	print("A", "B", sep="-", end=".\n")
	return true
}

fn test_print_only_kwarg() {
	print(end="[END]\n")
	return true
}

fn test_main() {
	print("--- Test Suite: Print ---")
	test_print_empty()
	test_print_basic()
	test_print_multi()
	test_print_kwargs()
	test_print_only_kwarg()
	print("✓ Print tests passed")
}

test_main()
