;; Keywords: syntax builtin type core
;; Core syntax facade for built-in syntax tables and tokenizer entry points.
;; References:
;; - std.core
module std.core.syntax(new_registry, registry, reset_registry, clone_registry, clone_registry_in, merge_registry, merge_registry_in, register_macro, register_macro_in, unregister_macro, unregister_macro_in, register_attribute, register_attribute_in, unregister_attribute, unregister_attribute_in, get_macro_handler, get_macro_handler_in, get_attr_handler, get_attr_handler_in, is_macro_registered, is_macro_registered_in, is_attr_registered, is_attr_registered_in, list_macros, list_macros_in, list_attributes, list_attributes_in, form, is_form, form_head, form_tail, expand_macro, expand_macro_in, expand_macro_fixpoint, expand_macro_fixpoint_in, expand_form, expand_form_in, expand_form_deep, expand_form_deep_in, new_rewriter, clear_rewriter, register_rewrite, list_rewrites, rewrite_once, rewrite_fixpoint, apply_attribute, apply_attribute_in)
use std.core
use std.core.syntax.syntax as syntax_impl
use std.core.reflect
use std.core.dict_mod (dict_write)

fn _attr_ensure_exact(str name, list args, int want) int {
   if args.len != want { panic("@" + name + " expects " + to_str(want) + " argument(s)") }
   0
}

fn _attr_flag(any node, str key, any value=true) any {
   if type(node) != "dict" { return node }
   dict_write(node, key, value)
}

fn _attr_extern(any node, list args) any {
   _attr_ensure_exact("extern", args, 1)
   if !is_str(args[0]) { panic("@extern argument must be string") }
   node = _attr_flag(node, "is_extern", true)
   node = _attr_flag(node, "link_name", args[0])
   node
}

fn _attr_naked(any node, list args) any {
   _attr_ensure_exact("naked", args, 0)
   _attr_flag(node, "attr_naked", true)
}

fn _attr_jit(any node, list args) any {
   _attr_ensure_exact("jit", args, 0)
   _attr_flag(node, "attr_jit", true)
}

fn _attr_thread(any node, list args) any {
   _attr_ensure_exact("thread", args, 0)
   _attr_flag(node, "attr_thread", true)
}

fn _attr_pure(any node, list args) any {
   _attr_ensure_exact("pure", args, 0)
   _attr_flag(node, "attr_pure", true)
}

fn _attr_cache(any node, list args) any {
   _attr_ensure_exact("cache", args, 0)
   _attr_flag(node, "attr_cache", true)
}

fn _attr_effects(any node, list args) any {
   if args.len == 0 { panic("@effects expects at least one argument") }
   node = _attr_flag(node, "effect_contract_known", true)
   node = _attr_flag(node, "effect_contract", args)
   node
}

fn _attr_llvm(any node, list args) any {
   if args.len < 1 || args.len > 2 { panic("@llvm expects 1 or 2 argument(s)") }
   node = _attr_flag(node, "llvm_attr", args[0])
   if args.len == 2 { node = _attr_flag(node, "llvm_value", args[1]) }
   node
}

fn _builtin_attr_names() list {
   ["extern", "naked", "jit", "thread", "pure", "cache", "effects", "llvm"]
}

fn _builtin_attr_handler(str name) any {
   if name == "extern" { return _attr_extern }
   if name == "naked" { return _attr_naked }
   if name == "jit" { return _attr_jit }
   if name == "thread" { return _attr_thread }
   if name == "pure" { return _attr_pure }
   if name == "cache" { return _attr_cache }
   if name == "effects" { return _attr_effects }
   if name == "llvm" { return _attr_llvm }
   nil
}

fn _seed_defaults(dict reg) dict {
   reg = dict_write(reg, "attrs", dict(8))
   reg = dict_write(reg, "attr_order", _builtin_attr_names())
   reg
}

fn _new_registry_raw(int cap=8) dict {
   mut reg = dict(4)
   reg = dict_write(reg, "macros", dict(cap))
   reg = dict_write(reg, "macro_order", list(cap))
   reg = dict_write(reg, "attrs", dict(cap))
   reg = dict_write(reg, "attr_order", list(cap))
   reg
}

mut __registry = _new_registry_raw()
__registry = _seed_defaults(__registry)

fn new_registry(int cap=8) dict {
   "Creates a new explicit syntax registry."
   return syntax_impl.new_registry(cap)
}

fn registry() dict {
   "Returns the process-wide syntax registry."
   return __registry
}

fn reset_registry() dict {
   "Clears and re-initializes the process-wide syntax registry."
   __registry = _new_registry_raw()
   __registry = _seed_defaults(__registry)
   return __registry
}

fn clone_registry() dict {
   "Returns a structural clone of the process-wide registry."
   return syntax_impl.clone_registry(__registry)
}

