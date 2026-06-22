;; Keywords: ffi foreign-function-interface interop os
;; Os Ffi for Nytrix
;; References:
;; - std.os
module std.os.ffi(RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL,
   dlopen, dlopen_any, dlopen_checked, dlsym, dlclose, dlerror,
   call0_void, call0_i32, call1_void, call1_u32_void, call2_void,
   call3_void, call4_void, call5_void, call7_void, call9_void,
   call1_f32_void, call2_f32_void, call3_f32_void, call4_f32_void,
   call0, call1, call1_i64, call1_u32, call2, call3, call4, call5,
   call0_ptr, call1_ptr, call2_ptr, call3_ptr, call4_ptr, call5_ptr,
   call2_ptr_u32, call3_ptr_u64_ptr, call3_ptr_u32_ptr,
   call3_ptr_ptr_u32, call4_ptr_ptr_ptr_ptr_void,
   call6, call7, call8, call9, call10, call11, call12, call13, call14, call15,
   ffi_call, bind, call_ext, bind_all, bind_linked, import_all, import_linked, extern_all,
   __call0, __call0_void, __call0_ptr, __call1, __call1_ptr, __call1_void, __call2, __call2_ptr,
   __call2_void, __call3, __call3_ptr, __call3_void, __call4, __call4_ptr, __call4_void, __call5, __call5_ptr,
   __call5_void,
   __call6, __call7, __call7_void, __call8, __call9, __call9_void, __call10, __call11, __call12,
   __call13, __call14, __call15,
   __call2_ptr_u32, __call3_ptr_u64_ptr, __call3_ptr_u32_ptr,
   __call3_ptr_ptr_u32, __call3_ptr_u64_ptr_i32,
   __call4_ptr_ptr_ptr_ptr_i32, __call4_ptr_ptr_ptr_u64_i32,
   __call4_ptr_u32_u64_ptr_i32, __call4_ptr_u64_ptr_ptr_i32,
   __call5_ptr_ptr_ptr_u64_i32_i32, __call1_f32_void, __call2_f32_void,
   __call3_f32_void, __call4_f32_void, __call4_ptr_ptr_ptr_ptr_void,
   CStruct, CType, cstr, cptr, u8, i8, u16, i16, u32, i32, u64, i64,
   f32, f64, ptr, handle, sizeof_struct, offsetof_struct, malloc, free,
cstruct_set, cstruct_get, bind_lib, tag_native)

use std.core
use std.core.mem as mem
use std.core.dict_mod as _d
use std.os.prim
use std.os.path as ospath

fn RTLD_LAZY() int {
   "Returns the RTLD_LAZY flag for `dlopen` (1)."
   1
}

fn RTLD_NOW() int {
   "Returns the RTLD_NOW flag for `dlopen` (2)."
   2
}

fn RTLD_GLOBAL() int {
   "Returns the RTLD_GLOBAL flag for `dlopen` (256)."
   256
}

fn RTLD_LOCAL() int {
   "Returns the RTLD_LOCAL flag for `dlopen` (0)."
   0
}

fn dlopen(str path, int flags) any {
   "Opens a dynamic library at `path` with the given `flags`. Returns a library handle, or 0 on failure."
   __dlopen(path, flags)
}

fn dlsym(any h, any s) any {
   "Returns the memory address of the symbol `s` within library handle `h`."
   if is_str(s) { return __dlsym(h, s) }
   0
}

fn dlclose(any h) any {
   "Decrements the reference count on the dynamic library handle `h`."
   __dlclose(h)
}

fn dlerror() str {
   "Returns a human-readable string describing the last error that occurred from an FFI/DL operation."
   __dlerror()
}

fn _try(str path, int flags) any {
   def h = __dlopen(path, flags)
   if h != 0 { return h }
   0
}

fn dlopen_checked(any name, any required_symbol, int flags=0) any {
   "Opens library `name` and verifies that `required_symbol` exists before returning the handle."
   mut eff_flags = flags
   if eff_flags == 0 { eff_flags = RTLD_NOW() | RTLD_GLOBAL() }
   def h = dlopen_any(name, eff_flags)
   if h == 0 { return 0 }
   if required_symbol && required_symbol.len > 0 && dlsym(h, required_symbol) == 0 {
      dlclose(h)
      return 0
   }
   h
}

