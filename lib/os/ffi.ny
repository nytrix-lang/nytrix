;; Keywords: ffi foreign-function-interface interop os
;; Foreign-function interface, dynamic loading, C layouts, and header bindings.
;; Low-level C ABI call variants are exported alongside the FFI helpers.
;; References:
;; - core.os
module core.os.ffi(generate, gen_file, RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL,
   dlopen, dlopen_any, dlopen_checked, dlsym, dlclose, dlerror,
   ffi_call, bind, call_ext, bind_all, bind_linked, import_all, import_linked, extern_all,
   CStruct, CType, cstr, cptr, u8, i8, u16, i16, u32, i32, u64, i64,
   f32, f64, ptr, handle, sizeof_struct, offsetof_struct, malloc, free,
   cstruct_set, cstruct_get, bind_lib, tag_native,
   any0, any1, any2, any3, any4, any5, any6, any7, any8, any9, any10, any11, any12, any13, any14, any15,
   pointer0, pointer1, pointer2, pointer3, pointer4, pointer5,
   void0, void1, void2, void3, void4, void5, void6, void7, void8, void9, void10,
   int32zero, int64one, uint32one, uint32voidone,
   float32voidone, float32voidtwo, float32voidthree, float32voidfour,
   pointer2uint32, pointer3uint64pointer, pointer3uint32pointer, pointer3pointeruint32,
   pointer4pointerpointerpointervoid, pointer4pointerpointerpointerint32, pointer4pointerpointeruint64int32,
   pointer4uint32uint64pointerint32, pointer4uint64pointerpointerint32, pointer3uint64pointerint32,
   pointer5pointerpointeruint64int32int32)
use core
use core.dict_mod as _d
use core.os.prim
use core.os.path as ospath
use core.str as str
use core.os (file_read, file_write)

fn any0(any f) any { __call0(f) }

fn any1(any f, any a0) any { __call1(f, a0) }

fn any2(any f, any a0, any a1) any { __call2(f, a0, a1) }

fn any3(any f, any a0, any a1, any a2) any { __call3(f, a0, a1, a2) }

fn any4(any f, any a0, any a1, any a2, any a3) any { __call4(f, a0, a1, a2, a3) }

fn any5(any f, any a0, any a1, any a2, any a3, any a4) any { __call5(f, a0, a1, a2, a3, a4) }

fn any6(any f, any a0, any a1, any a2, any a3, any a4, any a5) any { __call6(f, a0, a1, a2, a3, a4, a5) }

fn any7(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6) any { __call7(f, a0, a1, a2, a3, a4, a5, a6) }

fn any8(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7) any { __call8(f, a0, a1, a2, a3, a4, a5, a6, a7) }

fn any9(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8) any { __call9(f, a0, a1, a2, a3, a4, a5, a6, a7, a8) }

fn any10(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9) any { __call10(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) }

fn any11(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9, any a10) any { __call11(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) }

fn any12(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9, any a10, any a11) any { __call12(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) }

fn any13(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9, any a10, any a11, any a12) any { __call13(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) }

fn any14(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9, any a10, any a11, any a12, any a13) any { __call14(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) }

fn any15(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9, any a10, any a11, any a12, any a13, any a14) any { __call15(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) }

fn pointer0(any f) any { __call0_ptr(f) }

fn pointer1(any f, any a0) any { __call1_ptr(f, a0) }

fn pointer2(any f, any a0, any a1) any { __call2_ptr(f, a0, a1) }

fn pointer3(any f, any a0, any a1, any a2) any { __call3_ptr(f, a0, a1, a2) }

fn pointer4(any f, any a0, any a1, any a2, any a3) any { __call4_ptr(f, a0, a1, a2, a3) }

fn pointer5(any f, any a0, any a1, any a2, any a3, any a4) any { __call5_ptr(f, a0, a1, a2, a3, a4) }

fn void0(any f) any { __call0_void(f) }

fn void1(any f, any a0) any { __call1_void(f, a0) }

fn void2(any f, any a0, any a1) any { __call2_void(f, a0, a1) }

fn void3(any f, any a0, any a1, any a2) any { __call3_void(f, a0, a1, a2) }

fn void4(any f, any a0, any a1, any a2, any a3) any { __call4_void(f, a0, a1, a2, a3) }

