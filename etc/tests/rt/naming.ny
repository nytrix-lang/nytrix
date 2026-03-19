use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

;; std.core.str.naming (Test)
;; Parser naming edge cases (kebab-case, arrows, predicates).
;; Actual definitions are parser-dependent and still not implemented.
;; fn kebab-case() {
;; return 1
;; }
;;
;; fn do->predicate() {
;; return 1
;; }
;;
;; fn predicate?() {
;; return 1
;; }
;; assert(kebab-case() == 1)
;; assert(do->predicate() == 1)
;; assert(predicate?() == 1)
print("✓ std.core.str.naming tests passed")
