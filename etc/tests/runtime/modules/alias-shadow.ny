use std.core
use std.core as core

fn local_alias_shadow() {
   def core = [17, 23]
   assert(core.get(0) == 17, "local list shadows module alias")
   assert(core.get(1) == 23, "local list receiver is preserved")
}

fn parameter_alias_shadow(list core) {
   assert(core.get(0) == 31, "parameter shadows module alias")
}

local_alias_shadow()
parameter_alias_shadow([31])
assert(core.len([1, 2]) == 2, "module alias remains usable outside shadow")
