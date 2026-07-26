;; Known C-library signatures for call and type recovery.
module std.os.rev.decomp.type_library *

use std.core
use std.core.str as str
use std.os.rev.decomp.elf

fn _type_call_name(str name0) str {
   str.lower(_display_symbol(name0))
}

fn _type_library_signature(str name0) dict {
   def name = _type_call_name(name0)
   if name == "malloc" {
      return {"name": "malloc", "category": "allocation",
         "return": {"type": "void*", "shape": "heap_ptr", "role": "allocation"},
      "args": [{"name": "size", "type": "usize", "role": "alloc_size"}]}
   }
   if name == "calloc" {
      return {"name": "calloc", "category": "allocation",
         "return": {"type": "void*", "shape": "heap_ptr", "role": "zeroed_allocation"},
         "args": [{"name": "count", "type": "usize", "role": "alloc_count"},
      {"name": "elem_size", "type": "usize", "role": "alloc_elem_size"}]}
   }
   if name == "realloc" {
      return {"name": "realloc", "category": "allocation",
         "return": {"type": "void*", "shape": "heap_ptr", "role": "allocation_resize"},
         "args": [{"name": "ptr", "type": "void*", "role": "allocation"},
      {"name": "size", "type": "usize", "role": "alloc_size"}]}
   }
   if name == "free" {
      return {"name": "free", "category": "allocation",
         "return": {"type": "void", "shape": "none", "role": "deallocation"},
      "args": [{"name": "ptr", "type": "void*", "role": "allocation"}]}
   }
   if name == "fgets" {
      return {"name": "fgets", "category": "input",
         "return": {"type": "char*", "shape": "nullable_buffer_ptr", "role": "input_result"},
         "args": [{"name": "dst", "type": "char*", "role": "input_buffer"},
            {"name": "size", "type": "usize", "role": "buffer_size"},
      {"name": "stream", "type": "FILE*", "role": "stream"}]}
   }
   if name == "fread" {
      return {"name": "fread", "category": "input",
         "return": {"type": "usize", "shape": "scalar", "role": "bytes_read"},
         "args": [{"name": "dst", "type": "void*", "role": "input_buffer"},
            {"name": "size", "type": "usize", "role": "element_size"},
            {"name": "count", "type": "usize", "role": "element_count"},
      {"name": "stream", "type": "FILE*", "role": "stream"}]}
   }
   if name == "read" {
      return {"name": "read", "category": "input",
         "return": {"type": "ssize", "shape": "scalar", "role": "bytes_read"},
         "args": [{"name": "fd", "type": "int", "role": "fd"},
            {"name": "buf", "type": "bytes*", "role": "input_buffer"},
      {"name": "count", "type": "usize", "role": "buffer_size"}]}
   }
   if name == "memcpy" || name == "memmove" {
      return {"name": name, "category": "memory_transform",
         "return": {"type": "void*", "shape": "ptr", "role": "dst"},
         "args": [{"name": "dst", "type": "void*", "role": "write_buffer"},
            {"name": "src", "type": "const void*", "role": "read_buffer"},
      {"name": "len", "type": "usize", "role": "buffer_size"}]}
   }
   if name == "memset" || name == "memfrob" {
      mut args = []
      if name == "memfrob" {
         args = [{"name": "buf", "type": "void*", "role": "write_buffer"},
         {"name": "len", "type": "usize", "role": "buffer_size"}]
      } else {
         args = [{"name": "dst", "type": "void*", "role": "write_buffer"},
            {"name": "byte", "type": "u8", "role": "fill_byte"},
         {"name": "len", "type": "usize", "role": "buffer_size"}]
      }
      return {"name": name, "category": "memory_transform",
      "return": {"type": "void*", "shape": "ptr", "role": "dst"}, "args": args}
   }
   if name == "strcmp" || name == "strncmp" || name == "memcmp" {
      mut args = [{"name": "lhs", "type": name == "memcmp" ? "const void*" : "const char*", "role": "compare_lhs"},
      {"name": "rhs", "type": name == "memcmp" ? "const void*" : "const char*", "role": "compare_rhs"}]
      if name == "strncmp" || name == "memcmp" { args = args.append({"name": "len", "type": "usize", "role": "compare_size"}) }
      return {"name": name, "category": "compare_parse",
      "return": {"type": "int", "shape": "scalar", "role": "ordering"}, "args": args}
   }
   if name == "strlen" {
      return {"name": "strlen", "category": "compare_parse",
         "return": {"type": "usize", "shape": "scalar", "role": "length"},
      "args": [{"name": "s", "type": "const char*", "role": "string"}]}
   }
   if name == "atoi" || name == "strtol" {
      return {"name": name, "category": "compare_parse",
         "return": {"type": name == "atoi" ? "int" : "long", "shape": "scalar", "role": "parsed_int"},
      "args": [{"name": "s", "type": "const char*", "role": "string"}]}
   }
   if name == "fopen" {
      return {"name": "fopen", "category": "os",
         "return": {"type": "FILE*", "shape": "handle", "role": "stream"},
         "args": [{"name": "path", "type": "const char*", "role": "path"},
      {"name": "mode", "type": "const char*", "role": "mode"}]}
   }
   if name == "fclose" {
      return {"name": "fclose", "category": "os",
         "return": {"type": "int", "shape": "scalar", "role": "status"},
      "args": [{"name": "stream", "type": "FILE*", "role": "stream"}]}
   }
   dict()
}
