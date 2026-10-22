;; Keywords: documentation source syntax highlighting
;; Renders source snippets for terminal documentation views.
module core.compiler.tools.doc.source(highlight)
use core
use core.str as str
use core.math.parse.syntax.nytrix as ny_syntax

fn _palette_index(int kind) int {
   if kind == 0 || kind == 9 { return 0 }
   if kind == 1 || kind == 12 { return 1 }
   if kind == 2 { return 2 }
   if kind == 3 { return 3 }
   if kind == 4 { return 4 }
   if kind == 5 { return 5 }
   if kind == 6 { return 6 }
   if kind == 7 { return 7 }
   if kind == 11 || kind == 13 { return 8 }
   if kind == 17 { return 9 }
   -1
}

fn _is_space(int byte) bool {
   byte == 32 || byte == 9 || byte == 10 || byte == 13 || byte == 12 || byte == 11
}

fn _source_without_docstring(str source) str {
   mut int start = 0
   while start < source.len && _is_space(load8(source, start)) { start += 1 }
   def str trimmed = str.str_slice(source, start, source.len)
   mut int brace = 0
   while brace < trimmed.len && load8(trimmed, brace) != 123 { brace += 1 }
   if brace == trimmed.len { return trimmed }
   mut int i = brace + 1
   while i < trimmed.len && _is_space(load8(trimmed, i)) { i += 1 }
   if i >= trimmed.len { return trimmed }
   def int quote = load8(trimmed, i)
   if quote != 34 && quote != 39 { return trimmed }
   i += 1
   mut bool escaped = false
   mut bool closed = false
   while i < trimmed.len {
      def int byte = load8(trimmed, i)
      if escaped { escaped = false }
      elif byte == 92 { escaped = true }
      elif byte == quote {
         i += 1
         closed = true
         break
      }
      i += 1
   }
   if !closed { return trimmed }
   while i < trimmed.len && load8(trimmed, i) != 10 { i += 1 }
   if i < trimmed.len { i += 1 }
   str.str_slice(trimmed, 0, brace + 1) + "\n" +
   str.str_slice(trimmed, i, trimmed.len)
}

fn highlight(str source, any palette) str {
   "Renders Ny syntax-token spans with the supplied terminal palette."
   def str shown = _source_without_docstring(source)
   def tokens = ny_syntax.tokenize(shown, [])
   mut str output = ""
   mut str active_style = ""
   for token in tokens {
      def int kind = int(token.get(0))
      def int start = int(token.get(1))
      def int stop = start + int(token.get(2))
      def int color = _palette_index(kind)
      def str next_style = if color >= 0 { str(palette.get(color, "")) } else { "" }
      if next_style != active_style {
         if active_style.len > 0 { output += str(palette.get(10, "")) }
         if next_style.len > 0 { output += next_style }
         active_style = next_style
      }
      output += str.str_slice(shown, start, stop)
   }
   if active_style.len > 0 { output += str(palette.get(10, "")) }
   output
}

#main {
   assert(highlight("fn probe() { return 7 }", []) ==
      "fn probe() { return 7 }",
      "source highlighting preserves source when colors are disabled")
   assert(highlight("fn probe() {\n  \"documentation\"; comment\n  return 7\n}", []) ==
      "fn probe() {\n  return 7\n}",
      "source display removes a function's leading documentation string")
}
