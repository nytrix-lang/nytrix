;; Keywords: syntax builtin
;; Core Syntax builtins.
module std.core.syntax.builtin(register_defaults)
use std.core
use std.core.dict_mod
use std.core.reflect as core_ref
use std.core.syntax.syntax as syntax_impl

fn _ensure_argc(str: name, list: args, int: want): int {
   if(args.len != want){ panic("@" + name + " expects " + to_str(want) + " argument(s)") }
   0
}

fn _set_flag(any: node, str: key, any: value=true): any {
   if(!is_dict(node)){ return node }
   node.set(key, value)
}

fn attr_extern(any: node, list: args): any {
   "Builtin @extern metadata transform."
   _ensure_argc("extern", args, 1)
   if(!is_str(args[0])){ panic("@extern argument must be string") }
   node = _set_flag(node, "is_extern", true)
   node = _set_flag(node, "link_name", args[0])
   node
}

fn attr_naked(any: node, list: args): any {
   "Builtin @naked metadata transform."
   _ensure_argc("naked", args, 0)
   _set_flag(node, "attr_naked", true)
}

fn attr_jit(any: node, list: args): any {
   "Builtin @jit metadata transform."
   _ensure_argc("jit", args, 0)
   _set_flag(node, "attr_jit", true)
}

fn attr_thread(any: node, list: args): any {
   "Builtin @thread metadata transform."
   _ensure_argc("thread", args, 0)
   _set_flag(node, "attr_thread", true)
}

fn attr_pure(any: node, list: args): any {
   "Builtin @pure metadata transform."
   _ensure_argc("pure", args, 0)
   _set_flag(node, "attr_pure", true)
}

fn attr_cache(any: node, list: args): any {
   "Builtin @cache metadata transform."
   _ensure_argc("cache", args, 0)
   _set_flag(node, "attr_cache", true)
}

fn attr_effects(any: node, list: args): any {
   "Builtin @effects metadata transform."
   if(args.len == 0){ panic("@effects expects at least one argument") }
   node = _set_flag(node, "effect_contract_known", true)
   node = _set_flag(node, "effect_contract", args)
   node
}

fn attr_llvm(any: node, list: args): any {
   "Builtin @llvm metadata transform."
   if(args.len < 1 || args.len > 2){ panic("@llvm expects 1 or 2 argument(s)") }
   node = _set_flag(node, "llvm_attr", args[0])
   if(args.len == 2){ node = _set_flag(node, "llvm_value", args[1]) }
   node
}

fn register_defaults(dict: reg): dict {
   "Registers default std syntax handlers in `reg`."
   reg = syntax_impl.register_attribute(reg, "extern", attr_extern)
   reg = syntax_impl.register_attribute(reg, "naked", attr_naked)
   reg = syntax_impl.register_attribute(reg, "jit", attr_jit)
   reg = syntax_impl.register_attribute(reg, "thread", attr_thread)
   reg = syntax_impl.register_attribute(reg, "pure", attr_pure)
   reg = syntax_impl.register_attribute(reg, "cache", attr_cache)
   reg = syntax_impl.register_attribute(reg, "effects", attr_effects)
   reg = syntax_impl.register_attribute(reg, "llvm", attr_llvm)
   reg
}
