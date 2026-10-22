;; Keywords: documentation metadata header
;; Parse leading Ny source comments for documentation metadata.
;; References:
;; - core
;; - core.str
module core.compiler.tools.doc.meta(parse, extract, collect, resolve, render_entry,
   render_matches, render_missing)
use core
use core.str as str
use core.os (file_read, file_exists)
use core.os.fs as fs
use core.os.path as path

def _MAX_DEPTH = 64
def _MAX_ENTRIES = 100000

fn _lstrip(str value) str {
   mut start = 0
   while start < value.len &&
   (load8(value, start) == 32 || load8(value, start) == 9) {
      start += 1
   }
   str._substr(value, start, value.len)
}

fn _header_fields(str source, dict fields) str {
   mut title = ""
   mut keywords = ""
   def int source_len = source.len
   mut int start = 0
   while start < source_len {
      mut int stop = start
      while stop < source_len && load8(source, stop) != 10 { stop += 1 }
      mut line = _lstrip(str._substr(source, start, stop))
      start = stop + 1
      if line.len > 0 && load8(line, line.len - 1) == 13 {
         line = str._substr(line, 0, line.len - 1)
      }
      if str.startswith(str.lower(line), ";; keywords:") {
         keywords = str._substr(line, 12, line.len)
      } elif str.startswith(line, ";;") {
         def text = _lstrip(str._substr(line, 2, line.len))
         if title.len == 0 && text.len > 0 && text.len < 256 {
            title = text
         }
      } elif line.len > 0 && load8(line, 0) != 35 {
         break
      }
   }
   fields.set("keywords", str.cstr_to_str(keywords))
   str.cstr_to_str(title)
}

fn parse(str source) dict {
   "Extracts the initial `;;` title and `;; Keywords:` fields from source."
   mut fields = dict(1)
   def title = _header_fields(source, fields)
   def keywords = str.cstr_to_str(str(fields.get("keywords", "")))
   def bounded_keywords = str._substr(keywords, 0, min(keywords.len, 767))
   mut normalized = []
   def comma_parts = str.split(bounded_keywords, ",")
   for part in comma_parts {
      def spaced = str.str_replace(str.str_replace(part, "\t", " "), "\r", " ")
      def words = str.split_words(spaced)
      for word in words {
         def short_word = str.cstr_to_str(
            str._substr(word, 0, min(word.len, 127)))
         if short_word.len > 0 {
            normalized = normalized.append(str.cstr_to_str(str.lower(short_word)))
         }
      }
   }
   {"title":str.cstr_to_str(title), "keywords":normalized}
}

fn extract(str path) dict {
   "Reads a source file and returns its documentation header metadata."
   match file_read(path) {
      ok(source) -> { return {"ok":true, "value":parse(source)} }
      err(code) -> { return {"ok":false, "error":code} }
   }
}

fn _skip_dir(str name) bool {
   name == ".git" || name == ".cache" || name == "build" || name == "tmp" ||
   name == "node_modules" || name == "__pycache__" ||
   (name.len > 0 && load8(name, 0) == 46)
}

fn _supported(str file) bool {
   path.extname(file) == ".md" || path.extname(file) == ".txt" ||
   path.extname(file) == ".texi"
}

fn _route(str root, str file) str {
   def prefix = root.len > 0 && load8(root, root.len - 1) != 47 &&
   load8(root, root.len - 1) != 92 ? root + path.sep() : root
   mut relative = path.basename(file)
   if str.startswith(file, prefix) {
      relative = str._substr(file, prefix.len, file.len)
   }
   def without_ext = path.splitext(relative).get(0)
   str.str_replace(str.cstr_to_str(without_ext), "\\", "/")
}

fn _route_title(str route) str {
   mut title = Builder(route.len + 8)
   mut word_start = true
   mut i = 0
   while i < route.len {
      def byte = load8(route, i)
      if byte == 47 || byte == 92 {
         title = builder_append(title, " / ")
         word_start = true
      } elif byte == 45 || byte == 95 {
         title = builder_append(title, " ")
         word_start = true
      } else {
         def chr = str._substr(route, i, i + 1)
         title = builder_append(title, word_start ? str.upper(chr) : chr)
         word_start = false
      }
      i += 1
   }
   def result = builder_to_str(title)
   builder_free(title)
   result
}