fn dlopen_any(any name, int flags=0) any {
   "Attempts to open a dynamic library by searching for several platform-specific name variations and versions."
   if !is_str(name) || name.len == 0 { return 0 }
   if flags == 0 { flags = RTLD_NOW() | RTLD_GLOBAL() }
   def n = name.len
   if n >= 4 { if endswith(name, ".so") || endswith(name, ".dylib") || endswith(name, ".dll") { return _try(name, flags) } }
   def has_sep = ospath.has_sep(name)
   #windows {
      def h0 = _try(name, flags)
      if h0 { return h0 }
      def h1 = _try(name + ".dll", flags)
      if h1 { return h1 }
      if !has_sep {
         def h2 = _try("lib" + name + ".dll", flags)
         if h2 { return h2 }
      }
      return 0
   }
   #elif macos {
      def h0 = _try(name, flags)
      if h0 { return h0 }
      def h1 = _try(name + ".dylib", flags)
      if h1 { return h1 }
      if !has_sep {
         def h2 = _try("lib" + name + ".dylib", flags)
         if h2 { return h2 }
         def h3 = _try("lib" + name + ".0.dylib", flags)
         if h3 { return h3 }
      }
      return 0
   }
   #endif
   def h0 = _try(name, flags)
   if h0 { return h0 }
   def h1 = _try(name + ".so", flags)
   if h1 { return h1 }
   def h2 = _try(name + ".so.1", flags)
   if h2 { return h2 }
   if !has_sep {
      def h3 = _try("lib" + name + ".so", flags)
      if h3 { return h3 }
      def versions = ["0", "1", "2", "3", "8", "12", "14", "18"]
      mut i = 0
      while i < versions.len {
         def h4 = _try("lib" + name + ".so." + versions.get(i), flags)
         if h4 { return h4 }
         i += 1
      }
   }
   0
}

fn cstr(any s) ptr {
   "Ensures `s` is NUL-terminated for C APIs."
   mem.cstr(s)
}

fn cptr(any s) ptr {
   "Returns a C-compatible pointer view of `s` for typed FFI pointer parameters."
   cstr(s)
}

fn tag_native(any addr) any {
   "Tags a raw address as a native function pointer."
   __tag_native(addr)
}

fn call0(any f) any { "Low-level FFI call with 0 arguments." __call0(f) }

fn call0_ptr(any f) any { "Low-level FFI call with 0 arguments and pointer return." __call0_ptr(f) }

fn call1(any f, any a) any { "Low-level FFI call with 1 argument." __call1(f,a) }

fn call1_ptr(any f, any a) any { "Low-level FFI call with 1 argument and pointer return." __call1_ptr(f,a) }

fn call2(any f, any a, any b) any { "Low-level FFI call with 2 arguments." __call2(f,a,b) }

fn call2_ptr(any f, any a, any b) any { "Low-level FFI call with 2 arguments and pointer return." __call2_ptr(f,a,b) }

fn call2_ptr_u32(any f, any a, any b) any { "Low-level FFI call f(ptr,u32)->ptr." __call2_ptr_u32(f,a,b) }

fn call3(any f, any a, any b, any c) any { "Low-level FFI call with 3 arguments." __call3(f,a,b,c) }

fn call3_ptr(any f, any a, any b, any c) any { "Low-level FFI call with 3 arguments and pointer return." __call3_ptr(f,a,b,c) }

fn call3_ptr_u64_ptr(any f, any a, any b, any c) any { "Low-level FFI call f(ptr,u64,ptr)->ptr." __call3_ptr_u64_ptr(f,a,b,c) }

fn call3_ptr_u32_ptr(any f, any a, any b, any c) any { "Low-level FFI call f(ptr,u32,ptr)->ptr." __call3_ptr_u32_ptr(f,a,b,c) }

fn call3_ptr_ptr_u32(any f, any a, any b, any c) any { "Low-level FFI call f(ptr,ptr,u32)->ptr." __call3_ptr_ptr_u32(f,a,b,c) }

fn call4(any f, any a, any b, any c, any d) any { "Low-level FFI call with 4 arguments." __call4(f,a,b,c,d) }

fn call4_ptr(any f, any a, any b, any c, any d) any { "Low-level FFI call with 4 arguments and pointer return." __call4_ptr(f,a,b,c,d) }

fn call5(any f, any a, any b, any c, any d, any e) any { "Low-level FFI call with 5 arguments." __call5(f,a,b,c,d,e) }

