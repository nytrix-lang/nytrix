;; Keywords: documentation search index markdown sections
;; Extract section records used by the production documentation index.
;; References:
;; - core
;; - core.str
module core.compiler.tools.doc.index(markdown_sections, markdown_index_rows)
use core
use core.str as str

fn _space(int byte) bool {
   byte == 32 || (byte >= 9 && byte <= 13)
}

fn _trim(str value) str {
   mut int start = 0
   mut int stop = value.len
   while start < stop && _space(load8(value, start)) { start += 1 }
   while stop > start && _space(load8(value, stop - 1)) { stop -= 1 }
   str.str_slice(value, start, stop)
}

fn _heading(str body, int start, int stop) str {
   mut int pos = start
   mut int indent = 0
   while pos < stop && indent < 3 &&
   (load8(body, pos) == 32 || load8(body, pos) == 9) {
      pos += 1
      indent += 1
   }
   mut int hashes = 0
   while pos + hashes < stop && load8(body, pos + hashes) == 35 {
      hashes += 1
   }
   if hashes < 1 || hashes > 6 || pos + hashes >= stop ||
   !_space(load8(body, pos + hashes)) {
      return ""
   }
   pos += hashes
   while pos < stop && _space(load8(body, pos)) { pos += 1 }
   mut int end = stop
   while end > pos && _space(load8(body, end - 1)) { end -= 1 }
   while end > pos && load8(body, end - 1) == 35 { end -= 1 }
   while end > pos && _space(load8(body, end - 1)) { end -= 1 }
   if end <= pos { return "" }
   mut ptr title = str.Builder(end - pos)
   while pos < end {
      title = str.builder_append_byte(title, load8(body, pos))
      pos += 1
   }
   str.builder_to_str(title)
}

fn _record(str title, str display, int line, str body,
   int doc_start, int doc_stop, bool spans) dict {
   if spans {
      return {"name":title, "full_name":display + "#" + title,
         "path":display, "line":line, "doc_start":doc_start,
         "doc_stop":doc_stop}
   }
   {"name":title, "full_name":display + "#" + title, "path":display,
      "line":line, "doc":_trim(str.str_slice(body, doc_start, doc_stop))}
}

fn _line_end(str body, int start) int {
   mut int pos = start
   while pos < body.len && load8(body, pos) != 10 { pos += 1 }
   pos
}

fn _candidate_sections(str body, str display, str stem, str candidates,
   bool spans) list {
   mut list records = []
   mut str title = ""
   mut int section_start = 0
   mut int section_line = 1
   mut int offset = 0
   while offset < candidates.len {
      mut int line_start = 0
      mut int line_stop = 0
      mut int line_number = 0
      mut int field = 0
      while offset < candidates.len && field < 3 {
         def int byte = load8(candidates, offset)
         if byte == 44 || byte == 59 {
            if (field < 2 && byte != 44) || (field == 2 && byte != 59) {
               return []
            }
            field += 1
            offset += 1
         } else {
            def int digit = byte - 48
            if digit < 0 || digit > 9 { return [] }
            if field == 0 { line_start = line_start * 10 + digit }
            elif field == 1 { line_stop = line_stop * 10 + digit }
            else { line_number = line_number * 10 + digit }
            offset += 1
         }
      }
      if field != 3 { return [] }
      def str next_title = _heading(body, line_start, line_stop)
      if next_title.len > 0 {
         if title.len > 0 {
            records = records.append(_record(
                  title, display, section_line, body, section_start, line_start,
                  spans))
         }
         title = next_title
         section_start = line_start
         section_line = line_number
      }
   }
   if title.len > 0 {
      records = records.append(_record(
            title, display, section_line, body, section_start, body.len, spans))
   } elif records.len == 0 {
      if spans {
         records = records.append({"name":stem, "full_name":display,
               "path":display, "line":1, "doc_start":0,
               "doc_stop":body.len})
      } else {
         records = records.append({"name":stem, "full_name":display,
               "path":display, "line":1, "doc":_trim(body)})
      }
   }
   records
}

fn _index_row(str display, int line, int start, int stop, str title) str {
   str.join([display, "|", str(line), "|", str(start), "|", str(stop),
         "|", title, "\n"])
}

