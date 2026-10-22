;; Keywords: static site Markdown HTML renderer
;; Small deterministic Markdown renderer used by generated documentation pages.
;; References:
;; - core
;; - core.str
module core.compiler.tools.doc.markdown(render_static, render_static_batch)
use core
use core.str as str

fn _append_html_range(ptr out, str value, int start, int end) ptr {
   mut int i = start
   mut int plain_start = start
   while i < end {
      def int byte = load8(value, i)
      if byte == 38 || byte == 60 || byte == 62 || byte == 34 {
         if i > plain_start {
            out = str.builder_append(out, str._substr(value, plain_start, i))
         }
         if byte == 38 { out = str.builder_append(out, "&amp;") }
         elif byte == 60 { out = str.builder_append(out, "&lt;") }
         elif byte == 62 { out = str.builder_append(out, "&gt;") }
         else { out = str.builder_append(out, "&quot;") }
         plain_start = i + 1
      }
      i += 1
   }
   if end > plain_start {
      out = str.builder_append(out, str._substr(value, plain_start, end))
   }
   out
}

fn _append_html(ptr out, str value) ptr {
   _append_html_range(out, value, 0, value.len)
}

fn _external(str href) bool {
   if href.len == 0 { return false }
   if href.len > 1 && load8(href, 0) == 47 && load8(href, 1) == 47 { return true }
   mut int i = 0
   while i < href.len {
      def int byte = load8(href, i)
      if byte == 58 { return true }
      if byte == 47 || byte == 35 || byte == 63 { return false }
      i += 1
   }
   false
}

fn _route_for(str base, str href) dict {
   if href.len == 0 || _external(href) { return {"found":false} }
   def int hash = str.find(href, "#")
   def int query = str.find(href, "?")
   mut int end = href.len
   if hash >= 0 && hash < end { end = hash }
   if query >= 0 && query < end { end = query }
   def str anchor = hash >= 0 ? str._substr(href, hash + 1, href.len) : ""
   mut str path = str._substr(href, 0, end)
   if path.len == 0 { return {"found":true, "route":base, "anchor":anchor} }
   path = str.str_replace(path, "\\", "/")
   while str.startswith(path, "./") { path = str._substr(path, 2, path.len) }
   while str.startswith(path, "/") { path = str._substr(path, 1, path.len) }
   if str.startswith(path, "docs/") { path = str._substr(path, 5, path.len) }
   if str.endswith(path, ".md") { path = str._substr(path, 0, path.len - 3) }
   elif str.endswith(path, ".html") { path = str._substr(path, 0, path.len - 5) }
   elif str.endswith(path, ".htm") { path = str._substr(path, 0, path.len - 4) }
   if str.endswith(path, "/index") { path = str._substr(path, 0, path.len - 6) }
   def bool rooted = href.len > 0 && load8(href, 0) == 47 ||
   str.startswith(href, "docs/") || str.startswith(path, "learn/") ||
   str.startswith(path, "spec/")
   mut str combined = path
   if !rooted && base.len > 0 {
      def int slash = str.find_last(base, "/")
      def str parent = slash >= 0 ? str._substr(base, 0, slash) : ""
      combined = parent.len > 0 ? parent + "/" + path : path
   }
   mut parts = []
   mut int part_count = 0
   def segments = str.split(combined, "/")
   for segment in segments {
      def str item = segment
      if item.len == 0 || item == "." { continue }
      if item == ".." {
         if part_count > 0 { part_count -= 1 }
      } else {
         if part_count < parts.len { parts[part_count] = item }
         else { parts = parts.append(item) }
         part_count += 1
      }
   }
   mut str route = ""
   mut int part_idx = 0
   while part_idx < part_count {
      def str part = parts.get(part_idx)
      if route.len > 0 { route += "/" }
      route += part
      part_idx += 1
   }
   {"found":true, "route":route, "anchor":anchor}
}

fn _href(str from, str to, str anchor) str {
   if from == to { return "#" + anchor }
   mut int depth = from.len > 0 ? 1 : 0
   mut int i = 0
   while i < from.len {
      if load8(from, i) == 47 { depth += 1 }
      i += 1
   }
   mut str result = ""
   while depth > 0 {
      result += "../"
      depth -= 1
   }
   result += to.len > 0 ? to : "."
   if result.len == 0 || load8(result, result.len - 1) != 47 { result += "/" }
   if anchor.len > 0 { result += "#" + anchor }
   result
}

