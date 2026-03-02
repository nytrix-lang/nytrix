;; Keywords: core syntax
;; Core Syntax internals.

module std.core.syntax.syntax (
   new_registry, clear_registry,
   clone_registry, merge_registry,
   register_macro, unregister_macro,
   register_attribute, unregister_attribute,
   get_macro_handler, get_attr_handler,
   is_macro_registered, is_attr_registered,
   list_macros, list_attributes,
   form, is_form, form_head, form_tail,
   expand_macro, expand_macro_fixpoint,
   expand_form, expand_form_deep,
   new_rewriter, clear_rewriter,
   register_rewrite, list_rewrites,
   rewrite_once, rewrite_fixpoint,
   apply_attribute
)
use std.core *
use std.core.dict *
use std.core.reflect as core_ref

fn _ensure_registry(reg){
   "Internal helper."
   if(!is_dict(reg)){ panic("syntax registry must be a dict") }
   reg
}

fn _ensure_name(name){
   "Internal helper."
   if(!is_str(name)){ panic("syntax name must be a string") }
   if(core_ref.len(name) == 0){ panic("syntax name cannot be empty") }
   name
}

fn _ensure_handler(handler){
   "Internal helper."
   if(!handler){ panic("syntax handler cannot be none") }
   handler
}

fn _to_list(x){
   "Internal helper."
   if(is_list(x)){ return x }
   list(0)
}

fn _empty_list(){
   "Internal helper."
   list(0)
}

fn _list_without(xs, want){
   "Internal helper."
   mut out = list(core_ref.len(xs))
   mut i = 0
   while(i < core_ref.len(xs)){
      def x = get(xs, i, 0)
      if(!core_ref.eq(x, want)){ out = append(out, x) }
      i += 1
   }
   out
}

fn _registry_dict(reg, key, cap=8){
   "Internal helper."
   def v = dict_get(reg, key, 0)
   if(is_dict(v)){ return v }
   dict(cap)
}

fn _registry_list(reg, key, cap=8){
   "Internal helper."
   def v = dict_get(reg, key, 0)
   if(is_list(v)){ return v }
   list(cap)
}

fn _ensure_rewriter(rw){
   "Internal helper."
   if(!is_dict(rw)){ panic("syntax rewriter must be a dict") }
   rw
}

fn _rewriter_dict(rw, key, cap=8){
   "Internal helper."
   def v = dict_get(rw, key, 0)
   if(is_dict(v)){ return v }
   dict(cap)
}

fn _rewriter_list(rw, key, cap=8){
   "Internal helper."
   def v = dict_get(rw, key, 0)
   if(is_list(v)){ return v }
   list(cap)
}

fn new_registry(cap=8){
   "Creates a new syntax registry object."
   mut reg = dict(8)
   reg = dict_set(reg, "macros", dict(cap))
   reg = dict_set(reg, "macro_order", list(cap))
   reg = dict_set(reg, "attrs", dict(cap))
   reg = dict_set(reg, "attr_order", list(cap))
   reg
}

fn clear_registry(reg){
   "Clears all handlers from a registry while preserving object identity."
   reg = _ensure_registry(reg)
   reg = dict_set(reg, "macros", dict(8))
   reg = dict_set(reg, "macro_order", list(8))
   reg = dict_set(reg, "attrs", dict(8))
   reg = dict_set(reg, "attr_order", list(8))
   reg
}

fn clone_registry(reg){
   "Returns a structural clone of registry `reg`."
   reg = _ensure_registry(reg)
   mut out = dict(8)
   out = dict_set(out, "macros", dict_clone(_registry_dict(reg, "macros")))
   out = dict_set(out, "macro_order", list_clone(_registry_list(reg, "macro_order")))
   out = dict_set(out, "attrs", dict_clone(_registry_dict(reg, "attrs")))
   out = dict_set(out, "attr_order", list_clone(_registry_list(reg, "attr_order")))
   out
}

fn register_macro(reg, name, handler){
   "Registers a macro handler in `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   handler = _ensure_handler(handler)
   mut macros = _registry_dict(reg, "macros")
   mut order = _registry_list(reg, "macro_order")
   def existed = dict_has(macros, name)
   macros = dict_set(macros, name, handler)
   reg = dict_set(reg, "macros", macros)
   if(!existed){
      order = append(order, name)
      reg = dict_set(reg, "macro_order", order)
   }
   reg
}

