use std.core
use std.core.syntax as syntax
use std.core.test as test

fn __list_has(xs, want){
   mut i = 0
   while(i < xs.len){
      if(xs.get(i, 0) == want){ return true }
      i += 1
   }
   false
}

fn __macro_double(node){
   def args = node.get("args", list(0))
   if(args.len != 1){ return 0 }
   def x = args.get(0, 0)
   if(!is_int(x)){ return 0 }
   x + x
}

fn __macro_double_plus1(node){
   def args = node.get("args", list(0))
   if(args.len != 1){ return 0 }
   def x = args.get(0, 0)
   if(!is_int(x)){ return 0 }
   (x + x) + 1
}

fn __macro_add2(node){
   def args = node.get("args", list(0))
   if(args.len != 2){ return 0 }
   def a = args.get(0, 0)
   def b = args.get(1, 0)
   if(!is_int(a) || !is_int(b)){ return 0 }
   a + b
}

fn __macro_to_double_form(node){
   def args = node.get("args", list(0))
   syntax.form("double", args)
}

fn __macro_ping(node){
   def args = node.get("args", list(0))
   syntax.form("pong", args)
}

fn __macro_pong(node){
   def args = node.get("args", list(0))
   syntax.form("ping", args)
}

fn __attr_mark(node, args){
   if(!is_dict(node)){ return node }
   mut out = node
   out = out.set("marked", true)
   out = out.set("arg_count", args.len)
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
   if(tail.len != 1){ return value }
   syntax.form("inc1", tail)
}

fn __rw_eval_inc1(value){
   if(!syntax.is_form(value, "inc1")){ return value }
   def tail = syntax.form_tail(value)
   if(tail.len != 1){ return value }
   def x = tail.get(0, 0)
   if(!is_int(x)){ return value }
   x + 1
}

fn test_defaults_and_surface(){
   syntax.reset_registry()
   def reg = syntax.registry()
   assert(is_dict(reg), "registry should be dict")
   def attrs = syntax.list_attributes()
   assert(__list_has(attrs, "extern"), "builtin @extern should be present")
   assert(__list_has(attrs, "effects"), "builtin @effects should be present")
   assert(__list_has(attrs, "llvm"), "builtin @llvm should be present")
   assert(!syntax.is_macro_registered("double"),
   "reset should not keep custom macro registrations")
   assert(!syntax.get_macro_handler("double"),
   "missing macro handler should return none")
   assert(!!syntax.get_attr_handler("effects"),
   "builtin attr handler should be resolvable")
   assert(!syntax.is_form(42), "non-list should not be form")
   assert(syntax.form_head(42, -1) == -1, "form_head default should be returned")
   def tail = syntax.form_tail(42)
   assert(is_list(tail) && tail.len == 0,
   "form_tail on non-form should return empty list")
}

fn test_registry_local_ops(){
   mut reg = syntax.new_registry()
   assert(!syntax.is_macro_registered_in(reg, "double"),
   "fresh registry should not contain custom macro")
   assert(!syntax.is_attr_registered_in(reg, "mark"),
   "fresh registry should not contain custom attribute")
   reg = syntax.register_macro_in(reg, "double", __macro_double)
   reg = syntax.register_macro_in(reg, "add2", __macro_add2)
   reg = syntax.register_attribute_in(reg, "mark", __attr_mark)
   assert(syntax.is_macro_registered_in(reg, "double"),
   "macro should be registered in local registry")
   assert(syntax.is_attr_registered_in(reg, "mark"),
   "attribute should be registered in local registry")
   assert(!!syntax.get_macro_handler_in(reg, "double"),
   "local macro handler should be retrievable")
   assert(!!syntax.get_attr_handler_in(reg, "mark"),
   "local attribute handler should be retrievable")
   def macros = syntax.list_macros_in(reg)
   assert(macros.len == 2, "local macro list should keep explicit registrations")
   assert(macros.get(0, "") == "double",
   "first local macro should preserve registration order")
   assert(macros.get(1, "") == "add2",
   "second local macro should preserve registration order")
   def attrs = syntax.list_attributes_in(reg)
   assert(attrs.len == 1 && attrs.get(0, "") == "mark",
   "local attribute list should preserve registration order")
   reg = syntax.unregister_macro_in(reg, "double")
   assert(!syntax.is_macro_registered_in(reg, "double"),
   "unregister_macro_in should remove macro handler")
   def macros2 = syntax.list_macros_in(reg)
   assert(macros2.len == 1 && macros2.get(0, "") == "add2",
   "unregister_macro_in should update deterministic order")
   reg = syntax.unregister_attribute_in(reg, "mark")
   assert(!syntax.is_attr_registered_in(reg, "mark"),
   "unregister_attribute_in should remove attribute handler")
   def attrs2 = syntax.list_attributes_in(reg)
   assert(attrs2.len == 0,
   "unregister_attribute_in should update deterministic order")
}

