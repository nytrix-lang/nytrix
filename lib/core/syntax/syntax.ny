;; Keywords: core syntax parser ast
;; Core Syntax internals.
;; References:
;; - std.core.syntax
;; - std.core
module std.core.syntax.syntax(new_registry, clear_registry, clone_registry, merge_registry, register_macro, unregister_macro, register_attribute, unregister_attribute, get_macro_handler, get_attr_handler, is_macro_registered, is_attr_registered, list_macros, list_attributes, form, is_form, form_head, form_tail, expand_macro, expand_macro_fixpoint, expand_form, expand_form_deep, new_rewriter, clear_rewriter, register_rewrite, list_rewrites, rewrite_once, rewrite_fixpoint, apply_attribute)
use std.core
use std.core.dict_mod
use std.core.reflect as core_ref

fn _ensure_registry(dict reg) dict {
   if type(reg) != "dict" { panic("syntax registry must be a dict") }
   reg
}

fn _ensure_name(str name) str {
   ; Typed `str` parameters may be raw constant pointers in native code;
   ; `is_str` only recognizes managed objects.  The string ABI already
   ; validated this parameter, so validate the content through its length.
   if !name || __str_len(name) == 0 { panic("syntax name cannot be empty") }
   name
}

fn _ensure_handler(any handler) any {
   if !handler { panic("syntax handler cannot be nil") }
   handler
}

fn _to_list(any x) list {
   if is_list(x) { return x }
   list(0)
}

fn _empty_list() list { list(0) }

fn _clone_list(list xs) list {
   if type(xs) != "list" { return list(0) }
   def n = xs.len
   mut out = list(n)
   mut i = 0
   while i < n {
      out = out.append(xs.get(i, 0))
      i += 1
   }
   out
}

fn _list_without(list xs, any want) list {
   def n = xs.len
   mut out = list(n)
   mut i = 0
   while i < n {
      def x = xs.get(i, 0)
      ; Registry names are immutable C-string values in native lists.  Keep
      ; the equality in the positive branch: the native `!=` lowering has a
      ; different boolean ABI for raw string pointers and can invert the
      ; filter at this dynamic boundary.
      if x == want { } else { out = out.append(x) }
      i += 1
   }
   out
}

fn _registry_dict(dict reg, str key, int cap=8) dict {
   def v = reg.get(key, 0)
   if type(v) == "dict" { return v }
   dict(cap)
}

fn _registry_list(dict reg, str key, int cap=8) list {
   def v = reg.get(key, 0)
   if type(v) == "list" { return v }
   list(cap)
}

fn _ensure_rewriter(dict rw) dict {
   if type(rw) != "dict" { panic("syntax rewriter must be a dict") }
   rw
}

fn _rewriter_dict(dict rw, str key, int cap=8) dict {
   def v = rw.get(key, 0)
   if type(v) == "dict" { return v }
   dict(cap)
}

fn _rewriter_list(dict rw, str key, int cap=8) list {
   def v = rw.get(key, 0)
   if type(v) == "list" { return v }
   list(cap)
}

fn new_registry(int cap=8) dict {
   "Creates a new syntax registry object."
   return {
      "macros": dict(cap),
      "macro_order": list(cap),
      "attrs": dict(cap),
      "attr_order": list(cap)
   }
}

fn clear_registry(dict reg) dict {
   "Clears all handlers from a registry while preserving object identity."
   reg = _ensure_registry(reg)
   reg = dict_set(reg, "macros", dict(8))
   reg = dict_set(reg, "macro_order", list(8))
   reg = dict_set(reg, "attrs", dict(8))
   reg = dict_set(reg, "attr_order", list(8))
   reg
}

fn clone_registry(dict reg) dict {
   "Returns a structural clone of registry `reg`."
   reg = _ensure_registry(reg)
   return {
      "macros": dict_clone(_registry_dict(reg, "macros")),
      "macro_order": _clone_list(_registry_list(reg, "macro_order")),
      "attrs": dict_clone(_registry_dict(reg, "attrs")),
      "attr_order": _clone_list(_registry_list(reg, "attr_order"))
   }
}

fn register_macro(dict reg, str name, any handler) dict {
   "Registers a macro handler in `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   handler = _ensure_handler(handler)
   mut macros = _registry_dict(reg, "macros")
   mut order = _registry_list(reg, "macro_order")
   def existed = __dict_has_str_raw(macros, name) != 0
   macros = dict_set(macros, name, handler)
   reg = dict_set(reg, "macros", macros)
   if !existed {
      if type(order) != "list" { order = list(8) }
      order = order.append(name)
      reg = dict_set(reg, "macro_order", order)
   }
   reg
}