fn clone_registry_in(dict reg) dict {
   "Returns a structural clone of `reg`."
   return syntax_impl.clone_registry(reg)
}

fn merge_registry(dict reg, bool overwrite=true) dict {
   "Merges `reg` into the process-wide registry."
   __registry = syntax_impl.merge_registry(__registry, reg, overwrite)
   return __registry
}

fn merge_registry_in(dict dst, dict src, bool overwrite=true) dict {
   "Merges `src` into `dst`."
   return syntax_impl.merge_registry(dst, src, overwrite)
}

fn register_macro(str name, any handler) dict {
   "Registers a macro handler in the process-wide registry."
   __registry = syntax_impl.register_macro(__registry, name, handler)
   return __registry
}

fn register_macro_in(dict reg, str name, any handler) dict {
   "Registers a macro handler in the provided registry."
   return syntax_impl.register_macro(reg, name, handler)
}

fn unregister_macro(str name) dict {
   "Unregisters a macro handler from the process-wide registry."
   __registry = syntax_impl.unregister_macro(__registry, name)
   return __registry
}

fn unregister_macro_in(dict reg, str name) dict {
   "Unregisters a macro handler from the provided registry."
   return syntax_impl.unregister_macro(reg, name)
}

fn register_attribute(str name, any handler) dict {
   "Registers an attribute handler in the process-wide registry."
   __registry = syntax_impl.register_attribute(__registry, name, handler)
   return __registry
}

fn register_attribute_in(dict reg, str name, any handler) dict {
   "Registers an attribute handler in the provided registry."
   return syntax_impl.register_attribute(reg, name, handler)
}

fn unregister_attribute(str name) dict {
   "Unregisters an attribute handler from the process-wide registry."
   __registry = syntax_impl.unregister_attribute(__registry, name)
   return __registry
}

fn unregister_attribute_in(dict reg, str name) dict {
   "Unregisters an attribute handler from the provided registry."
   return syntax_impl.unregister_attribute(reg, name)
}

fn get_macro_handler(str name) any {
   "Returns a macro handler from the process-wide registry."
   return syntax_impl.get_macro_handler(__registry, name)
}

fn get_macro_handler_in(dict reg, str name) any {
   "Returns a macro handler from the provided registry."
   return syntax_impl.get_macro_handler(reg, name)
}

fn get_attr_handler(str name) any {
   "Returns an attribute handler from the process-wide registry."
   def handler = syntax_impl.get_attr_handler(__registry, name)
   if handler { return handler }
   return _builtin_attr_handler(name)
}

fn get_attr_handler_in(dict reg, str name) any {
   "Returns an attribute handler from the provided registry."
   return syntax_impl.get_attr_handler(reg, name)
}

fn is_macro_registered(str name) bool {
   "Returns true when a macro handler is registered."
   return syntax_impl.is_macro_registered(__registry, name)
}

fn is_macro_registered_in(dict reg, str name) bool {
   "Returns true when a macro handler is registered in `reg`."
   return syntax_impl.is_macro_registered(reg, name)
}

fn is_attr_registered(str name) bool {
   "Returns true when an attribute handler is registered."
   syntax_impl.is_attr_registered(__registry, name) || !!_builtin_attr_handler(name)
}

fn is_attr_registered_in(dict reg, str name) bool {
   "Returns true when an attribute handler is registered in `reg`."
   return syntax_impl.is_attr_registered(reg, name)
}

fn list_macros() list {
   "Returns macro names in deterministic registration order."
   return syntax_impl.list_macros(__registry)
}

fn list_macros_in(dict reg) list {
   "Returns macro names from `reg` in deterministic registration order."
   return syntax_impl.list_macros(reg)
}

fn list_attributes() list {
   "Returns attribute names in deterministic registration order."
   return syntax_impl.list_attributes(__registry)
}

fn list_attributes_in(dict reg) list {
   "Returns attribute names from `reg` in deterministic registration order."
   return syntax_impl.list_attributes(reg)
}

fn form(any head, any args=0) list {
   "Builds an s-expression-style form list `[head, ...args]`."
   return syntax_impl.form(head, args)
}

fn is_form(any value, any head=0) bool {
   "Returns true when `value` is a syntax form."
   return syntax_impl.is_form(value, head)
}

fn form_head(any value, any default=0) any {
   "Returns the head symbol/value from a syntax form."
   return syntax_impl.form_head(value, default)
}

fn form_tail(any value) list {
   "Returns tail arguments from a syntax form."
   return syntax_impl.form_tail(value)
}

fn expand_macro(str name, any args=0, any body=0, any tok=0) any {
   "Expands a macro using the process-wide registry."
   return syntax_impl.expand_macro(__registry, name, args, body, tok)
}

fn expand_macro_in(dict reg, str name, any args=0, any body=0, any tok=0) any {
   "Expands a macro using the provided registry."
   return syntax_impl.expand_macro(reg, name, args, body, tok)
}

