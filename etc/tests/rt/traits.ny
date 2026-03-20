use std.core
use std.core.iter

fn trait_numeric(numeric: x): number { x + 1 }

fn trait_sequence(sequence: xs): int { count(xs) }

fn trait_indexable(indexable: xs): any { get(xs, 0) }

fn trait_iterable(iterable: xs): int { len(xs) }

fn trait_allocator(allocator: p): allocator { p }
assert(trait_numeric(41) == 42, "numeric static capability")
assert(trait_sequence([1, 2, 3]) == 3, "sequence list capability")
assert(trait_sequence("abc") == 3, "sequence string capability")
assert(trait_indexable([9, 8]) == 9, "indexable capability")
assert(trait_iterable([1, 2]) == 2, "iterable capability")
mut direct_count_xs = [4, 5, 6]
assert(count(direct_count_xs) == 3, "count fast path for known list")
assert(count("fast") == 4, "count fast path for known string")
