module DeclaredModuleProvider(
   make_report, make_report_any
)

use std.core

fn make_report() dict {
   mut out = dict(4)
   out = out.set("ok", true)
   out
}

fn make_report_any() any {
   mut out = dict(4)
   out = out.set("ok", true)
   out
}
