;; Keywords: os ffi
;; Os Ffi for Nytrix

module std.os.ffi (
   RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL, dlopen, dlopen_any, dlopen_checked, dlsym, dlclose, dlerror,
   call0_void, call0_i32, call1_void, call1_u32_void, call2_void, call3_void, call4_f32_void, call0, call1, call1_i64, call1_u32, call2, call3, call4, call5,
   call6, call7, call8, call9, call10, call11, call12, call13, call14, call15, ffi_call,
   bind, call_ext, bind_all, bind_linked, import_all, import_linked, extern_all,

   ; C-FFI Helpers
   CStruct, CType, cstr,
   u8, i8, u16, i16, u32, i32, u64, i64, f32, f64, ptr, handle,
   sizeof_struct, offsetof_struct, malloc, free,
   cstruct_set, cstruct_get, bind_lib, tag_native
)

use std.core *
use std.core as core
use std.core.mem as mem
use std.core.dict_mod as _d
use std.str *
use std.os *
use std.os.path as ospath

;; Dynamic Loading

fn RTLD_LAZY(){
   "Returns the RTLD_LAZY flag for `dlopen` (1)."
   1
}
fn RTLD_NOW(){
   "Returns the RTLD_NOW flag for `dlopen` (2)."
   2
}
fn RTLD_GLOBAL(){
   "Returns the RTLD_GLOBAL flag for `dlopen` (256)."
   256
}
fn RTLD_LOCAL(){
   "Returns the RTLD_LOCAL flag for `dlopen` (0)."
   0
}

fn dlopen(path, flags){
   "Opens a dynamic library at `path` with the given `flags`. Returns a library handle, or 0 on failure."
   __dlopen(path, flags)
}
fn dlsym(h, s){
   "Returns the memory address of the symbol `s` within library handle `h`."
   __dlsym(h, s)
}
fn dlclose(h){
   "Decrements the reference count on the dynamic library handle `h`."
   __dlclose(h)
}
fn dlerror(){
   "Returns a human-readable string describing the last error that occurred from an FFI/DL operation."
   __dlerror()
}

fn _try(path, flags){
   "Attempts a direct `dlopen` call and logs the attempt when sound debug mode is enabled."
   if(env("NY_AUDIO_DEBUG")){ print("FFI: trying " + path) }
   def h = __dlopen(path, flags)
   if(h != 0){
      if(env("NY_AUDIO_DEBUG")){ print("FFI: loaded " + path + " handle=" + to_str(h)) }
      return h
   }
   0
}

fn dlopen_checked(name, required_symbol, flags=0){
   "Opens library `name` and verifies that `required_symbol` exists before returning the handle."
   mut eff_flags = flags
   if(eff_flags == 0){ eff_flags = RTLD_NOW() | RTLD_GLOBAL() }
   def h = dlopen_any(name, eff_flags)
   if(h == 0){ return 0 }
   if(required_symbol && str_len(required_symbol) > 0 && dlsym(h, required_symbol) == 0){
      dlclose(h)
      return 0
   }
   h
}

fn dlopen_any(name, flags=0){
   "Attempts to open a dynamic library by searching for several platform-specific name variations and versions."
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
         def h3 = _try("lib" + name + ".0.dylib", flags)
         if(h3){ return h3 }
      }
      return 0
   }
   ; linux/other
   def h0 = _try(name, flags)
   if(h0){ return h0 }
   def h1 = _try(name + ".so", flags)
   if(h1){ return h1 }
   def h2 = _try(name + ".so.1", flags)
   if(h2){ return h2 }
   if(!has_sep){
      def h3 = _try("lib" + name + ".so", flags)
      if(h3){ return h3 }
      def versions = ["0", "1", "2", "3", "8", "12", "14", "18"]
      mut i = 0
      while(i < core.len(versions)){
         def h4 = _try("lib" + name + ".so." + core.get(versions, i), flags)
         if(h4){ return h4 }
         i += 1
      }
   }
   0
}

;; C string helper

fn cstr(s){
   "Ensures `s` is NUL-terminated for C APIs."
   mem.cstr(s)
}

fn tag_native(addr){
   "Tags a raw address as a native function pointer."
   __tag_native(addr)
}

;; Low-level Calls

