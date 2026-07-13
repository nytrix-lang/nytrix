;; Keywords: reasoning command macro comptime metadata generated module LSP
;; Ordinary module-defined reasoning commands backed by the syntax registry.
module std.math.logic.commands(
   new_registry, register, standard_registry, run, metadata, names)

use std.core
use std.core.syntax as syntax
use std.math.logic.solve as solve

;; Creates and returns the registry.
fn new_registry() dict {
   {"syntax":syntax.new_registry(), "metadata":dict(8), "order":[]}
}

;; Returns the result of the `register` operation.
fn register(dict registry, str name, any handler, str summary,
   str module_name, str result_kind="certificate-report") dict {
   if name == "" || !handler { panic("reasoning command requires name and handler") }
   def existed = registry.get("metadata").contains(name)
   registry["syntax"] = syntax.register_macro_in(registry.get("syntax"),
      name, handler)
   registry.get("metadata")[name] = {"name":name, "summary":summary,
      "module":module_name, "result_kind":result_kind,
      "kind":"reasoning-command"}
   if !existed { registry["order"] = registry.get("order").append(name) }
   registry
}

fn _arithmetic(any node) any {
   def args = node.get("args", [])
   if args.len != 1 { panic("arithmetic reasoning expects one expression") }
   solve.arithmetic(args[0])
}

fn _congruence(any node) any {
   def args = node.get("args", [])
   if args.len != 3 {
      panic("congruence reasoning expects equalities, left, and right")
   }
   solve.congruence(args[0], args[1], args[2])
}

fn _finite(any node) any {
   def args = node.get("args", [])
   if args.len != 2 { panic("finite reasoning expects domains and predicate") }
   solve.finite(args[0], args[1])
}

fn _linear(any node) any {
   def args = node.get("args", [])
   if args.len != 2 { panic("linear reasoning expects constraints and bounds") }
   solve.linear(args[0], args[1])
}

fn _induction(any node) any {
   def args = node.get("args", [])
   if args.len != 3 { panic("induction reasoning expects predicate, first, last") }
   solve.induction(args[0], args[1], args[2])
}

;; Returns the result of the `standard_registry` operation.
fn standard_registry() dict {
   mut registry = new_registry()
   registry = register(registry, "arithmetic", _arithmetic,
      "Normalize a closed arithmetic term with a bounded certificate.",
      "std.math.logic.solve")
   registry = register(registry, "congruence", _congruence,
      "Close bounded term equalities by congruence.",
      "std.math.logic.solve")
   registry = register(registry, "finite", _finite,
      "Search explicitly bounded finite domains.", "std.math.logic.solve")
   registry = register(registry, "linear", _linear,
      "Search bounded integer linear constraints.", "std.math.logic.solve")
   register(registry, "induction", _induction,
      "Check a bounded induction interval.", "std.math.logic.solve")
}

;; Returns the result of the `run` operation.
fn run(dict registry, str name, list args) any {
   syntax.expand_macro_in(registry.get("syntax"), name, args)
}

;; Returns the result of the `metadata` operation.
fn metadata(dict registry, str name="") any {
   if name != "" { return registry.get("metadata").get(name, nil) }
   mut out = []
   mut i = 0
   while i < registry.get("order").len {
      out = out.append(registry.get("metadata").get(
         registry.get("order")[i]))
      i += 1
   }
   out
}

fn names(dict registry) list { registry.get("order") }