fn _inline(str text, str route) str {
   mut ptr out = str.Builder(text.len + 32)
   mut bool code = false
   mut int i = 0
   while i < text.len {
      def int byte = load8(text, i)
      if byte == 96 {
         out = str.builder_append(out, code ? "</code>" : "<code>")
         code = !code
         i += 1
         continue
      }
      if !code && byte == 91 {
         def int label_end = str.find(str._substr(text, i + 1, text.len), "](")
         if label_end >= 0 {
            def int close_label = i + 1 + label_end
            def int href_end = str.find(str._substr(text, close_label + 2, text.len), ")")
            if href_end >= 0 {
               def str label = str._substr(text, i + 1, close_label)
               def str href = str._substr(text, close_label + 2, close_label + 2 + href_end)
               mut str final_href = href
               if !_external(href) {
                  def dict resolved = _route_for(route, href)
                  def str target_route = resolved.get("route", "")
                  def str target_anchor = resolved.get("anchor", "")
                  final_href = _href(route, target_route, target_anchor)
               }
               out = str.builder_append(out, "<a href=\"")
               out = _append_html(out, final_href)
               out = str.builder_append(out, _external(href) ? "\" rel=\"noopener noreferrer\">" : "\">")
               out = str.builder_append(out, _inline(label, route))
               out = str.builder_append(out, "</a>")
               i = close_label + 2 + href_end + 1
               continue
            }
         }
      }
      def int span_start = i
      i += 1
      while i < text.len {
         def int next_byte = load8(text, i)
         if next_byte == 96 || (!code && next_byte == 91) { break }
         i += 1
      }
      out = _append_html_range(out, text, span_start, i)
   }
   if code { out = str.builder_append(out, "</code>") }
   def str result = str.builder_to_str(out)
   str.builder_free(out)
   result
}

fn _trim(str line) str {
   mut int start = 0
   mut int end = line.len
   while start < end && (load8(line, start) == 32 || load8(line, start) == 9 || load8(line, start) == 13) { start += 1 }
   while end > start && (load8(line, end - 1) == 32 || load8(line, end - 1) == 9 || load8(line, end - 1) == 13) { end -= 1 }
   str._substr(line, start, end)
}

fn _close(ptr out, bool ul, bool table) ptr {
   if ul { out = str.builder_append(out, "</ul>\n") }
   if table { out = str.builder_append(out, "</tbody></table>\n") }
   out
}

fn _table_row(ptr out, str line, str route) ptr {
   mut int i = 0
   while i < line.len {
      def int byte = load8(line, i)
      if byte != 124 && byte != 45 && byte != 58 && byte != 32 && byte != 9 { break }
      i += 1
   }
   if i == line.len { return out }
   out = str.builder_append(out, "<tr>")
   mut int start = load8(line, 0) == 124 ? 1 : 0
   while start < line.len {
      mut int end = start
      while end < line.len && load8(line, end) != 124 { end += 1 }
      mut int left = start
      mut int right = end
      while left < right && (load8(line, left) == 32 || load8(line, left) == 9) { left += 1 }
      while right > left && (load8(line, right - 1) == 32 || load8(line, right - 1) == 9) { right -= 1 }
      out = str.builder_append(out, "<td>")
      out = str.builder_append(out, _inline(str._substr(line, left, right), route))
      out = str.builder_append(out, "</td>")
      start = end < line.len ? end + 1 : end
   }
   str.builder_append(out, "</tr>\n")
}

fn _code_fence(ptr out, str line, bool closing) ptr {
   if closing { return str.builder_append(out, "</code></pre>\n") }
   out = str.builder_append(out, "<pre><code")
   mut int start = 3
   while start < line.len && (load8(line, start) == 32 || load8(line, start) == 9) { start += 1 }
   mut int end = start
   while end < line.len {
      def int byte = load8(line, end)
      if !(str.ascii_is_alnum(byte) || byte == 95 || byte == 45) { break }
      end += 1
   }
   if end > start {
      out = str.builder_append(out, " class=\"language-")
      out = _append_html(out, str._substr(line, start, end))
      out = str.builder_append(out, "\"")
   } else { out = str.builder_append(out, " class=\"language-ny\"") }
   str.builder_append(out, ">")
}