fn unregister_macro(dict reg, str name) dict {
   "Unregisters macro handler `name` from `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   mut macros = _registry_dict(reg, "macros")
   mut order = _registry_list(reg, "macro_order")
   ; `name` is validated as a string above.  Call the string-key bridge
   ; directly so packed native C-string pointers cannot fall through the
   ; dynamic `is_str` classifier and be hashed as integers.
   macros = __dict_delete_str_raw(macros, name)
   order = _list_without(order, name)
   reg = dict_set(reg, "macros", macros)
   reg = dict_set(reg, "macro_order", order)
   reg
}

fn register_attribute(dict reg, str name, any handler) dict {
   "Registers an attribute handler in `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   handler = _ensure_handler(handler)
   mut attrs = _registry_dict(reg, "attrs")
   mut order = _registry_list(reg, "attr_order")
   def existed = __dict_has_str_raw(attrs, name) != 0
   attrs = dict_set(attrs, name, handler)
   reg = dict_set(reg, "attrs", attrs)
   if !existed {
      if type(order) != "list" { order = list(8) }
      order = order.append(name)
      reg = dict_set(reg, "attr_order", order)
   }
   reg
}

fn unregister_attribute(dict reg, str name) dict {
   "Unregisters attribute handler `name` from `reg`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   mut attrs = _registry_dict(reg, "attrs")
   mut order = _registry_list(reg, "attr_order")
   attrs = __dict_delete_str_raw(attrs, name)
   order = _list_without(order, name)
   reg = dict_set(reg, "attrs", attrs)
   reg = dict_set(reg, "attr_order", order)
   reg
}

fn _merge_macros(dict dst, dict src, bool overwrite=true) dict {
   def src_macros = _registry_dict(src, "macros")
   def src_order = _registry_list(src, "macro_order")
   mut i = 0
   while i < src_order.len {
      def name = src_order.get(i, "")
      if name && name.len > 0 {
         def handler = __dict_get_str_raw(src_macros, name, nil)
         ; A callable handler can be a raw closure pointer whose dynamic
         ; truthiness is not a reliable presence test.  The string-key map is
         ; authoritative; only a missing key means there is nothing to merge.
         if __dict_has_str_raw(src_macros, name) != 0 {
            if !__dict_has_str_raw(_registry_dict(dst, "macros"), name) || overwrite {
               dst = register_macro(dst, name, handler)
            }
         }
      }
      i += 1
   }
   dst
}

fn _merge_attributes(dict dst, dict src, bool overwrite=true) dict {
   def src_attrs = _registry_dict(src, "attrs")
   def src_order = _registry_list(src, "attr_order")
   mut i = 0
   while i < src_order.len {
      def name = src_order.get(i, "")
      if name && name.len > 0 {
         def handler = __dict_get_str_raw(src_attrs, name, nil)
         if __dict_has_str_raw(src_attrs, name) != 0 {
            if !__dict_has_str_raw(_registry_dict(dst, "attrs"), name) || overwrite {
               dst = register_attribute(dst, name, handler)
            }
         }
      }
      i += 1
   }
   dst
}

fn merge_registry(dict dst, dict src, bool overwrite=true) dict {
   "Merges `src` handlers into `dst` using deterministic source registration order."
   dst = _ensure_registry(dst)
   src = _ensure_registry(src)
   dst = _merge_macros(dst, src, overwrite)
   dst = _merge_attributes(dst, src, overwrite)
   dst
}

fn get_macro_handler(dict reg, str name) any {
   "Returns macro handler for `name`, or nil."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def macros = _registry_dict(reg, "macros")
   __dict_get_str_raw(macros, name, nil)
}

fn get_attr_handler(dict reg, str name) any {
   "Returns attribute handler for `name`, or nil."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def attrs = _registry_dict(reg, "attrs")
   __dict_get_str_raw(attrs, name, nil)
}

fn is_macro_registered(dict reg, str name) bool {
   "Returns true when a macro handler exists for `name`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def macros = _registry_dict(reg, "macros")
   __dict_has_str_raw(macros, name) != 0
}

fn is_attr_registered(dict reg, str name) bool {
   "Returns true when an attribute handler exists for `name`."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def attrs = _registry_dict(reg, "attrs")
   __dict_has_str_raw(attrs, name) != 0
}

fn list_macros(dict reg) list {
   "Returns macro names in deterministic registration order."
   reg = _ensure_registry(reg)
   def order = _registry_list(reg, "macro_order")
   _clone_list(order)
}

fn list_attributes(dict reg) list {
   "Returns attribute names in deterministic registration order."
   reg = _ensure_registry(reg)
   def order = _registry_list(reg, "attr_order")
   _clone_list(order)
}

fn _macro_node(str name, any args, any body, any tok) dict {
   return {
      "name": name,
      "args": _to_list(args),
      "body": body,
      "tok": tok
   }
}

fn _is_macro_node(any node) bool {
   if type(node) != "dict" { return false }
   def name = node.get("name", 0)
   name && __str_len(name) > 0
}

