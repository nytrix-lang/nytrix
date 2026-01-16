use std.core

fn test_while_strict() {
	print("Testing while strict syntax...")
	def i = 0
	while (i < 3) {
		i = i + 1
	}
	assert(i == 3, "strict while")
	print("✓ while strict passed")
}

fn test_for_strict() {
	print("Testing for strict syntax...")
	def sum = 0
	def list_vals = [1, 2, 3]
	for (x in list_vals) {
		sum = sum + x
	}
	assert(sum == 6, "strict for")
	print("✓ for strict passed")
}

test_while_strict()
test_for_strict()
print("✓ Loop strict syntax passed")
