;; Keywords: core primitives test
;; Unit tests for std.core.primitives

module std.core.primitives_test

if(comptime{__main()}){
    use std.core.primitives *
    use std.core.test *

    assert((1 + 2) == 3, "add")
    assert((5 - 2) == 3, "sub")
    assert((3 * 4) == 12, "mul")
    assert((10 / 2) == 5, "div")
    assert((10 % 3) == 1, "mod")

    assert((1 == 1), "eq true")
    assert(!(1 == 2), "eq false")
    assert((2 > 1), "gt")
    assert((1 < 2), "lt")
    assert((1 >= 1), "ge")
    assert((1 <= 1), "le")

    assert((5 & 3) == 1, "band")
    assert((4 | 2) == 6, "bor")
    assert((5 ^ 3) == 6, "bxor")
    assert((1 << 2) == 4, "bshl")
    assert((4 >> 1) == 2, "bshr")

    assert(is_int(123), "is_int")
    assert(!is_int("s"), "is_int str")

    assert(is_none(0), "is_none")
    assert(!is_none(1), "is_none int")

    print("✓ std.core.primitives tests passed")
}