fn test_registry_global_ops(){
   syntax.reset_registry()
   syntax.register_macro("double", __macro_double)
   syntax.register_attribute("mark", __attr_mark)
   assert(syntax.is_macro_registered("double"),
   "global macro should be registered")
   assert(syntax.is_attr_registered("mark"),
   "global attribute should be registered")
   def runtime_expanded = syntax.expand_macro("double", [21])
   def comptime_expanded = comptime{ return 21 + 21 }
   assert(runtime_expanded == comptime_expanded,
   "runtime and comptime expansion contract should match")
   mut node = dict(2)
   node = node.set("name", "demo")
   def marked = syntax.apply_attribute("mark", node, ["x", "y"])
   assert(marked.get("marked", false),
   "global attribute application should run custom handler")
   assert(marked.get("arg_count", 0) == 2,
   "attribute args should be forwarded to handler")
   syntax.reset_registry()
   assert(!syntax.is_macro_registered("double"),
   "reset should clear custom macro")
   assert(!syntax.is_attr_registered("mark"),
   "reset should clear custom attribute")
   assert(syntax.is_attr_registered("effects"),
   "reset should re-register builtin attributes")
}

fn test_registry_clone_merge_ops(){
   mut base = syntax.new_registry()
   base = syntax.register_macro_in(base, "double", __macro_double)
   base = syntax.register_attribute_in(base, "mark", __attr_mark)
   mut cloned = syntax.clone_registry_in(base)
   assert(is_dict(cloned), "clone_registry_in should return registry dict")
   cloned = syntax.register_macro_in(cloned, "add2", __macro_add2)
   assert(!syntax.is_macro_registered_in(base, "add2"),
   "cloned registry updates should not mutate source")
   cloned = syntax.unregister_macro_in(cloned, "double")
   assert(syntax.is_macro_registered_in(base, "double"),
   "cloned unregister should not mutate source")
   mut incoming = syntax.new_registry()
   incoming = syntax.register_macro_in(incoming, "double", __macro_double_plus1)
   incoming = syntax.register_macro_in(incoming, "add2", __macro_add2)
   incoming = syntax.register_attribute_in(incoming, "mark2", __attr_mark)
   base = syntax.merge_registry_in(base, incoming, false)
   assert(syntax.expand_macro_in(base, "double", [5]) == 10,
   "merge_registry_in without overwrite should keep existing handlers")
   assert(syntax.expand_macro_in(base, "add2", [20, 22]) == 42,
   "merge_registry_in should add missing handlers")
   assert(syntax.is_attr_registered_in(base, "mark2"),
   "merge_registry_in should add missing attributes")
   def merged_macros = syntax.list_macros_in(base)
   assert(merged_macros.len == 2,
   "merged macro list should include destination + incoming entries")
   assert(merged_macros.get(0, "") == "double",
   "merged macro order should keep destination registrations first")
   assert(merged_macros.get(1, "") == "add2",
   "merged macro order should append incoming registrations")
   base = syntax.merge_registry_in(base, incoming, true)
   assert(syntax.expand_macro_in(base, "double", [5]) == 11,
   "merge_registry_in with overwrite should replace existing handlers")
   syntax.reset_registry()
   syntax.register_macro("double", __macro_double)
   assert(is_dict(syntax.clone_registry()), "clone_registry should clone global registry")
   syntax.merge_registry(incoming, true)
   assert(syntax.expand_macro("double", [5]) == 11,
   "merge_registry should update process-wide registry")
   syntax.unregister_macro("double")
   assert(!syntax.is_macro_registered("double"),
   "unregister_macro should remove process-wide macro handler")
   syntax.register_attribute("mark", __attr_mark)
   syntax.unregister_attribute("mark")
   assert(!syntax.is_attr_registered("mark"),
   "unregister_attribute should remove process-wide attribute handler")
   syntax.reset_registry()
}