fn call5_ptr(any f, any a, any b, any c, any d, any e) any { "Low-level FFI call with 5 arguments and pointer return." __call5_ptr(f,a,b,c,d,e) }

fn call6(any f, any a, any b, any c, any d, any e, any g) any { "Low-level FFI call with 6 arguments." __call6(f,a,b,c,d,e,g) }

fn call7(any f, any a, any b, any c, any d, any e, any g, any h) any { "Low-level FFI call with 7 arguments." __call7(f,a,b,c,d,e,g,h) }

fn call8(any f, any a, any b, any c, any d, any e, any g, any h, any i) any { "Low-level FFI call with 8 arguments." __call8(f,a,b,c,d,e,g,h,i) }

fn call9(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j) any { "Low-level FFI call with 9 arguments." __call9(f,a,b,c,d,e,g,h,i,j) }

fn call10(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j, any k) any { "Low-level FFI call with 10 arguments." __call10(f,a,b,c,d,e,g,h,i,j,k) }

fn call11(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j, any k, any l) any { "Low-level FFI call with 11 arguments." __call11(f,a,b,c,d,e,g,h,i,j,k,l) }

fn call12(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j, any k, any l, any m) any { "Low-level FFI call with 12 arguments." __call12(f,a,b,c,d,e,g,h,i,j,k,l,m) }

fn call13(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j, any k, any l, any m, any n) any { "Low-level FFI call with 13 arguments." __call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n) }

fn call14(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j, any k, any l, any m, any n, any o) any { "Low-level FFI call with 14 arguments." __call14(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o) }

fn call15(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j, any k, any l, any m, any n, any o, any p) any { "Low-level FFI call with 15 arguments." __call15(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o,p) }

fn call0_void(any f) any {
   "Calls `f` with no arguments and ignores any return value."
   __call0_void(f)
}

fn call0_i32(any f) int {
   "Calls `f` with no arguments and returns a 32-bit integer result."
   __call0_i32(f)
}

fn call1_void(any f, any a) any {
   "Calls `f` with one argument and ignores any return value."
   __call1_void(f,a)
}

fn call1_u32_void(any f, any a) any {
   "Calls `f` with one unsigned 32-bit argument and ignores any return value."
   __call1_u32_void(f,a)
}

fn call2_void(any f, any a, any b) any {
   "Calls `f` with two arguments and ignores any return value."
   __call2_void(f,a,b)
}

fn call3_void(any f, any a, any b, any c) any {
   "Calls `f` with three arguments and ignores any return value."
   __call3_void(f,a,b,c)
}

fn call4_void(any f, any a, any b, any c, any d) any {
   "Calls `f` with four arguments and ignores any return value."
   __call4_void(f,a,b,c,d)
}

fn call5_void(any f, any a, any b, any c, any d, any e) any {
   "Calls `f` with five arguments and ignores any return value."
   __call5_void(f,a,b,c,d,e)
}

fn call7_void(any f, any a, any b, any c, any d, any e, any g, any h) any {
   "Calls `f` with seven arguments and ignores any return value."
   __call7_void(f,a,b,c,d,e,g,h)
}

fn call9_void(any f, any a, any b, any c, any d, any e, any g, any h, any i, any j) any {
   "Calls `f` with nine arguments and ignores any return value."
   __call9_void(f,a,b,c,d,e,g,h,i,j)
}

fn call1_f32_void(any f, any a) any {
   "Calls `f` with one 32-bit float argument and ignores any return value."
   __call1_f32_void(f,a)
}

fn call2_f32_void(any f, any a, any b) any {
   "Calls `f` with two 32-bit float arguments and ignores any return value."
   __call2_f32_void(f,a,b)
}

fn call3_f32_void(any f, any a, any b, any c) any {
   "Calls `f` with three 32-bit float arguments and ignores any return value."
   __call3_f32_void(f,a,b,c)
}

fn call4_f32_void(any f, any a, any b, any c, any d) any {
   "Calls `f` with four 32-bit float arguments and ignores any return value."
   __call4_f32_void(f,a,b,c,d)
}

fn call4_ptr_ptr_ptr_ptr_void(any f, any a, any b, any c, any d) any {
   "Calls `f(ptr, ptr, ptr, ptr)` and ignores any return value."
   __call4_ptr_ptr_ptr_ptr_void(f,a,b,c,d)
}

