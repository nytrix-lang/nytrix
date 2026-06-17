;; NY-006: std.os.args.argv out-of-range used to segfault instead of returning
;; a safe empty/null-like result.
;;
;; Historical bad signal:
;;   ny --compiler-asserts -c $'use std.core\nuse std.os.args\nprint(argv(2))'
;; exited with SIGSEGV when argc() == 1.
;;
;; Current local retest on 2026-06-01 exits 0 and prints an empty line, so this
;; is tracked as a fixed-candidate regression guard.
use std.core
use std.os.args

print(argv(argc() + 1))
