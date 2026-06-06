;; Keywords: data serialization csv delimited parse
;; Comma-Separated Values (CSV) Parser and Generator for Nytrix
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc4180.html
;; References:
;; - std.parse.data
;; - std.parse
module std.parse.data.csv(decode, encode)
use std.core
use std.core.str as str

fn decode(any data, str sep=",") list {
   "Decodes a CSV string into a list of lists."
   if(!is_str(data)){ return [] }
   def n = data.len
   mut rows = []
   mut row = []
   mut cell_b = Builder(64)
   mut in_quote = false
   mut i = 0
   def s_code = load8(sep, 0)
   while(i < n){
      def c = load8(data, i)
      if(in_quote){
         case c {
            34 -> { ; '"'
               if(i + 1 < n && load8(data, i + 1) == 34){
                  cell_b = builder_append(cell_b, "\"")
                  i += 1
               } else {
                  in_quote = false
               }
            }
            _ -> { cell_b = builder_append(cell_b, chr(c)) }
         }
      } else {
         case c {
            34 -> { in_quote = true }
            _ if c == s_code -> {
               row = row.append(builder_to_str(cell_b))
               builder_free(cell_b)
               cell_b = Builder(64)
            }
            10 -> {
               row = row.append(builder_to_str(cell_b))
               rows = rows.append(row)
               row = []
               builder_free(cell_b)
               cell_b = Builder(64)
            }
            13 -> {
               if(i + 1 < n && load8(data, i + 1) == 10){ i += 1 }
               row = row.append(builder_to_str(cell_b))
               rows = rows.append(row)
               row = []
               builder_free(cell_b)
               cell_b = Builder(64)
            }
            _ -> { cell_b = builder_append(cell_b, chr(c)) }
         }
      }
      i += 1
   }
   def last_cell = builder_to_str(cell_b)
   if(last_cell.len > 0 || row.len > 0){
      row = row.append(last_cell)
      rows = rows.append(row)
   }
   builder_free(cell_b)
   rows
}

fn encode(list rows, str sep=",") str {
   "Encodes a list of lists into a CSV string."
   mut out = Builder(256)
   mut i = 0
   def r_len = rows.len
   while(i < r_len){
      def row = rows.get(i)
      mut j = 0
      def c_len = row.len
      while(j < c_len){
         mut cell = to_str(row.get(j))
         def needs_quote = str.str_contains(cell, sep) || str.str_contains(cell, "\"") ||
         str.str_contains(cell, "\n") || str.str_contains(cell, "\r")
         if(needs_quote){
            def cell_n = cell.len
            mut escaped = Builder(max(16, cell_n + 8))
            escaped = builder_append(escaped, "\"")
            mut k = 0
            while(k < cell_n){
               def c = load8(cell, k)
               if(c == 34){ escaped = builder_append(escaped, "\"\"") }
               else { escaped = builder_append(escaped, chr(c)) }
               k += 1
            }
            escaped = builder_append(escaped, "\"")
            cell = builder_to_str(escaped)
            builder_free(escaped)
         }
         out = builder_append(out, cell)
         if(j + 1 < c_len){ out = builder_append(out, sep) }
         j += 1
      }
      out = builder_append(out, "\n")
      i += 1
   }
   def s = builder_to_str(out)
   builder_free(out)
   s
}

#main {
   def raw = "name,team,score\nAda,compiler,98\nKen,stdlib,91\n"
   def rows = decode(raw)
   assert_eq(rows.get(0).get(0), "name", "csv header")
   assert_eq(rows.get(1).get(0), "Ada", "csv first row")
   assert_eq(rows.get(2).get(2), "91", "csv field")
   def out = encode([["a", "b,c"], ["quote", "x\"y"]])
   assert(str.str_contains(out, "\"b,c\""), "csv quotes separator")
   assert(str.str_contains(out, "\"x\"\"y\""), "csv escapes quote")
   print("✓ std.parse.data.csv self-test passed")
}
