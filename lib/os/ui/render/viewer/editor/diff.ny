;; Keywords: editor diff changes compare lines os ui render viewer text
;; Diff view helpers for comparing editor buffers and rendering changed lines.
;; References:
;; - std.os.ui.render.viewer.editor.core
module std.os.ui.render.viewer.editor.diff(side_by_side)
use std.core
use std.core.str as str
use std.math (max, min)

fn _clean(str s) str {
   str.str_replace(str.str_replace(s, "\r\n", "\n"), "\t", "   ")
}

fn _clip(str s, int width) str {
   if(width <= 0){ return "" }
   if(s.len <= width){ return s }
   str.str_slice(s, 0, max(0, width - 3)) + "..."
}

fn _pad(str s, int width) str {
   mut out = _clip(s, width)
   while(out.len < width){ out += " " }
   out
}

fn _line_mark(str a, str b, bool left_missing, bool right_missing) str {
   if(left_missing){ return ">" }
   if(right_missing){ return "<" }
   a == b ? " " : "|"
}

fn _summary(list left, list right) list {
   mut same = 0
   mut changed = 0
   mut added = 0
   mut removed = 0
   def n = max(left.len, right.len)
   mut i = 0
   while(i < n){
      def lm = i >= left.len
      def rm = i >= right.len
      if(lm){ added += 1 }
      elif(rm){ removed += 1 }
      elif(to_str(left.get(i, "")) == to_str(right.get(i, ""))){ same += 1 }
      else { changed += 1 }
      i += 1
   }
   [same, changed, added, removed]
}

fn side_by_side(str left_text, str right_text, str left_name="buffer", str right_name="clipboard", int width=68) str {
   "Returns a vertical side-by-side line diff."
   def left = str.split(_clean(left_text), "\n")
   def right = str.split(_clean(right_text), "\n")
   def sum = _summary(left, right)
   def n = max(left.len, right.len)
   mut out = "diff " + left_name + " <-> " + right_name + "\n"
   out += "same " + to_str(sum.get(0)) + "  changed " + to_str(sum.get(1)) + "  added " + to_str(sum.get(2)) + "  removed " + to_str(sum.get(3)) + "\n\n"
   out += _pad("left: " + left_name, width) + "   !   " + "right: " + right_name + "\n"
   out += str.repeat("-", width) + "---+---" + str.repeat("-", width) + "\n"
   mut i = 0
   while(i < n){
      def lm = i >= left.len
      def rm = i >= right.len
      def a = lm ? "" : to_str(left.get(i, ""))
      def b = rm ? "" : to_str(right.get(i, ""))
      def mark = _line_mark(a, b, lm, rm)
      out += _pad(to_str(i + 1) + " " + a, width) + "   " + mark + "   " + to_str(i + 1) + " " + _clip(b, width) + "\n"
      i += 1
   }
   out
}

#main {
   def d = side_by_side("a\nb\nc", "a\nx\nc\nd", "left", "right", 12)
   assert(str.str_contains(d, "changed 1") && str.str_contains(d, ">"), "editor side diff")
   print("✓ viewer editor diff test passed")
}