fn void5(any f, any a0, any a1, any a2, any a3, any a4) any { __call5_void(f, a0, a1, a2, a3, a4) }

fn void6(any f, any a0, any a1, any a2, any a3, any a4, any a5) any { __call6_void(f, a0, a1, a2, a3, a4, a5) }

fn void7(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6) any { __call7_void(f, a0, a1, a2, a3, a4, a5, a6) }

fn void8(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7) any { __call8_void(f, a0, a1, a2, a3, a4, a5, a6, a7) }

fn void9(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8) any { __call9_void(f, a0, a1, a2, a3, a4, a5, a6, a7, a8) }

fn void10(any f, any a0, any a1, any a2, any a3, any a4, any a5, any a6, any a7, any a8, any a9) any { __call10_void(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) }

fn int32zero(any f) int { __call0_i32(f) }

fn int64one(any f, any a) any { __call1_i64(f, a) }

fn uint32one(any f, any a) any { __call1_u32(f, a) }

fn uint32voidone(any f, any a) any { __call1_u32_void(f, a) }

fn float32voidone(any f, any a) any { __call1_f32_void(f, a) }

fn float32voidtwo(any f, any a, any b) any { __call2_f32_void(f, a, b) }

fn float32voidthree(any f, any a, any b, any c) any { __call3_f32_void(f, a, b, c) }

fn float32voidfour(any f, any a, any b, any c, any d) any { __call4_f32_void(f, a, b, c, d) }

fn pointer2uint32(any f, any a, any b) any { __call2_ptr_u32(f, a, b) }

fn pointer3uint64pointer(any f, any a, any b, any c) any { __call3_ptr_u64_ptr(f, a, b, c) }

fn pointer3uint32pointer(any f, any a, any b, any c) any { __call3_ptr_u32_ptr(f, a, b, c) }

fn pointer3pointeruint32(any f, any a, any b, any c) any { __call3_ptr_ptr_u32(f, a, b, c) }

fn pointer4pointerpointerpointervoid(any f, any a, any b, any c, any d) any { __call4_ptr_ptr_ptr_ptr_void(f, a, b, c, d) }

fn pointer4pointerpointeruint64int32(any f, any a, any b, any c, any d) any { __call4_ptr_ptr_ptr_u64_i32(f, a, b, c, d) }

fn pointer5pointerpointeruint64int32int32(any f, any a, any b, any c, any d, any e) any { __call5_ptr_ptr_ptr_u64_i32_i32(f, a, b, c, d, e) }

fn pointer3uint64pointerint32(any f, any a, any b, any c) int { __call3_ptr_u64_ptr_i32(f, a, b, c) }

fn pointer4pointerpointerpointerint32(any f, any a, any b, any c, any d) int { __call4_ptr_ptr_ptr_ptr_i32(f, a, b, c, d) }

fn pointer4uint32uint64pointerint32(any f, any a, any b, any c, any d) int { __call4_ptr_u32_u64_ptr_i32(f, a, b, c, d) }

fn pointer4uint64pointerpointerint32(any f, any a, any b, any c, any d) int { __call4_ptr_u64_ptr_ptr_i32(f, a, b, c, d) }

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
   s + ""
}

fn cptr(any s) ptr {
   "Returns a C-compatible pointer view of `s` for typed FFI pointer parameters."
   cstr(s)
}

fn tag_native(any addr) any {
   "Tags a raw address as a native function pointer."
   __tag_native(addr)
}

