;; flags: -emit-only

fn _ny_regress_emit_only_self_recursive_fastpath() {
   _ny_regress_emit_only_self_recursive_fastpath()
}

fn _ny_regress_emit_only_self_recursive_fastpath_return(): int {
   return _ny_regress_emit_only_self_recursive_fastpath_return()
}
