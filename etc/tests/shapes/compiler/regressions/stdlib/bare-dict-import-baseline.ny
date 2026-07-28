;; NY-005 baseline: importing std.core first makes the std.core.dict submodule
;; import compile. The known-bug replay uses this to distinguish the bare import
;; diagnostic from a broader broken standard-library setup.
use std.core
use std.core.dict

0
