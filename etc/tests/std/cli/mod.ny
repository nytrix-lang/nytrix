use std.io
use std.cli
use std.core.error

;; std.cli (Test)
;; AOT-safe sanity tests only.
;; No assumptions about flag parsing semantics.

print("Testing std.cli...")

; argc / argv
def n = argc()
assert(n >= 0, "argc >= 0")

if (n > 0) {
   def a0 = argv(0)
   assert(a0 != 0, "argv(0) exists")
}

; args()
def xs = args()
assert(list_len(xs) == n, "args len == argc")

; contains_flag / get_flag must not panic
contains_flag("--anything")
get_flag("--anything", 0)

; parse_args must not panic and must return structure
def test_xs = ["prog", "a", "b"]
def parsed = parse_args(test_xs)

assert(type(parsed) == "dict", "parse_args returns dict")

def flags = get(parsed, "flags")
def pos   = get(parsed, "pos")

assert(type(flags) == "dict", "flags is dict")
assert(type(pos) == "list", "pos is list")

; positional passthrough only
assert(list_len(pos) == 3, "positional len")
assert(get(pos, 0) == "prog", "pos 0")
assert(get(pos, 1) == "a", "pos 1")
assert(get(pos, 2) == "b", "pos 2")

print("✓ std.cli tests passed")
