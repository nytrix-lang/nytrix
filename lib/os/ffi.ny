;; Keywords: os ffi
;; Os Ffi module.

module std.os.ffi (
   RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL, dlopen, dlopen_any, dlsym, dlclose, dlerror,
   call0_void, call1_void, call2_void, call3_void, call4_f32_void, call0, call1, call1_i64, call2, call3, call4, call5,
   call6, call7, call8, call9, call10, call11, call12, call13, call14, call15, ffi_call,
   bind, call_ext, bind_all, bind_linked, import_all, import_linked, extern_all,

   ;; New C-FFI Helpers
   CStruct, CType,
   u8, i8, u16, i16, u32, i32, u64, i64, f32, f64, ptr, handle,
   sizeof_struct, offsetof_struct, malloc, free,
   set, get, bind_lib
)

use std.core as core
use std.core.dict as _d
use std.text *
use std.os *
use std.os.path as ospath

;; Dynamic Loading

fn RTLD_LAZY() {
   "Returns the flag for lazy symbol binding."
   1
}

fn RTLD_NOW() {
   "Returns the flag for immediate symbol binding."
   2
}

fn RTLD_GLOBAL() {
   "Returns the flag to make symbols available to subsequently loaded libraries."
   256
}

fn RTLD_LOCAL() {
   "Returns the flag to keep symbols private to the library."
   0
}

fn dlopen(path, flags) {
   "Opens a dynamic library file."
   __dlopen(path, flags)
}

fn dlsym(h, s) {
   "Retrieves the address of a symbol from a loaded library."
   __dlsym(h, s)
}

fn dlclose(h) {
   "Closes a previously opened dynamic library."
   __dlclose(h)
}

fn dlerror() {
   "Returns a string describing the last error that occurred during dynamic loading."
   __dlerror()
}

fn _try(path, flags){
   "Internal helper to attempt opening a library at a specific path."
   def h = __dlopen(path, flags)
   if(h != 0){ return h }
   0
}

fn dlopen_any(name, flags=0){
   "Attempts to open a dynamic library by searching for several platform-specific name variations."
   if(!core.is_str(name) || str_len(name) == 0){ return 0 }
   def n = str_len(name)
   if(n >= 4){
      if(endswith(name, ".so") || endswith(name, ".dylib") || endswith(name, ".dll")){ return _try(name, flags) }
   }
   def has_sep = ospath.has_sep(name)
   def osn = __os_name()
   if(osn == "windows"){
      def h0 = _try(name, flags)
      if(h0){ return h0 }
      def h1 = _try(name + ".dll", flags)
      if(h1){ return h1 }
      if(!has_sep){
         def h2 = _try("lib" + name + ".dll", flags)
         if(h2){ return h2 }
      }
      return 0
   }
   if(osn == "macos"){
      def h0 = _try(name, flags)
      if(h0){ return h0 }
      def h1 = _try(name + ".dylib", flags)
      if(h1){ return h1 }
      if(!has_sep){
         def h2 = _try("lib" + name + ".dylib", flags)
         if(h2){ return h2 }
      }
      return 0
   }
   ;; linux/other
   def h0 = _try(name, flags)
   if(h0){ return h0 }
   def h1 = _try(name + ".so", flags)
   if(h1){ return h1 }
   def h2 = _try(name + ".so.1", flags)
   if(h2){ return h2 }
   if(!has_sep){
      def h3 = _try("lib" + name + ".so", flags)
      if(h3){ return h3 }
      def h4 = _try("lib" + name + ".so.1", flags)
      if(h4){ return h4 }
   }
   0
}

;; Low-level Calls

fn call0_void(f) {
   "Calls a C function with 0 arguments and no return value."
   __call0(f)
}

fn call1_void(f,a) {
   "Calls a C function with 1 argument and no return value."
   __call1(f,a)
}

fn call2_void(f,a,b) {
   "Calls a C function with 2 arguments and no return value."
   __call2(f,a,b)
}

fn call3_void(f,a,b,c) {
   "Calls a C function with 3 arguments and no return value."
   __call3(f,a,b,c)
}

fn call0(f) {
   "Calls a C function with 0 arguments."
   __call0(f)
}

fn call1(f,a) {
   "Calls a C function with 1 argument."
   __call1(f,a)
}