fn call0(f){ "Low-level FFI call with 0 arguments." __call0(f) }
fn call1(f,a){ "Low-level FFI call with 1 argument." __call1(f,a) }
fn call2(f,a,b){ "Low-level FFI call with 2 arguments." __call2(f,a,b) }
fn call3(f,a,b,c){ "Low-level FFI call with 3 arguments." __call3(f,a,b,c) }
fn call4(f,a,b,c,d){ "Low-level FFI call with 4 arguments." __call4(f,a,b,c,d) }
fn call5(f,a,b,c,d,e){ "Low-level FFI call with 5 arguments." __call5(f,a,b,c,d,e) }
fn call6(f,a,b,c,d,e,g){ "Low-level FFI call with 6 arguments." __call6(f,a,b,c,d,e,g) }
fn call7(f,a,b,c,d,e,g,h){ "Low-level FFI call with 7 arguments." __call7(f,a,b,c,d,e,g,h) }
fn call8(f,a,b,c,d,e,g,h,i){ "Low-level FFI call with 8 arguments." __call8(f,a,b,c,d,e,g,h,i) }
fn call9(f,a,b,c,d,e,g,h,i,j){ "Low-level FFI call with 9 arguments." __call9(f,a,b,c,d,e,g,h,i,j) }
fn call10(f,a,b,c,d,e,g,h,i,j,k){ "Low-level FFI call with 10 arguments." __call10(f,a,b,c,d,e,g,h,i,j,k) }
fn call11(f,a,b,c,d,e,g,h,i,j,k,l){ "Low-level FFI call with 11 arguments." __call11(f,a,b,c,d,e,g,h,i,j,k,l) }
fn call12(f,a,b,c,d,e,g,h,i,j,k,l,m){ "Low-level FFI call with 12 arguments." __call12(f,a,b,c,d,e,g,h,i,j,k,l,m) }
fn call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n){ "Low-level FFI call with 13 arguments." __call13(f,a,b,c,d,e,g,h,i,j,k,l,m,n) }
fn call14(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o){ "Low-level FFI call with 14 arguments." __call14(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o) }
fn call15(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o,p){ "Low-level FFI call with 15 arguments." __call15(f,a,b,c,d,e,g,h,i,j,k,l,m,n,o,p) }

fn call0_void(f){
   "Calls `f` with no arguments and ignores any return value."
   __call0(f)
}
fn call0_i32(f){
   "Calls `f` with no arguments and returns a 32-bit integer result."
   __call0_i32(f)
}
fn call1_void(f,a){
   "Calls `f` with one argument and ignores any return value."
   __call1(f,a)
}
fn call1_u32_void(f,a){
   "Calls `f` with one unsigned 32-bit argument and ignores any return value."
   __call1_u32_void(f,a)
}
fn call2_void(f,a,b){
   "Calls `f` with two arguments and ignores any return value."
   __call2(f,a,b)
}
fn call3_void(f,a,b,c){
   "Calls `f` with three arguments and ignores any return value."
   __call3(f,a,b,c)
}
fn call4_f32_void(f,a,b,c,d){
   "Calls `f` with four arguments where the fourth is a 32-bit float and ignores any return value."
   __call4_f32_void(f,a,b,c,d)
}
fn call1_u32(f,a){
   "Calls `f` with one unsigned 32-bit argument and returns the raw result."
   __call1_u32(f,a)
}
fn call1_i64(f,a){
   "Calls `f` with one argument and returns a 64-bit integer result."
   __call1_i64(f,a)
}

fn ffi_call(fptr, args){
   "Calls external function at `fptr` with `args` list. Supports up to 10 arguments."
   def n = core.len(args)
   if(n==0){ return call0(fptr)  }
   if(n==1){ def a = core.get(args,0) return call1(fptr, a)  }
   if(n==2){ def a = core.get(args,0) def b = core.get(args,1) return call2(fptr, a, b)  }
   if(n==3){ def a = core.get(args,0) def b = core.get(args,1) def c = core.get(args,2) return call3(fptr, a, b, c)  }
   if(n==4){ def a = core.get(args,0) def b = core.get(args,1) def c = core.get(args,2) def d = core.get(args,3) return call4(fptr, a, b, c, d)  }
   if(n==5){ def a = core.get(args,0) def b = core.get(args,1) def c = core.get(args,2) def d = core.get(args,3) def e = core.get(args,4) return call5(fptr, a, b, c, d, e)  }
   if(n==6){ def a = core.get(args,0) def b = core.get(args,1) def c = core.get(args,2) def d = core.get(args,3) def e = core.get(args,4) def f = core.get(args,5) return call6(fptr, a, b, c, d, e, f)  }
   if(n >= 7){ panic("ffi_call robust mode only supports 0-6 args for now") }
   0
}

;; Binding

fn bind(h, name){
   "Returns a Nytrix function binding to external symbol `name` in library `h`."
   def fptr = dlsym(h, name)
   if(fptr != 0){ return fn(...args){ ffi_call(fptr, args) } }
   0
}

fn call_ext(h, name, ...args){
   "Calls external symbol `name` in library `h` with provided arguments."
   def fptr = dlsym(h, name)
   if(fptr != 0){ return ffi_call(fptr, args) }
   0
}

fn _bind_map(h, names, target=0){
   "Builds a dictionary of resolvable symbol wrappers, optionally mirroring them into `target`."
   mut res = _d.dict()
   mut i = 0 mut n = core.len(names)
   while(i < n){
      def name = core.get(names, i)
      def b = bind(h, name)
      if(b != 0){
         res = _d.dict_set(res, name, b)
         if(target != 0){ _d.dict_set(target, name, b) }
      }
      i += 1
   }
   res
}

fn bind_all(h, names){
   "Returns a dictionary of callable wrappers for each resolvable symbol in `names`."
   _bind_map(h, names)
}