fn expand_macro_fixpoint(str name, any args=0, any body=0, any tok=0, int max_steps=64) any {
   "Expands a macro to fixpoint using the process-wide registry."
   return syntax_impl.expand_macro_fixpoint(__registry, name, args, body, tok, max_steps)
}

fn expand_macro_fixpoint_in(dict reg, str name, any args=0, any body=0, any tok=0, int max_steps=64) any {
   "Expands a macro to fixpoint using the provided registry."
   return syntax_impl.expand_macro_fixpoint(reg, name, args, body, tok, max_steps)
}

fn expand_form(any value, any tok=0, int max_steps=64) any {
   "Expands a Lisp-style form using the process-wide registry."
   return syntax_impl.expand_form(__registry, value, tok, max_steps)
}

fn expand_form_in(dict reg, any value, any tok=0, int max_steps=64) any {
   "Expands a Lisp-style form using the provided registry."
   return syntax_impl.expand_form(reg, value, tok, max_steps)
}

fn expand_form_deep(any value, any tok=0, int max_steps=64) any {
   "Recursively expands nested forms using the process-wide registry."
   return syntax_impl.expand_form_deep(__registry, value, tok, max_steps)
}

fn expand_form_deep_in(dict reg, any value, any tok=0, int max_steps=64) any {
   "Recursively expands nested forms using the provided registry."
   return syntax_impl.expand_form_deep(reg, value, tok, max_steps)
}

fn new_rewriter(int cap=8) dict {
   "Creates a deterministic syntax rewrite pipeline."
   return syntax_impl.new_rewriter(cap)
}

fn clear_rewriter(dict rw) dict {
   "Clears all rewrite rules from `rw`."
   return syntax_impl.clear_rewriter(rw)
}

fn register_rewrite(dict rw, str name, any handler) dict {
   "Registers a named rewrite pass in `rw`."
   return syntax_impl.register_rewrite(rw, name, handler)
}

fn list_rewrites(dict rw) list {
   "Returns rewrite pass names in deterministic registration order."
   return syntax_impl.list_rewrites(rw)
}

fn rewrite_once(dict rw, any value) any {
   "Runs one rewrite pass sequence over `value`."
   return syntax_impl.rewrite_once(rw, value)
}

fn rewrite_fixpoint(dict rw, any value, int max_steps=64) any {
   "Runs rewrite passes until stable or step limit is reached."
   return syntax_impl.rewrite_fixpoint(rw, value, max_steps)
}

fn apply_attribute(str name, any node, any args=0) any {
   "Applies an attribute handler using the process-wide registry."
   def handler = get_attr_handler(name)
   if !handler { return node }
   if is_list(args) { return handler(node, args) }
   return handler(node, list(0))
}

fn apply_attribute_in(dict reg, str name, any node, any args=0) any {
   "Applies an attribute handler using the provided registry."
   return syntax_impl.apply_attribute(reg, name, node, args)
}

#main {
   fn _syntax_public_self_macro(any node) any { node }
   fn _syntax_public_self_attr(dict node, list args) dict { node.set("marked", args.len >= 0) }
   fn _syntax_public_self_double(any v) any { is_int(v) ? v * 2 : v }
   reset_registry()
   assert(is_attr_registered("extern"), "syntax facade builtin attr")
   register_macro("public_id", _syntax_public_self_macro)
   assert(is_macro_registered("public_id") && list_macros().contains("public_id"), "syntax facade register macro")
   def expanded = expand_form(form("public_id", [1, 2, 3]))
   assert(is_form(expanded, "public_id") && form_tail(expanded).len == 3, "syntax facade expand form")
   unregister_macro("public_id")
   assert(!is_macro_registered("public_id"), "syntax facade unregister macro")
   mut reg = new_registry(8)
   reg = register_macro_in(reg, "id2", _syntax_public_self_macro)
   assert(is_macro_registered_in(reg, "id2"), "syntax facade register in")
   reg = unregister_macro_in(reg, "id2")
   assert(!is_macro_registered_in(reg, "id2"), "syntax facade unregister in")
   register_attribute("mark", _syntax_public_self_attr)
   assert(is_attr_registered("mark") && apply_attribute("mark", dict(2), list(0)).get("marked", false), "syntax facade attr")
   unregister_attribute("mark")
   assert(!is_attr_registered("mark"), "syntax facade unregister attr")
   mut rw = new_rewriter(4)
   rw = register_rewrite(rw, "double", _syntax_public_self_double)
   assert(list_rewrites(rw).contains("double") && rewrite_once(rw, 3) == 6 && rewrite_fixpoint(rw, 3, 1) == 6, "syntax facade rewrite")
   rw = clear_rewriter(rw)
   assert(list_rewrites(rw).len == 0, "syntax facade clear rewrite")
   print("✓ std.core.syntax self-test passed")
}