fn call1_i64(f,a) {
   "Calls a C function with 1 i64 argument."
   __call1_i64(f,a)
}

fn call2(f,a,b) {
   "Calls a C function with 2 arguments."
   __call2(f,a,b)
}

fn call3(f,a,b,c) {
   "Calls a C function with 3 arguments."
   __call3(f,a,b,c)
}

fn call4(f,a,b,c,d) {
   "Calls a C function with 4 arguments."
   __call4(f,a,b,c,d)
}

fn call5(f,a,b,c,d,e) {
   "Calls a C function with 5 arguments."
   __call5(f,a,b,c,d,e)
}

fn call6(f,a,b,c,d,e,g) {
   "Calls a C function with 6 arguments."
   __call6(f,a,b,c,d,e,g)
}

fn call7(f,a,b,c,d,e,g,h) {
   "Calls a C function with 7 arguments."
   __call7(f,a,b,c,d,e,g,h)
}

fn call8(f,a,b,c,d,e,g,h,i) {
   "Calls a C function with 8 arguments."
   __call8(f,a,b,c,d,e,g,h,i)
}

fn call9(f,a,b,c,d,e,g,h,i,j) {
   "Calls a C function with 9 arguments."
   __call9(f,a,b,c,d,e,g,h,i,j)
}

fn call10(f,a,b,c,d,e,g,h,i,j,k) {
   "Calls a C function with 10 arguments."
   __call10(f,a,b,c,d,e,g,h,i,j,k)
}

fn call11(f,a,b,c,d,e,g,h,i,j,k,l) {
   "Calls a C function with 11 arguments."
   __call11(f,a,b,c,d,e,g,h,i,j,k,l)
}

fn call12(f,a,b,c,d,e,g,h,i,j,k,l,m) {
   "Calls a C function with 12 arguments."
   __call12(f,a,b,c,d,e,g,h,i,j,k,l,m)
}

fn call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n) {
   "Calls a C function with 13 arguments."
   __call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n)
}

fn call14(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o) {
   "Calls a C function with 14 arguments."
   __call14(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o)
}

fn call15(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o,p) {
   "Calls a C function with 15 arguments."
   __call15(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o,p)
}

fn call4_f32_void(f,a,b,c,d) {
   "Calls a C function with 4 f32 arguments and no return value."
   __call4_f32_void(f,a,b,c,d)
}

fn ffi_call(fptr, args){
   "Dynamically calls a C function pointer with a list of arguments."
   def n = core.len(args)
   print("FFI_CALL: fptr=" + to_str(fptr) + " n=" + to_str(n))
   if(n==0){ return call0(fptr)  }
   if(n==1){ return call1(fptr, core.get(args,0))  }
   if(n==2){ return call2(fptr, core.get(args,0), core.get(args,1))  }
   if(n==3){ return call3(fptr, core.get(args,0), core.get(args,1), core.get(args,2))  }
   if(n==4){ return call4(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3))  }
   if(n==5){ return call5(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4))  }
   if(n==6){ return call6(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5))  }
   if(n==7){ return call7(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6))  }
   if(n==8){ return call8(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7))  }
   if(n==9){ return call9(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8))  }
   if(n==10){ return call10(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9))  }
   if(n==11){ return call11(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10))  }
   if(n==12){ return call12(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10), core.get(args,11))  }
   if(n==13){ return call13(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10), core.get(args,11), core.get(args,12))  }
   if(n==14){ return call14(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10), core.get(args,11), core.get(args,12), core.get(args,13))  }
   if(n==15){ return call15(fptr, core.get(args,0), core.get(args,1), core.get(args,2), core.get(args,3), core.get(args,4), core.get(args,5), core.get(args,6), core.get(args,7), core.get(args,8), core.get(args,9), core.get(args,10), core.get(args,11), core.get(args,12), core.get(args,13), core.get(args,14))  }
   panic("ffi_call supports 0-15 args")
}

;; Binding

fn bind(h, name){
   "Binds a C symbol to a Nytrix function wrapper."
   def fptr = dlsym(h, name)
   if(fptr != 0){
      return fn(...args){ ffi_call(fptr, args) }
   }
   0
}

