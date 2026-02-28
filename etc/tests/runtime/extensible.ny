use std.core *
use std.core.dict *
use std.core.syntax as syntax
use std.core.test as test

fn __list_has(xs, want){
   mut i = 0
   while(i < len(xs)){
      if(get(xs, i, 0) == want){ return true }
      i += 1
   }
   false
}

fn __macro_double(node){
   def args = dict_get(node, "args", list(0))
   if(len(args) != 1){ return 0 }
   def x = get(args, 0, 0)
   if(!is_int(x)){ return 0 }
   x + x
}

fn __macro_double_plus1(node){
   def args = dict_get(node, "args", list(0))
   if(len(args) != 1){ return 0 }
   def x = get(args, 0, 0)
   if(!is_int(x)){ return 0 }
   (x + x) + 1
}

fn __macro_add2(node){
   def args = dict_get(node, "args", list(0))
   if(len(args) != 2){ return 0 }
   def a = get(args, 0, 0)
   def b = get(args, 1, 0)
   if(!is_int(a) || !is_int(b)){ return 0 }
   a + b
}

fn __macro_to_double_form(node){
   def args = dict_get(node, "args", list(0))
   syntax.form("double", args)
}

fn __macro_ping(node){
   def args = dict_get(node, "args", list(0))
   syntax.form("pong", args)
}

fn __macro_pong(node){
   def args = dict_get(node, "args", list(0))
   syntax.form("ping", args)
}

fn __attr_mark(node, args){
   if(!is_dict(node)){ return node }
   mut out = node
   out = dict_set(out, "marked", true)
   out = dict_set(out, "arg_count", len(args))
   out
}

fn __rw_add1(value){
   if(!is_int(value)){ return value }
   value + 1
}

fn __rw_mul2(value){
   if(!is_int(value)){ return value }
   value * 2
}

fn __rw_shift(value){
   if(!syntax.is_form(value, "shift")){ return value }
   def tail = syntax.form_tail(value)
   if(len(tail) != 1){ return value }
   syntax.form("inc1", tail)
}

fn __rw_eval_inc1(value){
   if(!syntax.is_form(value, "inc1")){ return value }
   def tail = syntax.form_tail(value)
   if(len(tail) != 1){ return value }
   def x = get(tail, 0, 0)
   if(!is_int(x)){ return value }
   x + 1
}

fn test_defaults_and_surface(){
   syntax.reset_registry()

   def reg = syntax.registry()
   test.assert(is_dict(reg), "registry should be dict")

   def attrs = syntax.list_attributes()
   test.assert(__list_has(attrs, "extern"), "builtin @extern should be present")
   test.assert(__list_has(attrs, "effects"), "builtin @effects should be present")
   test.assert(__list_has(attrs, "llvm"), "builtin @llvm should be present")

   test.assert(!syntax.is_macro_registered("double"),
               "reset should not keep custom macro registrations")
   test.assert(!syntax.get_macro_handler("double"),
               "missing macro handler should return none")
   test.assert(!!syntax.get_attr_handler("effects"),
               "builtin attr handler should be resolvable")

   test.assert(!syntax.is_form(42), "non-list should not be form")
   test.assert(syntax.form_head(42, -1) == -1, "form_head default should be returned")
   def tail = syntax.form_tail(42)
   test.assert(is_list(tail) && len(tail) == 0,
               "form_tail on non-form should return empty list")
}

