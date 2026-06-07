use std.core

mut xs = [4, 1, 9]
assert(__store_item(xs, 1, 7) == 7, "__store_item returns stored value")
assert(xs == [4, 7, 9], "__store_item mutates list")

xs = __list_reserve(xs, 32)
assert(xs.len == 3, "__list_reserve keeps length")
assert(xs == [4, 7, 9], "__list_reserve keeps values")
assert(__list_sum_int_range(xs, 0, xs.len) == 20, "__list_sum_int_range sums full list")
assert(__list_sum_int_range(xs, -4, 2) == 11, "__list_sum_int_range clamps negative start")
assert(__list_sum_int_range(xs, 2, 99) == 9, "__list_sum_int_range clamps stop")

mut sortable = [9, 4, 7, 1]
assert(__sort_list(sortable) == [1, 4, 7, 9], "__sort_list returns sorted list")
assert(sortable == [1, 4, 7, 9], "__sort_list mutates list")

mut d = dict(1)
d = __dict_reserve(d, 12)
d = __dict_write_fast(d, "a", 10)
d = __dict_write_fast(d, "b", 20)
d = __dict_write_fast(d, "a", 30)
assert(d.get("a", 0) == 30, "__dict_write_fast overwrites existing key")
assert(d.get("b", 0) == 20, "__dict_write_fast inserts key")
assert(load64(d, 0) == 2, "__dict_write_fast keeps dict cardinality")

print("✓ runtime container tests passed")
