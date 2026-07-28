use std.core
use "./local.ny"

assert(helper_val() == 123, "core profile imports helper_val")
assert(helper_add(3, 4) == 7, "core profile imports helper_add")

;; The debug profile is intentionally not imported here; explicit debug import is covered in use.ny.
assert(to_str([1, 2, 3].long) == "66051", "std.core exposes list .long")
assert(to_str("ABC".long) == "4276803", "std.core exposes str .long")
assert(to_str("010203".unhex.long) == "66051", "std.core exposes unhex .long")
assert(to_str("ABC".to_bytes.long) == "4276803", "std.core exposes to_bytes .long")
assert(to_str(123.long) == "123", "std.core exposes int .long through any")
mut bytes raw = bytes(3)
raw = bytes_set(raw, 0, 1)
raw = bytes_set(raw, 1, 2)
raw = bytes_set(raw, 2, 3)
assert(bytes_get(raw, 2) == 3, "std.core exposes typed bytes_get")
assert(to_str(raw.to_list.long) == "66051", "std.core exposes native bytes .to_list.long")