fn _path_depth(str root, str current) int {
   mut start = root.len
   if start < current.len && load8(root, start - 1) != 47 &&
   load8(root, start - 1) != 92 { start += 1 }
   mut depth = start < current.len ? 1 : 0
   mut i = start
   while i < current.len {
      def byte = load8(current, i)
      if byte == 47 || byte == 92 { depth += 1 }
      i += 1
   }
   depth
}

fn _collect_tree(str root, int max_depth, int max_entries) any {
   mut pending = [str.cstr_to_str(root)]
   mut visited = dict(128)
   mut pages = []
   mut cursor = 0
   while cursor < pending.len {
      if cursor >= max_entries { return nil }
      def current = str(pending.get(cursor))
      def depth = _path_depth(root, current)
      cursor += 1
      if depth > max_depth { return nil }
      def canonical = str.cstr_to_str(path.normalize(current))
      if visited.contains(canonical) { continue }
      visited[canonical] = true
      def listed = fs.list_dir_checked(canonical)
      if !is_list(listed) && fs.is_dir(canonical) { return nil }
      def entries = is_list(listed) ? sorted(listed) : []
      if entries.len == 0 {
         if fs.is_dir(canonical) {
            if _supported(canonical) { return nil }
            continue
         }
         if !_supported(canonical) { continue }
         match file_read(canonical) {
            ok(source) -> {
               def route = str.cstr_to_str(_route(root, canonical))
               def metadata = parse(source)
               def title = str(metadata.get("title", ""))
               mut dict page = dict(4)
               page["route"] = route
               if title.len > 0 {
                  page["title"] = title
               } else {
                  page["title"] = _route_title(route)
               }
               page["keywords"] = metadata.get("keywords", [])
               page["path"] = str.cstr_to_str(canonical)
               pages = pages.append(page)
            }
            err(_) -> { return nil }
         }
         continue
      }
      if pending.len + entries.len > max_entries { return nil }
      mut i = 0
      while i < entries.len {
         def name = str(entries.get(i))
         i += 1
         if _skip_dir(name) { continue }
         def child = str.cstr_to_str(path.join(canonical, name))
         pending = pending.append(child)
      }
   }
   mut ordered = []
   for page in pages {
      def route = str(page.get("route"))
      mut at = ordered.len
      ordered = ordered.append(page)
      while at > 0 &&
      str(ordered.get(at - 1).get("route")) > route {
         ordered[at] = ordered.get(at - 1)
         at -= 1
      }
      ordered[at] = page
   }
   ordered
}

fn collect(any root, int max_depth=32, int max_entries=100000) any {
   "Returns route-sorted documentation records; returns nil on invalid roots or resource-limit breaches."
   if !is_str(root) || root.len == 0 || max_depth < 0 || max_entries < 1 ||
   max_depth > _MAX_DEPTH || max_entries > _MAX_ENTRIES {
      return nil
   }
   def normalized = path.normalize(root)
   if !file_exists(normalized) || !fs.is_dir(normalized) { return nil }
   _collect_tree(normalized, max_depth, max_entries)
}

fn _dotted_suffix_match(str left, str right) bool {
   if left.len > right.len {
      def start = left.len - right.len
      return load8(left, start - 1) == 46 &&
      str._substr(left, start, left.len) == right
   }
   if right.len > left.len {
      def start = right.len - left.len
      return load8(right, start - 1) == 46 &&
      str._substr(right, start, right.len) == left
   }
   false
}

fn _basename(str name) str {
   mut start = 0
   mut i = 0
   while i < name.len {
      if load8(name, i) == 46 { start = i + 1 }
      i += 1
   }
   str._substr(name, start, name.len)
}

fn _resolve_matches(list names, str query, bool suffix) dict {
   mut matches = []
   mut i = 0
   while i < names.len {
      def name = names.get(i)
      if is_str(name) {
         mut matched = false
         if suffix {
            matched = _dotted_suffix_match(name, query)
         } else {
            matched = _basename(name) == query
         }
         if matched { matches = matches.append(i) }
      }
      i += 1
   }
   if matches.len == 1 {
      return {"kind":"found", "index":matches.get(0), "matches":matches}
   }
   if matches.len > 1 {
      return {"kind":"ambiguous", "index":-1, "matches":matches}
   }
   {"kind":"missing", "index":-1, "matches":[]}
}