fn test_registry_local_ops(){
   mut reg = syntax.new_registry()
   test.assert(!syntax.is_macro_registered_in(reg, "double"),
               "fresh registry should not contain custom macro")
   test.assert(!syntax.is_attr_registered_in(reg, "mark"),
               "fresh registry should not contain custom attribute")

   reg = syntax.register_macro_in(reg, "double", __macro_double)
   reg = syntax.register_macro_in(reg, "add2", __macro_add2)
   reg = syntax.register_attribute_in(reg, "mark", __attr_mark)

   test.assert(syntax.is_macro_registered_in(reg, "double"),
               "macro should be registered in local registry")
   test.assert(syntax.is_attr_registered_in(reg, "mark"),
               "attribute should be registered in local registry")
   test.assert(!!syntax.get_macro_handler_in(reg, "double"),
               "local macro handler should be retrievable")
   test.assert(!!syntax.get_attr_handler_in(reg, "mark"),
               "local attribute handler should be retrievable")

   def macros = syntax.list_macros_in(reg)
   test.assert(len(macros) == 2, "local macro list should keep explicit registrations")
   test.assert(get(macros, 0, "") == "double",
               "first local macro should preserve registration order")
   test.assert(get(macros, 1, "") == "add2",
               "second local macro should preserve registration order")

   def attrs = syntax.list_attributes_in(reg)
   test.assert(len(attrs) == 1 && get(attrs, 0, "") == "mark",
               "local attribute list should preserve registration order")

   reg = syntax.unregister_macro_in(reg, "double")
   test.assert(!syntax.is_macro_registered_in(reg, "double"),
               "unregister_macro_in should remove macro handler")
   def macros2 = syntax.list_macros_in(reg)
   test.assert(len(macros2) == 1 && get(macros2, 0, "") == "add2",
               "unregister_macro_in should update deterministic order")

   reg = syntax.unregister_attribute_in(reg, "mark")
   test.assert(!syntax.is_attr_registered_in(reg, "mark"),
               "unregister_attribute_in should remove attribute handler")
   def attrs2 = syntax.list_attributes_in(reg)
   test.assert(len(attrs2) == 0,
               "unregister_attribute_in should update deterministic order")
}

fn test_registry_global_ops(){
   syntax.reset_registry()
   syntax.register_macro("double", __macro_double)
   syntax.register_attribute("mark", __attr_mark)

   test.assert(syntax.is_macro_registered("double"),
               "global macro should be registered")
   test.assert(syntax.is_attr_registered("mark"),
               "global attribute should be registered")

   def runtime_expanded = syntax.expand_macro("double", [21])
   def comptime_expanded = comptime{ return 21 + 21 }
   test.assert(runtime_expanded == comptime_expanded,
               "runtime and comptime expansion contract should match")

   mut node = dict(2)
   node = dict_set(node, "name", "demo")
   def marked = syntax.apply_attribute("mark", node, ["x", "y"])
   test.assert(dict_get(marked, "marked", false),
               "global attribute application should run custom handler")
   test.assert(dict_get(marked, "arg_count", 0) == 2,
               "attribute args should be forwarded to handler")

   syntax.reset_registry()
   test.assert(!syntax.is_macro_registered("double"),
               "reset should clear custom macro")
   test.assert(!syntax.is_attr_registered("mark"),
               "reset should clear custom attribute")
   test.assert(syntax.is_attr_registered("effects"),
               "reset should re-register builtin attributes")
}

