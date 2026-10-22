;; Keywords: cli commands aliases dispatch
;; Classifies Nytrix command tokens, aliases, and leading global options.
;; References:
;; - core.str
;; - core.os
module core.compiler.tools.cli(select_unified_command)
use core.str (split)
use core.str as str
use core.os (file_write, exit)

fn _decode_hex(str hex) str {
   if hex.len % 2 != 0 { return "" }
   mut str value = ""
   mut int i = 0
   while i < hex.len {
      def int high = str.hex_val(load8(hex, i))
      def int low = str.hex_val(load8(hex, i + 1))
      if high < 0 || high > 15 || low < 0 || low > 15 { return "" }
      value += chr(high * 16 + low)
      i += 2
   }
   value
}

fn _route(str command, bool after_leading_args) int {
   if command == "fmt" || command == "format" { return 1 }
   if command == "test" || command == "tests" { return 2 }
   if command == "perf" || command == "bench" { return 3 }
   if command == "doc" || command == "docs" || command == "web" { return 4 }
   if command == "make" || command == "build" { return 5 }
   if command == "pkg" { return 6 }
   if command == "new" { return 7 }
   if command == "get" || command == "install" {
      return if after_leading_args { 8 } else { 10 }
   }
   if command == "lsp" { return 9 }
   0
}

fn _argument_index(str row) int {
   def int tab = str.find(row, "\t")
   if tab <= 0 { return -1 }
   mut int value = 0
   mut int i = 0
   while i < tab {
      def int byte = load8(row, i)
      if byte < 48 || byte > 57 { return -1 }
      value = value * 10 + byte - 48
      if value > 2147483647 { return -1 }
      i += 1
   }
   value
}

fn select_unified_command(str arguments_hex, str output_path_hex) int {
   def str arguments = _decode_hex(arguments_hex)
   def str output_path = _decode_hex(output_path_hex)
   if arguments_hex.len > 2097152 || output_path.len == 0 { exit(2) }
   mut int command_index = 0
   mut int scan_end = 1
   mut str command = ""
   mut int expected_index = 1
   mut bool skip_next = false
   for encoded_row in split(arguments, "\n") {
      if encoded_row.len == 0 { continue }
      def str row = str(encoded_row)
      def int tab = str.find(row, "\t")
      def int index = _argument_index(row)
      if tab < 0 || index != expected_index { exit(2) }
      def str arg = _decode_hex(str.str_slice(row, tab + 1, row.len))
      if str.str_slice(row, tab + 1, row.len).len % 2 != 0 { exit(2) }
      if skip_next {
         skip_next = false
         expected_index += 1
         scan_end = expected_index
         continue
      }
      if arg.len == 0 {
         expected_index += 1
         scan_end = expected_index
         continue
      }
      if arg == "--color" || arg == "--color=" || arg == "--no-color" ||
      str.startswith(arg, "--color=") {
         if arg == "--color" || arg == "--color=" { skip_next = true }
         expected_index += 1
         scan_end = expected_index
         continue
      }
      if arg == "--" || str.startswith(arg, "-") {
         scan_end = index
         break
      }
      command_index = index
      command = arg
      scan_end = index
      break
   }
   def int route = if command_index > 0 {
      _route(command, command_index != 1)
   } else { 0 }
   match file_write(output_path, str(command_index) + "\t" + str(route) +
      "\t" + str(scan_end)) {
      ok(_) -> {}
      err(_) -> exit(2)
   }
   0
}
