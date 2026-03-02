use std.core.error *
use std.core *

print("Verifying Result allocation logic...")

;; Test Ok creation and unwrapping
def val = 42
def r = ok(val)

;; Check basics
assert(is_ok(r), "ok(42) should be is_ok")
assert(!is_err(r), "ok(42) should not be is_err")
assert_eq(unwrap(r), 42, "unwrap(ok(42)) should be 42")

;; Test Err creation and unwrapping
def msg = "some error"
def e = err(msg)
assert(is_err(e), "err(msg) should be is_err")
assert(!is_ok(e), "err(msg) should not be is_ok")

;; Test unwrap_or
assert_eq(unwrap_or(r, 0), 42, "unwrap_or(ok, 0) should be 42")
assert_eq(unwrap_or(e, 0), 0, "unwrap_or(err, 0) should be 0")

;; Verify Tag values manually using internal knowledge
;; TAG_OK is 104, TAG_ERR is 105 (raw integer values returned by __tagof)

def t_ok = __tagof(r)
def t_err = __tagof(e)

print("TAG_OK=", t_ok, "TAG_ERR=", t_err)

assert(t_ok == 104, "TAG_OK should be 104")
assert(t_err == 105, "TAG_ERR should be 105")

print("✓ Result allocation logic verification passed")
