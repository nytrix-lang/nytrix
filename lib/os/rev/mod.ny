;; Keywords: reverse decompiler symbolic solver facade
;; Stable composed API over the rev/* implementation package. Use this module
;; when scripts want one reverse stack instead of individual engine modules.
module std.os.rev(
   status, backend_status,
   load, inspect, analyze, triage, report,
   entry, arch, arch_profile, default_target,
   functions, recover_functions, imports, strings,
   lift, cfg, ssa, vsa, facts, function_model, structured, semantic_summary,
   state_machines, type_recovery, pseudocode, decompile, decompile_all,
   solve_decompiled_input, solve_decompile, solve_binary_input,
   solve_bit_sliced_ascii_transform, solve_byte_constraints,
   symbolic_backend_status, z3_available, unicorn_available,
)

use std.core
use std.os.rev.decomp as dc
use std.os.rev.solve as rs
use std.os.rev.symbolic as sym

fn _opts(any opts) dict {
   is_dict(opts) ? opts : dict()
}

fn _opt(any opts, str key, any fallback) any {
   _opts(opts).get(key, fallback)
}

fn _opt_int(any opts, str key, int fallback) int {
   int(_opt(opts, key, fallback))
}

fn status() dict {
   "Return a compact status record for the composed reverse stack."
   {
      "decompiler": dc.tool_status(),
      "symbolic": sym.backend_status(),
      "solver": {
         "module": "std.os.rev.solve",
         "z3": sym.z3_available(),
      },
   }
}

fn backend_status() dict {
   "Alias for status()."
   status()
}

fn symbolic_backend_status() dict {
   "Return the lower-level symbolic backend status."
   sym.backend_status()
}

fn z3_available() bool {
   sym.z3_available()
}

fn unicorn_available() bool {
   sym.unicorn_available()
}

fn load(any source) dict {
   if is_dict(source) { return source }
   dc.load(to_str(source))
}

fn entry(any source) int {
   dc.entry(source)
}

fn arch(any source) str {
   dc.arch(source)
}

fn arch_profile(any source) dict {
   dc.arch_profile(source)
}

fn analyze(any source, any opts=dict()) dict {
   dc.analyze(source, opts)
}

fn triage(any source, any opts=dict()) dict {
   dc.triage(source, opts)
}

fn report(any source, int string_limit=12) str {
   dc.report(source, string_limit)
}

fn functions(any source) list {
   dc.functions(source)
}

fn recover_functions(any source, int scan_bytes=65536) list {
   dc.recover_functions(source, scan_bytes)
}

fn imports(any source) list {
   dc.imports(source)
}

fn strings(any source, int min_len=4, int limit=512) list {
   dc.strings(source, min_len, limit)
}

fn default_target(any source, any target=0, any opts=dict()) any {
   def bin = load(source)
   if !bin.get("ok", false) { return target }
   _solve_target(bin, target, opts)
}

fn inspect(any source, any opts=dict()) dict {
   "Return a compact, one-load summary for scripts that need a quick reverse overview."
   def bin = load(source)
   if !bin.get("ok", false) { return {"ok": false, "analysis": bin, "target": 0} }
   def o = _opts(opts)
   def string_limit = _opt_int(o, "string_limit", 24)
   def min_string = _opt_int(o, "min_string", 4)
   def scan_bytes = _opt_int(o, "scan_bytes", 65536)
   def target = default_target(bin, o.get("target", 0), o)
   def fs = functions(bin)
   def recovered = recover_functions(bin, scan_bytes)
   {
      "ok": true,
      "path": bin.get("path", ""),
      "arch": arch(bin),
      "profile": arch_profile(bin),
      "entry": entry(bin),
      "target": target,
      "function_count": fs.len,
      "recovered_function_count": recovered.len,
      "import_count": imports(bin).len,
      "string_count": strings(bin, min_string, string_limit).len,
      "analysis": bin,
   }
}

fn lift(any source, any target=".text", int max_bytes=1024) list {
   dc.lift(load(source), target, max_bytes)
}

fn cfg(any source, any target=0, int max_bytes=2048) dict {
   dc.cfg(source, target, max_bytes)
}

fn ssa(any source, any target=0, any opts=dict()) dict {
   dc.ssa(source, target, opts)
}

fn vsa(any source, any target=0, any opts=dict()) dict {
   dc.vsa(source, target, opts)
}

fn facts(any source, any target=0, any opts=dict()) dict {
   dc.facts(source, target, opts)
}

fn function_model(any source, any target=0, int max_bytes=1024) dict {
   dc.function_model(source, target, max_bytes)
}

fn structured(any source, any target=0, int max_bytes=1024) dict {
   dc.structured(source, target, max_bytes)
}

fn semantic_summary(any source, any target=0, any opts=dict()) dict {
   dc.semantic_summary(source, target, opts)
}

fn state_machines(any source, any target=0, int max_bytes=2048) list {
   dc.state_machines(source, target, max_bytes)
}

fn type_recovery(any source, any target=0, any opts=dict()) dict {
   dc.type_recovery(source, target, opts)
}

fn pseudocode(any source, any target=0, any opts=dict()) str {
   dc.ny_pseudocode(source, target, opts)
}

fn decompile(any source, any target=0, any opts=dict()) dict {
   dc.decompile(source, target, opts)
}

fn decompile_all(any source, any opts=dict()) dict {
   dc.decompile_all(source, opts)
}

fn solve_decompiled_input(str text, any opts=dict()) dict {
   rs.solve_decompiled_input(text, opts)
}

fn _has_function_named(dict bin, str name) bool {
   def fs = dc.functions(bin)
   mut i = 0
   while i < fs.len {
      if fs[i].get("name", "") == name { return true }
      i += 1
   }
   false
}

fn _solve_target(dict bin, any target, any opts=dict()) any {
   def o = _opts(opts)
   if o.get("raw_entry", false) { return target }
   if o.contains("solve_target") { return o.get("solve_target") }
   if o.contains("target") { return o.get("target") }
   target == 0 && _has_function_named(bin, "main") ? "main" : target
}

fn _solve_opts(any opts) any {
   def solve_opts = _opt(opts, "solve_opts", nil)
   is_dict(solve_opts) ? solve_opts : opts
}

fn solve_decompile(any source, any target=0, any opts=dict()) dict {
   "Decompile a target and run the direct Ny predicate/input solver over the rendered text."
   def bin = load(source)
   if !bin.get("ok", false) {
      return {
         "ok": false,
         "target": target,
         "analysis": bin,
         "decompile": {"ok": false, "text": "", "analysis": bin},
         "solve": {"status": "unknown", "reason": "binary load failed"},
      }
   }
   def dec_target = _solve_target(bin, target, opts)
   def dec = dc.decompile(bin, dec_target, opts)
   if !dec.get("ok", false) {
      return {
         "ok": false,
         "target": dec_target,
         "decompile": dec,
         "solve": {"status": "unknown", "reason": "decompile failed"},
      }
   }
   dec.set("target", dec_target).set("solve", rs.solve_decompiled_input(dec.get("text", ""), _solve_opts(opts)))
}

fn solve_binary_input(any source, any target=0, any opts=dict()) dict {
   "Alias for solve_decompile(...), named for crackme-style input recovery."
   solve_decompile(source, target, opts)
}

fn solve_bit_sliced_ascii_transform(str target, int input_len, int output_len, any opts=dict()) dict {
   rs.solve_bit_sliced_ascii_transform(target, input_len, output_len, opts)
}

fn solve_byte_constraints(any raw_constraints, any opts=dict()) dict {
   rs.solve_byte_constraints(raw_constraints, opts)
}

#main {
   def s = status()
   assert(s.get("solver", dict()).get("module", "") == "std.os.rev.solve", "reverse facade canonical solver")
   assert(is_dict(s.get("decompiler", 0)) && is_dict(symbolic_backend_status()), "reverse facade backend status")
   assert(solve_byte_constraints([], dict()).get("status", "") != "", "reverse facade solver dispatch")
   print("✓ std.os.rev self-test passed")
}