fn call4_ptr_ptr_ptr_u64_i32(any f, any a, any b, any c, any d) any {
   "Calls `f` with three pointers and a 64-bit length, returning an i32 result."
   __call4_ptr_ptr_ptr_u64_i32(f,a,b,c,d)
}

fn call5_ptr_ptr_ptr_u64_i32_i32(any f, any a, any b, any c, any d, any e) any {
   "Calls `f` with three pointers, a 64-bit length, and a 32-bit level, returning an i32 result."
   __call5_ptr_ptr_ptr_u64_i32_i32(f,a,b,c,d,e)
}

fn call1_u32(any f, any a) any {
   "Calls `f` with one unsigned 32-bit argument and returns the raw result."
   __call1_u32(f,a)
}

fn call1_i64(any f, any a) any {
   "Calls `f` with one argument and returns a 64-bit integer result."
   __call1_i64(f,a)
}

fn ffi_call(any fptr, list args) any {
   "Dynamic FFI fallback: calls external function at `fptr` with `args` list. Supports 0-15 arguments; use extern/#include for native ABI calls and wider signatures."
   def n = args.len
   if n==0 { return call0(fptr)  }
   if n==1 { def a = args.get(0) return call1(fptr, a)  }
   if n==2 { def a = args.get(0) def b = args.get(1) return call2(fptr, a, b)  }
   if n==3 { def a = args.get(0) def b = args.get(1) def c = args.get(2) return call3(fptr, a, b, c)  }
   if n==4 { def a = args.get(0) def b = args.get(1) def c = args.get(2) def d = args.get(3) return call4(fptr, a, b, c, d)  }
   if n==5 { def a = args.get(0) def b = args.get(1) def c = args.get(2) def d = args.get(3) def e = args.get(4) return call5(fptr, a, b, c, d, e)  }
   if n==6 { def a = args.get(0) def b = args.get(1) def c = args.get(2) def d = args.get(3) def e = args.get(4) def f = args.get(5) return call6(fptr, a, b, c, d, e, f)  }
   if n==7 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) return call7(fptr,a,b,c,d,e,f,g) }
   if n==8 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) return call8(fptr,a,b,c,d,e,f,g,h) }
   if n==9 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) return call9(fptr,a,b,c,d,e,f,g,h,i) }
   if n==10 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) return call10(fptr,a,b,c,d,e,f,g,h,i,j) }
   if n==11 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) return call11(fptr,a,b,c,d,e,f,g,h,i,j,k) }
   if n==12 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) return call12(fptr,a,b,c,d,e,f,g,h,i,j,k,l) }
   if n==13 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) def m=args.get(12) return call13(fptr,a,b,c,d,e,f,g,h,i,j,k,l,m) }
   if n==14 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) def m=args.get(12) def o=args.get(13) return call14(fptr,a,b,c,d,e,f,g,h,i,j,k,l,m,o) }
   if n==15 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) def m=args.get(12) def o=args.get(13) def p=args.get(14) return call15(fptr,a,b,c,d,e,f,g,h,i,j,k,l,m,o,p) }
   panic("ffi_call supports 0-15 args")
   0
}

fn bind(any h, str name) any {
   "Returns a Nytrix function binding to external symbol `name` in library `h`."
   def fptr = dlsym(h, name)
   if fptr != 0 { return fn(...args) { ffi_call(fptr, args) } }
   0
}

fn call_ext(any h, str name, ...args) any {
   "Calls external symbol `name` in library `h` with provided arguments."
   def fptr = dlsym(h, name)
   if fptr != 0 { return ffi_call(fptr, args) }
   0
}

fn _bind_map(any h, list names, any target=0) dict {
   mut res = _d.dict(8)
   mut i, n = 0, names.len
   while i < n {
      def name = names.get(i)
      def b = bind(h, name)
      if b != 0 {
         res = res.set(name, b)
         if target != 0 { target.set(name, b) }
      }
      i += 1
   }
   res
}

fn bind_all(any h, list names) dict {
   "Returns a dictionary of callable wrappers for each resolvable symbol in `names`."
   _bind_map(h, names)
}

fn bind_linked(list names) dict {
   "Binds `names` from the current process image."
   bind_all(0, names)
}

fn import_all(any h, list names) bool {
   "Imports each resolvable symbol in `names` into the current global scope."
   _bind_map(h, names, __globals())
   true
}

fn import_linked(list names) bool {
   "Imports `names` from the current process image into the global scope."
   import_all(0, names)
}

