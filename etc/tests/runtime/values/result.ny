use std.core.error
use std.core

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

;; Verify tag values through the runtime tag table.
def t_ok = __tagof(r)
def t_err = __tagof(e)
print("TAG_OK=", t_ok, "TAG_ERR=", t_err)
assert(t_ok == __runtime_tag("ok"), "TAG_OK should match runtime")
assert(t_err == __runtime_tag("err"), "TAG_ERR should match runtime")
print("✓ Result allocation logic verification passed")
