use std.core
use std.math.random as rnd

mut xs = [10, 20, 30]
def r = rnd.randrange(1, 5)
assert(r >= 1 && r < 5, "randrange keeps scalar bounds")
def picked = rnd.choice(xs)
assert(picked == 10 || picked == 20 || picked == 30, "choice returns an element from the list")
xs = rnd.shuffle(xs)
assert(xs.len == 3, "shuffle preserves list length")
print("✓ random scalar tests passed")
