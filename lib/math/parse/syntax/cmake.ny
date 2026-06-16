;; Keywords: syntax cmake build-system parse highlight
;; CMake syntax highlighter
;; References:
;; - std.math.parse.syntax
;; - std.math.parse.syntax.helpers
module std.math.parse.syntax.cmake(tokenize)
use std.core
use std.core.str as str
use std.math.parse.syntax.helpers as _h

def KW = "if;else;elseif;endif;foreach;endforeach;while;endwhile;function;endfunction;macro;endmacro;include;include_directories;add_executable;add_library;add_subdirectory;add_definitions;target_link_libraries;target_include_directories;set;unset;option;find_package;find_path;find_file;find_library;find_program;project;cmake_minimum_required;set_property;get_property;get_target_property;set_target_properties;configure_file;install;export;message;return;break;continue;list;string;math;file;exec_program;execute_process;add_custom_command;add_custom_target;add_test;enable_testing;ctest"

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   while i < src_len {
      def ch = load8(source, i)
      if _h.is_space_ch(ch) {
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif ch == 35 {
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif ch == 34 {
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif _h.is_alpha_ch(ch) {
         def j = _h.scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         if _h.in_list(word, KW) { out_tokens = _h.add_tok(out_tokens, 0, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif ch == 36 && i + 1 < src_len && load8(source, i + 1) == 123 {
         mut j = i + 2
         while j < src_len && load8(source, j) != 125 { j += 1 }
         if j < src_len { j += 1 }
         out_tokens = _h.add_tok(out_tokens, 8, i, j - i)
         i = j
      } elif ch == 40 || ch == 41 {
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