fn ffi_call(any fptr, list args) any {
   "Dynamic FFI fallback: calls external function at `fptr` with `args` list. Supports 0-15 arguments; use extern/#include for native ABI calls and wider signatures."
   def n = args.len
   if n==0 { return any0(fptr)  }
   if n==1 { def a = args.get(0) return any1(fptr, a)  }
   if n==2 { def a = args.get(0) def b = args.get(1) return any2(fptr, a, b)  }
   if n==3 { def a = args.get(0) def b = args.get(1) def c = args.get(2) return any3(fptr, a, b, c)  }
   if n==4 { def a = args.get(0) def b = args.get(1) def c = args.get(2) def d = args.get(3) return any4(fptr, a, b, c, d)  }
   if n==5 { def a = args.get(0) def b = args.get(1) def c = args.get(2) def d = args.get(3) def e = args.get(4) return any5(fptr, a, b, c, d, e)  }
   if n==6 { def a = args.get(0) def b = args.get(1) def c = args.get(2) def d = args.get(3) def e = args.get(4) def f = args.get(5) return any6(fptr, a, b, c, d, e, f)  }
   if n==7 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) return any7(fptr,a,b,c,d,e,f,g) }
   if n==8 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) return any8(fptr,a,b,c,d,e,f,g,h) }
   if n==9 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) return any9(fptr,a,b,c,d,e,f,g,h,i) }
   if n==10 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) return any10(fptr,a,b,c,d,e,f,g,h,i,j) }
   if n==11 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) return any11(fptr,a,b,c,d,e,f,g,h,i,j,k) }
   if n==12 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) return any12(fptr,a,b,c,d,e,f,g,h,i,j,k,l) }
   if n==13 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) def m=args.get(12) return any13(fptr,a,b,c,d,e,f,g,h,i,j,k,l,m) }
   if n==14 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) def m=args.get(12) def o=args.get(13) return any14(fptr,a,b,c,d,e,f,g,h,i,j,k,l,m,o) }
   if n==15 { def a=args.get(0) def b=args.get(1) def c=args.get(2) def d=args.get(3) def e=args.get(4) def f=args.get(5) def g=args.get(6) def h=args.get(7) def i=args.get(8) def j=args.get(9) def k=args.get(10) def l=args.get(11) def m=args.get(12) def o=args.get(13) def p=args.get(14) return any15(fptr,a,b,c,d,e,f,g,h,i,j,k,l,m,o,p) }
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
         if arity == 0 { res = res.set(nm, fn() { any0(fptr) }) }
         elif arity == 1 { res = res.set(nm, fn(a) { any1(fptr, a) }) }
         elif arity == 2 { res = res.set(nm, fn(a, b) { any2(fptr, a, b) }) }
         elif arity == 3 { res = res.set(nm, fn(a, b, c) { any3(fptr, a, b, c) }) }
         elif arity == 4 { res = res.set(nm, fn(a, b, c, d) { any4(fptr, a, b, c, d) }) }
         elif arity == 5 { res = res.set(nm, fn(a, b, c, d, e) { any5(fptr, a, b, c, d, e) }) }
         elif arity == 6 { res = res.set(nm, fn(a, b, c, d, e, f) { any6(fptr, a, b, c, d, e, f) }) }
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
   pointer4pointerpointerpointervoid(p, a, b, c, d)
   print("ver = " + to_str(load32(a,0)) + "." + to_str(load32(b,0)) + "." + to_str(load32(c,0)) + "." + to_str(load32(d,0)))
   free(a) free(b) free(c) free(d)
   print("✓ core.os.ffi self-test passed")
}

;; C header binding generator. Keeps parser helpers private to this module.
fn _ffi_gen_input_max_bytes() int { 1 << 20 }

;; C scalar type name -> byte size text (LP64 widths).  Empty for names
;; outside the supported subset.
fn _ffi_gen_scalar_size(str name) str {
   if name == "long" { return "8" }
   if name == "int" { return "4" }
   if name == "short" { return "2" }
   if name == "char" { return "1" }
   if name == "double" { return "8" }
   if name == "float" { return "4" }
   ""
}

;; C scalar type name -> core.os.ffi CType tag.
fn _ffi_gen_scalar_tag(str name) str {
   if name == "long" { return "i64" }
   if name == "int" { return "i32" }
   if name == "short" { return "i16" }
   if name == "char" { return "i8" }
   if name == "double" { return "f64" }
   if name == "float" { return "f32" }
   ""
}

fn _ffi_gen_strip_comments_and_directives(str source) str {
   mut out = ""
   mut i = 0
   mut start = 0
   while i < source.len {
      def int byte = load8(source, i)
      def int next = i + 1 < source.len ? load8(source, i + 1) : 0
      if byte == 47 && next == 42 {
         out += str.str_slice(source, start, i) + " "
         i += 2
         while i + 1 < source.len &&
         !(load8(source, i) == 42 && load8(source, i + 1) == 47) {
            i += 1
         }
         if i + 1 < source.len { i += 2 }
         start = i
         continue
      }
      if byte == 47 && next == 47 {
         out += str.str_slice(source, start, i) + " "
         i += 2
         while i < source.len && load8(source, i) != 10 { i += 1 }
         start = i
         continue
      }
      if byte == 35 {
         out += str.str_slice(source, start, i)
         while i < source.len && load8(source, i) != 10 { i += 1 }
         start = i
         continue
      }
      i += 1
   }
   out += str.str_slice(source, start, source.len)
   out
}

