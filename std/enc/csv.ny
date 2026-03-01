;; Keywords: enc csv rfc4180
;; CSV Parser and Generator.
;; Reference: https://www.rfc-editor.org/rfc/rfc4180.html

module std.enc.csv (
   decode, encode
)

use std.core *
use std.text as str

fn decode(data, sep=","){
   "Decodes a CSV string into a list of lists."
   if(!is_str(data)){ return [] }
   def n = len(data)
   mut rows = []
   mut row = []
   mut cell = ""
   mut in_quote = false
   mut i = 0
   def s_code = load8(sep, 0)
   while(i < n){
      def c = load8(data, i)
      if(in_quote){
         if(c == 34){ ;; '"'
            if(i + 1 < n && load8(data, i + 1) == 34){
               cell = cell + "\""
               i += 1
            } else {
               in_quote = false
            }
         } else {
            cell = cell + chr(c)
         }
      } else {
         if(c == 34){
            in_quote = true
         } elif(c == s_code){
            row = append(row, cell)
            cell = ""
         } elif(c == 10){ ;; '\n'
            row = append(row, cell)
            rows = append(rows, row)
            row = []
            cell = ""
         } elif(c == 13){ ;; '\r'
            if(i + 1 < n && load8(data, i + 1) == 10){
               i += 1
            }
            row = append(row, cell)
            rows = append(rows, row)
            row = []
            cell = ""
         } else {
            cell = cell + chr(c)
         }
      }
      i += 1
   }
   if(len(cell) > 0 || len(row) > 0){
      row = append(row, cell)
      rows = append(rows, row)
   }
   rows
}

fn encode(rows, sep=","){
   "Encodes a list of lists into a CSV string."
   mut out = ""
   mut i = 0
   def r_len = len(rows)
   while(i < r_len){
      def row = get(rows, i)
      mut j = 0
      def c_len = len(row)
      while(j < c_len){
         mut cell = to_str(get(row, j))
         ;; Escape if needed
         if(str.str_contains(cell, sep) || str.str_contains(cell, "\"") || str.str_contains(cell, "\n") || str.str_contains(cell, "\r")){
            mut escaped = "\""
            mut k = 0
            while(k < len(cell)){
               def c = load8(cell, k)
               if(c == 34){ escaped = escaped + "\"\"" }
               else { escaped = escaped + chr(c) }
               k += 1
            }
            cell = escaped + "\""
         }
         out = out + cell
         if(j + 1 < c_len){ out = out + sep }
         j += 1
      }
      out = out + "\n"
      i += 1
   }
   out
}

if(comptime{__main()}){
   use std.core.error *
   def data = "name,age,city\n\"John \"\"Big\"\" Doe\",30,New York\nJane,25,London"
   def rows = decode(data)
   assert(len(rows) == 3, "csv rows count")
   assert(get(get(rows, 1), 0) == "John \"Big\" Doe", "csv escaped cell")
   assert(get(get(rows, 2), 2) == "London", "csv last cell")
   
   def enc = encode(rows)
   def rows2 = decode(enc)
   assert(len(rows2) == 3, "csv roundtrip count")
   assert(get(get(rows2, 1), 0) == "John \"Big\" Doe", "csv roundtrip data")
   
   ;; Edge cases
   assert(len(decode("")) == 0, "empty input")
   assert(len(decode(123)) == 0, "non-string input")

   ;; Single row
   def rows3 = decode("a,b,c")
   assert(len(rows3) == 1, "single row")
   assert(len(get(rows3, 0)) == 3, "single row columns")

   ;; Empty cells
   def rows4 = decode("a,,c")
   assert(get(get(rows4, 0), 1) == "", "empty cell")

   ;; Quoted newline
   def rows6 = decode("a,\"b\nc\",d")
   assert(len(rows6) == 1, "quoted newline row count")
   assert(get(get(rows6, 0), 1) == "b\nc", "quoted newline content")

   ;; Semicolon separator
   def rows7 = decode("a;b;c", ";")
   assert(len(rows7) == 1, "semicolon separator")
   assert(len(get(rows7, 0)) == 3, "semicolon columns")

   ;; Only separators
   def rows8 = decode(",,,")
   assert(len(get(rows8, 0)) == 4, "only separators")

   print("✓ std.enc.csv tests passed")
}
