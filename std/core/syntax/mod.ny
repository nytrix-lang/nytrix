;; Keywords: core syntax
;; Core Syntax module.

module std.core.syntax (
   new_registry, registry, reset_registry,
   clone_registry, clone_registry_in,
   merge_registry, merge_registry_in,
   register_macro, register_macro_in,
   unregister_macro, unregister_macro_in,
   register_attribute, register_attribute_in,
   unregister_attribute, unregister_attribute_in,
   get_macro_handler, get_macro_handler_in,
   get_attr_handler, get_attr_handler_in,
   is_macro_registered, is_macro_registered_in,
   is_attr_registered, is_attr_registered_in,
   list_macros, list_macros_in,
   list_attributes, list_attributes_in,
   form, is_form, form_head, form_tail,
   expand_macro, expand_macro_in,
   expand_macro_fixpoint, expand_macro_fixpoint_in,
   expand_form, expand_form_in,
   expand_form_deep, expand_form_deep_in,
   new_rewriter, clear_rewriter,
   register_rewrite, list_rewrites,
   rewrite_once, rewrite_fixpoint,
   apply_attribute, apply_attribute_in
)
use std.core *
use std.core.syntax.syntax as syntax_impl
use std.core.syntax.builtin as syntax_builtin

mut __registry = syntax_impl.new_registry()
__registry = syntax_builtin.register_defaults(__registry)

fn new_registry(cap=8){
   "Creates a new explicit syntax registry."
   syntax_impl.new_registry(cap)
}

fn registry(){
   "Returns the process-wide syntax registry."
   __registry
}

fn reset_registry(){
   "Clears and re-initializes the process-wide syntax registry."
   __registry = syntax_impl.clear_registry(__registry)
   __registry = syntax_builtin.register_defaults(__registry)
   __registry
}

fn clone_registry(){
   "Returns a structural clone of the process-wide registry."
   syntax_impl.clone_registry(__registry)
}

fn clone_registry_in(reg){
   "Returns a structural clone of `reg`."
   syntax_impl.clone_registry(reg)
}

fn merge_registry(reg, overwrite=true){
   "Merges `reg` into the process-wide registry."
   __registry = syntax_impl.merge_registry(__registry, reg, overwrite)
   __registry
}

fn merge_registry_in(dst, src, overwrite=true){
   "Merges `src` into `dst`."
   syntax_impl.merge_registry(dst, src, overwrite)
}

fn register_macro(name, handler){
   "Registers a macro handler in the process-wide registry."
   __registry = syntax_impl.register_macro(__registry, name, handler)
   __registry
}

fn register_macro_in(reg, name, handler){
   "Registers a macro handler in the provided registry."
   syntax_impl.register_macro(reg, name, handler)
}

fn unregister_macro(name){
   "Unregisters a macro handler from the process-wide registry."
   __registry = syntax_impl.unregister_macro(__registry, name)
   __registry
}

fn unregister_macro_in(reg, name){
   "Unregisters a macro handler from the provided registry."
   syntax_impl.unregister_macro(reg, name)
}

fn register_attribute(name, handler){
   "Registers an attribute handler in the process-wide registry."
   __registry = syntax_impl.register_attribute(__registry, name, handler)
   __registry
}

fn register_attribute_in(reg, name, handler){
   "Registers an attribute handler in the provided registry."
   syntax_impl.register_attribute(reg, name, handler)
}

fn unregister_attribute(name){
   "Unregisters an attribute handler from the process-wide registry."
   __registry = syntax_impl.unregister_attribute(__registry, name)
   __registry
}

fn unregister_attribute_in(reg, name){
   "Unregisters an attribute handler from the provided registry."
   syntax_impl.unregister_attribute(reg, name)
}

fn get_macro_handler(name){
   "Returns a macro handler from the process-wide registry."
   syntax_impl.get_macro_handler(__registry, name)
}

fn get_macro_handler_in(reg, name){
   "Returns a macro handler from the provided registry."
   syntax_impl.get_macro_handler(reg, name)
}

fn get_attr_handler(name){
   "Returns an attribute handler from the process-wide registry."
   syntax_impl.get_attr_handler(__registry, name)
}

fn get_attr_handler_in(reg, name){
   "Returns an attribute handler from the provided registry."
   syntax_impl.get_attr_handler(reg, name)
}

fn is_macro_registered(name){
   "Returns true when a macro handler is registered."
   syntax_impl.is_macro_registered(__registry, name)
}

fn is_macro_registered_in(reg, name){
   "Returns true when a macro handler is registered in `reg`."
   syntax_impl.is_macro_registered(reg, name)
}

fn is_attr_registered(name){
   "Returns true when an attribute handler is registered."
   syntax_impl.is_attr_registered(__registry, name)
}

fn is_attr_registered_in(reg, name){
   "Returns true when an attribute handler is registered in `reg`."
   syntax_impl.is_attr_registered(reg, name)
}

fn list_macros(){
   "Returns macro names in deterministic registration order."
   syntax_impl.list_macros(__registry)
}

fn list_macros_in(reg){
   "Returns macro names from `reg` in deterministic registration order."
   syntax_impl.list_macros(reg)
}

fn list_attributes(){
   "Returns attribute names in deterministic registration order."
   syntax_impl.list_attributes(__registry)
}

fn list_attributes_in(reg){
   "Returns attribute names from `reg` in deterministic registration order."
   syntax_impl.list_attributes(reg)
}