;; C declarations in the supported subset never contain a vertical bar.
fn _ffi_gen_sep() str { "|" }

;; Piece `index` of a `|`-separated encoded value (see _ffi_gen_parse_type).
fn _ffi_gen_tok(str joined, int index) str { _ffi_gen_piece(joined, index) }

fn _ffi_gen_tok_count(str joined) int { _ffi_gen_count_pieces(joined) }

;; Piece `index` of a `|`-separated record string.
fn _ffi_gen_piece(str record, int index) str {
   mut start = 0
   mut seen = 0
   mut i = 0
   while i <= record.len {
      if i == record.len || load8(record, i) == 124 {
         if seen == index { return str.str_slice(record, start, i) }
         seen += 1
         start = i + 1
      }
      i += 1
   }
   ""
}

fn _ffi_gen_count_pieces(str record) int {
   if record.len == 0 { return 0 }
   mut seen = 1
   mut i = 0
   while i < record.len {
      if load8(record, i) == 124 { seen += 1 }
      i += 1
   }
   seen
}

;; Split on top-level `;`, keeping brace-balanced typedef bodies together.
fn _ffi_gen_split_declarations(str text) str {
   mut out = ""
   mut start = 0
   mut depth = 0
   mut i = 0
   while i < text.len {
      def int byte = load8(text, i)
      if byte == 123 {
         depth += 1
      }
      if byte == 125 {
         depth -= 1
      }
      if byte == 59 && depth == 0 {
         def declaration = str.strip(str.str_slice(text, start, i))
         if declaration.len > 0 {
            if out.len > 0 {
               out += _ffi_gen_sep()
            }
            out += declaration
         }
         start = i + 1
      }
      i += 1
   }
   def declaration = str.strip(str.str_slice(text, start, text.len))
   if declaration.len > 0 {
      if out.len > 0 {
         out += _ffi_gen_sep()
      }
      out += declaration
   }
   out
}

fn _ffi_gen_word_byte(int byte) bool {
   (byte >= 97 && byte <= 122) || (byte >= 65 && byte <= 90) ||
   (byte >= 48 && byte <= 57) || byte == 95
}

fn _ffi_gen_tokens(str decl) str {
   mut out = ""
   mut word_start = -1
   mut i = 0
   while i < decl.len {
      def int byte = load8(decl, i)
      if _ffi_gen_word_byte(byte) {
         if word_start < 0 { word_start = i }
      } else {
         if word_start >= 0 {
            if out.len > 0 {
               out += _ffi_gen_sep()
            }
            out += str.str_slice(decl, word_start, i)
            word_start = -1
         }
         if byte != 32 && byte != 9 && byte != 10 && byte != 13 {
            if out.len > 0 {
               out += _ffi_gen_sep()
            }
            out += str.str_slice(decl, i, i + 1)
         }
      }
      i += 1
   }
   if word_start >= 0 {
      if out.len > 0 {
         out += _ffi_gen_sep()
      }
      out += str.str_slice(decl, word_start, decl.len)
   }
   out
}

;; Parse one scalar or pointer declarator from `toks[at]`; the encoded
;; result is `next_index|ctype_tag|size|base_type`.
;; Identifier predicate on the first byte: string relational compares do
;; not survive module lowering, so classification always uses bytes.
fn _ffi_gen_is_ident_start(str name) bool {
   if name.len == 0 { return false }
   def int b = load8(name, 0)
   _ffi_gen_word_byte(b) && !(b >= 48 && b <= 57)
}

;; Extracts the `index`-th `|`-separated field without allocating per byte.
fn _ffi_gen_part(str encoded, int index) str {
   mut start = 0
   mut seen = 0
   mut i = 0
   while i <= encoded.len {
      if i == encoded.len || load8(encoded, i) == 124 {
         if seen == index { return str.str_slice(encoded, start, i) }
         seen += 1
         start = i + 1
      }
      i += 1
   }
   ""
}