fn bind_linked(names){
   "Binds `names` from the current process image."
   bind_all(0, names)
}

fn import_all(h, names){
   "Imports each resolvable symbol in `names` into the current global scope."
   _bind_map(h, names, __globals())
   true
}

fn import_linked(names){
   "Imports `names` from the current process image into the global scope."
   import_all(0, names)
}

fn extern_all(){
   "Placeholder for compatibility with import-style APIs."
   0
}

;; Memory

fn malloc(n){
   "Allocates `n` bytes and returns a raw pointer, or `0` for non-positive sizes."
   if(n<=0){return 0}
   __malloc(n)
}

fn free(p){
   "Frees raw pointer `p` when it is non-zero."
   if(p){ __free(p) }
   0
}

;; C-Type definitions

fn u8(){
   "Returns the FFI descriptor for an unsigned 8-bit integer."
   ["u8", 1, 1]
}

fn i8(){
   "Returns the FFI descriptor for a signed 8-bit integer."
   ["i8", 1, 1]
}

fn u16(){
   "Returns the FFI descriptor for an unsigned 16-bit integer."
   ["u16", 2, 2]
}

fn i16(){
   "Returns the FFI descriptor for a signed 16-bit integer."
   ["i16", 2, 2]
}

fn u32(){
   "Returns the FFI descriptor for an unsigned 32-bit integer."
   ["u32", 4, 4]
}

fn i32(){
   "Returns the FFI descriptor for a signed 32-bit integer."
   ["i32", 4, 4]
}

fn u64(){
   "Returns the FFI descriptor for an unsigned 64-bit integer."
   ["u64", 8, 8]
}

fn i64(){
   "Returns the FFI descriptor for a signed 64-bit integer."
   ["i64", 8, 8]
}

fn f32(){
   "Returns the FFI descriptor for a 32-bit floating-point value."
   ["f32", 4, 4]
}

fn f64(){
   "Returns the FFI descriptor for a 64-bit floating-point value."
   ["f64", 8, 8]
}

fn ptr(){
   "Returns the FFI descriptor for a raw pointer."
   ["ptr", 8, 8]
}

fn handle(){
   "Returns the canonical pointer-like FFI type descriptor."
   ptr()
}

fn _align(offset, alignment){
   "Internal: rounds `offset` up to the next multiple of `alignment`."
   def rem = offset % alignment
   if(rem == 0){ return offset }
   offset + (alignment - rem)
}

fn CStruct(fields){
   "Defines a C-style structure layout from field definitions. Handles automatic alignment."
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
   "Returns the size in bytes for struct descriptor `d`."
   if(core.is_list(d)){ return core.get(d, 1) }
   _d.dict_get(d, "size", 0)
}

fn offsetof_struct(d, name){
   "Returns the byte offset of field `name` within struct descriptor `d`."
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   _d.dict_get(info, "offset", -1)
}

fn cstruct_set(p, d, name, val){
   "Sets field `name` in structure at memory address `p` with definition `d` to `val`."
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   if(!info){ return false }
   def off = _d.dict_get(info, "offset")
   def typ = _d.dict_get(info, "type")
   if(eq(typ, "u32") || eq(typ, "i32")){ core.store32(p, val, off) }
   elif(eq(typ, "u64") || eq(typ, "i64") || eq(typ, "ptr")){ core.store64_h(p, val, off) }
   elif(eq(typ, "f32")){ core.store32_f32(p, val, off) }
   elif(eq(typ, "u8") || eq(typ, "i8")){ core.store8(p, val, off) }
   elif(eq(typ, "u16") || eq(typ, "i16")){ core.store16(p, val, off) }
   true
}

fn cstruct_get(p, d, name){
   "Gets field `name` from structure at memory address `p` using definition `d`."
   def fs = _d.dict_get(d, "fields", 0)
   def info = _d.dict_get(fs, name, 0)
   if(!info){ return 0 }
   def off = _d.dict_get(info, "offset")
   def typ = _d.dict_get(info, "type")
   if(eq(typ, "u32") || eq(typ, "i32")){ return core.load32(p, off) }
   elif(eq(typ, "u64") || eq(typ, "i64") || eq(typ, "ptr")){ return core.load64_h(p, off) }
   elif(eq(typ, "f32")){ return core.load32_f32(p, off) }
   elif(eq(typ, "u8") || eq(typ, "i8")){ return core.load8(p, off) }
   elif(eq(typ, "u16") || eq(typ, "i16")){ return core.load16(p, off) }
   0
}

fn bind_lib(name_or_h, syms){
   "Binds a list of symbols from a library (by name or handle) into a dictionary."
   mut lib = 0
   if(core.is_str(name_or_h)){ lib = dlopen_any(name_or_h, RTLD_NOW()) }
   else { lib = name_or_h }
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
         else {
         res = _d.dict_set(res, nm, fn(...args){ ffi_call(fptr, args) })
         }
      }
      i += 1
   }
   res
}