fn call_ext(h, name, ...args){
   "Calls a C symbol directly from a loaded library."
   def fptr = dlsym(h, name)
   if(fptr != 0){ return ffi_call(fptr, args) }
   0
}

fn bind_all(h, names){
   "Binds multiple C symbols from a library into a dictionary of wrappers."
   mut res = _d.dict()
   mut i = 0 mut n = core.len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(h, name)
      if(b != 0){ res = _d.dict_set(res, name, b) }
      i += 1
   }
   res
}

fn bind_linked(names){
   "Binds symbols from the current process."
   bind_all(0, names)
}

fn import_all(h, names){
   "Imports multiple C symbols from a library into the global namespace."
   mut g = __globals()
   mut i = 0 mut n = core.len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(h, name)
      if(b != 0){ _d.dict_set(g, name, b) }
      i += 1
   }
   true
}

fn import_linked(names){
   "Imports symbols from the current process into the global namespace."
   import_all(0, names)
}

fn extern_all(){
   "Reserved for future use."
   0
}

;; Memory

fn malloc(n) {
   "Allocates raw memory."
   if(n<=0){return 0}
   __malloc(n)
}

fn free(p) {
   "Frees raw memory."
   if(p){ __free(p) }
   0
}

;; C-Type definitions

fn u8() {
   "Returns metadata for an 8-bit unsigned integer."
   ["u8", 1, 1]
}

fn i8() {
   "Returns metadata for an 8-bit signed integer."
   ["i8", 1, 1]
}

fn u16() {
   "Returns metadata for a 16-bit unsigned integer."
   ["u16", 2, 2]
}

fn i16() {
   "Returns metadata for a 16-bit signed integer."
   ["i16", 2, 2]
}

fn u32() {
   "Returns metadata for a 32-bit unsigned integer."
   ["u32", 4, 4]
}

fn i32() {
   "Returns metadata for a 32-bit signed integer."
   ["i32", 4, 4]
}

fn u64() {
   "Returns metadata for a 64-bit unsigned integer."
   ["u64", 8, 8]
}

fn i64() {
   "Returns metadata for a 64-bit signed integer."
   ["i64", 8, 8]
}

fn f32() {
   "Returns metadata for a 32-bit floating-point number."
   ["f32", 4, 4]
}

fn f64() {
   "Returns metadata for a 64-bit floating-point number."
   ["f64", 8, 8]
}

fn ptr() {
   "Returns metadata for a machine pointer."
   ["ptr", 8, 8]
}

fn handle() {
   "Returns metadata for an opaque handle (pointer-sized)."
   ["ptr", 8, 8]
}

fn _align(offset, alignment){
   "Internal helper to align an offset."
   def rem = offset % alignment
   if(rem == 0){ return offset }
   offset + (alignment - rem)
}

fn CStruct(fields){
   "Defines a C-compatible structure layout from a list of field definitions."
   mut d_lyt = core.dict()
   mut curr_off = 0
   mut m_align = 1
   mut i = 0
   while(i < core.len(fields)){
      def fld = core.get(fields, i)
      def typ = core.get(fld, 0)
      def nm = core.get(fld, 1)
      def sz = core.get(typ, 1)
      def al = core.get(typ, 2)
      curr_off = _align(curr_off, al)
      mut info = core.dict()
      info = _d.dict_set(info, "offset", curr_off)
      info = _d.dict_set(info, "type", core.get(typ, 0))
      info = _d.dict_set(info, "size", sz)
      d_lyt = _d.dict_set(d_lyt, nm, info)
      if(al > m_align){ m_align = al }
      curr_off += sz
      i += 1
   }
   def t_size = _align(curr_off, m_align)
   mut sd = core.dict()
   sd = _d.dict_set(sd, "fields", d_lyt)
   sd = _d.dict_set(sd, "size", t_size)
   sd = _d.dict_set(sd, "align", m_align)
   sd
}

fn sizeof_struct(d){
   "Returns the size of a CStruct or list."
   if(core.is_list(d)){ return core.get(d, 1) }
   _d.dict_get(d, "size", 0)
}

fn offsetof_struct(d, name){
   "Returns the byte offset of a named field in a CStruct."
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   _d.dict_get(info, "offset", -1)
}