fn _ffi_gen_parse_type(str toks, int at) Result<str, str> {
   mut i = at
   if _ffi_gen_tok(toks, i) == "const" { i += 1 }
   def base = _ffi_gen_tok(toks, i)
   def size_text = _ffi_gen_scalar_size(base)
   if size_text.len == 0 { return err("unsupported base type '" + base + "'") }
   i += 1
   mut pointer = false
   while _ffi_gen_tok(toks, i) == "*" {
      pointer = true
      i += 1
   }
   def tag = "i64"
   if pointer {
      tag = "ptr"
      i += 1
      return ok(str(i) + "|ptr|8|" + base)
   }
   ok(str(i) + "|" + _ffi_gen_scalar_tag(base) + "|" + size_text + "|" + base)
}

fn _ffi_gen_emit_field(str toks, int at) Result<str, str> {
   mut out = ""
   match _ffi_gen_parse_type(toks, at) {
      err(reason) -> { return err(reason) }
      ok(encoded) -> {
         def next_index = str.atoi(_ffi_gen_part(encoded, 0))
         def tag = _ffi_gen_part(encoded, 1)
         def size = _ffi_gen_part(encoded, 2)
         def name = _ffi_gen_tok(toks, next_index)
         if !_ffi_gen_is_ident_start(name) {
            return err("field is missing a name")
         }
         out = "[ffi.CType(\"" + tag + "\", " + size + "), \"" + name + "\"]"
      }
   }
   ok(out)
}

fn _ffi_gen_gen_struct(str decl, str out_name) Result<str, str> {
   def open = str.find(decl, "{")
   def close = str.find_last(decl, "}")
   if open < 0 || close <= open { return err("struct body braces are missing") }
   def body = str.str_slice(decl, open + 1, close)
   def fields_text = _ffi_gen_split_declarations(body)
   mut fields = ""
   mut field_count = 0
   mut skipped = ""
   def total = _ffi_gen_count_pieces(fields_text)
   mut index = 0
   while index < total {
      def toks = _ffi_gen_tokens(_ffi_gen_piece(fields_text, index))
      match _ffi_gen_emit_field(toks, 0) {
         ok(field) -> {
            if field_count > 0 {
               fields += ", "
            }
            fields += field
            field_count += 1
         }
         err(reason) -> {
            skipped += ";; ffi.gen: unsupported: field in " + out_name +
            " skipped(" + reason + ")\n"
         }
      }
      index += 1
   }
   if field_count == 0 {
      return err("struct " + out_name + " has no supported fields")
   }
   ok(skipped + "def " + out_name + " = ffi.CStruct([" + fields + "])")
}

;; Split a parameter list on top-level commas (encoded record string).
fn _ffi_gen_split_params(str text) str {
   mut out = ""
   mut start = 0
   mut depth = 0
   mut i = 0
   while i < text.len {
      def int byte = load8(text, i)
      if byte == 40 {
         depth += 1
      }
      if byte == 41 {
         depth -= 1
      }
      if byte == 44 && depth == 0 {
         def parameter = str.strip(str.str_slice(text, start, i))
         if parameter.len > 0 {
            if out.len > 0 {
               out += _ffi_gen_sep()
            }
            out += parameter
         }
         start = i + 1
      }
      i += 1
   }
   def parameter = str.strip(str.str_slice(text, start, text.len))
   if parameter.len > 0 {
      if out.len > 0 {
         out += _ffi_gen_sep()
      }
      out += parameter
   }
   out
}

