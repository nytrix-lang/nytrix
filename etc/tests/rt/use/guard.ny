use std.core

mut runtime_main_seen = false
mut hash_main_seen = false
def comptime_main_seen = comptime{ return __main() }

#main {
   runtime_main_seen = true
}

#main {
   hash_main_seen = true
}

fn main_guard_runtime_seen() bool {
   runtime_main_seen
}

fn main_guard_comptime_seen() bool {
   comptime_main_seen
}

fn hash_main_guard_seen() bool {
   hash_main_seen
}

#main {
   assert(runtime_main_seen, "runtime __main() is true in direct file")
   assert(comptime_main_seen, "comptime __main() is true in direct file")
   assert(hash_main_seen, "#main guard is true in direct file")
   print("✓ main guard tests passed")
}