fn set(p, d, name, val){
   "Sets a field value in a raw structure pointer based on its CStruct layout."
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   if(!info){ return false }
   def off = _d.dict_get(info, "offset")
   def typ = _d.dict_get(info, "type")
   if(eq(typ, "u32") || eq(typ, "i32")){ core.store32(p, val, off) }
   elif(eq(typ, "u64") || eq(typ, "i64") || eq(typ, "ptr")){ core.store64_raw(p, val, off) }
   elif(eq(typ, "f32")){ core.store32_f32(p, val, off) }
   elif(eq(typ, "u8") || eq(typ, "i8")){ core.store8(p, val, off) }
   elif(eq(typ, "u16") || eq(typ, "i16")){ core.store16(p, val, off) }
   true
}

fn get(p, d, name){
   "Gets a field value from a raw structure pointer based on its CStruct layout."
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   if(!info){ return 0 }
   def off = _d.dict_get(info, "offset")
   def typ = _d.dict_get(info, "type")
   if(eq(typ, "u32") || eq(typ, "i32")){ return core.load32(p, off) }
   elif(eq(typ, "u64") || eq(typ, "i64") || eq(typ, "ptr")){ return core.load64(p, off) }
   elif(eq(typ, "f32")){ return core.load32_f32(p, off) }
   elif(eq(typ, "u8") || eq(typ, "i8")){ return core.load8(p, off) }
   elif(eq(typ, "u16") || eq(typ, "i16")){ return core.load16(p, off) }
   0
}

fn bind_lib(path, syms){
   "Loads a library and binds a list of symbols into a dictionary of wrappers."
   def lib = dlopen_any(path, RTLD_NOW())
   if(!lib){ return 0 }
   mut res = _d.dict()
   mut i = 0
   while(i < core.len(syms)){
      def s = core.get(syms, i)
      mut nm = "" mut arity = -1
      if(core.is_list(s)){ nm = core.get(s, 0) arity = core.get(s, 1) }
      else { nm = s }
      def fptr = dlsym(lib, nm)
      if(fptr){
         if(arity == 0){ res = _d.dict_set(res, nm, fn(){ call0(fptr) }) }
         elif(arity == 1){ res = _d.dict_set(res, nm, fn(a){ call1(fptr, a) }) }
         elif(arity == 2){ res = _d.dict_set(res, nm, fn(a, b){ call2(fptr, a, b) }) }
         elif(arity == 3){ res = _d.dict_set(res, nm, fn(a, b, c){ call3(fptr, a, b, c) }) }
         elif(arity == 4){ res = _d.dict_set(res, nm, fn(a, b, c, d){ call4(fptr, a, b, c, d) }) }
         elif(arity == 5){ res = _d.dict_set(res, nm, fn(a, b, c, d, e){ call5(fptr, a, b, c, d, e) }) }
         elif(arity == 6){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f){ call6(fptr, a, b, c, d, e, f) }) }
         elif(arity == 7){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g){ call7(fptr, a, b, c, d, e, f, g) }) }
         elif(arity == 8){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h){ call8(fptr, a, b, c, d, e, f, g, h) }) }
         elif(arity == 9){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j){ call9(fptr, a, b, c, d, e, f, g, h, j) }) }
         elif(arity == 10){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j, k){ call10(fptr, a, b, c, d, e, f, g, h, j, k) }) }
         elif(arity == 11){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j, k, l){ call11(fptr, a, b, c, d, e, f, g, h, j, k, l) }) }
         elif(arity == 12){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j, k, l, m){ call12(fptr, a, b, c, d, e, f, g, h, j, k, l, m) }) }
         elif(arity == 13){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j, k, l, m, n){ call13(fptr, a, b, c, d, e, f, g, h, j, k, l, m, n) }) }
         elif(arity == 14){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j, k, l, m, n, o){ call14(fptr, a, b, c, d, e, f, g, h, j, k, l, m, n, o) }) }
         elif(arity == 15){ res = _d.dict_set(res, nm, fn(a, b, c, d, e, f, g, h, j, k, l, m, n, o, p){ call15(fptr, a, b, c, d, e, f, g, h, j, k, l, m, n, o, p) }) }
         else {
            res = _d.dict_set(res, nm, fn(...args){ ffi_call(fptr, args) })
         }
      }
      i += 1
   }
   res
}