fn _macro_node_name(dict node) str {
   if !_is_macro_node(node) { return "" }
   node.get("name", "")
}

fn _macro_node_args(dict node) list {
   if !_is_macro_node(node) { return _empty_list() }
   _to_list(node.get("args", _empty_list()))
}

fn _to_macro_node(any value, any tok=0) any {
   if _is_macro_node(value) { return value }
   ; Macro handlers conventionally return `[name, ...args]`. Use the list
   ; representation directly here: the native ABI may carry a valid list
   ; without enough dynamic metadata for the broader `is_form` predicate.
   if is_list(value) {
      def n = value.len
      if n > 0 {
         mut args = list(0)
         mut i = 1
         while i < n {
            args = args.append(value.get(i, 0))
            i += 1
         }
         return _macro_node(value.get(0, 0), args, 0, tok)
      }
   }
   0
}

fn _macro_node_to_form(any node) any {
   if !_is_macro_node(node) { return node }
   mut out = list(0).append(_macro_node_name(node))
   def args = _macro_node_args(node)
   mut i = 0
   while i < args.len {
      out = out.append(args.get(i, 0))
      i += 1
   }
   out
}

fn form(any head, any args=0) list {
   "Builds an s-expression-style form list `[head, ...args]`."
   mut out = list(0).append(head)
   def tail = _to_list(args)
   mut i = 0
   while i < tail.len {
      out = out.append(tail.get(i, 0))
      i += 1
   }
   out
}

fn is_form(any value, any head=0) bool {
   "Returns true when `value` is a list form with optional matching head."
   if !is_list(value) { return false }
   if value.len == 0 { return false }
   if head == 0 { return true }
   value.get(0, 0) == head
}

fn form_head(any value, any default=0) any {
   "Returns form head or `default`."
   if !is_form(value) { return default }
   value.get(0, default)
}

fn form_tail(any value) list {
   "Returns tail elements from a form."
   if !is_form(value) { return list(0) }
   ; Copy explicitly through indexed reads so a polymorphic slice result does
   ; not lose the native tbuf payload metadata at this concrete list boundary.
   def n = value.len
   mut out = list(n)
   mut i = 1
   while i < n {
      out = out.append(value.get(i, 0))
      i += 1
   }
   out
}

fn expand_macro(dict reg, str name, any args=0, any body=0, any tok=0) any {
   "Expands macro `name`. Returns nil when no handler exists."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def handler = get_macro_handler(reg, name)
   if !handler { return 0 }
   def node = _macro_node(name, args, body, tok)
   handler(node)
}

fn expand_macro_fixpoint(dict reg, str name, any args=0, any body=0, any tok=0, int max_steps=64) any {
   "Expands macro repeatedly until stable or `max_steps` is reached."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   mut current = _macro_node(name, args, body, tok)
   mut steps = 0
   while steps < max_steps {
      def cur_name = _macro_node_name(current)
      if !cur_name || __str_len(cur_name) == 0 { return current }
      def handler = get_macro_handler(reg, cur_name)
      if !handler { return current }
      def out = handler(current)
      def next = _to_macro_node(out, tok)
      if !next { return out }
      if core_ref.eq(current, next) { return next }
      current = next
      steps += 1
   }
   current
}

fn expand_form(dict reg, any value, any tok=0, int max_steps=64) any {
   "Expands a Lisp-style form `[head, ...args]` through macro registry."
   reg = _ensure_registry(reg)
   if !is_form(value) { return value }
   def name = form_head(value, "")
   if !name || __str_len(name) == 0 { return value }
   def args = form_tail(value)
   def out = expand_macro_fixpoint(reg, name, args, 0, tok, max_steps)
   if !is_form(out) { return out }
   def node = _to_macro_node(out, tok)
   if !node { return out }
   _macro_node_to_form(node)
}

fn _expand_form_list(dict reg, list xs, any tok=0, int max_steps=64) list {
   mut out = list(xs.len)
   mut i = 0
   while i < xs.len {
      out = out.append(expand_form_deep(reg, xs.get(i, 0), tok, max_steps))
      i += 1
   }
   out
}

fn expand_form_deep(dict reg, any value, any tok=0, int max_steps=64) any {
   "Recursively expands forms in nested lists until stable."
   reg = _ensure_registry(reg)
   if max_steps <= 0 { return value }
   if is_list(value) {
      ; A list whose head is itself a form is a nested sequence, not a macro
      ; form. Traverse it instead of treating the pointer as a string name.
      def head = form_head(value, 0)
      if is_form(head) {
         return _expand_form_list(reg, value, tok, max_steps)
      }
      ; Unknown heads are ordinary list data. Only invoke the macro pipeline
      ; when the registry actually owns the head, otherwise `expand_form`
      ; would expose its internal macro-node dictionary to callers.
      if is_str(head) {
         if is_macro_registered(reg, head) {
            def expanded = expand_form(reg, value, tok, max_steps)
            if !core_ref.eq(expanded, value) { return expand_form_deep(reg, expanded, tok, max_steps - 1) }
         }
      }
      return _expand_form_list(reg, value, tok, max_steps)
   }
   value
}

