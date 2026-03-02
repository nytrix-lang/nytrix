;; Keywords: enc xml
;; Simple XML Parser.
;; Reference: https://www.rfc-editor.org/rfc/rfc3470.html

module std.enc.xml (
   parse, encode, Node
)

use std.core *
use std.text as str

fn Node(name, attr=dict(), children=[], text=""){
   "Auto-generated docstring: Node."
   mut n = dict(8)
   dict_set(n, "name", name)
   dict_set(n, "attr", attr)
   dict_set(n, "children", children)
   dict_set(n, "text", text)
   n
}

fn _skip_ws(s, p, n){
   "Auto-generated docstring: _skip_ws."
   while(p < n){
      def c = load8(s, p)
      if(c <= 32){ p += 1 }
      else { break }
   }
   p
}

fn _parse_attr(s, p, n){
   "Auto-generated docstring: _parse_attr."
   mut attrs = dict()
   while(p < n){
      p = _skip_ws(s, p, n)
      if(p >= n || load8(s, p) == 62 || load8(s, p) == 47){ break } ;; '>' or '/'
      mut key = ""
      while(p < n){
         def c = load8(s, p)
         if(c == 61 || c <= 32){ break }
         key = key + chr(c)
         p += 1
      }
      p = _skip_ws(s, p, n)
      if(p < n && load8(s, p) == 61){ ;; '='
         p += 1
         p = _skip_ws(s, p, n)
         def quote = load8(s, p)
         if(quote == 34 || quote == 39){ ;; '"' or "'"
            p += 1
            mut val = ""
            while(p < n && load8(s, p) != quote){
               val = val + chr(load8(s, p))
               p += 1
            }
            p += 1
            attrs = dict_set(attrs, key, val)
         }
      } else {
         attrs = dict_set(attrs, key, true)
      }
   }
   [attrs, p]
}

fn parse(data){
   "Parses a simple XML string into a tree of nodes."
   if(!is_str(data)){ return 0 }
   def n = len(data)
   mut p = 0
   mut stack = []
   mut root = 0
   while(p < n){
      p = _skip_ws(data, p, n)
      if(p >= n){ break }
      if(load8(data, p) == 60){ ;; '<'
         p += 1
         if(p < n && load8(data, p) == 47){ ;; '</'
            p += 1
            mut name = ""
            while(p < n && load8(data, p) != 62){
               name = name + chr(load8(data, p))
               p += 1
            }
            p += 1
            if(len(stack) > 1){
               stack = pop(stack)
            }
         } elif(p < n && load8(data, p) == 33){ ;; '<!' (Comment or CDATA)
            while(p < n && load8(data, p) != 62){ p += 1 }
            p += 1
         } elif(p < n && load8(data, p) == 63){ ;; '<?' (Declaration)
            while(p < n && load8(data, p) != 62){ p += 1 }
            p += 1
         } else {
            mut name = ""
            while(p < n){
               def c = load8(data, p)
               if(c == 62 || c == 47 || c <= 32){ break }
               name = name + chr(c)
               p += 1
            }
            def attr_res = _parse_attr(data, p, n)
            def attrs = get(attr_res, 0)
            p = get(attr_res, 1)
            mut self_closing = false
            if(p < n && load8(data, p) == 47){
               self_closing = true
               p += 1
            }
            if(p < n && load8(data, p) == 62){ p += 1 }
            def node = Node(name, attrs)
            if(root == 0){ root = node }
            if(len(stack) > 0){
               def parent = get(stack, len(stack) - 1)
               mut children = dict_get(parent, "children")
               children = append(children, node)
               dict_set(parent, "children", children)
            }
            if(!self_closing){
               stack = append(stack, node)
            }
         }
      } else {
         mut text = ""
         while(p < n && load8(data, p) != 60){
            text = text + chr(load8(data, p))
            p += 1
         }
         if(len(stack) > 0){
            def current = get(stack, len(stack) - 1)
            def existing = dict_get(current, "text")
            dict_set(current, "text", existing + str.strip(text))
         }
      }
   }
   root
}

fn encode(node){
   "Serializes a node tree into an XML string."
   if(!is_dict(node)){ return "" }
   def name = dict_get(node, "name", "node")
   def attrs = dict_get(node, "attr", dict())
   def children = dict_get(node, "children", [])
   def text = dict_get(node, "text", "")
   mut out = "<" + name
   def attr_keys = dict_keys(attrs)
   mut i = 0
   while(i < len(attr_keys)){
      def k = get(attr_keys, i)
      def v = dict_get(attrs, k)
      if(v == true){ out = out + " " + k }
      else { out = out + " " + k + "='" + to_str(v) + "'" }
      i += 1
   }
   if(len(children) == 0 && str.len(text) == 0){
      out = out + "/>"
   } else {
      out = out + ">" + text
      mut j = 0
      while(j < len(children)){
         out = out + encode(get(children, j))
         j += 1
      }
      out = out + "</" + name + ">"
   }
   out
}

if(comptime{__main()}){
   use std.core.error *
   def data = "<?xml version='1.0'?><root><item id='1' active>Hello</item><item id='2'>World</item></root>"
   def root = parse(data)
   assert(root != 0, "xml parse root")
   assert(dict_get(root, "name") == "root", "xml root name")
   def children = dict_get(root, "children")
   assert(len(children) == 2, "xml children count")
   def item1 = get(children, 0)
   assert(dict_get(item1, "name") == "item", "xml child name")
   assert(dict_get(dict_get(item1, "attr"), "id") == "1", "xml attr")
    assert(dict_get(item1, "text") == "Hello", "xml text content")

    def encoded = encode(root)
    assert(str.str_contains(encoded, "id='1'"), "xml encode attr")
    assert(str.str_contains(encoded, "</root>"), "xml encode root close")

    print("✓ std.enc.xml tests passed")
}