fn test_registry_clone_merge_ops(){
   mut base = syntax.new_registry()
   base = syntax.register_macro_in(base, "double", __macro_double)
   base = syntax.register_attribute_in(base, "mark", __attr_mark)

   mut cloned = syntax.clone_registry_in(base)
   test.assert(is_dict(cloned), "clone_registry_in should return registry dict")
   cloned = syntax.register_macro_in(cloned, "add2", __macro_add2)
   test.assert(!syntax.is_macro_registered_in(base, "add2"),
               "cloned registry updates should not mutate source")
   cloned = syntax.unregister_macro_in(cloned, "double")
   test.assert(syntax.is_macro_registered_in(base, "double"),
               "cloned unregister should not mutate source")

   mut incoming = syntax.new_registry()
   incoming = syntax.register_macro_in(incoming, "double", __macro_double_plus1)
   incoming = syntax.register_macro_in(incoming, "add2", __macro_add2)
   incoming = syntax.register_attribute_in(incoming, "mark2", __attr_mark)

   base = syntax.merge_registry_in(base, incoming, false)
   test.assert(syntax.expand_macro_in(base, "double", [5]) == 10,
               "merge_registry_in without overwrite should keep existing handlers")
   test.assert(syntax.expand_macro_in(base, "add2", [20, 22]) == 42,
               "merge_registry_in should add missing handlers")
   test.assert(syntax.is_attr_registered_in(base, "mark2"),
               "merge_registry_in should add missing attributes")
   def merged_macros = syntax.list_macros_in(base)
   test.assert(len(merged_macros) == 2,
               "merged macro list should include destination + incoming entries")
   test.assert(get(merged_macros, 0, "") == "double",
               "merged macro order should keep destination registrations first")
   test.assert(get(merged_macros, 1, "") == "add2",
               "merged macro order should append incoming registrations")

   base = syntax.merge_registry_in(base, incoming, true)
   test.assert(syntax.expand_macro_in(base, "double", [5]) == 11,
               "merge_registry_in with overwrite should replace existing handlers")

   syntax.reset_registry()
   syntax.register_macro("double", __macro_double)
   test.assert(is_dict(syntax.clone_registry()), "clone_registry should clone global registry")
   syntax.merge_registry(incoming, true)
   test.assert(syntax.expand_macro("double", [5]) == 11,
               "merge_registry should update process-wide registry")
   syntax.unregister_macro("double")
   test.assert(!syntax.is_macro_registered("double"),
               "unregister_macro should remove process-wide macro handler")
   syntax.register_attribute("mark", __attr_mark)
   syntax.unregister_attribute("mark")
   test.assert(!syntax.is_attr_registered("mark"),
               "unregister_attribute should remove process-wide attribute handler")
   syntax.reset_registry()
}

fn test_form_expansion_surface(){
   mut reg = syntax.new_registry()
   reg = syntax.register_macro_in(reg, "double", __macro_double)
   reg = syntax.register_macro_in(reg, "double_form", __macro_to_double_form)
   reg = syntax.register_macro_in(reg, "add2", __macro_add2)

   def expr = syntax.form("double_form", [21])
   test.assert(syntax.is_form(expr, "double_form"), "form head should match")
   test.assert(syntax.form_head(expr, "") == "double_form",
               "form_head should expose macro name")
   def tail = syntax.form_tail(expr)
   test.assert(len(tail) == 1 && get(tail, 0, 0) == 21,
               "form_tail should preserve arguments")

   def out_form = syntax.expand_form_in(reg, expr)
   test.assert(out_form == 42, "expand_form_in should follow macro chain")
   def out_macro = syntax.expand_macro_fixpoint_in(reg, "double_form", [21])
   test.assert(out_macro == 42, "expand_macro_fixpoint_in should follow macro chain")
   test.assert(syntax.expand_macro_in(reg, "double", [8]) == 16,
               "expand_macro_in should use provided registry")

   syntax.reset_registry()
   syntax.register_macro("double", __macro_double)
   test.assert(syntax.expand_form(syntax.form("double", [9])) == 18,
               "expand_form should use process-wide registry")

   def nested = syntax.form("begin", [
      syntax.form("add2", [20, 22]),
      [syntax.form("double", [3]), syntax.form("double", [5])]
   ])
   def deep1 = syntax.expand_form_deep_in(reg, nested)
   test.assert(syntax.is_form(deep1, "begin"), "deep expansion should keep non-macro head")
   test.assert(get(deep1, 1, 0) == 42, "nested direct form should expand")
   def inner = get(deep1, 2, list(0))
   test.assert(is_list(inner), "nested list should remain list")
   test.assert(get(inner, 0, 0) == 6, "first nested form should expand")
   test.assert(get(inner, 1, 0) == 10, "second nested form should expand")

   def deep2 = syntax.expand_form_deep_in(reg, deep1)
   test.assert(eq(deep2, deep1),
               "deep expansion should be idempotent after reaching fixpoint")

   mut cyc = syntax.new_registry()
   cyc = syntax.register_macro_in(cyc, "ping", __macro_ping)
   cyc = syntax.register_macro_in(cyc, "pong", __macro_pong)
   def limited = syntax.expand_macro_fixpoint_in(cyc, "ping", [1], 0, 0, 3)
   test.assert(is_dict(limited), "limited cycle expansion should return macro node")
   test.assert(dict_get(limited, "name", "") == "pong",
               "fixpoint max_steps should stop deterministically")
}