fn test_form_expansion_surface(){
   mut reg = syntax.new_registry()
   reg = syntax.register_macro_in(reg, "double", __macro_double)
   reg = syntax.register_macro_in(reg, "double_form", __macro_to_double_form)
   reg = syntax.register_macro_in(reg, "add2", __macro_add2)
   def expr = syntax.form("double_form", [21])
   assert(syntax.is_form(expr, "double_form"), "form head should match")
   assert(syntax.form_head(expr, "") == "double_form",
   "form_head should expose macro name")
   def tail = syntax.form_tail(expr)
   assert(tail.len == 1 && tail.get(0, 0) == 21,
   "form_tail should preserve arguments")
   def out_form = syntax.expand_form_in(reg, expr)
   assert(out_form == 42, "expand_form_in should follow macro chain")
   def out_macro = syntax.expand_macro_fixpoint_in(reg, "double_form", [21])
   assert(out_macro == 42, "expand_macro_fixpoint_in should follow macro chain")
   assert(syntax.expand_macro_in(reg, "double", [8]) == 16,
   "expand_macro_in should use provided registry")
   syntax.reset_registry()
   syntax.register_macro("double", __macro_double)
   assert(syntax.expand_form(syntax.form("double", [9])) == 18,
   "expand_form should use process-wide registry")
   def nested = syntax.form("begin", [
         syntax.form("add2", [20, 22]),
         [syntax.form("double", [3]), syntax.form("double", [5])]
   ])
   def deep1 = syntax.expand_form_deep_in(reg, nested)
   assert(syntax.is_form(deep1, "begin"), "deep expansion should keep non-macro head")
   assert(deep1.get(1, 0) == 42, "nested direct form should expand")
   def inner = deep1.get(2, list(0))
   assert(is_list(inner), "nested list should remain list")
   assert(inner.get(0, 0) == 6, "first nested form should expand")
   assert(inner.get(1, 0) == 10, "second nested form should expand")
   def deep2 = syntax.expand_form_deep_in(reg, deep1)
   assert(eq(deep2, deep1),
   "deep expansion should be idempotent after reaching fixpoint")
   mut cyc = syntax.new_registry()
   cyc = syntax.register_macro_in(cyc, "ping", __macro_ping)
   cyc = syntax.register_macro_in(cyc, "pong", __macro_pong)
   def limited = syntax.expand_macro_fixpoint_in(cyc, "ping", [1], 0, 0, 3)
   assert(is_dict(limited), "limited cycle expansion should return macro node")
   assert(limited.get("name", "") == "pong",
   "fixpoint max_steps should stop deterministically")
}

fn test_attribute_surface_and_fallback(){
   mut reg = syntax.new_registry()
   reg = syntax.register_attribute_in(reg, "mark", __attr_mark)
   mut node = dict(2)
   node = node.set("name", "node1")
   def unchanged = syntax.apply_attribute_in(reg, "missing", node, ["x"])
   assert(is_dict(unchanged), "missing attribute should return original node")
   assert(!unchanged.get("marked", false),
   "missing attribute should not mutate node metadata")
   def marked = syntax.apply_attribute_in(reg, "mark", node, ["a", "b"])
   assert(marked.get("marked", false),
   "local attribute handler should apply")
   assert(marked.get("arg_count", 0) == 2,
   "local attribute handler should receive args")
   syntax.reset_registry()
   mut fn_node = dict(4)
   fn_node = fn_node.set("name", "demo_fn")
   def effects_node = syntax.apply_attribute("effects", fn_node, ["none"])
   assert(effects_node.get("effect_contract_known", false),
   "builtin @effects should set known flag")
   def effect_contract = effects_node.get("effect_contract", list(0))
   assert(effect_contract.len == 1 && effect_contract.get(0, "") == "none",
   "builtin @effects should keep provided effect contract")
   def llvm_node = syntax.apply_attribute("llvm", fn_node, ["noinline"])
   assert(llvm_node.get("llvm_attr", "") == "noinline",
   "builtin @llvm should set llvm attribute name")
   def extern_node = syntax.apply_attribute("extern", fn_node, ["puts"])
   assert(extern_node.get("is_extern", false),
   "builtin @extern should mark extern")
   assert(extern_node.get("link_name", "") == "puts",
   "builtin @extern should set link name")
}

fn test_rewriter_surface(){
   mut rw = syntax.new_rewriter()
   rw = syntax.register_rewrite(rw, "add1", __rw_add1)
   rw = syntax.register_rewrite(rw, "mul2", __rw_mul2)
   def names = syntax.list_rewrites(rw)
   assert(names.len == 2, "rewriter should expose deterministic pass list")
   assert(names.get(0, "") == "add1", "first rewrite should preserve order")
   assert(names.get(1, "") == "mul2", "second rewrite should preserve order")
   assert(syntax.rewrite_once(rw, 10) == 22,
   "rewrite_once should run handlers in registration order")
   rw = syntax.clear_rewriter(rw)
   assert(len(syntax.list_rewrites(rw)) == 0, "clear_rewriter should remove rules")
   assert(syntax.rewrite_once(rw, 10) == 10,
   "rewrite_once should return input when no rules exist")
   rw = syntax.register_rewrite(rw, "shift", __rw_shift)
   rw = syntax.register_rewrite(rw, "eval_inc1", __rw_eval_inc1)
   assert(syntax.rewrite_fixpoint(rw, syntax.form("shift", [41])) == 42,
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
   assert(caught, "empty macro name should fail")
   caught = false
   try {
      syntax.register_attribute("", __attr_mark)
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   assert(caught, "empty attribute name should fail")
   syntax.reset_registry()
   caught = false
   try {
      syntax.apply_attribute("effects", dict(2), list(0))
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   assert(caught, "@effects without args should fail")
   caught = false
   try {
      syntax.apply_attribute("extern", dict(2), [123])
   } catch e {
      caught = true
      if(e){ caught = true }
   }
   assert(caught, "@extern with non-string argument should fail")
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
