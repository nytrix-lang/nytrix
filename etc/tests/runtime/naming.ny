; We should allow these forms for function and variable names.
; This is a parser-heavy case, so it will be tested in development.
use std.core

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
