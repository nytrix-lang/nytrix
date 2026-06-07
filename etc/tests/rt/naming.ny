use std.core

fn snake_case_name() int { 1 }
fn _leading_underscore2() int { 2 }

def value_123 = snake_case_name() + _leading_underscore2()
assert(value_123 == 3, "identifier names allow underscores and digit tails")
print("✓ naming tests passed")