fn new_rewriter(int cap=8) dict {
   "Creates a deterministic rewrite pipeline object."
   return {"rules": dict(cap), "rule_order": list(cap)}
}

fn clear_rewriter(dict rw) dict {
   "Clears rewrite rules while preserving object identity."
   rw = _ensure_rewriter(rw)
   rw = dict_set(rw, "rules", dict(8))
   rw = dict_set(rw, "rule_order", list(8))
   rw
}

fn register_rewrite(dict rw, str name, any handler) dict {
   "Registers a rewrite handler in deterministic order."
   rw = _ensure_rewriter(rw)
   name = _ensure_name(name)
   handler = _ensure_handler(handler)
   mut rules = _rewriter_dict(rw, "rules")
   mut order = _rewriter_list(rw, "rule_order")
   def existed = rules.contains(name)
   rules = dict_set(rules, name, handler)
   rw = dict_set(rw, "rules", rules)
   if !existed {
      order = order.append(name)
      rw = dict_set(rw, "rule_order", order)
   }
   rw
}

fn list_rewrites(dict rw) list {
   "Returns registered rewrite names in deterministic order."
   rw = _ensure_rewriter(rw)
   def order = _rewriter_list(rw, "rule_order")
   _clone_list(order)
}

fn rewrite_once(dict rw, any value) any {
   "Runs one rewrite pass over `value` using registration order."
   rw = _ensure_rewriter(rw)
   def order = _rewriter_list(rw, "rule_order")
   def rules = _rewriter_dict(rw, "rules")
   mut out = value
   mut i = 0
   while i < order.len {
      def name = order.get(i, "")
      if is_str(name) && name.len > 0 {
         def handler = rules.get(name, 0)
         if handler { out = handler(out) }
      }
      i += 1
   }
   out
}

fn rewrite_fixpoint(dict rw, any value, int max_steps=64) any {
   "Runs rewrite passes until output stabilizes or step limit is reached."
   rw = _ensure_rewriter(rw)
   mut current = value
   mut steps = 0
   while steps < max_steps {
      def next = rewrite_once(rw, current)
      if core_ref.eq(current, next) { return next }
      current = next
      steps += 1
   }
   current
}

fn apply_attribute(dict reg, str name, any node, any args=0) any {
   "Applies attribute `name` to `node`. Returns original node on fallback."
   reg = _ensure_registry(reg)
   name = _ensure_name(name)
   def handler = get_attr_handler(reg, name)
   if !handler { return node }
   handler(node, _to_list(args))
}

#main {
   fn _syntax_self_macro(any node) any { node }
   fn _syntax_self_attr(dict node, list args) dict { node.set("marked", args.len >= 0) }
   fn _syntax_self_double(any v) any { is_int(v) ? v * 2 : v }
   mut reg = new_registry(8)
   assert(list_macros(reg).len == 0 && list_attributes(reg).len == 0, "syntax new_registry")
   reg = register_macro(reg, "id", _syntax_self_macro)
   assert(is_macro_registered(reg, "id"), "syntax register macro")
   def f = form("id", [10, 20])
   assert(is_form(f, "id") && form_head(f, "") == "id" && form_tail(f).len == 2 && is_form(expand_form(reg, f), "id"), "syntax forms")
   reg = register_attribute(reg, "mark", _syntax_self_attr)
   assert(is_attr_registered(reg, "mark") && apply_attribute(reg, "mark", dict(2), list(0)).get("marked", false), "syntax attr")
   mut reg2 = new_registry(4)
   reg2 = register_macro(reg2, "id2", _syntax_self_macro)
   reg = merge_registry(reg, reg2, true)
   assert(is_macro_registered(reg, "id2"), "syntax merge registry")
   reg = unregister_macro(reg, "id")
   assert(!is_macro_registered(reg, "id"), "syntax unregister macro")
   reg = unregister_attribute(reg, "mark")
   assert(!is_attr_registered(reg, "mark"), "syntax unregister attr")
   mut rw = new_rewriter(4)
   rw = register_rewrite(rw, "double", _syntax_self_double)
   assert(list_rewrites(rw).contains("double") && rewrite_once(rw, 3) == 6 && rewrite_fixpoint(rw, 3, 1) == 6, "syntax rewrite")
   rw = clear_rewriter(rw)
   assert(list_rewrites(rw).len == 0, "syntax clear rewriter")
   print("✓ std.core.syntax.syntax self-test passed")
}
