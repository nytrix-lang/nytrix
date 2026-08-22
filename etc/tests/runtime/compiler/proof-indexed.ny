fn require_positive(proof<5 > 0> p) int { 1 }
def proof positive = prove(5 > 0)
assert(require_positive(positive) == 1, "indexed compile-time proof")

fn require_equal(proof<left == right> p) int { 2 }
def left = 7
def right = 7
def proof equality = prove(right == left)
assert(require_equal(equality) == 2, "equality proof normalization")
