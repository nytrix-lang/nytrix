;; Keywords: core report diagnostic error warning format style terminal
;; Diagnostic report formatting for compiler tools and diagnostics.
use std.core.str
use std.core.term as term
module std.core.report(format_error, format_warning, format_note, format_location, format_line, format_source_line, indent)
fn format_error(str message, str file="", int line=0, int col=0) str {
   "Formats a diagnostic error message with optional file:line:col location."
   mut out = ""
   if file.len > 0 {
      out = file
      if line > 0 { out = out + ":" + to_str(line) }
      if col > 0 { out = out + ":" + to_str(col) }
      out = out + ": "
   }
   out = term.color("error", "red") + ": " + message
   out
}

fn format_warning(str message, str file="", int line=0, int col=0) str {
   "Formats a diagnostic warning message with optional file:line:col location."
   mut out = ""
   if file.len > 0 {
      out = file
      if line > 0 { out = out + ":" + to_str(line) }
      if col > 0 { out = out + ":" + to_str(col) }
      out = out + ": "
   }
   out = term.color("warning", "yellow") + ": " + message
   out
}

fn format_note(str message) str {
   "Formats a diagnostic note."
   term.color("note", "cyan") + ": " + message
}

fn format_location(str file, int line, int col) str {
   "Formats a file:line:col location string."
   mut out = file
   if line > 0 { out = out + ":" + to_str(line) }
   if col > 0 { out = out + ":" + to_str(col) }
   out
}

fn format_line(int num, int width) str {
   "Formats a line number with fixed width for source display."
   def s = to_str(num)
   def pad = width - s.len
   if pad > 0 { str.repeat(" ", pad) + s } else { s }
}

fn format_source_line(str src_line, int line_num, int err_col, int err_len, str color_name="red") str {
   "Formats a source line with an underline annotation at the error location."
   def gutter = format_line(line_num, 4)
   def prefix = gutter + " | "
   def underline = str.repeat(" ", prefix.len + err_col) + term.color(str.repeat("^", err_len), color_name)
   prefix + src_line + "\n" + underline
}

fn indent(str text, int depth=2) str {
   "Indents each line of text by `depth` spaces."
   def pad = str.repeat(" ", depth)
   def lines = text.split("\n")
   mut out = ""
   mut first = 1
   for line in lines {
      if !first { out = out + "\n" }
      out = out + pad + line
      first = 0
   }
   out
}
