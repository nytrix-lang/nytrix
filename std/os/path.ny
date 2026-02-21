;; Keywords: os path
;; Path helpers.

module std.os.path (
   sep, has_sep, is_abs, join, normalize, basename, dirname, extname, splitext
)
use std.core *
use std.str *

fn _is_windows(){
   "Internal helper."
   eq(__os_name(), "windows")
}

fn sep(){
   "Returns the platform-specific path separator ('\\\\' on Windows, '/' otherwise)."
   if(_is_windows()){ return "\\" }
   "/"
}

fn _is_sep(c){
   "Internal helper."
   if(c == 47){ return true }
   if(c == 92){ return true }
   false
}

fn _is_alpha(c){
   "Internal helper."
   if(c >= 65){
      if(c <= 90){ return true }
   }
   if(c >= 97){
      if(c <= 122){ return true }
   }
   false
}

fn has_sep(p){
   "Returns true if path contains '/' or '\\\\'."
   if(!is_str(p)){ return false }
   def n = str_len(p)
   mut i = 0
   while(i < n){
      if(_is_sep(__load8_idx(p, i))){ return true }
      i += 1
   }
   false
}

fn is_abs(p){
   "Function `is_abs`."
   if(!is_str(p)){ return false }
   def n = str_len(p)
   if(n == 0){ return false }
   def c0 = __load8_idx(p, 0)
   if(_is_windows()){
      if(n >= 2 && __load8_idx(p, 1) == 58){ return true }
      if(n >= 2 && c0 == 92 && __load8_idx(p, 1) == 92){ return true }
      if(n >= 2 && c0 == 47 && __load8_idx(p, 1) == 47){ return true }
      return false
   }
   c0 == 47
}

fn join(a, b){
   "Function `join`."
   if(!is_str(a) || str_len(a) == 0){ return b }
   if(!is_str(b) || str_len(b) == 0){ return a }
   if(is_abs(b)){ return b }
   def s = sep()
   def al = str_len(a)
   def bl = str_len(b)
   if(al > 0 && bl > 0){
      def ac = __load8_idx(a, al - 1)
      def bc = __load8_idx(b, 0)
      if(_is_sep(ac) || _is_sep(bc)){ return a + b }
   }
   a + s + b
}

fn _is_drive_prefix(p){
   "Internal helper."
   if(!is_str(p)){ return false }
   def n = str_len(p)
   if(n < 2){ return false }
   if(!_is_alpha(load8(p, 0))){ return false }
   if(load8(p, 1) != 58){ return false }
   true
}

fn _last_sep_idx(s){
   "Internal helper."
   if(!is_str(s)){ return -1 }
   mut i = str_len(s) - 1
   while(i >= 0){
      if(_is_sep(load8(s, i))){ return i }
      i -= 1
   }
   -1
}

fn _slice_str(s, start, stop){
   "Internal helper."
   if(!is_str(s)){ return "" }
   def n = str_len(s)
   if(start < 0){ start = 0 }
   if(stop < 0){ stop = 0 }
   if(start > n){ start = n }
   if(stop > n){ stop = n }
   if(start >= stop){ return "" }
   def m = stop - start
   def out = malloc(m + 1)
   if(!out){ return "" }
   init_str(out, m)
   mut i = 0
   while(i < m){
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, m)
   out
}

fn normalize(p){
   "Function `normalize`."
   if(!is_str(p)){ return "" }
   if(str_len(p) == 0){ return "" }
   def sepch = sep()
   mut s = p
   if(_is_windows()){
      s = replace_all(s, "/", "\\")
   } else {
      s = replace_all(s, "\\", "/")
   }
   def n = str_len(s)
   mut prefix = ""
   mut abs = false
   mut rest = s
   if(_is_windows()){
      if(n >= 2 && _is_sep(load8(s, 0)) && _is_sep(load8(s, 1))){
         prefix = "\\\\"
         abs = true
         rest = _slice_str(s, 2, n)
      } elif(_is_drive_prefix(s)){
         prefix = _slice_str(s, 0, 2)
         rest = _slice_str(s, 2, n)
         if(str_len(rest) > 0 && _is_sep(load8(rest, 0))){
            abs = true
            rest = _slice_str(rest, 1, str_len(rest))
         }
      } elif(n > 0 && _is_sep(load8(s, 0))){
         abs = true
         rest = _slice_str(s, 1, n)
      }
   } else {
      if(_is_sep(load8(s, 0))){
         abs = true
         rest = _slice_str(s, 1, n)
      }
   }
   def raw_parts = split(rest, sepch)
   mut parts = list(len(raw_parts))
   mut i = 0
   while(i < len(raw_parts)){
      def comp = get(raw_parts, i, "")
      if(str_len(comp) == 0 || comp == "."){
         i += 1
         continue
      }
      if(comp == ".."){
         if(len(parts) > 0){
            def last = get(parts, len(parts) - 1, "")
            if(last != ".."){
               pop(parts)
               i += 1
               continue
            }
         }
         if(!abs){ parts = append(parts, comp) }
         i += 1
         continue
      }
      parts = append(parts, comp)
      i += 1
   }
   mut out = ""
   if(str_len(prefix) > 0){
      if(abs && _is_drive_prefix(prefix)){
         out = prefix + sepch
      } else {
         out = prefix
      }
   } else if(abs){
      out = sepch
   }
   mut idx = 0
   def part_count = len(parts)
   while(idx < part_count){
      def part = get(parts, idx, "")
      if(str_len(out) > 0){
         def last = load8(out, str_len(out) - 1)
         if(!_is_sep(last)){ out = out + sepch }
      }
      out = out + part
      idx += 1
   }
   if(str_len(out) == 0 && part_count == 0){
      if(abs){ return sepch }
      if(str_len(prefix) > 0){ return prefix }
      if(n == 0){ return "" }
      return "."
   }
   out
}