fn markdown_index_rows(str body, str display_hex, str stem_hex,
   str candidates) str {
   "Returns compact section spans for the production search-index adapter."
   mut list rows = []
   def encoded_display = display_hex
   mut str title = ""
   mut int section_start = 0
   mut int section_line = 1
   mut int offset = 0
   while offset < candidates.len {
      mut int line_start = 0
      mut int line_stop = 0
      mut int line_number = 0
      mut int field = 0
      while offset < candidates.len && field < 3 {
         def int byte = load8(candidates, offset)
         if byte == 44 || byte == 59 {
            if (field < 2 && byte != 44) || (field == 2 && byte != 59) {
               return ""
            }
            field += 1
            offset += 1
         } else {
            def int digit = byte - 48
            if digit < 0 || digit > 9 { return "" }
            if field == 0 { line_start = line_start * 10 + digit }
            elif field == 1 { line_stop = line_stop * 10 + digit }
            else { line_number = line_number * 10 + digit }
            offset += 1
         }
      }
      if field != 3 { return "" }
      def str next_title = _heading(body, line_start, line_stop)
      if next_title.len > 0 {
         if title.len > 0 {
            rows = rows.append(_index_row(encoded_display, section_line,
                  section_start, line_start, title))
         }
         title = next_title
         section_start = line_start
         section_line = line_number
      }
   }
   if title.len > 0 {
      rows = rows.append(_index_row(encoded_display, section_line,
            section_start, body.len, title))
   } else {
      rows = rows.append(_index_row(encoded_display, 1, 0, body.len, stem_hex))
   }
   str.join(rows)
}

@jit
fn markdown_sections(str body, str display, str stem, str candidates="",
   bool spans=false) list {
   "Extracts Markdown heading sections as documentation-search records."
   if spans || candidates.len > 0 {
      return _candidate_sections(body, display, stem, candidates, spans)
   }
   mut list records = []
   mut str title = ""
   mut int section_start = 0
   mut int section_line = 1
   mut int line = 1
   mut int pos = 0
   while pos <= body.len {
      def line_start = pos
      pos = _line_end(body, pos)
      def str next_title = _heading(body, line_start, pos)
      if next_title.len > 0 {
         if title.len > 0 {
            records = records.append(_record(
                  title, display, section_line, body, section_start, line_start,
                  spans))
         }
         title = next_title
         section_start = line_start
         section_line = line
      }
      if pos >= body.len { break }
      pos += 1
      line += 1
   }
   if title.len > 0 {
      records = records.append(_record(
            title, display, section_line, body, section_start, body.len, spans))
   } elif records.len == 0 {
      if spans {
         records = records.append({"name":stem, "full_name":display,
               "path":display, "line":1, "doc_start":0,
               "doc_stop":body.len})
      } else {
         records = records.append({"name":stem, "full_name":display,
               "path":display, "line":1, "doc":_trim(body)})
      }
   }
   records
}

#main {
   def sections = markdown_sections(
      "Lead\n\n## First ##  \nOne\n\n### Second\nTwo\n", "guide/page.md", "page")
   assert(sections.len == 2, "extracts each Markdown heading section")
   assert(sections.get(0).get("name") == "First" &&
      sections.get(0).get("full_name") == "guide/page.md#First" &&
      sections.get(0).get("line") == 3 &&
      sections.get(0).get("doc") == "## First ##  \nOne",
      "preserves heading body and source line")
   assert(sections.get(1).get("name") == "Second" &&
      sections.get(1).get("line") == 6 &&
      sections.get(1).get("doc") == "### Second\nTwo",
      "handles nested headings")
   def indexed = markdown_sections(
      "Lead\n\n## First ##  \nOne\n\n### Second\nTwo\n",
      "guide/page.md", "page", "6,19,3;25,35,6;")
   assert(indexed.len == sections.len &&
      indexed.get(0).get("name") == sections.get(0).get("name") &&
      indexed.get(0).get("doc") == sections.get(0).get("doc") &&
      indexed.get(1).get("name") == sections.get(1).get("name") &&
      indexed.get(1).get("doc") == sections.get(1).get("doc"),
      "pre-scanned candidates preserve the Ny section result")
   def span_rows = markdown_sections(
      "Lead\n\n## First ##  \nOne\n\n### Second\nTwo\n",
      "guide/page.md", "page", "6,19,3;25,35,6;", true)
   assert(span_rows.len == 2 && span_rows.get(0).get("doc_start") == 6 &&
      span_rows.get(0).get("doc_stop") == 25 &&
      span_rows.get(1).get("doc_start") == 25 &&
      span_rows.get(1).get("doc_stop") == 40,
      "section spans match the trimmed C transport boundaries")
   def rows = markdown_index_rows(
      "Lead\n\n## First ##  \nOne\n\n### Second\nTwo\n",
      "67756964652f706167652e6d64", "70616765", "6,19,3;25,35,6;")
   assert(rows ==
      "67756964652f706167652e6d64|3|6|25|First\n" +
      "67756964652f706167652e6d64|6|25|40|Second\n",
      "compact index rows preserve heading names, lines, and spans")
   def plain = markdown_sections("  text\n", "guide/plain.md", "plain")
   assert(plain.len == 1 && plain.get(0).get("name") == "plain" &&
      plain.get(0).get("doc") == "text",
      "indexes documents without headings")
   print("✓ core.compiler.tools.doc.index tests passed")
}