fn extern_all() int {
   "Placeholder for compatibility with import-style APIs."
   0
}

fn malloc(int n) ptr {
   "Allocates `n` bytes and returns a raw pointer, or `0` for non-positive sizes."
   if n<=0 {return 0}
   __malloc(n)
}

fn free(any p) int {
   "Frees raw pointer `p` when it is non-zero."
   if p { __free(p) }
   0
}

comptime template _ffi_ctype(name, tag, size, align){
   fn name() list {
      "Returns an FFI scalar type descriptor."
      [tag, size, align]
   }
}

comptime emit _ffi_ctype(u8,  "u8",  1, 1)
comptime emit _ffi_ctype(i8,  "i8",  1, 1)
comptime emit _ffi_ctype(u16, "u16", 2, 2)
comptime emit _ffi_ctype(i16, "i16", 2, 2)
comptime emit _ffi_ctype(u32, "u32", 4, 4)
comptime emit _ffi_ctype(i32, "i32", 4, 4)
comptime emit _ffi_ctype(u64, "u64", 8, 8)
comptime emit _ffi_ctype(i64, "i64", 8, 8)
comptime emit _ffi_ctype(f32, "f32", 4, 4)
comptime emit _ffi_ctype(f64, "f64", 8, 8)
comptime emit _ffi_ctype(ptr, "ptr", 8, 8)

fn CType(any tag, int size, int align=0) list {
   "Returns an FFI scalar type descriptor `[tag, size, align]`."
   if align <= 0 { align = size }
   [tag, size, align]
}

fn handle() list {
   "Returns the canonical pointer-like FFI type descriptor."
   ptr()
}

fn _align(int offset, int alignment) int {
   def rem = offset % alignment
   if rem == 0 { return offset }
   offset + (alignment - rem)
}

fn CStruct(list fields) dict {
   "Defines a dynamic C-style structure descriptor. Good for REPL/probes; use `layout` for compiled ABI field access."
   mut d_lyt = dict(8)
   mut curr_off = 0
   mut m_align = 1
   mut i = 0
   while i < fields.len {
      def fld = fields.get(i)
      def typ = fld.get(0)
      def nm = fld.get(1)
      def sz = typ.get(1)
      def al = typ.get(2)
      curr_off = _align(curr_off, al)
      mut info = dict(8)
      info = info.set("offset", curr_off)
      info = info.set("type", typ.get(0))
      info = info.set("size", sz)
      d_lyt = d_lyt.set(nm, info)
      if al > m_align { m_align = al }
      curr_off += sz
      i += 1
   }
   def t_size = _align(curr_off, m_align)
   mut sd = dict(8)
   sd = sd.set("fields", d_lyt)
   sd = sd.set("size", t_size)
   sd = sd.set("align", m_align)
   sd
}

fn sizeof_struct(any d) int {
   "Returns the size in bytes for struct descriptor `d`."
   if is_list(d) { return d.get(1) }
   d.get("size", 0)
}

fn offsetof_struct(any d, any name) int {
   "Returns the byte offset of field `name` within struct descriptor `d`."
   def fs = d.get("fields", 0)
   def info = fs.get(name, 0)
   info.get("offset", -1)
}

fn _cstruct_storage_kind(any typ) int {
   if !is_str(typ) { return 0 }
   case typ {
      "u32", "i32" -> 32
      "u64", "i64", "ptr" -> 64
      "f32" -> 33
      "u8", "i8" -> 8
      "u16", "i16" -> 16
      _ -> 0
   }
}

fn cstruct_set(any p, any d, any name, any val) bool {
   "Sets field `name` through a dynamic CStruct descriptor."
   def fs = d.get("fields", 0)
   def info = fs.get(name, 0)
   if !info { return false }
   def off = info.get("offset")
   def kind = _cstruct_storage_kind(info.get("type"))
   if kind == 32 { store32(p, val, off) }
   elif kind == 64 { store64_h(p, val, off) }
   elif kind == 33 { store32_f32(p, val, off) }
   elif kind == 8 { store8(p, val, off) }
   elif kind == 16 { store16(p, val, off) }
   true
}

fn cstruct_get(any p, any d, any name) any {
   "Gets field `name` through a dynamic CStruct descriptor."
   def fs = d.get("fields", 0)
   def info = fs.get(name, 0)
   if !info { return 0 }
   def off = info.get("offset")
   def kind = _cstruct_storage_kind(info.get("type"))
   case kind {
      32 -> load32(p, off)
      64 -> load64_h(p, off)
      33 -> load32_f32(p, off)
      8 -> load8(p, off)
      16 -> load16(p, off)
      _ -> 0
   }
}

