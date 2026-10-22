;; Keywords: syntax builtin core
;; Core Syntax builtins.
;; References:
;; - core.compiler.syntax
;; - core
module core.compiler.syntax.builtin(register_defaults, attr_extern, attr_naked, attr_jit, attr_thread, attr_pure, attr_cache, attr_effects, attr_backend)
use core
use core (is_str as core_is_str)
use core.dict_mod
use core.reflect as core_ref
use core.compiler.syntax.syntax as syntax_impl

fn _ensure_argc(name, args, want) {
   if args.len != want { panic("@" + name + " expects " + to_str(want) + " argument(s)") }
   0
}

fn _set_flag(any node, str key, any value=true) any {
   if !is_dict(node) { return node }
   node.set(key, value)
}

fn attr_extern(node, args) {
   "Builtin @extern metadata transform."
   _ensure_argc("extern", args, 1)
   if !core_is_str(args[0]) { panic("@extern argument must be string") }
   node = _set_flag(node, "is_extern", true)
   node = _set_flag(node, "link_name", args[0])
   node
}

fn attr_naked(node, args) {
   "Builtin @naked metadata transform."
   _ensure_argc("naked", args, 0)
   _set_flag(node, "attr_naked", true)
}

fn attr_jit(node, args) {
   "Builtin @jit metadata transform."
   _ensure_argc("jit", args, 0)
   _set_flag(node, "attr_jit", true)
}

fn attr_thread(node, args) {
   "Builtin @thread metadata transform."
   _ensure_argc("thread", args, 0)
   _set_flag(node, "attr_thread", true)
}

fn attr_pure(node, args) {
   "Builtin @pure metadata transform."
   _ensure_argc("pure", args, 0)
   _set_flag(node, "attr_pure", true)
}

fn attr_cache(node, args) {
   "Builtin @cache metadata transform."
   _ensure_argc("cache", args, 0)
   _set_flag(node, "attr_cache", true)
}

fn attr_effects(node, args) {
   "Builtin @effects metadata transform."
   if args.len == 0 { panic("@effects expects at least one argument") }
   node = _set_flag(node, "effect_contract_known", true)
   node = _set_flag(node, "effect_contract", args)
   node
}

fn attr_backend(node, args) {
   "Builtin backend metadata transform."
   if args.len < 1 || args.len > 2 { panic("@backend expects 1 or 2 argument(s)") }
   node = _set_flag(node, "backend_attr", args[0])
   if args.len == 2 { node = _set_flag(node, "backend_value", args[1]) }
   node
}

fn register_defaults(dict reg) dict {
   "Registers default std syntax handlers in `reg`."
   ; Keep the handlers as top-level function values.  Native lowering can
   ; represent those directly; nested wrapper closures may be materialized as
   ; nil when this module is loaded through the stdlib cache.
   reg = syntax_impl.register_attribute(
      reg, "extern", __mark_dynamic_callable_tagged_args(attr_extern))
   reg = syntax_impl.register_attribute(
      reg, "naked", __mark_dynamic_callable_tagged_args(attr_naked))
   reg = syntax_impl.register_attribute(
      reg, "jit", __mark_dynamic_callable_tagged_args(attr_jit))
   reg = syntax_impl.register_attribute(
      reg, "thread", __mark_dynamic_callable_tagged_args(attr_thread))
   reg = syntax_impl.register_attribute(
      reg, "pure", __mark_dynamic_callable_tagged_args(attr_pure))
   reg = syntax_impl.register_attribute(
      reg, "cache", __mark_dynamic_callable_tagged_args(attr_cache))
   reg = syntax_impl.register_attribute(
      reg, "effects", __mark_dynamic_callable_tagged_args(attr_effects))
   reg = syntax_impl.register_attribute(
      reg, "backend", __mark_dynamic_callable_tagged_args(attr_backend))
   reg
}

#main {
   mut reg = syntax_impl.new_registry(8)
   reg = register_defaults(reg)
   assert(syntax_impl.is_attr_registered(reg, "extern"), "syntax builtin extern")
   assert(syntax_impl.is_attr_registered(reg, "effects"), "syntax builtin effects")
   def ex = syntax_impl.apply_attribute(reg, "extern", dict(2), ["puts"])
   assert(ex.get("is_extern", false), "syntax builtin extern flag")
   assert(ex.get("link_name", "") == "puts", "syntax builtin extern link")
   def pure = syntax_impl.apply_attribute(reg, "pure", dict(2), list(0))
   assert(pure.get("attr_pure", false), "syntax builtin pure")
   def cached = syntax_impl.apply_attribute(reg, "cache", dict(2), list(0))
   assert(cached.get("attr_cache", false), "syntax builtin cache")
   def fx = syntax_impl.apply_attribute(reg, "effects", dict(2), ["io"])
   assert(fx.get("effect_contract_known", false), "syntax builtin effects flag")
   assert(fx.get("effect_contract", list(0)).len == 1, "syntax builtin effects list")
   def be = syntax_impl.apply_attribute(reg, "backend", dict(2), ["alwaysinline", 1])
   assert(be.get("backend_attr", "") == "alwaysinline", "syntax builtin backend attr")
   assert(be.get("backend_value", 0) == 1, "syntax builtin backend value")
   print("✓ core.compiler.syntax.builtin self-test passed")
}
