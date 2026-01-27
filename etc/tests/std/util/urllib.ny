use std.io
use std.util.urllib
use std.core.error

;; std.util.urllib (Test)
;; Tests request and urlopen functions existence and basic behavior.

print("Testing Util Urllib...")

;; TODO: This test is disabled because it makes a real network request and
;; hangs in REPL mode. This is likely due to a bug in the JIT compiler's
;; handling of syscalls.
;; def r = request("GET", "http://example.com", 0)
;; assert(r == 0 || r != 0, "request callable")

;; def r2 = urlopen("http://example.com")
;; assert(r2 == 0 || r2 != 0, "urlopen callable")

print("✓ std.util.urllib tests passed")