fn bind_lib(any name_or_h, list syms) any {
   "Binds a list of symbols from a library(by name or handle) into a dictionary."
   mut lib = 0
   if is_str(name_or_h) { lib = dlopen_any(name_or_h, RTLD_NOW()) }
   else { lib = name_or_h }
   if !lib { return 0 }
   mut res = _d.dict(8)
   mut i = 0
   while i < syms.len {
      def s = syms.get(i)
      mut nm, arity = "", -1
      if is_list(s) { nm = s.get(0) arity = s.get(1) }
      else { nm = s }
      def fptr = dlsym(lib, nm)
      if fptr {
         if arity == 0 { res = res.set(nm, fn() { call0(fptr) }) }
         elif arity == 1 { res = res.set(nm, fn(a) { call1(fptr, a) }) }
         elif arity == 2 { res = res.set(nm, fn(a, b) { call2(fptr, a, b) }) }
         elif arity == 3 { res = res.set(nm, fn(a, b, c) { call3(fptr, a, b, c) }) }
         elif arity == 4 { res = res.set(nm, fn(a, b, c, d) { call4(fptr, a, b, c, d) }) }
         elif arity == 5 { res = res.set(nm, fn(a, b, c, d, e) { call5(fptr, a, b, c, d, e) }) }
         elif arity == 6 { res = res.set(nm, fn(a, b, c, d, e, f) { call6(fptr, a, b, c, d, e, f) }) }
         else {
            res = res.set(nm, fn(...args) { ffi_call(fptr, args) })
         }
      }
      i += 1
   }
   res
}

#main {
   assert(RTLD_NOW() == 2 && RTLD_GLOBAL() == 256, "ffi dlopen constants")
   def u32_desc, custom = CType("u32", 4, 4), CType("word", 4)
   assert(u32_desc.get(0) == "u32" && u32_desc.get(1) == 4, "ffi u32 descriptor")
   assert(custom.get(0) == "word" && custom.get(2) == 4, "ffi CType alignment")
   def t32, t16, t8 = CType("u32", 4, 4), CType("u16", 2, 2), CType("u8", 1, 1)
   def lyt = CStruct([[t32, "a"], [t16, "b"], [t8, "c"]])
   assert(offsetof_struct(lyt, "a") == 0 && offsetof_struct(lyt, "b") == 4 && offsetof_struct(lyt, "c") == 6, "ffi struct offsets")
   assert(sizeof_struct(lyt) >= 8, "ffi struct size")
   def p = malloc(sizeof_struct(lyt))
   assert(p != 0, "ffi malloc")
   assert(cstruct_set(p, lyt, "a", 0x11223344) && cstruct_set(p, lyt, "b", 0x5566) && cstruct_set(p, lyt, "c", 0x77), "ffi struct stores")
   assert(cstruct_get(p, lyt, "a") == 0x11223344 && cstruct_get(p, lyt, "b") == 0x5566 && cstruct_get(p, lyt, "c") == 0x77, "ffi struct loads")
   free(p)
   assert(dlopen_any("__nytrix_missing_library_for_test__", RTLD_NOW()) == 0, "ffi missing dlopen")
   ;; dlsym z3 probe
   def h = dlopen_checked("z3", "Z3_mk_config", RTLD_NOW() | RTLD_GLOBAL())
   assert(h != 0, "dlopen z3")
   def p = dlsym(h, "Z3_get_version")
   print("Z3_get_version ptr = " + to_str(p))
   def p2 = dlsym(h, "Z3_mk_bv_sort")
   print("Z3_mk_bv_sort ptr = " + to_str(p2))
   def a = malloc(4)
   def b = malloc(4)
   def c = malloc(4)
   def d = malloc(4)
   store32(a,0,0) store32(b,0,0) store32(c,0,0) store32(d,0,0)
   call4_ptr_ptr_ptr_ptr_void(p, a, b, c, d)
   print("ver = " + to_str(load32(a,0)) + "." + to_str(load32(b,0)) + "." + to_str(load32(c,0)) + "." + to_str(load32(d,0)))
   free(a) free(b) free(c) free(d)
   print("✓ std.os.ffi self-test passed")
}
