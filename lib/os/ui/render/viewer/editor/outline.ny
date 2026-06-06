;; Keywords: editor outline symbols navigation os ui render viewer text
;; Outline tree helpers for symbols, headings, and editor navigation.
;; References:
;; - std.os.ui.render.viewer.editor.lsp
module std.os.ui.render.viewer.editor.outline(ROW_H, symbols, row_y, row_at, icon_for)
use std.core
use std.core.str as str
use std.math (min)

def ROW_H = 21.0

fn _ident_after(str s, int start) str {
   mut i = start
   while(i < s.len && load8(s, i) == 32){ i += 1 }
   def begin = i
   while(i < s.len){
      def c = load8(s, i)
      if((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95 || c == 35){
         i += 1
      } else {
         break
      }
   }
   str.str_slice(s, begin, i)
}

fn icon_for(str kind) str {
   if(kind == "fn"){ return "callable" }
   if(kind == "struct"){ return "classlist" }
   if(kind == "enum"){ return "dictionary" }
   if(kind == "module"){ return "filesystem" }
   if(kind == "main"){ return "debugcontinue" }
   "graphnode"
}

fn _push_symbol(list out, str kind, str name, int line) list {
   if(name.len <= 0){ return out }
   out.append({"kind": kind, "name": name, "line": line, "icon": icon_for(kind)})
}

fn symbols(list lines, str filename="", int max_lines=0) list {
   "Extracts a compact symbol outline from source lines."
   mut out = []
   mut i = 0
   def n = max_lines > 0 ? min(lines.len, max_lines) : lines.len
   while(i < n){
      def raw = to_str(lines.get(i, ""))
      def s = str.strip(raw)
      if(str.startswith(s, "fn ")){ out = _push_symbol(out, "fn", _ident_after(s, 3), i) }
      elif(str.startswith(s, "struct ")){ out = _push_symbol(out, "struct", _ident_after(s, 7), i) }
      elif(str.startswith(s, "enum ")){ out = _push_symbol(out, "enum", _ident_after(s, 5), i) }
      elif(str.startswith(s, "module ")){ out = _push_symbol(out, "module", _ident_after(s, 7), i) }
      elif(str.startswith(s, "#main")){ out = _push_symbol(out, "main", "#main", i) }
      i += 1
   }
   out
}

fn row_y(f64 panel_y, int idx) f64 {
   panel_y + 28.0 + float(idx) * ROW_H
}

fn row_at(f64 panel_x, f64 panel_y, f64 panel_w, f64 panel_h, f64 x, f64 y, int count) int {
   if(x < panel_x || x > panel_x + panel_w){ return -1 }
   if(y < panel_y + 28.0 || y > panel_y + panel_h){ return -1 }
   def rel = y - panel_y - 28.0
   def idx = int(rel / ROW_H)
   if(idx < 0 || idx >= count){ return -1 }
   def in_row = rel - float(idx) * ROW_H
   in_row >= 0.0 && in_row < ROW_H ? idx : -1
}

#main {
   def syms = symbols(["module a", "fn main() int {", "struct Thing {", "#main {"])
   assert(syms.len == 4 && syms.get(1).get("name") == "main", "outline symbols")
   assert(row_at(0.0, 0.0, 100.0, 100.0, 5.0, 29.0, 2) == 0, "outline row hit")
   print("✓ viewer editor outline test passed")
}