fn unregister_macro(reg, name){
   "Unregisters macro handler `name` from `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   mut macros = _registry_dict(reg, "macros")
   mut order = _registry_list(reg, "macro_order")
   if(!dict_has(macros, name)){ return reg }
   macros = dict_del(macros, name)
   order = _list_without(order, name)
   reg = dict_set(reg, "macros", macros)
   reg = dict_set(reg, "macro_order", order)
   reg
}

fn register_attribute(reg, name, handler){
   "Registers an attribute handler in `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   handler = _ensure_handler(handler)
   mut attrs = _registry_dict(reg, "attrs")
   mut order = _registry_list(reg, "attr_order")
   def existed = dict_has(attrs, name)
   attrs = dict_set(attrs, name, handler)
   reg = dict_set(reg, "attrs", attrs)
   if(!existed){
      order = append(order, name)
      reg = dict_set(reg, "attr_order", order)
   }
   reg
}

fn unregister_attribute(reg, name){
   "Unregisters attribute handler `name` from `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   mut attrs = _registry_dict(reg, "attrs")
   mut order = _registry_list(reg, "attr_order")
   if(!dict_has(attrs, name)){ return reg }
   attrs = dict_del(attrs, name)
   order = _list_without(order, name)
   reg = dict_set(reg, "attrs", attrs)
   reg = dict_set(reg, "attr_order", order)
   reg
}

fn _merge_macros(dst, src, overwrite=true){
   "Internal helper."
   def src_macros = _registry_dict(src, "macros")
   def src_order = _registry_list(src, "macro_order")
   mut i = 0
   while(i < core_ref.len(src_order)){
      def name = get(src_order, i, "")
      if(is_str(name) && core_ref.len(name) > 0){
         def handler = dict_get(src_macros, name, 0)
         if(handler){
            if(!is_macro_registered(dst, name) || overwrite){
               dst = register_macro(dst, name, handler)
            }
         }
      }
      i += 1
   }
   dst
}

fn _merge_attributes(dst, src, overwrite=true){
   "Internal helper."
   def src_attrs = _registry_dict(src, "attrs")
   def src_order = _registry_list(src, "attr_order")
   mut i = 0
   while(i < core_ref.len(src_order)){
      def name = get(src_order, i, "")
      if(is_str(name) && core_ref.len(name) > 0){
         def handler = dict_get(src_attrs, name, 0)
         if(handler){
            if(!is_attr_registered(dst, name) || overwrite){
               dst = register_attribute(dst, name, handler)
            }
         }
      }
      i += 1
   }
   dst
}

fn merge_registry(dst, src, overwrite=true){
   "Merges `src` handlers into `dst` using deterministic source registration order."
   dst = _ensure_registry(dst)
   src = _ensure_registry(src)
   dst = _merge_macros(dst, src, overwrite)
   dst = _merge_attributes(dst, src, overwrite)
   dst
}

fn get_macro_handler(reg, name){
   "Returns macro handler for `name`, or none."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def macros = _registry_dict(reg, "macros")
   dict_get(macros, name, 0)
}

fn get_attr_handler(reg, name){
   "Returns attribute handler for `name`, or none."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def attrs = _registry_dict(reg, "attrs")
   dict_get(attrs, name, 0)
}

fn is_macro_registered(reg, name){
   "Returns true when a macro handler exists for `name`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def macros = _registry_dict(reg, "macros")
   dict_has(macros, name)
}

fn is_attr_registered(reg, name){
   "Returns true when an attribute handler exists for `name`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def attrs = _registry_dict(reg, "attrs")
   dict_has(attrs, name)
}

fn list_macros(reg){
   "Returns macro names in deterministic registration order."
   reg = _ensure_registry(reg)
   def order = _registry_list(reg, "macro_order")
   list_clone(order)
}

fn list_attributes(reg){
   "Returns attribute names in deterministic registration order."
   reg = _ensure_registry(reg)
   def order = _registry_list(reg, "attr_order")
   list_clone(order)
}

fn _macro_node(name, args, body, tok){
   "Internal helper."
   mut node = dict(8)
   node = dict_set(node, "name", name)
   node = dict_set(node, "args", _to_list(args))
   node = dict_set(node, "body", body)
   node = dict_set(node, "tok", tok)
   node
}

fn _is_macro_node(node){
   "Internal helper."
   if(!is_dict(node)){ return false }
   is_str(dict_get(node, "name", 0))
}

fn _macro_node_name(node){
   "Internal helper."
   if(!_is_macro_node(node)){ return "" }
   dict_get(node, "name", "")
}