fn _ffi_gen_gen_extern(str decl) Result<str, str> {
   def open = str.find(decl, "(")
   def close = str.find_last(decl, ")")
   if open < 0 || close <= open { return err("prototype parentheses missing") }
   def head_toks = _ffi_gen_tokens(str.strip(str.str_slice(decl, 0, open)))
   mut idx = 0
   if _ffi_gen_tok(head_toks, 0) == "extern" { idx = 1 }
   ;; The prototype name is the last head token; everything before it is
   ;; the (possibly const-qualified) return type.
   def name = _ffi_gen_tok(head_toks, _ffi_gen_tok_count(head_toks) - 1)
   if name.len == 0 { return err("prototype is missing a name") }
   ;; `void` (and a bare `extern name(...)`) omits the generated return tag.
   def ret_tag = "i64"
   if _ffi_gen_tok(head_toks, idx) == "void" {
      ret_tag = ""
   } else if _ffi_gen_tok_count(head_toks) - 1 > idx {
      match _ffi_gen_parse_type(head_toks, idx) {
         err(reason) -> { return err("return type: " + reason) }
         ok(encoded) -> {
            ret_tag = _ffi_gen_part(encoded, 1)
         }
      }
   } else {
      ret_tag = ""
   }
   def params_text = _ffi_gen_split_params(str.str_slice(decl, open + 1, close))
   mut params = ""
   mut param_count = 0
   def total = _ffi_gen_count_pieces(params_text)
   mut index = 0
   while index < total {
      def raw = _ffi_gen_piece(params_text, index)
      def toks = _ffi_gen_tokens(raw)
      if _ffi_gen_tok_count(toks) == 1 && _ffi_gen_tok(toks, 0) == "void" {
         index += 1
         continue
      }
      match _ffi_gen_parse_type(toks, 0) {
         err(reason) -> {
            return err("parameter '" + raw + "': " + reason)
         }
         ok(encoded) -> {
            def pname = _ffi_gen_tok(toks, str.atoi(_ffi_gen_part(encoded, 0)))
            if pname.len == 0 { pname = "_" }
            if param_count > 0 {
               params += ", "
            }
            params += _ffi_gen_part(encoded, 1) + " " + pname
            param_count += 1
         }
      }
      index += 1
   }
   mut out = "extern {\n  fn " + name + "(" + params + ")"
   if ret_tag.len > 0 {
      out += " " + ret_tag
   }
   out += "\n}"
   ok(out)
}

fn generate(str header) Result<str, str> {
   "Generates Ny FFI declarations from C header `header` text; returns
   `ok(generated_text)` or `err(reason)`."
   if header.len > _ffi_gen_input_max_bytes() {
      return err("header exceeds the 1 MiB ffi.gen budget")
   }
   def text = _ffi_gen_strip_comments_and_directives(header)
   def decls = _ffi_gen_split_declarations(text)
   mut lines = ""
   mut line_count = 0
   def total = _ffi_gen_count_pieces(decls)
   mut index = 0
   while index < total {
      def decl = str.strip(_ffi_gen_piece(decls, index))
      def toks = _ffi_gen_tokens(decl)
      def first = _ffi_gen_tok(toks, 0)
      def second = _ffi_gen_tok(toks, 1)
      mut produced = ""
      if first == "typedef" && second == "struct" {
         def name = _ffi_gen_tok(toks, _ffi_gen_tok_count(toks) - 1)
         match _ffi_gen_gen_struct(decl, name) {
            ok(text_ok) -> { produced = text_ok }
            err(reason) -> {
               produced = ";; ffi.gen: unsupported: " + reason
            }
         }
      } else if first == "union" || second == "union" {
         produced = ";; ffi.gen: unsupported: union layout"
      } else if first == "enum" || second == "enum" {
         produced = ";; ffi.gen: unsupported: enum constants"
      } else if first == "extern" || first == "long" || first == "int" ||
      first == "double" || first == "float" || first == "short" ||
      first == "char" ||
      (first == "const" && second != "struct") {
         match _ffi_gen_gen_extern(decl) {
            ok(text_ok) -> { produced = text_ok }
            err(reason) -> {
               produced = ";; ffi.gen: unsupported: " + reason
            }
         }
      } else {
         produced = ";; ffi.gen: unsupported: declaration starts with '" +
         first + "'"
      }
      if produced.len > 0 {
         if line_count > 0 {
            lines += "\n"
         }
         lines += produced
         line_count += 1
      }
      index += 1
   }
   if line_count == 0 { return err("header contained no declarations") }
   ok(lines + "\n")
}

fn gen_file(str in_path, str out_path) Result<int, str> {
   "Reads the C header at `in_path`, runs `generate`, and writes the Ny
   declarations to `out_path` ; returns `ok(bytes_written)` or `err(reason)`."
   match file_read(in_path) {
      ok(header) -> {
         match generate(header) {
            ok(text) -> {
               match file_write(out_path, text) {
                  ok(count) -> { ok(int(count)) }
                  err(code) -> { err("write failed: " + str(code)) }
               }
            }
            err(reason) -> { err(reason) }
         }
      }
      err(code) -> { err("read failed: " + str(code)) }
   }
}