fn basename(p){
   "Function `basename`."
   if(!is_str(p)){ return "" }
   def npath = normalize(p)
   def n = str_len(npath)
   if(n == 0){ return "" }
   def s = sep()
   if(npath == s){ return s }
   mut i = n - 1
   while(i >= 0 && _is_sep(load8(npath, i))){ i -= 1 }
   if(i < 0){ return sep() }
   def head = _slice_str(npath, 0, i + 1)
   def j = _last_sep_idx(head)
   if(j < 0){ return head }
   _slice_str(head, j + 1, str_len(head))
}

fn dirname(p){
   "Function `dirname`."
   if(!is_str(p)){ return "." }
   def npath = normalize(p)
   def n = str_len(npath)
   if(n == 0){ return "." }
   def s = sep()
   if(npath == s){ return s }
   if(_is_windows() && _is_drive_prefix(npath) && n == 3 && _is_sep(load8(npath, 2))){
      return npath
   }
   mut end = n
   while(end > 1 && _is_sep(load8(npath, end - 1))){ end -= 1 }
   def trimmed = _slice_str(npath, 0, end)
   def j = _last_sep_idx(trimmed)
   if(j < 0){ return "." }
   if(j == 0){ return s }
   if(_is_windows() && j == 2 && _is_drive_prefix(trimmed)){
      return _slice_str(trimmed, 0, 3)
   }
   _slice_str(trimmed, 0, j)
}

fn _last_index_byte(s, want){
   "Internal helper."
   if(!is_str(s)){ return -1 }
   mut i = str_len(s) - 1
   while(i >= 0){
      if(__load8_idx(s, i) == want){ return i }
      i -= 1
   }
   -1
}

fn extname(p){
   "Returns the file extension, including the dot (e.g. '.txt')."
   if(!is_str(p)){ return "" }
   def b = basename(p)
   def n = str_len(b)
   if(n == 0){ return "" }
   def dot = _last_index_byte(b, 46) ;; '.'
   if(dot <= 0 || dot == n - 1){ return "" }
   def out = malloc(n - dot + 1)
   if(!out){ return "" }
   init_str(out, n - dot)
   mut i = 0
   while(i < n - dot){
      store8(out, __load8_idx(b, dot + i), i)
      i += 1
   }
   store8(out, 0, n - dot)
   out
}

fn splitext(p){
   "Splits path into [root, ext]."
   if(!is_str(p)){ return ["", ""] }
   def ext = extname(p)
   if(str_len(ext) == 0){ return [p, ""] }
   def root_len = str_len(p) - str_len(ext)
   if(root_len < 0){ return [p, ""] }
   [_slice_str(p, 0, root_len), ext]
}

if(comptime{__main()}){
    use std.os.path as path
    use std.core *
    use std.core.error *
    use std.str *

    def s = path.sep()
    assert(str_len(s) == 1, "sep is one byte")

    def is_win = (s == "\\")

    if(is_win){
       assert(path.is_abs("C:\\tmp"), "windows is_abs drive")
       assert(path.is_abs("\\\\server\\share"), "windows is_abs unc")
    } else {
       assert(path.is_abs("/tmp"), "unix is_abs absolute")
       assert(!path.is_abs("tmp"), "unix is_abs relative")
    }

    assert((path.join("a", "b") == "a" + s + "b"), "join simple")
    assert((path.normalize("a/./b/../c") == "a" + s + "c"), "normalize relative")

    assert((path.basename("a" + s + "b" + s + "c.txt") == "c.txt"), "basename file")
    assert((path.dirname("a" + s + "b" + s + "c.txt") == "a" + s + "b"), "dirname nested")

    if(!is_win){
       assert((path.dirname("/tmp") == "/"), "dirname absolute child")
       assert((path.dirname("/") == "/"), "dirname root")
       assert((path.basename("/") == "/"), "basename root")
    }

    assert((path.extname("file.txt") == ".txt"), "extname simple")
    assert((path.extname("archive.tar.gz") == ".gz"), "extname last extension")
    assert((path.extname(".bashrc") == ""), "extname hidden no extension")
    assert((path.extname("noext") == ""), "extname missing")

    def sp = path.splitext("archive.tar.gz")
    assert((get(sp, 0) == "archive.tar"), "splitext root")
    assert((get(sp, 1) == ".gz"), "splitext ext")

    def sp2 = path.splitext("noext")
    assert((get(sp2, 0) == "noext"), "splitext noext root")
    assert((get(sp2, 1) == ""), "splitext noext ext")

    print("✓ std.os.path tests passed")
}
