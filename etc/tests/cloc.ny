#!/bin/ny
;; Cloc (Example) - Line of Code Counter

use std.core *
use std.core.error *
use std.str *
use std.str.glob *
use std.str.io *
use std.str.bytes *
use std.os *
use std.os.fs *
use std.os.args *
use std.os.path as ospath

def SKIP_DIRS = ["build", ".git", ".cache", "node_modules", "__pycache__", ".venv", "dist", "out", ".", ".."]
def SOURCE_ROOTS = ["src", "std", "etc/tests"]
def SOURCE_EXTS = ["**/*.ny", "**/*.nyt", "**/*.c", "**/*.h"]

fn _is_ws(c){
   c == 32 || c == 9 || c == 13
}

fn count_lines(path){
   def src = unwrap_or(file_read(path), "")
   if(str_len(src) == 0){ return 0 }
   mut lc = 0
   def n = str_len(src)
   mut i = 0
   while(i < n){
      def line_start = i
      while(i < n && load8(src, i) != 10){ i += 1 }
      mut k = line_start
      while(k < i && _is_ws(load8(src, k))){ k += 1 }
      if(k < i){
         def c0 = load8(src, k)
         def c1 = (k + 1 < i) ? load8(src, k + 1) : 0
         if(!((c0 == 59 && c1 == 59) || (c0 == 47 && c1 == 47))){
            lc += 1
         }
      }
      if(i < n && load8(src, i) == 10){ i += 1 }
   }
   lc
}

fn pattern_ext(pattern){
   if(endswith(pattern, ".ny")){ return ".ny" }
   if(endswith(pattern, ".nyt")){ return ".nyt" }
   if(endswith(pattern, ".c")){ return ".c" }
   if(endswith(pattern, ".h")){ return ".h" }
   ""
}

fn path_matches(pattern, np, name){
   def ext = pattern_ext(pattern)
   if(str_len(ext) > 0 && (str_contains(pattern, "*") || str_contains(pattern, "?"))){
      return endswith(np, ext) || endswith(name, ext)
   }
   glob_match(pattern, np) || glob_match(pattern, name)
}

fn should_skip_name(b){
   for(s in SKIP_DIRS){
      if(eq(b, s)){ return true }
   }
   false
}

fn collect_files(root, pattern){
   mut out = list(16)
   mut stack = list(8)
   mut seen = dict(64)
   mut r = root
   if(str_len(r) == 0){ r = "." }
   stack = append(stack, r)
   while(len(stack) > 0){
      mut dir = pop(stack)
      if(str_len(dir) == 0){ dir = "." }
      dir = ospath.normalize(dir)
      if(dict_get(seen, dir, false)){ continue }
      seen = dict_set(seen, dir, true)
      if(dir != "." && should_skip_name(ospath.basename(dir))){ continue }

      def names = list_dir(dir)
      mut i = 0
      while(i < len(names)){
         def name = get(names, i)
         if(should_skip_name(name)){ i += 1 continue }
         def p = ospath.join(dir, name)
         if(is_dir(p)){
            stack = append(stack, p)
         } elif(is_file(p)){
            def np = replace_all(p, "\\", "/")
            if(path_matches(pattern, np, name)){
               out = append(out, p)
            }
         }
         i += 1
      }
   }
   out
}

fn files_from_arg(arg){
   if(!is_str(arg) || str_len(arg) == 0){ return list(0) }
   if(is_file(arg)){ return [arg] }
   if(is_dir(arg)){
      mut out = list(16)
      for(pat in SOURCE_EXTS){
         out = extend(out, collect_files(arg, pat))
      }
      return out
   }
   if(str_contains(arg, "*") || str_contains(arg, "?")){
      mut d = ospath.dirname(arg)
      if(str_len(d) == 0 || str_contains(d, "*") || str_contains(d, "?")){ d = "." }
      return collect_files(d, arg)
   }
   list(0)
}

fn dedup_paths(paths){
   mut out = list(len(paths))
   mut seen = dict(len(paths) * 2 + 8)
   for(p in paths){
      if(!dict_get(seen, p, false)){
         seen = dict_set(seen, p, true)
         out = append(out, p)
      }
   }
   out
}

fn print_top(paths, counts, top_n){
   mut lim = top_n
   if(lim < 1){ lim = 1 }
   if(lim > len(counts)){ lim = len(counts) }
   mut used = bytes(len(counts))
   mut rank = 1
   while(rank <= lim){
      mut best_i = -1
      mut best_lc = -1
      mut i = 0
      while(i < len(counts)){
         if(bytes_get(used, i) == 0){
            def lc = get(counts, i, 0)
            if(lc > best_lc){
               best_lc = lc
               best_i = i
            }
         }
         i += 1
      }
      if(best_i < 0){ break }
      bytes_set(used, best_i, 1)
      def best_path = get(paths, best_i, "")
      def best_lc_out = get(counts, best_i, 0)
      print(f"{rank}. {best_path}: {best_lc_out}")
      rank += 1
   }
}

fn is_full_flag(a){
   eq(a, "--full") || eq(a, "-f")
}

fn read_top_flag(a, default_n){
   def parts = split(a, "=")
   if(len(parts) > 1){
      def n = atoi(get(parts, 1, ""))
      if(n > 0){ return n }
   }
   default_n
}

fn is_target_arg(a){
   if(!is_str(a) || str_len(a) == 0){ return false }
   if(startswith(a, "-")){ return false }
   if(is_file(a) || is_dir(a)){ return true }
   str_contains(a, "*") || str_contains(a, "?")
}

def a = args()
mut targets = list(8)
mut show_full = false
mut top_n = 20

mut i = 1
while(i < len(a)){
   def cur = get(a, i, "")
   if(eq(cur, "--")){
      i += 1
      continue
   } elif(is_full_flag(cur)){
      show_full = true
   } elif(startswith(cur, "--top=")){
      top_n = read_top_flag(cur, top_n)
   } elif(is_target_arg(cur)){
      targets = append(targets, cur)
   }
   i += 1
}

mut files = list(64)
if(len(targets) > 0){
   for(t in targets){
      files = extend(files, files_from_arg(t))
   }
} else {
   for(root in SOURCE_ROOTS){
      if(is_dir(root)){
         for(pat in SOURCE_EXTS){
            files = extend(files, collect_files(root, pat))
         }
      }
   }
}
files = dedup_paths(files)

mut row_paths = list(len(files))
mut row_counts = list(len(files))
mut total_lines = 0
mut total_src = 0
mut total_std = 0
mut total_tests = 0

for(f in files){
   def lc = count_lines(f)
   row_paths = append(row_paths, f)
   row_counts = append(row_counts, lc)
   total_lines += lc

   def np = replace_all(f, "\\", "/")
   if(startswith(np, "src/")){
      total_src += lc
   } elif(startswith(np, "std/")){
      total_std += lc
   } elif(startswith(np, "etc/tests/")){
      total_tests += lc
   }

   if(show_full){
      print(f"{f}: {lc}")
   }
}

if(show_full){
   print(f"Total lines: {total_lines}")
} else {
   print(f"Files: {len(files)}")
   print(f"Total lines: {total_lines}")
   print(f"src: {total_src}")
   print(f"std: {total_std}")
   print(f"etc/tests: {total_tests}")
   print(f"other: {total_lines - total_src - total_std - total_tests}")
   print(f"Top {top_n}:")
   print_top(row_paths, row_counts, top_n)
}