fn _macro_node_args(node){
   "Internal helper."
   if(!_is_macro_node(node)){ return _empty_list() }
   _to_list(dict_get(node, "args", _empty_list()))
}

fn _to_macro_node(value, tok=0){
   "Internal helper."
   if(_is_macro_node(value)){ return value }
   if(is_form(value)){
      return _macro_node(form_head(value), form_tail(value), 0, tok)
   }
   0
}

fn _macro_node_to_form(node){
   "Internal helper."
   if(!_is_macro_node(node)){ return node }
   mut out = append(list(0), _macro_node_name(node))
   def args = _macro_node_args(node)
   mut i = 0
   while(i < core_ref.len(args)){
      out = append(out, get(args, i, 0))
      i += 1
   }
   out
}

fn form(head, args=0){
   "Builds an s-expression-style form list `[head, ...args]`."
   mut out = append(list(0), head)
   def tail = _to_list(args)
   mut i = 0
   while(i < core_ref.len(tail)){
      out = append(out, get(tail, i, 0))
      i += 1
   }
   out
}

fn is_form(value, head=0){
   "Returns true when `value` is a list form with optional matching head."
   if(!is_list(value)){ return false }
   if(core_ref.len(value) == 0){ return false }
   if(head == 0){ return true }
   get(value, 0, 0) == head
}

fn form_head(value, default=0){
   "Returns form head or `default`."
   if(!is_form(value)){ return default }
   get(value, 0, default)
}

fn form_tail(value){
   "Returns tail elements from a form."
   if(!is_form(value)){ return list(0) }
   slice(value, 1, core_ref.len(value), 1)
}

fn expand_macro(reg, name, args=0, body=0, tok=0){
   "Expands macro `name`. Returns none when no handler exists."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def handler = get_macro_handler(reg, name)
   if(!handler){ return 0 }
   def node = _macro_node(name, args, body, tok)
   handler(node)
}

fn expand_macro_fixpoint(reg, name, args=0, body=0, tok=0, max_steps=64){
   "Expands macro repeatedly until stable or `max_steps` is reached."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   mut current = _macro_node(name, args, body, tok)
   mut steps = 0
   while(steps < max_steps){
      def cur_name = _macro_node_name(current)
      if(!is_str(cur_name) || core_ref.len(cur_name) == 0){ return current }
      def handler = get_macro_handler(reg, cur_name)
      if(!handler){ return current }
      def out = handler(current)
      def next = _to_macro_node(out, tok)
      if(!next){ return out }
      if(core_ref.eq(current, next)){ return next }
      current = next
      steps += 1
   }
   current
}

fn expand_form(reg, value, tok=0, max_steps=64){
   "Expands a Lisp-style form `[head, ...args]` through macro registry."
   reg = _ensure_registry(reg)
   if(!is_form(value)){ return value }
   def name = form_head(value, "")
   if(!is_str(name) || core_ref.len(name) == 0){ return value }
   def args = form_tail(value)
   def out = expand_macro_fixpoint(reg, name, args, 0, tok, max_steps)
   def node = _to_macro_node(out, tok)
   if(!node){ return out }
   _macro_node_to_form(node)
}

fn _expand_form_list(reg, xs, tok=0, max_steps=64){
   "Internal helper."
   mut out = list(core_ref.len(xs))
   mut i = 0
   while(i < core_ref.len(xs)){
      out = append(out, expand_form_deep(reg, get(xs, i, 0), tok, max_steps))
      i += 1
   }
   out
}

fn expand_form_deep(reg, value, tok=0, max_steps=64){
   "Recursively expands forms in nested lists until stable."
   reg = _ensure_registry(reg)
   if(max_steps <= 0){ return value }
   if(is_form(value)){
      def expanded = expand_form(reg, value, tok, max_steps)
      if(!core_ref.eq(expanded, value)){
         return expand_form_deep(reg, expanded, tok, max_steps - 1)
      }
   }
   if(!is_list(value)){ return value }
   _expand_form_list(reg, value, tok, max_steps)
}

fn new_rewriter(cap=8){
   "Creates a deterministic rewrite pipeline object."
   mut rw = dict(4)
   rw = dict_set(rw, "rules", dict(cap))
   rw = dict_set(rw, "rule_order", list(cap))
   rw
}

