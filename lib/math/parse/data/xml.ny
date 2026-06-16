;; Keywords: data serialization xml parse
;; Extensible Markup Language (XML) Parser and Generator for Nytrix
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc3470.html
;; References:
;; - std.math.parse.data
;; - std.math.parse
module std.math.parse.data.xml(parse, encode, Node)
use std.core
use std.core.str as str

fn Node(str name, any attr=0, any children=0, any text="") dict {
   "Creates an XML node record."
   mut n = dict(8)
   n["name"] = name
   n["attr"] = is_dict(attr) ? attr : dict()
   n["children"] = is_list(children) ? children : []
   n["text"] = text
   n
}

fn _skip_ws(str s, int p, int n) int {
   while p < n {
      def c = load8(s, p)
      if c <= 32 { p += 1 }
      else { break }
   }
   p
}

@inline
fn _attr_key_stop(int c) bool {
   return case c {
      47, 61, 62 -> true
      _ -> c <= 32
   }
}

fn _parse_attr(str s, int p, int n) list {
   mut attrs = dict(8)
   while p < n {
      p = _skip_ws(s, p, n)
      if p >= n || load8(s, p) == 62 || load8(s, p) == 47 { break }
      mut kb = Builder(32)
      while p < n {
         def c = load8(s, p)
         if _attr_key_stop(c) { break }
         kb = builder_append(kb, chr(c))
         p += 1
      }
      def key = builder_to_str(kb)
      builder_free(kb)
      p = _skip_ws(s, p, n)
      if p < n && load8(s, p) == 61 {
         p += 1
         p = _skip_ws(s, p, n)
         def quote = load8(s, p)
         if quote == 34 || quote == 39 {
            p += 1
            mut vb = Builder(64)
            while p < n && load8(s, p) != quote {
               vb = builder_append(vb, chr(load8(s, p)))
               p += 1
            }
            def val = builder_to_str(vb)
            builder_free(vb)
            if key.len > 0 { attrs = attrs.set(key, val) }
            if p < n { p += 1 }
         } else {
            mut vb = Builder(32)
            while p < n {
               def c = load8(s, p)
               if c <= 32 || c == 47 || c == 62 { break }
               vb = builder_append(vb, chr(c))
               p += 1
            }
            def val = builder_to_str(vb)
            builder_free(vb)
            if key.len > 0 { attrs = attrs.set(key, val) }
         }
      } else {
         if key.len > 0 { attrs = attrs.set(key, true) }
      }
   }
   [attrs, p]
}

fn parse(any data) any {
   "Parses a simple XML string into a tree of nodes."
   if !is_str(data) { return 0 }
   def n = data.len
   mut p = 0
   mut stack = []
   mut root = 0
   while p < n {
      p = _skip_ws(data, p, n)
      if p >= n { break }
      if load8(data, p) == 60 {
         p += 1
         if p < n && load8(data, p) == 47 {
            p += 1
            mut nb = Builder(16)
            while p < n && load8(data, p) != 62 {
               nb = builder_append(nb, chr(load8(data, p)))
               p += 1
            }
            def name = builder_to_str(nb)
            builder_free(nb)
            p += 1
            if stack.len > 1 { stack.pop() }
         } elif p < n && load8(data, p) == 33 {
            while p < n && load8(data, p) != 62 { p += 1 }
            p += 1
         } elif p < n && load8(data, p) == 63 {
            while p < n && load8(data, p) != 62 { p += 1 }
            p += 1
         } else {
            mut nb = Builder(16)
            while p < n {
               def c = load8(data, p)
               if c == 62 || c == 47 || c <= 32 { break }
               nb = builder_append(nb, chr(c))
               p += 1
            }
            def name = builder_to_str(nb)
            builder_free(nb)
            def attr_res = _parse_attr(data, p, n)
            def attrs = attr_res.get(0)
            p = attr_res.get(1)
            mut self_closing = false
            if p < n && load8(data, p) == 47 {
               self_closing = true
               p += 1
            }
            if p < n && load8(data, p) == 62 { p += 1 }
            def node = Node(name, attrs)
            if root == 0 { root = node }
            if stack.len > 0 {
               def parent = stack.get(stack.len - 1)
               mut children = parent.get("children")
               children = children.append(node)
               parent["children"] = children
            }
            if !self_closing { stack = stack.append(node) }
         }
      } else {
         mut tb = Builder(32)
         while p < n && load8(data, p) != 60 {
            tb = builder_append(tb, chr(load8(data, p)))
            p += 1
         }
         def text = builder_to_str(tb)
         builder_free(tb)
         if stack.len > 0 {
            def current = stack.get(stack.len - 1)
            def existing = current.get("text")
            current["text"] = existing + str.strip(text)
         }
      }
   }
   root
}

fn _xml_encode_node(any node) str {
   "Serializes a node tree into an XML string."
   if !is_dict(node) { return "" }
   def name = node.get("name", "node")
   def attrs = node.get("attr", dict())
   def children = node.get("children", [])
   def text = node.get("text", "")
   mut out = Builder(64)
   out = builder_append(out, "<")
   out = builder_append(out, name)
   def attr_keys = dict_keys(attrs)
   mut i = 0
   def attr_keys_n = attr_keys.len
   while i < attr_keys_n {
      def k, v = attr_keys.get(i), attrs.get(k)
      out = builder_append(out, " ")
      out = builder_append(out, k)
      if v != true {
         out = builder_append(out, "='")
         out = builder_append(out, to_str(v))
         out = builder_append(out, "'")
      }
      i += 1
   }
   if children.len == 0 && text.len == 0 { out = builder_append(out, "/>") } else {
      out = builder_append(out, ">")
      out = builder_append(out, text)
      mut j = 0
      def children_n = children.len
      while j < children_n {
         out = builder_append(out, _xml_encode_node(children.get(j)))
         j += 1
      }
      out = builder_append(out, "</")
      out = builder_append(out, name)
      out = builder_append(out, ">")
   }
   def s_out = builder_to_str(out)
   builder_free(out)
   s_out
}

fn encode(any node) str {
   "Serializes a node tree into an XML string."
   _xml_encode_node(node)
}
