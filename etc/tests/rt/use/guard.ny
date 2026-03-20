use std.core

mut runtime_main_seen = false
def comptime_main_seen = comptime{ return __main() }

if(__main()){
   runtime_main_seen = true
}

fn main_guard_runtime_seen(): bool {
   runtime_main_seen
}

fn main_guard_comptime_seen(): bool {
   comptime_main_seen
}