fn clear_rewriter(rw){
   "Clears rewrite rules while preserving object identity."
   rw = _ensure_rewriter(rw)
   rw = dict_set(rw, "rules", dict(8))
   rw = dict_set(rw, "rule_order", list(8))
   rw
}

fn register_rewrite(rw, name, handler){
   "Registers a rewrite handler in deterministic order."
   rw = _ensure_rewriter(rw)
   name = _ensure_name(name)
   handler = _ensure_handler(handler)
   mut rules = _rewriter_dict(rw, "rules")
   mut order = _rewriter_list(rw, "rule_order")
   def existed = dict_has(rules, name)
   rules = dict_set(rules, name, handler)
   rw = dict_set(rw, "rules", rules)
   if(!existed){
      order = append(order, name)
      rw = dict_set(rw, "rule_order", order)
   }
   rw
}

fn list_rewrites(rw){
   "Returns registered rewrite names in deterministic order."
   rw = _ensure_rewriter(rw)
   def order = _rewriter_list(rw, "rule_order")
   list_clone(order)
}

fn rewrite_once(rw, value){
   "Runs one rewrite pass over `value` using registration order."
   rw = _ensure_rewriter(rw)
   def order = _rewriter_list(rw, "rule_order")
   def rules = _rewriter_dict(rw, "rules")
   mut out = value
   mut i = 0
   while(i < core_ref.len(order)){
      def name = get(order, i, "")
      if(is_str(name) && core_ref.len(name) > 0){
         def handler = dict_get(rules, name, 0)
         if(handler){ out = handler(out) }
      }
      i += 1
   }
   out
}

fn rewrite_fixpoint(rw, value, max_steps=64){
   "Runs rewrite passes until output stabilizes or step limit is reached."
   rw = _ensure_rewriter(rw)
   mut current = value
   mut steps = 0
   while(steps < max_steps){
      def next = rewrite_once(rw, current)
      if(core_ref.eq(current, next)){ return next }
      current = next
      steps += 1
   }
   current
}

fn apply_attribute(reg, name, node, args=0){
   "Applies attribute `name` to `node`. Returns original node on fallback."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def handler = get_attr_handler(reg, name)
   if(!handler){ return node }
   handler(node, _to_list(args))
}

if(comptime{__main()}){

    fn macro_id(node){
       "Test helper."
       node
    }

    fn attr_mark(node, args){
       "Test helper."
       dict_set(node, "marked", true)
    }

    fn rw_double(v){
       "Test helper."
       if(is_int(v)){ return v * 2 }
       v
    }

    mut reg = new_registry(8)
    assert(len(list_macros(reg)) == 0, "new_registry macros")
    assert(len(list_attributes(reg)) == 0, "new_registry attrs")

    reg = register_macro(reg, "id", macro_id)
    assert(is_macro_registered(reg, "id"), "register_macro")
    assert(get_macro_handler(reg, "id"), "get_macro_handler")

    def f = form("id", [10, 20])
    assert(is_form(f, "id"), "is_form")
    assert(form_head(f, "") == "id", "form_head")
    assert(len(form_tail(f)) == 2, "form_tail")

    def expanded = expand_form(reg, f)
    assert(is_form(expanded, "id"), "expand_form")
    assert(form_head(expanded, "") == "id", "expand_form head")
    assert(len(form_tail(expanded)) == 2, "expand_form tail")

    reg = register_attribute(reg, "mark", attr_mark)
    assert(is_attr_registered(reg, "mark"), "register_attribute")
    def n = apply_attribute(reg, "mark", dict(2), list(0))
    assert(dict_get(n, "marked", false), "apply_attribute")

    mut reg2 = new_registry(4)
    reg2 = register_macro(reg2, "id2", macro_id)
    reg = merge_registry(reg, reg2, true)
    assert(is_macro_registered(reg, "id2"), "merge_registry")

    reg = unregister_macro(reg, "id")
    assert(!is_macro_registered(reg, "id"), "unregister_macro")
    reg = unregister_attribute(reg, "mark")
    assert(!is_attr_registered(reg, "mark"), "unregister_attribute")

    mut rw = new_rewriter(4)
    rw = register_rewrite(rw, "double", rw_double)
    assert(contains(list_rewrites(rw), "double"), "register_rewrite")
    assert(rewrite_once(rw, 3) == 6, "rewrite_once")
    assert(rewrite_fixpoint(rw, 3, 1) == 6, "rewrite_fixpoint")
    rw = clear_rewriter(rw)
    assert(len(list_rewrites(rw)) == 0, "clear_rewriter")
}
