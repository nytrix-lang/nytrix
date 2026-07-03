;; Keywords: syntax builtin core
;; Core Syntax builtins.
;; References:
;; - std.core.syntax
;; - std.core
module std.core.syntax.builtin(register_defaults)
use std.core
use std.core.dict_mod
use std.core.reflect as core_ref
use std.core.syntax.syntax as syntax_impl

fn _ensure_argc(str name, list args, int want) int {
   if args.len != want { panic("@" + name + " expects " + to_str(want) + " argument(s)") }
   0
}

fn _set_flag(any node, str key, any value=true) any {
   if !is_dict(node) { return node }
   node.set(key, value)
}

fn attr_extern(any node, list args) any {
   "Builtin @extern metadata transform."
   _ensure_argc("extern", args, 1)
   if !is_str(args[0]) { panic("@extern argument must be string") }
   node = _set_flag(node, "is_extern", true)
   node = _set_flag(node, "link_name", args[0])
   node
}

fn attr_naked(any node, list args) any {
   "Builtin @naked metadata transform."
   _ensure_argc("naked", args, 0)
   _set_flag(node, "attr_naked", true)
}

fn attr_jit(any node, list args) any {
   "Builtin @jit metadata transform."
   _ensure_argc("jit", args, 0)
   _set_flag(node, "attr_jit", true)
}

fn attr_thread(any node, list args) any {
   "Builtin @thread metadata transform."
   _ensure_argc("thread", args, 0)
   _set_flag(node, "attr_thread", true)
}

fn attr_pure(any node, list args) any {
   "Builtin @pure metadata transform."
   _ensure_argc("pure", args, 0)
   _set_flag(node, "attr_pure", true)
}

fn attr_cache(any node, list args) any {
   "Builtin @cache metadata transform."
   _ensure_argc("cache", args, 0)
   _set_flag(node, "attr_cache", true)
}

fn attr_effects(any node, list args) any {
   "Builtin @effects metadata transform."
   if args.len == 0 { panic("@effects expects at least one argument") }
   node = _set_flag(node, "effect_contract_known", true)
   node = _set_flag(node, "effect_contract", args)
   node
}

fn attr_backend(any node, list args) any {
   "Builtin backend metadata transform."
   if args.len < 1 || args.len > 2 { panic("@backend expects 1 or 2 argument(s)") }
   node = _set_flag(node, "backend_attr", args[0])
   if args.len == 2 { node = _set_flag(node, "backend_value", args[1]) }
   node
}

fn register_defaults(dict reg) dict {
   "Registers default std syntax handlers in `reg`."
   reg = syntax_impl.register_attribute(reg, "extern", attr_extern)
   reg = syntax_impl.register_attribute(reg, "naked", attr_naked)
   reg = syntax_impl.register_attribute(reg, "jit", attr_jit)
   reg = syntax_impl.register_attribute(reg, "thread", attr_thread)
   reg = syntax_impl.register_attribute(reg, "pure", attr_pure)
   reg = syntax_impl.register_attribute(reg, "cache", attr_cache)
   reg = syntax_impl.register_attribute(reg, "effects", attr_effects)
   reg = syntax_impl.register_attribute(reg, "backend", attr_backend)
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
   print("✓ std.core.syntax.builtin self-test passed")
}
