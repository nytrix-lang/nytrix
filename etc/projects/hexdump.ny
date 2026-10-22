#!/usr/bin/env ny
;; Keywords: cli hexdump file bytes example
;; A minimal xxd-style hex dump in Nytrix.
;;
;;   hexdump hello.txt
;;   00000000  48 65 6c 6c 6f 2c 20 43  21 0a                |Hello, C!.|
;;
;; Options:
;;   -n N   show at most N bytes
;;   -s N   start reading at byte offset N
;;   -w N   bytes per row (default 16)
use core
use core.os (exit, file_read)
use core.os.args as cli
use core.str as str

def USAGE = "usage: hexdump [-n N] [-s N] [-w N] FILE"

fn parse_count(str text, int fallback) int {
   "Parses a non-negative decimal count, or returns `fallback`."
   text.len == 0 ? fallback : str.atoi(text)
}

fn print_row(str data, int off, int end, int width) {
   "Prints one row of `width` bytes starting at byte offset `off`."
   def half = width / 2
   mut hex = str.to_hex(off, 8) + "  "
   mut ascii = ""
   mut j = 0
   while j < width {
      if j == half { hex = hex + " " }
      def at = off + j
      if at < end {
         def b = str.byte_at(data, at)
         hex = hex + str.to_hex(b, 2) + " "
         ascii = ascii + (str.ascii_is_printable(b) ? str.chr(b) : ".")
      } else {
         hex = hex + "   "
      }
      j += 1
   }
   print(hex + " |" + ascii + "|")
}

def argv = cli.args()
mut path = ""
mut limit = -1
mut start = 0
mut width = 16
mut i = 1
while i < argv.len {
   def arg = to_str(argv.get(i))
   if arg == "-n" || arg == "-s" || arg == "-w" {
      def val = (i + 1 < argv.len) ? to_str(argv.get(i + 1)) : ""
      if arg == "-n" { limit = parse_count(val, limit) }
      elif arg == "-s" { start = parse_count(val, start) }
      else {
         def w = parse_count(val, width)
         if w > 0 { width = w }
      }
      i += 2
   } else {
      if path.len == 0 { path = arg }
      i += 1
   }
}

if path.len == 0 {
   eprint(USAGE)
   exit(2)
}

match file_read(path) {
   err(code) -> {
      eprint("hexdump: cannot read " + path + " (error " + to_str(code) + ")")
      exit(1)
   }
   ok(data) -> {
      def total = data.len
      mut from = start < 0 ? 0 : start
      if from > total { from = total }
      mut to = total
      if limit >= 0 && from + limit < to { to = from + limit }
      mut off = from
      while off < to {
         print_row(data, off, to, width)
         off += width
      }
   }
}

0
