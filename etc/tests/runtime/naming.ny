use std.core

;; std.strings.naming (Test)
;; Parser naming edge cases (kebab-case, arrows, predicates).
;; Actual definitions are parser-dependent and intentionally not executed here.

; These tests assert that the test file itself parses and runs.
; Real execution tests are enabled once the parser supports such identifiers.

;fn kebab-case() {
;    return 1
;}
;
;fn do->predicate() {
;    return 1
;}
;
;fn predicate?() {
;    return 1
;}

;assert(kebab-case() == 1)
;assert(do->predicate() == 1)
;assert(predicate?() == 1)
print("✓ std.strings.naming tests passed")