fn test_attribute_surface_and_fallback(){
   mut reg = syntax.new_registry()
   reg = syntax.register_attribute_in(reg, "mark", __attr_mark)

   mut node = dict(2)
   node = dict_set(node, "name", "node1")
   def unchanged = syntax.apply_attribute_in(reg, "missing", node, ["x"])
   test.assert(is_dict(unchanged), "missing attribute should return original node")
   test.assert(!dict_get(unchanged, "marked", false),
               "missing attribute should not mutate node metadata")

   def marked = syntax.apply_attribute_in(reg, "mark", node, ["a", "b"])
   test.assert(dict_get(marked, "marked", false),
               "local attribute handler should apply")
   test.assert(dict_get(marked, "arg_count", 0) == 2,
               "local attribute handler should receive args")

   syntax.reset_registry()
   mut fn_node = dict(4)
   fn_node = dict_set(fn_node, "name", "demo_fn")

   def effects_node = syntax.apply_attribute("effects", fn_node, ["none"])
   test.assert(dict_get(effects_node, "effect_contract_known", false),
               "builtin @effects should set known flag")
   def effect_contract = dict_get(effects_node, "effect_contract", list(0))
   test.assert(len(effect_contract) == 1 && get(effect_contract, 0, "") == "none",
               "builtin @effects should keep provided effect contract")

   def llvm_node = syntax.apply_attribute("llvm", fn_node, ["noinline"])
   test.assert(dict_get(llvm_node, "llvm_attr", "") == "noinline",
               "builtin @llvm should set llvm attribute name")

   def extern_node = syntax.apply_attribute("extern", fn_node, ["puts"])
   test.assert(dict_get(extern_node, "is_extern", false),
               "builtin @extern should mark extern")
   test.assert(dict_get(extern_node, "link_name", "") == "puts",
               "builtin @extern should set link name")
}

fn test_rewriter_surface(){
   mut rw = syntax.new_rewriter()
   rw = syntax.register_rewrite(rw, "add1", __rw_add1)
   rw = syntax.register_rewrite(rw, "mul2", __rw_mul2)

   def names = syntax.list_rewrites(rw)
   test.assert(len(names) == 2, "rewriter should expose deterministic pass list")
   test.assert(get(names, 0, "") == "add1", "first rewrite should preserve order")
   test.assert(get(names, 1, "") == "mul2", "second rewrite should preserve order")
   test.assert(syntax.rewrite_once(rw, 10) == 22,
               "rewrite_once should run handlers in registration order")

   rw = syntax.clear_rewriter(rw)
   test.assert(len(syntax.list_rewrites(rw)) == 0, "clear_rewriter should remove rules")
   test.assert(syntax.rewrite_once(rw, 10) == 10,
               "rewrite_once should return input when no rules exist")

   rw = syntax.register_rewrite(rw, "shift", __rw_shift)
   rw = syntax.register_rewrite(rw, "eval_inc1", __rw_eval_inc1)
   test.assert(syntax.rewrite_fixpoint(rw, syntax.form("shift", [41])) == 42,
               "rewrite_fixpoint should stabilize chained rewrites")
}

fn test_error_paths(){
   mut caught = false
   try {
      syntax.register_macro("", __macro_double)
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   test.assert(caught, "empty macro name should fail")

   caught = false
   try {
      syntax.register_attribute("", __attr_mark)
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   test.assert(caught, "empty attribute name should fail")

   syntax.reset_registry()
   caught = false
   try {
      syntax.apply_attribute("effects", dict(2), list(0))
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   test.assert(caught, "@effects without args should fail")

   caught = false
   try {
      syntax.apply_attribute("extern", dict(2), [123])
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   test.assert(caught, "@extern with non-string argument should fail")
}

test_defaults_and_surface()
test_registry_local_ops()
test_registry_global_ops()
test_registry_clone_merge_ops()
test_form_expansion_surface()
test_attribute_surface_and_fallback()
test_rewriter_surface()
test_error_paths()

print("✓ extensible syntax tests passed")