fn form(head, args=0){
   "Builds an s-expression-style form list `[head, ...args]`."
   syntax_impl.form(head, args)
}

fn is_form(value, head=0){
   "Returns true when `value` is a syntax form."
   syntax_impl.is_form(value, head)
}

fn form_head(value, default=0){
   "Returns the head symbol/value from a syntax form."
   syntax_impl.form_head(value, default)
}

fn form_tail(value){
   "Returns tail arguments from a syntax form."
   syntax_impl.form_tail(value)
}

fn expand_macro(name, args=0, body=0, tok=0){
   "Expands a macro using the process-wide registry."
   syntax_impl.expand_macro(__registry, name, args, body, tok)
}

fn expand_macro_in(reg, name, args=0, body=0, tok=0){
   "Expands a macro using the provided registry."
   syntax_impl.expand_macro(reg, name, args, body, tok)
}

fn expand_macro_fixpoint(name, args=0, body=0, tok=0, max_steps=64){
   "Expands a macro to fixpoint using the process-wide registry."
   syntax_impl.expand_macro_fixpoint(__registry, name, args, body, tok, max_steps)
}

fn expand_macro_fixpoint_in(reg, name, args=0, body=0, tok=0, max_steps=64){
   "Expands a macro to fixpoint using the provided registry."
   syntax_impl.expand_macro_fixpoint(reg, name, args, body, tok, max_steps)
}

fn expand_form(value, tok=0, max_steps=64){
   "Expands a Lisp-style form using the process-wide registry."
   syntax_impl.expand_form(__registry, value, tok, max_steps)
}

fn expand_form_in(reg, value, tok=0, max_steps=64){
   "Expands a Lisp-style form using the provided registry."
   syntax_impl.expand_form(reg, value, tok, max_steps)
}

fn expand_form_deep(value, tok=0, max_steps=64){
   "Recursively expands nested forms using the process-wide registry."
   syntax_impl.expand_form_deep(__registry, value, tok, max_steps)
}

fn expand_form_deep_in(reg, value, tok=0, max_steps=64){
   "Recursively expands nested forms using the provided registry."
   syntax_impl.expand_form_deep(reg, value, tok, max_steps)
}

fn new_rewriter(cap=8){
   "Creates a deterministic syntax rewrite pipeline."
   syntax_impl.new_rewriter(cap)
}

fn clear_rewriter(rw){
   "Clears all rewrite rules from `rw`."
   syntax_impl.clear_rewriter(rw)
}

fn register_rewrite(rw, name, handler){
   "Registers a named rewrite pass in `rw`."
   syntax_impl.register_rewrite(rw, name, handler)
}

fn list_rewrites(rw){
   "Returns rewrite pass names in deterministic registration order."
   syntax_impl.list_rewrites(rw)
}

fn rewrite_once(rw, value){
   "Runs one rewrite pass sequence over `value`."
   syntax_impl.rewrite_once(rw, value)
}

fn rewrite_fixpoint(rw, value, max_steps=64){
   "Runs rewrite passes until stable or step limit is reached."
   syntax_impl.rewrite_fixpoint(rw, value, max_steps)
}

fn apply_attribute(name, node, args=0){
   "Applies an attribute handler using the process-wide registry."
   syntax_impl.apply_attribute(__registry, name, node, args)
}

fn apply_attribute_in(reg, name, node, args=0){
   "Applies an attribute handler using the provided registry."
   syntax_impl.apply_attribute(reg, name, node, args)
}

if(comptime{__main()}){

    reset_registry()
    assert(is_attr_registered("extern"), "defaults loaded")

    fn macro_id(node){
       "Test helper."
       node
    }

    register_macro("id", macro_id)
    assert(is_macro_registered("id"), "register_macro")
    assert(contains(list_macros(), "id"), "list_macros")

    def form0 = form("id", [1, 2, 3])
    def expanded = expand_form(form0)
    assert(is_form(expanded, "id"), "expand_form")
    assert(form_head(expanded, "") == "id", "form_head")
    assert(len(form_tail(expanded)) == 3, "form_tail")

    unregister_macro("id")
    assert(!is_macro_registered("id"), "unregister_macro")

    mut reg = new_registry(8)
    reg = register_macro_in(reg, "id2", macro_id)
    assert(is_macro_registered_in(reg, "id2"), "register_macro_in")
    reg = unregister_macro_in(reg, "id2")
    assert(!is_macro_registered_in(reg, "id2"), "unregister_macro_in")

    fn attr_tag(node, args){
       "Test helper."
       dict_set(node, "tagged", true)
    }

    register_attribute("tag", attr_tag)
    assert(is_attr_registered("tag"), "register_attribute")
    def n = apply_attribute("tag", dict(2), list(0))
    assert(dict_get(n, "tagged", false), "apply_attribute")
    unregister_attribute("tag")
    assert(!is_attr_registered("tag"), "unregister_attribute")

    mut rw = new_rewriter(4)
    fn rw_add1(v){
       "Test helper."
       if(is_int(v)){ return v + 1 }
       v
    }
    rw = register_rewrite(rw, "add1", rw_add1)
    assert(contains(list_rewrites(rw), "add1"), "register_rewrite")
    assert(rewrite_once(rw, 1) == 2, "rewrite_once")
    assert(rewrite_fixpoint(rw, 1, 1) == 2, "rewrite_fixpoint")
    rw = clear_rewriter(rw)
    assert(len(list_rewrites(rw)) == 0, "clear_rewriter")
}