fn _line(str line, str route, bool ul, bool table) dict {
   mut ptr out = str.Builder(128)
   if line.len == 0 {
      out = _close(out, ul, table)
      def str html = str.builder_to_str(out)
      str.builder_free(out)
      return {"html":html, "ul":false, "table":false}
   }
   if load8(line, 0) == 35 {
      out = _close(out, ul, table)
      mut int level = 0
      while level < line.len && load8(line, level) == 35 { level += 1 }
      if level < 1 { level = 1 }
      if level > 4 { level = 4 }
      mut int start = level
      while start < line.len && (load8(line, start) == 32 || load8(line, start) == 9) { start += 1 }
      def str tag = "h" + str(level)
      out = str.builder_append(out, "<" + tag + ">")
      out = str.builder_append(out, _inline(str._substr(line, start, line.len), route))
      out = str.builder_append(out, "</" + tag + ">\n")
      def str html = str.builder_to_str(out)
      str.builder_free(out)
      return {"html":html, "ul":false, "table":false}
   }
   if line.len >= 2 && load8(line, 0) == 45 && (load8(line, 1) == 32 || load8(line, 1) == 9) {
      if table { out = str.builder_append(out, "</tbody></table>\n") }
      if !ul { out = str.builder_append(out, "<ul>\n") }
      mut int start = 2
      while start < line.len && (load8(line, start) == 32 || load8(line, start) == 9) { start += 1 }
      out = str.builder_append(out, "<li>" + _inline(str._substr(line, start, line.len), route) + "</li>\n")
      def str html = str.builder_to_str(out)
      str.builder_free(out)
      return {"html":html, "ul":true, "table":false}
   }
   if load8(line, 0) == 124 {
      if ul { out = str.builder_append(out, "</ul>\n") }
      if !table { out = str.builder_append(out, "<table><tbody>\n") }
      out = _table_row(out, line, route)
      def str html = str.builder_to_str(out)
      str.builder_free(out)
      return {"html":html, "ul":false, "table":true}
   }
   out = _close(out, ul, table)
   out = str.builder_append(out, "<p>" + _inline(line, route) + "</p>\n")
   def str html = str.builder_to_str(out)
   str.builder_free(out)
   {"html":html, "ul":false, "table":false}
}

fn render_static(str markdown, str route) str {
   "Renders the static-site Markdown subset with the same stable HTML as ny web."
   mut ptr out = str.Builder(markdown.len + 128)
   mut bool code = false
   mut bool ul = false
   mut bool table = false
   mut int start = 0
   mut int i = 0
   while i <= markdown.len {
      if i < markdown.len && load8(markdown, i) != 10 {
         i += 1
         continue
      }
      mut str line = str._substr(markdown, start, i)
      if str.endswith(line, "\r") { line = str._substr(line, 0, line.len - 1) }
      if line.len >= 3 && str.startswith(line, "```") {
         out = _close(out, ul, table)
         ul = false
         table = false
         out = _code_fence(out, line, code)
         code = !code
      } elif code {
         out = _append_html(out, line)
         out = str.builder_append(out, "\n")
      } else {
         line = _trim(line)
         def dict next = _line(line, route, ul, table)
         def str html = next.get("html")
         out = str.builder_append(out, html)
         ul = next.get("ul")
         table = next.get("table")
      }
      start = i + 1
      i += 1
   }
   if code { out = str.builder_append(out, "</code></pre>\n") }
   out = _close(out, ul, table)
   def str result = str.builder_to_str(out)
   str.builder_free(out)
   result
}

fn render_static_batch(any markdowns, any routes) any {
   "Renders parallel source and route lists in one compiler invocation."
   if !is_list(markdowns) || !is_list(routes) || markdowns.len != routes.len { return nil }
   mut results = []
   mut int i = 0
   while i < markdowns.len {
      def str markdown = markdowns.get(i)
      def str route = routes.get(i)
      results = results.append(render_static(markdown, route))
      i += 1
   }
   results
}