fn resolve(list names, str query) dict {
   "Resolves a documentation query by exact name, dotted suffix, then basename."
   if query.len == 0 { return {"kind":"missing", "index":-1, "matches":[]} }
   mut i = 0
   while i < names.len {
      if is_str(names.get(i)) && names.get(i) == query {
         return {"kind":"found", "index":i, "matches":[i]}
      }
      i += 1
   }
   def suffix = _resolve_matches(names, query, true)
   if suffix.get("kind") != "missing" { return suffix }
   _resolve_matches(names, _basename(query), false)
}

fn render_matches(str query, list names, list indices, str yellow, str reset) str {
   "Renders the bounded ambiguity message for a documentation lookup."
   if indices.len < 2 { return "" }
   mut output = yellow + "Multiple matches found for '" + query + "':" + reset + "\n"
   for index in indices {
      if is_int(index) {
         def int i = int(index)
         if i >= 0 && i < names.len {
            def any name = names.get(i)
            if is_str(name) { output += "  - " + str(name) + "\n" }
         }
      }
   }
   output
}

fn render_missing(str query, str red, str reset) str {
   "Renders the REPL message for a documentation name that did not resolve."
   red + "No documentation found for '" + query + "'" + reset + "\n"
}

fn render_entry(int kind, str name, str definition, str documentation,
   bool has_source, str bold, str magenta, str reset, str cyan,
   str green, str gray) str {
   "Renders the REPL documentation heading, definition, and body with caller-selected terminal styles."
   def label = kind == 1 ? "Package" : kind == 2 ? "Module" :
   kind == 3 ? "Function" : kind == 5 ? "Method" : "Symbol"
   mut output = "\n" + bold + magenta + label + reset + " " + cyan + name +
   reset + "\n"
   if definition.len > 0 {
      output += "  " + green + definition + reset + "\n"
   }
   if documentation.len > 0 {
      output += "  " + documentation + "\n"
   } else {
      output += "  " + gray + "(no documentation string available)" + reset +
      "\n"
   }
   if !has_source { output += "\n" }
   output
}

#main {
   assert(render_entry(3, "core.math.sin", "fn sin(float x) float", "Returns the sine.", false, "<b>", "<m>", "<r>", "<c>", "<g>", "<d>") ==
      "\n<b><m>Function<r> <c>core.math.sin<r>\n  <g>fn sin(float x) float<r>\n  Returns the sine.\n\n",
      "documentation entry renderer preserves styled layout")
   assert(render_entry(5, "Thing.run", "", "", true, "", "", "", "", "", "") ==
      "\nMethod Thing.run\n  (no documentation string available)\n",
      "documentation renderer handles missing body and following source")
}

#main {
   def metadata = parse(";; Example page\n;; Keywords: Ny, Compiler, parser\n\n# Content\n")
   assert(metadata.get("title") == "Example page" &&
      metadata.get("keywords") == ["ny", "compiler", "parser"],
      "documentation metadata parses title and normalized keywords")
   def late = parse("# Heading\n;; ignored\n")
   assert(late.get("title") == "" && late.get("keywords").len == 0,
      "documentation metadata only reads the initial comment header")
   def names = ["core.math.sin", "core.math.cos", "core.math.sin"]
   assert(resolve(names, "core.math.cos").get("index") == 1 &&
      resolve(names, "math.cos").get("index") == 1,
      "documentation resolution prioritizes exact and unique suffix matches")
   assert(resolve(names, "sin").get("kind") == "ambiguous" &&
      resolve(names, "missing").get("kind") == "missing",
      "documentation resolution reports ambiguity and misses explicitly")
   assert(render_matches("sin", names, [0, 2], "<y>", "<r>") ==
      "<y>Multiple matches found for 'sin':<r>\n  - core.math.sin\n  - core.math.sin\n",
      "documentation ambiguity rendering preserves matched names")
   assert(render_missing("missing", "<e>", "<r>") ==
      "<e>No documentation found for 'missing'<r>\n",
      "documentation miss rendering preserves the REPL message")
   print("✓ core.compiler.tools.doc.meta tests passed")
}
