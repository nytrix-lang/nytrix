;; NY-005: bare std.core.dict import fails unless std.core is imported first.
;;
;; Current local bad signal on 2026-06-01:
;;   Error: Could not load std.ny or standard library source files.
;;
;; Baseline that should pass:
;;   etc/tests/fuzz/errors/stdlib/bare-dict-import-baseline.ny

use std.core.dict
0
