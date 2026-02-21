;; Keywords: core syntax
;; Core Syntax builtins.

module std.core.syntax.builtin (
   register_defaults
)
use std.core *
use std.core.dict *
use std.core.reflect as core_ref
use std.core.syntax.syntax as syntax_impl

fn _ensure_argc(name, args, want){
   "Internal helper."
   if(core_ref.len(args) != want){
      panic("@" + name + " expects " + to_str(want) + " argument(s)")
   }
   0
}

fn _set_flag(node, key, value=true){
   "Internal helper."
   if(!is_dict(node)){ return node }
   dict_set(node, key, value)
}

fn attr_extern(node, args){
   "Builtin @extern metadata transform."
   _ensure_argc("extern", args, 1)
   if(!is_str(args[0])){ panic("@extern argument must be string") }
   node = _set_flag(node, "is_extern", true)
   node = _set_flag(node, "link_name", args[0])
   node
}

fn attr_naked(node, args){
   "Builtin @naked metadata transform."
   _ensure_argc("naked", args, 0)
   _set_flag(node, "attr_naked", true)
}

fn attr_jit(node, args){
   "Builtin @jit metadata transform."
   _ensure_argc("jit", args, 0)
   _set_flag(node, "attr_jit", true)
}

fn attr_thread(node, args){
   "Builtin @thread metadata transform."
   _ensure_argc("thread", args, 0)
   _set_flag(node, "attr_thread", true)
}

fn attr_pure(node, args){
   "Builtin @pure metadata transform."
   _ensure_argc("pure", args, 0)
   _set_flag(node, "attr_pure", true)
}

fn attr_effects(node, args){
   "Builtin @effects metadata transform."
   if(core_ref.len(args) == 0){
      panic("@effects expects at least one argument")
   }
   node = _set_flag(node, "effect_contract_known", true)
   node = _set_flag(node, "effect_contract", args)
   node
}

fn attr_llvm(node, args){
   "Builtin @llvm metadata transform."
   if(core_ref.len(args) < 1 || core_ref.len(args) > 2){
      panic("@llvm expects 1 or 2 argument(s)")
   }
   node = _set_flag(node, "llvm_attr", args[0])
   if(core_ref.len(args) == 2){
      node = _set_flag(node, "llvm_value", args[1])
   }
   node
}

fn register_defaults(reg){
   "Registers default std syntax handlers in `reg`."
   reg = syntax_impl.register_attribute(reg, "extern", attr_extern)
   reg = syntax_impl.register_attribute(reg, "naked", attr_naked)
   reg = syntax_impl.register_attribute(reg, "jit", attr_jit)
   reg = syntax_impl.register_attribute(reg, "thread", attr_thread)
   reg = syntax_impl.register_attribute(reg, "pure", attr_pure)
   reg = syntax_impl.register_attribute(reg, "effects", attr_effects)
   reg = syntax_impl.register_attribute(reg, "llvm", attr_llvm)
   reg
}

if(comptime{__main()}){

    mut reg = syntax_impl.new_registry(8)
    reg = register_defaults(reg)
    assert(syntax_impl.is_attr_registered(reg, "extern"), "register extern")
    assert(syntax_impl.is_attr_registered(reg, "effects"), "register effects")

    def ex = attr_extern(dict(2), ["puts"])
    assert(dict_get(ex, "is_extern", false), "attr_extern flag")
    assert(dict_get(ex, "link_name", "") == "puts", "attr_extern link name")

    def pure = attr_pure(dict(2), list(0))
    assert(dict_get(pure, "attr_pure", false), "attr_pure flag")

    def fx = attr_effects(dict(2), ["io"])
    assert(dict_get(fx, "effect_contract_known", false), "attr_effects known")
    def contract = dict_get(fx, "effect_contract", list(0))
    assert(is_list(contract), "attr_effects list")
    assert(len(contract) == 1, "attr_effects len")

    def ll = attr_llvm(dict(2), ["alwaysinline", 1])
    assert(dict_get(ll, "llvm_attr", "") == "alwaysinline", "attr_llvm name")
    assert(dict_get(ll, "llvm_value", 0) == 1, "attr_llvm value")
}
