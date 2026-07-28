;; Keywords: decompiler pseudocode structure braces switches formatting
;; Structural normalization for rendered Ny pseudocode blocks and delimiters.
module std.os.rev.decomp.structure *

use std.core
use std.core.str as str

fn _clean_next_nonblank_index(list lines, int start) int {
   mut i = start
   while i < lines.len && str.strip(lines[i]).len == 0 { i += 1 }
   i
}

fn _clean_compact_blank_lines(str text) str {
   def lines = str.split(text, "\n")
   mut b = str.Builder(text.len)
   mut blank = 0
   mut i = 0
   while i < lines.len {
      if i == lines.len - 1 && lines[i].len == 0 { break }
      def raw = str.strip(lines[i])
      if raw.len == 0 {
         def next = _clean_next_nonblank_index(lines, i + 1)
         if next < lines.len && str.strip(lines[next]) == "}" { i += 1 continue }
         if blank > 0 { i += 1 continue }
         blank += 1
         b = str.builder_append(b, "\n")
         i += 1
         continue
      }
      blank = 0
      b = str.builder_append(b, lines[i] + "\n")
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _clean_structure_report(str text) dict {
   mut depth = 0
   mut min_depth = 0
   mut opens = 0
   mut closes = 0
   mut line = 1
   mut first_bad_line = 0
   mut i = 0
   while i < text.len {
      def c = load8(text, i)
      if c == 10 { line += 1 }
      elif c == 123 {
         depth += 1
         opens += 1
      } elif c == 125 {
         depth -= 1
         closes += 1
         if depth < min_depth {
            min_depth = depth
            if first_bad_line == 0 { first_bad_line = line }
         }
      }
      i += 1
   }
   {"ok": depth == 0 && min_depth >= 0, "open": opens, "close": closes,
   "depth": depth, "min_depth": min_depth, "first_bad_line": first_bad_line}
}

fn _clean_balance_trailing_braces(str text) str {
   mut depth = 0
   mut i = 0
   while i < text.len {
      def c = load8(text, i)
      if c == 123 { depth += 1 }
      elif c == 125 && depth > 0 { depth -= 1 }
      i += 1
   }
   if depth <= 0 { return text }
   mut b = str.Builder(text.len + depth * 4)
   b = str.builder_append(b, text)
   if text.len == 0 || load8(text, text.len - 1) != 10 { b = str.builder_append(b, "\n") }
   while depth > 0 {
      b = str.builder_append(b, "}\n")
      depth -= 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _clean_switch_block_at(list lines, int start) dict {
   if start < 0 || start >= lines.len { return {"ok": false} }
   def head = str.strip(lines[start])
   if !str.startswith(head, "switch(") || !str.endswith(head, "{") { return {"ok": false} }
   mut depth = 0
   mut block = ""
   mut i = start
   while i < lines.len {
      def line = lines[i]
      block = block + line + "\n"
      mut j = 0
      while j < line.len {
         def c = load8(line, j)
         if c == 123 { depth += 1 }
         elif c == 125 { depth -= 1 }
         j += 1
      }
      i += 1
      if depth <= 0 { return {"ok": true, "text": block, "next": i} }
   }
   {"ok": false}
}

fn _clean_drop_repeated_switches(str text) str {
   def lines = str.split(text, "\n")
   mut b = str.Builder(text.len)
   mut last_switch = ""
   mut i = 0
   while i < lines.len {
      if i == lines.len - 1 && lines[i].len == 0 { break }
      def sw = _clean_switch_block_at(lines, i)
      if sw.get("ok", false) {
         def body = sw.get("text", "")
         if body == last_switch {
            i = int(sw.get("next", i + 1))
            continue
         }
         b = str.builder_append(b, body)
         last_switch = body
         i = int(sw.get("next", i + 1))
         continue
      }
      if str.strip(lines[i]).len > 0 { last_switch = "" }
      b = str.builder_append(b, lines[i] + "\n")
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

#main {
   assert(_clean_next_nonblank_index(["", "  ", "value"], 0) == 2, "next nonblank")
   assert(_clean_structure_report("if ok{\n}\n").get("ok", false), "balanced report")
   assert(_clean_balance_trailing_braces("if ok{\n") == "if ok{\n}\n", "balance trailing brace")
   assert(_clean_drop_repeated_switches("switch(x){\n}\nswitch(x){\n}\n") == "switch(x){\n}\n", "deduplicate switch")
   print("✓ std.os.rev.decomp.structure self-test passed")
}
